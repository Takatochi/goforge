#include <iostream>
#include <memory>
#include <optional>
#include <string>

#include "goforge/goforge.hpp"

using namespace goforge;
using namespace goforge::query;
using namespace goforge::data;

// 1. Unified Model: JSON DTO + Query AST + SQL Table Schema (zero external codegen!)
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

struct CreateUserDto {
    std::string name;
    std::string email;
    std::string role{"user"};
    int age{18};
    std::optional<std::string> bio;

    std::optional<std::string> validate() const {
        if (name.empty()) return "name must not be empty";
        if (email.find('@') == std::string::npos) return "email must contain '@'";
        if (age < 13) return "age must be >= 13";
        return std::nullopt;
    }
};
GOFORGE_JSON_DTO_DEFAULT(CreateUserDto, name, email, role, age, bio)

// 2. EF-Style DbContext (automatically offloads blocking SQLite calls onto BlockingPool!)
struct AppDbContext final : DbContext {
    AppDbContext(std::shared_ptr<IDbDriver> drv, BlockingPool& pool)
        : DbContext(std::move(drv), pool) {}

    DbSet<User> users() noexcept { return set<User>(); }
};

// 3. Zero-Lambda Controller backed directly by SQLite via AppDbContext!
struct UserController {
    AppDbContext& db;
    explicit UserController(AppDbContext& ctx) : db(ctx) {}

    ApiResponse create(Body<CreateUserDto> dto) {
        User u{0, dto->name, dto->email, dto->role, dto->age, true, dto->bio};
        db.add(u);
        db.save_changes(); // Executes parameterized INSERT inside transaction & backfills u.id!
        return created(u);
    }

    std::optional<User> get_by_id(Path<"id", int> id) {
        return db.users().find(*id);
    }

    Page<User> list(Query<"q"> q, Query<"min_age", int> min_age,
                    Query<"page", int> page, Query<"limit", int> limit) {
        auto set = db.users().where(col(&User::active) == true && col(&User::age) >= min_age.value_or(0));
        if (q.has_value() && !q->empty()) {
            set = set.where(col(&User::name).icontains(*q) || col(&User::email).icontains(*q));
        }
        return set.order_by(&User::role).then_by_desc(&User::age).paginate(
            static_cast<std::size_t>(std::max(1, page.value_or(1))),
            static_cast<std::size_t>(std::max(1, limit.value_or(20)))
        );
    }

    ApiResponse remove(Path<"id", int> id) {
        return db.users().remove_now(*id) ? no_content() : not_found("user not found");
    }
};

int main() {
    auto sqlite = SqliteDriver::open("goforge_demo.db");

    auto app = WebApp::create({.host = "127.0.0.1", .port = 8080, .io_threads = 2})
        .instance<IDbDriver>(sqlite)
        .scoped<AppDbContext, std::shared_ptr<IDbDriver>, BlockingPool>()
        .use_defaults();

    app.invoke([](std::shared_ptr<IDbDriver> drv, BlockingPool& pool) {
        AppDbContext db(std::move(drv), pool);
        db.ensure_created<User>();
        if (!db.users().any()) {
            db.add(User{0, "Artur Havor", "artur@goforge.dev", "admin", 25, true, "Creator"});
            db.add(User{0, "Olena K.",    "olena@goforge.dev", "admin", 31, true, std::nullopt});
            db.save_changes();
        }
    });

    app.controller<UserController, AppDbContext>("/api/v1/users")
       .post("", &UserController::create)
       .get("", &UserController::list)
       .get("/:id", &UserController::get_by_id)
       .del("/:id", &UserController::remove);

    std::cout << "Starting goforge ORM + SQLite3 REST API on http://127.0.0.1:8080 ...\n";
    app.run();
    return 0;
}
