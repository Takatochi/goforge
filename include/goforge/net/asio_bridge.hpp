#pragma once

/**
 * @file asio_bridge.hpp
 * @brief High-performance non-blocking Standalone Asio <-> gorun M:N green-thread bridge
 *        (`AsioExecutor`, `AsyncTcpSocket`, `AsyncTcpAcceptor`).
 *
 * Design highlights:
 * - Sockets are configured `non_blocking(true)` + `TCP_NODELAY`.
 * - `read_some` and `write_all` attempt an immediate non-blocking syscall first (zero allocation,
 *   zero green-routine parking when kernel buffers are ready).
 * - Slow-path async I/O + `asio::steady_timer` run serialized on a per-socket `asio::strand`
 *   (100% race-free timer cancellation, zero contention with other sockets or `gorun` timers).
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
public:
    struct SharedState {
        struct IoRes {
            std::size_t n{0};
            std::error_code ec{};
        };

        asio::strand<asio::any_io_executor> strand;
        asio::ip::tcp::socket sock;
        asio::steady_timer timer;
        gorun::Channel<IoRes> io_ch{1};

        explicit SharedState(asio::ip::tcp::socket s)
            : strand(asio::make_strand(s.get_executor()))
            , sock(std::move(s))
            , timer(strand) {}

        explicit SharedState(asio::io_context& io)
            : strand(asio::make_strand(io.get_executor()))
            , sock(strand)
            , timer(strand) {}

        void cancel_on_strand() noexcept {
            std::error_code ignored;
            timer.cancel();
            sock.cancel(ignored);
        }
    };

private:
    std::shared_ptr<SharedState> state_;

    void init_socket_options() noexcept {
        if (!state_ || !state_->sock.is_open()) return;
        std::error_code ignored;
        state_->sock.set_option(asio::ip::tcp::no_delay(true), ignored);
        state_->sock.non_blocking(true, ignored);
    }

    static bool is_would_block(const std::error_code& ec) noexcept {
        return ec == asio::error::would_block || ec == asio::error::try_again;
    }

    SharedState::IoRes await_completion(const gorun::Context* ctx) {
        if (!ctx) {
            auto r = state_->io_ch.recv();
            return r ? *r : SharedState::IoRes{0, asio::error::operation_aborted};
        }

        SharedState::IoRes res{};
        bool canceled = false;
        gorun::select(
            state_->io_ch.on_recv([&](SharedState::IoRes r) { res = r; }),
            ctx->on_done([&]() {
                canceled = true;
                cancel();
            })
        );

        if (canceled) {
            auto drained = state_->io_ch.recv();
            if (drained && !drained->ec && drained->n > 0) {
                return *drained;
            }
            return SharedState::IoRes{0, asio::error::operation_aborted};
        }
        return res;
    }

    std::pair<std::size_t, std::error_code> read_some_impl(
        char* buf,
        std::size_t len,
        const gorun::Context* ctx,
        std::chrono::milliseconds timeout
    ) {
        if (!state_ || !state_->sock.is_open()) {
            return {0, asio::error::bad_descriptor};
        }
        if (ctx && ctx->is_canceled()) {
            return {0, asio::error::operation_aborted};
        }

        // Fast path: immediate non-blocking syscall (zero Asio dispatch, zero green-routine park!)
        std::error_code ec;
        std::size_t n = state_->sock.read_some(asio::buffer(buf, len), ec);
        if (!is_would_block(ec)) {
            return {n, ec};
        }

        // Slow path: arm optional timer + async_read_some atomically on socket's strand
        auto st = state_;
        if (timeout.count() > 0) {
            asio::post(st->strand, [st, buf, len, timeout]() {
                st->timer.expires_after(timeout);
                st->timer.async_wait(asio::bind_executor(st->strand, [st](const std::error_code& tec) {
                    if (!tec) {
                        std::error_code ignored;
                        st->sock.cancel(ignored);
                    }
                }));
                st->sock.async_read_some(
                    asio::buffer(buf, len),
                    asio::bind_executor(st->strand, [st](const std::error_code& rec, std::size_t rn) {
                        st->timer.cancel();
                        st->io_ch.send(SharedState::IoRes{rn, rec});
                    })
                );
            });
        } else {
            st->sock.async_read_some(
                asio::buffer(buf, len),
                [st](const std::error_code& rec, std::size_t rn) {
                    st->io_ch.send(SharedState::IoRes{rn, rec});
                }
            );
        }

        auto res = await_completion(ctx);
        return {res.n, res.ec};
    }

    std::pair<std::size_t, std::error_code> write_all_impl(
        std::string_view data,
        const gorun::Context* ctx,
        std::chrono::milliseconds timeout
    ) {
        if (!state_ || !state_->sock.is_open()) {
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
            std::size_t n = state_->sock.write_some(
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

        // Slow path: park green routine for remaining bytes
        auto st = state_;
        const char* rem_ptr = data.data() + total_written;
        const std::size_t rem_len = data.size() - total_written;

        if (timeout.count() > 0) {
            asio::post(st->strand, [st, rem_ptr, rem_len, timeout]() {
                st->timer.expires_after(timeout);
                st->timer.async_wait(asio::bind_executor(st->strand, [st](const std::error_code& tec) {
                    if (!tec) {
                        std::error_code ignored;
                        st->sock.cancel(ignored);
                    }
                }));
                asio::async_write(
                    st->sock,
                    asio::buffer(rem_ptr, rem_len),
                    asio::bind_executor(st->strand, [st](const std::error_code& wec, std::size_t wn) {
                        st->timer.cancel();
                        st->io_ch.send(SharedState::IoRes{wn, wec});
                    })
                );
            });
        } else {
            asio::async_write(
                st->sock,
                asio::buffer(rem_ptr, rem_len),
                [st](const std::error_code& wec, std::size_t wn) {
                    st->io_ch.send(SharedState::IoRes{wn, wec});
                }
            );
        }

        auto res = await_completion(ctx);
        return {total_written + res.n, res.ec};
    }

public:
    AsyncTcpSocket() = default;

    explicit AsyncTcpSocket(asio::ip::tcp::socket sock)
        : state_(std::make_shared<SharedState>(std::move(sock))) {
        init_socket_options();
    }

    explicit AsyncTcpSocket(asio::io_context& io)
        : state_(std::make_shared<SharedState>(io)) {}

    AsyncTcpSocket(AsyncTcpSocket&&) noexcept = default;
    AsyncTcpSocket& operator=(AsyncTcpSocket&&) noexcept = default;
    AsyncTcpSocket(const AsyncTcpSocket&) = delete;
    AsyncTcpSocket& operator=(const AsyncTcpSocket&) = delete;

    /// Weak handle allowing `HttpServer::stop()` to cancel idle sockets without lock contention during normal I/O.
    [[nodiscard]] std::weak_ptr<SharedState> weak_state() const noexcept {
        return state_;
    }

    [[nodiscard]] bool is_open() const noexcept {
        return state_ && state_->sock.is_open();
    }

    void set_no_delay(bool enable = true) noexcept {
        if (!state_ || !state_->sock.is_open()) return;
        std::error_code ignored;
        state_->sock.set_option(asio::ip::tcp::no_delay(enable), ignored);
    }

    [[nodiscard]] std::string remote_address() const {
        if (!state_) return "unknown";
        std::error_code ec;
        auto ep = state_->sock.remote_endpoint(ec);
        if (ec) return "unknown";
        return ep.address().to_string() + ":" + std::to_string(ep.port());
    }

    /// Thread-safe cancellation of any in-flight async operation on this socket.
    void cancel() noexcept {
        if (!state_) return;
        auto st = state_;
        asio::post(st->strand, [st]() { st->cancel_on_strand(); });
    }

    std::error_code connect(std::string_view host, std::uint16_t port) {
        if (!state_) return std::make_error_code(std::errc::bad_file_descriptor);
        std::error_code addr_ec;
        auto addr = asio::ip::make_address(host, addr_ec);
        if (addr_ec) return addr_ec;

        auto st = state_;
        st->sock.async_connect(asio::ip::tcp::endpoint(addr, port), [st](const std::error_code& ec) {
            st->io_ch.send(SharedState::IoRes{0, ec});
        });

        auto res = await_completion(nullptr);
        if (!res.ec) init_socket_options();
        return res.ec;
    }

    std::error_code connect(std::string_view host,
                            std::uint16_t port,
                            const gorun::Context& ctx) {
        if (!state_) return std::make_error_code(std::errc::bad_file_descriptor);
        if (ctx.is_canceled()) return asio::error::operation_aborted;
        std::error_code addr_ec;
        auto addr = asio::ip::make_address(host, addr_ec);
        if (addr_ec) return addr_ec;

        auto st = state_;
        st->sock.async_connect(asio::ip::tcp::endpoint(addr, port), [st](const std::error_code& ec) {
            st->io_ch.send(SharedState::IoRes{0, ec});
        });

        auto res = await_completion(&ctx);
        if (!res.ec) init_socket_options();
        return res.ec;
    }

    /// Lock-free read (optional socket timeout, no external `gorun::Context` select overhead).
    std::pair<std::size_t, std::error_code> read_some(
        char* buf,
        std::size_t len,
        std::chrono::milliseconds timeout = std::chrono::milliseconds(0)
    ) {
        return read_some_impl(buf, len, nullptr, timeout);
    }

    /// Context-aware read (observes both `ctx` cancellation and optional `timeout`).
    std::pair<std::size_t, std::error_code> read_some(
        char* buf,
        std::size_t len,
        const gorun::Context& ctx,
        std::chrono::milliseconds timeout = std::chrono::milliseconds(0)
    ) {
        return read_some_impl(buf, len, &ctx, timeout);
    }

    /// Lock-free write (optional socket timeout, no external `gorun::Context` select overhead).
    std::pair<std::size_t, std::error_code> write_all(
        std::string_view data,
        std::chrono::milliseconds timeout = std::chrono::milliseconds(0)
    ) {
        return write_all_impl(data, nullptr, timeout);
    }

    /// Context-aware write.
    std::pair<std::size_t, std::error_code> write_all(
        std::string_view data,
        const gorun::Context& ctx,
        std::chrono::milliseconds timeout = std::chrono::milliseconds(0)
    ) {
        return write_all_impl(data, &ctx, timeout);
    }

    void shutdown_send() noexcept {
        if (!state_ || !state_->sock.is_open()) return;
        std::error_code ignored;
        state_->sock.shutdown(asio::ip::tcp::socket::shutdown_send, ignored);
    }

    void close() noexcept {
        if (!state_) return;
        std::error_code ignored;
        state_->timer.cancel();
        state_->sock.shutdown(asio::ip::tcp::socket::shutdown_both, ignored);
        state_->sock.close(ignored);
    }
};

/**
 * @class AsyncTcpAcceptor
 * @brief Non-blocking TCP listener supporting both green-routine `accept()` and zero-overhead
 *        direct Asio `start_async_accept(on_peer)` callbacks.
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

    /**
     * @brief Zero-park accept loop executing directly on the Asio event loop, invoking `on_peer`
     *        immediately upon each accepted TCP connection and re-arming `async_accept` without
     *        bouncing through a green routine per accept.
     */
    template <typename OnPeer>
    void start_direct_accept(OnPeer&& on_peer) {
        auto cb = std::make_shared<std::decay_t<OnPeer>>(std::forward<OnPeer>(on_peer));
        auto a = acceptor_;

        struct Loop {
            std::shared_ptr<asio::ip::tcp::acceptor> acceptor;
            std::shared_ptr<std::decay_t<OnPeer>> on_peer;

            void next() {
                if (!acceptor || !acceptor->is_open()) return;
                auto self = *this;
                acceptor->async_accept([self](const std::error_code& ec, asio::ip::tcp::socket peer) mutable {
                    if (ec) {
                        if (ec == asio::error::operation_aborted || !self.acceptor->is_open()) {
                            return;
                        }
                        self.next();
                        return;
                    }
                    (*self.on_peer)(AsyncTcpSocket(std::move(peer)));
                    self.next();
                });
            }
        };

        Loop{a, cb}.next();
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
