#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "goforge/goforge.hpp"

using namespace std::chrono_literals;

#define GOFORGE_ASSERT(cond)                                                              \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            std::cerr << "\n[FAIL] Assertion failed at " << __FILE__ << ":" << __LINE__  \
                      << " -> " << #cond << "\n";                                         \
            std::exit(1);                                                                 \
        }                                                                                 \
    } while (0)

namespace {

goforge::http::HttpRequest make_req(std::string method,
                                    std::string target,
                                    std::string body = "",
                                    std::vector<std::pair<std::string, std::string>> hdrs = {}) {
    goforge::http::HttpRequest req;
    req.method = std::move(method);
    req.target = target;
    auto qpos = target.find('?');
    if (qpos == std::string::npos) {
        req.path = target;
    } else {
        req.path = target.substr(0, qpos);
        req.raw_query = target.substr(qpos + 1);
    }
    req.body = std::move(body);
    for (auto& [k, v] : hdrs) {
        req.headers.set(k, std::move(v));
    }
    return req;
}

struct LiveResp {
    int status{0};
    std::string raw_headers;
    std::string body;
};

LiveResp send_http(goforge::net::AsioExecutor& io, std::uint16_t port, std::string_view wire) {
    goforge::net::AsyncTcpSocket sock(io.context());
    GOFORGE_ASSERT(!sock.connect("127.0.0.1", port));
    auto [written, wec] = sock.write_all(wire);
    GOFORGE_ASSERT(!wec && written == wire.size());

    std::string raw;
    char buf[4096];
    for (;;) {
        auto [n, ec] = sock.read_some(buf, sizeof(buf));
        if (ec || n == 0) break;
        raw.append(buf, n);
    }
    sock.close();

    auto sep = raw.find("\r\n\r\n");
    GOFORGE_ASSERT(sep != std::string::npos);
    LiveResp out;
    out.raw_headers = raw.substr(0, sep);
    out.body = raw.substr(sep + 4);
    auto sp1 = out.raw_headers.find(' ');
    auto sp2 = out.raw_headers.find(' ', sp1 + 1);
    out.status = std::stoi(out.raw_headers.substr(sp1 + 1, sp2 - (sp1 + 1)));
    return out;
}

// Test 01: Static vs :param vs *wildcard routing priority, 404, and 405 Method Not Allowed
void test_01_router_matching_priority_and_405() {
    std::cout << "[TEST 01] Trie Router (:param, *wildcard, static priority, 404, 405)... ";

    goforge::web::Router router;
    router.get("/users/me", [](goforge::web::HttpContext& c) {
        c.text(200, "static:me");
    });
    router.get("/users/:id", [](goforge::web::HttpContext& c) {
        auto id = c.param_as<int>("id").value_or(-1);
        c.text(200, "param:" + std::to_string(id));
    });
    router.post("/users/:id", [](goforge::web::HttpContext& c) {
        c.text(201, "post:" + c.param_or("id"));
    });
    router.get("/orgs/:org/repos/:repo", [](goforge::web::HttpContext& c) {
        c.text(200, c.param_or("org") + "/" + c.param_or("repo"));
    });
    router.get("/static/*filepath", [](goforge::web::HttpContext& c) {
        c.text(200, "file:" + c.param_or("filepath"));
    });

    auto r_static = router.dispatch(make_req("GET", "/users/me"));
    GOFORGE_ASSERT(r_static.status == 200 && r_static.body == "static:me");

    auto r_param = router.dispatch(make_req("GET", "/users/42"));
    GOFORGE_ASSERT(r_param.status == 200 && r_param.body == "param:42");

    auto r_multi = router.dispatch(make_req("GET", "/orgs/takatochi/repos/goforge"));
    GOFORGE_ASSERT(r_multi.status == 200 && r_multi.body == "takatochi/goforge");

    auto r_wild = router.dispatch(make_req("GET", "/static/assets/css/app.min.css"));
    GOFORGE_ASSERT(r_wild.status == 200 && r_wild.body == "file:assets/css/app.min.css");

    auto r_404 = router.dispatch(make_req("GET", "/does/not/exist"));
    GOFORGE_ASSERT(r_404.status == 404);

    auto r_405 = router.dispatch(make_req("DELETE", "/users/42"));
    GOFORGE_ASSERT(r_405.status == 405);
    GOFORGE_ASSERT(r_405.headers.get_or("Allow") == "GET, POST");

    std::cout << "PASSED\n";
}

// Test 02: Query string parsing, URL decoding & typed conversions
void test_02_query_parsing_and_url_decoding() {
    std::cout << "[TEST 02] Query string parsing, URL decoding & param_as/query_as... ";

    goforge::web::Router router;
    router.get("/search/:category", [](goforge::web::HttpContext& c) {
        auto cat = c.param_or("category");
        auto q = c.query_or("q");
        int page = c.query_as<int>("page").value_or(1);
        int missing = c.query_as<int>("limit").value_or(20);
        c.text(200, cat + "|" + q + "|" + std::to_string(page) + "|" + std::to_string(missing));
    });

    auto r = router.dispatch(make_req("GET", "/search/c%2B%2B?q=hello+world%21&page=3"));
    GOFORGE_ASSERT(r.status == 200);
    GOFORGE_ASSERT(r.body == "c++|hello world!|3|20");

    std::cout << "PASSED\n";
}

// Test 03: Nested RouteGroups, middleware execution order & abort_with_json()
void test_03_route_groups_and_middleware_chain() {
    std::cout << "[TEST 03] Nested RouteGroups, middleware ordering & abort short-circuit... ";

    std::vector<std::string> trace;
    goforge::web::Router router;

    router.use([&](goforge::web::HttpContext& c) {
        trace.push_back("global:before");
        c.next();
        trace.push_back("global:after");
    });

    auto api = router.group("/api");
    api.use([&](goforge::web::HttpContext& c) {
        trace.push_back("api:before");
        c.next();
        trace.push_back("api:after");
    });

    auto v1 = api.group("/v1");
    v1.use(goforge::web::middleware::bearer_auth([](std::string_view token) -> std::optional<std::string> {
        if (token == "secret-jwt") return std::string("artur");
        return std::nullopt;
    }));
    v1.get("/profile", [&](goforge::web::HttpContext& c) {
        trace.push_back("handler:" + c.get<std::string>("auth_subject").value_or(""));
        c.json(200, "{\"user\":\"" + c.get<std::string>("auth_subject").value_or("") + "\"}");
    });

    // 3.1 Unauthorized -> aborted by bearer_auth, handler must NOT execute
    auto unauth = router.dispatch(make_req("GET", "/api/v1/profile"));
    GOFORGE_ASSERT(unauth.status == 401);
    std::vector<std::string> expected_unauth{"global:before", "api:before", "api:after", "global:after"};
    GOFORGE_ASSERT(trace == expected_unauth);

    // 3.2 Authorized -> full onion execution order
    trace.clear();
    auto ok = router.dispatch(make_req("GET", "/api/v1/profile", "", {{"Authorization", "Bearer secret-jwt"}}));
    GOFORGE_ASSERT(ok.status == 200 && ok.body == "{\"user\":\"artur\"}");
    std::vector<std::string> expected_ok{
        "global:before", "api:before", "handler:artur", "api:after", "global:after"
    };
    GOFORGE_ASSERT(trace == expected_ok);

    std::cout << "PASSED\n";
}

// Test 04: Auto-injected DI handlers with per-request ServiceScope (zero leaks across 100 requests)
struct RequestTrace {
    static inline std::atomic<int> live_instances{0};
    static inline std::atomic<int> seq{1};
    int id;
    RequestTrace() : id(seq.fetch_add(1)) { live_instances.fetch_add(1); }
    ~RequestTrace() { live_instances.fetch_sub(1); }
};

struct IGreetingRepo {
    virtual ~IGreetingRepo() = default;
    virtual std::string greeting_for(int id) const = 0;
};

struct MemoryGreetingRepo final : IGreetingRepo {
    std::string greeting_for(int id) const override {
        return "hi-" + std::to_string(id);
    }
};

void test_04_di_auto_injected_handlers_and_scoped_lifecycle() {
    std::cout << "[TEST 04] DI-injected handlers + automatic per-request ServiceScope (0 leaks)... ";

    RequestTrace::live_instances.store(0);
    RequestTrace::seq.store(1);

    goforge::ServiceCollection sc;
    sc.add_singleton_as<IGreetingRepo, MemoryGreetingRepo>()
      .add_scoped<RequestTrace>();
    auto provider = sc.build();

    goforge::web::Router router;
    router.get("/greet/:id", [](goforge::web::HttpContext& c, IGreetingRepo& repo, RequestTrace& tr) {
        int id = c.param_as<int>("id").value_or(0);
        // Same scoped instance if resolved manually too
        GOFORGE_ASSERT(c.resolve<RequestTrace>().id == tr.id);
        c.text(200, repo.greeting_for(id) + "#" + std::to_string(tr.id));
    });

    for (int i = 1; i <= 100; ++i) {
        auto r = router.dispatch(make_req("GET", "/greet/" + std::to_string(i)), gorun::background(), &provider);
        GOFORGE_ASSERT(r.status == 200);
        GOFORGE_ASSERT(r.body == "hi-" + std::to_string(i) + "#" + std::to_string(i));
    }
    GOFORGE_ASSERT(RequestTrace::live_instances.load() == 0);

    std::cout << "PASSED\n";
}

// Test 05: Recovery middleware catches exceptions and returns structured 500 JSON
void test_05_recovery_middleware() {
    std::cout << "[TEST 05] Recovery middleware catches handler exceptions cleanly... ";

    std::string captured_err;
    goforge::web::Router router;
    router.use(goforge::web::middleware::recovery([&](const goforge::web::HttpContext&, const std::string& msg) {
        captured_err = msg;
    }));
    router.get("/panic", [](goforge::web::HttpContext&) {
        throw std::runtime_error("db shard offline");
    });

    auto r = router.dispatch(make_req("GET", "/panic"));
    GOFORGE_ASSERT(r.status == 500);
    GOFORGE_ASSERT(captured_err == "db shard offline");
    GOFORGE_ASSERT(r.body.find("db shard offline") != std::string::npos);

    std::cout << "PASSED\n";
}

// Test 06: Timeout middleware cancels gorun::Context and returns 504 Gateway Timeout
void test_06_timeout_middleware() {
    std::cout << "[TEST 06] Timeout middleware cancels gorun::Context -> 504 Gateway Timeout... ";

    goforge::web::Router router;
    router.use(goforge::web::middleware::timeout(20ms));
    router.get("/slow", [](goforge::web::HttpContext& c) {
        if (!goforge::web::sleep_with_context(c.go_ctx(), 200ms)) {
            return; // Canceled cooperatively -> Timeout middleware writes 504!
        }
        c.text(200, "finished");
    });
    router.get("/fast", [](goforge::web::HttpContext& c) {
        if (goforge::web::sleep_with_context(c.go_ctx(), 2ms)) {
            c.text(200, "fast-ok");
        }
    });

    auto t0 = std::chrono::steady_clock::now();
    auto slow_r = router.dispatch(make_req("GET", "/slow"));
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0
    ).count();

