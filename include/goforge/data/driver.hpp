#pragma once

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

#include "goforge/data/sql_compiler.hpp"

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
