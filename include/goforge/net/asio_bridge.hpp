#pragma once

/**
 * @file asio_bridge.hpp
 * @brief Non-blocking Standalone Asio <-> gorun M:N green-thread bridge (`AsioExecutor`,
 *        `AsyncTcpSocket`, `AsyncTcpAcceptor`).
 *
 * Bridges Asio asynchronous socket completions into `gorun::Channel` / `gorun::select` so every
 * network accept, read, write, and connect parks only the calling green routine (zero OS-thread
 * blocking on `gorun` workers) and supports cooperative cancellation via `gorun::Context`.
 */

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <asio.hpp>

#include "gorun/gorun.hpp"

namespace goforge::net {

/**
 * @class AsioExecutor
 * @brief Owns a background `asio::io_context` event loop (IOCP on Windows, epoll on Linux, kqueue on macOS).
 */
class AsioExecutor {
    asio::io_context io_;
    std::optional<asio::executor_work_guard<asio::io_context::executor_type>> guard_;
    std::vector<std::jthread> threads_;
    std::atomic<bool> running_{false};

public:
    explicit AsioExecutor(std::size_t io_threads = 1) {
        start(io_threads);
    }

    ~AsioExecutor() {
        stop();
    }

    AsioExecutor(const AsioExecutor&) = delete;
    AsioExecutor& operator=(const AsioExecutor&) = delete;

    asio::io_context& context() noexcept { return io_; }

    void start(std::size_t io_threads = 1) {
        bool expected = false;
        if (!running_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            return;
        }
        if (io_threads == 0) {
            io_threads = 1;
        }
        io_.restart();
        guard_.emplace(asio::make_work_guard(io_));
        threads_.reserve(io_threads);
        for (std::size_t i = 0; i < io_threads; ++i) {
            threads_.emplace_back([this]() {
                io_.run();
            });
        }
    }

    void stop() {
        bool expected = true;
        if (!running_.compare_exchange_strong(expected, false, std::memory_order_acq_rel)) {
            return;
        }
        guard_.reset();
        io_.stop();
        for (auto& t : threads_) {
            if (t.joinable()) {
                t.join();
            }
        }
        threads_.clear();
    }
};

/**
 * @class AsyncTcpSocket
 * @brief Green-thread-friendly TCP socket wrapper over `asio::ip::tcp::socket`.
 *
 * Every I/O operation parks the calling `gorun` routine without blocking any `gorun` worker thread,
 * and responds immediately to `gorun::Context` cancellation/deadlines by cancelling pending Asio I/O.
 */
class AsyncTcpSocket {
    std::shared_ptr<asio::ip::tcp::socket> sock_;

    void cancel_async() noexcept {
        if (!sock_) return;
        auto s = sock_;
        asio::post(s->get_executor(), [s]() {
            std::error_code ignored;
            s->cancel(ignored);
        });
    }

public:
    AsyncTcpSocket() = default;

    explicit AsyncTcpSocket(asio::ip::tcp::socket sock)
        : sock_(std::make_shared<asio::ip::tcp::socket>(std::move(sock))) {}

    explicit AsyncTcpSocket(asio::io_context& io)
        : sock_(std::make_shared<asio::ip::tcp::socket>(io)) {}

    AsyncTcpSocket(AsyncTcpSocket&&) noexcept = default;
    AsyncTcpSocket& operator=(AsyncTcpSocket&&) noexcept = default;
    AsyncTcpSocket(const AsyncTcpSocket&) = delete;
    AsyncTcpSocket& operator=(const AsyncTcpSocket&) = delete;

    [[nodiscard]] bool is_open() const noexcept {
        return sock_ && sock_->is_open();
    }

    /// Configures TCP_NODELAY for low-latency HTTP responses.
    void set_no_delay(bool enable = true) noexcept {
        if (!sock_) return;
        std::error_code ignored;
        sock_->set_option(asio::ip::tcp::no_delay(enable), ignored);
    }

