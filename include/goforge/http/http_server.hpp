#pragma once

/**
 * @file http_server.hpp
 * @brief Non-blocking HTTP/1.1 server built 100% natively on `gorun` M:N green threads and
 *        `gorun::net` (Windows IOCP / Linux epoll / macOS kqueue) — zero Asio dependency.
 *
 * Architecture:
 * - Zero external networking dependencies: uses `gorun::net::TcpListener` & `TcpStream` directly.
 * - 1 accepted TCP connection = 1 `gorun` green routine (`gorun::go(config_.connection_stack_size, ...)`).
 * - HTTP/1.1 Keep-Alive reuse with zero-timer synchronous fast-path I/O, `gorun::Semaphore` concurrency cap,
 *   exception isolation (`500`), and graceful shutdown draining via `TcpStream::cancel_handle()` + `goforge::Lifecycle`.
 */

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "gorun/gorun.hpp"
#include "goforge/lifecycle.hpp"
#include "goforge/http/http_types.hpp"

namespace goforge::http {

/**
 * @struct HttpServerConfig
 * @brief Configuration for `goforge::http::HttpServer`.
 */
struct HttpServerConfig {
    std::string host{"127.0.0.1"};
    std::uint16_t port{8080}; ///< Use `0` to bind an OS-assigned ephemeral port
    std::size_t io_threads{2}; ///< Informational hint (managed automatically by `gorun::net` poller)
    std::size_t max_connections{10000};
    std::chrono::milliseconds read_timeout{std::chrono::seconds(15)};
    std::chrono::milliseconds write_timeout{std::chrono::seconds(15)};
    std::size_t max_header_bytes{16 * 1024};
    std::size_t max_body_bytes{4 * 1024 * 1024};
    gorun::StackSize connection_stack_size{gorun::StackSize::Default};
};

using HttpHandler = std::function<HttpResponse(const HttpRequest&, gorun::Context)>;

/**
 * @class HttpServer
 * @brief High-concurrency non-blocking HTTP/1.1 server powered purely by `gorun` M:N green threads & `gorun::net`.
 */
class HttpServer {
    HttpServerConfig config_;
    HttpHandler handler_;

    std::unique_ptr<gorun::net::TcpListener> listener_;
    gorun::Semaphore conn_sem_;

    gorun::Context server_ctx_;
    gorun::CancelFunc server_cancel_;
    gorun::WaitGroup active_conns_wg_;

    std::mutex active_sockets_mtx_;
    std::unordered_map<std::uint64_t, std::weak_ptr<gorun::net::detail::CancelState>> active_sockets_;
    std::atomic<std::uint64_t> next_conn_id_{1};

    std::atomic<bool> running_{false};
    std::atomic<bool> stopping_{false};
    std::atomic<std::uint64_t> total_requests_{0};
    std::atomic<std::uint64_t> active_connections_{0};

    void handle_connection(gorun::net::TcpStream sock, std::uint64_t conn_id) {
        GORUN_DEFER {
            sock.close();
            {
                std::lock_guard<std::mutex> lk(active_sockets_mtx_);
                active_sockets_.erase(conn_id);
            }
            active_connections_.fetch_sub(1, std::memory_order_relaxed);
            conn_sem_.release();
            active_conns_wg_.done();
        };

        const std::string peer = sock.peer_address();
        std::string recv_buf;
        recv_buf.reserve(4096);
        char chunk[4096];

        while (!stopping_.load(std::memory_order_acquire)) {
            // 1. Read until complete HTTP headers arrive (zero-timer fast-path via gorun::net!)
            ParseHeaderResult parsed{};
            bool read_failed = false;

            for (;;) {
                parsed = parse_http_headers(recv_buf, config_.max_header_bytes, config_.max_body_bytes);
                if (parsed.status != ParseStatus::Incomplete) {
                    break;
                }

                auto r = sock.read(chunk, sizeof(chunk), config_.read_timeout);
                if (!r.ok() || r.eof()) {
                    read_failed = true;
                    break;
                }
                recv_buf.append(chunk, static_cast<std::size_t>(r.bytes));
            }

            if (read_failed) {
                break;
            }

            if (parsed.status == ParseStatus::BadRequest) {
                auto wire = HttpResponse::text(400, "Bad Request\n").serialize(false);
                (void)sock.write_all(wire, config_.write_timeout);
                break;
            }
            if (parsed.status == ParseStatus::HeadersTooLarge) {
                auto wire = HttpResponse::text(431, "Request Header Fields Too Large\n").serialize(false);
                (void)sock.write_all(wire, config_.write_timeout);
                break;
            }
            if (parsed.status == ParseStatus::PayloadTooLarge) {
                auto wire = HttpResponse::text(413, "Payload Too Large\n").serialize(false);
                (void)sock.write_all(wire, config_.write_timeout);
                break;
            }

            // 2. Read remaining body bytes if Content-Length > 0
            const std::size_t total_msg_size = parsed.header_end_pos + parsed.content_length;
            while (recv_buf.size() < total_msg_size) {
                auto r = sock.read(chunk, sizeof(chunk), config_.read_timeout);
                if (!r.ok() || r.eof()) {
                    read_failed = true;
                    break;
                }
                recv_buf.append(chunk, static_cast<std::size_t>(r.bytes));
            }
            if (read_failed) {
                break;
            }

            HttpRequest req = std::move(parsed.request);
            req.remote_addr = peer;
            if (parsed.content_length > 0) {
                req.body.assign(recv_buf.data() + parsed.header_end_pos, parsed.content_length);
            }
            recv_buf.erase(0, total_msg_size);

            const bool keep_alive = req.keep_alive() && !stopping_.load(std::memory_order_acquire);
            total_requests_.fetch_add(1, std::memory_order_relaxed);

            // 3. Execute request handler inside this green routine with exception isolation
            HttpResponse resp;
            try {
                if (handler_) {
                    resp = handler_(req, server_ctx_);
                } else {
                    resp = HttpResponse::text(404, "Not Found\n");
                }
            } catch (const std::exception& ex) {
                resp = HttpResponse::error(500, "internal_server_error", ex.what());
            } catch (...) {
                resp = HttpResponse::error(500, "internal_server_error");
            }

            // 4. Write serialized HTTP/1.1 response (zero-timer fast path!)
            std::string wire = resp.serialize(keep_alive);
            auto wr = sock.write_all(wire, config_.write_timeout);
            if (!wr.ok() || static_cast<std::size_t>(wr.bytes) != wire.size()) {
                break;
            }

            if (!keep_alive) {
                break;
            }
        }
    }

public:
    explicit HttpServer(HttpServerConfig cfg, HttpHandler handler = nullptr)
        : config_(std::move(cfg))
        , handler_(std::move(handler))
        , conn_sem_(static_cast<std::ptrdiff_t>(config_.max_connections > 0 ? config_.max_connections : 1024)) {}

