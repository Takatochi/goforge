#pragma once

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

#include "goforge/blocking_pool.hpp"
#include "goforge/data/driver.hpp"
#include "goforge/data/sql_compiler.hpp"
#include "goforge/query.hpp"

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
