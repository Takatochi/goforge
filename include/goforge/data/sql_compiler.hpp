#pragma once

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

#include "goforge/query.hpp"

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
