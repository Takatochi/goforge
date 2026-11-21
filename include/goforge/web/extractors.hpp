#pragma once

/**
 * @file extractors.hpp
 * @brief Ergonomic typed HTTP extractors (`Body<T>`, `Path<"id", int>`, `Query<"page", int>`,
 *        `Header<"Authorization">`) and smart return types (`ApiResponse`, `Result<T>`, `ok()`,
 *        `created()`, `not_found()`, `err()`) for zero-boilerplate route handlers & controllers.
 */

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "goforge/json.hpp"
#include "goforge/web/context.hpp"

namespace goforge::web {

/**
 * @struct FixedString
 * @brief C++20 structural string wrapper enabling compile-time named extractors like `Path<"id", int>`.
 */
template <std::size_t N>
struct FixedString {
    char data[N]{};

    constexpr FixedString(const char (&str)[N]) noexcept {
        for (std::size_t i = 0; i < N; ++i) {
            data[i] = str[i];
        }
    }

    [[nodiscard]] constexpr std::string_view view() const noexcept {
        return std::string_view(data, N > 0 ? N - 1 : 0);
    }
};

// ============================================================================
// Typed Request Extractors
// ============================================================================

/// Extracts and validates a JSON request body into DTO `T` (`400` on syntax/type error, `422` on `.validate()` fail).
template <typename T>
struct Body {
    using value_type = T;
    T value;

    [[nodiscard]] T& operator*() noexcept { return value; }
    [[nodiscard]] const T& operator*() const noexcept { return value; }
    [[nodiscard]] T* operator->() noexcept { return &value; }
    [[nodiscard]] const T* operator->() const noexcept { return &value; }
    operator const T&() const noexcept { return value; }
    operator T&() noexcept { return value; }
};

/// Shorthand extractor for untyped JSON request bodies (`Body<goforge::Json>`).
using JsonBody = Body<Json>;

/// Extracts a required path parameter (`:name`) converted to `T` (`400` if missing or invalid number).
template <FixedString Name, typename T = std::string>
struct Path {
    using value_type = T;
    static constexpr auto param_name = Name;

    T value{};

    [[nodiscard]] const T& operator*() const noexcept { return value; }
    [[nodiscard]] T& operator*() noexcept { return value; }
    [[nodiscard]] const T* operator->() const noexcept { return &value; }
    operator const T&() const noexcept { return value; }
};

/// Extracts an optional query parameter (`?name=val`) converted to `T` (`400` if present but invalid number).
template <FixedString Name, typename T = std::string>
struct Query {
    using value_type = T;
    static constexpr auto param_name = Name;

    std::optional<T> value{};

    [[nodiscard]] bool has_value() const noexcept { return value.has_value(); }
    explicit operator bool() const noexcept { return value.has_value(); }
    [[nodiscard]] const T& operator*() const { return *value; }
    [[nodiscard]] T value_or(T fallback) const { return value.value_or(std::move(fallback)); }
};

/// Extracts an optional HTTP request header (`Header<"Authorization">`).
template <FixedString Name>
struct Header {
    static constexpr auto header_name = Name;

    std::optional<std::string> value{};

    [[nodiscard]] bool has_value() const noexcept { return value.has_value(); }
    explicit operator bool() const noexcept { return value.has_value(); }
    [[nodiscard]] const std::string& operator*() const { return *value; }
    [[nodiscard]] std::string value_or(std::string fallback) const { return value.value_or(std::move(fallback)); }
};

// ============================================================================
// Smart Handler Return Types (`ApiResponse`, `Result<T>`, Helper Factories)
// ============================================================================

/**
 * @struct ApiResponse
 * @brief Lightweight value-type HTTP response returned directly from typed handlers or controllers.
 */
struct ApiResponse {
    int status{200};
    std::string content_type{"application/json"};
    std::string body;
    std::vector<std::pair<std::string, std::string>> headers;

    ApiResponse& header(std::string key, std::string val) {
        headers.emplace_back(std::move(key), std::move(val));
        return *this;
    }