    GOFORGE_ASSERT(slow_r.status == 504);
    GOFORGE_ASSERT(elapsed < 120); // Exited near 20ms, NOT 200ms!

    auto fast_r = router.dispatch(make_req("GET", "/fast"));
    GOFORGE_ASSERT(fast_r.status == 200 && fast_r.body == "fast-ok");

    std::cout << "PASSED (timed out in " << elapsed << " ms)\n";
}

// Test 07: RateLimit middleware (gorun::Semaphore) — 429 reject mode & queuing mode
void test_07_rate_limit_middleware() {
    std::cout << "[TEST 07] RateLimit middleware (gorun::Semaphore: 429 reject & bounded queue)... ";

    // 7.1 Immediate reject (max 2 concurrent -> 6 parallel requests => 2 OK, 4 HTTP 429)
    {
        goforge::web::Router router;
        router.use(goforge::web::middleware::rate_limit(2, /*reject_immediately=*/true));
        router.get("/limited", [](goforge::web::HttpContext& c) {
            gorun::sleep_for(25ms);
            c.text(200, "ok");
        });

        std::atomic<int> ok_count{0};
        std::atomic<int> rejected_429{0};
        {
            gorun::Scope scope;
            for (int i = 0; i < 6; ++i) {
                gorun::go([&]() {
                    auto r = router.dispatch(make_req("GET", "/limited"));
                    if (r.status == 200) ok_count.fetch_add(1);
                    else if (r.status == 429) rejected_429.fetch_add(1);
                });
            }
        }
        GOFORGE_ASSERT(ok_count.load() == 2);
        GOFORGE_ASSERT(rejected_429.load() == 4);
    }

    // 7.2 Queuing mode (max 3 concurrent -> 15 parallel requests => all 15 OK, active never > 3)
    {
        std::atomic<int> active{0};
        std::atomic<int> peak{0};
        goforge::web::Router router;
        router.use(goforge::web::middleware::rate_limit(3, /*reject_immediately=*/false));
        router.get("/queued", [&](goforge::web::HttpContext& c) {
            int cur = active.fetch_add(1) + 1;
            int prev = peak.load();
            while (cur > prev && !peak.compare_exchange_weak(prev, cur)) {}
            gorun::sleep_for(4ms);
            active.fetch_sub(1);
            c.text(200, "ok");
        });

        std::atomic<int> ok_count{0};
        {
            gorun::Scope scope;
            for (int i = 0; i < 15; ++i) {
                gorun::go([&]() {
                    auto r = router.dispatch(make_req("GET", "/queued"));
                    if (r.status == 200) ok_count.fetch_add(1);
                });
            }
        }
        GOFORGE_ASSERT(ok_count.load() == 15);
        GOFORGE_ASSERT(peak.load() <= 3);
    }

    std::cout << "PASSED\n";
}

