#pragma once

/**
 * @file asio_bridge.hpp
 * @brief High-performance non-blocking Standalone Asio <-> gorun M:N green-thread bridge
 *        (`AsioExecutor`, `AsyncTcpSocket`, `AsyncTcpAcceptor`).
 *
 * Uses Go-netpoll-style non-blocking fast paths:
 * - Sockets are configured `non_blocking(true)` + `TCP_NODELAY`.
 * - `read_some` and `write_all` attempt an immediate non-blocking syscall first (zero allocation,
 *   zero green-routine parking when kernel buffers are ready).
 * - On `would_block`, the calling green routine parks on a reusable per-socket `gorun::Channel`
 *   until Asio (`IOCP` / `epoll` / `kqueue`) signals readiness or a `gorun::Context` / `asio::steady_timer` fires.
 */

#include <atomic>
#include <chrono>
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
 * @brief Green-thread-friendly non-blocking TCP socket over `asio::ip::tcp::socket`.
 */
class AsyncTcpSocket {
    struct IoRes {
        std::size_t n{0};
        std::error_code ec{};
    };

    std::shared_ptr<asio::ip::tcp::socket> sock_;
    std::shared_ptr<asio::steady_timer> timer_;
    std::shared_ptr<gorun::Channel<IoRes>> io_ch_; ///< Reusable 1-slot wakeup channel (zero alloc per I/O)

    void init_socket_options() noexcept {
        if (!sock_ || !sock_->is_open()) return;
        std::error_code ignored;
        sock_->set_option(asio::ip::tcp::no_delay(true), ignored);
        sock_->non_blocking(true, ignored);
    }

    void cancel_async() noexcept {
        if (!sock_) return;
        auto s = sock_;
        asio::post(s->get_executor(), [s]() {
            std::error_code ignored;
            s->cancel(ignored);
        });
    }

    static bool is_would_block(const std::error_code& ec) noexcept {
        return ec == asio::error::would_block || ec == asio::error::try_again;
    }

    void arm_timer(std::chrono::milliseconds timeout) {
        if (timeout.count() <= 0 || !timer_ || !sock_) return;
        timer_->expires_after(timeout);
        auto s = sock_;
        timer_->async_wait([s](const std::error_code& ec) {
            if (!ec) {
                std::error_code ignored;
                s->cancel(ignored);
            }
        });
    }

    void disarm_timer() noexcept {
        if (!timer_) return;
        timer_->cancel();
    }

    IoRes await_io(const gorun::Context* ctx, std::chrono::milliseconds timeout) {
        if (timeout.count() > 0) {
            arm_timer(timeout);
        }
        GORUN_DEFER {
            if (timeout.count() > 0) {
                disarm_timer();
            }
        };

        if (!ctx) {
            auto r = io_ch_->recv();
            return r ? *r : IoRes{0, asio::error::operation_aborted};
        }

        IoRes res{};
        bool canceled = false;
        gorun::select(
            io_ch_->on_recv([&](IoRes r) { res = r; }),
            ctx->on_done([&]() {
                canceled = true;
                cancel_async();
            })
        );

        if (canceled) {
            auto drained = io_ch_->recv();
            if (drained && !drained->ec && drained->n > 0) {
                return *drained;
            }
            return IoRes{0, asio::error::operation_aborted};
        }
        return res;
    }

    std::pair<std::size_t, std::error_code> read_some_impl(
        char* buf,
        std::size_t len,
        const gorun::Context* ctx,
        std::chrono::milliseconds timeout
    ) {
        if (!sock_ || !sock_->is_open()) {
            return {0, asio::error::bad_descriptor};
        }
        if (ctx && ctx->is_canceled()) {
            return {0, asio::error::operation_aborted};
        }

        // Fast path: immediate non-blocking read (zero park, zero Asio dispatch when buffered)
        std::error_code ec;
        std::size_t n = sock_->read_some(asio::buffer(buf, len), ec);
        if (!is_would_block(ec)) {
            return {n, ec};
        }

        // Slow path: park green routine until Asio reactor completes read
        auto ch = io_ch_;
        auto s = sock_;
        s->async_read_some(asio::buffer(buf, len), [ch, s](const std::error_code& rec, std::size_t rn) {
            ch->send(IoRes{rn, rec});
        });

        IoRes res = await_io(ctx, timeout);
        return {res.n, res.ec};
    }

    std::pair<std::size_t, std::error_code> write_all_impl(
        std::string_view data,
        const gorun::Context* ctx,
        std::chrono::milliseconds timeout
    ) {
        if (!sock_ || !sock_->is_open()) {
            return {0, asio::error::bad_descriptor};
        }
        if (data.empty()) {
            return {0, {}};
        }
        if (ctx && ctx->is_canceled()) {
            return {0, asio::error::operation_aborted};
        }

        // Fast path: immediate non-blocking write (handles 99%+ of HTTP responses synchronously!)
        std::size_t total_written = 0;
        while (total_written < data.size()) {
            std::error_code ec;
            std::size_t n = sock_->write_some(
                asio::buffer(data.data() + total_written, data.size() - total_written),
                ec
            );
            total_written += n;
            if (!ec) continue;
            if (is_would_block(ec)) break;
            return {total_written, ec};
        }

        if (total_written == data.size()) {
            return {total_written, {}};
        }

        // Slow path: park green routine for remaining bytes via Asio
        auto ch = io_ch_;
        auto s = sock_;
        asio::async_write(
            *s,
            asio::buffer(data.data() + total_written, data.size() - total_written),
            [ch, s](const std::error_code& wec, std::size_t wn) {
                ch->send(IoRes{wn, wec});
            }
        );

        IoRes res = await_io(ctx, timeout);
        return {total_written + res.n, res.ec};
    }

public:
    AsyncTcpSocket() = default;

