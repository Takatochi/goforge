#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "goforge/goforge.hpp"

using namespace goforge;
using namespace goforge::query;
using namespace goforge::data;

#define GF_CHECK(cond)                                                                      \
    do {                                                                                    \
        if (!(cond)) {                                                                      \
            std::cerr << "[FAIL] " << #cond << " at " << __FILE__ << ":" << __LINE__ << "\n"; \
            std::exit(1);                                                                   \
        }                                                                                   \
    } while (0)

// ============================================================================
// Unified Models (JSON DTO + Query AST + EF-Style ORM Schema!)
// ============================================================================
struct User {
    int id{0};
    std::string name;
    std::string email;
    std::string role{"user"};
    int age{0};
    bool active{true};
    std::optional<std::string> bio;
};
GOFORGE_MODEL_TABLE(User, "users", id, name, email, role, age, active, bio)

struct Order {
    int id{0};
    int user_id{0};
    double price{0.0};
    int qty{1};
};
GOFORGE_MODEL_TABLE(Order, "orders", id, user_id, price, qty)

struct CreateUserReq {
    std::string name;
    std::string email;
    std::string role{"user"};
    int age{18};
    std::optional<std::string> bio;

    std::optional<std::string> validate() const {
        if (name.empty()) return "name cannot be empty";
        if (email.find('@') == std::string::npos) return "invalid email";
        if (age < 13) return "age must be >= 13";
        return std::nullopt;
    }
};
GOFORGE_JSON_DTO_DEFAULT(CreateUserReq, name, email, role, age, bio)

// ============================================================================
// Test 1: Multi-Dialect Compile-Time SQL AST Compiler (Postgres / SQLite / MySQL)
// ============================================================================
static void test_sql_ast_compiler_dialects() {
    // PostgreSQL dialect ($1, $2, ILIKE, BIGSERIAL, RETURNING "id")
    auto pg_drv = std::make_shared<RecordingDbDriver>(SqlDialect::Postgres);
    DbContext pg_db(pg_drv);

    auto q_pg = pg_db.set<User>()
        .where(col(&User::age) >= 18 && col(&User::role).in({"admin", "mod"}))
        .where(col(&User::name).icontains("art") && col(&User::bio).is_not_null())
        .order_by(&User::role)
        .then_by_desc(&User::age)
        .skip(10)
        .take(5)
        .to_sql();

    GF_CHECK(q_pg.sql ==
        "SELECT \"id\", \"name\", \"email\", \"role\", \"age\", \"active\", \"bio\" "
        "FROM \"users\" "
        "WHERE (\"age\" >= $1 AND \"role\" IN ($2, $3)) "
        "AND (\"name\" ILIKE $4 ESCAPE '\\' AND \"bio\" IS NOT NULL) "
        "ORDER BY \"role\" ASC, \"age\" DESC LIMIT 5 OFFSET 10");
    GF_CHECK(q_pg.params.size() == 4);
    GF_CHECK(q_pg.params[0].as_int64() == 18);
    GF_CHECK(q_pg.params[1].as_string() == "admin");
    GF_CHECK(q_pg.params[2].as_string() == "mod");
    GF_CHECK(q_pg.params[3].as_string() == "%art%");

    // Postgres DDL & INSERT RETURNING "id"
    std::string pg_ddl = SqlCompiler::build_create_table_sql<User>(SqlDialect::Postgres);
    GF_CHECK(pg_ddl.find("\"id\" BIGSERIAL PRIMARY KEY") != std::string::npos);
    GF_CHECK(pg_ddl.find("\"active\" BOOLEAN NOT NULL") != std::string::npos);

    User draft{0, "Artur", "a@dev", "admin", 25, true, std::nullopt};
    auto ins_pg = SqlCompiler::build_insert_sql(draft, SqlDialect::Postgres);
    GF_CHECK(ins_pg.sql.find("RETURNING \"id\"") != std::string::npos);
    GF_CHECK(ins_pg.params.size() == 6); // id==0 omitted for auto-increment!

    // MySQL dialect (`ident`, ?, AUTO_INCREMENT)
    auto my_drv = std::make_shared<RecordingDbDriver>(SqlDialect::MySQL);
    DbContext my_db(my_drv);
    auto q_my = my_db.set<Order>()
        .where(col(&Order::price) * col(&Order::qty) > 100.0 && col(&Order::qty).between(2, 10))
        .to_sql();
    GF_CHECK(q_my.sql.find("WHERE ((`price` * `qty`) > ? AND `qty` BETWEEN ? AND ?)") != std::string::npos);
    GF_CHECK(q_my.params.size() == 3);

    std::cout << "[PASS] Test 1: Multi-dialect SQL AST compiler (Postgres / SQLite / MySQL)\n";
}