// Test 08: Full end-to-end live-TCP REST service (HttpServer + Router + AppBuilder + DI + Middleware)
class UserStore {
    gorun::RwMutex rw_;
    std::unordered_map<int, std::string> users_;

public:
    void upsert(int id, std::string name) {
        std::unique_lock<gorun::RwMutex> lk(rw_);
        users_[id] = std::move(name);
    }

    std::optional<std::string> find(int id) {
        std::shared_lock<gorun::RwMutex> lk(rw_);
        auto it = users_.find(id);
        if (it == users_.end()) return std::nullopt;
        return it->second;
    }

    bool remove(int id) {
        std::unique_lock<gorun::RwMutex> lk(rw_);
        return users_.erase(id) > 0;
    }
};

void test_08_end_to_end_live_rest_service() {
    std::cout << "[TEST 08] End-to-end live TCP REST API (App + DI + Router + Auth + CRUD)... ";

    std::atomic<int> logged_requests{0};
    RequestTrace::live_instances.store(0);

    auto app = goforge::AppBuilder{}
        .provide_singleton<UserStore>()
        .provide_scoped<RequestTrace>()
        .provide_singleton<goforge::web::Router>()
        .provide_instance(goforge::http::HttpServerConfig{.host = "127.0.0.1", .port = 0})
        .provide([](const goforge::http::HttpServerConfig& cfg,
                    goforge::web::Router& router,
                    goforge::ServiceProvider& sp,
                    goforge::Lifecycle& lc) -> std::shared_ptr<goforge::http::HttpServer> {
            auto srv = std::make_shared<goforge::http::HttpServer>(cfg, router.to_http_handler(&sp));
            srv->attach_lifecycle(lc, "rest-http");
            return srv;
        })
        .invoke([&](goforge::web::Router& router, goforge::http::HttpServer&) {
            router.use(goforge::web::middleware::recovery());
            router.use(goforge::web::middleware::request_logger([&](const goforge::web::middleware::RequestLogEntry&) {
                logged_requests.fetch_add(1, std::memory_order_relaxed);
            }));

            auto v1 = router.group("/api/v1");
            v1.use(goforge::web::middleware::bearer_auth([](std::string_view tok) -> std::optional<std::string> {
                return tok == "dev-token" ? std::optional<std::string>("admin") : std::nullopt;
            }));

            v1.post("/users/:id", [](goforge::web::HttpContext& c, UserStore& store, RequestTrace& trace) {
                int id = c.param_as<int>("id").value_or(0);
                store.upsert(id, c.body());
                c.set_header("X-Trace-Id", std::to_string(trace.id));
                c.json(201, "{\"id\":" + std::to_string(id) + ",\"name\":\"" + c.body() + "\"}");
            });

            v1.get("/users/:id", [](goforge::web::HttpContext& c, UserStore& store) {
                int id = c.param_as<int>("id").value_or(0);
                auto name = store.find(id);
                if (!name) {
                    c.json(404, "{\"error\":\"user_not_found\"}");
                    return;
                }
                c.json(200, "{\"id\":" + std::to_string(id) + ",\"name\":\"" + *name + "\"}");
            });

            v1.del("/users/:id", [](goforge::web::HttpContext& c, UserStore& store) {
                int id = c.param_as<int>("id").value_or(0);
                if (!store.remove(id)) {
                    c.json(404, "{\"error\":\"user_not_found\"}");
                    return;
                }
                c.no_content();
            });
        })
        .build();

    app->start();
    auto port = app->services().resolve<goforge::http::HttpServer>().port();
    goforge::net::AsioExecutor client_io(1);

    // 1. Unauthenticated POST -> 401
    auto r_unauth = send_http(client_io, port,
        "POST /api/v1/users/7 HTTP/1.1\r\nHost: localhost\r\nContent-Length: 5\r\nConnection: close\r\n\r\nArtur");
    GOFORGE_ASSERT(r_unauth.status == 401);

    // 2. Authenticated POST -> 201 Created
    auto r_post = send_http(client_io, port,
        "POST /api/v1/users/7 HTTP/1.1\r\nHost: localhost\r\nAuthorization: Bearer dev-token\r\n"
        "Content-Length: 5\r\nConnection: close\r\n\r\nArtur");
    GOFORGE_ASSERT(r_post.status == 201);
    GOFORGE_ASSERT(r_post.body == "{\"id\":7,\"name\":\"Artur\"}");
    GOFORGE_ASSERT(r_post.raw_headers.find("X-Trace-Id:") != std::string::npos);

    // 3. GET -> 200 OK
    auto r_get = send_http(client_io, port,
        "GET /api/v1/users/7 HTTP/1.1\r\nHost: localhost\r\nAuthorization: Bearer dev-token\r\nConnection: close\r\n\r\n");
    GOFORGE_ASSERT(r_get.status == 200);
    GOFORGE_ASSERT(r_get.body == "{\"id\":7,\"name\":\"Artur\"}");

    // 4. DELETE -> 204 No Content
    auto r_del = send_http(client_io, port,
        "DELETE /api/v1/users/7 HTTP/1.1\r\nHost: localhost\r\nAuthorization: Bearer dev-token\r\nConnection: close\r\n\r\n");
    GOFORGE_ASSERT(r_del.status == 204);

    // 5. GET after DELETE -> 404
    auto r_gone = send_http(client_io, port,
        "GET /api/v1/users/7 HTTP/1.1\r\nHost: localhost\r\nAuthorization: Bearer dev-token\r\nConnection: close\r\n\r\n");
    GOFORGE_ASSERT(r_gone.status == 404);

    auto errs = app->stop();
    GOFORGE_ASSERT(errs.empty());
    GOFORGE_ASSERT(logged_requests.load() == 5);
    GOFORGE_ASSERT(RequestTrace::live_instances.load() == 0);

    std::cout << "PASSED\n";
}

} // namespace

int main() {
    std::cout << std::unitbuf;
    std::cout << "======================================================================\n";
    std::cout << "  goforge Stage 3 Test Suite — Gin Router, HttpContext & Middleware\n";
    std::cout << "======================================================================\n";

    test_01_router_matching_priority_and_405();
    test_02_query_parsing_and_url_decoding();
    test_03_route_groups_and_middleware_chain();
    test_04_di_auto_injected_handlers_and_scoped_lifecycle();
    test_05_recovery_middleware();
    test_06_timeout_middleware();
    test_07_rate_limit_middleware();
    test_08_end_to_end_live_rest_service();

    std::cout << "======================================================================\n";
    std::cout << "  ALL 8 STAGE 3 WEB LAYER TESTS PASSED SUCCESSFULLY!\n";
    std::cout << "======================================================================\n";
    return 0;
}
