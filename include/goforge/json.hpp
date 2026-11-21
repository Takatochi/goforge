#pragma once

/**
 * @file json.hpp
 * @brief Ergonomic JSON serialization, DTO binding, `std::optional<T>` support, and
 *        validation primitives powered by `nlohmann/json`.
 *
 * Provides:
 * - `goforge::Json` (alias for `nlohmann::json`) and `goforge::H` (Gin-style object map builder)
 * - Automatic `std::optional<T>` serialization/deserialization (`null` <-> `std::nullopt`)
 * - `GOFORGE_JSON_DTO(Type, fields...)` for strict DTOs (missing field -> 400 Bad Request)
 * - `GOFORGE_JSON_DTO_DEFAULT(Type, fields...)` for partial/PATCH DTOs using struct defaults
 * - Automatic `.validate()` hook detection (`ValidatableDto` concept -> 422 Unprocessable Entity)
 */

#include <concepts>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include <nlohmann/json.hpp>

// Native `std::optional<T>` support across all goforge DTOs!
namespace nlohmann {

template <typename T>
struct adl_serializer<std::optional<T>> {
    static void to_json(json& j, const std::optional<T>& opt) {
        if (opt.has_value()) {
            j = *opt;
        } else {
            j = nullptr;
        }
    }

    static void from_json(const json& j, std::optional<T>& opt) {
        if (j.is_null()) {
            opt = std::nullopt;
        } else {
            opt = j.get<T>();
        }
    }
};

} // namespace nlohmann

/**
 * @brief Declares strict JSON serialization & deserialization for a DTO struct.
 *        Missing fields or type mismatches produce a clear `400 Bad Request` bind error.
 */
#define GOFORGE_JSON_DTO(Type, ...) NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Type, __VA_ARGS__)

/**
 * @brief Declares JSON serialization & deserialization using C++ member default values
 *        for omitted JSON fields (ideal for PATCH requests, optional fields, and configs).
 */
#define GOFORGE_JSON_DTO_DEFAULT(Type, ...) NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(Type, __VA_ARGS__)

namespace goforge {

/// Primary JSON value type (`nlohmann::json`).
using Json = nlohmann::json;

/// Gin-style shorthand for building inline JSON objects: `goforge::H{{"status", "ok"}, {"count", 42}}`.
using H = nlohmann::json;

/**
 * @brief Concept satisfied by DTO structs defining `std::optional<std::string> validate() const`.
 *        When satisfied, `HttpContext::bind_json<T>()` automatically runs `validate()` and responds
 *        with `422 Unprocessable Entity` if validation fails.
 */
template <typename T>
concept ValidatableDto = requires(const T& dto) {
    { dto.validate() } -> std::convertible_to<std::optional<std::string>>;
};

/**
 * @struct BindResult
 * @brief Result of parsing and validating a JSON request body into a typed DTO `T`.
 */
template <typename T>
struct BindResult {
    std::optional<T> value;
    int http_status{200};
    std::string error_code;
    std::string message;

    [[nodiscard]] bool ok() const noexcept { return value.has_value(); }
    explicit operator bool() const noexcept { return ok(); }
};

/// Builds a standardized JSON error payload: `{"error": code, "message": msg}`.
inline Json make_error_json(std::string_view error_code, std::string_view message = "") {
    Json obj = Json::object();
    obj["error"] = std::string(error_code);
    if (!message.empty()) {
        obj["message"] = std::string(message);
    }
    return obj;
}

/// Parses a raw JSON payload into a generic `goforge::Json` document with clean error reporting.
inline BindResult<Json> parse_json_body(std::string_view body) {
    BindResult<Json> res;
    if (body.empty()) {
        res.http_status = 400;
        res.error_code = "invalid_json";
        res.message = "request body is empty";
        return res;
    }
    try {
        res.value = Json::parse(body.begin(), body.end());
        res.http_status = 200;
    } catch (const Json::parse_error& ex) {
        res.http_status = 400;
        res.error_code = "invalid_json";
        res.message = ex.what();
    } catch (const std::exception& ex) {
        res.http_status = 400;
        res.error_code = "invalid_json";
        res.message = ex.what();
    }
    return res;
}

/**
 * @brief Parses JSON body into typed DTO `T`, checking schema/types (`400 Bad Request`) and
 *        invoking `dto.validate()` (`422 Unprocessable Entity`) if `T` satisfies `ValidatableDto`.
 */
template <typename T>
BindResult<T> parse_and_validate_json(std::string_view body) {
    BindResult<T> res;
    auto parsed = parse_json_body(body);
    if (!parsed.ok()) {
        res.http_status = parsed.http_status;
        res.error_code = std::move(parsed.error_code);
        res.message = std::move(parsed.message);
        return res;
    }

    try {
        T dto = parsed.value->template get<T>();
        if constexpr (ValidatableDto<T>) {
            if (auto validation_err = dto.validate(); validation_err.has_value()) {
                res.http_status = 422;
                res.error_code = "validation_failed";
                res.message = std::move(*validation_err);
                return res;
            }
        }
        res.value = std::move(dto);
        res.http_status = 200;
    } catch (const Json::type_error& ex) {
        res.http_status = 400;
        res.error_code = "invalid_payload";
        res.message = ex.what();
    } catch (const Json::out_of_range& ex) {
        res.http_status = 400;
        res.error_code = "missing_field";
        res.message = ex.what();
    } catch (const std::exception& ex) {
        res.http_status = 400;
        res.error_code = "invalid_payload";
        res.message = ex.what();
    }
    return res;
}

} // namespace goforge