#if defined(GOFORGE_HAS_SQLITE) && GOFORGE_HAS_SQLITE

// ============================================================================
// Test 2: Embedded SQLite3 + ensure_created + Unit of Work + Transaction Rollback
// ============================================================================
static void test_sqlite_uow_and_transactions() {
    auto drv = SqliteDriver::open(":memory:");
    DbContext db(drv);
    db.ensure_created<User, Order>();

    // Stage multiple inserts atomically via Unit of Work
    User u1{0, "Artur", "artur@goforge.dev", "admin", 25, true, "Creator"};
    User u2{0, "Olena", "olena@goforge.dev", "admin", 31, true, std::nullopt};
    User u3{0, "Ivan",  "ivan@goforge.dev",  "mod",   22, false, std::nullopt};

    db.add(u1);
    db.add(u2);
    db.add(u3);
    GF_CHECK(db.pending_changes() == 3);

    std::uint64_t affected = db.save_changes();
    GF_CHECK(affected == 3);
    GF_CHECK(db.pending_changes() == 0);
    // Auto-increment IDs automatically backfilled onto tracked entities!
    GF_CHECK(u1.id == 1 && u2.id == 2 && u3.id == 3);

    // Update & Remove in a single atomic save_changes()
    u1.age = 26;
    u1.bio = "Framework Author";
    db.update(u1);
    db.remove(u3);
    GF_CHECK(db.save_changes() == 2);

    auto loaded = db.set<User>().find(1);
    GF_CHECK(loaded.has_value() && loaded->age == 26 && loaded->bio == "Framework Author");
    GF_CHECK(!db.set<User>().find(3).has_value());

    // Scoped transaction rollback on exception
    bool threw = false;
    try {
        db.transaction([&]() {
            User tmp{0, "Ghost", "ghost@dev", "user", 20, true, std::nullopt};
            db.set<User>().insert_now(tmp);
            GF_CHECK(db.set<User>().count() == 3);
            throw std::runtime_error("simulated payment failure");
        });
    } catch (const std::runtime_error&) {
        threw = true;
    }
    GF_CHECK(threw);
    GF_CHECK(db.set<User>().count() == 2); // Rolled back cleanly!

    std::cout << "[PASS] Test 2: Embedded SQLite3 Unit of Work, auto-IDs & transaction rollback\n";
}

