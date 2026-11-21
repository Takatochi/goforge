#pragma once

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

#include "goforge/json.hpp"

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
