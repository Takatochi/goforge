#pragma once

/**
 * @file middleware.hpp
 * @brief Built-in Gin-style middleware components for `goforge::web::Router`:
 *        `recovery`, `request_logger`, `timeout`, `rate_limit` (via `gorun::Semaphore`),
 *        and `bearer_auth`.
 */

#include <chrono>
#include <cstddef>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "gorun/gorun.hpp"
#include "goforge/web/context.hpp"

namespace goforge::web::middleware {

/**
 * @struct RequestLogEntry
 * @brief Structured access-log record emitted by `request_logger(...)`.
 */
struct RequestLogEntry {
    std::string method;
    std::string path;
    std::string remote_addr;
    int status{200};
    std::int64_t latency_us{0};
};

/**
 * @brief Catches uncaught C++ exceptions in downstream middleware/handlers and returns
 *        a `500 Internal Server Error` JSON response without crashing the server.
 */
inline HandlerFunc recovery(std::function<void(const HttpContext&, const std::string&)> on_error = nullptr) {
    return [cb = std::move(on_error)](HttpContext& c) {
        try {
            c.next();
        } catch (const std::exception& ex) {
            if (cb) cb(c, ex.what());
            c.abort_with_error(500, "internal_server_error", ex.what());
        } catch (...) {
            if (cb) cb(c, "unknown exception");
            c.abort_with_error(500, "internal_server_error");
        }
    };
}

/**
 * @brief Measures request latency and emits a structured `RequestLogEntry` after `c.next()`.
 */
inline HandlerFunc request_logger(std::function<void(const RequestLogEntry&)> sink) {
    return [sink = std::move(sink)](HttpContext& c) {
        auto t0 = std::chrono::steady_clock::now();
        c.next();
        auto dt = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - t0
        ).count();
        if (sink) {
            sink(RequestLogEntry{
                .method = c.method(),
                .path = c.path(),
                .remote_addr = c.remote_addr(),
                .status = c.response().status,
                .latency_us = dt
            });
        }
    };
}

/**
 * @brief Attaches a `gorun::with_timeout(c.go_ctx(), limit)` deadline to `HttpContext`.
 *
 * Cooperative handlers/middleware observing `c.go_ctx()` (via `gorun::select`, `sleep_with_context`,
 * or `AsyncTcpSocket`) abort immediately upon deadline expiration. If the deadline expires without
 * an explicit response (or with `DeadlineExceeded`), this middleware returns `504 Gateway Timeout`.
 */
inline HandlerFunc timeout(std::chrono::milliseconds limit) {
    return [limit](HttpContext& c) {
        auto [tctx, cancel] = gorun::with_timeout(c.go_ctx(), limit);
        GORUN_DEFER { cancel(); };

        gorun::Context prev = c.go_ctx();
        c.set_go_ctx(tctx);
        GORUN_DEFER { c.set_go_ctx(prev); };

        c.next();

        if (tctx.err() == gorun::ContextError::DeadlineExceeded && !c.is_written()) {
            c.abort_with_error(504, "gateway_timeout", "request deadline exceeded");
        }
    };
}

/**
 * @brief Concurrency limiter backed by `gorun::Semaphore` (zero OS-thread blocking).
 * @param max_concurrent Maximum number of concurrent green routines allowed through this middleware.
 * @param reject_immediately If `true`, immediately returns `429 Too Many Requests` when saturated;
 *                           if `false`, parks excess green routines on `gorun::Semaphore` until a slot opens.
 */
inline HandlerFunc rate_limit(std::size_t max_concurrent, bool reject_immediately = false) {
    auto sem = std::make_shared<gorun::Semaphore>(
        static_cast<std::ptrdiff_t>(max_concurrent > 0 ? max_concurrent : 1)
    );
    return [sem, reject_immediately](HttpContext& c) {
        if (reject_immediately) {
            if (!sem->try_acquire()) {
                c.abort_with_error(429, "too_many_requests");
                return;
            }
        } else {
            sem->acquire();
        }
        GORUN_DEFER { sem->release(); };
        c.next();
    };
}

/**
 * @brief Bearer token authentication middleware (`Authorization: Bearer <token>`).
 * @param validator Receives the raw token and returns an authenticated subject string (or `std::nullopt` to reject with 401).
 */
inline HandlerFunc bearer_auth(std::function<std::optional<std::string>(std::string_view)> validator) {
    return [validator = std::move(validator)](HttpContext& c) {
        auto auth = c.header("Authorization");
        constexpr std::string_view kPrefix = "Bearer ";
        if (!auth || auth->size() <= kPrefix.size() || auth->substr(0, kPrefix.size()) != kPrefix) {
            c.abort_with_error(401, "unauthorized", "missing bearer token");
            return;
        }
        std::string_view token = auth->substr(kPrefix.size());
        auto subject = validator ? validator(token) : std::nullopt;
        if (!subject) {
            c.abort_with_error(401, "unauthorized", "invalid token");
            return;
        }
        c.set("auth_subject", std::move(*subject));
        c.next();
    };
}

} // namespace goforge::web::middleware
