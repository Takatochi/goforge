#pragma once

/**
 * @file sqlite_driver.hpp
 * @brief Embedded SQLite 3 backend implementing `goforge::data::IDbDriver` with
 * WAL mode, foreign-key enforcement, prepared statement parameter binding, and
 * thread-safe access.
 */

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <sqlite3.h>

#include "goforge/data/driver.hpp"

namespace goforge::data {

/**
 * @class SqliteDriver
 * @brief Production-ready embedded SQLite 3 driver for
 * `goforge::data::DbContext`.
 */
class SqliteDriver final : public IDbDriver {
  struct StmtDeleter {
    void operator()(sqlite3_stmt *stmt) const noexcept {
      if (stmt)
        sqlite3_finalize(stmt);
    }
  };
  using UniqueStmt = std::unique_ptr<sqlite3_stmt, StmtDeleter>;

  sqlite3 *db_{nullptr};
  mutable std::recursive_mutex mtx_;

  void exec_pragma_unlocked(const char *sql) {
    char *errmsg = nullptr;
    int rc = sqlite3_exec(db_, sql, nullptr, nullptr, &errmsg);
    if (rc != SQLITE_OK) {
      std::string msg = errmsg ? errmsg : sqlite3_errmsg(db_);
      if (errmsg)
        sqlite3_free(errmsg);
      throw DbError("SQLite pragma error: " + msg);
    }
  }

  UniqueStmt prepare_and_bind_unlocked(const std::string &sql,
                                       const std::vector<SqlValue> &params) {
    sqlite3_stmt *raw = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql.c_str(), static_cast<int>(sql.size()),
                                &raw, nullptr);
    if (rc != SQLITE_OK) {
      throw DbError("SQLite prepare failed: " +
                    std::string(sqlite3_errmsg(db_)) + " | SQL: " + sql);
    }
    UniqueStmt stmt(raw);

    for (std::size_t i = 0; i < params.size(); ++i) {
      const int idx = static_cast<int>(i + 1);
      const auto &v = params[i];
      int brc = SQLITE_OK;
      if (v.is_null()) {
        brc = sqlite3_bind_null(stmt.get(), idx);
      } else if (const auto *iv = std::get_if<std::int64_t>(&v.data)) {
        brc = sqlite3_bind_int64(stmt.get(), idx, *iv);
      } else if (const auto *bv = std::get_if<bool>(&v.data)) {
        brc = sqlite3_bind_int(stmt.get(), idx, *bv ? 1 : 0);
      } else if (const auto *dv = std::get_if<double>(&v.data)) {
        brc = sqlite3_bind_double(stmt.get(), idx, *dv);
      } else if (const auto *sv = std::get_if<std::string>(&v.data)) {
        brc = sqlite3_bind_text(stmt.get(), idx, sv->c_str(),
                                static_cast<int>(sv->size()), SQLITE_TRANSIENT);
      }
      if (brc != SQLITE_OK) {
        throw DbError("SQLite bind failed at param " + std::to_string(idx) +
                      ": " + sqlite3_errmsg(db_));
      }
    }
    return stmt;
  }

public:
  explicit SqliteDriver(const std::string &path = ":memory:") {
    int rc = sqlite3_open_v2(path.c_str(), &db_,
                             SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                                 SQLITE_OPEN_FULLMUTEX,
                             nullptr);
    if (rc != SQLITE_OK) {
      std::string err = db_ ? sqlite3_errmsg(db_) : "unknown error";
      if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
      }
      throw DbError("Failed to open SQLite database '" + path + "': " + err);
    }