    ~HttpServer() {
        stop();
    }

    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    void set_handler(HttpHandler handler) {
        handler_ = std::move(handler);
    }

    /// Binds the native `gorun::net::TcpListener` and spawns the non-blocking accept green routine.
    void start() {
        bool expected = false;
        if (!running_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            return;
        }
        stopping_.store(false, std::memory_order_release);

        auto [ctx, cancel] = gorun::with_cancel(gorun::background());
        server_ctx_ = std::move(ctx);
        server_cancel_ = std::move(cancel);

        try {
            listener_ = std::make_unique<gorun::net::TcpListener>(
                gorun::net::TcpListener::bind(config_.host, config_.port)
            );
        } catch (...) {
            running_.store(false, std::memory_order_release);
            listener_.reset();
            throw;
        }

        // Spawn lightweight accept loop green routine (parks on IOCP / epoll / kqueue)
        active_conns_wg_.add(1);
        gorun::go(gorun::StackSize::Compact, [this]() {
            GORUN_DEFER { active_conns_wg_.done(); };
            while (!stopping_.load(std::memory_order_acquire)) {
                auto peer_opt = listener_->accept(server_ctx_);
                if (!peer_opt) {
                    if (stopping_.load(std::memory_order_acquire) || server_ctx_.is_canceled()) {
                        break;
                    }
                    continue;
                }

                gorun::net::TcpStream peer_sock = std::move(*peer_opt);
                if (stopping_.load(std::memory_order_acquire)) {
                    peer_sock.close();
                    break;
                }

                if (!conn_sem_.try_acquire()) {
                    peer_sock.close();
                    continue;
                }

                const std::uint64_t conn_id = next_conn_id_.fetch_add(1, std::memory_order_relaxed);
                {
                    std::lock_guard<std::mutex> lk(active_sockets_mtx_);
                    active_sockets_.emplace(conn_id, peer_sock.cancel_handle());
                }

                active_connections_.fetch_add(1, std::memory_order_relaxed);
                active_conns_wg_.add(1);

                gorun::go(config_.connection_stack_size, [this, s = std::move(peer_sock), conn_id]() mutable {
                    handle_connection(std::move(s), conn_id);
                });
            }
        });
    }

    /// Gracefully stops accepting new connections, wakes idle Keep-Alive readers, and awaits in-flight requests.
    void stop() {
        bool expected = true;
        if (!running_.compare_exchange_strong(expected, false, std::memory_order_acq_rel)) {
            return;
        }

        stopping_.store(true, std::memory_order_release);
        if (server_cancel_) {
            server_cancel_();
        }
        if (listener_) {
            listener_->close();
        }

        // Cancel all idle/waiting sockets cleanly via their thread-safe CancelState handles
        {
            std::vector<std::shared_ptr<gorun::net::detail::CancelState>> to_cancel;
            {
                std::lock_guard<std::mutex> lk(active_sockets_mtx_);
                to_cancel.reserve(active_sockets_.size());
                for (auto& [_, weak_st] : active_sockets_) {
                    if (auto st = weak_st.lock()) {
                        to_cancel.push_back(std::move(st));
                    }
                }
            }
            for (auto& st : to_cancel) {
                st->cancel_io();
            }
        }

        active_conns_wg_.wait();
        listener_.reset();
    }

    /// Registers `start()` / `stop()` hooks on a `goforge::Lifecycle` for Uber Fx-style App integration.
    void attach_lifecycle(Lifecycle& lc, std::string hook_name = "http-server") {
        lc.append({
            .name = std::move(hook_name),
            .on_start = [this](gorun::Context) { start(); },
            .on_stop = [this](gorun::Context) { stop(); }
        });
    }

    [[nodiscard]] std::uint16_t port() const noexcept {
        return listener_ ? listener_->local_port() : config_.port;
    }

    [[nodiscard]] std::uint64_t total_requests() const noexcept {
        return total_requests_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::uint64_t active_connections() const noexcept {
        return active_connections_.load(std::memory_order_relaxed);
    }
};

} // namespace goforge::http
