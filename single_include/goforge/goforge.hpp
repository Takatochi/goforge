#pragma once
// =====================================================================================
//  goforge — single-header core distribution (GENERATED FILE, DO NOT EDIT)
//  Fx-Style DI, Lifecycle, BlockingPool, Gin-Style Router, HttpContext, Middleware &
//  Production Observability for C++20/23 on top of gorun.
//
//  Source:  https://github.com/Takatochi/goforge
//  Author:  Artur Havor
//  License: Apache License 2.0
//
//  Regenerate with: python scripts/amalgamate.py
// =====================================================================================
// ---- begin goforge/goforge.hpp ----

/**
 * @file goforge.hpp
 * @brief Umbrella header for the goforge C++20/23 web & application framework built on gorun.
 */

// ---- begin goforge/json.hpp ----

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
// ---- end goforge/json.hpp ----
// ---- begin goforge/query.hpp ----

/**
 * @file query.hpp
 * @brief Fluent C# / EF-style Query Engine (`goforge::query`) with Compile-Time Expression Trees (AST),
 *        member-pointer selectors, `it` placeholder expressions, opt-in short lambda macros (`GF_FN`),
 *        entity column reflection (`GOFORGE_MODEL`), and JSON-ready pagination (`Page<T>`).
 *
 * Designed as a unified query foundation:
 * - Today: zero-overhead fluent in-memory querying (`from(users).where(col(&User::age) >= 18).order_by(&User::name).to_vec()`).
 * - Future (`goforge::data`): the exact same compile-time AST (`ColExpr`, `CmpExpr`, `LogicExpr`, `StrExpr`,
 *   `InExpr`, `BetweenExpr`, `NullCheckExpr`, `EntityMeta<T>`) compiles directly to parameterized SQL for an
 *   Entity Framework-style `DbContext` / `DbSet<T>`.
 */

#include <algorithm>
#include <cctype>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <numeric>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>



// ============================================================================
// Opt-in Concise Lambda Shorthand Macros (zero runtime overhead)
// ============================================================================

/// Concise single-parameter expression lambda: `GF_FN(u, u.age >= 18 && u.active)`
#ifndef GF_FN
#define GF_FN(param, expr) [&](const auto& param) { return (expr); }
#endif

/// Concise two-parameter expression lambda: `GF_FN2(acc, u, acc + u.score)`
#ifndef GF_FN2
#define GF_FN2(p1, p2, expr) [&](const auto& p1, const auto& p2) { return (expr); }
#endif

/// Concise mutable action lambda: `GF_ACT(u, u.score += 10)`
#ifndef GF_ACT
#define GF_ACT(param, ...) [&](auto& param) { __VA_ARGS__; }
#endif

// ============================================================================
// Preprocessor Foreach Helpers for GOFORGE_MODEL (pure standard C++20, up to 24 fields)
// ============================================================================
#define GOFORGE_PP_EXPAND(x) x
#define GOFORGE_PP_NARG(...) GOFORGE_PP_EXPAND(GOFORGE_PP_NARG_IMPL(__VA_ARGS__, \
    24,23,22,21,20,19,18,17,16,15,14,13,12,11,10,9,8,7,6,5,4,3,2,1,0))
#define GOFORGE_PP_NARG_IMPL( \
    _1,_2,_3,_4,_5,_6,_7,_8,_9,_10,_11,_12,_13,_14,_15,_16,_17,_18,_19,_20,_21,_22,_23,_24,N,...) N

#define GOFORGE_PP_CAT(a, b) GOFORGE_PP_CAT_IMPL(a, b)
#define GOFORGE_PP_CAT_IMPL(a, b) a##b

#define GOFORGE_PP_FE_1(m, t, x) m(t, x)
#define GOFORGE_PP_FE_2(m, t, x, ...) m(t, x), GOFORGE_PP_EXPAND(GOFORGE_PP_FE_1(m, t, __VA_ARGS__))
#define GOFORGE_PP_FE_3(m, t, x, ...) m(t, x), GOFORGE_PP_EXPAND(GOFORGE_PP_FE_2(m, t, __VA_ARGS__))
#define GOFORGE_PP_FE_4(m, t, x, ...) m(t, x), GOFORGE_PP_EXPAND(GOFORGE_PP_FE_3(m, t, __VA_ARGS__))
#define GOFORGE_PP_FE_5(m, t, x, ...) m(t, x), GOFORGE_PP_EXPAND(GOFORGE_PP_FE_4(m, t, __VA_ARGS__))
#define GOFORGE_PP_FE_6(m, t, x, ...) m(t, x), GOFORGE_PP_EXPAND(GOFORGE_PP_FE_5(m, t, __VA_ARGS__))
#define GOFORGE_PP_FE_7(m, t, x, ...) m(t, x), GOFORGE_PP_EXPAND(GOFORGE_PP_FE_6(m, t, __VA_ARGS__))
#define GOFORGE_PP_FE_8(m, t, x, ...) m(t, x), GOFORGE_PP_EXPAND(GOFORGE_PP_FE_7(m, t, __VA_ARGS__))
#define GOFORGE_PP_FE_9(m, t, x, ...) m(t, x), GOFORGE_PP_EXPAND(GOFORGE_PP_FE_8(m, t, __VA_ARGS__))
#define GOFORGE_PP_FE_10(m, t, x, ...) m(t, x), GOFORGE_PP_EXPAND(GOFORGE_PP_FE_9(m, t, __VA_ARGS__))
#define GOFORGE_PP_FE_11(m, t, x, ...) m(t, x), GOFORGE_PP_EXPAND(GOFORGE_PP_FE_10(m, t, __VA_ARGS__))
#define GOFORGE_PP_FE_12(m, t, x, ...) m(t, x), GOFORGE_PP_EXPAND(GOFORGE_PP_FE_11(m, t, __VA_ARGS__))
#define GOFORGE_PP_FE_13(m, t, x, ...) m(t, x), GOFORGE_PP_EXPAND(GOFORGE_PP_FE_12(m, t, __VA_ARGS__))
#define GOFORGE_PP_FE_14(m, t, x, ...) m(t, x), GOFORGE_PP_EXPAND(GOFORGE_PP_FE_13(m, t, __VA_ARGS__))
#define GOFORGE_PP_FE_15(m, t, x, ...) m(t, x), GOFORGE_PP_EXPAND(GOFORGE_PP_FE_14(m, t, __VA_ARGS__))
#define GOFORGE_PP_FE_16(m, t, x, ...) m(t, x), GOFORGE_PP_EXPAND(GOFORGE_PP_FE_15(m, t, __VA_ARGS__))
#define GOFORGE_PP_FE_17(m, t, x, ...) m(t, x), GOFORGE_PP_EXPAND(GOFORGE_PP_FE_16(m, t, __VA_ARGS__))
#define GOFORGE_PP_FE_18(m, t, x, ...) m(t, x), GOFORGE_PP_EXPAND(GOFORGE_PP_FE_17(m, t, __VA_ARGS__))
#define GOFORGE_PP_FE_19(m, t, x, ...) m(t, x), GOFORGE_PP_EXPAND(GOFORGE_PP_FE_18(m, t, __VA_ARGS__))
#define GOFORGE_PP_FE_20(m, t, x, ...) m(t, x), GOFORGE_PP_EXPAND(GOFORGE_PP_FE_19(m, t, __VA_ARGS__))
#define GOFORGE_PP_FE_21(m, t, x, ...) m(t, x), GOFORGE_PP_EXPAND(GOFORGE_PP_FE_20(m, t, __VA_ARGS__))
#define GOFORGE_PP_FE_22(m, t, x, ...) m(t, x), GOFORGE_PP_EXPAND(GOFORGE_PP_FE_21(m, t, __VA_ARGS__))
#define GOFORGE_PP_FE_23(m, t, x, ...) m(t, x), GOFORGE_PP_EXPAND(GOFORGE_PP_FE_22(m, t, __VA_ARGS__))
#define GOFORGE_PP_FE_24(m, t, x, ...) m(t, x), GOFORGE_PP_EXPAND(GOFORGE_PP_FE_23(m, t, __VA_ARGS__))

#define GOFORGE_PP_FOR_EACH(m, t, ...) \
    GOFORGE_PP_EXPAND(GOFORGE_PP_CAT(GOFORGE_PP_FE_, GOFORGE_PP_NARG(__VA_ARGS__))(m, t, __VA_ARGS__))