    explicit AsyncTcpSocket(asio::ip::tcp::socket sock)
        : sock_(std::make_shared<asio::ip::tcp::socket>(std::move(sock)))
        , timer_(std::make_shared<asio::steady_timer>(sock_->get_executor()))
        , io_ch_(std::make_shared<gorun::Channel<IoRes>>(1)) {
        init_socket_options();
    }

    explicit AsyncTcpSocket(asio::io_context& io)
        : sock_(std::make_shared<asio::ip::tcp::socket>(io))
        , timer_(std::make_shared<asio::steady_timer>(io))
        , io_ch_(std::make_shared<gorun::Channel<IoRes>>(1)) {}

    AsyncTcpSocket(AsyncTcpSocket&&) noexcept = default;
    AsyncTcpSocket& operator=(AsyncTcpSocket&&) noexcept = default;
    AsyncTcpSocket(const AsyncTcpSocket&) = delete;
    AsyncTcpSocket& operator=(const AsyncTcpSocket&) = delete;

    [[nodiscard]] bool is_open() const noexcept {
        return sock_ && sock_->is_open();
    }

    void set_no_delay(bool enable = true) noexcept {
        if (!sock_ || !sock_->is_open()) return;
        std::error_code ignored;
        sock_->set_option(asio::ip::tcp::no_delay(enable), ignored);
    }

    [[nodiscard]] std::string remote_address() const {
        if (!sock_) return "unknown";
        std::error_code ec;
        auto ep = sock_->remote_endpoint(ec);
        if (ec) return "unknown";
        return ep.address().to_string() + ":" + std::to_string(ep.port());
    }

    std::error_code connect(std::string_view host, std::uint16_t port) {
        if (!sock_) return std::make_error_code(std::errc::bad_file_descriptor);
        std::error_code addr_ec;
        auto addr = asio::ip::make_address(host, addr_ec);
        if (addr_ec) return addr_ec;

        auto ch = io_ch_;
        auto s = sock_;
        s->async_connect(asio::ip::tcp::endpoint(addr, port), [ch, s](const std::error_code& ec) {
            ch->send(IoRes{0, ec});
        });

        IoRes res = await_io(nullptr, std::chrono::milliseconds(0));
        if (!res.ec) init_socket_options();
        return res.ec;
    }

    std::error_code connect(std::string_view host,
                            std::uint16_t port,
                            const gorun::Context& ctx,
                            std::chrono::milliseconds timeout = std::chrono::milliseconds(0)) {
        if (!sock_) return std::make_error_code(std::errc::bad_file_descriptor);
        if (ctx.is_canceled()) return asio::error::operation_aborted;
        std::error_code addr_ec;
        auto addr = asio::ip::make_address(host, addr_ec);
        if (addr_ec) return addr_ec;

        auto ch = io_ch_;
        auto s = sock_;
        s->async_connect(asio::ip::tcp::endpoint(addr, port), [ch, s](const std::error_code& ec) {
            ch->send(IoRes{0, ec});
        });

        IoRes res = await_io(&ctx, timeout);
        if (!res.ec) init_socket_options();
        return res.ec;
    }

    std::pair<std::size_t, std::error_code> read_some(char* buf, std::size_t len) {
        return read_some_impl(buf, len, nullptr, std::chrono::milliseconds(0));
    }

    std::pair<std::size_t, std::error_code> read_some(
        char* buf,
        std::size_t len,
        const gorun::Context& ctx,
        std::chrono::milliseconds timeout = std::chrono::milliseconds(0)
    ) {
        return read_some_impl(buf, len, &ctx, timeout);
    }

    std::pair<std::size_t, std::error_code> write_all(std::string_view data) {
        return write_all_impl(data, nullptr, std::chrono::milliseconds(0));
    }

    std::pair<std::size_t, std::error_code> write_all(
        std::string_view data,
        const gorun::Context& ctx,
        std::chrono::milliseconds timeout = std::chrono::milliseconds(0)
    ) {
        return write_all_impl(data, &ctx, timeout);
    }

    void shutdown_send() noexcept {
        if (!sock_ || !sock_->is_open()) return;
        std::error_code ignored;
        sock_->shutdown(asio::ip::tcp::socket::shutdown_send, ignored);
    }

    void close() noexcept {
        if (timer_) {
            timer_->cancel();
        }
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
    struct AcceptRes {
        std::optional<AsyncTcpSocket> sock;
        std::error_code ec{};
    };

    std::shared_ptr<asio::ip::tcp::acceptor> acceptor_;
    std::shared_ptr<gorun::Channel<AcceptRes>> accept_ch_;

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
        : acceptor_(std::make_shared<asio::ip::tcp::acceptor>(io))
        , accept_ch_(std::make_shared<gorun::Channel<AcceptRes>>(1)) {}

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

    [[nodiscard]] std::uint16_t local_port() const {
        if (!acceptor_ || !acceptor_->is_open()) return 0;
        std::error_code ec;
        auto ep = acceptor_->local_endpoint(ec);
        return ec ? 0 : ep.port();
    }

    std::pair<std::optional<AsyncTcpSocket>, std::error_code> accept(const gorun::Context& ctx) {
        if (!acceptor_ || !acceptor_->is_open()) {
            return {std::nullopt, asio::error::bad_descriptor};
        }
        if (ctx.is_canceled()) {
            return {std::nullopt, asio::error::operation_aborted};
        }

        auto ch = accept_ch_;
        auto a = acceptor_;
        a->async_accept([ch, a](const std::error_code& ec, asio::ip::tcp::socket peer) {
            if (ec) {
                ch->send(AcceptRes{std::nullopt, ec});
            } else {
                ch->send(AcceptRes{AsyncTcpSocket(std::move(peer)), {}});
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
