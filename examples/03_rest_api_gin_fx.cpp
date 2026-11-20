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

#include "goforge/goforge.hpp"

using namespace std::chrono_literals;

// ----------------------------------------------------------------------------
// 1. Domain Services (Injected automatically into Gin-style Route Handlers!)
// ----------------------------------------------------------------------------
class IUserRepository {
public:
    virtual ~IUserRepository() = default;
    virtual void save(int id, std::string name) = 0;
    virtual std::optional<std::string> find_by_id(int id) = 0;
};

class InMemoryUserRepository final : public IUserRepository {
    gorun::RwMutex rw_mtx_;
    std::unordered_map<int, std::string> db_;

public:
    InMemoryUserRepository() {
        db_[1] = "Artur Havor";
        db_[2] = "Ada Lovelace";
    }

    void save(int id, std::string name) override {
        std::unique_lock<gorun::RwMutex> lk(rw_mtx_);
        db_[id] = std::move(name);
    }

    std::optional<std::string> find_by_id(int id) override {
        std::shared_lock<gorun::RwMutex> lk(rw_mtx_);
        auto it = db_.find(id);
        if (it == db_.end()) return std::nullopt;
        return it->second;
    }
};

// Scoped service: created fresh for each HTTP request and destroyed automatically at request end
struct RequestTrace {
    static inline std::atomic<std::uint64_t> seq{1000};
    std::string trace_id{"req-" + std::to_string(seq.fetch_add(1, std::memory_order_relaxed))};
};

// ----------------------------------------------------------------------------
// 2. Application Wiring (Uber Fx + Gin Style)
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
            // Global Middleware: Recovery + Structured Request Logging + 100ms Request Timeout
            router.use(goforge::web::middleware::recovery([&](const goforge::web::HttpContext& c, const std::string& err) {
                std::lock_guard<gorun::Mutex> lk(cout_mtx);
                std::cerr << "[RECOVERY] " << c.method() << " " << c.path() << " panicked: " << err << "\n";
            }));
            router.use(goforge::web::middleware::request_logger([&](const goforge::web::middleware::RequestLogEntry& e) {
                std::lock_guard<gorun::Mutex> lk(cout_mtx);
                std::cout << "[ACCESS] " << e.method << " " << e.path
                          << " -> " << e.status << " (" << e.latency_us << " us)\n";
            }));
            router.use(goforge::web::middleware::timeout(100ms));

            // Public healthcheck
            router.get("/health", [](goforge::web::HttpContext& c) {
                c.json(200, "{\"status\":\"ok\",\"framework\":\"goforge\"}");
            });

            // Protected API v1 group with Bearer Auth + gorun::Semaphore Rate Limiting
            auto api = router.group("/api/v1");
            api.use(goforge::web::middleware::rate_limit(64));
            api.use(goforge::web::middleware::bearer_auth([](std::string_view token) -> std::optional<std::string> {
                if (token == "demo-secret") return std::string("service-client");
                return std::nullopt;
            }));

            // DI-injected route handlers: `IUserRepository&` (Singleton) & `RequestTrace&` (Scoped)
            api.get("/users/:id", [](goforge::web::HttpContext& c, IUserRepository& repo, RequestTrace& trace) {
                int id = c.param_as<int>("id").value_or(-1);
                c.set_header("X-Trace-Id", trace.trace_id);
                auto name = repo.find_by_id(id);
                if (!name) {
                    c.json(404, "{\"error\":\"user_not_found\",\"trace\":\"" + trace.trace_id + "\"}");
                    return;
                }
                c.json(200, "{\"id\":" + std::to_string(id) + ",\"name\":\"" + *name +
                            "\",\"trace\":\"" + trace.trace_id + "\"}");
            });

            api.post("/users/:id", [](goforge::web::HttpContext& c, IUserRepository& repo, RequestTrace& trace) {
                int id = c.param_as<int>("id").value_or(-1);
                repo.save(id, c.body());
                c.set_header("X-Trace-Id", trace.trace_id);
                c.json(201, "{\"created_id\":" + std::to_string(id) + ",\"name\":\"" + c.body() + "\"}");
            });
        })
        .build();

    app->start();
    auto& server = app->services().resolve<goforge::http::HttpServer>();
    std::cout << "[goforge] REST API running at http://127.0.0.1:" << server.port() << "\n";

    if (stay_alive) {
        std::cout << "[goforge] Try: curl -H \"Authorization: Bearer demo-secret\" http://127.0.0.1:"
                  << server.port() << "/api/v1/users/1\n";
        app->run_until_signal();
        return 0;
    }

    // Built-in self-test client demonstrating GET /health, 401, POST + GET /api/v1/users/3
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
    call("GET /api/v1/users/1 HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
    call("GET /api/v1/users/1 HTTP/1.1\r\nHost: localhost\r\nAuthorization: Bearer demo-secret\r\nConnection: close\r\n\r\n");
    call("POST /api/v1/users/3 HTTP/1.1\r\nHost: localhost\r\nAuthorization: Bearer demo-secret\r\nContent-Length: 13\r\nConnection: close\r\n\r\nGrace Hopper!");
    call("GET /api/v1/users/3 HTTP/1.1\r\nHost: localhost\r\nAuthorization: Bearer demo-secret\r\nConnection: close\r\n\r\n");

    (void)app->stop();
    std::cout << "[goforge] REST API stopped gracefully.\n";
    return 0;
}