#define GOFORGE_MODEL_FIELD_PAIR(Type, field) \
    ::goforge::query::TypedField<Type, decltype(Type::field)>{#field, &Type::field}

namespace goforge::query {

// ============================================================================
// Entity Metadata (Compile-Time Reflection for Query AST & Future SQL ORM)
// ============================================================================

template <typename Obj, typename MemberT>
struct TypedField {
    std::string_view name;
    MemberT Obj::* ptr;
};

template <typename Obj>
struct EntityMeta {
    static constexpr bool registered = false;
    static constexpr std::string_view table_name = "";
    static constexpr std::tuple<> fields() noexcept { return {}; }
};

/// Resolves the compile-time column name of a member pointer if `Obj` was declared with `GOFORGE_MODEL`.
template <typename Obj, typename MemberT>
constexpr std::string_view column_name_of(MemberT Obj::* ptr) noexcept {
    if constexpr (EntityMeta<Obj>::registered) {
        std::string_view found{};
        std::apply([&](const auto&... f) {
            (([&] {
                if constexpr (std::is_same_v<decltype(f.ptr), MemberT Obj::*>) {
                    if (f.ptr == ptr) {
                        found = f.name;
                    }
                }
            }()), ...);
        }, EntityMeta<Obj>::fields());
        return found;
    }
    return {};
}

/// Returns the table name of `Obj` if declared with `GOFORGE_MODEL` / `GOFORGE_MODEL_TABLE`.
template <typename Obj>
constexpr std::string_view table_name_of() noexcept {
    return EntityMeta<Obj>::table_name;
}

// ============================================================================
// Compile-Time Expression Tree (AST) — Unified In-Memory & Future SQL Contract
// ============================================================================

struct ExprBase {};

template <typename T>
concept IsExpr = std::derived_from<std::remove_cvref_t<T>, ExprBase>;

enum class CmpOp : std::uint8_t { Eq, Ne, Lt, Le, Gt, Ge };
enum class LogicOp : std::uint8_t { And, Or };
enum class StrOp : std::uint8_t { Contains, IContains, StartsWith, EndsWith };
enum class ArithOp : std::uint8_t { Add, Sub, Mul, Div };

namespace detail {

template <typename T>
struct IsOptional : std::false_type {};

template <typename T>
struct IsOptional<std::optional<T>> : std::true_type {};

inline std::string_view to_sv(const std::string& s) noexcept { return s; }
inline std::string_view to_sv(std::string_view sv) noexcept { return sv; }
inline std::string_view to_sv(const char* s) noexcept { return s ? std::string_view(s) : std::string_view{}; }

template <typename T>
inline std::optional<std::string_view> to_opt_sv(const std::optional<T>& opt) noexcept {
    if (!opt.has_value()) return std::nullopt;
    return to_sv(*opt);
}

template <typename T>
inline std::optional<std::string_view> to_opt_sv(const T& v) noexcept {
    return to_sv(v);
}

inline bool icontains_sv(std::string_view hay, std::string_view needle) noexcept {
    if (needle.empty()) return true;
    if (needle.size() > hay.size()) return false;
    auto it = std::search(
        hay.begin(), hay.end(),
        needle.begin(), needle.end(),
        [](char a, char b) {
            return std::tolower(static_cast<unsigned char>(a)) ==
                   std::tolower(static_cast<unsigned char>(b));
        }
    );
    return it != hay.end();
}

template <typename A, typename B>
constexpr bool compare_values(CmpOp op, const A& a, const B& b) {
    if constexpr (IsOptional<std::remove_cvref_t<A>>::value && !IsOptional<std::remove_cvref_t<B>>::value) {
        if (!a.has_value()) return op == CmpOp::Ne;
        return compare_values(op, *a, b);
    } else if constexpr (!IsOptional<std::remove_cvref_t<A>>::value && IsOptional<std::remove_cvref_t<B>>::value) {
        if (!b.has_value()) return op == CmpOp::Ne;
        return compare_values(op, a, *b);
    } else {
        switch (op) {
            case CmpOp::Eq: return a == b;
            case CmpOp::Ne: return a != b;
            case CmpOp::Lt: return a < b;
            case CmpOp::Le: return a <= b;
            case CmpOp::Gt: return a > b;
            case CmpOp::Ge: return a >= b;
        }
        return false;
    }
}

template <typename T>
auto normalize_literal(T&& v) {
    using NoRef = std::remove_reference_t<T>;
    using Clean = std::decay_t<T>;
    if constexpr (std::is_array_v<NoRef> && std::is_same_v<std::remove_cv_t<std::remove_extent_t<NoRef>>, char>) {
        return std::string(v);
    } else if constexpr (std::is_same_v<Clean, const char*> || std::is_same_v<Clean, char*>) {
        return v ? std::string(v) : std::string{};
    } else if constexpr (std::is_same_v<Clean, std::string_view>) {
        return std::string(v);
    } else {
        return std::forward<T>(v);
    }
}

} // namespace detail

/// AST literal value leaf node (exposes `.value` for future SQL bind parameter extraction).
template <typename T>
struct LiteralExpr : ExprBase {
    T value;

    constexpr explicit LiteralExpr(T v) : value(std::move(v)) {}

    template <typename Elem>
    constexpr const T& eval(const Elem&) const noexcept {
        return value;
    }
};

template <typename T>
auto wrap_expr(T&& x) {
    if constexpr (IsExpr<T>) {
        return std::forward<T>(x);
    } else {
        auto lit = detail::normalize_literal(std::forward<T>(x));
        return LiteralExpr<std::decay_t<decltype(lit)>>(std::move(lit));
    }
}

/// AST comparison node (`lhs <op> rhs`).
template <CmpOp Op, typename Lhs, typename Rhs>
struct CmpExpr : ExprBase {
    static constexpr CmpOp op = Op;
    Lhs lhs;
    Rhs rhs;

    template <typename Elem>
    constexpr bool eval(const Elem& elem) const {
        return detail::compare_values(Op, lhs.eval(elem), rhs.eval(elem));
    }

    template <typename Elem>
    constexpr bool operator()(const Elem& elem) const {
        return eval(elem);
    }
};

/// AST logical node (`lhs && rhs` or `lhs || rhs`).
template <LogicOp Op, typename Lhs, typename Rhs>
struct LogicExpr : ExprBase {
    static constexpr LogicOp op = Op;
    Lhs lhs;
    Rhs rhs;

    template <typename Elem>
    constexpr bool eval(const Elem& elem) const {
        if constexpr (Op == LogicOp::And) {
            return static_cast<bool>(lhs.eval(elem)) && static_cast<bool>(rhs.eval(elem));
        } else {
            return static_cast<bool>(lhs.eval(elem)) || static_cast<bool>(rhs.eval(elem));
        }
    }

    template <typename Elem>
    constexpr bool operator()(const Elem& elem) const {
        return eval(elem);
    }
};

/// AST logical negation node (`!inner`).
template <typename Inner>
struct NotExpr : ExprBase {
    Inner inner;

    template <typename Elem>
    constexpr bool eval(const Elem& elem) const {
        return !static_cast<bool>(inner.eval(elem));
    }

    template <typename Elem>
    constexpr bool operator()(const Elem& elem) const {
        return eval(elem);
    }
};

/// AST arithmetic node (`lhs + rhs`, `-`, `*`, `/`).
template <ArithOp Op, typename Lhs, typename Rhs>
struct ArithExpr : ExprBase {
    static constexpr ArithOp op = Op;
    Lhs lhs;
    Rhs rhs;

    template <typename Elem>
    constexpr auto eval(const Elem& elem) const {
        if constexpr (Op == ArithOp::Add) return lhs.eval(elem) + rhs.eval(elem);
        else if constexpr (Op == ArithOp::Sub) return lhs.eval(elem) - rhs.eval(elem);
        else if constexpr (Op == ArithOp::Mul) return lhs.eval(elem) * rhs.eval(elem);
        else return lhs.eval(elem) / rhs.eval(elem);
    }

    template <typename Elem>
    constexpr auto operator()(const Elem& elem) const {
        return eval(elem);
    }
};

/// AST string predicate node (`LIKE '%...%'`, `LIKE '...%'`, `LIKE '%...'`, case-insensitive contains).
template <StrOp Op, typename Target>
struct StrExpr : ExprBase {
    static constexpr StrOp op = Op;
    Target target;
    std::string pattern;

    template <typename Elem>
    bool eval(const Elem& elem) const {
        auto opt_sv = detail::to_opt_sv(target.eval(elem));
        if (!opt_sv.has_value()) return false;
        std::string_view s = *opt_sv;
        if constexpr (Op == StrOp::Contains) {
            return s.find(pattern) != std::string_view::npos;
        } else if constexpr (Op == StrOp::IContains) {
            return detail::icontains_sv(s, pattern);
        } else if constexpr (Op == StrOp::StartsWith) {
            return s.starts_with(pattern);
        } else {
            return s.ends_with(pattern);
        }
    }

    template <typename Elem>
    bool operator()(const Elem& elem) const {
        return eval(elem);
    }
};

/// AST set-membership node (`IN (...)` / `NOT IN (...)`).
template <typename Target, typename ValT, bool Negated = false>
struct InExpr : ExprBase {
    static constexpr bool negated = Negated;
    Target target;
    std::vector<ValT> values;

    template <typename Elem>
    bool eval(const Elem& elem) const {
        const auto& v = target.eval(elem);
        bool found = false;
        for (const auto& candidate : values) {
            if (detail::compare_values(CmpOp::Eq, v, candidate)) {
                found = true;
                break;
            }
        }
        return Negated ? !found : found;
    }

    template <typename Elem>
    bool operator()(const Elem& elem) const {
        return eval(elem);
    }
};

/// AST inclusive range node (`BETWEEN lo AND hi`).
template <typename Target, typename LoT, typename HiT>
struct BetweenExpr : ExprBase {
    Target target;
    LoT lo;
    HiT hi;

    template <typename Elem>
    constexpr bool eval(const Elem& elem) const {
        const auto& v = target.eval(elem);
        return detail::compare_values(CmpOp::Ge, v, lo) &&
               detail::compare_values(CmpOp::Le, v, hi);
    }

    template <typename Elem>
    constexpr bool operator()(const Elem& elem) const {
        return eval(elem);
    }
};

/// AST nullability node (`IS NULL` / `IS NOT NULL` for `std::optional<T>` fields).
template <typename Target, bool ExpectNull>
struct NullCheckExpr : ExprBase {
    static constexpr bool expect_null = ExpectNull;
    Target target;

    template <typename Elem>
    constexpr bool eval(const Elem& elem) const noexcept {
        const auto& v = target.eval(elem);
        if constexpr (detail::IsOptional<std::remove_cvref_t<decltype(v)>>::value) {
            return ExpectNull ? !v.has_value() : v.has_value();
        } else {
            return !ExpectNull;
        }
    }

    template <typename Elem>
    constexpr bool operator()(const Elem& elem) const noexcept {
        return eval(elem);
    }
};

/// Mixin giving `ColExpr` and `IdentityExpr` fluent predicate methods (`.contains()`, `.in()`, `.between()`, `.is_null()`).
template <typename Derived>
struct ExprMethods : ExprBase {
    const Derived& self() const noexcept { return static_cast<const Derived&>(*this); }

    StrExpr<StrOp::Contains, Derived> contains(std::string_view s) const {
        return StrExpr<StrOp::Contains, Derived>{{}, self(), std::string(s)};
    }

    StrExpr<StrOp::IContains, Derived> icontains(std::string_view s) const {
        return StrExpr<StrOp::IContains, Derived>{{}, self(), std::string(s)};
    }

    StrExpr<StrOp::StartsWith, Derived> starts_with(std::string_view s) const {
        return StrExpr<StrOp::StartsWith, Derived>{{}, self(), std::string(s)};
    }

    StrExpr<StrOp::EndsWith, Derived> ends_with(std::string_view s) const {
        return StrExpr<StrOp::EndsWith, Derived>{{}, self(), std::string(s)};
    }

    template <typename V>
    auto in(std::initializer_list<V> list) const {
        using StoredT = std::decay_t<decltype(detail::normalize_literal(std::declval<V>()))>;
        std::vector<StoredT> vals;
        vals.reserve(list.size());
        for (const auto& item : list) vals.push_back(detail::normalize_literal(item));
        return InExpr<Derived, StoredT, false>{{}, self(), std::move(vals)};
    }

    template <typename V>
    auto in(std::vector<V> vals) const {
        return InExpr<Derived, V, false>{{}, self(), std::move(vals)};
    }

    template <typename V>
    auto not_in(std::initializer_list<V> list) const {
        using StoredT = std::decay_t<decltype(detail::normalize_literal(std::declval<V>()))>;
        std::vector<StoredT> vals;
        vals.reserve(list.size());
        for (const auto& item : list) vals.push_back(detail::normalize_literal(item));
        return InExpr<Derived, StoredT, true>{{}, self(), std::move(vals)};
    }

    template <typename Lo, typename Hi>
    auto between(Lo&& lo, Hi&& hi) const {
        auto lo_n = detail::normalize_literal(std::forward<Lo>(lo));
        auto hi_n = detail::normalize_literal(std::forward<Hi>(hi));
        return BetweenExpr<Derived, std::decay_t<decltype(lo_n)>, std::decay_t<decltype(hi_n)>>{
            {}, self(), std::move(lo_n), std::move(hi_n)
        };
    }

    NullCheckExpr<Derived, true> is_null() const noexcept {
        return NullCheckExpr<Derived, true>{{}, self()};
    }

    NullCheckExpr<Derived, false> is_not_null() const noexcept {
        return NullCheckExpr<Derived, false>{{}, self()};
    }
};

/// AST column/field descriptor node (`col(&User::age)`). Carries both member pointer & column name for SQL.
template <typename Obj, typename MemberT>
struct ColExpr : ExprMethods<ColExpr<Obj, MemberT>> {
    using object_type = Obj;
    using member_type = MemberT;

    MemberT Obj::* ptr;
    std::string_view name{};

    constexpr ColExpr(MemberT Obj::* p, std::string_view col_name = {}) noexcept
        : ptr(p), name(col_name) {}

    constexpr const MemberT& eval(const Obj& obj) const noexcept {
        return obj.*ptr;
    }

    constexpr const MemberT& operator()(const Obj& obj) const noexcept {
        return obj.*ptr;
    }
};

/// AST identity node (`it`) for querying primitive/value collections (`from(nums).where(it > 10)`).
struct IdentityExpr : ExprMethods<IdentityExpr> {
    template <typename T>
    constexpr const T& eval(const T& x) const noexcept {
        return x;
    }

    template <typename T>
    constexpr const T& operator()(const T& x) const noexcept {
        return x;
    }
};

/// Global identity placeholder for primitive/value expressions: `from(nums).where(it > 5 && it < 20)`.
inline constexpr IdentityExpr it{};

/// Creates a compile-time `ColExpr` AST node for a struct field (auto-attaches column name if `GOFORGE_MODEL` was used).
template <typename Obj, typename MemberT>
constexpr ColExpr<Obj, MemberT> col(MemberT Obj::* ptr, std::string_view custom_name = {}) noexcept {
    return ColExpr<Obj, MemberT>(ptr, custom_name.empty() ? column_name_of(ptr) : custom_name);
}

/// Alias for `col(&Type::member)`.
template <typename Obj, typename MemberT>
constexpr ColExpr<Obj, MemberT> field(MemberT Obj::* ptr, std::string_view custom_name = {}) noexcept {
    return col(ptr, custom_name);
}

// ---- Comparison Operators on AST Expressions ----
template <typename L, typename R> requires (IsExpr<L> || IsExpr<R>)
auto operator==(L&& l, R&& r) {
    auto wl = wrap_expr(std::forward<L>(l));
    auto wr = wrap_expr(std::forward<R>(r));
    return CmpExpr<CmpOp::Eq, decltype(wl), decltype(wr)>{{}, std::move(wl), std::move(wr)};
}

template <typename L, typename R> requires (IsExpr<L> || IsExpr<R>)
auto operator!=(L&& l, R&& r) {
    auto wl = wrap_expr(std::forward<L>(l));
    auto wr = wrap_expr(std::forward<R>(r));
    return CmpExpr<CmpOp::Ne, decltype(wl), decltype(wr)>{{}, std::move(wl), std::move(wr)};
}

template <typename L, typename R> requires (IsExpr<L> || IsExpr<R>)
auto operator<(L&& l, R&& r) {
    auto wl = wrap_expr(std::forward<L>(l));
    auto wr = wrap_expr(std::forward<R>(r));
    return CmpExpr<CmpOp::Lt, decltype(wl), decltype(wr)>{{}, std::move(wl), std::move(wr)};
}

template <typename L, typename R> requires (IsExpr<L> || IsExpr<R>)
auto operator<=(L&& l, R&& r) {
    auto wl = wrap_expr(std::forward<L>(l));
    auto wr = wrap_expr(std::forward<R>(r));
    return CmpExpr<CmpOp::Le, decltype(wl), decltype(wr)>{{}, std::move(wl), std::move(wr)};
}

template <typename L, typename R> requires (IsExpr<L> || IsExpr<R>)
auto operator>(L&& l, R&& r) {
    auto wl = wrap_expr(std::forward<L>(l));
    auto wr = wrap_expr(std::forward<R>(r));
    return CmpExpr<CmpOp::Gt, decltype(wl), decltype(wr)>{{}, std::move(wl), std::move(wr)};
}

template <typename L, typename R> requires (IsExpr<L> || IsExpr<R>)
auto operator>=(L&& l, R&& r) {
    auto wl = wrap_expr(std::forward<L>(l));
    auto wr = wrap_expr(std::forward<R>(r));
    return CmpExpr<CmpOp::Ge, decltype(wl), decltype(wr)>{{}, std::move(wl), std::move(wr)};
}

// ---- Logical Operators on AST Expressions ----
template <IsExpr L, IsExpr R>
auto operator&&(L&& l, R&& r) {
    return LogicExpr<LogicOp::And, std::decay_t<L>, std::decay_t<R>>{{}, std::forward<L>(l), std::forward<R>(r)};
}

template <IsExpr L, IsExpr R>
auto operator||(L&& l, R&& r) {
    return LogicExpr<LogicOp::Or, std::decay_t<L>, std::decay_t<R>>{{}, std::forward<L>(l), std::forward<R>(r)};
}

template <IsExpr E>
auto operator!(E&& e) {
    return NotExpr<std::decay_t<E>>{{}, std::forward<E>(e)};
}

// ---- Arithmetic Operators on AST Expressions ----
template <typename L, typename R> requires (IsExpr<L> || IsExpr<R>)
auto operator+(L&& l, R&& r) {
    auto wl = wrap_expr(std::forward<L>(l));
    auto wr = wrap_expr(std::forward<R>(r));
    return ArithExpr<ArithOp::Add, decltype(wl), decltype(wr)>{{}, std::move(wl), std::move(wr)};
}

template <typename L, typename R> requires (IsExpr<L> || IsExpr<R>)
auto operator-(L&& l, R&& r) {
    auto wl = wrap_expr(std::forward<L>(l));
    auto wr = wrap_expr(std::forward<R>(r));
    return ArithExpr<ArithOp::Sub, decltype(wl), decltype(wr)>{{}, std::move(wl), std::move(wr)};
}

template <typename L, typename R> requires (IsExpr<L> || IsExpr<R>)
auto operator*(L&& l, R&& r) {
    auto wl = wrap_expr(std::forward<L>(l));
    auto wr = wrap_expr(std::forward<R>(r));
    return ArithExpr<ArithOp::Mul, decltype(wl), decltype(wr)>{{}, std::move(wl), std::move(wr)};
}

template <typename L, typename R> requires (IsExpr<L> || IsExpr<R>)
auto operator/(L&& l, R&& r) {
    auto wl = wrap_expr(std::forward<L>(l));
    auto wr = wrap_expr(std::forward<R>(r));
    return ArithExpr<ArithOp::Div, decltype(wl), decltype(wr)>{{}, std::move(wl), std::move(wr)};
}

// ---- Uniform Evaluator for AST Expressions, Member Pointers, and Callables / GF_FN ----
namespace detail {

template <typename Fn, typename Elem>
constexpr decltype(auto) eval_fn(const Fn& fn, const Elem& elem) {
    if constexpr (IsExpr<Fn>) {
        return fn.eval(elem);
    } else {
        return std::invoke(fn, elem);
    }
}

} // namespace detail

// ============================================================================
// Pagination Result (`Page<T>`) & Grouping Result (`Group<K, V>`)
// ============================================================================

/**
 * @struct Page
 * @brief REST-ready paginated query slice with automatic `nlohmann::json` serialization.
 */
template <typename T>
struct Page {
    std::vector<T> items;
    std::size_t total{0};
    std::size_t page{1};
    std::size_t page_size{20};
    std::size_t total_pages{0};
    bool has_next{false};
    bool has_prev{false};
};

/**
 * @struct Group
 * @brief Grouped bucket returned by `QuerySeq<T>::group_by(key)`.
 */
template <typename K, typename V>
struct Group {
    K key;
    std::vector<V> items;

    [[nodiscard]] std::size_t count() const noexcept { return items.size(); }

    template <typename Selector>
    [[nodiscard]] auto sum(Selector&& sel) const {
        using ValT = std::remove_cvref_t<decltype(detail::eval_fn(sel, items.front()))>;
        ValT total{};
        for (const auto& item : items) {
            total += detail::eval_fn(sel, item);
        }
        return total;
    }

    template <typename Selector>
    [[nodiscard]] std::optional<double> avg(Selector&& sel) const {
        if (items.empty()) return std::nullopt;
        double total = 0.0;
        for (const auto& item : items) {
            total += static_cast<double>(detail::eval_fn(sel, item));
        }
        return total / static_cast<double>(items.size());
    }
};

// ============================================================================
// Fluent Query Sequence (`QuerySeq<T>` & `OrderedQuerySeq<T, Cmp>`)
// ============================================================================

template <typename T, typename Cmp>
class OrderedQuerySeq;

/**
 * @class QuerySeq
 * @brief Fluent C# / EF-style in-memory query builder accepting compile-time AST expressions
 *        (`col(&User::age) >= 18`, `it > 10`), member pointers (`&User::name`), or callables (`GF_FN`).
 */
template <typename T>
class QuerySeq {
protected:
    std::vector<T> data_;

public:
    using value_type = T;

    QuerySeq() = default;
    explicit QuerySeq(std::vector<T> data) : data_(std::move(data)) {}

    // ---- Iterators (allows range-based for loops directly on QuerySeq) ----
    [[nodiscard]] auto begin() noexcept { return data_.begin(); }
    [[nodiscard]] auto end() noexcept { return data_.end(); }
    [[nodiscard]] auto begin() const noexcept { return data_.begin(); }
    [[nodiscard]] auto end() const noexcept { return data_.end(); }

    // ---- Filtering ----
    template <typename Pred>
    [[nodiscard]] QuerySeq<T> where(Pred&& pred) const {
        std::vector<T> out;
        out.reserve(data_.size());
        for (const auto& item : data_) {
            if (static_cast<bool>(detail::eval_fn(pred, item))) {
                out.push_back(item);
            }
        }
        return QuerySeq<T>(std::move(out));
    }

    // ---- Projection ----
    template <typename Selector>
    [[nodiscard]] auto select(Selector&& sel) const {
        using OutT = std::remove_cvref_t<decltype(detail::eval_fn(sel, std::declval<const T&>()))>;
        std::vector<OutT> out;
        out.reserve(data_.size());
        for (const auto& item : data_) {
            out.push_back(detail::eval_fn(sel, item));
        }
        return QuerySeq<OutT>(std::move(out));
    }

    template <typename Selector>
    [[nodiscard]] auto select_many(Selector&& sel) const {
        using RangeT = std::remove_cvref_t<decltype(detail::eval_fn(sel, std::declval<const T&>()))>;
        using ElemT = std::ranges::range_value_t<RangeT>;
        std::vector<ElemT> out;
        for (const auto& item : data_) {
            const auto& sub = detail::eval_fn(sel, item);
            for (const auto& sub_elem : sub) {
                out.push_back(sub_elem);
            }
        }
        return QuerySeq<ElemT>(std::move(out));
    }

    // ---- Ordering (supports chained .then_by() / .then_by_desc()) ----
    template <typename KeySelector>
    [[nodiscard]] auto order_by(KeySelector&& key) const;

    template <typename KeySelector>
    [[nodiscard]] auto order_by_desc(KeySelector&& key) const;

    // ---- Slicing & Reordering ----
    [[nodiscard]] QuerySeq<T> skip(std::size_t n) const {
        if (n >= data_.size()) return QuerySeq<T>{};
        return QuerySeq<T>(std::vector<T>(data_.begin() + static_cast<std::ptrdiff_t>(n), data_.end()));
    }

    [[nodiscard]] QuerySeq<T> take(std::size_t n) const {
        std::size_t count = std::min(n, data_.size());
        return QuerySeq<T>(std::vector<T>(data_.begin(), data_.begin() + static_cast<std::ptrdiff_t>(count)));
    }

    [[nodiscard]] QuerySeq<T> slice(std::size_t offset, std::size_t count) const {
        return skip(offset).take(count);
    }

    [[nodiscard]] QuerySeq<T> reverse() const {
        std::vector<T> out = data_;
        std::reverse(out.begin(), out.end());
        return QuerySeq<T>(std::move(out));
    }

    // ---- Deduplication ----
    [[nodiscard]] QuerySeq<T> distinct() const {
        std::vector<T> out;
        out.reserve(data_.size());
        for (const auto& item : data_) {
            if (std::find(out.begin(), out.end(), item) == out.end()) {
                out.push_back(item);
            }
        }
        return QuerySeq<T>(std::move(out));
    }

    template <typename KeySelector>
    [[nodiscard]] QuerySeq<T> distinct_by(KeySelector&& key) const {
        using KeyT = std::remove_cvref_t<decltype(detail::eval_fn(key, std::declval<const T&>()))>;
        std::vector<T> out;
        out.reserve(data_.size());
        std::vector<KeyT> seen;
        seen.reserve(data_.size());
        for (const auto& item : data_) {
            KeyT k = detail::eval_fn(key, item);
            if (std::find(seen.begin(), seen.end(), k) == seen.end()) {
                seen.push_back(std::move(k));
                out.push_back(item);
            }
        }
        return QuerySeq<T>(std::move(out));
    }

    // ---- Grouping (preserves insertion order of groups) ----
    template <typename KeySelector>
    [[nodiscard]] auto group_by(KeySelector&& key) const {
        using KeyT = std::remove_cvref_t<decltype(detail::eval_fn(key, std::declval<const T&>()))>;
        std::vector<Group<KeyT, T>> groups;
        for (const auto& item : data_) {
            KeyT k = detail::eval_fn(key, item);
            auto it_g = std::find_if(groups.begin(), groups.end(), [&](const auto& g) { return g.key == k; });
            if (it_g == groups.end()) {
                groups.push_back(Group<KeyT, T>{std::move(k), {item}});
            } else {
                it_g->items.push_back(item);
            }
        }
        return QuerySeq<Group<KeyT, T>>(std::move(groups));
    }

    // ---- Inner Join ----
    template <std::ranges::input_range InnerRange, typename OuterKey, typename InnerKey, typename Combiner>
    [[nodiscard]] auto join(const InnerRange& inner,
                            OuterKey&& outer_key,
                            InnerKey&& inner_key,
                            Combiner&& combiner) const {
        using InnerT = std::ranges::range_value_t<InnerRange>;
        using OutT = std::remove_cvref_t<std::invoke_result_t<Combiner, const T&, const InnerT&>>;
        std::vector<OutT> out;
        for (const auto& o : data_) {
            auto ok = detail::eval_fn(outer_key, o);
            for (const auto& in : inner) {
                if (ok == detail::eval_fn(inner_key, in)) {
                    out.push_back(std::invoke(combiner, o, in));
                }
            }
        }
        return QuerySeq<OutT>(std::move(out));
    }

    // ---- Concatenation ----
    template <std::ranges::input_range OtherRange>
    [[nodiscard]] QuerySeq<T> concat(const OtherRange& other) const {
        std::vector<T> out = data_;
        for (const auto& item : other) {
            out.push_back(item);
        }
        return QuerySeq<T>(std::move(out));
    }

    // ---- Terminal / Materialization Operations ----
    [[nodiscard]] std::vector<T> to_vec() const & { return data_; }
    [[nodiscard]] std::vector<T> to_vec() && noexcept { return std::move(data_); }

    template <typename KeySelector>
    [[nodiscard]] auto to_map(KeySelector&& key) const {
        using KeyT = std::remove_cvref_t<decltype(detail::eval_fn(key, std::declval<const T&>()))>;
        std::unordered_map<KeyT, T> map;
        map.reserve(data_.size());
        for (const auto& item : data_) {
            map.insert_or_assign(detail::eval_fn(key, item), item);
        }
        return map;
    }

    /// Returns a 1-based paginated slice (`Page<T>`), directly serializable to JSON by `goforge`.
    [[nodiscard]] Page<T> paginate(std::size_t page, std::size_t page_size) const {
        if (page == 0) page = 1;
        if (page_size == 0) page_size = 20;

        Page<T> p;
        p.total = data_.size();
        p.page = page;
        p.page_size = page_size;
        p.total_pages = (p.total == 0) ? 0 : ((p.total + page_size - 1) / page_size);
        p.has_prev = page > 1 && p.total > 0;
        p.has_next = page < p.total_pages;

        const std::size_t offset = (page - 1) * page_size;
        if (offset < data_.size()) {
            const std::size_t cnt = std::min(page_size, data_.size() - offset);
            p.items.assign(
                data_.begin() + static_cast<std::ptrdiff_t>(offset),
                data_.begin() + static_cast<std::ptrdiff_t>(offset + cnt)
            );
        }
        return p;
    }

    [[nodiscard]] std::optional<T> first() const {
        if (data_.empty()) return std::nullopt;
        return data_.front();
    }

    template <typename Pred>
    [[nodiscard]] std::optional<T> first(Pred&& pred) const {
        for (const auto& item : data_) {
            if (static_cast<bool>(detail::eval_fn(pred, item))) {
                return item;
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] T first_or(T fallback) const {
        return data_.empty() ? std::move(fallback) : data_.front();
    }

    [[nodiscard]] std::optional<T> last() const {
        if (data_.empty()) return std::nullopt;
        return data_.back();
    }

    [[nodiscard]] std::optional<T> single() const {
        if (data_.empty()) return std::nullopt;
        if (data_.size() > 1) {
            throw std::runtime_error("QuerySeq::single() found more than one element.");
        }
        return data_.front();
    }

    [[nodiscard]] std::size_t count() const noexcept { return data_.size(); }

    template <typename Pred>
    [[nodiscard]] std::size_t count(Pred&& pred) const {
        std::size_t c = 0;
        for (const auto& item : data_) {
            if (static_cast<bool>(detail::eval_fn(pred, item))) ++c;
        }
        return c;
    }

    [[nodiscard]] bool empty() const noexcept { return data_.empty(); }
    [[nodiscard]] bool any() const noexcept { return !data_.empty(); }

    template <typename Pred>
    [[nodiscard]] bool any(Pred&& pred) const {
        for (const auto& item : data_) {
            if (static_cast<bool>(detail::eval_fn(pred, item))) return true;
        }
        return false;
    }

    template <typename Pred>
    [[nodiscard]] bool all(Pred&& pred) const {
        for (const auto& item : data_) {
            if (!static_cast<bool>(detail::eval_fn(pred, item))) return false;
        }
        return true;
    }

    template <typename Pred>
    [[nodiscard]] bool none(Pred&& pred) const {
        return !any(std::forward<Pred>(pred));
    }

    // ---- Aggregations ----
    [[nodiscard]] T sum() const requires std::is_arithmetic_v<T> {
        T total{};
        for (const auto& item : data_) total += item;
        return total;
    }

    template <typename Selector>
    [[nodiscard]] auto sum(Selector&& sel) const {
        using ValT = std::remove_cvref_t<decltype(detail::eval_fn(sel, std::declval<const T&>()))>;
        ValT total{};
        for (const auto& item : data_) total += detail::eval_fn(sel, item);
        return total;
    }

    [[nodiscard]] std::optional<double> avg() const requires std::is_arithmetic_v<T> {
        if (data_.empty()) return std::nullopt;
        double total = 0.0;
        for (const auto& item : data_) total += static_cast<double>(item);
        return total / static_cast<double>(data_.size());
    }

    template <typename Selector>
    [[nodiscard]] std::optional<double> avg(Selector&& sel) const {
        if (data_.empty()) return std::nullopt;
        double total = 0.0;
        for (const auto& item : data_) total += static_cast<double>(detail::eval_fn(sel, item));
        return total / static_cast<double>(data_.size());
    }

    [[nodiscard]] std::optional<T> min() const {
        if (data_.empty()) return std::nullopt;
        return *std::min_element(data_.begin(), data_.end());
    }

    [[nodiscard]] std::optional<T> max() const {
        if (data_.empty()) return std::nullopt;
        return *std::max_element(data_.begin(), data_.end());
    }

    template <typename Selector>
    [[nodiscard]] std::optional<T> min_by(Selector&& sel) const {
        if (data_.empty()) return std::nullopt;
        return *std::min_element(data_.begin(), data_.end(), [&](const T& a, const T& b) {
            return detail::eval_fn(sel, a) < detail::eval_fn(sel, b);
        });
    }

    template <typename Selector>
    [[nodiscard]] std::optional<T> max_by(Selector&& sel) const {
        if (data_.empty()) return std::nullopt;
        return *std::max_element(data_.begin(), data_.end(), [&](const T& a, const T& b) {
            return detail::eval_fn(sel, a) < detail::eval_fn(sel, b);
        });
    }

    template <typename Acc, typename Reducer>
    [[nodiscard]] Acc aggregate(Acc init, Reducer&& fn) const {
        for (const auto& item : data_) {
            init = std::invoke(fn, std::move(init), item);
        }
        return init;
    }

    template <typename Action>
    void for_each(Action&& fn) const {
        for (const auto& item : data_) {
            std::invoke(fn, item);
        }
    }
};

/**
 * @class OrderedQuerySeq
 * @brief Ordered fluent sequence returned by `order_by` / `order_by_desc` supporting `.then_by()` and `.then_by_desc()`.
 */
template <typename T, typename Cmp>
class OrderedQuerySeq : public QuerySeq<T> {
    Cmp cmp_;

public:
    OrderedQuerySeq(std::vector<T> data, Cmp cmp)
        : QuerySeq<T>(std::move(data)), cmp_(std::move(cmp)) {
        std::stable_sort(this->data_.begin(), this->data_.end(), cmp_);
    }

    template <typename KeySelector>
    [[nodiscard]] auto then_by(KeySelector&& key) const {
        auto next_cmp = [prev = cmp_, k = std::forward<KeySelector>(key)](const T& a, const T& b) {
            if (prev(a, b)) return true;
            if (prev(b, a)) return false;
            return detail::eval_fn(k, a) < detail::eval_fn(k, b);
        };
        return OrderedQuerySeq<T, decltype(next_cmp)>(this->data_, std::move(next_cmp));
    }

    template <typename KeySelector>
    [[nodiscard]] auto then_by_desc(KeySelector&& key) const {
        auto next_cmp = [prev = cmp_, k = std::forward<KeySelector>(key)](const T& a, const T& b) {
            if (prev(a, b)) return true;
            if (prev(b, a)) return false;
            return detail::eval_fn(k, b) < detail::eval_fn(k, a);
        };
        return OrderedQuerySeq<T, decltype(next_cmp)>(this->data_, std::move(next_cmp));
    }
};

template <typename T>
template <typename KeySelector>
auto QuerySeq<T>::order_by(KeySelector&& key) const {
    auto cmp = [k = std::forward<KeySelector>(key)](const T& a, const T& b) {
        return detail::eval_fn(k, a) < detail::eval_fn(k, b);
    };
    return OrderedQuerySeq<T, decltype(cmp)>(data_, std::move(cmp));
}

template <typename T>
template <typename KeySelector>
auto QuerySeq<T>::order_by_desc(KeySelector&& key) const {
    auto cmp = [k = std::forward<KeySelector>(key)](const T& a, const T& b) {
        return detail::eval_fn(k, b) < detail::eval_fn(k, a);
    };
    return OrderedQuerySeq<T, decltype(cmp)>(data_, std::move(cmp));
}

// ============================================================================
// Entry Points (`from(range)`, `from({...})`, `from_values(map)`)
// ============================================================================

/// Creates a fluent `QuerySeq<T>` from any C++20 range (`std::vector`, `std::array`, `std::list`, `std::span`, ...).
template <std::ranges::input_range Range>
[[nodiscard]] auto from(const Range& range) {
    using ElemT = std::ranges::range_value_t<Range>;
    std::vector<ElemT> items;
    if constexpr (std::ranges::sized_range<Range>) {
        items.reserve(std::ranges::size(range));
    }
    for (const auto& elem : range) {
        items.push_back(elem);
    }
    return QuerySeq<ElemT>(std::move(items));
}

/// Creates a fluent `QuerySeq<T>` from an initializer list (`from({1, 2, 3, 4})`).
template <typename T>
[[nodiscard]] QuerySeq<T> from(std::initializer_list<T> init) {
    return QuerySeq<T>(std::vector<T>(init));
}

/// Creates a fluent `QuerySeq<ValueT>` from the values of an associative map (`std::unordered_map`, `std::map`).
template <typename Map>
[[nodiscard]] auto from_values(const Map& map) {
    using ValueT = typename Map::mapped_type;
    std::vector<ValueT> items;
    items.reserve(map.size());
    for (const auto& [k, v] : map) {
        items.push_back(v);
    }
    return QuerySeq<ValueT>(std::move(items));
}

} // namespace goforge::query

// Automatic JSON serialization for `goforge::query::Page<T>`!
namespace nlohmann {

template <typename T>
struct adl_serializer<goforge::query::Page<T>> {
    static void to_json(json& j, const goforge::query::Page<T>& p) {
        j = json{
            {"items", p.items},
            {"total", p.total},
            {"page", p.page},
            {"page_size", p.page_size},
            {"total_pages", p.total_pages},
            {"has_next", p.has_next},
            {"has_prev", p.has_prev}
        };
    }

    static void from_json(const json& j, goforge::query::Page<T>& p) {
        j.at("items").get_to(p.items);
        j.at("total").get_to(p.total);
        j.at("page").get_to(p.page);
        j.at("page_size").get_to(p.page_size);
        j.at("total_pages").get_to(p.total_pages);
        j.at("has_next").get_to(p.has_next);
        j.at("has_prev").get_to(p.has_prev);
    }
};

} // namespace nlohmann

/**
 * @brief Declares a unified `goforge` Model/DTO with:
 *        1. JSON serialization & default-value deserialization (`GOFORGE_JSON_DTO_DEFAULT`), and
 *        2. Compile-time `goforge::query::EntityMeta<Type>` reflection (table name + column names)
 *           so `col(&Type::field)` automatically carries SQL column metadata for future ORM support.
 */
#define GOFORGE_MODEL_TABLE(Type, TableNameLiteral, ...)                                         \
    GOFORGE_JSON_DTO_DEFAULT(Type, __VA_ARGS__)                                                  \
    namespace goforge::query {                                                                   \
    template <>                                                                                  \
    struct EntityMeta<Type> {                                                                    \
        static constexpr bool registered = true;                                                 \
        static constexpr std::string_view table_name = TableNameLiteral;                         \
        static constexpr auto fields() noexcept {                                                \
            return std::make_tuple(GOFORGE_PP_FOR_EACH(GOFORGE_MODEL_FIELD_PAIR, Type, __VA_ARGS__)); \
        }                                                                                        \
    };                                                                                           \
    }

/// Declares a unified `goforge` Model/DTO using `#Type` as the default table name.
#define GOFORGE_MODEL(Type, ...) GOFORGE_MODEL_TABLE(Type, #Type, __VA_ARGS__)
// ---- end goforge/query.hpp ----
// ---- begin goforge/di.hpp ----

/**
 * @file di.hpp
 * @brief Compile-time typed Dependency Injection container for C++20/23 (`ServiceCollection`,
 *        `ServiceProvider`, `ServiceScope`) with Singleton, Transient, and Scoped lifetimes,
 *        automatic argument resolution, cycle detection, and captive-dependency validation.
 */

#include <atomic>
#include <concepts>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <typeindex>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "gorun/gorun.hpp"

namespace goforge {

class ServiceProvider;
class ServiceScope;

/**
 * @enum Lifetime
 * @brief Controls the lifetime and sharing semantics of a registered service.
 */
enum class Lifetime : std::uint8_t {
    Singleton, ///< One shared instance per `ServiceProvider` (application lifetime)
    Transient, ///< A fresh instance created on every resolution
    Scoped     ///< One shared instance per `ServiceScope` (e.g., per HTTP request)
};

/// Base exception type for all Dependency Injection container errors.
class DiError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// Thrown when a requested or declared service dependency is not registered.
class ServiceNotFoundError : public DiError {
public:
    using DiError::DiError;
};

/// Thrown during `ServiceCollection::build()` when a circular dependency graph is detected.
class CircularDependencyError : public DiError {
public:
    using DiError::DiError;
};

/// Thrown during `ServiceCollection::build()` when a `Singleton` depends on a `Scoped` service.
class CaptiveDependencyError : public DiError {
public:
    using DiError::DiError;
};

/// Thrown when resolving a `Scoped` service directly from the root `ServiceProvider` without a `ServiceScope`.
class ScopeRequiredError : public DiError {
public:
    using DiError::DiError;
};

namespace detail {

/**
 * @brief Extracts a readable compile-time type name without requiring ABI demangling.
 */
template <typename T>
inline std::string type_name() {
#if defined(__clang__) || defined(__GNUC__)
    std::string_view p = __PRETTY_FUNCTION__;
    auto start = p.find("T = ");
    if (start != std::string_view::npos) {
        start += 4;
        auto end = p.find_first_of(";]", start);
        if (end != std::string_view::npos) {
            return std::string(p.substr(start, end - start));
        }
    }
    return std::string(p);
#elif defined(_MSC_VER)
    std::string_view p = __FUNCSIG__;
    auto start = p.find("type_name<");
    if (start != std::string_view::npos) {
        start += 10;
        auto end = p.rfind(">(void)");
        if (end != std::string_view::npos && end > start) {
            return std::string(p.substr(start, end - start));
        }
    }
    return std::string(p);
#else
    return typeid(T).name();
#endif
}

template <typename T>
struct IsSharedPtr : std::false_type {};

template <typename T>
struct IsSharedPtr<std::shared_ptr<T>> : std::true_type {
    using element_type = T;
};

template <typename T>
struct IsUniquePtr : std::false_type {};

template <typename T>
struct IsUniquePtr<std::unique_ptr<T>> : std::true_type {
    using element_type = T;
};

/// Extracts function signature traits for free functions, function pointers, and lambdas (const or mutable).
template <typename T>
struct FunctionTraits : FunctionTraits<decltype(&std::decay_t<T>::operator())> {};

template <typename R, typename... Args>
struct FunctionTraits<R (*)(Args...)> {
    using return_type = R;
    using args_tuple = std::tuple<Args...>;
};

template <typename R, typename... Args>
struct FunctionTraits<R(Args...)> {
    using return_type = R;
    using args_tuple = std::tuple<Args...>;
};

template <typename C, typename R, typename... Args>
struct FunctionTraits<R (C::*)(Args...) const> {
    using class_type = C;
    using return_type = R;
    using args_tuple = std::tuple<Args...>;
};

template <typename C, typename R, typename... Args>
struct FunctionTraits<R (C::*)(Args...)> {
    using class_type = C;
    using return_type = R;
    using args_tuple = std::tuple<Args...>;
};

template <typename C, typename R, typename... Args>
struct FunctionTraits<R (C::*)(Args...) const noexcept> {
    using class_type = C;
    using return_type = R;
    using args_tuple = std::tuple<Args...>;
};

template <typename C, typename R, typename... Args>
struct FunctionTraits<R (C::*)(Args...) noexcept> {
    using class_type = C;
    using return_type = R;
    using args_tuple = std::tuple<Args...>;
};

/// Deduces the registered service type from a factory return type (`T`, `std::shared_ptr<T>`, or `std::unique_ptr<T>`).
template <typename R>
struct UnwrapReturnType {
    using type = std::remove_cvref_t<R>;
};

template <typename T>
struct UnwrapReturnType<std::shared_ptr<T>> {
    using type = std::remove_cv_t<T>;
};

template <typename T>
struct UnwrapReturnType<std::unique_ptr<T>> {
    using type = std::remove_cv_t<T>;
};

/// Normalizes a factory return value into `std::shared_ptr<ServiceType>`.
template <typename ServiceType, typename R>
std::shared_ptr<ServiceType> normalize_service_ptr(R&& value) {
    using DecayedR = std::decay_t<R>;
    if constexpr (IsSharedPtr<DecayedR>::value) {
        if (!value) {
            throw DiError("Factory returned nullptr for service: " + type_name<ServiceType>());
        }
        return std::static_pointer_cast<ServiceType>(std::forward<R>(value));
    } else if constexpr (IsUniquePtr<DecayedR>::value) {
        if (!value) {
            throw DiError("Factory returned nullptr for service: " + type_name<ServiceType>());
        }
        std::shared_ptr<typename DecayedR::element_type> sp(std::move(value));
        return std::static_pointer_cast<ServiceType>(std::move(sp));
    } else {
        return std::static_pointer_cast<ServiceType>(
            std::make_shared<DecayedR>(std::forward<R>(value))
        );
    }
}

/// Identifies built-in container self-parameters (`ServiceProvider&`, `ServiceScope&`) excluded from graph deps.
template <typename Param>
constexpr bool is_container_param_v =
    std::is_same_v<std::remove_cvref_t<Param>, ServiceProvider> ||
    std::is_same_v<std::remove_cvref_t<Param>, ServiceScope>;

/// Extracts the underlying service type required for a dependency parameter (`T&`, `const T&`, `std::shared_ptr<T>`).
template <typename CleanParam>
struct UnwrapSharedOrDirect {
    using type = CleanParam;
};

template <typename T>
struct UnwrapSharedOrDirect<std::shared_ptr<T>> {
    using type = std::remove_cv_t<T>;
};

template <typename Param>
struct DependencyServiceType {
    using type = typename UnwrapSharedOrDirect<std::remove_cvref_t<Param>>::type;
};

struct DependencyInfo {
    std::type_index key;
    std::string name;
};

template <typename Param>
void append_dependency_if_service(std::vector<DependencyInfo>& out) {
    if constexpr (!is_container_param_v<Param>) {
        using ServiceT = typename DependencyServiceType<Param>::type;
        out.push_back(DependencyInfo{std::type_index(typeid(ServiceT)), type_name<ServiceT>()});
    }
}

template <typename Tuple, std::size_t... Is>
std::vector<DependencyInfo> collect_dependencies_impl(std::index_sequence<Is...>) {
    std::vector<DependencyInfo> deps;
    deps.reserve(sizeof...(Is));
    (append_dependency_if_service<std::tuple_element_t<Is, Tuple>>(deps), ...);
    return deps;
}

template <typename Tuple>
std::vector<DependencyInfo> collect_dependencies() {
    return collect_dependencies_impl<Tuple>(std::make_index_sequence<std::tuple_size_v<Tuple>>{});
}

/// Per-service lazy initialization slot using `gorun::Mutex` (never blocks OS worker threads!).
struct InstanceSlot {
    gorun::Mutex init_mtx;
    std::atomic<bool> ready{false};
    std::shared_ptr<void> instance{};
};

struct ServiceDescriptor {
    std::type_index key{typeid(void)};
    std::string name;
    Lifetime lifetime{Lifetime::Singleton};
    std::vector<DependencyInfo> dependencies;
    std::function<std::shared_ptr<void>(ServiceProvider&, ServiceScope*)> factory;
    std::shared_ptr<InstanceSlot> singleton_slot; ///< Pre-allocated for Singletons so resolution is lock-free on lookup
};

struct ProviderState {
    std::unordered_map<std::type_index, ServiceDescriptor> descriptors;
};

} // namespace detail

/**
 * @class ServiceScope
 * @brief Represents an isolated DI scope (e.g., a single HTTP request or background job).
 *
 * - `Scoped` services are constructed once per `ServiceScope` and destroyed when the scope is destroyed.
 * - `Singleton` services are delegated to the root `ServiceProvider`.
 * - `Transient` services are created fresh on every resolution.
 */
class ServiceScope {
    friend class ServiceProvider;

    ServiceProvider* root_;
    gorun::Mutex slots_mtx_;
    std::unordered_map<std::type_index, std::shared_ptr<detail::InstanceSlot>> scoped_slots_;

    std::shared_ptr<detail::InstanceSlot> get_or_create_slot(std::type_index key) {
        std::lock_guard<gorun::Mutex> lk(slots_mtx_);
        auto& slot = scoped_slots_[key];
        if (!slot) {
            slot = std::make_shared<detail::InstanceSlot>();
        }
        return slot;
    }

public:
    explicit ServiceScope(ServiceProvider& root) noexcept : root_(&root) {}

    ServiceScope(const ServiceScope&) = delete;
    ServiceScope& operator=(const ServiceScope&) = delete;
    ServiceScope(ServiceScope&&) = delete;
    ServiceScope& operator=(ServiceScope&&) = delete;

    /// Returns the root `ServiceProvider` owning this scope.
    ServiceProvider& root() noexcept { return *root_; }

    /// Resolves service `T` as a `std::shared_ptr<T>` within this scope.
    template <typename T>
    std::shared_ptr<std::remove_cv_t<T>> resolve_ptr();

    /// Resolves service `T` as a reference `T&` within this scope.
    template <typename T>
    T& resolve();

    /// Invokes a callable `fn(Deps...)`, automatically resolving all arguments within this scope.
    template <typename Fn>
    decltype(auto) invoke(Fn&& fn);
};

/**
 * @class ServiceProvider
 * @brief Immutable, concurrency-safe Dependency Injection resolver built from `ServiceCollection::build()`.
 *
 * Uses `gorun::Mutex` for lazy Singleton initialization so green routines contending on first-time
 * service construction park cleanly without blocking OS worker threads. After initialization,
 * Singleton resolution is lock-free (single atomic acquire load).
 */
class ServiceProvider {
    friend class ServiceCollection;
    friend class ServiceScope;

    std::shared_ptr<const detail::ProviderState> state_;

    explicit ServiceProvider(std::shared_ptr<const detail::ProviderState> state) noexcept
        : state_(std::move(state)) {}

    std::shared_ptr<void> resolve_erased(std::type_index key, std::string_view requested_name, ServiceScope* scope) {
        auto it = state_->descriptors.find(key);
        if (it == state_->descriptors.end()) {
            throw ServiceNotFoundError("Service not registered: " + std::string(requested_name));
        }

        const detail::ServiceDescriptor& desc = it->second;

        if (desc.lifetime == Lifetime::Transient) {
            return desc.factory(*this, scope);
        }

        if (desc.lifetime == Lifetime::Singleton) {
            auto& slot = *desc.singleton_slot;
            if (slot.ready.load(std::memory_order_acquire)) {
                return slot.instance;
            }
            std::lock_guard<gorun::Mutex> lk(slot.init_mtx);
            if (!slot.ready.load(std::memory_order_relaxed)) {
                slot.instance = desc.factory(*this, nullptr);
                slot.ready.store(true, std::memory_order_release);
            }
            return slot.instance;
        }

        // Lifetime::Scoped
        if (!scope) {
            throw ScopeRequiredError(
                "Cannot resolve Scoped service '" + desc.name +
                "' from root ServiceProvider without an active ServiceScope (call create_scope())."
            );
        }

        auto slot = scope->get_or_create_slot(key);
        if (slot->ready.load(std::memory_order_acquire)) {
            return slot->instance;
        }
        std::lock_guard<gorun::Mutex> lk(slot->init_mtx);
        if (!slot->ready.load(std::memory_order_relaxed)) {
            slot->instance = desc.factory(*this, scope);
            slot->ready.store(true, std::memory_order_release);
        }
        return slot->instance;
    }

    template <typename Param>
    decltype(auto) resolve_param(ServiceScope* scope) {
        using Clean = std::remove_cvref_t<Param>;
        if constexpr (std::is_same_v<Clean, ServiceProvider>) {
            return static_cast<ServiceProvider&>(*this);
        } else if constexpr (std::is_same_v<Clean, ServiceScope>) {
            if (!scope) {
                throw ScopeRequiredError("Cannot inject ServiceScope& outside of an active ServiceScope.");
            }
            return static_cast<ServiceScope&>(*scope);
        } else if constexpr (detail::IsSharedPtr<Clean>::value) {
            using Elem = typename detail::IsSharedPtr<Clean>::element_type;
            using RawT = std::remove_cv_t<Elem>;
            auto ptr = std::static_pointer_cast<RawT>(
                resolve_erased(std::type_index(typeid(RawT)), detail::type_name<RawT>(), scope)
            );
            return std::static_pointer_cast<Elem>(std::move(ptr));
        } else {
            static_assert(
                std::is_reference_v<Param> || std::is_copy_constructible_v<Clean>,
                "Injected non-reference parameter must be copy-constructible, or take T& / const T& / std::shared_ptr<T>."
            );
            auto ptr = std::static_pointer_cast<Clean>(
                resolve_erased(std::type_index(typeid(Clean)), detail::type_name<Clean>(), scope)
            );
            if constexpr (std::is_reference_v<Param>) {
                return static_cast<Param>(*ptr);
            } else {
                return *ptr;
            }
        }
    }

    template <typename Fn, typename ArgsTuple, std::size_t... Is>
    decltype(auto) invoke_impl(Fn&& fn, ServiceScope* scope, std::index_sequence<Is...>) {
        (void)scope;
        return std::invoke(
            std::forward<Fn>(fn),
            resolve_param<std::tuple_element_t<Is, ArgsTuple>>(scope)...
        );
    }

public:
    ServiceProvider() = default;

    /// Returns true if a service of type `T` is registered in this container.
    template <typename T>
    [[nodiscard]] bool contains() const noexcept {
        using CleanT = std::remove_cvref_t<T>;
        return state_ && state_->descriptors.contains(std::type_index(typeid(CleanT)));
    }

    /// Resolves service `T` as a `std::shared_ptr<T>`.
    template <typename T>
    std::shared_ptr<std::remove_cv_t<T>> resolve_ptr() {
        using RawT = std::remove_cv_t<T>;
        return std::static_pointer_cast<RawT>(
            resolve_erased(std::type_index(typeid(RawT)), detail::type_name<RawT>(), nullptr)
        );
    }

    /// Resolves service `T` as a reference `T&`.
    template <typename T>
    T& resolve() {
        return *resolve_ptr<std::remove_cvref_t<T>>();
    }

    /// Creates a new `ServiceScope` for per-request / per-job Scoped services.
    [[nodiscard]] ServiceScope create_scope() noexcept {
        return ServiceScope(*this);
    }

    /// Invokes `fn(Deps...)` by automatically resolving all its parameters from the root provider.
    template <typename Fn>
    decltype(auto) invoke(Fn&& fn) {
        using Traits = detail::FunctionTraits<std::decay_t<Fn>>;
        using ArgsTuple = typename Traits::args_tuple;
        return invoke_impl<Fn, ArgsTuple>(
            std::forward<Fn>(fn),
            nullptr,
            std::make_index_sequence<std::tuple_size_v<ArgsTuple>>{}
        );
    }

    /// Returns total number of registered service types.
    [[nodiscard]] std::size_t service_count() const noexcept {
        return state_ ? state_->descriptors.size() : 0;
    }
};

template <typename T>
inline std::shared_ptr<std::remove_cv_t<T>> ServiceScope::resolve_ptr() {
    using RawT = std::remove_cv_t<T>;
    return std::static_pointer_cast<RawT>(
        root_->resolve_erased(std::type_index(typeid(RawT)), detail::type_name<RawT>(), this)
    );
}

template <typename T>
inline T& ServiceScope::resolve() {
    return *resolve_ptr<std::remove_cvref_t<T>>();
}

template <typename Fn>
inline decltype(auto) ServiceScope::invoke(Fn&& fn) {
    using Traits = detail::FunctionTraits<std::decay_t<Fn>>;
    using ArgsTuple = typename Traits::args_tuple;
    return root_->template invoke_impl<Fn, ArgsTuple>(
        std::forward<Fn>(fn),
        this,
        std::make_index_sequence<std::tuple_size_v<ArgsTuple>>{}
    );
}

/**
 * @class ServiceCollection
 * @brief Builder for registering services, factories, instances, and interface bindings before `build()`.
 *
 * `build()` validates the complete dependency graph up-front:
 * - Missing dependencies (`ServiceNotFoundError`)
 * - Circular dependencies (`CircularDependencyError`, reporting the full cycle chain)
 * - Captive dependencies where a `Singleton` depends on a `Scoped` service (`CaptiveDependencyError`)
 */
class ServiceCollection {
    std::unordered_map<std::type_index, detail::ServiceDescriptor> descriptors_;

    template <typename ServiceType, typename Factory>
    ServiceCollection& register_factory(Lifetime lifetime, Factory&& factory) {
        using CleanService = std::remove_cv_t<ServiceType>;
        using DecayedFactory = std::decay_t<Factory>;
        using Traits = detail::FunctionTraits<DecayedFactory>;
        using ArgsTuple = typename Traits::args_tuple;

        detail::ServiceDescriptor desc;
        desc.key = std::type_index(typeid(CleanService));
        desc.name = detail::type_name<CleanService>();
        desc.lifetime = lifetime;
        desc.dependencies = detail::collect_dependencies<ArgsTuple>();
        if (lifetime == Lifetime::Singleton) {
            desc.singleton_slot = std::make_shared<detail::InstanceSlot>();
        }

        auto fn_holder = std::make_shared<DecayedFactory>(std::forward<Factory>(factory));
        desc.factory = [fn_holder](ServiceProvider& provider, ServiceScope* scope) -> std::shared_ptr<void> {
            auto result = provider.template invoke_impl<DecayedFactory&, ArgsTuple>(
                *fn_holder,
                scope,
                std::make_index_sequence<std::tuple_size_v<ArgsTuple>>{}
            );
            std::shared_ptr<CleanService> typed =
                detail::normalize_service_ptr<CleanService>(std::move(result));
            return std::static_pointer_cast<void>(std::move(typed));
        };

        descriptors_.insert_or_assign(desc.key, std::move(desc));
        return *this;
    }

    template <typename D>
    using CtorDepParam = std::conditional_t<
        std::is_reference_v<D> || detail::IsSharedPtr<std::remove_cvref_t<D>>::value,
        D,
        D&
    >;

    template <typename ServiceType, typename ImplType, typename... Deps>
    ServiceCollection& register_ctor(Lifetime lifetime) {
        static_assert(
            std::is_same_v<ServiceType, ImplType> || std::is_base_of_v<ServiceType, ImplType>,
            "ImplType must derive from ServiceType."
        );
        return register_factory<ServiceType>(
            lifetime,
            [](CtorDepParam<Deps>... deps) -> std::shared_ptr<ServiceType> {
                return std::static_pointer_cast<ServiceType>(
                    std::make_shared<ImplType>(std::forward<CtorDepParam<Deps>>(deps)...)
                );
            }
        );
    }

    static void validate_graph(const std::unordered_map<std::type_index, detail::ServiceDescriptor>& descs) {
        // 1. Verify all declared dependencies exist
        for (const auto& [key, desc] : descs) {
            for (const auto& dep : desc.dependencies) {
                if (!descs.contains(dep.key)) {
                    throw ServiceNotFoundError(
                        "Missing dependency: service '" + desc.name +
                        "' requires unregistered service '" + dep.name + "'."
                    );
                }
            }
        }

        // 2. 3-color DFS cycle detection (0 = unvisited, 1 = visiting, 2 = visited)
        std::unordered_map<std::type_index, int> color;
        std::vector<std::type_index> path;

        auto dfs_cycle = [&](auto&& self, std::type_index node) -> void {
            color[node] = 1;
            path.push_back(node);

            const auto& desc = descs.at(node);
            for (const auto& dep : desc.dependencies) {
                int c = color[dep.key];
                if (c == 1) {
                    std::ostringstream oss;
                    oss << "Circular dependency detected: ";
                    bool in_cycle = false;
                    for (const auto& p : path) {
                        if (p == dep.key) in_cycle = true;
                        if (in_cycle) oss << descs.at(p).name << " -> ";
                    }
                    oss << descs.at(dep.key).name;
                    throw CircularDependencyError(oss.str());
                }
                if (c == 0) {
                    self(self, dep.key);
                }
            }

            path.pop_back();
            color[node] = 2;
        };

        for (const auto& [key, _] : descs) {
            if (color[key] == 0) {
                dfs_cycle(dfs_cycle, key);
            }
        }

        // 3. Captive dependency validation: no Singleton may depend (directly or transitively) on a Scoped service
        for (const auto& [root_key, root_desc] : descs) {
            if (root_desc.lifetime != Lifetime::Singleton) continue;

            std::unordered_set<std::type_index> visited;
            std::vector<std::type_index> stack;
            stack.push_back(root_key);

            while (!stack.empty()) {
                std::type_index cur = stack.back();
                stack.pop_back();
                if (!visited.insert(cur).second) continue;

                for (const auto& dep : descs.at(cur).dependencies) {
                    const auto& dep_desc = descs.at(dep.key);
                    if (dep_desc.lifetime == Lifetime::Scoped) {
                        throw CaptiveDependencyError(
                            "Captive dependency detected: Singleton '" + root_desc.name +
                            "' cannot depend on Scoped service '" + dep_desc.name + "'."
                        );
                    }
                    stack.push_back(dep.key);
                }
            }
        }
    }

public:
    ServiceCollection() = default;

    /// Returns true if a descriptor for `T` has already been registered.
    template <typename T>
    [[nodiscard]] bool has() const noexcept {
        using CleanT = std::remove_cv_t<T>;
        return descriptors_.contains(std::type_index(typeid(CleanT)));
    }

    /// Registers a pre-built instance `std::shared_ptr<T>` as a `Singleton`.
    template <typename T>
    ServiceCollection& add_instance(std::shared_ptr<T> instance) {
        using CleanT = std::remove_cv_t<T>;
        if (!instance) {
            throw DiError("Cannot register nullptr instance for service: " + detail::type_name<CleanT>());
        }
        detail::ServiceDescriptor desc;
        desc.key = std::type_index(typeid(CleanT));
        desc.name = detail::type_name<CleanT>();
        desc.lifetime = Lifetime::Singleton;
        desc.singleton_slot = std::make_shared<detail::InstanceSlot>();
        desc.singleton_slot->instance = std::static_pointer_cast<void>(std::move(instance));
        desc.singleton_slot->ready.store(true, std::memory_order_release);
        desc.factory = [slot = desc.singleton_slot](ServiceProvider&, ServiceScope*) {
            return slot->instance;
        };
        descriptors_.insert_or_assign(desc.key, std::move(desc));
        return *this;
    }

    /// Registers a value by copying/moving it into a `std::shared_ptr<T>` `Singleton`.
    template <typename T>
        requires (!detail::IsSharedPtr<std::decay_t<T>>::value)
    ServiceCollection& add_instance(T&& value) {
        using CleanT = std::decay_t<T>;
        return add_instance<CleanT>(std::make_shared<CleanT>(std::forward<T>(value)));
    }

    /// Registers a factory `R(Deps...)` with automatic service type deduction and dependency injection.
    template <typename Factory>
    ServiceCollection& provide(Lifetime lifetime, Factory&& factory) {
        using Traits = detail::FunctionTraits<std::decay_t<Factory>>;
        using ServiceT = typename detail::UnwrapReturnType<typename Traits::return_type>::type;
        return register_factory<ServiceT>(lifetime, std::forward<Factory>(factory));
    }

    /// Registers a `Singleton` factory with automatic service type deduction.
    template <typename Factory>
    ServiceCollection& add_singleton_factory(Factory&& factory) {
        return provide(Lifetime::Singleton, std::forward<Factory>(factory));
    }

    /// Registers a `Transient` factory with automatic service type deduction.
    template <typename Factory>
    ServiceCollection& add_transient_factory(Factory&& factory) {
        return provide(Lifetime::Transient, std::forward<Factory>(factory));
    }

    /// Registers a `Scoped` factory with automatic service type deduction.
    template <typename Factory>
    ServiceCollection& add_scoped_factory(Factory&& factory) {
        return provide(Lifetime::Scoped, std::forward<Factory>(factory));
    }

    /// Registers a factory explicitly bound to `ServiceType` (e.g., an interface).
    template <typename ServiceType, typename Factory>
    ServiceCollection& add_factory(Lifetime lifetime, Factory&& factory) {
        return register_factory<ServiceType>(lifetime, std::forward<Factory>(factory));
    }

    /// Registers a `Singleton` constructed directly with `Impl(Deps...)`.
    template <typename Impl, typename... Deps>
    ServiceCollection& add_singleton() {
        return register_ctor<Impl, Impl, Deps...>(Lifetime::Singleton);
    }

    /// Registers a `Singleton` implementing `Interface` via `Impl(Deps...)`.
    template <typename Interface, typename Impl, typename... Deps>
        requires (!std::is_same_v<Interface, Impl> && std::is_base_of_v<Interface, Impl>)
    ServiceCollection& add_singleton_as() {
        return register_ctor<Interface, Impl, Deps...>(Lifetime::Singleton);
    }

    /// Registers a `Transient` constructed directly with `Impl(Deps...)`.
    template <typename Impl, typename... Deps>
    ServiceCollection& add_transient() {
        return register_ctor<Impl, Impl, Deps...>(Lifetime::Transient);
    }

    /// Registers a `Transient` implementing `Interface` via `Impl(Deps...)`.
    template <typename Interface, typename Impl, typename... Deps>
        requires (!std::is_same_v<Interface, Impl> && std::is_base_of_v<Interface, Impl>)
    ServiceCollection& add_transient_as() {
        return register_ctor<Interface, Impl, Deps...>(Lifetime::Transient);
    }

    /// Registers a `Scoped` service constructed directly with `Impl(Deps...)`.
    template <typename Impl, typename... Deps>
    ServiceCollection& add_scoped() {
        return register_ctor<Impl, Impl, Deps...>(Lifetime::Scoped);
    }

    /// Registers a `Scoped` service implementing `Interface` via `Impl(Deps...)`.
    template <typename Interface, typename Impl, typename... Deps>
        requires (!std::is_same_v<Interface, Impl> && std::is_base_of_v<Interface, Impl>)
    ServiceCollection& add_scoped_as() {
        return register_ctor<Interface, Impl, Deps...>(Lifetime::Scoped);
    }

    /**
     * @brief Validates the dependency graph (missing deps, cycles, captive scopes) and builds an immutable `ServiceProvider`.
     */
    [[nodiscard]] ServiceProvider build() const {
        validate_graph(descriptors_);
        auto state = std::make_shared<detail::ProviderState>();
        state->descriptors = descriptors_;
        return ServiceProvider(std::move(state));
    }
};

} // namespace goforge
// ---- end goforge/di.hpp ----
// ---- begin goforge/lifecycle.hpp ----

/**
 * @file lifecycle.hpp
 * @brief Uber Fx-style ordered application lifecycle (`Lifecycle`, `LifecycleHook`) with
 *        automatic rollback on startup failure and reverse-order graceful shutdown.
 */

#include <cstddef>
#include <exception>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "gorun/gorun.hpp"

namespace goforge {

/**
 * @struct LifecycleHook
 * @brief A pair of `on_start` / `on_stop` callbacks registered with `Lifecycle::append(...)`.
 */
struct LifecycleHook {
    std::string name;                                 ///< Human-readable hook identifier for diagnostics
    std::function<void(gorun::Context)> on_start{};   ///< Invoked in registration order during `App::start()`
    std::function<void(gorun::Context)> on_stop{};    ///< Invoked in reverse order during `App::stop()`
};

/**
 * @class Lifecycle
 * @brief Manages ordered startup and reverse-order shutdown of application components.
 *
 * Mirrors Uber Fx's `fx.Lifecycle`:
 * - Services can inject `goforge::Lifecycle&` in their constructors/factories and call `append(...)`.
 * - `start(ctx)` runs hooks `[0 .. N-1]`. If hook `k` throws, `on_stop` is automatically invoked in
 *   reverse order for hooks `[k-1 .. 0]` before rethrowing the original startup error.
 * - `stop(ctx)` runs `on_stop` in reverse order `[N-1 .. 0]`, ensuring every component gets a chance
 *   to clean up even if an earlier `on_stop` throws.
 */
class Lifecycle {
    gorun::Mutex mtx_;
    std::vector<LifecycleHook> hooks_;
    std::size_t started_count_{0};

public:
    Lifecycle() = default;
    Lifecycle(const Lifecycle&) = delete;
    Lifecycle& operator=(const Lifecycle&) = delete;

    /// Appends a lifecycle hook. Safe to call from constructors, factories, or invoke callbacks.
    void append(LifecycleHook hook) {
        std::lock_guard<gorun::Mutex> lk(mtx_);
        hooks_.push_back(std::move(hook));
    }

    /// Returns the total number of registered hooks.
    [[nodiscard]] std::size_t hook_count() {
        std::lock_guard<gorun::Mutex> lk(mtx_);
        return hooks_.size();
    }

    /// Starts all registered hooks in FIFO order. Automatically rolls back already-started hooks on failure.
    void start(gorun::Context ctx) {
        std::vector<LifecycleHook> snapshot;
        {
            std::lock_guard<gorun::Mutex> lk(mtx_);
            snapshot = hooks_;
            started_count_ = 0;
        }

        std::size_t started = 0;
        try {
            for (std::size_t i = 0; i < snapshot.size(); ++i) {
                if (ctx.is_canceled()) {
                    throw std::runtime_error(
                        "Lifecycle start aborted before hook '" + snapshot[i].name + "': " +
                        std::string(gorun::to_string(*ctx.err()))
                    );
                }
                if (snapshot[i].on_start) {
                    snapshot[i].on_start(ctx);
                }
                started = i + 1;
            }
            std::lock_guard<gorun::Mutex> lk(mtx_);
            started_count_ = started;
        } catch (...) {
            // Rollback hooks [started-1 .. 0] in reverse order
            auto [rb_ctx, rb_cancel] = gorun::with_cancel(gorun::background());
            GORUN_DEFER { rb_cancel(); };
            for (std::size_t i = started; i > 0; --i) {
                const auto& hook = snapshot[i - 1];
                if (hook.on_stop) {
                    try {
                        hook.on_stop(rb_ctx);
                    } catch (...) {
                    }
                }
            }
            {
                std::lock_guard<gorun::Mutex> lk(mtx_);
                started_count_ = 0;
            }
            throw;
        }
    }

    /**
     * @brief Stops all started hooks in reverse (LIFO) order.
     * @return List of error messages from hooks whose `on_stop` threw (empty if clean shutdown).
     */
    std::vector<std::string> stop(gorun::Context ctx) {
        std::vector<LifecycleHook> snapshot;
        std::size_t count = 0;
        {
            std::lock_guard<gorun::Mutex> lk(mtx_);
            snapshot = hooks_;
            count = started_count_;
            started_count_ = 0;
        }

        std::vector<std::string> errors;
        for (std::size_t i = count; i > 0; --i) {
            const auto& hook = snapshot[i - 1];
            if (!hook.on_stop) continue;
            try {
                hook.on_stop(ctx);
            } catch (const std::exception& ex) {
                errors.push_back("Hook '" + hook.name + "' on_stop failed: " + ex.what());
            } catch (...) {
                errors.push_back("Hook '" + hook.name + "' on_stop failed with unknown exception");
            }
        }
        return errors;
    }
};

} // namespace goforge
// ---- end goforge/lifecycle.hpp ----
// ---- begin goforge/blocking_pool.hpp ----

/**
 * @file blocking_pool.hpp
 * @brief Dedicated OS-thread pool for offloading synchronous/blocking I/O (e.g., libpq, sqlite3,
 *        disk I/O) without blocking `gorun` M:N green-thread worker threads.
 */

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <exception>
#include <functional>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "gorun/gorun.hpp"

namespace goforge {

/**
 * @class BlockingPool
 * @brief Offloads blocking syscalls / legacy synchronous drivers onto dedicated OS threads while
 *        parking the calling `gorun` green routine on a `gorun::Channel` (zero worker starvation).
 */
class BlockingPool {
    std::mutex mtx_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> tasks_;
    std::vector<std::jthread> threads_;
    bool stopping_{false};

    void worker_loop() {
        for (;;) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lk(mtx_);
                cv_.wait(lk, [this]() { return stopping_ || !tasks_.empty(); });
                if (stopping_ && tasks_.empty()) {
                    return;
                }
                task = std::move(tasks_.front());
                tasks_.pop_front();
            }
            if (task) {
                task();
            }
        }
    }

public:
    explicit BlockingPool(std::size_t thread_count = 4) {
        if (thread_count == 0) {
            thread_count = 1;
        }
        threads_.reserve(thread_count);
        for (std::size_t i = 0; i < thread_count; ++i) {
            threads_.emplace_back([this]() { worker_loop(); });
        }
    }

    ~BlockingPool() {
        shutdown();
    }

    BlockingPool(const BlockingPool&) = delete;
    BlockingPool& operator=(const BlockingPool&) = delete;

    /// Returns the number of dedicated blocking OS threads.
    [[nodiscard]] std::size_t thread_count() const noexcept {
        return threads_.size();
    }

    /// Gracefully drains pending tasks and joins all blocking OS worker threads.
    void shutdown() {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (stopping_) return;
            stopping_ = true;
        }
        cv_.notify_all();
        for (auto& t : threads_) {
            if (t.joinable()) {
                t.join();
            }
        }
    }

    /**
     * @brief Executes a blocking callable `fn()` on the dedicated OS pool and parks the calling
     *        `gorun` green routine until completion (rethrows any exception thrown by `fn`).
     */
    template <typename Fn>
    auto run(Fn&& fn) -> std::invoke_result_t<std::decay_t<Fn>&> {
        using R = std::invoke_result_t<std::decay_t<Fn>&>;

        if constexpr (std::is_void_v<R>) {
            auto done_ch = std::make_shared<gorun::Channel<std::exception_ptr>>(1);
            auto fn_ptr = std::make_shared<std::decay_t<Fn>>(std::forward<Fn>(fn));
            {
                std::lock_guard<std::mutex> lk(mtx_);
                if (stopping_) {
                    throw std::runtime_error("BlockingPool is shutting down");
                }
                tasks_.emplace_back([done_ch, fn_ptr]() {
                    std::exception_ptr ep;
                    try {
                        (*fn_ptr)();
                    } catch (...) {
                        ep = std::current_exception();
                    }
                    done_ch->send(std::move(ep));
                });
            }
            cv_.notify_one();

            auto res = done_ch->recv();
            if (res && *res) {
                std::rethrow_exception(*res);
            }
        } else {
            struct Box {
                std::optional<R> value;
                std::exception_ptr error;
            };
            auto done_ch = std::make_shared<gorun::Channel<Box>>(1);
            auto fn_ptr = std::make_shared<std::decay_t<Fn>>(std::forward<Fn>(fn));
            {
                std::lock_guard<std::mutex> lk(mtx_);
                if (stopping_) {
                    throw std::runtime_error("BlockingPool is shutting down");
                }
                tasks_.emplace_back([done_ch, fn_ptr]() {
                    Box box;
                    try {
                        box.value.emplace((*fn_ptr)());
                    } catch (...) {
                        box.error = std::current_exception();
                    }
                    done_ch->send(std::move(box));
                });
            }
            cv_.notify_one();

            auto res = done_ch->recv();
            if (!res) {
                throw std::runtime_error("BlockingPool channel closed unexpectedly");
            }
            if (res->error) {
                std::rethrow_exception(res->error);
            }
            return std::move(*res->value);
        }
    }
};

} // namespace goforge
// ---- end goforge/blocking_pool.hpp ----
// ---- begin goforge/app.hpp ----

