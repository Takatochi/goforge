#include <atomic>
#include <chrono>
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

using namespace std::chrono_literals;

// ----------------------------------------------------------------------------
// 1. Typed JSON DTOs with Automatic Validation (Zero raw JSON string literals!)
// ----------------------------------------------------------------------------
struct CreateUserDto {
    std::string name;
    std::string email;
    int age{0};
    std::optional<std::string> bio;

    std::optional<std::string> validate() const {
        if (name.empty()) return "name must not be empty";
        if (email.find('@') == std::string::npos) return "email must contain '@'";
        if (age < 13) return "age must be >= 13";
        return std::nullopt;
    }
};
GOFORGE_JSON_DTO_DEFAULT(CreateUserDto, name, email, age, bio)

struct UserDto {
    int id{0};
    std::string name;
    std::string email;
    int age{0};
    std::optional<std::string> bio;
    std::string trace_id;
};
GOFORGE_JSON_DTO(UserDto, id, name, email, age, bio, trace_id)

// ----------------------------------------------------------------------------
// 2. Domain Services (Injected automatically into Gin-style Route Handlers!)
// ----------------------------------------------------------------------------
class IUserRepository {
public:
    virtual ~IUserRepository() = default;
    virtual UserDto create(const CreateUserDto& input, std::string trace_id) = 0;
    virtual std::optional<UserDto> find_by_id(int id, std::string trace_id) = 0;
};

class InMemoryUserRepository final : public IUserRepository {
    gorun::RwMutex rw_mtx_;
    int next_id_{3};
    std::unordered_map<int, UserDto> db_;

public:
    InMemoryUserRepository() {
        db_[1] = UserDto{1, "Artur Havor", "artur@goforge.dev", 25, "Framework author", ""};
        db_[2] = UserDto{2, "Ada Lovelace", "ada@goforge.dev", 36, std::nullopt, ""};
    }

    UserDto create(const CreateUserDto& input, std::string trace_id) override {
        std::unique_lock<gorun::RwMutex> lk(rw_mtx_);
        int id = next_id_++;
        UserDto user{id, input.name, input.email, input.age, input.bio, std::move(trace_id)};
        db_[id] = user;
        return user;
    }

    std::optional<UserDto> find_by_id(int id, std::string trace_id) override {
        std::shared_lock<gorun::RwMutex> lk(rw_mtx_);
        auto it = db_.find(id);
        if (it == db_.end()) return std::nullopt;
        UserDto copy = it->second;
        copy.trace_id = std::move(trace_id);
        return copy;
    }
};

struct RequestTrace {
    static inline std::atomic<std::uint64_t> seq{1000};
    std::string trace_id{"req-" + std::to_string(seq.fetch_add(1, std::memory_order_relaxed))};
};

