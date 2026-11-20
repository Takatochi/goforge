#pragma once

/**
 * @file context.hpp
 * @brief Gin-style per-request `HttpContext` with path/query parameters, URL decoding,
 *        typed key-value store, `gorun::Context` cancellation, `ServiceScope` DI access,
 *        response builders, and middleware chain control (`next()`, `abort()`).
 */

#include <any>
#include <charconv>
#include <cctype>
#include <cstddef>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "gorun/gorun.hpp"
#include "goforge/di.hpp"
#include "goforge/http/http_types.hpp"

namespace goforge::web {

class HttpContext;

/// Middleware and route handler signature (mirrors `gin.HandlerFunc`).
using HandlerFunc = std::function<void(HttpContext&)>;

namespace detail {

inline int hex_digit_value(char c) noexcept {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

inline std::string url_decode(std::string_view input, bool plus_as_space = true) {
    std::string out;
    out.reserve(input.size());
    for (std::size_t i = 0; i < input.size(); ++i) {
        char c = input[i];
        if (plus_as_space && c == '+') {
            out.push_back(' ');
        } else if (c == '%' && i + 2 < input.size()) {
            int hi = hex_digit_value(input[i + 1]);
            int lo = hex_digit_value(input[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
            } else {
                out.push_back(c);
            }
        } else {
            out.push_back(c);
        }
    }
    return out;
}

inline std::unordered_map<std::string, std::string> parse_query_string(std::string_view raw) {
    std::unordered_map<std::string, std::string> result;
    std::size_t pos = 0;
    while (pos < raw.size()) {
        auto amp = raw.find('&', pos);
        std::string_view pair = (amp == std::string_view::npos)
            ? raw.substr(pos)
            : raw.substr(pos, amp - pos);
        pos = (amp == std::string_view::npos) ? raw.size() : amp + 1;

        if (pair.empty()) continue;
        auto eq = pair.find('=');
        if (eq == std::string_view::npos) {
            result.insert_or_assign(url_decode(pair), std::string{});
        } else {
            result.insert_or_assign(
                url_decode(pair.substr(0, eq)),
                url_decode(pair.substr(eq + 1))
            );
        }
    }
    return result;
}

template <typename T>
std::optional<T> parse_number(std::string_view sv) noexcept {
    if (sv.empty()) return std::nullopt;
    T value{};
    const char* first = sv.data();
    const char* last = first + sv.size();
    auto [ptr, ec] = std::from_chars(first, last, value);
    if (ec != std::errc{} || ptr != last) {
        return std::nullopt;
    }
    return value;
}

} // namespace detail

/**
 * @brief Sleeps cooperatively inside a green routine until `duration` elapses or `ctx` is canceled.
 * @return `true` if the full duration elapsed, `false` if canceled by `ctx` (e.g., Timeout middleware).
 */
inline bool sleep_with_context(gorun::Context ctx, std::chrono::milliseconds duration) {
    if (ctx.is_canceled()) return false;
    bool completed = false;
    gorun::select(
        gorun::on_timeout(duration, [&]() { completed = true; }),
        ctx.on_done([&]() { completed = false; })
    );
    return completed;
}

/**
 * @class HttpContext
 * @brief Per-request execution context passed through the middleware chain and route handler.
 */
class HttpContext {
    const http::HttpRequest* req_;
    http::HttpResponse resp_;
    gorun::Context go_ctx_;
    ServiceScope* scope_{nullptr};

    std::unordered_map<std::string, std::string> params_;
    mutable bool query_parsed_{false};
    mutable std::unordered_map<std::string, std::string> query_params_;
    std::unordered_map<std::string, std::any> values_;

    std::vector<HandlerFunc> handlers_;
    std::size_t handler_index_{0};
    bool aborted_{false};
    bool written_{false};

    void ensure_query_parsed() const {
        if (!query_parsed_) {
            query_params_ = detail::parse_query_string(req_->raw_query);
            query_parsed_ = true;
        }
    }

public:
    HttpContext(const http::HttpRequest& req,
                gorun::Context go_ctx,
                ServiceScope* scope = nullptr)
        : req_(&req)
        , go_ctx_(std::move(go_ctx))
        , scope_(scope) {}

    HttpContext(const HttpContext&) = delete;
    HttpContext& operator=(const HttpContext&) = delete;

    // ---- Request inspection ----
    [[nodiscard]] const http::HttpRequest& request() const noexcept { return *req_; }
    [[nodiscard]] const std::string& method() const noexcept { return req_->method; }
    [[nodiscard]] const std::string& path() const noexcept { return req_->path; }
    [[nodiscard]] const std::string& raw_query() const noexcept { return req_->raw_query; }
    [[nodiscard]] const std::string& body() const noexcept { return req_->body; }
    [[nodiscard]] const std::string& remote_addr() const noexcept { return req_->remote_addr; }

    [[nodiscard]] std::optional<std::string_view> header(std::string_view name) const {
        return req_->headers.get(name);
    }

    [[nodiscard]] std::string header_or(std::string_view name, std::string_view fallback = "") const {
        return req_->headers.get_or(name, fallback);
    }

    // ---- Path parameters (:id, *filepath) ----
    void set_params(std::unordered_map<std::string, std::string> params) {
        params_ = std::move(params);
    }

    [[nodiscard]] std::optional<std::string_view> param(std::string_view key) const {
        auto it = params_.find(std::string(key));
        if (it == params_.end()) return std::nullopt;
        return std::string_view(it->second);
    }

    [[nodiscard]] std::string param_or(std::string_view key, std::string_view fallback = "") const {
        auto v = param(key);
        return v ? std::string(*v) : std::string(fallback);
    }

    template <typename T>
        requires std::is_arithmetic_v<T>
    [[nodiscard]] std::optional<T> param_as(std::string_view key) const noexcept {
        auto v = param(key);
        if (!v) return std::nullopt;
        return detail::parse_number<T>(*v);
    }

    // ---- Query parameters (?key=val&...) ----
    [[nodiscard]] std::optional<std::string_view> query(std::string_view key) const {
        ensure_query_parsed();
        auto it = query_params_.find(std::string(key));
        if (it == query_params_.end()) return std::nullopt;
        return std::string_view(it->second);
    }

    [[nodiscard]] std::string query_or(std::string_view key, std::string_view fallback = "") const {
        auto v = query(key);
        return v ? std::string(*v) : std::string(fallback);
    }

    template <typename T>
        requires std::is_arithmetic_v<T>
    [[nodiscard]] std::optional<T> query_as(std::string_view key) const noexcept {
        auto v = query(key);
        if (!v) return std::nullopt;
        return detail::parse_number<T>(*v);
    }

    // ---- Per-request typed key-value bag (Gin-style Set/Get) ----
    template <typename T>
    void set(std::string key, T&& value) {
        values_.insert_or_assign(std::move(key), std::forward<T>(value));
    }

    template <typename T>
    [[nodiscard]] std::optional<T> get(const std::string& key) const {
        auto it = values_.find(key);
        if (it == values_.end()) return std::nullopt;
        if (const T* ptr = std::any_cast<T>(&it->second)) {
            return *ptr;
        }
        return std::nullopt;
    }

    [[nodiscard]] bool has_value(const std::string& key) const {
        return values_.contains(key);
    }

    // ---- gorun::Context cancellation & timeouts ----
    [[nodiscard]] gorun::Context go_ctx() const noexcept { return go_ctx_; }

    void set_go_ctx(gorun::Context ctx) noexcept { go_ctx_ = std::move(ctx); }

    [[nodiscard]] bool is_canceled() const noexcept { return go_ctx_.is_canceled(); }

    // ---- DI ServiceScope integration ----
    [[nodiscard]] bool has_scope() const noexcept { return scope_ != nullptr; }

    [[nodiscard]] ServiceScope& scope() {
        if (!scope_) {
            throw ScopeRequiredError("HttpContext has no active DI ServiceScope attached.");
        }
        return *scope_;
    }

    template <typename T>
    [[nodiscard]] T& resolve() {
        return scope().resolve<T>();
    }

    template <typename T>
    [[nodiscard]] std::shared_ptr<std::remove_cv_t<T>> resolve_ptr() {
        return scope().resolve_ptr<T>();
    }

    template <typename Fn>
    decltype(auto) invoke(Fn&& fn) {
        return scope().invoke(std::forward<Fn>(fn));
    }

    // ---- Response builders ----
    HttpContext& status(int code) {
        resp_.status = code;
        written_ = true;
        return *this;
    }

    HttpContext& set_header(std::string_view name, std::string value) {
        resp_.headers.set(name, std::move(value));
        return *this;
    }

    void text(int code, std::string body) {
        resp_.status = code;
        resp_.body = std::move(body);
        resp_.headers.set("Content-Type", "text/plain; charset=utf-8");
        written_ = true;
    }

    void json(int code, std::string json_body) {
        resp_.status = code;
        resp_.body = std::move(json_body);
        resp_.headers.set("Content-Type", "application/json; charset=utf-8");
        written_ = true;
    }

    void no_content() {
        resp_.status = 204;
        resp_.body.clear();
        written_ = true;
    }

    [[nodiscard]] bool is_written() const noexcept { return written_; }
    [[nodiscard]] http::HttpResponse& response() noexcept { return resp_; }
    [[nodiscard]] const http::HttpResponse& response() const noexcept { return resp_; }

    // ---- Middleware chain execution ----
    void set_handlers(std::vector<HandlerFunc> chain) {
        handlers_ = std::move(chain);
        handler_index_ = 0;
        aborted_ = false;
    }

    void next() {
        while (handler_index_ < handlers_.size() && !aborted_) {
            const std::size_t idx = handler_index_++;
            if (handlers_[idx]) {
                handlers_[idx](*this);
            }
        }
    }

    void abort() noexcept {
        aborted_ = true;
        handler_index_ = handlers_.size();
    }

    void abort_with_text(int code, std::string msg) {
        text(code, std::move(msg));
        abort();
    }

    void abort_with_json(int code, std::string json_body) {
        json(code, std::move(json_body));
        abort();
    }

    [[nodiscard]] bool is_aborted() const noexcept { return aborted_; }
};

} // namespace goforge::web