/**
 * @file app.hpp
 * @brief Uber Fx-style `AppBuilder` and `App` orchestrating DI resolution, `Lifecycle` startup/shutdown,
 *        tracked background `gorun` green routines, and `BlockingPool` integration.
 */

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "gorun/gorun.hpp"




namespace goforge {

/**
 * @struct AppConfig
 * @brief Configuration for `goforge::App` startup/shutdown timeouts and blocking offload pool.
 */
struct AppConfig {
    std::chrono::milliseconds start_timeout{std::chrono::seconds(15)};
    std::chrono::milliseconds stop_timeout{std::chrono::seconds(15)};
    std::size_t blocking_pool_threads{4};
};

/**
 * @class App
 * @brief Top-level application runtime owning the `ServiceProvider`, `Lifecycle`, and background routines.
 */
class App {
    friend class AppBuilder;

    AppConfig config_;
    ServiceProvider provider_;
    std::shared_ptr<Lifecycle> lifecycle_;
    std::shared_ptr<BlockingPool> blocking_pool_;
    std::vector<std::function<void(ServiceProvider&)>> invokers_;

    gorun::Context app_ctx_;
    gorun::CancelFunc app_cancel_;
    gorun::WaitGroup bg_wg_;
    std::atomic<bool> started_{false};
    std::atomic<bool> stopped_{false};
    gorun::Channel<int> shutdown_signal_ch_{1};