// ----------------------------------------------------------------------------
// 3. Application Wiring (Uber Fx + Gin + Typed JSON DTOs)
// ----------------------------------------------------------------------------
int main(int argc, char** argv) {
    const bool stay_alive = (argc > 1 && std::string_view(argv[1]) == "--serve");
    const std::uint16_t bind_port = stay_alive ? 8080 : 0;

    gorun::Mutex cout_mtx;

    auto app = goforge::AppBuilder{}
        .provide_singleton_as<IUserRepository, InMemoryUserRepository>()
        .provide_scoped<RequestTrace>()
        .provide_singleton<goforge::web::Router>()
        .provide_instance(goforge::http::HttpServerConfig{
            .host = "127.0.0.1",
            .port = bind_port,
            .io_threads = 2
        })
        .provide([](const goforge::http::HttpServerConfig& cfg,
                    goforge::web::Router& router,
                    goforge::ServiceProvider& sp,
                    goforge::Lifecycle& lc) -> std::shared_ptr<goforge::http::HttpServer> {
            auto server = std::make_shared<goforge::http::HttpServer>(cfg, router.to_http_handler(&sp));
            server->attach_lifecycle(lc, "goforge-rest-api");
            return server;
        })
        .invoke([&](goforge::web::Router& router, goforge::http::HttpServer&) {
            router.use(goforge::web::middleware::recovery());
            router.use(goforge::web::middleware::request_logger([&](const goforge::web::middleware::RequestLogEntry& e) {
                std::lock_guard<gorun::Mutex> lk(cout_mtx);
                std::cout << "[ACCESS] " << e.method << " " << e.path
                          << " -> " << e.status << " (" << e.latency_us << " us)\n";
            }));
            router.use(goforge::web::middleware::timeout(100ms));

            // Clean Gin-style goforge::H inline JSON object!
            router.get("/health", [](goforge::web::HttpContext& c) {
                c.json(200, goforge::H{{"status", "ok"}, {"framework", "goforge"}});
            });

            auto api = router.group("/api/v1");
            api.use(goforge::web::middleware::rate_limit(64));
            api.use(goforge::web::middleware::bearer_auth([](std::string_view token) -> std::optional<std::string> {
                if (token == "demo-secret") return std::string("service-client");
                return std::nullopt;
            }));

            // GET /api/v1/users/:id -> returns serialized UserDto or structured 404 JSON error
            api.get("/users/:id", [](goforge::web::HttpContext& c, IUserRepository& repo, RequestTrace& trace) {
                int id = c.param_as<int>("id").value_or(-1);
                c.set_header("X-Trace-Id", trace.trace_id);
                auto user = repo.find_by_id(id, trace.trace_id);
                if (!user) {
                    c.error(404, "user_not_found", "User #" + std::to_string(id) + " does not exist");
                    return;
                }
                c.json(200, *user);
            });

            // POST /api/v1/users -> automatic JSON binding + DTO validation (400 / 422) + 201 JSON response!
            api.post("/users", [](goforge::web::HttpContext& c, IUserRepository& repo, RequestTrace& trace) {
                auto payload = c.bind_json<CreateUserDto>();
                if (!payload) return; // Automatically replied 400 or 422 JSON!
                c.set_header("X-Trace-Id", trace.trace_id);
                c.json(201, repo.create(*payload, trace.trace_id));
            });
        })
        .build();

    app->start();
    auto& server = app->services().resolve<goforge::http::HttpServer>();
    std::cout << "[goforge] REST API running at http://127.0.0.1:" << server.port() << "\n";

    if (stay_alive) {
        std::cout << "[goforge] Press Ctrl+C to stop...\n";
        app->run_until_signal();
        return 0;
    }

    goforge::net::AsioExecutor client_io(1);
    auto call = [&](std::string_view raw_req) {
        goforge::net::AsyncTcpSocket sock(client_io.context());
        if (sock.connect("127.0.0.1", server.port())) return;
        (void)sock.write_all(raw_req);
        std::string resp;
        char buf[2048];
        for (;;) {
            auto [n, ec] = sock.read_some(buf, sizeof(buf));
            if (ec || n == 0) break;
            resp.append(buf, n);
        }
        auto sep = resp.find("\r\n\r\n");
        std::string body = (sep != std::string::npos) ? resp.substr(sep + 4) : resp;
        std::lock_guard<gorun::Mutex> lk(cout_mtx);
        std::cout << "  -> Response Body: " << body << "\n";
    };

    call("GET /health HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
    call("GET /api/v1/users/1 HTTP/1.1\r\nHost: localhost\r\nAuthorization: Bearer demo-secret\r\nConnection: close\r\n\r\n");

    std::string valid_json = goforge::H{
        {"name", "Grace Hopper"},
        {"email", "grace@navy.mil"},
        {"age", 40},
        {"bio", "COBOL pioneer"}
    }.dump();
    call("POST /api/v1/users HTTP/1.1\r\nHost: localhost\r\nAuthorization: Bearer demo-secret\r\n"
         "Content-Type: application/json\r\nContent-Length: " + std::to_string(valid_json.size()) +
         "\r\nConnection: close\r\n\r\n" + valid_json);

    std::string bad_json = goforge::H{{"name", "Bad"}, {"email", "invalid-email"}, {"age", 25}}.dump();
    call("POST /api/v1/users HTTP/1.1\r\nHost: localhost\r\nAuthorization: Bearer demo-secret\r\n"
         "Content-Type: application/json\r\nContent-Length: " + std::to_string(bad_json.size()) +
         "\r\nConnection: close\r\n\r\n" + bad_json);

    (void)app->stop();
    std::cout << "[goforge] REST API stopped gracefully.\n";
    return 0;
}
