#pragma once

/**
 * @file http_server.hpp
 * @brief Non-blocking HTTP/1.1 server built on Standalone Asio + `gorun` M:N green threads.
 *
 * Each accepted TCP connection runs in its own lightweight `gorun` green routine:
 * - Zero OS-thread blocking on `accept`, `read`, `write`, `gorun::Mutex`, `gorun::Channel`, or `sleep_for`.
 * - HTTP/1.1 Keep-Alive connection reuse with per-request read/write deadlines.
 * - Panic/exception isolation: an uncaught exception in a handler returns `500 Internal Server Error`
 *   without bringing down the server or other active requests.
 * - Graceful shutdown integrated with `goforge::Lifecycle` and `gorun::WaitGroup`.
 */

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

#include "gorun/gorun.hpp"
#include "goforge/lifecycle.hpp"
#include "goforge/http/http_types.hpp"
#include "goforge/net/asio_bridge.hpp"

namespace goforge::http {

/**
 * @struct HttpServerConfig
 * @brief Configuration for `goforge::http::HttpServer`.
 */
struct HttpServerConfig {
    std::string host{"127.0.0.1"};
    std::uint16_t port{8080}; ///< Use `0` to bind an OS-assigned ephemeral port
    std::size_t io_threads{1};
    std::size_t max_connections{10000};
    std::chrono::milliseconds read_timeout{std::chrono::seconds(15)};
    std::chrono::milliseconds write_timeout{std::chrono::seconds(15)};
    std::size_t max_header_bytes{16 * 1024};
    std::size_t max_body_bytes{4 * 1024 * 1024};
};

using HttpHandler = std::function<HttpResponse(const HttpRequest&, gorun::Context)>;

/**
 * @class HttpServer
 * @brief High-concurrency non-blocking HTTP/1.1 server executing each connection on a `gorun` green routine.
 */
class HttpServer {
    HttpServerConfig config_;
    HttpHandler handler_;

    std::unique_ptr<net::AsioExecutor> executor_;
    std::unique_ptr<net::AsyncTcpAcceptor> acceptor_;
    gorun::Semaphore conn_sem_;

    gorun::Context server_ctx_;
    gorun::CancelFunc server_cancel_;
    gorun::WaitGroup accept_wg_;
    gorun::WaitGroup active_conns_wg_;

    std::atomic<bool> running_{false};
    std::atomic<std::uint64_t> total_requests_{0};
    std::atomic<std::uint64_t> active_connections_{0};

    void handle_connection(net::AsyncTcpSocket sock) {
        GORUN_DEFER {
            sock.close();
            active_connections_.fetch_sub(1, std::memory_order_relaxed);
            conn_sem_.release();
            active_conns_wg_.done();
        };

        const std::string peer = sock.remote_address();
        std::string recv_buf;
        recv_buf.reserve(4096);
        char chunk[4096];

        while (!server_ctx_.is_canceled()) {
            // 1. Read until complete HTTP headers arrive
            ParseHeaderResult parsed{};
            bool read_failed = false;

            for (;;) {
                parsed = parse_http_headers(recv_buf, config_.max_header_bytes, config_.max_body_bytes);
                if (parsed.status != ParseStatus::Incomplete) {
                    break;
                }

                auto [read_ctx, cancel_read] = gorun::with_timeout(server_ctx_, config_.read_timeout);
                GORUN_DEFER { cancel_read(); };

                auto [n, ec] = sock.read_some(chunk, sizeof(chunk), read_ctx);
                if (ec || n == 0) {
                    read_failed = true;
                    break;
                }
                recv_buf.append(chunk, n);
            }

            if (read_failed) {
                break;
            }

            if (parsed.status == ParseStatus::BadRequest) {
                auto wire = HttpResponse::text(400, "Bad Request\n").serialize(false);
                (void)sock.write_all(wire, server_ctx_);
                break;
            }
            if (parsed.status == ParseStatus::HeadersTooLarge) {
                auto wire = HttpResponse::text(431, "Request Header Fields Too Large\n").serialize(false);
                (void)sock.write_all(wire, server_ctx_);
                break;
            }
            if (parsed.status == ParseStatus::PayloadTooLarge) {
                auto wire = HttpResponse::text(413, "Payload Too Large\n").serialize(false);
                (void)sock.write_all(wire, server_ctx_);
                break;
            }

            // 2. Read remaining body bytes if Content-Length > 0
            const std::size_t total_msg_size = parsed.header_end_pos + parsed.content_length;
            while (recv_buf.size() < total_msg_size) {
                auto [read_ctx, cancel_read] = gorun::with_timeout(server_ctx_, config_.read_timeout);
                GORUN_DEFER { cancel_read(); };

                auto [n, ec] = sock.read_some(chunk, sizeof(chunk), read_ctx);
                if (ec || n == 0) {
                    read_failed = true;
                    break;
                }
                recv_buf.append(chunk, n);
            }
            if (read_failed) {
                break;
            }

            HttpRequest req = std::move(parsed.request);
            req.remote_addr = peer;
            if (parsed.content_length > 0) {
                req.body.assign(recv_buf.data() + parsed.header_end_pos, parsed.content_length);
            }
            // Erase consumed request bytes while keeping any pipelined bytes for next iteration
            recv_buf.erase(0, total_msg_size);

            const bool keep_alive = req.keep_alive() && !server_ctx_.is_canceled();
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
                resp = HttpResponse::json(
                    500,
                    std::string("{\"error\":\"internal_server_error\",\"message\":\"") + ex.what() + "\"}"
                );
            } catch (...) {
                resp = HttpResponse::json(500, "{\"error\":\"internal_server_error\"}");
            }

            // 4. Write serialized HTTP/1.1 response without blocking OS workers
            std::string wire = resp.serialize(keep_alive);
            auto [write_ctx, cancel_write] = gorun::with_timeout(server_ctx_, config_.write_timeout);
            GORUN_DEFER { cancel_write(); };

            auto [written, wec] = sock.write_all(wire, write_ctx);
            if (wec || written != wire.size()) {
                break;
            }

            if (!keep_alive) {
                break;
            }
        }
    }