    App(AppConfig cfg,
        ServiceProvider provider,
        std::shared_ptr<Lifecycle> lifecycle,
        std::shared_ptr<BlockingPool> blocking_pool,
        std::vector<std::function<void(ServiceProvider&)>> invokers)
        : config_(cfg)
        , provider_(std::move(provider))
        , lifecycle_(std::move(lifecycle))
        , blocking_pool_(std::move(blocking_pool))
        , invokers_(std::move(invokers)) {
        auto [ctx, cancel] = gorun::with_cancel(gorun::background());
        app_ctx_ = std::move(ctx);
        app_cancel_ = std::move(cancel);
    }

public:
    App(const App&) = delete;
    App& operator=(const App&) = delete;

    ~App() {
        if (started_.load(std::memory_order_acquire) && !stopped_.load(std::memory_order_acquire)) {
            try {
                stop();
            } catch (...) {
            }
        }
    }

    /// Accesses the root `ServiceProvider`.
    [[nodiscard]] ServiceProvider& services() noexcept { return provider_; }

    /// Accesses the `Lifecycle` coordinator.
    [[nodiscard]] Lifecycle& lifecycle() noexcept { return *lifecycle_; }

    /// Accesses the dedicated `BlockingPool` for offloading blocking I/O.
    [[nodiscard]] BlockingPool& blocking_pool() noexcept { return *blocking_pool_; }

    /// Returns the root application `gorun::Context` (canceled when `stop()` begins).
    [[nodiscard]] gorun::Context context() const noexcept { return app_ctx_; }

    /// Creates a new `ServiceScope` (e.g., per HTTP request).
    [[nodiscard]] ServiceScope create_scope() noexcept { return provider_.create_scope(); }

    /**
     * @brief Spawns a managed background green routine on `gorun` that receives the application
     *        `gorun::Context` and is automatically awaited during `App::stop()`.
     */
    template <typename Fn>
    void spawn_background(Fn&& fn) {
        bg_wg_.add(1);
        gorun::Context ctx = app_ctx_;
        gorun::go([this, ctx, func = std::forward<Fn>(fn)]() mutable {
            GORUN_DEFER { bg_wg_.done(); };
            func(ctx);
        });
    }

    /**
     * @brief Executes all registered `invoke(...)` functions (instantiating services & hooks),
     *        then starts all `Lifecycle` hooks in FIFO order with `config_.start_timeout`.
     */
    void start() {
        bool expected = false;
        if (!started_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            return;
        }

        try {
            for (auto& inv : invokers_) {
                inv(provider_);
            }
            auto [start_ctx, cancel] = gorun::with_timeout(app_ctx_, config_.start_timeout);
            GORUN_DEFER { cancel(); };
            lifecycle_->start(start_ctx);
        } catch (...) {
            started_.store(false, std::memory_order_release);
            throw;
        }
    }

    /**
     * @brief Performs graceful shutdown:
     * 1. Cancels the root `app_ctx_` so background routines exit cooperatively.
     * 2. Waits for all `spawn_background(...)` green routines via `gorun::WaitGroup`.
     * 3. Runs `Lifecycle::stop()` hooks in reverse (LIFO) order.
     * 4. Shuts down the `BlockingPool`.
     */
    std::vector<std::string> stop() {
        bool expected = false;
        if (!stopped_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            return {};
        }

        if (app_cancel_) {
            app_cancel_();
        }
        shutdown_signal_ch_.close();

        bg_wg_.wait();

        auto [stop_ctx, cancel] = gorun::with_timeout(gorun::background(), config_.stop_timeout);
        GORUN_DEFER { cancel(); };
        auto errors = lifecycle_->stop(stop_ctx);

        if (blocking_pool_) {
            blocking_pool_->shutdown();
        }
        return errors;
    }

    /// Triggers shutdown for an application blocked inside `run()`.
    void request_shutdown() {
        shutdown_signal_ch_.close();
    }

    /// Starts the application, parks until `request_shutdown()` is called, then executes graceful `stop()`.
    void run() {
        start();
        (void)shutdown_signal_ch_.recv();
        stop();
    }

    /**
     * @brief Starts the application, installs `SIGINT` / `SIGTERM` signal handlers, parks until either
     *        an OS termination signal arrives or `request_shutdown()` is called, then runs graceful `stop()`.
     */
    void run_until_signal() {
        signal_flag().store(false, std::memory_order_release);
        std::signal(SIGINT, &App::on_os_signal);
        std::signal(SIGTERM, &App::on_os_signal);

        start();

        // Lightweight signal-polling green routine (does not block any OS worker thread)
        gorun::go([this]() {
            while (!app_ctx_.is_canceled() && !stopped_.load(std::memory_order_acquire)) {
                if (signal_flag().load(std::memory_order_acquire)) {
                    request_shutdown();
                    return;
                }
                gorun::sleep_for(std::chrono::milliseconds(25));
            }
        });

        (void)shutdown_signal_ch_.recv();
        stop();
    }

private:
    static std::atomic<bool>& signal_flag() noexcept {
        static std::atomic<bool> flag{false};
        return flag;
    }

    static void on_os_signal(int) noexcept {
        signal_flag().store(true, std::memory_order_release);
    }
};

/**
 * @class AppBuilder
 * @brief Fluent Uber Fx-style builder for wiring DI providers, invoke callbacks, and lifecycle hooks.
 */
class AppBuilder {
    AppConfig config_{};
    ServiceCollection services_{};
    std::vector<std::function<void(ServiceProvider&)>> invokers_;

public:
    AppBuilder() = default;

    /// Sets application startup/shutdown timeouts and blocking pool sizing.
    AppBuilder& with_config(AppConfig cfg) {
        config_ = cfg;
        return *this;
    }

    /// Direct access to the underlying `ServiceCollection`.
    ServiceCollection& services() noexcept { return services_; }

    /// Registers a pre-built instance as a `Singleton`.
    template <typename T>
    AppBuilder& provide_instance(T&& value) {
        services_.add_instance(std::forward<T>(value));
        return *this;
    }

    /// Registers a `Singleton` constructed with `Impl(Deps...)`.
    template <typename Impl, typename... Deps>
    AppBuilder& provide_singleton() {
        services_.add_singleton<Impl, Deps...>();
        return *this;
    }

    /// Registers a `Singleton` implementing `Interface` via `Impl(Deps...)`.
    template <typename Interface, typename Impl, typename... Deps>
    AppBuilder& provide_singleton_as() {
        services_.add_singleton_as<Interface, Impl, Deps...>();
        return *this;
    }

    /// Registers a `Transient` constructed with `Impl(Deps...)`.
    template <typename Impl, typename... Deps>
    AppBuilder& provide_transient() {
        services_.add_transient<Impl, Deps...>();
        return *this;
    }

    /// Registers a `Transient` implementing `Interface` via `Impl(Deps...)`.
    template <typename Interface, typename Impl, typename... Deps>
    AppBuilder& provide_transient_as() {
        services_.add_transient_as<Interface, Impl, Deps...>();
        return *this;
    }

    /// Registers a `Scoped` service constructed with `Impl(Deps...)`.
    template <typename Impl, typename... Deps>
    AppBuilder& provide_scoped() {
        services_.add_scoped<Impl, Deps...>();
        return *this;
    }

    /// Registers a `Scoped` service implementing `Interface` via `Impl(Deps...)`.
    template <typename Interface, typename Impl, typename... Deps>
    AppBuilder& provide_scoped_as() {
        services_.add_scoped_as<Interface, Impl, Deps...>();
        return *this;
    }

    /// Registers a factory `R(Deps...)` with the given `Lifetime` (default: `Singleton`).
    template <typename Factory>
    AppBuilder& provide(Factory&& factory, Lifetime lifetime = Lifetime::Singleton) {
        services_.provide(lifetime, std::forward<Factory>(factory));
        return *this;
    }

    /// Registers a factory explicitly bound to `ServiceType` (e.g., an interface).
    template <typename ServiceType, typename Factory>
    AppBuilder& provide_as(Factory&& factory, Lifetime lifetime = Lifetime::Singleton) {
        services_.add_factory<ServiceType>(lifetime, std::forward<Factory>(factory));
        return *this;
    }

    /**
     * @brief Registers an `invoke(Deps...)` function executed during `App::start()` to trigger eager
     *        service instantiation and register `Lifecycle` hooks (mirrors `fx.Invoke`).
     */
    template <typename Fn>
    AppBuilder& invoke(Fn&& fn) {
        auto holder = std::make_shared<std::decay_t<Fn>>(std::forward<Fn>(fn));
        invokers_.push_back([holder](ServiceProvider& sp) {
            sp.invoke(*holder);
        });
        return *this;
    }

    /**
     * @brief Validates the full dependency graph (missing deps, cycles, captive scopes) and builds `App`.
     */
    [[nodiscard]] std::unique_ptr<App> build() {
        auto lifecycle = std::make_shared<Lifecycle>();
        auto blocking_pool = std::make_shared<BlockingPool>(config_.blocking_pool_threads);

        services_.add_instance<Lifecycle>(lifecycle);
        services_.add_instance<BlockingPool>(blocking_pool);

        ServiceProvider provider = services_.build();
        return std::unique_ptr<App>(new App(
            config_,
            std::move(provider),
            std::move(lifecycle),
            std::move(blocking_pool),
            std::move(invokers_)
        ));
    }
};

} // namespace goforge
// ---- end goforge/app.hpp ----
// ---- begin goforge/http/http_types.hpp ----

/**
 * @file http_types.hpp
 * @brief HTTP/1.1 request/response models, case-insensitive headers, status reason phrases,
 *        and incremental parsing/serialization helpers.
 */

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>



namespace goforge::http {

namespace detail {

inline std::string to_lower_ascii(std::string_view sv) {
    std::string out;
    out.reserve(sv.size());
    for (unsigned char c : sv) {
        out.push_back(static_cast<char>(std::tolower(c)));
    }
    return out;
}

inline std::string_view trim_ows(std::string_view sv) noexcept {
    while (!sv.empty() && (sv.front() == ' ' || sv.front() == '\t')) sv.remove_prefix(1);
    while (!sv.empty() && (sv.back() == ' ' || sv.back() == '\t')) sv.remove_suffix(1);
    return sv;
}

inline std::string_view default_reason_phrase(int status) noexcept {
    switch (status) {
        case 200: return "OK";
        case 201: return "Created";
        case 202: return "Accepted";
        case 204: return "No Content";
        case 301: return "Moved Permanently";
        case 302: return "Found";
        case 304: return "Not Modified";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 408: return "Request Timeout";
        case 409: return "Conflict";
        case 413: return "Payload Too Large";
        case 422: return "Unprocessable Entity";
        case 429: return "Too Many Requests";
        case 431: return "Request Header Fields Too Large";
        case 500: return "Internal Server Error";
        case 502: return "Bad Gateway";
        case 503: return "Service Unavailable";
        case 504: return "Gateway Timeout";
        default:  return "Unknown";
    }
}

} // namespace detail

/**
 * @class HeaderMap
 * @brief Case-insensitive HTTP header container preserving canonical wire names for responses.
 */
class HeaderMap {
    struct Entry {
        std::string canonical_name;
        std::string value;
    };
    std::unordered_map<std::string, Entry> entries_;

public:
    void set(std::string_view name, std::string value) {
        std::string lower = detail::to_lower_ascii(name);
        entries_.insert_or_assign(std::move(lower), Entry{std::string(name), std::move(value)});
    }

    [[nodiscard]] bool contains(std::string_view name) const {
        return entries_.contains(detail::to_lower_ascii(name));
    }

    [[nodiscard]] std::optional<std::string_view> get(std::string_view name) const {
        auto it = entries_.find(detail::to_lower_ascii(name));
        if (it == entries_.end()) return std::nullopt;
        return std::string_view(it->second.value);
    }

    [[nodiscard]] std::string get_or(std::string_view name, std::string_view fallback = "") const {
        auto v = get(name);
        return v ? std::string(*v) : std::string(fallback);
    }

    void erase(std::string_view name) {
        entries_.erase(detail::to_lower_ascii(name));
    }

    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
    [[nodiscard]] bool empty() const noexcept { return entries_.empty(); }

    template <typename Fn>
    void for_each(Fn&& fn) const {
        for (const auto& [_, entry] : entries_) {
            fn(entry.canonical_name, entry.value);
        }
    }
};

/**
 * @struct HttpRequest
 * @brief Parsed HTTP/1.1 request message.
 */
struct HttpRequest {
    std::string method;
    std::string target;
    std::string path;
    std::string raw_query;
    std::string version{"HTTP/1.1"};
    HeaderMap headers;
    std::string body;
    std::string remote_addr;

    /// Returns whether this connection should remain open for HTTP/1.1 Keep-Alive.
    [[nodiscard]] bool keep_alive() const {
        auto conn = headers.get("Connection");
        if (conn) {
            std::string lower = detail::to_lower_ascii(*conn);
            if (lower.find("close") != std::string::npos) return false;
            if (lower.find("keep-alive") != std::string::npos) return true;
        }
        return version == "HTTP/1.1";
    }
};

/**
 * @struct HttpResponse
 * @brief HTTP/1.1 response message with wire serializer and convenience factories.
 */
struct HttpResponse {
    int status{200};
    std::string status_text;
    HeaderMap headers;
    std::string body;

    static HttpResponse text(int code, std::string content) {
        HttpResponse r;
        r.status = code;
        r.body = std::move(content);
        r.headers.set("Content-Type", "text/plain; charset=utf-8");
        return r;
    }

    static HttpResponse json(int code, std::string raw_json_body) {
        HttpResponse r;
        r.status = code;
        r.body = std::move(raw_json_body);
        r.headers.set("Content-Type", "application/json; charset=utf-8");
        return r;
    }

    static HttpResponse json(int code, const char* raw_json_body) {
        return json(code, std::string(raw_json_body ? raw_json_body : ""));
    }

    static HttpResponse json(int code, const Json& obj) {
        return json(code, obj.dump());
    }

    template <typename T>
        requires (!std::convertible_to<T, std::string_view> && !std::same_as<std::decay_t<T>, Json>)
    static HttpResponse json(int code, const T& dto) {
        return json(code, Json(dto).dump());
    }

    static HttpResponse error(int code, std::string_view error_code, std::string_view message = "") {
        return json(code, make_error_json(error_code, message));
    }

    static HttpResponse empty(int code = 204) {
        HttpResponse r;
        r.status = code;
        return r;
    }

