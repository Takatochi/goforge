#include <atomic>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "goforge/goforge.hpp"

#define GOFORGE_ASSERT(cond)                                                              \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            std::cerr << "\n[FAIL] Assertion failed at " << __FILE__ << ":" << __LINE__  \
                      << " -> " << #cond << "\n";                                         \
            std::exit(1);                                                                 \
        }                                                                                 \
    } while (0)

namespace dto {

struct CreateUserDto {
    std::string name;
    std::string email;
    int age{0};

    std::optional<std::string> validate() const {
        if (name.empty()) return "name must not be empty";
        if (email.find('@') == std::string::npos) return "email must contain '@'";
        if (age < 13) return "age must be at least 13";
        return std::nullopt;
    }
};
GOFORGE_JSON_DTO(CreateUserDto, name, email, age)

struct UpdateUserPatchDto {
    std::optional<std::string> name;
    std::optional<std::string> bio;
    int age{-1};
};
GOFORGE_JSON_DTO_DEFAULT(UpdateUserPatchDto, name, bio, age)

struct UserResponse {
    int id{0};
    std::string name;
    std::string email;
    int age{0};
    std::optional<std::string> bio;
};
GOFORGE_JSON_DTO(UserResponse, id, name, email, age, bio)

struct UserListResponse {
    int total{0};
    std::vector<UserResponse> items;
};
GOFORGE_JSON_DTO(UserListResponse, total, items)

} // namespace dto