    /// Returns peer endpoint string (`ip:port`) or `"unknown"` if closed.
    [[nodiscard]] std::string remote_address() const {
        if (!sock_) return "unknown";
        std::error_code ec;
        auto ep = sock_->remote_endpoint(ec);
        if (ec) return "unknown";
        return ep.address().to_string() + ":" + std::to_string(ep.port());
    }

    /// Non-blocking TCP connect that parks the calling green routine until connected or canceled.
    std::error_code connect(std::string_view host, std::uint16_t port, gorun::Context ctx = gorun::background()) {
        if (!sock_) {
            return std::make_error_code(std::errc::bad_file_descriptor);
        }
        if (ctx.is_canceled()) {
            return asio::error::operation_aborted;
        }

        std::error_code addr_ec;
        auto addr = asio::ip::make_address(host, addr_ec);
        if (addr_ec) {
            return addr_ec;
        }
        asio::ip::tcp::endpoint ep(addr, port);

        auto ch = std::make_shared<gorun::Channel<std::error_code>>(1);
        auto s = sock_;
        s->async_connect(ep, [ch, s](const std::error_code& ec) {
            ch->send(ec);
        });

        std::error_code result_ec;
        bool canceled = false;
        gorun::select(
            ch->on_recv([&](std::error_code ec) { result_ec = ec; }),
            ctx.on_done([&]() {
                canceled = true;
                cancel_async();
            })
        );

        if (canceled) {
            // Drain completion to ensure socket operation is finished before caller reuses/destroys socket
            (void)ch->recv();
            return asio::error::operation_aborted;
        }
        return result_ec;
    }

    /**
     * @brief Reads up to `len` bytes into `buf`, parking the calling green routine until data arrives,
     *        EOF/error occurs, or `ctx` is canceled.
     */
    std::pair<std::size_t, std::error_code> read_some(
        char* buf,
        std::size_t len,
        gorun::Context ctx = gorun::background()
    ) {
        if (!sock_ || !sock_->is_open()) {
            return {0, asio::error::bad_descriptor};
        }
        if (ctx.is_canceled()) {
            return {0, asio::error::operation_aborted};
        }

        struct ReadRes {
            std::size_t n{0};
            std::error_code ec{};
        };

        auto ch = std::make_shared<gorun::Channel<ReadRes>>(1);
        auto s = sock_;
        s->async_read_some(asio::buffer(buf, len), [ch, s](const std::error_code& ec, std::size_t n) {
            ch->send(ReadRes{n, ec});
        });

        ReadRes res{};
        bool canceled = false;
        gorun::select(
            ch->on_recv([&](ReadRes r) { res = r; }),
            ctx.on_done([&]() {
                canceled = true;
                cancel_async();
            })
        );

        if (canceled) {
            // Wait for Asio completion before returning so `buf` (often on caller stack!) is never touched after return!
            auto drained = ch->recv();
            if (drained && !drained->ec && drained->n > 0) {
                return {drained->n, {}};
            }
            return {0, asio::error::operation_aborted};
        }
        return {res.n, res.ec};
    }

    /**
     * @brief Writes all bytes of `data` to the socket, parking the calling green routine until complete
     *        or canceled.
     */
    std::pair<std::size_t, std::error_code> write_all(
        std::string_view data,
        gorun::Context ctx = gorun::background()
    ) {
        if (!sock_ || !sock_->is_open()) {
            return {0, asio::error::bad_descriptor};
        }
        if (data.empty()) {
            return {0, {}};
        }
        if (ctx.is_canceled()) {
            return {0, asio::error::operation_aborted};
        }

        struct WriteRes {
            std::size_t n{0};
            std::error_code ec{};
        };

        auto ch = std::make_shared<gorun::Channel<WriteRes>>(1);
        auto s = sock_;
        asio::async_write(*s, asio::buffer(data.data(), data.size()),
            [ch, s](const std::error_code& ec, std::size_t n) {
                ch->send(WriteRes{n, ec});
            });

        WriteRes res{};
        bool canceled = false;
        gorun::select(
            ch->on_recv([&](WriteRes r) { res = r; }),
            ctx.on_done([&]() {
                canceled = true;
                cancel_async();
            })
        );

        if (canceled) {
            // Wait for completion so caller's `data` buffer remains valid until Asio releases it!
            (void)ch->recv();
            return {0, asio::error::operation_aborted};
        }
        return {res.n, res.ec};
    }

