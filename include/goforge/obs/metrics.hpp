#pragma once

/**
 * @file metrics.hpp
 * @brief Production observability for `goforge`: `MetricsRegistry` (RPS, status counters,
 *        in-flight gauge, p50/p95/p99 latency percentiles, JSON & Prometheus exposition)
 *        and `HealthRegistry` (`/healthz`, `/readyz`, `/metrics`).
 */

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "gorun/gorun.hpp"
#include "goforge/obs/logger.hpp"
#include "goforge/web/context.hpp"
#include "goforge/web/router.hpp"

namespace goforge::obs {

struct MetricsSnapshot {
    double uptime_sec{0.0};
    double avg_rps{0.0};
    std::uint64_t total_requests{0};
    std::int64_t in_flight{0};
    std::uint64_t status_2xx{0};
    std::uint64_t status_3xx{0};
    std::uint64_t status_4xx{0};
    std::uint64_t status_5xx{0};
    std::uint64_t latency_p50_us{0};
    std::uint64_t latency_p95_us{0};
    std::uint64_t latency_p99_us{0};
    std::uint64_t latency_max_us{0};
};

/**
 * @class MetricsRegistry
 * @brief Lightweight HTTP metrics collector tracking request counts, status classes, in-flight gauge,
 *        and sliding-window latency percentiles (p50/p95/p99/max) without blocking OS threads.
 */
class MetricsRegistry {
    static constexpr std::size_t kWindowSize = 4096;

    std::chrono::steady_clock::time_point started_at_{std::chrono::steady_clock::now()};
    std::atomic<std::uint64_t> total_requests_{0};
    std::atomic<std::int64_t> in_flight_{0};
    std::atomic<std::uint64_t> status_2xx_{0};
    std::atomic<std::uint64_t> status_3xx_{0};
    std::atomic<std::uint64_t> status_4xx_{0};
    std::atomic<std::uint64_t> status_5xx_{0};

    mutable gorun::Mutex lat_mtx_;
    std::array<std::uint64_t, kWindowSize> latencies_{};
    std::size_t lat_pos_{0};
    std::size_t lat_count_{0};

    static std::uint64_t percentile_sorted(const std::vector<std::uint64_t>& sorted, double q) noexcept {
        if (sorted.empty()) return 0;
        double idx = std::ceil(q * static_cast<double>(sorted.size())) - 1.0;
        if (idx < 0.0) idx = 0.0;
        std::size_t i = static_cast<std::size_t>(idx);
        if (i >= sorted.size()) i = sorted.size() - 1;
        return sorted[i];
    }

public:
    MetricsRegistry() = default;

    void record(int status_code, std::uint64_t latency_us) {
        total_requests_.fetch_add(1, std::memory_order_relaxed);
        if (status_code >= 200 && status_code < 300) {
            status_2xx_.fetch_add(1, std::memory_order_relaxed);
        } else if (status_code >= 300 && status_code < 400) {
            status_3xx_.fetch_add(1, std::memory_order_relaxed);
        } else if (status_code >= 400 && status_code < 500) {
            status_4xx_.fetch_add(1, std::memory_order_relaxed);
        } else if (status_code >= 500) {
            status_5xx_.fetch_add(1, std::memory_order_relaxed);
        }

        std::lock_guard<gorun::Mutex> lk(lat_mtx_);
        latencies_[lat_pos_] = latency_us;
        lat_pos_ = (lat_pos_ + 1) % kWindowSize;
        if (lat_count_ < kWindowSize) {
            ++lat_count_;
        }
    }

    [[nodiscard]] MetricsSnapshot snapshot() const {
        MetricsSnapshot s;
        auto now = std::chrono::steady_clock::now();
        s.uptime_sec = std::chrono::duration<double>(now - started_at_).count();
        s.total_requests = total_requests_.load(std::memory_order_relaxed);
        s.in_flight = in_flight_.load(std::memory_order_relaxed);
        s.status_2xx = status_2xx_.load(std::memory_order_relaxed);
        s.status_3xx = status_3xx_.load(std::memory_order_relaxed);
        s.status_4xx = status_4xx_.load(std::memory_order_relaxed);
        s.status_5xx = status_5xx_.load(std::memory_order_relaxed);
        s.avg_rps = (s.uptime_sec > 1e-6) ? (static_cast<double>(s.total_requests) / s.uptime_sec) : 0.0;

        std::vector<std::uint64_t> copy;
        {
            std::lock_guard<gorun::Mutex> lk(lat_mtx_);
            copy.assign(latencies_.begin(), latencies_.begin() + static_cast<std::ptrdiff_t>(lat_count_));
        }

        if (!copy.empty()) {
            std::sort(copy.begin(), copy.end());
            s.latency_p50_us = percentile_sorted(copy, 0.50);
            s.latency_p95_us = percentile_sorted(copy, 0.95);
            s.latency_p99_us = percentile_sorted(copy, 0.99);
            s.latency_max_us = copy.back();
        }
        return s;
    }

    [[nodiscard]] std::string to_json() const {
        auto s = snapshot();
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2);
        oss << "{"
            << "\"uptime_sec\":" << s.uptime_sec << ","
            << "\"total_requests\":" << s.total_requests << ","
            << "\"in_flight\":" << s.in_flight << ","
            << "\"avg_rps\":" << s.avg_rps << ","
            << "\"status\":{"
            << "\"2xx\":" << s.status_2xx << ","
            << "\"3xx\":" << s.status_3xx << ","
            << "\"4xx\":" << s.status_4xx << ","
            << "\"5xx\":" << s.status_5xx
            << "},"
            << "\"latency_us\":{"
            << "\"p50\":" << s.latency_p50_us << ","
            << "\"p95\":" << s.latency_p95_us << ","
            << "\"p99\":" << s.latency_p99_us << ","
            << "\"max\":" << s.latency_max_us
            << "}"
            << "}";
        return oss.str();
    }