    void apply(HttpContext& ctx) const {
        for (const auto& [k, v] : headers) {
            ctx.set_header(k, v);
        }
        if (status == 204 && body.empty()) {
            ctx.status(204);
            return;
        }
        ctx.data(status, content_type, body);
    }
};

/// Returns `200 OK` with empty JSON `{}`.
inline ApiResponse ok() {
    return ApiResponse{200, "application/json", "{}", {}};
}

/// Returns `200 OK` with any JSON-serializable DTO, `Json`/`H`, `std::vector`, or `Page<T>`.
template <typename T>
inline ApiResponse ok(const T& payload) {
    Json j = payload;
    return ApiResponse{200, "application/json", j.dump(), {}};
}

/// Returns `201 Created` with any JSON-serializable payload.
template <typename T>
inline ApiResponse created(const T& payload) {
    Json j = payload;
    return ApiResponse{201, "application/json", j.dump(), {}};
}

/// Returns `202 Accepted` with any JSON-serializable payload.
template <typename T>
inline ApiResponse accepted(const T& payload) {
    Json j = payload;
    return ApiResponse{202, "application/json", j.dump(), {}};
}

/// Returns `204 No Content`.
inline ApiResponse no_content() {
    return ApiResponse{204, "text/plain; charset=utf-8", "", {}};
}

/// Returns plain text with the given HTTP status (default `200`).
inline ApiResponse text(std::string content, int status_code = 200) {
    return ApiResponse{status_code, "text/plain; charset=utf-8", std::move(content), {}};
}

/// Returns HTML with the given HTTP status (default `200`).
inline ApiResponse html(std::string content, int status_code = 200) {
    return ApiResponse{status_code, "text/html; charset=utf-8", std::move(content), {}};
}

/// Returns a structured JSON error response: `{"error": code, "message": msg}`.
inline ApiResponse err(int status_code, std::string_view code, std::string_view message = "") {
    return ApiResponse{status_code, "application/json", make_error_json(code, message).dump(), {}};
}

inline ApiResponse bad_request(std::string_view message = "") {
    return err(400, "bad_request", message);
}

inline ApiResponse unauthorized(std::string_view message = "") {
    return err(401, "unauthorized", message);
}

inline ApiResponse forbidden(std::string_view message = "") {
    return err(403, "forbidden", message);
}

inline ApiResponse not_found(std::string_view message = "") {
    return err(404, "not_found", message);
}

inline ApiResponse conflict(std::string_view message = "") {
    return err(409, "conflict", message);
}

/**
 * @class Result
 * @brief Either a typed success payload `T` (serialized as `200 OK` JSON) or an explicit `ApiResponse`
 *        (e.g. `created(dto)`, `not_found("...")`, `err(409, ...)`).
 */
template <typename T>
class Result {
    std::variant<T, ApiResponse> storage_;

public:
    Result(T val) : storage_(std::move(val)) {}
    Result(ApiResponse resp) : storage_(std::move(resp)) {}

    [[nodiscard]] bool is_ok() const noexcept { return std::holds_alternative<T>(storage_); }