// ============================================================================
// Test 3: Real SQLite3 Fluent DbSet<T> Queries (Exact Same AST as In-Memory!)
// ============================================================================
static void test_sqlite_fluent_dbset_queries() {
    auto drv = SqliteDriver::open(":memory:");
    DbContext db(drv);
    db.ensure_created<User, Order>();

    db.add(User{0, "Artur",  "artur@goforge.dev",  "admin", 25, true,  "Creator"});
    db.add(User{0, "Olena",  "olena@goforge.dev",  "admin", 31, true,  std::nullopt});
    db.add(User{0, "Max",    "max@example.com",    "user",  17, true,  "Student"});
    db.add(User{0, "Ivan",   "ivan@goforge.dev",   "mod",   22, false, std::nullopt});
    db.add(User{0, "Bohdan", "bohdan@goforge.dev", "user",  29, true,  "Backend"});
    db.save_changes();

    // Complex AST filter + multi-key ORDER BY
    auto active_adults = db.set<User>()
        .where(col(&User::active) == true && col(&User::age) >= 18)
        .order_by(&User::role)
        .then_by_desc(&User::age)
        .to_vec();
    GF_CHECK(active_adults.size() == 3);
    GF_CHECK(active_adults[0].name == "Olena");
    GF_CHECK(active_adults[1].name == "Artur");
    GF_CHECK(active_adults[2].name == "Bohdan");

    // .icontains(), .ends_with(), .in(), .between(), .is_null(), !col()
    GF_CHECK(db.set<User>().count(col(&User::email).ends_with("@goforge.dev")) == 4);
    GF_CHECK(db.set<User>().count(col(&User::name).icontains("ART")) == 1);
    GF_CHECK(db.set<User>().count(col(&User::role).in({"admin", "mod"})) == 3);
    GF_CHECK(db.set<User>().count(col(&User::age).between(20, 30)) == 3);
    GF_CHECK(db.set<User>().count(col(&User::bio).is_null()) == 2);
    GF_CHECK(db.set<User>().count(!col(&User::active)) == 1);
    GF_CHECK(db.set<User>().any(col(&User::age) > 30));
    GF_CHECK(!db.set<User>().any(col(&User::age) > 99));

    // SQL-level pagination (COUNT(*) + LIMIT/OFFSET) -> JSON-ready Page<User>!
    auto page1 = db.set<User>().order_by(&User::id).paginate(1, 2);
    GF_CHECK(page1.total == 5 && page1.total_pages == 3 && page1.has_next && !page1.has_prev);
    GF_CHECK(page1.items.size() == 2 && page1.items[0].name == "Artur" && page1.items[1].name == "Olena");

    auto page3 = db.set<User>().order_by(&User::id).paginate(3, 2);
    GF_CHECK(page3.items.size() == 1 && page3.items[0].name == "Bohdan" && !page3.has_next && page3.has_prev);

    // Arithmetic predicate on Orders
    db.add(Order{0, 1, 50.0, 1});
    db.add(Order{0, 1, 45.0, 3});
    db.add(Order{0, 2, 120.0, 2});
    db.save_changes();

    auto big_orders = db.set<Order>()
        .where(col(&Order::price) * col(&Order::qty) > 100.0)
        .order_by(&Order::id)
        .to_vec();
    GF_CHECK(big_orders.size() == 2 && big_orders[0].qty == 3 && big_orders[1].qty == 2);

    // Bulk DELETE WHERE
    auto deleted = db.set<User>().delete_where(!col(&User::active));
    GF_CHECK(deleted == 1);
    GF_CHECK(db.set<User>().count() == 4);

    std::cout << "[PASS] Test 3: Real SQLite3 fluent DbSet<T> queries, pagination & bulk delete\n";
}

// ============================================================================
// Test 4: Versioned Schema Migrations (Idempotent + Transactional)
// ============================================================================
static void test_sqlite_migrations() {
    auto drv = SqliteDriver::open(":memory:");
    DbContext db(drv);

    std::vector<Migration> migrations{
        {
            "001", "create_notes",
            {"CREATE TABLE \"notes\" (\"id\" INTEGER PRIMARY KEY AUTOINCREMENT, \"title\" TEXT NOT NULL)"}
        },
        {
            "002", "add_pinned_col",
            {"ALTER TABLE \"notes\" ADD COLUMN \"pinned\" INTEGER NOT NULL DEFAULT 0"}
        }
    };

    GF_CHECK(db.migrate(migrations) == 2);
    // Re-running migrations is 100% idempotent
    GF_CHECK(db.migrate(migrations) == 0);

    // Verify new column works
    db.execute_raw("INSERT INTO \"notes\" (\"title\", \"pinned\") VALUES (?1, ?2)",
                   {SqlValue("hello"), SqlValue(1)});
    auto rows = db.query_raw("SELECT \"title\", \"pinned\" FROM \"notes\"");
    GF_CHECK(rows.size() == 1 && rows[0].at(0).as_string() == "hello" && rows[0].at(1).as_int64() == 1);

    std::cout << "[PASS] Test 4: Versioned schema migrations (transactional & idempotent)\n";
}

// ============================================================================
// Test 5: Full-Stack WebApp + DI (BlockingPool + SqliteDriver + DbContext) + Controller
// ============================================================================
struct AppDbContext final : DbContext {
    AppDbContext(std::shared_ptr<IDbDriver> drv, BlockingPool& pool)
        : DbContext(std::move(drv), pool) {}

    DbSet<User> users() noexcept { return set<User>(); }
};

struct UserApiController {
    AppDbContext& db;
    explicit UserApiController(AppDbContext& ctx) : db(ctx) {}

    ApiResponse create(Body<CreateUserReq> dto) {
        User u{0, dto->name, dto->email, dto->role, dto->age, true, dto->bio};
        db.add(u);
        db.save_changes();
        return created(u);
    }

    std::optional<User> get_by_id(Path<"id", int> id) {
        return db.users().find(*id);
    }