    [[nodiscard]] std::string to_prometheus() const {
        auto s = snapshot();
        std::ostringstream oss;
        oss << "# TYPE goforge_http_requests_total counter\n"
            << "goforge_http_requests_total " << s.total_requests << "\n"
            << "# TYPE goforge_http_in_flight gauge\n"
            << "goforge_http_in_flight " << s.in_flight << "\n"
            << "# TYPE goforge_http_responses_by_class_total counter\n"
            << "goforge_http_responses_by_class_total{class=\"2xx\"} " << s.status_2xx << "\n"
            << "goforge_http_responses_by_class_total{class=\"3xx\"} " << s.status_3xx << "\n"
            << "goforge_http_responses_by_class_total{class=\"4xx\"} " << s.status_4xx << "\n"
            << "goforge_http_responses_by_class_total{class=\"5xx\"} " << s.status_5xx << "\n"
            << "# TYPE goforge_http_latency_microseconds summary\n"
            << "goforge_http_latency_microseconds{quantile=\"0.50\"} " << s.latency_p50_us << "\n"
            << "goforge_http_latency_microseconds{quantile=\"0.95\"} " << s.latency_p95_us << "\n"
            << "goforge_http_latency_microseconds{quantile=\"0.99\"} " << s.latency_p99_us << "\n";
        return oss.str();
    }

    /// Returns a Gin-style middleware recording every HTTP request's status and latency.
    [[nodiscard]] web::HandlerFunc middleware() {
        return [this](web::HttpContext& c) {
            in_flight_.fetch_add(1, std::memory_order_relaxed);
            auto t0 = std::chrono::steady_clock::now();
            bool recorded = false;
            GORUN_DEFER {
                in_flight_.fetch_sub(1, std::memory_order_relaxed);
                if (!recorded) {
                    auto dt = static_cast<std::uint64_t>(
                        std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - t0
                        ).count()
                    );
                    record(500, dt);
                }
            };
            c.next();
            auto dt = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - t0
                ).count()
            );
            record(c.response().status, dt);
            recorded = true;
        };
    }
};

struct HealthCheckResult {
    bool healthy{true};
    std::string detail{"ok"};
};

/**
 * @class HealthRegistry
 * @brief Kubernetes-style liveness (`/healthz`), readiness (`/readyz`), and `/metrics` endpoint mount.
 */
class HealthRegistry {
    struct CheckEntry {
        std::string name;
        std::function<HealthCheckResult()> fn;
    };

    gorun::Mutex mtx_;
    std::vector<CheckEntry> checks_;

public:
    HealthRegistry() = default;

    void add_check(std::string name, std::function<HealthCheckResult()> check_fn) {
        std::lock_guard<gorun::Mutex> lk(mtx_);
        checks_.push_back(CheckEntry{std::move(name), std::move(check_fn)});
    }

    /// Evaluates all readiness checks and returns `{all_healthy, json_body}`.
    [[nodiscard]] std::pair<bool, std::string> evaluate_readiness() {
        std::vector<CheckEntry> snapshot;
        {
            std::lock_guard<gorun::Mutex> lk(mtx_);
            snapshot = checks_;
        }

        bool all_ok = true;
        std::string checks_json = "{";
        for (std::size_t i = 0; i < snapshot.size(); ++i) {
            HealthCheckResult r;
            try {
                r = snapshot[i].fn ? snapshot[i].fn() : HealthCheckResult{true, "ok"};
            } catch (const std::exception& ex) {
                r = HealthCheckResult{false, ex.what()};
            } catch (...) {
                r = HealthCheckResult{false, "unknown error"};
            }
            if (!r.healthy) all_ok = false;
            if (i > 0) checks_json.push_back(',');
            detail::append_json_escaped(checks_json, snapshot[i].name);
            checks_json += ":{\"healthy\":";
            checks_json += (r.healthy ? "true" : "false");
            checks_json += ",\"detail\":";
            detail::append_json_escaped(checks_json, r.detail);
            checks_json.push_back('}');
        }
        checks_json.push_back('}');

        std::string body = std::string("{\"status\":\"") + (all_ok ? "ready" : "degraded") +
                           "\",\"checks\":" + checks_json + "}";
        return {all_ok, std::move(body)};
    }

    /// Mounts `/healthz`, `/readyz`, and `/metrics` onto a `goforge::web::Router`.
    void mount(web::Router& router, MetricsRegistry& metrics) {
        router.get("/healthz", [](web::HttpContext& c) {
            c.json(200, "{\"status\":\"alive\"}");
        });

        router.get("/readyz", [this](web::HttpContext& c) {
            auto [ok, body] = evaluate_readiness();
            c.json(ok ? 200 : 503, std::move(body));
        });

        router.get("/metrics", [&metrics](web::HttpContext& c) {
            if (c.query_or("format") == "prometheus") {
                c.text(200, metrics.to_prometheus());
            } else {
                c.json(200, metrics.to_json());
            }
        });
    }
};

} // namespace goforge::obs