    void apply(HttpContext& ctx) const {
        if (const auto* val = std::get_if<T>(&storage_)) {
            ctx.json(200, *val);
        } else {
            std::get<ApiResponse>(storage_).apply(ctx);
        }
    }
};

// ============================================================================
// Extraction & Result Serialization Traits (used by `Router` / `adapt_handler`)
// ============================================================================
namespace detail {

template <typename T> struct IsBodyExtractor : std::false_type {};
template <typename T> struct IsBodyExtractor<Body<T>> : std::true_type {};

template <typename T> struct IsPathExtractor : std::false_type {};
template <FixedString Name, typename T> struct IsPathExtractor<Path<Name, T>> : std::true_type {};

template <typename T> struct IsQueryExtractor : std::false_type {};
template <FixedString Name, typename T> struct IsQueryExtractor<Query<Name, T>> : std::true_type {};

template <typename T> struct IsHeaderExtractor : std::false_type {};
template <FixedString Name> struct IsHeaderExtractor<Header<Name>> : std::true_type {};

template <typename T> struct IsOptionalType : std::false_type {};
template <typename T> struct IsOptionalType<std::optional<T>> : std::true_type {};

template <typename T> struct IsResultType : std::false_type {};
template <typename T> struct IsResultType<Result<T>> : std::true_type {};

template <typename T>
constexpr bool is_http_extractor_v =
    IsBodyExtractor<std::remove_cvref_t<T>>::value ||
    IsPathExtractor<std::remove_cvref_t<T>>::value ||
    IsQueryExtractor<std::remove_cvref_t<T>>::value ||
    IsHeaderExtractor<std::remove_cvref_t<T>>::value;

template <typename TargetT>
std::optional<TargetT> convert_param_value(std::string_view sv) {
    if constexpr (std::is_same_v<TargetT, std::string>) {
        return std::string(sv);
    } else if constexpr (std::is_same_v<TargetT, bool>) {
        if (sv == "true" || sv == "1" || sv == "yes") return true;
        if (sv == "false" || sv == "0" || sv == "no") return false;
        return std::nullopt;
    } else if constexpr (std::is_arithmetic_v<TargetT>) {
        return parse_number<TargetT>(sv);
    } else {
        static_assert(std::is_same_v<TargetT, std::string>,
                      "Path/Query extractor supports std::string, bool, and arithmetic types.");
        return std::nullopt;
    }
}

/// Extracts a single HTTP extractor (`Body`, `Path`, `Query`, `Header`) into `std::optional<Clean>`,
/// automatically writing a 400/422 JSON error and aborting `ctx` on failure.
template <typename Clean>
std::optional<Clean> extract_http_arg(HttpContext& ctx) {
    if constexpr (IsBodyExtractor<Clean>::value) {
        using DtoT = typename Clean::value_type;
        if constexpr (std::is_same_v<DtoT, Json>) {
            auto parsed = ctx.bind_json_value();
            if (!parsed) return std::nullopt;
            return Clean{std::move(*parsed)};
        } else {
            auto parsed = ctx.bind_json<DtoT>();
            if (!parsed) return std::nullopt;
            return Clean{std::move(*parsed)};
        }
    } else if constexpr (IsPathExtractor<Clean>::value) {
        using ValT = typename Clean::value_type;
        constexpr std::string_view key = Clean::param_name.view();
        auto raw = ctx.param(key);
        if (!raw) {
            ctx.abort_with_json(400, make_error_json("missing_path_param", "missing path parameter: " + std::string(key)));
            return std::nullopt;
        }
        auto parsed = convert_param_value<ValT>(*raw);
        if (!parsed) {
            ctx.abort_with_json(400, make_error_json("invalid_path_param", "invalid path parameter: " + std::string(key)));
            return std::nullopt;
        }
        return Clean{std::move(*parsed)};
    } else if constexpr (IsQueryExtractor<Clean>::value) {
        using ValT = typename Clean::value_type;
        constexpr std::string_view key = Clean::param_name.view();
        auto raw = ctx.query(key);
        if (!raw) {
            return Clean{std::nullopt};
        }
        auto parsed = convert_param_value<ValT>(*raw);
        if (!parsed) {
            ctx.abort_with_json(400, make_error_json("invalid_query_param", "invalid query parameter: " + std::string(key)));
            return std::nullopt;
        }
        return Clean{std::move(*parsed)};
    } else if constexpr (IsHeaderExtractor<Clean>::value) {
        constexpr std::string_view key = Clean::header_name.view();
        auto raw = ctx.header(key);
        if (!raw) return Clean{std::nullopt};
        return Clean{std::string(*raw)};
    }
}

/// Automatically writes a typed handler's return value to `HttpContext`.
template <typename R>
void write_handler_result(HttpContext& ctx, R&& res) {
    using CleanR = std::remove_cvref_t<R>;
    if constexpr (std::is_same_v<CleanR, ApiResponse>) {
        res.apply(ctx);
    } else if constexpr (std::is_same_v<CleanR, http::HttpResponse>) {
        ctx.response() = std::forward<R>(res);
    } else if constexpr (IsResultType<CleanR>::value) {
        res.apply(ctx);
    } else if constexpr (IsOptionalType<CleanR>::value) {
        if (res.has_value()) {
            write_handler_result(ctx, *res);
        } else {
            ctx.error(404, "not_found");
        }
    } else if constexpr (std::is_same_v<CleanR, Json>) {
        ctx.json(200, res);
    } else if constexpr (std::is_same_v<CleanR, std::string> ||
                         std::is_same_v<CleanR, std::string_view> ||
                         std::is_same_v<CleanR, const char*>) {
        ctx.text(200, std::string(res));
    } else {
        ctx.json(200, res);
    }
}

} // namespace detail
} // namespace goforge::web

// Re-export ergonomic HTTP extractors & response helpers into `namespace goforge`
namespace goforge {

using web::FixedString;
using web::Body;
using web::JsonBody;
using web::Path;
using web::Query;
using web::Header;
using web::ApiResponse;
using web::Result;
using web::ok;
using web::created;
using web::accepted;
using web::no_content;
using web::text;
using web::html;
using web::err;
using web::bad_request;
using web::unauthorized;
using web::forbidden;
using web::not_found;
using web::conflict;

} // namespace goforge