    /// Serializes this response into an HTTP/1.1 wire buffer.
    [[nodiscard]] std::string serialize(bool keep_alive) const {
        std::string_view reason = status_text.empty()
            ? detail::default_reason_phrase(status)
            : std::string_view(status_text);

        std::string out;
        out.reserve(128 + body.size());
        out += "HTTP/1.1 ";
        out += std::to_string(status);
        out += ' ';
        out += reason;
        out += "\r\n";

        bool has_content_length = false;
        bool has_connection = false;
        bool has_server = false;

        headers.for_each([&](const std::string& k, const std::string& v) {
            std::string lower = detail::to_lower_ascii(k);
            if (lower == "content-length") has_content_length = true;
            if (lower == "connection") has_connection = true;
            if (lower == "server") has_server = true;
            out += k;
            out += ": ";
            out += v;
            out += "\r\n";
        });

        if (!has_server) {
            out += "Server: goforge\r\n";
        }
        if (!has_content_length) {
            out += "Content-Length: ";
            out += std::to_string(body.size());
            out += "\r\n";
        }
        if (!has_connection) {
            out += keep_alive ? "Connection: keep-alive\r\n" : "Connection: close\r\n";
        }
        out += "\r\n";
        out += body;
        return out;
    }
};

enum class ParseStatus : std::uint8_t {
    Ok,
    Incomplete,
    BadRequest,
    HeadersTooLarge,
    PayloadTooLarge
};

struct ParseHeaderResult {
    ParseStatus status{ParseStatus::Incomplete};
    HttpRequest request;
    std::size_t header_end_pos{0}; ///< Offset immediately after "\r\n\r\n"
    std::size_t content_length{0};
};

/**
 * @brief Parses HTTP/1.1 request line and headers from `raw_buf` (non-destructive).
 */
inline ParseHeaderResult parse_http_headers(
    std::string_view raw_buf,
    std::size_t max_header_bytes,
    std::size_t max_body_bytes
) {
    ParseHeaderResult res;
    auto sep = raw_buf.find("\r\n\r\n");
    if (sep == std::string_view::npos) {
        if (raw_buf.size() > max_header_bytes) {
            res.status = ParseStatus::HeadersTooLarge;
        } else {
            res.status = ParseStatus::Incomplete;
        }
        return res;
    }

    if (sep + 4 > max_header_bytes) {
        res.status = ParseStatus::HeadersTooLarge;
        return res;
    }

    res.header_end_pos = sep + 4;
    std::string_view head = raw_buf.substr(0, sep);

    // 1. Parse request line: METHOD SP REQUEST-TARGET SP HTTP-VERSION
    auto first_crlf = head.find("\r\n");
    std::string_view req_line = (first_crlf == std::string_view::npos) ? head : head.substr(0, first_crlf);

    auto sp1 = req_line.find(' ');
    if (sp1 == std::string_view::npos || sp1 == 0) {
        res.status = ParseStatus::BadRequest;
        return res;
    }
    auto sp2 = req_line.find(' ', sp1 + 1);
    if (sp2 == std::string_view::npos || sp2 == sp1 + 1 || sp2 + 1 >= req_line.size()) {
        res.status = ParseStatus::BadRequest;
        return res;
    }

    std::string_view method = req_line.substr(0, sp1);
    std::string_view target = req_line.substr(sp1 + 1, sp2 - (sp1 + 1));
    std::string_view version = req_line.substr(sp2 + 1);

    if (version != "HTTP/1.1" && version != "HTTP/1.0") {
        res.status = ParseStatus::BadRequest;
        return res;
    }

    res.request.method = std::string(method);
    res.request.target = std::string(target);
    res.request.version = std::string(version);

    auto qpos = target.find('?');
    if (qpos == std::string_view::npos) {
        res.request.path = std::string(target);
    } else {
        res.request.path = std::string(target.substr(0, qpos));
        res.request.raw_query = std::string(target.substr(qpos + 1));
    }

    // 2. Parse header lines
    if (first_crlf != std::string_view::npos) {
        std::size_t pos = first_crlf + 2;
        while (pos < head.size()) {
            auto next = head.find("\r\n", pos);
            std::string_view line = (next == std::string_view::npos)
                ? head.substr(pos)
                : head.substr(pos, next - pos);
            pos = (next == std::string_view::npos) ? head.size() : next + 2;

            if (line.empty()) continue;
            auto colon = line.find(':');
            if (colon == std::string_view::npos || colon == 0) {
                res.status = ParseStatus::BadRequest;
                return res;
            }
            std::string_view name = line.substr(0, colon);
            std::string_view val = detail::trim_ows(line.substr(colon + 1));
            res.request.headers.set(name, std::string(val));
        }
    }

    // 3. Parse optional Content-Length
    if (auto cl = res.request.headers.get("Content-Length")) {
        std::size_t len = 0;
        const char* first = cl->data();
        const char* last = first + cl->size();
        auto [ptr, ec] = std::from_chars(first, last, len);
        if (ec != std::errc{} || ptr != last) {
            res.status = ParseStatus::BadRequest;
            return res;
        }
        if (len > max_body_bytes) {
            res.status = ParseStatus::PayloadTooLarge;
            return res;
        }
        res.content_length = len;
    }

    res.status = ParseStatus::Ok;
    return res;
}

} // namespace goforge::http
// ---- end goforge/http/http_types.hpp ----
// ---- begin goforge/web/context.hpp ----

/**
 * @file context.hpp
 * @brief Gin-style per-request `HttpContext` with path/query parameters, URL
 * decoding, typed key-value store, `gorun::Context` cancellation,
 * `ServiceScope` DI access, response builders, and middleware chain control
 * (`next()`, `abort()`).
 */

#include <any>
#include <cctype>
#include <charconv>
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

namespace goforge::web {

class HttpContext;

/// Middleware and route handler signature (mirrors `gin.HandlerFunc`).
using HandlerFunc = std::function<void(HttpContext &)>;

namespace detail {

inline int hex_digit_value(char c) noexcept {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return 10 + (c - 'a');
  if (c >= 'A' && c <= 'F')
    return 10 + (c - 'A');
  return -1;
}

inline std::string url_decode(std::string_view input,
                              bool plus_as_space = true) {
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

inline std::unordered_map<std::string, std::string>
parse_query_string(std::string_view raw) {
  std::unordered_map<std::string, std::string> result;
  std::size_t pos = 0;
  while (pos < raw.size()) {
    auto amp = raw.find('&', pos);
    std::string_view pair = (amp == std::string_view::npos)
                                ? raw.substr(pos)
                                : raw.substr(pos, amp - pos);
    pos = (amp == std::string_view::npos) ? raw.size() : amp + 1;

    if (pair.empty())
      continue;
    auto eq = pair.find('=');
    if (eq == std::string_view::npos) {
      result.insert_or_assign(url_decode(pair), std::string{});
    } else {
      result.insert_or_assign(url_decode(pair.substr(0, eq)),
                              url_decode(pair.substr(eq + 1)));
    }
  }
  return result;
}

template <typename T>
std::optional<T> parse_number(std::string_view sv) noexcept {
  if (sv.empty())
    return std::nullopt;
  T value{};
  const char *first = sv.data();
  const char *last = first + sv.size();
  auto [ptr, ec] = std::from_chars(first, last, value);
  if (ec != std::errc{} || ptr != last) {
    return std::nullopt;
  }
  return value;
}

} // namespace detail

/**
 * @brief Sleeps cooperatively inside a green routine until `duration` elapses
 * or `ctx` is canceled.
 * @return `true` if the full duration elapsed, `false` if canceled by `ctx`
 * (e.g., Timeout middleware).
 */
inline bool sleep_with_context(gorun::Context ctx,
                               std::chrono::milliseconds duration) {
  if (ctx.is_canceled())
    return false;
  bool completed = false;
  gorun::select(gorun::on_timeout(duration, [&]() { completed = true; }),
                ctx.on_done([&]() { completed = false; }));
  return completed;
}

/**
 * @class HttpContext
 * @brief Per-request execution context passed through the middleware chain and
 * route handler.
 */
class HttpContext {
  const http::HttpRequest *req_;
  http::HttpResponse resp_;
  gorun::Context go_ctx_;
  ServiceScope *scope_{nullptr};

  std::unordered_map<std::string, std::string> params_;
  mutable bool query_parsed_{false};
  mutable std::unordered_map<std::string, std::string> query_params_;
  std::unordered_map<std::string, std::any> values_;

  const std::vector<HandlerFunc> *global_handlers_{nullptr};
  const std::vector<HandlerFunc> *route_handlers_{nullptr};
  std::vector<HandlerFunc> owned_route_handlers_;
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
  HttpContext(const http::HttpRequest &req, gorun::Context go_ctx,
              ServiceScope *scope = nullptr)
      : req_(&req), go_ctx_(std::move(go_ctx)), scope_(scope) {}

  HttpContext(const HttpContext &) = delete;
  HttpContext &operator=(const HttpContext &) = delete;

  // ---- Request inspection ----
  [[nodiscard]] const http::HttpRequest &request() const noexcept {
    return *req_;
  }
  [[nodiscard]] const std::string &method() const noexcept {
    return req_->method;
  }
  [[nodiscard]] const std::string &path() const noexcept { return req_->path; }
  [[nodiscard]] const std::string &raw_query() const noexcept {
    return req_->raw_query;
  }
  [[nodiscard]] const std::string &body() const noexcept { return req_->body; }
  [[nodiscard]] const std::string &remote_addr() const noexcept {
    return req_->remote_addr;
  }

  [[nodiscard]] std::optional<std::string_view>
  header(std::string_view name) const {
    return req_->headers.get(name);
  }

  [[nodiscard]] std::string header_or(std::string_view name,
                                      std::string_view fallback = "") const {
    return req_->headers.get_or(name, fallback);
  }

  // ---- Path parameters (:id, *filepath) ----
  void set_params(std::unordered_map<std::string, std::string> params) {
    params_ = std::move(params);
  }

  [[nodiscard]] std::optional<std::string_view>
  param(std::string_view key) const {
    auto it = params_.find(std::string(key));
    if (it == params_.end())
      return std::nullopt;
    return std::string_view(it->second);
  }

  [[nodiscard]] std::string param_or(std::string_view key,
                                     std::string_view fallback = "") const {
    auto v = param(key);
    return v ? std::string(*v) : std::string(fallback);
  }

  template <typename T>
    requires std::is_arithmetic_v<T>
  [[nodiscard]] std::optional<T> param_as(std::string_view key) const noexcept {
    auto v = param(key);
    if (!v)
      return std::nullopt;
    return detail::parse_number<T>(*v);
  }

  // ---- Query parameters (?key=val&...) ----
  [[nodiscard]] std::optional<std::string_view>
  query(std::string_view key) const {
    ensure_query_parsed();
    auto it = query_params_.find(std::string(key));
    if (it == query_params_.end())
      return std::nullopt;
    return std::string_view(it->second);
  }

  [[nodiscard]] std::string query_or(std::string_view key,
                                     std::string_view fallback = "") const {
    auto v = query(key);
    return v ? std::string(*v) : std::string(fallback);
  }

  template <typename T>
    requires std::is_arithmetic_v<T>
  [[nodiscard]] std::optional<T> query_as(std::string_view key) const noexcept {
    auto v = query(key);
    if (!v)
      return std::nullopt;
    return detail::parse_number<T>(*v);
  }

  // ---- Per-request typed key-value bag (Gin-style Set/Get) ----
  template <typename T> void set(std::string key, T &&value) {
    values_.insert_or_assign(std::move(key), std::forward<T>(value));
  }

  template <typename T>
  [[nodiscard]] std::optional<T> get(const std::string &key) const {
    auto it = values_.find(key);
    if (it == values_.end())
      return std::nullopt;
    if (const T *ptr = std::any_cast<T>(&it->second)) {
      return *ptr;
    }
    return std::nullopt;
  }

  [[nodiscard]] bool has_value(const std::string &key) const {
    return values_.contains(key);
  }

  // ---- gorun::Context cancellation & timeouts ----
  [[nodiscard]] gorun::Context go_ctx() const noexcept { return go_ctx_; }

  void set_go_ctx(gorun::Context ctx) noexcept { go_ctx_ = std::move(ctx); }

  [[nodiscard]] bool is_canceled() const noexcept {
    return go_ctx_.is_canceled();
  }

  // ---- DI ServiceScope integration ----
  [[nodiscard]] bool has_scope() const noexcept { return scope_ != nullptr; }

  [[nodiscard]] ServiceScope &scope() {
    if (!scope_) {
      throw ScopeRequiredError(
          "HttpContext has no active DI ServiceScope attached.");
    }
    return *scope_;
  }

  template <typename T> [[nodiscard]] T &resolve() {
    return scope().resolve<T>();
  }

  template <typename T>
  [[nodiscard]] std::shared_ptr<std::remove_cv_t<T>> resolve_ptr() {
    return scope().resolve_ptr<T>();
  }

  template <typename Fn> decltype(auto) invoke(Fn &&fn) {
    return scope().invoke(std::forward<Fn>(fn));
  }

  // ---- Typed JSON Request Binding & Validation (Gin ShouldBindJSON style)
  // ----

  /// Parses the raw HTTP body into a generic `goforge::Json` value without
  /// writing an error response.
  [[nodiscard]] BindResult<Json> try_bind_json_value() const {
    return parse_json_body(req_->body);
  }

  /// Parses the raw HTTP body into a `goforge::Json` value; automatically
  /// aborts with `400` on parse error.
  [[nodiscard]] std::optional<Json> bind_json_value() {
    auto res = try_bind_json_value();
    if (!res.ok()) {
      abort_with_error(res.http_status, res.error_code, res.message);
      return std::nullopt;
    }
    return std::move(res.value);
  }

  /**
   * @brief Parses and validates the JSON body into DTO `T` without writing an
   * error response. Checks JSON syntax/schema (`400`) and calls
   * `dto.validate()` (`422`) if defined.
   */
  template <typename T> [[nodiscard]] BindResult<T> try_bind_json() const {
    return parse_and_validate_json<T>(req_->body);
  }

  /**
   * @brief Parses and validates the JSON body into DTO `T` (mirrors Gin's
   * `ShouldBindJSON`). On failure, automatically writes a structured JSON error
   * (`400` or `422`), aborts the middleware chain, and returns `std::nullopt`.
   */
  template <typename T> [[nodiscard]] std::optional<T> bind_json() {
    auto res = try_bind_json<T>();
    if (!res.ok()) {
      abort_with_error(res.http_status, res.error_code, res.message);
      return std::nullopt;
    }
    return std::move(res.value);
  }

  // ---- Response builders ----
  HttpContext &status(int code) {
    resp_.status = code;
    written_ = true;
    return *this;
  }

  HttpContext &set_header(std::string_view name, std::string value) {
    resp_.headers.set(name, std::move(value));
    return *this;
  }

  void data(int code, std::string_view content_type, std::string body) {
    resp_.status = code;
    resp_.body = std::move(body);
    resp_.headers.set("Content-Type", std::string(content_type));
    written_ = true;
  }

  void text(int code, std::string body) {
    data(code, "text/plain; charset=utf-8", std::move(body));
  }

  /// Writes a pre-serialized raw JSON string response.
  void json_raw(int code, std::string raw_json_body) {
    resp_.status = code;
    resp_.body = std::move(raw_json_body);
    resp_.headers.set("Content-Type", "application/json; charset=utf-8");
    written_ = true;
  }

  /// Writes a pre-serialized JSON string (backwards-compatible overload).
  void json(int code, std::string raw_json_body) {
    json_raw(code, std::move(raw_json_body));
  }

  /// Exact-match overload for raw JSON string literals (prevents ambiguity with
  /// `nlohmann::json(const char*)`).
  void json(int code, const char *raw_json_body) {
    json_raw(code, std::string(raw_json_body ? raw_json_body : ""));
  }

  /// Serializes a `goforge::Json` / `goforge::H` value to JSON and sets
  /// `Content-Type: application/json`.
  void json(int code, const Json &value) { json_raw(code, value.dump()); }

  /// Serializes any DTO (`GOFORGE_JSON_DTO`), `std::vector<Dto>`, `std::map`,
  /// etc. directly to JSON!
  template <typename T>
    requires(!std::convertible_to<T, std::string_view> &&
             !std::same_as<std::decay_t<T>, Json>)
  void json(int code, const T &dto) {
    json_raw(code, Json(dto).dump());
  }

  /// Writes a standardized structured JSON error: `{"error": error_code,
  /// "message": message}`.
  void error(int code, std::string_view error_code,
             std::string_view message = "") {
    json(code, make_error_json(error_code, message));
  }

  void no_content() {
    resp_.status = 204;
    resp_.body.clear();
    written_ = true;
  }

  [[nodiscard]] bool is_written() const noexcept { return written_; }
  [[nodiscard]] http::HttpResponse &response() noexcept { return resp_; }
  [[nodiscard]] const http::HttpResponse &response() const noexcept {
    return resp_;
  }

  // ---- Middleware chain execution (zero-copy view over global + route handler
  // chains!) ----
  void set_handler_views(const std::vector<HandlerFunc> *global_mws,
                         const std::vector<HandlerFunc> *route_mws) noexcept {
    global_handlers_ = global_mws;
    route_handlers_ = route_mws;
    owned_route_handlers_.clear();
    handler_index_ = 0;
    aborted_ = false;
  }

  void set_handler_with_fallback(const std::vector<HandlerFunc> *global_mws,
                                 HandlerFunc fallback_endpoint) {
    global_handlers_ = global_mws;
    owned_route_handlers_.clear();
    owned_route_handlers_.push_back(std::move(fallback_endpoint));
    route_handlers_ = &owned_route_handlers_;
    handler_index_ = 0;
    aborted_ = false;
  }

  void set_handlers(std::vector<HandlerFunc> chain) {
    global_handlers_ = nullptr;
    owned_route_handlers_ = std::move(chain);
    route_handlers_ = &owned_route_handlers_;
    handler_index_ = 0;
    aborted_ = false;
  }

  void next() {
    const std::size_t g_sz = global_handlers_ ? global_handlers_->size() : 0;
    const std::size_t r_sz = route_handlers_ ? route_handlers_->size() : 0;
    const std::size_t total = g_sz + r_sz;
    while (handler_index_ < total && !aborted_) {
      const std::size_t idx = handler_index_++;
      const HandlerFunc &fn = (idx < g_sz) ? (*global_handlers_)[idx]
                                           : (*route_handlers_)[idx - g_sz];
      if (fn) {
        fn(*this);
      }
    }
}

    void abort() noexcept {
  aborted_ = true;
  const std::size_t g_sz = global_handlers_ ? global_handlers_->size() : 0;
  const std::size_t r_sz = route_handlers_ ? route_handlers_->size() : 0;
  handler_index_ = g_sz + r_sz;
}

void abort_with_text(int code, std::string msg) {
  text(code, std::move(msg));
  abort();
}

void abort_with_json(int code, std::string raw_json_body) {
  json_raw(code, std::move(raw_json_body));
  abort();
}

void abort_with_json(int code, const char *raw_json_body) {
  json_raw(code, std::string(raw_json_body ? raw_json_body : ""));
  abort();
}

void abort_with_json(int code, const Json &value) {
  json(code, value);
  abort();
}

template <typename T>
  requires(!std::convertible_to<T, std::string_view> &&
           !std::same_as<std::decay_t<T>, Json>)
void abort_with_json(int code, const T &dto) {
  json(code, dto);
  abort();
}

void abort_with_error(int code, std::string_view error_code,
                      std::string_view message = "") {
  error(code, error_code, message);
  abort();
}

[[nodiscard]] bool is_aborted() const noexcept { return aborted_; }
};

} // namespace goforge::web
// ---- end goforge/web/context.hpp ----
// ---- begin goforge/web/extractors.hpp ----

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
    [[nodiscard]] const T* operator->() const { return &(*value); }
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
    [[nodiscard]] const std::string* operator->() const { return &(*value); }
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
// ---- end goforge/web/extractors.hpp ----
// ---- begin goforge/web/router.hpp ----

/**
 * @file router.hpp
 * @brief Gin-style segment radix/trie `Router` and `RouteGroup` supporting static paths,
 *        named parameters (`:id`), catch-all wildcards (`*filepath`), nested middleware chains,
 *        `405 Method Not Allowed` with `Allow` header, and compile-time DI-injected handlers.
 */

#include <algorithm>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "gorun/gorun.hpp"





namespace goforge::web {

namespace detail {

inline std::vector<std::string_view> split_path_segments(std::string_view path) {
    std::vector<std::string_view> segs;
    std::size_t i = 0;
    while (i < path.size()) {
        while (i < path.size() && path[i] == '/') ++i;
        if (i >= path.size()) break;
        std::size_t start = i;
        while (i < path.size() && path[i] != '/') ++i;
        segs.push_back(path.substr(start, i - start));
    }
    return segs;
}

inline std::string join_paths(std::string_view prefix, std::string_view suffix) {
    if (prefix.empty() || prefix == "/") {
        if (suffix.empty()) return "/";
        if (suffix.front() == '/') return std::string(suffix);
        return "/" + std::string(suffix);
    }
    std::string out(prefix);
    while (out.size() > 1 && out.back() == '/') out.pop_back();
    if (suffix.empty() || suffix == "/") return out;
    if (suffix.front() != '/') out.push_back('/');
    out.append(suffix);
    return out;
}

/// Helper invoking any callable or controller member function with mixed HTTP extractors,
/// `HttpContext&`, and DI services, plus automatic serialization of non-`void` return types.
template <typename ArgsTuple, typename Invoker, std::size_t... Is>
void invoke_adapted_handler(HttpContext& ctx, Invoker&& invoker, std::index_sequence<Is...>) {
    if constexpr (sizeof...(Is) == 0) {
        using ReturnT = decltype(invoker());
        if constexpr (std::is_void_v<ReturnT>) {
            invoker();
        } else {
            auto result = invoker();
            if (!ctx.is_aborted()) {
                write_handler_result(ctx, std::move(result));
            }
        }
        return;
    } else {
        // 1. Pre-extract all HTTP extractors (Body/Path/Query/Header) first so parse/validation failures
        //    short-circuit cleanly before invoking the handler.
        auto extract_slot = [&]<typename Param>() {
            using Clean = std::remove_cvref_t<Param>;
            if constexpr (is_http_extractor_v<Param>) {
                return extract_http_arg<Clean>(ctx);
            } else {
                return std::true_type{};
            }
        };

        auto slots = std::make_tuple(extract_slot.template operator()<std::tuple_element_t<Is, ArgsTuple>>()...);

        bool all_ok = true;
        auto check_slot = [&]<std::size_t I>() {
            using Param = std::tuple_element_t<I, ArgsTuple>;
            if constexpr (is_http_extractor_v<Param>) {
                if (!std::get<I>(slots).has_value()) {
                    all_ok = false;
                }
            }
        };
        (check_slot.template operator()<Is>(), ...);
        if (!all_ok) {
            return;
        }

        // 2. Materialize each argument (extracted HTTP value, HttpContext&, or DI service)
        auto get_arg = [&]<std::size_t I>() -> decltype(auto) {
            using Param = std::tuple_element_t<I, ArgsTuple>;
            using Clean = std::remove_cvref_t<Param>;
            if constexpr (is_http_extractor_v<Param>) {
                return static_cast<Param>(*std::get<I>(slots));
            } else if constexpr (std::is_same_v<Clean, HttpContext>) {
                return static_cast<HttpContext&>(ctx);
            } else if constexpr (std::is_same_v<Clean, ServiceScope>) {
                return ctx.scope();
            } else if constexpr (std::is_same_v<Clean, ServiceProvider>) {
                return ctx.scope().root();
            } else if constexpr (goforge::detail::IsSharedPtr<Clean>::value) {
                using Elem = typename goforge::detail::IsSharedPtr<Clean>::element_type;
                using RawT = std::remove_cv_t<Elem>;
                return std::static_pointer_cast<Elem>(ctx.resolve_ptr<RawT>());
            } else if constexpr (std::is_reference_v<Param>) {
                return static_cast<Param>(ctx.resolve<Clean>());
            } else {
                return ctx.resolve<Clean>();
            }
        };

        using ReturnT = decltype(invoker(get_arg.template operator()<Is>()...));
        if constexpr (std::is_void_v<ReturnT>) {
            invoker(get_arg.template operator()<Is>()...);
        } else {
            auto result = invoker(get_arg.template operator()<Is>()...);
            if (!ctx.is_aborted()) {
                write_handler_result(ctx, std::move(result));
            }
        }
    }
}

template <typename Fn>
constexpr bool is_direct_void_handler() noexcept {
    if constexpr (std::is_invocable_v<Fn&, HttpContext&>) {
        return std::is_void_v<std::invoke_result_t<Fn&, HttpContext&>>;
    }
    return false;
}

/// Adapts:
/// 1. Classic `void(HttpContext&)` (zero-overhead fast path),
/// 2. Classic DI `void(HttpContext&, Deps...)`,
/// 3. Ergonomic Minimal API `R(Body<Dto>, Path<"id", int>, Query<"q">, MyService&, ...)`,
/// 4. Zero-lambda Controller member pointers `&UserController::get_by_id` (resolved via DI!).
template <typename Fn>
HandlerFunc adapt_handler(Fn&& fn) {
    using DecayedFn = std::decay_t<Fn>;
    if constexpr (std::is_member_function_pointer_v<DecayedFn>) {
        using Traits = goforge::detail::FunctionTraits<DecayedFn>;
        using ControllerT = typename Traits::class_type;
        using ArgsTuple = typename Traits::args_tuple;
        return [method = fn](HttpContext& ctx) {
            auto& controller = ctx.resolve<ControllerT>();
            auto caller = [&](auto&&... args) -> decltype(auto) {
                return (controller.*method)(std::forward<decltype(args)>(args)...);
            };
            invoke_adapted_handler<ArgsTuple>(
                ctx,
                caller,
                std::make_index_sequence<std::tuple_size_v<ArgsTuple>>{}
            );
        };
    } else if constexpr (is_direct_void_handler<DecayedFn>()) {
        return HandlerFunc(std::forward<Fn>(fn));
    } else {
        using Traits = goforge::detail::FunctionTraits<DecayedFn>;
        using ArgsTuple = typename Traits::args_tuple;
        auto holder = std::make_shared<DecayedFn>(std::forward<Fn>(fn));
        return [holder](HttpContext& ctx) {
            invoke_adapted_handler<ArgsTuple>(
                ctx,
                *holder,
                std::make_index_sequence<std::tuple_size_v<ArgsTuple>>{}
            );
        };
    }
}

struct StringViewHash {
    using is_transparent = void;
    std::size_t operator()(std::string_view sv) const noexcept {
        return std::hash<std::string_view>{}(sv);
    }
    std::size_t operator()(const std::string& s) const noexcept {
        return std::hash<std::string_view>{}(s);
    }
};

struct TrieNode {
    std::unordered_map<std::string, std::unique_ptr<TrieNode>, StringViewHash, std::equal_to<>> static_children;

    std::string param_name;
    std::unique_ptr<TrieNode> param_child;

    std::string wildcard_name;
    std::unique_ptr<TrieNode> wildcard_child;

