#include <iostream>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "goforge/goforge.hpp"

using namespace goforge;
using namespace goforge::query;

// Unified Model: JSON serialization + compile-time table/column reflection for Query AST & future ORM
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

struct IUserRepo {
    virtual ~IUserRepo() = default;
    virtual User create(const CreateUserDto& in) = 0;
    virtual std::optional<User> find(int id) const = 0;
    virtual Page<User> search(std::string_view q, int min_age, std::size_t page, std::size_t size) const = 0;
};

struct MemoryUserRepo final : IUserRepo {
    mutable gorun::RwMutex rw;
    int next_id{4};
    std::unordered_map<int, User> rows{
        {1, {1, "Artur Havor", "artur@goforge.dev", "admin", 25, true, "Creator"}},
        {2, {2, "Olena K.", "olena@goforge.dev", "admin", 31, true, std::nullopt}},
        {3, {3, "Bohdan M.", "bohdan@goforge.dev", "user", 22, true, "C++ Dev"}}
    };

    User create(const CreateUserDto& in) override {
        std::unique_lock lk(rw);
        int id = next_id++;
        User u{id, in.name, in.email, in.role, in.age, true, in.bio};
        rows[id] = u;
        return u;
    }

    std::optional<User> find(int id) const override {
        std::shared_lock lk(rw);
        auto it_row = rows.find(id);
        return it_row == rows.end() ? std::nullopt : std::optional<User>(it_row->second);
    }

    Page<User> search(std::string_view q, int min_age, std::size_t page, std::size_t size) const override {
        std::shared_lock lk(rw);
        auto seq = from_values(rows)
            .where(col(&User::active) == true && col(&User::age) >= min_age);
        if (!q.empty()) {
            seq = seq.where(col(&User::name).icontains(q) || col(&User::email).icontains(q));
        }
        return seq.order_by(&User::role).then_by_desc(&User::age).paginate(page, size);
    }
};

// Zero-lambda Controller: resolved automatically via DI per request!
struct UserController {
    IUserRepo& repo;
    explicit UserController(IUserRepo& r) : repo(r) {}

    ApiResponse create(Body<CreateUserDto> dto) {
        return created(repo.create(*dto));
    }

    std::optional<User> get_by_id(Path<"id", int> id) const {
        return repo.find(*id);
    }

    Page<User> list(Query<"q"> q, Query<"min_age", int> min_age,
                    Query<"page", int> page, Query<"limit", int> limit) const {
        return repo.search(
            q.value_or(""),
            min_age.value_or(0),
            static_cast<std::size_t>(std::max(1, page.value_or(1))),
            static_cast<std::size_t>(std::max(1, limit.value_or(20)))
        );
    }
};

int main() {
    auto app = WebApp::create({.host = "127.0.0.1", .port = 8080, .io_threads = 2})
        .singleton<IUserRepo, MemoryUserRepo>()
        .use_defaults();

    app.controller<UserController, IUserRepo>("/api/v1/users")
       .post("", &UserController::create)
       .get("", &UserController::list)
       .get("/:id", &UserController::get_by_id);

    std::cout << "Starting ergonomic goforge WebApp on http://127.0.0.1:8080 ...\n";
    app.run();
    return 0;
}