    sqlite3_busy_timeout(db_, 5000);
    exec_pragma_unlocked("PRAGMA foreign_keys = ON;");
    if (path != ":memory:" && path.find("mode=memory") == std::string::npos) {
      exec_pragma_unlocked("PRAGMA journal_mode = WAL;");
      exec_pragma_unlocked("PRAGMA synchronous = NORMAL;");
    }
  }

  ~SqliteDriver() override {
    if (db_) {
      sqlite3_close(db_);
      db_ = nullptr;
    }
  }

  SqliteDriver(const SqliteDriver &) = delete;
  SqliteDriver &operator=(const SqliteDriver &) = delete;

  /// Factory helper returning a DI-ready `std::shared_ptr<SqliteDriver>`.
  [[nodiscard]] static std::shared_ptr<SqliteDriver>
  open(const std::string &path = ":memory:") {
    return std::make_shared<SqliteDriver>(path);
  }

  [[nodiscard]] SqlDialect dialect() const noexcept override {
    return SqlDialect::SQLite;
  }

  /// Holds the connection lock for the entire transaction lifetime so
  /// concurrent BlockingPool threads never interleave overlapping BEGIN/COMMIT
  /// statements on a shared SQLite connection.
  void with_transaction(const std::function<void()> &fn) override {
    std::lock_guard lk(mtx_);
    begin_tx();
    try {
      fn();
      commit_tx();
    } catch (...) {
      try {
        rollback_tx();
      } catch (...) {
      }
      throw;
    }
  }

  ExecResult execute(const std::string &sql,
                     const std::vector<SqlValue> &params = {}) override {
    std::lock_guard lk(mtx_);
    auto stmt = prepare_and_bind_unlocked(sql, params);
    int rc = sqlite3_step(stmt.get());
    while (rc == SQLITE_ROW) {
      rc = sqlite3_step(stmt.get());
    }
    if (rc != SQLITE_DONE) {
      throw DbError("SQLite execute failed: " +
                    std::string(sqlite3_errmsg(db_)) + " | SQL: " + sql);
    }
    ExecResult res;
    res.rows_affected = static_cast<std::uint64_t>(sqlite3_changes(db_));
    res.last_insert_id =
        static_cast<std::int64_t>(sqlite3_last_insert_rowid(db_));
    return res;
  }

  std::vector<SqlRow> query(const std::string &sql,
                            const std::vector<SqlValue> &params = {}) override {
    std::lock_guard lk(mtx_);
    auto stmt = prepare_and_bind_unlocked(sql, params);
    const int col_count = sqlite3_column_count(stmt.get());

    std::vector<std::string> col_names;
    col_names.reserve(static_cast<std::size_t>(col_count));
    for (int i = 0; i < col_count; ++i) {
      const char *n = sqlite3_column_name(stmt.get(), i);
      col_names.emplace_back(n ? n : "");
    }

    std::vector<SqlRow> rows;
    int rc = SQLITE_OK;
    while ((rc = sqlite3_step(stmt.get())) == SQLITE_ROW) {
      SqlRow row;
      row.columns.reserve(static_cast<std::size_t>(col_count));
      for (int i = 0; i < col_count; ++i) {
        const int col_type = sqlite3_column_type(stmt.get(), i);
        SqlValue val;
        switch (col_type) {
        case SQLITE_NULL:
          val = SqlValue(nullptr);
          break;
        case SQLITE_INTEGER:
          val = SqlValue(
              static_cast<std::int64_t>(sqlite3_column_int64(stmt.get(), i)));
          break;
        case SQLITE_FLOAT:
          val = SqlValue(sqlite3_column_double(stmt.get(), i));
          break;
        case SQLITE_TEXT: {
          const unsigned char *txt = sqlite3_column_text(stmt.get(), i);
          const int len = sqlite3_column_bytes(stmt.get(), i);
          val = SqlValue(std::string(reinterpret_cast<const char *>(txt),
                                     static_cast<std::size_t>(len)));
          break;
        }
        case SQLITE_BLOB: {
          const void *blob = sqlite3_column_blob(stmt.get(), i);
          const int len = sqlite3_column_bytes(stmt.get(), i);
          val = SqlValue(std::string(static_cast<const char *>(blob),
                                     static_cast<std::size_t>(len)));
          break;
        }
        default:
          val = SqlValue(nullptr);
          break;
        }
        row.columns.emplace_back(col_names[static_cast<std::size_t>(i)],
                                 std::move(val));
      }
      rows.push_back(std::move(row));
    }
    if (rc != SQLITE_DONE) {
      throw DbError("SQLite query failed: " + std::string(sqlite3_errmsg(db_)) +
                    " | SQL: " + sql);
    }
    return rows;
  }
};

} // namespace goforge::data