namespace {

goforge::http::HttpRequest make_json_req(std::string method, std::string path, std::string body = "") {
    goforge::http::HttpRequest req;
    req.method = std::move(method);
    req.target = path;
    req.path = std::move(path);
    req.body = std::move(body);
    req.headers.set("Content-Type", "application/json");
    return req;
}

// Test 01: Gin-style goforge::H inline JSON objects and c.error()
void test_01_inline_json_and_errors() {
    std::cout << "[TEST 01] Gin-style goforge::H inline objects & structured c.error()... ";

    goforge::web::Router router;
    router.get("/info", [](goforge::web::HttpContext& c) {
        c.json(200, goforge::H{
            {"framework", "goforge"},
            {"version", "0.5.0"},
            {"features", goforge::Json::array({"di", "router", "json"})}
        });
    });
    router.get("/fail", [](goforge::web::HttpContext& c) {
        c.error(403, "forbidden", "insufficient permissions");
    });

    auto r1 = router.dispatch(make_json_req("GET", "/info"));
    GOFORGE_ASSERT(r1.status == 200);
    auto j1 = goforge::Json::parse(r1.body);
    GOFORGE_ASSERT(j1["framework"] == "goforge");
    GOFORGE_ASSERT(j1["features"].size() == 3);

    auto r2 = router.dispatch(make_json_req("GET", "/fail"));
    GOFORGE_ASSERT(r2.status == 403);
    auto j2 = goforge::Json::parse(r2.body);
    GOFORGE_ASSERT(j2["error"] == "forbidden");
    GOFORGE_ASSERT(j2["message"] == "insufficient permissions");

    std::cout << "PASSED\n";
}

// Test 02: Strict GOFORGE_JSON_DTO binding (201 valid, 400 parse/missing/type errors)
void test_02_strict_dto_binding_and_error_codes() {
    std::cout << "[TEST 02] Strict GOFORGE_JSON_DTO binding (201 OK, 400 parse/missing/type)... ";

    goforge::web::Router router;
    router.post("/users", [](goforge::web::HttpContext& c) {
        auto input = c.bind_json<dto::CreateUserDto>();
        if (!input) return; // Error already written automatically!

        dto::UserResponse created{
            .id = 101,
            .name = input->name,
            .email = input->email,
            .age = input->age,
            .bio = std::nullopt
        };
        c.json(201, created);
    });

    // Valid DTO
    auto ok = router.dispatch(make_json_req(
        "POST", "/users",
        goforge::H{{"name", "Artur"}, {"email", "artur@goforge.dev"}, {"age", 24}}.dump()
    ));
    GOFORGE_ASSERT(ok.status == 201);
    auto created = goforge::Json::parse(ok.body).get<dto::UserResponse>();
    GOFORGE_ASSERT(created.id == 101 && created.name == "Artur" && !created.bio.has_value());

    // Malformed JSON -> 400 invalid_json
    auto bad_syntax = router.dispatch(make_json_req("POST", "/users", "{broken json"));
    GOFORGE_ASSERT(bad_syntax.status == 400);
    GOFORGE_ASSERT(goforge::Json::parse(bad_syntax.body)["error"] == "invalid_json");

    // Missing required field ("email") -> 400 missing_field
    auto missing = router.dispatch(make_json_req("POST", "/users", goforge::H{{"name", "Artur"}, {"age", 24}}.dump()));
    GOFORGE_ASSERT(missing.status == 400);
    GOFORGE_ASSERT(goforge::Json::parse(missing.body)["error"] == "missing_field");

    // Wrong field type ("age" is string instead of int) -> 400 invalid_payload
    auto wrong_type = router.dispatch(make_json_req(
        "POST", "/users",
        goforge::H{{"name", "Artur"}, {"email", "a@b.c"}, {"age", "twenty"}}.dump()
    ));
    GOFORGE_ASSERT(wrong_type.status == 400);
    GOFORGE_ASSERT(goforge::Json::parse(wrong_type.body)["error"] == "invalid_payload");

    std::cout << "PASSED\n";
}

// Test 03: Automatic DTO business validation (ValidatableDto -> 422 Unprocessable Entity)
void test_03_dto_validation_422() {
    std::cout << "[TEST 03] Automatic ValidatableDto::validate() -> 422 Unprocessable Entity... ";

    bool handler_body_ran = false;
    goforge::web::Router router;
    router.post("/users", [&](goforge::web::HttpContext& c) {
        auto input = c.bind_json<dto::CreateUserDto>();
        if (!input) return;
        handler_body_ran = true;
        c.json(201, *input);
    });

    auto invalid_email = router.dispatch(make_json_req(
        "POST", "/users",
        goforge::H{{"name", "Bob"}, {"email", "not-an-email"}, {"age", 20}}.dump()
    ));
    GOFORGE_ASSERT(!handler_body_ran);
    GOFORGE_ASSERT(invalid_email.status == 422);
    auto j = goforge::Json::parse(invalid_email.body);
    GOFORGE_ASSERT(j["error"] == "validation_failed");
    GOFORGE_ASSERT(j["message"] == "email must contain '@'");

    auto underage = router.dispatch(make_json_req(
        "POST", "/users",
        goforge::H{{"name", "Kid"}, {"email", "kid@example.com"}, {"age", 10}}.dump()
    ));
    GOFORGE_ASSERT(underage.status == 422);
    GOFORGE_ASSERT(goforge::Json::parse(underage.body)["message"] == "age must be at least 13");

    std::cout << "PASSED\n";
}

// Test 04: GOFORGE_JSON_DTO_DEFAULT + std::optional<T> partial PATCH updates
void test_04_partial_patch_dto_and_optional_fields() {
    std::cout << "[TEST 04] GOFORGE_JSON_DTO_DEFAULT & std::optional<T> PATCH updates... ";

    dto::UserResponse user{1, "Old Name", "old@dev.io", 20, "Initial bio"};

    goforge::web::Router router;
    router.patch("/users/1", [&](goforge::web::HttpContext& c) {
        auto patch = c.bind_json<dto::UpdateUserPatchDto>();
        if (!patch) return;
        if (patch->name) user.name = *patch->name;
        if (patch->bio) user.bio = *patch->bio;
        if (patch->age >= 0) user.age = patch->age;
        c.json(200, user);
    });

    // Only update `bio`, omit `name` and `age`!
    auto r = router.dispatch(make_json_req("PATCH", "/users/1", goforge::H{{"bio", "Updated bio"}}.dump()));
    GOFORGE_ASSERT(r.status == 200);
    auto updated = goforge::Json::parse(r.body).get<dto::UserResponse>();
    GOFORGE_ASSERT(updated.name == "Old Name");
    GOFORGE_ASSERT(updated.age == 20);
    GOFORGE_ASSERT(updated.bio.has_value() && *updated.bio == "Updated bio");

    std::cout << "PASSED\n";
}

// Test 05: Nested DTOs & STL collections serialized directly with c.json(200, list_dto)
void test_05_nested_dtos_and_collections() {
    std::cout << "[TEST 05] Nested DTOs & std::vector<UserResponse> direct serialization... ";

    goforge::web::Router router;
    router.get("/users", [](goforge::web::HttpContext& c) {
        dto::UserListResponse resp{
            .total = 2,
            .items = {
                dto::UserResponse{1, "Artur", "artur@goforge.dev", 25, "Creator"},
                dto::UserResponse{2, "Ada", "ada@goforge.dev", 36, std::nullopt}
            }
        };
        c.json(200, resp);
    });

    auto r = router.dispatch(make_json_req("GET", "/users"));
    GOFORGE_ASSERT(r.status == 200);
    auto parsed = goforge::Json::parse(r.body).get<dto::UserListResponse>();
    GOFORGE_ASSERT(parsed.total == 2);
    GOFORGE_ASSERT(parsed.items.size() == 2);
    GOFORGE_ASSERT(parsed.items[0].bio == "Creator");
    GOFORGE_ASSERT(!parsed.items[1].bio.has_value());

    std::cout << "PASSED\n";
}

// Test 06: Live TCP typed DTO REST CRUD over HttpServer + AppBuilder + DI
#if defined(GOFORGE_HAS_ASIO) && GOFORGE_HAS_ASIO
void test_06_live_tcp_typed_dto_crud() {
    std::cout << "[TEST 06] Live TCP typed DTO REST API (POST/GET/PATCH with nlohmann/json)... ";

    struct Repo {
        gorun::RwMutex mtx;
        int next_id{1};
        std::unordered_map<int, dto::UserResponse> rows;

        dto::UserResponse create(const dto::CreateUserDto& in) {
            std::unique_lock lk(mtx);
            int id = next_id++;
            dto::UserResponse u{id, in.name, in.email, in.age, std::nullopt};
            rows[id] = u;
            return u;
        }
        std::optional<dto::UserResponse> find(int id) {
            std::shared_lock lk(mtx);
            auto it = rows.find(id);
            if (it == rows.end()) return std::nullopt;
            return it->second;
        }
    };

    auto app = goforge::AppBuilder{}
        .provide_singleton<Repo>()
        .provide_singleton<goforge::web::Router>()
        .provide_instance(goforge::http::HttpServerConfig{.host = "127.0.0.1", .port = 0})
        .provide([](const goforge::http::HttpServerConfig& cfg,
                    goforge::web::Router& r,
                    goforge::ServiceProvider& sp,
                    goforge::Lifecycle& lc) {
            auto srv = std::make_shared<goforge::http::HttpServer>(cfg, r.to_http_handler(&sp));
            srv->attach_lifecycle(lc, "dto-http");
            return srv;
        })
        .invoke([](goforge::web::Router& r, goforge::http::HttpServer&) {
            r.post("/api/users", [](goforge::web::HttpContext& c, Repo& repo) {
                auto body = c.bind_json<dto::CreateUserDto>();
                if (!body) return;
                c.json(201, repo.create(*body));
            });
            r.get("/api/users/:id", [](goforge::web::HttpContext& c, Repo& repo) {
                auto u = repo.find(c.param_as<int>("id").value_or(0));
                if (!u) {
                    c.error(404, "user_not_found");
                    return;
                }
                c.json(200, *u);
            });
        })
        .build();

    app->start();
    auto port = app->services().resolve<goforge::http::HttpServer>().port();
    goforge::net::AsioExecutor io(1);

    auto send_raw = [&](std::string_view wire) -> std::pair<int, std::string> {
        goforge::net::AsyncTcpSocket sock(io.context());
        GOFORGE_ASSERT(!sock.connect("127.0.0.1", port));
        (void)sock.write_all(wire);
        std::string raw;
        char buf[4096];
        for (;;) {
            auto [n, ec] = sock.read_some(buf, sizeof(buf));
            if (ec || n == 0) break;
            raw.append(buf, n);
        }
        sock.close();
        auto sep = raw.find("\r\n\r\n");
        auto head = raw.substr(0, sep);
        auto sp1 = head.find(' ');
        auto sp2 = head.find(' ', sp1 + 1);
        return {std::stoi(head.substr(sp1 + 1, sp2 - (sp1 + 1))), raw.substr(sep + 4)};
    };

    std::string body = goforge::H{{"name", "Linus"}, {"email", "linus@kernel.org"}, {"age", 55}}.dump();
    auto [st1, b1] = send_raw(
        "POST /api/users HTTP/1.1\r\nHost: localhost\r\nContent-Type: application/json\r\n"
        "Content-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body
    );
    GOFORGE_ASSERT(st1 == 201);
    auto created = goforge::Json::parse(b1).get<dto::UserResponse>();
    GOFORGE_ASSERT(created.id == 1 && created.name == "Linus");

    auto [st2, b2] = send_raw("GET /api/users/1 HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
    GOFORGE_ASSERT(st2 == 200);
    GOFORGE_ASSERT(goforge::Json::parse(b2)["email"] == "linus@kernel.org");

    (void)app->stop();
    std::cout << "PASSED\n";
}
#endif

} // namespace

int main() {
    std::cout << std::unitbuf;
    std::cout << "======================================================================\n";
    std::cout << "  goforge Stage 5 Test Suite — JSON Serialization, DTOs & Validation\n";
    std::cout << "======================================================================\n";

    test_01_inline_json_and_errors();
    test_02_strict_dto_binding_and_error_codes();
    test_03_dto_validation_422();
    test_04_partial_patch_dto_and_optional_fields();
    test_05_nested_dtos_and_collections();
#if defined(GOFORGE_HAS_ASIO) && GOFORGE_HAS_ASIO
    test_06_live_tcp_typed_dto_crud();
#endif

    std::cout << "======================================================================\n";
    std::cout << "  ALL JSON & DTO TESTS PASSED SUCCESSFULLY!\n";
    std::cout << "======================================================================\n";
    return 0;
}