    void accept_loop() {
        GORUN_DEFER { accept_wg_.done(); };

        while (!server_ctx_.is_canceled()) {
            auto [sock_opt, ec] = acceptor_->accept(server_ctx_);
            if (ec || !sock_opt) {
                if (server_ctx_.is_canceled()) {
                    break;
                }
                gorun::sleep_for(std::chrono::milliseconds(1));
                continue;
            }

            conn_sem_.acquire();
            if (server_ctx_.is_canceled()) {
                conn_sem_.release();
                sock_opt->close();
                break;
            }

            active_connections_.fetch_add(1, std::memory_order_relaxed);
            active_conns_wg_.add(1);

            // Spawn 1 lightweight gorun green routine per accepted connection (move-only AsyncTcpSocket!)
            gorun::go([this, peer_sock = std::move(*sock_opt)]() mutable {
                handle_connection(std::move(peer_sock));
            });
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

    /// Starts the Asio event loop, binds the TCP listener, and launches the `gorun` accept routine.
    void start() {
        bool expected = false;
        if (!running_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            return;
        }

        auto [ctx, cancel] = gorun::with_cancel(gorun::background());
        server_ctx_ = std::move(ctx);
        server_cancel_ = std::move(cancel);

        executor_ = std::make_unique<net::AsioExecutor>(config_.io_threads);
        acceptor_ = std::make_unique<net::AsyncTcpAcceptor>(executor_->context());

        auto ec = acceptor_->listen(config_.host, config_.port);
        if (ec) {
            running_.store(false, std::memory_order_release);
            acceptor_.reset();
            executor_.reset();
            throw std::runtime_error(
                "HttpServer failed to listen on " + config_.host + ":" +
                std::to_string(config_.port) + " -> " + ec.message()
            );
        }

        accept_wg_.add(1);
        gorun::go([this]() {
            accept_loop();
        });
    }

    /// Gracefully stops accepting new connections, cancels idle readers, waits for active requests, and stops Asio.
    void stop() {
        bool expected = true;
        if (!running_.compare_exchange_strong(expected, false, std::memory_order_acq_rel)) {
            return;
        }

        if (server_cancel_) {
            server_cancel_();
        }
        if (acceptor_) {
            acceptor_->close();
        }

        accept_wg_.wait();
        active_conns_wg_.wait();

        acceptor_.reset();
        if (executor_) {
            executor_->stop();
            executor_.reset();
        }
    }

    /// Registers `start()` / `stop()` hooks on a `goforge::Lifecycle` for Uber Fx-style App integration.
    void attach_lifecycle(Lifecycle& lc, std::string hook_name = "http-server") {
        lc.append({
            .name = std::move(hook_name),
            .on_start = [this](gorun::Context) { start(); },
            .on_stop = [this](gorun::Context) { stop(); }
        });
    }

    /// Returns the actual bound TCP port (especially useful when `config.port == 0`).
    [[nodiscard]] std::uint16_t port() const {
        return acceptor_ ? acceptor_->local_port() : config_.port;
    }

    [[nodiscard]] std::uint64_t total_requests() const noexcept {
        return total_requests_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::uint64_t active_connections() const noexcept {
        return active_connections_.load(std::memory_order_relaxed);
    }
};

} // namespace goforge::http