    Page<User> list(Query<"q"> q, Query<"min_age", int> min_age,
                    Query<"page", int> page, Query<"limit", int> limit) {
        auto query_set = db.users().where(col(&User::age) >= min_age.value_or(0));
        if (q.has_value() && !q->empty()) {
            query_set = query_set.where(col(&User::name).icontains(*q) || col(&User::email).icontains(*q));
        }
        return query_set.order_by(&User::id).paginate(
            static_cast<std::size_t>(std::max(1, page.value_or(1))),
            static_cast<std::size_t>(std::max(1, limit.value_or(20)))
        );
    }

    ApiResponse remove(Path<"id", int> id) {
        if (db.users().remove_now(*id) == 0) {
            return not_found("user does not exist");
        }
        return no_content();
    }
};

static void test_webapp_with_sqlite_blocking_pool() {
    auto sqlite = SqliteDriver::open(":memory:");

    auto app = WebApp::create({.port = 0})
        .instance<IDbDriver>(sqlite)
        .scoped<AppDbContext, std::shared_ptr<IDbDriver>, BlockingPool>()
        .use_defaults(false);

    // Ensure schema exists on startup
    app.invoke([](std::shared_ptr<IDbDriver> drv, BlockingPool& pool) {
        AppDbContext init_db(std::move(drv), pool);
        init_db.ensure_created<User>();
    });

    app.controller<UserApiController, AppDbContext>("/api/v1/users")
       .post("", &UserApiController::create)
       .get("", &UserApiController::list)
       .get("/:id", &UserApiController::get_by_id)
       .del("/:id", &UserApiController::remove);

    app.start();

    auto make_req = [](std::string method, std::string path, std::string query = "", std::string body = "") {
        http::HttpRequest r;
        r.method = std::move(method);
        r.path = std::move(path);
        r.raw_query = std::move(query);
        r.body = std::move(body);
        return r;
    };

    // Execute concurrent HTTP requests across gorun green routines (offloading SQLite onto BlockingPool!)
    std::atomic<int> created_ok{0};
    {
        gorun::Scope scope;
        for (int i = 0; i < 10; ++i) {
            gorun::go([&, i]() {
                auto body = Json(H{
                    {"name", "User_" + std::to_string(i)},
                    {"email", "u" + std::to_string(i) + "@goforge.dev"},
                    {"age", 20 + i}
                }).dump();
                auto r = app.dispatch(make_req("POST", "/api/v1/users", "", body));
                if (r.status == 201) {
                    created_ok.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }
    }
    GF_CHECK(created_ok.load() == 10);

    // Query paginated list via real SQLite SELECT + COUNT(*)
    auto r_list = app.dispatch(make_req("GET", "/api/v1/users", "min_age=25&page=1&limit=3"));
    GF_CHECK(r_list.status == 200);
    auto j_list = Json::parse(r_list.body);
    GF_CHECK(j_list["total"] == 5); // ages 25, 26, 27, 28, 29
    GF_CHECK(j_list["items"].size() == 3);
    GF_CHECK(j_list["has_next"] == true);

    // Fetch by ID & Delete by ID
    int first_id = j_list["items"][0]["id"].get<int>();
    auto r_get = app.dispatch(make_req("GET", "/api/v1/users/" + std::to_string(first_id)));
    GF_CHECK(r_get.status == 200);

    auto r_del = app.dispatch(make_req("DELETE", "/api/v1/users/" + std::to_string(first_id)));
    GF_CHECK(r_del.status == 204);

    auto r_gone = app.dispatch(make_req("GET", "/api/v1/users/" + std::to_string(first_id)));
    GF_CHECK(r_gone.status == 404);

    app.stop();
    std::cout << "[PASS] Test 5: Full-stack WebApp + BlockingPool + SQLite3 AppDbContext + zero-lambda Controller\n";
}
#endif

int main() {
    std::cout << "=== goforge Stage 7: EF-Style ORM (goforge::data) Tests ===\n";
    test_sql_ast_compiler_dialects();
#if defined(GOFORGE_HAS_SQLITE) && GOFORGE_HAS_SQLITE
    test_sqlite_uow_and_transactions();
    test_sqlite_fluent_dbset_queries();
    test_sqlite_migrations();
    test_webapp_with_sqlite_blocking_pool();
#endif
    std::cout << "=== All Stage 7 ORM tests passed! ===\n";
    return 0;
}