    bool has_handler{false};
    std::vector<HandlerFunc> chain;
};

struct RouteMatch {
    bool matched{false};
    const std::vector<HandlerFunc>* chain{nullptr};
    std::unordered_map<std::string, std::string> params;
};

inline bool match_trie(const TrieNode* node,
                       const std::vector<std::string_view>& segs,
                       std::size_t depth,
                       RouteMatch& out) {
    if (!node) return false;

    if (depth == segs.size()) {
        if (node->has_handler) {
            out.matched = true;
            out.chain = &node->chain;
            return true;
        }
        // Allow trailing empty wildcard match (e.g. "/static/*filepath" matching "/static/")
        if (node->wildcard_child && node->wildcard_child->has_handler) {
            out.params.insert_or_assign(node->wildcard_name, std::string{});
            out.matched = true;
            out.chain = &node->wildcard_child->chain;
            return true;
        }
        return false;
    }

    // 1. Static segment priority (zero-allocation transparent string_view lookup!)
    auto it = node->static_children.find(segs[depth]);
    if (it != node->static_children.end()) {
        if (match_trie(it->second.get(), segs, depth + 1, out)) {
            return true;
        }
    }

    // 2. Named parameter (:id) priority
    if (node->param_child) {
        std::string decoded = url_decode(segs[depth], false);
        auto prev_it = out.params.find(node->param_name);
        std::optional<std::string> prev_val;
        if (prev_it != out.params.end()) prev_val = prev_it->second;

        out.params.insert_or_assign(node->param_name, std::move(decoded));
        if (match_trie(node->param_child.get(), segs, depth + 1, out)) {
            return true;
        }
        if (prev_val) {
            out.params.insert_or_assign(node->param_name, std::move(*prev_val));
        } else {
            out.params.erase(node->param_name);
        }
    }

    // 3. Catch-all wildcard (*filepath) priority
    if (node->wildcard_child && node->wildcard_child->has_handler) {
        std::string rest;
        for (std::size_t i = depth; i < segs.size(); ++i) {
            if (i > depth) rest.push_back('/');
            rest.append(url_decode(segs[i], false));
        }
        out.params.insert_or_assign(node->wildcard_name, std::move(rest));
        out.matched = true;
        out.chain = &node->wildcard_child->chain;
        return true;
    }

    return false;
}

} // namespace detail

class Router;

/**
 * @class RouteGroup
 * @brief Scoped route prefix + inherited middleware list (mirrors `gin.RouterGroup`).
 */
class RouteGroup {
    friend class Router;

    Router* router_;
    std::string prefix_;
    std::vector<HandlerFunc> middlewares_;

    RouteGroup(Router& router, std::string prefix, std::vector<HandlerFunc> middlewares)
        : router_(&router)
        , prefix_(std::move(prefix))
        , middlewares_(std::move(middlewares)) {}

public:
    /// Appends one or more middleware functions to this group (inherited by child groups & routes).
    RouteGroup& use(HandlerFunc mw) {
        middlewares_.push_back(std::move(mw));
        return *this;
    }

    /// Creates a nested child `RouteGroup` inheriting this group's prefix and middleware chain.
    [[nodiscard]] RouteGroup group(std::string_view sub_prefix) const {
        return RouteGroup(*router_, detail::join_paths(prefix_, sub_prefix), middlewares_);
    }

    /// Registers a route for an arbitrary HTTP method (supports both `HttpContext&` and DI-injected handlers).
    template <typename Fn>
    RouteGroup& handle(std::string_view method, std::string_view path, Fn&& fn);

    template <typename Fn>
    RouteGroup& get(std::string_view path, Fn&& fn) { return handle("GET", path, std::forward<Fn>(fn)); }

    template <typename Fn>
    RouteGroup& post(std::string_view path, Fn&& fn) { return handle("POST", path, std::forward<Fn>(fn)); }

    template <typename Fn>
    RouteGroup& put(std::string_view path, Fn&& fn) { return handle("PUT", path, std::forward<Fn>(fn)); }

    template <typename Fn>
    RouteGroup& del(std::string_view path, Fn&& fn) { return handle("DELETE", path, std::forward<Fn>(fn)); }

    template <typename Fn>
    RouteGroup& patch(std::string_view path, Fn&& fn) { return handle("PATCH", path, std::forward<Fn>(fn)); }

    template <typename Fn>
    RouteGroup& head(std::string_view path, Fn&& fn) { return handle("HEAD", path, std::forward<Fn>(fn)); }

    template <typename Fn>
    RouteGroup& options(std::string_view path, Fn&& fn) { return handle("OPTIONS", path, std::forward<Fn>(fn)); }
};

/**
 * @class Router
 * @brief Gin-style HTTP router with segment trie matching, route groups, global/group middleware,
 *        automatic `404`/`405` handling, and per-request `ServiceScope` DI execution.
 */
class Router {
    friend class RouteGroup;

    std::unordered_map<std::string, std::unique_ptr<detail::TrieNode>> trees_;
    std::vector<HandlerFunc> global_middlewares_;
    HandlerFunc no_route_handler_;

    void add_route(std::string_view method,
                   std::string_view full_path,
                   const std::vector<HandlerFunc>& group_mws,
                   HandlerFunc endpoint) {
        auto& root = trees_[std::string(method)];
        if (!root) {
            root = std::make_unique<detail::TrieNode>();
        }

        auto segs = detail::split_path_segments(full_path);
        detail::TrieNode* cur = root.get();

        for (std::size_t i = 0; i < segs.size(); ++i) {
            std::string_view seg = segs[i];
            if (!seg.empty() && seg.front() == '*') {
                if (i + 1 != segs.size()) {
                    throw std::invalid_argument(
                        "Catch-all wildcard '" + std::string(seg) +
                        "' must be the final segment in route: " + std::string(full_path)
                    );
                }
                std::string name(seg.substr(1));
                if (name.empty()) {
                    throw std::invalid_argument("Wildcard segment requires a name in route: " + std::string(full_path));
                }
                if (!cur->wildcard_child) {
                    cur->wildcard_name = std::move(name);
                    cur->wildcard_child = std::make_unique<detail::TrieNode>();
                } else if (cur->wildcard_name != name) {
                    throw std::invalid_argument("Conflicting wildcard parameter name in route: " + std::string(full_path));
                }
                cur = cur->wildcard_child.get();
            } else if (!seg.empty() && seg.front() == ':') {
                std::string name(seg.substr(1));
                if (name.empty()) {
                    throw std::invalid_argument("Parameter segment requires a name in route: " + std::string(full_path));
                }
                if (!cur->param_child) {
                    cur->param_name = std::move(name);
                    cur->param_child = std::make_unique<detail::TrieNode>();
                } else if (cur->param_name != name) {
                    throw std::invalid_argument(
                        "Conflicting param name ':" + name + "' vs ':" + cur->param_name +
                        "' in route: " + std::string(full_path)
                    );
                }
                cur = cur->param_child.get();
            } else {
                auto& next = cur->static_children[std::string(seg)];
                if (!next) {
                    next = std::make_unique<detail::TrieNode>();
                }
                cur = next.get();
            }
        }

        std::vector<HandlerFunc> chain;
        chain.reserve(group_mws.size() + 1);
        for (const auto& mw : group_mws) {
            chain.push_back(mw);
        }
        chain.push_back(std::move(endpoint));

        cur->has_handler = true;
        cur->chain = std::move(chain);
    }

public:
    Router() = default;

    /// Appends a global middleware executed for every route (including 404/405).
    Router& use(HandlerFunc mw) {
        global_middlewares_.push_back(std::move(mw));
        return *this;
    }

    /// Sets a custom 404 Not Found handler.
    Router& no_route(HandlerFunc handler) {
        no_route_handler_ = std::move(handler);
        return *this;
    }

    /// Creates a top-level `RouteGroup` (e.g. `router.group("/api/v1")`).
    [[nodiscard]] RouteGroup group(std::string_view prefix) {
        return RouteGroup(*this, detail::join_paths("", prefix), {});
    }

    template <typename Fn>
    Router& handle(std::string_view method, std::string_view path, Fn&& fn) {
        add_route(method, path, {}, detail::adapt_handler(std::forward<Fn>(fn)));
        return *this;
    }

    template <typename Fn>
    Router& get(std::string_view path, Fn&& fn) { return handle("GET", path, std::forward<Fn>(fn)); }

    template <typename Fn>
    Router& post(std::string_view path, Fn&& fn) { return handle("POST", path, std::forward<Fn>(fn)); }

    template <typename Fn>
    Router& put(std::string_view path, Fn&& fn) { return handle("PUT", path, std::forward<Fn>(fn)); }

    template <typename Fn>
    Router& del(std::string_view path, Fn&& fn) { return handle("DELETE", path, std::forward<Fn>(fn)); }

    template <typename Fn>
    Router& patch(std::string_view path, Fn&& fn) { return handle("PATCH", path, std::forward<Fn>(fn)); }

    template <typename Fn>
    Router& head(std::string_view path, Fn&& fn) { return handle("HEAD", path, std::forward<Fn>(fn)); }

    template <typename Fn>
    Router& options(std::string_view path, Fn&& fn) { return handle("OPTIONS", path, std::forward<Fn>(fn)); }

    /**
     * @brief Dispatches a single `HttpRequest` through global middleware, route middleware, and handler,
     *        creating an automatic per-request DI `ServiceScope` if `provider` is provided.
     */
    [[nodiscard]] http::HttpResponse dispatch(const http::HttpRequest& req,
                                              gorun::Context go_ctx = gorun::background(),
                                              ServiceProvider* provider = nullptr) const {
        std::optional<ServiceScope> scope_storage;
        ServiceScope* scope_ptr = nullptr;
        if (provider) {
            scope_storage.emplace(*provider);
            scope_ptr = &(*scope_storage);
        }

        HttpContext ctx(req, std::move(go_ctx), scope_ptr);
        auto segs = detail::split_path_segments(req.path);

        detail::RouteMatch match;
        if (auto it = trees_.find(req.method); it != trees_.end()) {
            detail::match_trie(it->second.get(), segs, 0, match);
        }

        if (match.matched && match.chain) {
            ctx.set_params(std::move(match.params));
            ctx.set_handler_views(&global_middlewares_, match.chain);
        } else {
            // Check whether another HTTP method matches this path -> 405 Method Not Allowed
            std::vector<std::string> allowed_methods;
            for (const auto& [m, root] : trees_) {
                if (m == req.method) continue;
                detail::RouteMatch probe;
                if (detail::match_trie(root.get(), segs, 0, probe)) {
                    allowed_methods.push_back(m);
                }
            }

            if (!allowed_methods.empty()) {
                std::sort(allowed_methods.begin(), allowed_methods.end());
                std::string allow_hdr;
                for (std::size_t i = 0; i < allowed_methods.size(); ++i) {
                    if (i > 0) allow_hdr += ", ";
                    allow_hdr += allowed_methods[i];
                }
                ctx.set_handler_with_fallback(&global_middlewares_, [allow = std::move(allow_hdr)](HttpContext& c) {
                    c.set_header("Allow", allow);
                    c.error(405, "method_not_allowed");
                });
            } else if (no_route_handler_) {
                ctx.set_handler_with_fallback(&global_middlewares_, no_route_handler_);
            } else {
                ctx.set_handler_with_fallback(&global_middlewares_, [](HttpContext& c) {
                    c.error(404, "not_found");
                });
            }
        }

        ctx.next();

        return std::move(ctx.response());
    }

    /// Adapts this router into a `goforge::http::HttpHandler` suitable for `HttpServer`.
    [[nodiscard]] std::function<http::HttpResponse(const http::HttpRequest&, gorun::Context)>
    to_http_handler(ServiceProvider* provider = nullptr) const {
        return [this, provider](const http::HttpRequest& req, gorun::Context ctx) {
            return dispatch(req, std::move(ctx), provider);
        };
    }
};

template <typename Fn>
inline RouteGroup& RouteGroup::handle(std::string_view method, std::string_view path, Fn&& fn) {
    router_->add_route(
        method,
        detail::join_paths(prefix_, path),
        middlewares_,
        detail::adapt_handler(std::forward<Fn>(fn))
    );
    return *this;
}

} // namespace goforge::web
// ---- end goforge/web/router.hpp ----
// ---- begin goforge/web/middleware.hpp ----

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
// ---- end goforge/web/middleware.hpp ----
// ---- begin goforge/obs/logger.hpp ----

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
// ---- end goforge/obs/logger.hpp ----
// ---- begin goforge/obs/metrics.hpp ----

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

    std::array<std::atomic<std::uint64_t>, kWindowSize> latencies_{};
    std::atomic<std::uint64_t> lat_writes_{0};

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

    /// 100% lock-free hot-path metric recording (zero mutex contention across all gorun workers!).
    void record(int status_code, std::uint64_t latency_us) noexcept {
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

        const std::uint64_t slot = lat_writes_.fetch_add(1, std::memory_order_relaxed) & (kWindowSize - 1);
        latencies_[static_cast<std::size_t>(slot)].store(latency_us, std::memory_order_relaxed);
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

        const std::uint64_t writes = lat_writes_.load(std::memory_order_relaxed);
        const std::size_t count = static_cast<std::size_t>(std::min<std::uint64_t>(writes, kWindowSize));
        std::vector<std::uint64_t> copy;
        copy.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            copy.push_back(latencies_[i].load(std::memory_order_relaxed));
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
            c.json(200, H{{"status", "alive"}});
        });

        router.get("/readyz", [this](web::HttpContext& c) {
            auto [ok, body] = evaluate_readiness();
            c.json_raw(ok ? 200 : 503, std::move(body));
        });

        router.get("/metrics", [&metrics](web::HttpContext& c) {
            if (c.query_or("format") == "prometheus") {
                c.text(200, metrics.to_prometheus());
            } else {
                c.json_raw(200, metrics.to_json());
            }
        });
    }
};

} // namespace goforge::obs
// ---- end goforge/obs/metrics.hpp ----

#if !defined(GORUN_DISABLE_NET)
// ---- begin goforge/http/http_server.hpp ----

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
// ---- end goforge/http/http_server.hpp ----
#endif

// ---- begin goforge/web_app.hpp ----

/**
 * @file web_app.hpp
 * @brief Lightweight, zero-boilerplate `WebApp` facade over `AppBuilder`, `Router`, `Lifecycle`,
 *        Observability, and `HttpServer`, with fluent route groups and zero-lambda `ControllerGroup`.
 */

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>







#if !defined(GORUN_DISABLE_NET)

#endif

namespace goforge {

/**
 * @struct WebAppConfig
 * @brief Unified configuration for `goforge::WebApp` (HTTP server + App lifecycle + BlockingPool).
 */
struct WebAppConfig {
    std::string host{"127.0.0.1"};
    std::uint16_t port{8080};
    std::size_t io_threads{2};
    std::size_t max_connections{10000};
    std::size_t blocking_pool_threads{4};
    std::chrono::milliseconds read_timeout{std::chrono::seconds(15)};
    std::chrono::milliseconds write_timeout{std::chrono::seconds(15)};
    std::chrono::milliseconds start_timeout{std::chrono::seconds(15)};
    std::chrono::milliseconds stop_timeout{std::chrono::seconds(15)};
};

class WebApp;

/**
 * @class ControllerGroup
 * @brief Zero-lambda fluent route binder for a DI-managed controller class `Controller`.
 *
 * Example:
 * ```cpp
 * app.controller<UserController, IUserRepo>("/api/v1/users")
 *    .post("", &UserController::create)
 *    .get("/:id", &UserController::get_by_id);
 * ```
 */
template <typename Controller>
class ControllerGroup {
    web::RouteGroup group_;

public:
    explicit ControllerGroup(web::RouteGroup group) : group_(std::move(group)) {}

    ControllerGroup& use(web::HandlerFunc mw) {
        group_.use(std::move(mw));
        return *this;
    }

    ControllerGroup& timeout(std::chrono::milliseconds dur) {
        group_.use(web::middleware::timeout(dur));
        return *this;
    }

    ControllerGroup& rate_limit(std::size_t max_concurrent) {
        group_.use(web::middleware::rate_limit(max_concurrent));
        return *this;
    }

    template <typename MethodPtr>
        requires std::is_member_function_pointer_v<std::decay_t<MethodPtr>>
    ControllerGroup& handle(std::string_view method, std::string_view path, MethodPtr fn) {
        using Traits = detail::FunctionTraits<std::decay_t<MethodPtr>>;
        static_assert(
            std::is_base_of_v<typename Traits::class_type, Controller>,
            "Member function pointer must belong to this Controller class."
        );
        group_.handle(method, path, fn);
        return *this;
    }

    template <typename MethodPtr>
    ControllerGroup& get(std::string_view path, MethodPtr fn) { return handle("GET", path, fn); }

    template <typename MethodPtr>
    ControllerGroup& post(std::string_view path, MethodPtr fn) { return handle("POST", path, fn); }

    template <typename MethodPtr>
    ControllerGroup& put(std::string_view path, MethodPtr fn) { return handle("PUT", path, fn); }

    template <typename MethodPtr>
    ControllerGroup& del(std::string_view path, MethodPtr fn) { return handle("DELETE", path, fn); }

    template <typename MethodPtr>
    ControllerGroup& patch(std::string_view path, MethodPtr fn) { return handle("PATCH", path, fn); }
};

/**
 * @class WebGroup
 * @brief Fluent wrapper over `web::RouteGroup` adding middleware shortcuts and nested `.controller<C>()`.
 */
class WebGroup {
    std::shared_ptr<void> app_state_;
    WebApp* app_owner_{nullptr};
    web::RouteGroup group_;

    friend class WebApp;
    WebGroup(std::shared_ptr<void> app_state, WebApp* owner, web::RouteGroup group)
        : app_state_(std::move(app_state)), app_owner_(owner), group_(std::move(group)) {}

public:
    WebGroup& use(web::HandlerFunc mw) {
        group_.use(std::move(mw));
        return *this;
    }

    WebGroup& timeout(std::chrono::milliseconds dur) {
        group_.use(web::middleware::timeout(dur));
        return *this;
    }

    WebGroup& rate_limit(std::size_t max_concurrent) {
        group_.use(web::middleware::rate_limit(max_concurrent));
        return *this;
    }

    template <typename Validator>
    WebGroup& bearer_auth(Validator&& validator) {
        group_.use(web::middleware::bearer_auth(std::forward<Validator>(validator)));
        return *this;
    }

    [[nodiscard]] WebGroup group(std::string_view sub_prefix) {
        return WebGroup(app_state_, app_owner_, group_.group(sub_prefix));
    }

    template <typename Controller, typename... Deps>
    [[nodiscard]] ControllerGroup<Controller> controller(std::string_view sub_prefix = "");

    template <typename Fn>
    WebGroup& handle(std::string_view method, std::string_view path, Fn&& fn) {
        group_.handle(method, path, std::forward<Fn>(fn));
        return *this;
    }

    template <typename Fn>
    WebGroup& get(std::string_view path, Fn&& fn) { return handle("GET", path, std::forward<Fn>(fn)); }

    template <typename Fn>
    WebGroup& post(std::string_view path, Fn&& fn) { return handle("POST", path, std::forward<Fn>(fn)); }

    template <typename Fn>
    WebGroup& put(std::string_view path, Fn&& fn) { return handle("PUT", path, std::forward<Fn>(fn)); }

    template <typename Fn>
    WebGroup& del(std::string_view path, Fn&& fn) { return handle("DELETE", path, std::forward<Fn>(fn)); }

    template <typename Fn>
    WebGroup& patch(std::string_view path, Fn&& fn) { return handle("PATCH", path, std::forward<Fn>(fn)); }
};

/**
 * @class WebApp
 * @brief Lightweight ergonomic application handle unifying DI, Router, Observability,
 *        and `HttpServer` in ~10 lines of C++20. Uses shared handle semantics so fluent chaining
 *        like `auto app = WebApp::create({.port = 8080}).singleton<...>().use_defaults();` works seamlessly.
 */
class WebApp {
    struct State {
        WebAppConfig config;
        AppBuilder builder;
        std::shared_ptr<web::Router> router;
        std::shared_ptr<obs::StructuredLogger> logger;
        std::shared_ptr<obs::MetricsRegistry> metrics;
        std::shared_ptr<obs::HealthRegistry> health;
        std::unique_ptr<App> app;
        bool defaults_applied{false};

        explicit State(WebAppConfig cfg)
            : config(std::move(cfg))
            , router(std::make_shared<web::Router>())
            , logger(std::make_shared<obs::StructuredLogger>())
            , metrics(std::make_shared<obs::MetricsRegistry>())
            , health(std::make_shared<obs::HealthRegistry>()) {}

        void ensure_built() {
            if (app) return;

            builder.with_config(AppConfig{
                .start_timeout = config.start_timeout,
                .stop_timeout = config.stop_timeout,
                .blocking_pool_threads = config.blocking_pool_threads
            });

            builder.provide_instance(router);
            builder.provide_instance(logger);
            builder.provide_instance(metrics);
            builder.provide_instance(health);

#if !defined(GORUN_DISABLE_NET)
            http::HttpServerConfig srv_cfg;
            srv_cfg.host = config.host;
            srv_cfg.port = config.port;
            srv_cfg.io_threads = config.io_threads;
            srv_cfg.max_connections = config.max_connections;
            srv_cfg.read_timeout = config.read_timeout;
            srv_cfg.write_timeout = config.write_timeout;

            builder.provide_instance(srv_cfg);
            builder.provide([](const http::HttpServerConfig& cfg,
                               web::Router& r,
                               ServiceProvider& sp,
                               Lifecycle& lc) {
                auto srv = std::make_shared<http::HttpServer>(cfg, r.to_http_handler(&sp));
                srv->attach_lifecycle(lc, "http-server");
                return srv;
            });
            builder.invoke([](http::HttpServer&) {});
#endif

            app = builder.build();
        }
    };

    std::shared_ptr<State> state_;

    friend class WebGroup;

    template <typename Controller, typename... Deps>
    static void register_controller_in_state(State& st) {
        if constexpr (sizeof...(Deps) > 0) {
            st.builder.provide_scoped<Controller, Deps...>();
        } else if (!st.builder.services().template has<Controller>()) {
            static_assert(
                std::is_default_constructible_v<Controller>,
                "Controller with constructor dependencies must either be registered via "
                "app.scoped<MyController, Dep1, ...>() or app.controller<MyController, Dep1, ...>(prefix)."
            );
            st.builder.provide_scoped<Controller>();
        }
    }

public:
    explicit WebApp(WebAppConfig cfg = {})
        : state_(std::make_shared<State>(std::move(cfg))) {}

    WebApp(const WebApp&) = default;
    WebApp& operator=(const WebApp&) = default;
    WebApp(WebApp&&) noexcept = default;
    WebApp& operator=(WebApp&&) noexcept = default;

    /// Factory entry point: `auto app = goforge::WebApp::create({.port = 8080});`
    [[nodiscard]] static WebApp create(WebAppConfig cfg = {}) {
        return WebApp(std::move(cfg));
    }

    // ---- Concise DI Registration (auto-detects Interface -> Impl bindings!) ----

    template <typename T>
    WebApp& singleton() {
        state_->builder.provide_singleton<T>();
        return *this;
    }

    template <typename T1, typename T2, typename... Rest>
    WebApp& singleton() {
        if constexpr (!std::is_same_v<T1, T2> && std::is_base_of_v<T1, T2>) {
            state_->builder.provide_singleton_as<T1, T2, Rest...>();
        } else {
            state_->builder.provide_singleton<T1, T2, Rest...>();
        }
        return *this;
    }

    template <typename T>
    WebApp& scoped() {
        state_->builder.provide_scoped<T>();
        return *this;
    }

    template <typename T1, typename T2, typename... Rest>
    WebApp& scoped() {
        if constexpr (!std::is_same_v<T1, T2> && std::is_base_of_v<T1, T2>) {
            state_->builder.provide_scoped_as<T1, T2, Rest...>();
        } else {
            state_->builder.provide_scoped<T1, T2, Rest...>();
        }
        return *this;
    }

    template <typename T>
    WebApp& transient() {
        state_->builder.provide_transient<T>();
        return *this;
    }

    template <typename T1, typename T2, typename... Rest>
    WebApp& transient() {
        if constexpr (!std::is_same_v<T1, T2> && std::is_base_of_v<T1, T2>) {
            state_->builder.provide_transient_as<T1, T2, Rest...>();
        } else {
            state_->builder.provide_transient<T1, T2, Rest...>();
        }
        return *this;
    }

    template <typename Interface, typename Impl>
        requires std::is_base_of_v<Interface, Impl>
    WebApp& instance(std::shared_ptr<Impl> ptr) {
        state_->builder.services().template add_instance<Interface>(
            std::static_pointer_cast<Interface>(std::move(ptr))
        );
        return *this;
    }

    template <typename T>
    WebApp& instance(T&& value) {
        state_->builder.provide_instance(std::forward<T>(value));
        return *this;
    }

    template <typename Factory>
    WebApp& provide(Factory&& factory, Lifetime lifetime = Lifetime::Singleton) {
        state_->builder.provide(std::forward<Factory>(factory), lifetime);
        return *this;
    }

    template <typename Fn>
    WebApp& invoke(Fn&& fn) {
        state_->builder.invoke(std::forward<Fn>(fn));
        return *this;
    }

    // ---- Middleware & Built-in Observability ----

    WebApp& use(web::HandlerFunc mw) {
        state_->router->use(std::move(mw));
        return *this;
    }

    /// Installs metrics, panic recovery, JSON access logging, and mounts `/healthz`, `/readyz`, `/metrics`.
    WebApp& use_defaults(bool enable_access_log = true) {
        if (!state_->defaults_applied) {
            state_->defaults_applied = true;
            state_->router->use(state_->metrics->middleware());
            state_->router->use(web::middleware::recovery());
            if (enable_access_log) {
                state_->router->use(state_->logger->access_log_middleware());
            }
            state_->health->mount(*state_->router, *state_->metrics);
        }
        return *this;
    }

    WebApp& use_recovery() { return use(web::middleware::recovery()); }
    WebApp& use_metrics() { return use(state_->metrics->middleware()); }
    WebApp& use_access_log() { return use(state_->logger->access_log_middleware()); }
    WebApp& use_timeout(std::chrono::milliseconds dur) { return use(web::middleware::timeout(dur)); }
    WebApp& use_rate_limit(std::size_t max_concurrent) { return use(web::middleware::rate_limit(max_concurrent)); }