    void shutdown_send() noexcept {
        if (!sock_ || !sock_->is_open()) return;
        std::error_code ignored;
        sock_->shutdown(asio::ip::tcp::socket::shutdown_send, ignored);
    }

    void close() noexcept {
        if (!sock_) return;
        std::error_code ignored;
        sock_->shutdown(asio::ip::tcp::socket::shutdown_both, ignored);
        sock_->close(ignored);
    }
};

/**
 * @class AsyncTcpAcceptor
 * @brief Non-blocking TCP listener bridging `asio::ip::tcp::acceptor::async_accept` to `gorun` routines.
 */
class AsyncTcpAcceptor {
    std::shared_ptr<asio::ip::tcp::acceptor> acceptor_;

    void cancel_async() noexcept {
        if (!acceptor_) return;
        auto a = acceptor_;
        asio::post(a->get_executor(), [a]() {
            std::error_code ignored;
            a->cancel(ignored);
        });
    }

public:
    explicit AsyncTcpAcceptor(asio::io_context& io)
        : acceptor_(std::make_shared<asio::ip::tcp::acceptor>(io)) {}

    /// Binds to `host:port` (`port == 0` picks an OS ephemeral port) and begins listening.
    std::error_code listen(std::string_view host, std::uint16_t port, int backlog = asio::socket_base::max_listen_connections) {
        std::error_code ec;
        auto addr = asio::ip::make_address(host, ec);
        if (ec) return ec;

        asio::ip::tcp::endpoint ep(addr, port);
        acceptor_->open(ep.protocol(), ec);
        if (ec) return ec;

        acceptor_->set_option(asio::socket_base::reuse_address(true), ec);
        if (ec) return ec;

        acceptor_->bind(ep, ec);
        if (ec) return ec;

        acceptor_->listen(backlog, ec);
        return ec;
    }

    /// Returns the actual bound TCP port (useful when listening on port 0).
    [[nodiscard]] std::uint16_t local_port() const {
        if (!acceptor_ || !acceptor_->is_open()) return 0;
        std::error_code ec;
        auto ep = acceptor_->local_endpoint(ec);
        return ec ? 0 : ep.port();
    }

    /// Accepts the next incoming TCP connection, parking the calling green routine until arrival or cancellation.
    std::pair<std::optional<AsyncTcpSocket>, std::error_code> accept(gorun::Context ctx = gorun::background()) {
        if (!acceptor_ || !acceptor_->is_open()) {
            return {std::nullopt, asio::error::bad_descriptor};
        }
        if (ctx.is_canceled()) {
            return {std::nullopt, asio::error::operation_aborted};
        }

        struct AcceptRes {
            std::optional<AsyncTcpSocket> sock;
            std::error_code ec{};
        };

        auto ch = std::make_shared<gorun::Channel<AcceptRes>>(1);
        auto a = acceptor_;
        a->async_accept([ch, a](const std::error_code& ec, asio::ip::tcp::socket peer) {
            if (ec) {
                ch->send(AcceptRes{std::nullopt, ec});
            } else {
                AsyncTcpSocket wrapped(std::move(peer));
                wrapped.set_no_delay(true);
                ch->send(AcceptRes{std::move(wrapped), {}});
            }
        });

        AcceptRes res{};
        bool canceled = false;
        gorun::select(
            ch->on_recv([&](AcceptRes r) { res = std::move(r); }),
            ctx.on_done([&]() {
                canceled = true;
                cancel_async();
            })
        );

        if (canceled) {
            (void)ch->recv();
            return {std::nullopt, asio::error::operation_aborted};
        }
        return {std::move(res.sock), res.ec};
    }

    void close() noexcept {
        if (!acceptor_) return;
        auto a = acceptor_;
        asio::post(a->get_executor(), [a]() {
            std::error_code ignored;
            a->cancel(ignored);
            a->close(ignored);
        });
    }
};

} // namespace goforge::net
