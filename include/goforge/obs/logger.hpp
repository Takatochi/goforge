#pragma once

/**
 * @file logger.hpp
 * @brief Non-OS-blocking structured JSON logger (`StructuredLogger`) with RFC-8259 string escaping,
 *        UTC timestamps, log-level filtering, and built-in Gin-style access log middleware.
 */

#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <initializer_list>
#include <iostream>
#include <mutex>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>

#include "gorun/gorun.hpp"
#include "goforge/web/context.hpp"
#include "goforge/web/middleware.hpp"

namespace goforge::obs {

enum class LogLevel : std::uint8_t {
    Debug = 0,
    Info = 1,
    Warn = 2,
    Error = 3
};

namespace detail {

inline std::string_view level_to_string(LogLevel lvl) noexcept {
    switch (lvl) {
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info:  return "INFO";
        case LogLevel::Warn:  return "WARN";
        case LogLevel::Error: return "ERROR";
    }
    return "INFO";
}

inline void append_json_escaped(std::string& out, std::string_view input) {
    out.push_back('"');
    for (unsigned char c : input) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
                    out += buf;
                } else {
                    out.push_back(static_cast<char>(c));
                }
                break;
        }
    }
    out.push_back('"');
}

inline std::string utc_timestamp_iso8601() {
    using namespace std::chrono;
    auto now = system_clock::now();
    auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
    std::time_t tt = system_clock::to_time_t(now);
    std::tm utc_tm{};
#if defined(_WIN32)
    gmtime_s(&utc_tm, &tt);
#else
    gmtime_r(&tt, &utc_tm);
#endif
    char buf[32];
    std::snprintf(
        buf,
        sizeof(buf),
        "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
        utc_tm.tm_year + 1900,
        utc_tm.tm_mon + 1,
        utc_tm.tm_mday,
        utc_tm.tm_hour,
        utc_tm.tm_min,
        utc_tm.tm_sec,
        static_cast<int>(ms.count())
    );
    return std::string(buf);
}

} // namespace detail

/**
 * @class StructuredLogger
 * @brief Thread-safe, green-routine-friendly JSON line logger synchronized with `gorun::Mutex`.
 */
class StructuredLogger {
    gorun::Mutex mtx_;
    std::ostream* out_;
    std::atomic<LogLevel> min_level_;

public:
    explicit StructuredLogger(LogLevel min_level = LogLevel::Info, std::ostream& out = std::cout) noexcept
        : out_(&out), min_level_(min_level) {}

    void set_level(LogLevel lvl) noexcept {
        min_level_.store(lvl, std::memory_order_release);
    }

    [[nodiscard]] LogLevel level() const noexcept {
        return min_level_.load(std::memory_order_acquire);
    }

    void log(LogLevel lvl,
             std::string_view msg,
             std::initializer_list<std::pair<std::string_view, std::string_view>> fields = {}) {
        if (static_cast<std::uint8_t>(lvl) < static_cast<std::uint8_t>(min_level_.load(std::memory_order_acquire))) {
            return;
        }

        std::string line;
        line.reserve(128 + msg.size());
        line += "{\"ts\":";
        detail::append_json_escaped(line, detail::utc_timestamp_iso8601());
        line += ",\"level\":";
        detail::append_json_escaped(line, detail::level_to_string(lvl));
        line += ",\"msg\":";
        detail::append_json_escaped(line, msg);

        for (const auto& [k, v] : fields) {
            line.push_back(',');
            detail::append_json_escaped(line, k);
            line.push_back(':');
            detail::append_json_escaped(line, v);
        }
        line += "}\n";

        std::lock_guard<gorun::Mutex> lk(mtx_);
        (*out_) << line;
        out_->flush();
    }

    void debug(std::string_view msg,
               std::initializer_list<std::pair<std::string_view, std::string_view>> fields = {}) {
        log(LogLevel::Debug, msg, fields);
    }

    void info(std::string_view msg,
              std::initializer_list<std::pair<std::string_view, std::string_view>> fields = {}) {
        log(LogLevel::Info, msg, fields);
    }

    void warn(std::string_view msg,
              std::initializer_list<std::pair<std::string_view, std::string_view>> fields = {}) {
        log(LogLevel::Warn, msg, fields);
    }

    void error(std::string_view msg,
               std::initializer_list<std::pair<std::string_view, std::string_view>> fields = {}) {
        log(LogLevel::Error, msg, fields);
    }

    /// Returns a Gin-style access logging middleware writing structured JSON lines for each HTTP request.
    [[nodiscard]] web::HandlerFunc access_log_middleware() {
        return web::middleware::request_logger([this](const web::middleware::RequestLogEntry& e) {
            std::string status_str = std::to_string(e.status);
            std::string latency_str = std::to_string(e.latency_us);
            LogLevel lvl = (e.status >= 500) ? LogLevel::Error
                         : (e.status >= 400) ? LogLevel::Warn
                                             : LogLevel::Info;
            log(lvl, "http_request", {
                {"method", e.method},
                {"path", e.path},
                {"status", status_str},
                {"latency_us", latency_str},
                {"remote", e.remote_addr}
            });
        });
    }
};

} // namespace goforge::obs