    // ---- Direct Routes, Groups & Controllers ----

    [[nodiscard]] WebGroup group(std::string_view prefix) {
        return WebGroup(state_, this, state_->router->group(prefix));
    }

    template <typename Controller, typename... Deps>
    [[nodiscard]] ControllerGroup<Controller> controller(std::string_view prefix = "") {
        register_controller_in_state<Controller, Deps...>(*state_);
        return ControllerGroup<Controller>(state_->router->group(prefix));
    }

    template <typename Fn>
    WebApp& handle(std::string_view method, std::string_view path, Fn&& fn) {
        state_->router->handle(method, path, std::forward<Fn>(fn));
        return *this;
    }

    template <typename Fn>
    WebApp& get(std::string_view path, Fn&& fn) { return handle("GET", path, std::forward<Fn>(fn)); }

    template <typename Fn>
    WebApp& post(std::string_view path, Fn&& fn) { return handle("POST", path, std::forward<Fn>(fn)); }

    template <typename Fn>
    WebApp& put(std::string_view path, Fn&& fn) { return handle("PUT", path, std::forward<Fn>(fn)); }

    template <typename Fn>
    WebApp& del(std::string_view path, Fn&& fn) { return handle("DELETE", path, std::forward<Fn>(fn)); }

    template <typename Fn>
    WebApp& patch(std::string_view path, Fn&& fn) { return handle("PATCH", path, std::forward<Fn>(fn)); }

    // ---- Component Accessors ----
    [[nodiscard]] web::Router& router() noexcept { return *state_->router; }
    [[nodiscard]] obs::StructuredLogger& logger() noexcept { return *state_->logger; }
    [[nodiscard]] obs::MetricsRegistry& metrics() noexcept { return *state_->metrics; }
    [[nodiscard]] obs::HealthRegistry& health() noexcept { return *state_->health; }

    [[nodiscard]] App& app() {
        state_->ensure_built();
        return *state_->app;
    }

    // ---- Execution & In-Memory Dispatch (works with or without Asio!) ----

    void start() {
        state_->ensure_built();
        state_->app->start();
    }

    std::vector<std::string> stop() {
        if (!state_->app) return {};
        return state_->app->stop();
    }

    void run() {
        state_->ensure_built();
        state_->app->run_until_signal();
    }

    /// Dispatches a synthetic `HttpRequest` directly through the DI + Router pipeline (ideal for tests & playgrounds).
    [[nodiscard]] http::HttpResponse dispatch(const http::HttpRequest& req,
                                              gorun::Context go_ctx = gorun::background()) {
        state_->ensure_built();
        return state_->router->dispatch(req, std::move(go_ctx), &state_->app->services());
    }

#if !defined(GORUN_DISABLE_NET)
    [[nodiscard]] std::uint16_t bound_port() {
        state_->ensure_built();
        return state_->app->services().resolve<http::HttpServer>().port();
    }
#endif
};

template <typename Controller, typename... Deps>
inline ControllerGroup<Controller> WebGroup::controller(std::string_view sub_prefix) {
    auto st = std::static_pointer_cast<WebApp::State>(app_state_);
    WebApp::register_controller_in_state<Controller, Deps...>(*st);
    return ControllerGroup<Controller>(group_.group(sub_prefix));
}

} // namespace goforge
// ---- end goforge/web_app.hpp ----

// ---- begin goforge/data/sql_compiler.hpp ----

/**
 * @file sql_compiler.hpp
 * @brief Compile-time SQL dialect compiler translating `goforge::query` Expression Trees (AST)
 *        and `GOFORGE_MODEL` / `GOFORGE_MODEL_TABLE` reflection metadata into parameterized,
 *        SQL-injection-proof SQL (SQLite, PostgreSQL, MySQL) plus struct hydration.
 */

#include <concepts>
#include <cstdint>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>



namespace goforge::data {

/// Supported SQL dialects for identifier quoting, bind placeholders, DDL types, and LIKE semantics.
enum class SqlDialect : std::uint8_t {
    SQLite,
    Postgres,
    MySQL
};

/**
 * @struct SqlValue
 * @brief Strongly-typed SQL bind parameter / result cell value.
 */
struct SqlValue {
    using Storage = std::variant<std::monostate, std::int64_t, double, bool, std::string>;
    Storage data{std::monostate{}};

    SqlValue() = default;
    SqlValue(std::nullptr_t) noexcept : data(std::monostate{}) {}
    SqlValue(std::monostate) noexcept : data(std::monostate{}) {}
    SqlValue(bool v) noexcept : data(v) {}

    template <std::integral T>
        requires (!std::is_same_v<std::remove_cvref_t<T>, bool>)
    SqlValue(T v) noexcept : data(static_cast<std::int64_t>(v)) {}

    template <std::floating_point T>
    SqlValue(T v) noexcept : data(static_cast<double>(v)) {}

    SqlValue(std::string v) noexcept : data(std::move(v)) {}
    SqlValue(std::string_view v) : data(std::string(v)) {}
    SqlValue(const char* v) : data(v ? Storage(std::string(v)) : Storage(std::monostate{})) {}

    template <typename T>
    SqlValue(const std::optional<T>& opt) {
        if (opt.has_value()) {
            *this = SqlValue(*opt);
        } else {
            data = std::monostate{};
        }
    }

    [[nodiscard]] bool is_null() const noexcept { return std::holds_alternative<std::monostate>(data); }

    [[nodiscard]] std::int64_t as_int64() const {
        if (const auto* p = std::get_if<std::int64_t>(&data)) return *p;
        if (const auto* b = std::get_if<bool>(&data)) return *b ? 1 : 0;
        if (const auto* d = std::get_if<double>(&data)) return static_cast<std::int64_t>(*d);
        if (const auto* s = std::get_if<std::string>(&data)) return std::stoll(*s);
        throw std::runtime_error("SqlValue is NULL (expected integer)");
    }

    [[nodiscard]] double as_double() const {
        if (const auto* d = std::get_if<double>(&data)) return *d;
        if (const auto* p = std::get_if<std::int64_t>(&data)) return static_cast<double>(*p);
        if (const auto* s = std::get_if<std::string>(&data)) return std::stod(*s);
        throw std::runtime_error("SqlValue is NULL (expected double)");
    }

    [[nodiscard]] bool as_bool() const {
        if (const auto* b = std::get_if<bool>(&data)) return *b;
        if (const auto* p = std::get_if<std::int64_t>(&data)) return *p != 0;
        if (const auto* s = std::get_if<std::string>(&data)) return *s == "1" || *s == "true" || *s == "t";
        throw std::runtime_error("SqlValue is NULL (expected bool)");
    }

    [[nodiscard]] std::string as_string() const {
        if (const auto* s = std::get_if<std::string>(&data)) return *s;
        if (const auto* p = std::get_if<std::int64_t>(&data)) return std::to_string(*p);
        if (const auto* d = std::get_if<double>(&data)) return std::to_string(*d);
        if (const auto* b = std::get_if<bool>(&data)) return *b ? "true" : "false";
        return "";
    }

    [[nodiscard]] std::string debug_string() const {
        if (is_null()) return "NULL";
        if (const auto* s = std::get_if<std::string>(&data)) return "'" + *s + "'";
        if (const auto* b = std::get_if<bool>(&data)) return *b ? "true" : "false";
        if (const auto* p = std::get_if<std::int64_t>(&data)) return std::to_string(*p);
        if (const auto* d = std::get_if<double>(&data)) {
            std::ostringstream oss;
            oss << *d;
            return oss.str();
        }
        return "NULL";
    }
};

/**
 * @struct SqlRow
 * @brief Result row returned by `IDbDriver::query()`.
 */
struct SqlRow {
    std::vector<std::pair<std::string, SqlValue>> columns;

    [[nodiscard]] const SqlValue* find(std::string_view name) const noexcept {
        for (const auto& [k, v] : columns) {
            if (k == name) return &v;
        }
        return nullptr;
    }

    [[nodiscard]] const SqlValue& at(std::size_t idx) const {
        return columns.at(idx).second;
    }

    [[nodiscard]] std::size_t size() const noexcept { return columns.size(); }
    [[nodiscard]] bool empty() const noexcept { return columns.empty(); }
};

/**
 * @struct CompiledQuery
 * @brief Parameterized SQL query + bound parameter vector produced by `SqlCompiler` / `DbSet<T>`.
 */
struct CompiledQuery {
    std::string sql;
    std::vector<SqlValue> params;
};

namespace detail {

template <typename T>
struct IsOpt : std::false_type {};

template <typename T>
struct IsOpt<std::optional<T>> : std::true_type {
    using inner_type = T;
};

template <typename T>
constexpr bool is_sql_primitive_v =
    std::is_same_v<T, bool> ||
    std::is_integral_v<T> ||
    std::is_floating_point_v<T> ||
    std::is_same_v<T, std::string>;

template <typename T>
constexpr bool is_sql_scalar_v = []() {
    using Clean = std::remove_cvref_t<T>;
    if constexpr (is_sql_primitive_v<Clean>) {
        return true;
    } else if constexpr (IsOpt<Clean>::value) {
        return is_sql_primitive_v<typename IsOpt<Clean>::inner_type>;
    } else {
        return false;
    }
}();

inline std::string quote_ident(std::string_view name, SqlDialect dialect) {
    if (dialect == SqlDialect::MySQL) {
        return "`" + std::string(name) + "`";
    }
    return "\"" + std::string(name) + "\"";
}

inline std::string placeholder(std::size_t one_based_index, SqlDialect dialect) {
    switch (dialect) {
        case SqlDialect::SQLite:   return "?" + std::to_string(one_based_index);
        case SqlDialect::Postgres: return "$" + std::to_string(one_based_index);
        case SqlDialect::MySQL:    return "?";
    }
    return "?";
}

inline std::string escape_like_pattern(std::string_view raw) {
    std::string out;
    out.reserve(raw.size() + 4);
    for (char c : raw) {
        if (c == '%' || c == '_' || c == '\\') {
            out.push_back('\\');
        }
        out.push_back(c);
    }
    return out;
}

template <typename MemberT>
inline std::string sql_column_ddl(std::string_view col_name, SqlDialect dialect) {
    using Clean = std::remove_cvref_t<MemberT>;
    constexpr bool nullable = IsOpt<Clean>::value;
    using BaseT = typename std::conditional_t<nullable, IsOpt<Clean>, std::type_identity<Clean>>::type;
    // Note: when nullable is true, extract inner_type cleanly below:
    auto base_type_sql = [&]() -> std::string {
        if constexpr (nullable) {
            using Inner = typename IsOpt<Clean>::inner_type;
            if constexpr (std::is_same_v<Inner, bool>) {
                return dialect == SqlDialect::Postgres ? "BOOLEAN" : "INTEGER";
            } else if constexpr (std::is_integral_v<Inner>) {
                return dialect == SqlDialect::SQLite ? "INTEGER" : "BIGINT";
            } else if constexpr (std::is_floating_point_v<Inner>) {
                return dialect == SqlDialect::Postgres ? "DOUBLE PRECISION" : "REAL";
            } else {
                return "TEXT";
            }
        } else {
            if constexpr (std::is_same_v<Clean, bool>) {
                return dialect == SqlDialect::Postgres ? "BOOLEAN" : "INTEGER";
            } else if constexpr (std::is_integral_v<Clean>) {
                return dialect == SqlDialect::SQLite ? "INTEGER" : "BIGINT";
            } else if constexpr (std::is_floating_point_v<Clean>) {
                return dialect == SqlDialect::Postgres ? "DOUBLE PRECISION" : "REAL";
            } else {
                return "TEXT";
            }
        }
    };
    (void)sizeof(BaseT);

    std::string q = quote_ident(col_name, dialect);
    if (col_name == "id") {
        switch (dialect) {
            case SqlDialect::SQLite:   return q + " INTEGER PRIMARY KEY AUTOINCREMENT";
            case SqlDialect::Postgres: return q + " BIGSERIAL PRIMARY KEY";
            case SqlDialect::MySQL:    return q + " BIGINT AUTO_INCREMENT PRIMARY KEY";
        }
    }
    return q + " " + base_type_sql() + (nullable ? "" : " NOT NULL");
}

template <typename T>
inline void assign_from_sql_value(T& dst, const SqlValue& v) {
    using Clean = std::remove_cvref_t<T>;
    if constexpr (IsOpt<Clean>::value) {
        if (v.is_null()) {
            dst = std::nullopt;
        } else {
            typename IsOpt<Clean>::inner_type inner{};
            assign_from_sql_value(inner, v);
            dst = std::move(inner);
        }
    } else if constexpr (std::is_same_v<Clean, bool>) {
        if (!v.is_null()) dst = v.as_bool();
    } else if constexpr (std::is_integral_v<Clean>) {
        if (!v.is_null()) dst = static_cast<Clean>(v.as_int64());
    } else if constexpr (std::is_floating_point_v<Clean>) {
        if (!v.is_null()) dst = static_cast<Clean>(v.as_double());
    } else if constexpr (std::is_same_v<Clean, std::string>) {
        if (!v.is_null()) dst = v.as_string();
    }
}

// ---- AST Node Type Detection Helpers ----
template <typename T> struct IsColExpr : std::false_type {};
template <typename Obj, typename M> struct IsColExpr<query::ColExpr<Obj, M>> : std::true_type {};

template <typename T> struct IsLiteralExpr : std::false_type {};
template <typename V> struct IsLiteralExpr<query::LiteralExpr<V>> : std::true_type {};

template <typename T> struct IsCmpExpr : std::false_type {};
template <query::CmpOp Op, typename L, typename R>
struct IsCmpExpr<query::CmpExpr<Op, L, R>> : std::true_type {};

template <typename T> struct IsLogicExpr : std::false_type {};
template <query::LogicOp Op, typename L, typename R>
struct IsLogicExpr<query::LogicExpr<Op, L, R>> : std::true_type {};

template <typename T> struct IsNotExpr : std::false_type {};
template <typename Inner>
struct IsNotExpr<query::NotExpr<Inner>> : std::true_type {};

template <typename T> struct IsArithExpr : std::false_type {};
template <query::ArithOp Op, typename L, typename R>
struct IsArithExpr<query::ArithExpr<Op, L, R>> : std::true_type {};

template <typename T> struct IsStrExpr : std::false_type {};
template <query::StrOp Op, typename Target>
struct IsStrExpr<query::StrExpr<Op, Target>> : std::true_type {};

template <typename T> struct IsInExpr : std::false_type {};
template <typename Target, typename ValT, bool Neg>
struct IsInExpr<query::InExpr<Target, ValT, Neg>> : std::true_type {};

template <typename T> struct IsBetweenExpr : std::false_type {};
template <typename Target, typename Lo, typename Hi>
struct IsBetweenExpr<query::BetweenExpr<Target, Lo, Hi>> : std::true_type {};

template <typename T> struct IsNullCheckExpr : std::false_type {};
template <typename Target, bool ExpectNull>
struct IsNullCheckExpr<query::NullCheckExpr<Target, ExpectNull>> : std::true_type {};

} // namespace detail

/**
 * @struct SqlCompiler
 * @brief Compiles `goforge::query` AST expressions and `EntityMeta<T>` models into parameterized SQL.
 */
struct SqlCompiler {
    /// Compiles any `goforge::query` AST node into SQL fragment + appended bind parameters.
    template <typename Expr>
    static std::string compile_expr(const Expr& expr,
                                    std::vector<SqlValue>& params,
                                    SqlDialect dialect) {
        using E = std::remove_cvref_t<Expr>;

        if constexpr (detail::IsColExpr<E>::value) {
            if (expr.name.empty()) {
                throw std::invalid_argument(
                    "ColExpr has empty column name. Declare model with GOFORGE_MODEL / GOFORGE_MODEL_TABLE "
                    "or pass explicit name: col(&Type::field, \"column_name\")."
                );
            }
            return detail::quote_ident(expr.name, dialect);
        } else if constexpr (detail::IsLiteralExpr<E>::value) {
            params.emplace_back(expr.value);
            return detail::placeholder(params.size(), dialect);
        } else if constexpr (detail::IsCmpExpr<E>::value) {
            std::string l = compile_expr(expr.lhs, params, dialect);
            std::string r = compile_expr(expr.rhs, params, dialect);
            const char* sym = "=";
            switch (E::op) {
                case query::CmpOp::Eq: sym = "="; break;
                case query::CmpOp::Ne: sym = "<>"; break;
                case query::CmpOp::Lt: sym = "<"; break;
                case query::CmpOp::Le: sym = "<="; break;
                case query::CmpOp::Gt: sym = ">"; break;
                case query::CmpOp::Ge: sym = ">="; break;
            }
            return l + " " + sym + " " + r;
        } else if constexpr (detail::IsLogicExpr<E>::value) {
            std::string l = compile_expr(expr.lhs, params, dialect);
            std::string r = compile_expr(expr.rhs, params, dialect);
            const char* sym = (E::op == query::LogicOp::And) ? " AND " : " OR ";
            return "(" + l + sym + r + ")";
        } else if constexpr (detail::IsNotExpr<E>::value) {
            std::string inner = compile_expr(expr.inner, params, dialect);
            return "(NOT (" + inner + "))";
        } else if constexpr (detail::IsArithExpr<E>::value) {
            std::string l = compile_expr(expr.lhs, params, dialect);
            std::string r = compile_expr(expr.rhs, params, dialect);
            const char* sym = "+";
            switch (E::op) {
                case query::ArithOp::Add: sym = "+"; break;
                case query::ArithOp::Sub: sym = "-"; break;
                case query::ArithOp::Mul: sym = "*"; break;
                case query::ArithOp::Div: sym = "/"; break;
            }
            return "(" + l + " " + sym + " " + r + ")";
        } else if constexpr (detail::IsStrExpr<E>::value) {
            std::string target = compile_expr(expr.target, params, dialect);
            std::string escaped = detail::escape_like_pattern(expr.pattern);
            std::string pat;
            if constexpr (E::op == query::StrOp::Contains || E::op == query::StrOp::IContains) {
                pat = "%" + escaped + "%";
            } else if constexpr (E::op == query::StrOp::StartsWith) {
                pat = escaped + "%";
            } else {
                pat = "%" + escaped;
            }
            params.emplace_back(std::move(pat));
            std::string ph = detail::placeholder(params.size(), dialect);
            if constexpr (E::op == query::StrOp::IContains) {
                if (dialect == SqlDialect::Postgres) {
                    return target + " ILIKE " + ph + " ESCAPE '\\'";
                }
                return "LOWER(" + target + ") LIKE LOWER(" + ph + ") ESCAPE '\\'";
            } else {
                return target + " LIKE " + ph + " ESCAPE '\\'";
            }
        } else if constexpr (detail::IsInExpr<E>::value) {
            if (expr.values.empty()) {
                return E::negated ? "1 = 1" : "1 = 0";
            }
            std::string target = compile_expr(expr.target, params, dialect);
            std::string out = target + (E::negated ? " NOT IN (" : " IN (");
            for (std::size_t i = 0; i < expr.values.size(); ++i) {
                if (i > 0) out += ", ";
                params.emplace_back(expr.values[i]);
                out += detail::placeholder(params.size(), dialect);
            }
            out += ")";
            return out;
        } else if constexpr (detail::IsBetweenExpr<E>::value) {
            std::string target = compile_expr(expr.target, params, dialect);
            params.emplace_back(expr.lo);
            std::string lo_ph = detail::placeholder(params.size(), dialect);
            params.emplace_back(expr.hi);
            std::string hi_ph = detail::placeholder(params.size(), dialect);
            return target + " BETWEEN " + lo_ph + " AND " + hi_ph;
        } else if constexpr (detail::IsNullCheckExpr<E>::value) {
            std::string target = compile_expr(expr.target, params, dialect);
            return target + (E::expect_null ? " IS NULL" : " IS NOT NULL");
        } else {
            static_assert(detail::IsColExpr<E>::value,
                          "Unsupported expression in SQL query. Use goforge::query AST expressions "
                          "(col(&Model::field) == value, .contains(), .in(), .between(), .is_null(), &&, ||, !).");
            return "";
        }
    }

    /// Generates `CREATE TABLE IF NOT EXISTS "table" (...)` from `EntityMeta<T>`.
    template <typename T>
    static std::string build_create_table_sql(SqlDialect dialect) {
        static_assert(query::EntityMeta<T>::registered,
                      "Model must be registered with GOFORGE_MODEL or GOFORGE_MODEL_TABLE.");
        std::string sql = "CREATE TABLE IF NOT EXISTS " +
                          detail::quote_ident(query::EntityMeta<T>::table_name, dialect) + " (";
        bool first = true;
        std::apply([&](const auto&... f) {
            (([&] {
                using FieldT = std::remove_cvref_t<decltype(std::declval<const T&>().*(f.ptr))>;
                if constexpr (detail::is_sql_scalar_v<FieldT>) {
                    if (!first) sql += ", ";
                    first = false;
                    sql += detail::sql_column_ddl<FieldT>(f.name, dialect);
                }
            }()), ...);
        }, query::EntityMeta<T>::fields());
        sql += ")";
        return sql;
    }

    /// Generates quoted column list: `"id", "name", "email", ...`
    template <typename T>
    static std::string build_select_columns(SqlDialect dialect) {
        static_assert(query::EntityMeta<T>::registered,
                      "Model must be registered with GOFORGE_MODEL or GOFORGE_MODEL_TABLE.");
        std::string cols;
        bool first = true;
        std::apply([&](const auto&... f) {
            (([&] {
                using FieldT = std::remove_cvref_t<decltype(std::declval<const T&>().*(f.ptr))>;
                if constexpr (detail::is_sql_scalar_v<FieldT>) {
                    if (!first) cols += ", ";
                    first = false;
                    cols += detail::quote_ident(f.name, dialect);
                }
            }()), ...);
        }, query::EntityMeta<T>::fields());
        return cols;
    }

    /// Extracts the integer primary key (`id`) from `entity`, or `0` if absent.
    template <typename T>
    static std::int64_t get_entity_id(const T& entity) noexcept {
        static_assert(query::EntityMeta<T>::registered);
        std::int64_t result = 0;
        std::apply([&](const auto&... f) {
            (([&] {
                using FieldT = std::remove_cvref_t<decltype(entity.*(f.ptr))>;
                if constexpr (std::is_integral_v<FieldT> && !std::is_same_v<FieldT, bool>) {
                    if (f.name == "id") {
                        result = static_cast<std::int64_t>(entity.*(f.ptr));
                    }
                }
            }()), ...);
        }, query::EntityMeta<T>::fields());
        return result;
    }

    /// Sets the integer primary key (`id`) on `entity` (e.g. after AUTOINCREMENT `INSERT`).
    template <typename T>
    static void set_entity_id(T& entity, std::int64_t new_id) noexcept {
        static_assert(query::EntityMeta<T>::registered);
        std::apply([&](const auto&... f) {
            (([&] {
                using FieldT = std::remove_cvref_t<decltype(entity.*(f.ptr))>;
                if constexpr (std::is_integral_v<FieldT> && !std::is_same_v<FieldT, bool>) {
                    if (f.name == "id") {
                        entity.*(f.ptr) = static_cast<FieldT>(new_id);
                    }
                }
            }()), ...);
        }, query::EntityMeta<T>::fields());
    }

    /// Builds a parameterized `INSERT INTO "table" (...) VALUES (...)` query.
    /// Automatically omits `"id"` when `entity.id == 0` so AUTOINCREMENT assigns a new key.
    template <typename T>
    static CompiledQuery build_insert_sql(const T& entity, SqlDialect dialect) {
        static_assert(query::EntityMeta<T>::registered);
        CompiledQuery q;
        std::string cols;
        std::string vals;
        bool first = true;

        std::apply([&](const auto&... f) {
            (([&] {
                using FieldT = std::remove_cvref_t<decltype(entity.*(f.ptr))>;
                if constexpr (detail::is_sql_scalar_v<FieldT>) {
                    bool skip_auto_id = false;
                    if constexpr (std::is_integral_v<FieldT> && !std::is_same_v<FieldT, bool>) {
                        if (f.name == "id" && entity.*(f.ptr) == 0) {
                            skip_auto_id = true;
                        }
                    }
                    if (!skip_auto_id) {
                        if (!first) {
                            cols += ", ";
                            vals += ", ";
                        }
                        first = false;
                        cols += detail::quote_ident(f.name, dialect);
                        q.params.emplace_back(entity.*(f.ptr));
                        vals += detail::placeholder(q.params.size(), dialect);
                    }
                }
            }()), ...);
        }, query::EntityMeta<T>::fields());

        q.sql = "INSERT INTO " + detail::quote_ident(query::EntityMeta<T>::table_name, dialect) +
                " (" + cols + ") VALUES (" + vals + ")";
        if (dialect == SqlDialect::Postgres) {
            q.sql += " RETURNING " + detail::quote_ident("id", dialect);
        }
        return q;
    }

    /// Builds a parameterized `UPDATE "table" SET ... WHERE "id" = ?N` query.
    template <typename T>
    static CompiledQuery build_update_sql(const T& entity, SqlDialect dialect) {
        static_assert(query::EntityMeta<T>::registered);
        CompiledQuery q;
        std::string sets;
        bool first = true;

        std::apply([&](const auto&... f) {
            (([&] {
                using FieldT = std::remove_cvref_t<decltype(entity.*(f.ptr))>;
                if constexpr (detail::is_sql_scalar_v<FieldT>) {
                    if (f.name != "id") {
                        if (!first) sets += ", ";
                        first = false;
                        q.params.emplace_back(entity.*(f.ptr));
                        sets += detail::quote_ident(f.name, dialect) + " = " +
                                detail::placeholder(q.params.size(), dialect);
                    }
                }
            }()), ...);
        }, query::EntityMeta<T>::fields());

        q.params.emplace_back(get_entity_id(entity));
        q.sql = "UPDATE " + detail::quote_ident(query::EntityMeta<T>::table_name, dialect) +
                " SET " + sets + " WHERE " + detail::quote_ident("id", dialect) + " = " +
                detail::placeholder(q.params.size(), dialect);
        return q;
    }

    /// Builds a parameterized `DELETE FROM "table" WHERE "id" = ?1` query.
    template <typename T>
    static CompiledQuery build_delete_by_id_sql(std::int64_t id, SqlDialect dialect) {
        static_assert(query::EntityMeta<T>::registered);
        CompiledQuery q;
        q.params.emplace_back(id);
        q.sql = "DELETE FROM " + detail::quote_ident(query::EntityMeta<T>::table_name, dialect) +
                " WHERE " + detail::quote_ident("id", dialect) + " = " +
                detail::placeholder(1, dialect);
        return q;
    }

    /// Hydrates a C++ model struct `T` from a `SqlRow` using compile-time `EntityMeta<T>`.
    template <typename T>
    static T hydrate_entity(const SqlRow& row) {
        static_assert(query::EntityMeta<T>::registered);
        T entity{};
        std::apply([&](const auto&... f) {
            (([&] {
                using FieldT = std::remove_cvref_t<decltype(entity.*(f.ptr))>;
                if constexpr (detail::is_sql_scalar_v<FieldT>) {
                    if (const SqlValue* cell = row.find(f.name)) {
                        detail::assign_from_sql_value(entity.*(f.ptr), *cell);
                    }
                }
            }()), ...);
        }, query::EntityMeta<T>::fields());
        return entity;
    }
};

} // namespace goforge::data
// ---- end goforge/data/sql_compiler.hpp ----
// ---- begin goforge/data/driver.hpp ----

/**
 * @file driver.hpp
 * @brief Pluggable database driver interface (`IDbDriver`) and lightweight `RecordingDbDriver`
 *        for SQL inspection, unit tests, and header-only playgrounds.
 */

#include <cstdint>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>



namespace goforge::data {

/// Exception thrown on database connection, constraint, or SQL execution errors.
class DbError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// Result of executing a DML/DDL statement (`INSERT`, `UPDATE`, `DELETE`, `CREATE TABLE`).
struct ExecResult {
    std::uint64_t rows_affected{0};
    std::int64_t last_insert_id{0};
};

/**
 * @struct IDbDriver
 * @brief Backend-agnostic SQL driver contract (implemented by `SqliteDriver`, future `PostgresDriver`, etc.).
 */
struct IDbDriver {
    virtual ~IDbDriver() = default;

    [[nodiscard]] virtual SqlDialect dialect() const noexcept = 0;

    virtual ExecResult execute(const std::string& sql, const std::vector<SqlValue>& params = {}) = 0;

    virtual std::vector<SqlRow> query(const std::string& sql, const std::vector<SqlValue>& params = {}) = 0;

    virtual void begin_tx() { (void)execute("BEGIN TRANSACTION"); }
    virtual void commit_tx() { (void)execute("COMMIT"); }
    virtual void rollback_tx() { (void)execute("ROLLBACK"); }

    /// Executes `fn()` atomically inside a driver-serialized transaction (commit on success, rollback on throw).
    virtual void with_transaction(const std::function<void()>& fn) {
        begin_tx();
        try {
            fn();
            commit_tx();
        } catch (...) {
            try { rollback_tx(); } catch (...) {}
            throw;
        }
    }
};

/**
 * @class RecordingDbDriver
 * @brief Lightweight dry-run driver recording every executed SQL statement & bind parameters.
 *        Useful for dialect verification (`SQLite` vs `Postgres` vs `MySQL`), unit tests, and playgrounds.
 */
class RecordingDbDriver final : public IDbDriver {
    SqlDialect dialect_;
    mutable std::mutex mtx_;
    std::vector<CompiledQuery> history_;
    std::int64_t next_id_{1};

public:
    explicit RecordingDbDriver(SqlDialect d = SqlDialect::Postgres) noexcept : dialect_(d) {}

    [[nodiscard]] SqlDialect dialect() const noexcept override { return dialect_; }

    ExecResult execute(const std::string& sql, const std::vector<SqlValue>& params = {}) override {
        std::lock_guard lk(mtx_);
        history_.push_back(CompiledQuery{sql, params});
        return ExecResult{1, next_id_++};
    }

    std::vector<SqlRow> query(const std::string& sql, const std::vector<SqlValue>& params = {}) override {
        std::lock_guard lk(mtx_);
        history_.push_back(CompiledQuery{sql, params});
        return {};
    }

    [[nodiscard]] std::vector<CompiledQuery> history() const {
        std::lock_guard lk(mtx_);
        return history_;
    }

    void clear() {
        std::lock_guard lk(mtx_);
        history_.clear();
    }
};

} // namespace goforge::data
// ---- end goforge/data/driver.hpp ----
// ---- begin goforge/data/db_context.hpp ----

/**
 * @file db_context.hpp
 * @brief Entity Framework-style `DbContext`, fluent SQL-compiled `DbSet<T>`, Unit of Work
 *        (`add` / `update` / `remove` / atomic `save_changes()`), `ensure_created<Models...>()`,
 *        and versioned `Migration` runner — seamlessly offloading blocking DB I/O to `BlockingPool`
 *        so `gorun` M:N green-thread workers never stall.
 */

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>






namespace goforge::data {

/**
 * @struct Migration
 * @brief Versioned schema migration tracked in `_goforge_migrations`.
 */
struct Migration {
    std::string version;
    std::string name;
    std::vector<std::string> up_sql;
};

class DbContext;

/**
 * @class DbSet
 * @brief EF Core-style fluent query & repository handle for entity type `T`.
 *
 * Translates the exact same `goforge::query` compile-time AST (`col(&User::age) >= 18 && ...`)
 * into a single parameterized SQL query executed on `IDbDriver` (via `BlockingPool` when attached).
 */
template <typename T>
class DbSet {
    static_assert(query::EntityMeta<T>::registered,
                  "DbSet<T> requires T to be declared with GOFORGE_MODEL or GOFORGE_MODEL_TABLE.");

    DbContext* ctx_;
    std::vector<std::function<std::string(std::vector<SqlValue>&, SqlDialect)>> where_builders_;
    std::vector<std::pair<std::string, bool>> order_clauses_; // {col_name, ascending}
    std::optional<std::size_t> limit_;
    std::optional<std::size_t> offset_;

    template <typename KeySelector>
    static std::string resolve_column_name(const KeySelector& key) {
        using K = std::remove_cvref_t<KeySelector>;
        if constexpr (detail::IsColExpr<K>::value) {
            if (key.name.empty()) {
                throw std::invalid_argument("ColExpr column name is empty.");
            }
            return std::string(key.name);
        } else if constexpr (std::is_member_object_pointer_v<K>) {
            std::string_view name = query::column_name_of(key);
            if (name.empty()) {
                throw std::invalid_argument(
                    "Member pointer not found in EntityMeta<T>. Ensure field is listed in GOFORGE_MODEL."
                );
            }
            return std::string(name);
        } else {
            static_assert(std::is_member_object_pointer_v<K>,
                          "order_by / then_by expects &Model::field or col(&Model::field).");
            return "";
        }
    }

    void append_where_sql(std::string& sql, std::vector<SqlValue>& params, SqlDialect dialect) const {
        if (where_builders_.empty()) return;
        sql += " WHERE ";
        for (std::size_t i = 0; i < where_builders_.size(); ++i) {
            if (i > 0) sql += " AND ";
            sql += where_builders_[i](params, dialect);
        }
    }

    void append_order_sql(std::string& sql, SqlDialect dialect) const {
        if (order_clauses_.empty()) return;
        sql += " ORDER BY ";
        for (std::size_t i = 0; i < order_clauses_.size(); ++i) {
            if (i > 0) sql += ", ";
            sql += detail::quote_ident(order_clauses_[i].first, dialect);
            sql += order_clauses_[i].second ? " ASC" : " DESC";
        }
    }

public:
    explicit DbSet(DbContext& ctx) noexcept : ctx_(&ctx) {}

    // ---- Fluent Query Construction (compiles AST -> SQL) ----

    template <query::IsExpr Expr>
    [[nodiscard]] DbSet<T> where(Expr expr) const {
        DbSet<T> copy = *this;
        copy.where_builders_.push_back(
            [e = std::move(expr)](std::vector<SqlValue>& params, SqlDialect dialect) {
                return SqlCompiler::compile_expr(e, params, dialect);
            }
        );
        return copy;
    }

    template <typename KeySelector>
    [[nodiscard]] DbSet<T> order_by(const KeySelector& key) const {
        DbSet<T> copy = *this;
        copy.order_clauses_.clear();
        copy.order_clauses_.emplace_back(resolve_column_name(key), true);
        return copy;
    }

    template <typename KeySelector>
    [[nodiscard]] DbSet<T> order_by_desc(const KeySelector& key) const {
        DbSet<T> copy = *this;
        copy.order_clauses_.clear();
        copy.order_clauses_.emplace_back(resolve_column_name(key), false);
        return copy;
    }

    template <typename KeySelector>
    [[nodiscard]] DbSet<T> then_by(const KeySelector& key) const {
        DbSet<T> copy = *this;
        copy.order_clauses_.emplace_back(resolve_column_name(key), true);
        return copy;
    }

    template <typename KeySelector>
    [[nodiscard]] DbSet<T> then_by_desc(const KeySelector& key) const {
        DbSet<T> copy = *this;
        copy.order_clauses_.emplace_back(resolve_column_name(key), false);
        return copy;
    }

    [[nodiscard]] DbSet<T> skip(std::size_t n) const {
        DbSet<T> copy = *this;
        copy.offset_ = n;
        return copy;
    }

    [[nodiscard]] DbSet<T> take(std::size_t n) const {
        DbSet<T> copy = *this;
        copy.limit_ = n;
        return copy;
    }

    // ---- SQL Compilation & Inspection ----

    [[nodiscard]] CompiledQuery to_sql() const;
    [[nodiscard]] CompiledQuery to_count_sql() const;
    [[nodiscard]] CompiledQuery to_delete_sql() const;

    // ---- Query Execution ----

    [[nodiscard]] std::vector<T> to_vec() const;
    [[nodiscard]] std::optional<T> first() const;

    template <query::IsExpr Expr>
    [[nodiscard]] std::optional<T> first(Expr expr) const {
        return where(std::move(expr)).first();
    }

    [[nodiscard]] std::optional<T> find(std::int64_t id) const;
    [[nodiscard]] std::size_t count() const;

    template <query::IsExpr Expr>
    [[nodiscard]] std::size_t count(Expr expr) const {
        return where(std::move(expr)).count();
    }

    [[nodiscard]] bool any() const { return count() > 0; }

    template <query::IsExpr Expr>
    [[nodiscard]] bool any(Expr expr) const {
        return where(std::move(expr)).any();
    }

    /// Executes `COUNT(*)` + `LIMIT`/`OFFSET` and returns a JSON-serializable `Page<T>`.
    [[nodiscard]] query::Page<T> paginate(std::size_t page, std::size_t page_size) const;

    /// Executes bulk `DELETE FROM "table" WHERE ...` and returns number of deleted rows.
    std::uint64_t delete_where() const;

    template <query::IsExpr Expr>
    std::uint64_t delete_where(Expr expr) const {
        return where(std::move(expr)).delete_where();
    }

    // ---- Unit of Work Staging & Immediate Mutations ----

    void add(T& entity);
    void add(const T& entity);
    void update(const T& entity);
    void remove(std::int64_t id);
    void remove(const T& entity);

    /// Inserts immediately (outside pending UoW batch), sets `entity.id`, and returns `entity`.
    T& insert_now(T& entity);
    std::uint64_t update_now(const T& entity);
    std::uint64_t remove_now(std::int64_t id);
};

/**
 * @class DbContext
 * @brief Entity Framework-style database session managing `DbSet<T>` queries, atomic Unit of Work
 *        (`add`/`update`/`remove` + `save_changes()`), transactions, auto-schema creation, and migrations.
 *
 * When a `BlockingPool` is provided (e.g. via DI), every synchronous SQL driver call automatically
 * offloads to `BlockingPool::run(...)`, parking the calling `gorun` green routine without blocking workers.
 */
class DbContext {
    std::shared_ptr<IDbDriver> driver_;
    BlockingPool* pool_{nullptr};

    struct PendingOp {
        std::function<std::uint64_t(IDbDriver&)> apply;
    };
    std::vector<PendingOp> pending_ops_;

    static int& pool_reentrancy_depth() noexcept {
        static thread_local int depth = 0;
        return depth;
    }

    template <typename Fn>
    decltype(auto) run_db(Fn&& fn) const {
        if (pool_ && pool_reentrancy_depth() == 0) {
            return pool_->run([&]() -> decltype(auto) {
                struct DepthGuard {
                    int& d;
                    DepthGuard(int& ref) noexcept : d(ref) { ++d; }
                    ~DepthGuard() noexcept { --d; }
                } guard(pool_reentrancy_depth());
                return fn();
            });
        }
        return fn();
    }

public:
    explicit DbContext(std::shared_ptr<IDbDriver> driver, BlockingPool* pool = nullptr)
        : driver_(std::move(driver)), pool_(pool) {
        if (!driver_) {
            throw DbError("DbContext requires a non-null IDbDriver.");
        }
    }

    /// DI-friendly constructor: `app.scoped<DbContext, std::shared_ptr<IDbDriver>, BlockingPool>()`
    DbContext(std::shared_ptr<IDbDriver> driver, BlockingPool& pool)
        : DbContext(std::move(driver), &pool) {}

    virtual ~DbContext() = default;

    [[nodiscard]] SqlDialect dialect() const noexcept { return driver_->dialect(); }
    [[nodiscard]] IDbDriver& driver() noexcept { return *driver_; }
    [[nodiscard]] const IDbDriver& driver() const noexcept { return *driver_; }

    /// Returns a fluent `DbSet<T>` handle for querying and mutating model `T`.
    template <typename T>
    [[nodiscard]] DbSet<T> set() noexcept {
        return DbSet<T>(*this);
    }

    /// Creates tables (`CREATE TABLE IF NOT EXISTS`) for all listed `GOFORGE_MODEL` types.
    template <typename... Models>
    void ensure_created() {
        run_db([&]() {
            (([&] {
                std::string ddl = SqlCompiler::build_create_table_sql<Models>(driver_->dialect());
                (void)driver_->execute(ddl);
            }()), ...);
        });
    }

    /// Applies any pending versioned migrations atomically, recording history in `_goforge_migrations`.
    std::size_t migrate(const std::vector<Migration>& migrations) {
        return run_db([&]() -> std::size_t {
            const SqlDialect d = driver_->dialect();
            const std::string tbl = detail::quote_ident("_goforge_migrations", d);
            (void)driver_->execute(
                "CREATE TABLE IF NOT EXISTS " + tbl + " (" +
                detail::quote_ident("version", d) + " TEXT PRIMARY KEY, " +
                detail::quote_ident("name", d) + " TEXT NOT NULL)"
            );

            auto applied_rows = driver_->query(
                "SELECT " + detail::quote_ident("version", d) + " FROM " + tbl
            );
            std::unordered_set<std::string> applied;
            for (const auto& r : applied_rows) {
                if (!r.empty()) applied.insert(r.at(0).as_string());
            }

            std::size_t newly_applied = 0;
            for (const auto& m : migrations) {
                if (applied.contains(m.version)) continue;

                driver_->with_transaction([&]() {
                    for (const auto& stmt : m.up_sql) {
                        (void)driver_->execute(stmt);
                    }
                    std::string ins =
                        "INSERT INTO " + tbl + " (" +
                        detail::quote_ident("version", d) + ", " +
                        detail::quote_ident("name", d) + ") VALUES (" +
                        detail::placeholder(1, d) + ", " +
                        detail::placeholder(2, d) + ")";
                    (void)driver_->execute(ins, {SqlValue(m.version), SqlValue(m.name)});
                });
                ++newly_applied;
            }
            return newly_applied;
        });
    }

    // ---- Unit of Work Staging ----

    /// Stages an `INSERT` for `entity` and automatically populates `entity.id` when `save_changes()` commits.
    template <typename T>
    void add(T& entity) {
        static_assert(query::EntityMeta<T>::registered);
        T* ptr = &entity;
        pending_ops_.push_back(PendingOp{
            [ptr](IDbDriver& drv) -> std::uint64_t {
                auto q = SqlCompiler::build_insert_sql(*ptr, drv.dialect());
                auto res = drv.execute(q.sql, q.params);
                if (res.last_insert_id != 0 && SqlCompiler::get_entity_id(*ptr) == 0) {
                    SqlCompiler::set_entity_id(*ptr, res.last_insert_id);
                }
                return res.rows_affected;
            }
        });
    }

    /// Stages an `INSERT` for a value/const copy of `entity`.
    template <typename T>
    void add(const T& entity) {
        static_assert(query::EntityMeta<T>::registered);
        pending_ops_.push_back(PendingOp{
            [copy = entity](IDbDriver& drv) -> std::uint64_t {
                auto q = SqlCompiler::build_insert_sql(copy, drv.dialect());
                return drv.execute(q.sql, q.params).rows_affected;
            }
        });
    }

    /// Stages an `UPDATE` by primary key (`id`).
    template <typename T>
    void update(const T& entity) {
        static_assert(query::EntityMeta<T>::registered);
        pending_ops_.push_back(PendingOp{
            [copy = entity](IDbDriver& drv) -> std::uint64_t {
                auto q = SqlCompiler::build_update_sql(copy, drv.dialect());
                return drv.execute(q.sql, q.params).rows_affected;
            }
        });
    }

    /// Stages a `DELETE` by primary key (`id`).
    template <typename T>
    void remove(std::int64_t id) {
        static_assert(query::EntityMeta<T>::registered);
        pending_ops_.push_back(PendingOp{
            [id](IDbDriver& drv) -> std::uint64_t {
                auto q = SqlCompiler::build_delete_by_id_sql<T>(id, drv.dialect());
                return drv.execute(q.sql, q.params).rows_affected;
            }
        });
    }

    /// Stages a `DELETE` for `entity` using its `id` field.
    template <typename T>
    void remove(const T& entity) {
        remove<T>(SqlCompiler::get_entity_id(entity));
    }

    [[nodiscard]] std::size_t pending_changes() const noexcept { return pending_ops_.size(); }
    void discard_changes() noexcept { pending_ops_.clear(); }

    /// Atomically commits all staged `add`/`update`/`remove` changes inside a single transaction.
    std::uint64_t save_changes() {
        if (pending_ops_.empty()) return 0;
        auto ops = std::move(pending_ops_);
        pending_ops_.clear();

        return run_db([&]() -> std::uint64_t {
            std::uint64_t total_affected = 0;
            driver_->with_transaction([&]() {
                for (auto& op : ops) {
                    total_affected += op.apply(*driver_);
                }
            });
            return total_affected;
        });
    }

    /// Executes `fn()` inside an explicit driver-serialized transaction (`COMMIT` on normal return, `ROLLBACK` on throw).
    template <typename Fn>
    decltype(auto) transaction(Fn&& fn) {
        if constexpr (std::is_void_v<std::invoke_result_t<Fn>>) {
            run_db([&]() {
                driver_->with_transaction([&]() { fn(); });
            });
        } else {
            using R = std::invoke_result_t<Fn>;
            return run_db([&]() -> R {
                std::optional<R> res;
                driver_->with_transaction([&]() { res.emplace(fn()); });
                return std::move(*res);
            });
        }
    }

    // ---- Raw SQL Escape Hatch (automatically offloaded to BlockingPool) ----

    ExecResult execute_raw(const std::string& sql, const std::vector<SqlValue>& params = {}) {
        return run_db([&]() { return driver_->execute(sql, params); });
    }

    std::vector<SqlRow> query_raw(const std::string& sql, const std::vector<SqlValue>& params = {}) const {
        return run_db([&]() { return driver_->query(sql, params); });
    }
};

// ============================================================================
// DbSet<T> Out-of-Line Method Definitions
// ============================================================================

template <typename T>
inline CompiledQuery DbSet<T>::to_sql() const {
    const SqlDialect d = ctx_->dialect();
    CompiledQuery q;
    q.sql = "SELECT " + SqlCompiler::build_select_columns<T>(d) +
            " FROM " + detail::quote_ident(query::EntityMeta<T>::table_name, d);
    append_where_sql(q.sql, q.params, d);
    append_order_sql(q.sql, d);
    if (limit_.has_value()) {
        q.sql += " LIMIT " + std::to_string(*limit_);
    } else if (offset_.has_value()) {
        q.sql += (d == SqlDialect::SQLite) ? " LIMIT -1" : " LIMIT ALL";
    }
    if (offset_.has_value()) {
        q.sql += " OFFSET " + std::to_string(*offset_);
    }
    return q;
}

template <typename T>
inline CompiledQuery DbSet<T>::to_count_sql() const {
    const SqlDialect d = ctx_->dialect();
    CompiledQuery q;
    q.sql = "SELECT COUNT(*) FROM " + detail::quote_ident(query::EntityMeta<T>::table_name, d);
    append_where_sql(q.sql, q.params, d);
    return q;
}

template <typename T>
inline CompiledQuery DbSet<T>::to_delete_sql() const {
    const SqlDialect d = ctx_->dialect();
    CompiledQuery q;
    q.sql = "DELETE FROM " + detail::quote_ident(query::EntityMeta<T>::table_name, d);
    append_where_sql(q.sql, q.params, d);
    return q;
}

template <typename T>
inline std::vector<T> DbSet<T>::to_vec() const {
    auto compiled = to_sql();
    auto rows = ctx_->query_raw(compiled.sql, compiled.params);
    std::vector<T> out;
    out.reserve(rows.size());
    for (const auto& row : rows) {
        out.push_back(SqlCompiler::hydrate_entity<T>(row));
    }
    return out;
}

template <typename T>
inline std::optional<T> DbSet<T>::first() const {
    auto vec = take(1).to_vec();
    if (vec.empty()) return std::nullopt;
    return std::move(vec.front());
}

template <typename T>
inline std::optional<T> DbSet<T>::find(std::int64_t id) const {
    const SqlDialect d = ctx_->dialect();
    std::string sql = "SELECT " + SqlCompiler::build_select_columns<T>(d) +
                      " FROM " + detail::quote_ident(query::EntityMeta<T>::table_name, d) +
                      " WHERE " + detail::quote_ident("id", d) + " = " +
                      detail::placeholder(1, d) + " LIMIT 1";
    auto rows = ctx_->query_raw(sql, {SqlValue(id)});
    if (rows.empty()) return std::nullopt;
    return SqlCompiler::hydrate_entity<T>(rows.front());
}

template <typename T>
inline std::size_t DbSet<T>::count() const {
    auto compiled = to_count_sql();
    auto rows = ctx_->query_raw(compiled.sql, compiled.params);
    if (rows.empty() || rows.front().empty()) return 0;
    return static_cast<std::size_t>(rows.front().at(0).as_int64());
}

template <typename T>
inline query::Page<T> DbSet<T>::paginate(std::size_t page, std::size_t page_size) const {
    if (page == 0) page = 1;
    if (page_size == 0) page_size = 20;

    query::Page<T> p;
    p.total = count();
    p.page = page;
    p.page_size = page_size;
    p.total_pages = (p.total == 0) ? 0 : ((p.total + page_size - 1) / page_size);
    p.has_prev = page > 1 && p.total > 0;
    p.has_next = page < p.total_pages;

    const std::size_t off = (page - 1) * page_size;
    if (off < p.total) {
        p.items = skip(off).take(page_size).to_vec();
    }
    return p;
}

template <typename T>
inline std::uint64_t DbSet<T>::delete_where() const {
    auto compiled = to_delete_sql();
    return ctx_->execute_raw(compiled.sql, compiled.params).rows_affected;
}

template <typename T>
inline void DbSet<T>::add(T& entity) { ctx_->add(entity); }

template <typename T>
inline void DbSet<T>::add(const T& entity) { ctx_->add(entity); }

template <typename T>
inline void DbSet<T>::update(const T& entity) { ctx_->update(entity); }

template <typename T>
inline void DbSet<T>::remove(std::int64_t id) { ctx_->remove<T>(id); }

template <typename T>
inline void DbSet<T>::remove(const T& entity) { ctx_->remove(entity); }

template <typename T>
inline T& DbSet<T>::insert_now(T& entity) {
    auto q = SqlCompiler::build_insert_sql(entity, ctx_->dialect());
    auto res = ctx_->execute_raw(q.sql, q.params);
    if (res.last_insert_id != 0 && SqlCompiler::get_entity_id(entity) == 0) {
        SqlCompiler::set_entity_id(entity, res.last_insert_id);
    }
    return entity;
}

template <typename T>
inline std::uint64_t DbSet<T>::update_now(const T& entity) {
    auto q = SqlCompiler::build_update_sql(entity, ctx_->dialect());
    return ctx_->execute_raw(q.sql, q.params).rows_affected;
}

template <typename T>
inline std::uint64_t DbSet<T>::remove_now(std::int64_t id) {
    auto q = SqlCompiler::build_delete_by_id_sql<T>(id, ctx_->dialect());
    return ctx_->execute_raw(q.sql, q.params).rows_affected;
}

} // namespace goforge::data
// ---- end goforge/data/db_context.hpp ----

// ---- end goforge/goforge.hpp ----
