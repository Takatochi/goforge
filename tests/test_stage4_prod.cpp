#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

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

goforge::http::HttpRequest make_req(std::string method, std::string target) {
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
    return req;
}

struct LiveResp {
    int status{0};
    std::string body;
};

LiveResp send_http(std::uint16_t port, std::string_view wire) {
    auto sock = gorun::net::TcpStream::connect("127.0.0.1", port);
    auto wr = sock.write_all(wire);
    GOFORGE_ASSERT(wr.ok() && static_cast<std::size_t>(wr.bytes) == wire.size());

    std::string raw;
    char buf[4096];
    for (;;) {
        auto rd = sock.read(buf, sizeof(buf));
        if (!rd.ok() || rd.eof()) break;
        raw.append(buf, static_cast<std::size_t>(rd.bytes));
    }
    sock.close();

    auto sep = raw.find("\r\n\r\n");
    GOFORGE_ASSERT(sep != std::string::npos);
    std::string head = raw.substr(0, sep);
    auto sp1 = head.find(' ');
    auto sp2 = head.find(' ', sp1 + 1);
    return LiveResp{
        .status = std::stoi(head.substr(sp1 + 1, sp2 - (sp1 + 1))),
        .body = raw.substr(sep + 4)
    };
}

// Test 01: StructuredLogger JSON formatting, escaping, level filtering & access middleware
void test_01_structured_json_logger() {
    std::cout << "[TEST 01] StructuredLogger JSON escaping, level filtering & access middleware... ";

    std::ostringstream sink;
    goforge::obs::StructuredLogger logger(goforge::obs::LogLevel::Info, sink);

    // Filtered out (Debug < Info)
    logger.debug("should_not_appear");
    GOFORGE_ASSERT(sink.str().empty());

    // Special characters escaped properly
    logger.warn("quote \"here\"\nnewline", {{"service", "auth\\v1"}});
    std::string line = sink.str();
    GOFORGE_ASSERT(line.find("\"level\":\"WARN\"") != std::string::npos);
    GOFORGE_ASSERT(line.find("quote \\\"here\\\"\\nnewline") != std::string::npos);
    GOFORGE_ASSERT(line.find("\"service\":\"auth\\\\v1\"") != std::string::npos);

    // Access middleware emits INFO for 200 and WARN for 404
    sink.str("");
    goforge::web::Router router;
    router.use(logger.access_log_middleware());
    router.get("/ping", [](goforge::web::HttpContext& c) { c.text(200, "pong"); });

    auto r200 = router.dispatch(make_req("GET", "/ping"));
    auto r404 = router.dispatch(make_req("GET", "/missing"));
    GOFORGE_ASSERT(r200.status == 200 && r404.status == 404);

    std::string access_logs = sink.str();
    GOFORGE_ASSERT(access_logs.find("\"msg\":\"http_request\"") != std::string::npos);
    GOFORGE_ASSERT(access_logs.find("\"status\":\"200\"") != std::string::npos);
    GOFORGE_ASSERT(access_logs.find("\"status\":\"404\"") != std::string::npos);

    std::cout << "PASSED\n";
}

// Test 02: MetricsRegistry counters, percentiles, JSON & Prometheus exposition, panic-safe in_flight
void test_02_metrics_registry_percentiles_and_prometheus() {
    std::cout << "[TEST 02] MetricsRegistry (2xx/4xx/5xx, p50/p95/p99, Prometheus, in_flight=0)... ";

    goforge::obs::MetricsRegistry metrics;
    goforge::web::Router router;
    router.use(metrics.middleware());
    router.use(goforge::web::middleware::recovery());

    router.get("/ok", [](goforge::web::HttpContext& c) { c.text(200, "ok"); });
    router.get("/bad", [](goforge::web::HttpContext& c) { c.text(400, "bad"); });
    router.get("/boom", [](goforge::web::HttpContext&) { throw std::runtime_error("boom"); });

    for (int i = 0; i < 10; ++i) {
        GOFORGE_ASSERT(router.dispatch(make_req("GET", "/ok")).status == 200);
    }
    GOFORGE_ASSERT(router.dispatch(make_req("GET", "/bad")).status == 400);
    GOFORGE_ASSERT(router.dispatch(make_req("GET", "/boom")).status == 500);

    auto snap = metrics.snapshot();
    GOFORGE_ASSERT(snap.total_requests == 12);
    GOFORGE_ASSERT(snap.in_flight == 0);
    GOFORGE_ASSERT(snap.status_2xx == 10);
    GOFORGE_ASSERT(snap.status_4xx == 1);
    GOFORGE_ASSERT(snap.status_5xx == 1);
    GOFORGE_ASSERT(snap.latency_p99_us >= snap.latency_p50_us);

    std::string json = metrics.to_json();
    GOFORGE_ASSERT(json.find("\"total_requests\":12") != std::string::npos);
    GOFORGE_ASSERT(json.find("\"p99\":") != std::string::npos);

    std::string prom = metrics.to_prometheus();
    GOFORGE_ASSERT(prom.find("goforge_http_requests_total 12") != std::string::npos);
    GOFORGE_ASSERT(prom.find("goforge_http_latency_microseconds{quantile=\"0.99\"}") != std::string::npos);

    std::cout << "PASSED\n";
}

// Test 03: HealthRegistry (/healthz, /readyz state transitions, /metrics JSON & Prometheus)
void test_03_health_probes_and_metrics_mount() {
    std::cout << "[TEST 03] HealthRegistry (/healthz 200, /readyz 200->503->200, /metrics)... ";

    std::atomic<bool> db_up{true};
    goforge::obs::MetricsRegistry metrics;
    goforge::obs::HealthRegistry health;

    health.add_check("postgres", [&]() -> goforge::obs::HealthCheckResult {
        if (db_up.load(std::memory_order_acquire)) {
            return {true, "connected"};
        }
        return {false, "connection pool exhausted"};
    });

    goforge::web::Router router;
    router.use(metrics.middleware());
    health.mount(router, metrics);

    auto live = router.dispatch(make_req("GET", "/healthz"));
    GOFORGE_ASSERT(live.status == 200 && live.body.find("alive") != std::string::npos);

    auto ready1 = router.dispatch(make_req("GET", "/readyz"));
    GOFORGE_ASSERT(ready1.status == 200 && ready1.body.find("\"status\":\"ready\"") != std::string::npos);

    db_up.store(false, std::memory_order_release);
    auto degraded = router.dispatch(make_req("GET", "/readyz"));
    GOFORGE_ASSERT(degraded.status == 503 && degraded.body.find("\"status\":\"degraded\"") != std::string::npos);

    db_up.store(true, std::memory_order_release);
    auto recovered = router.dispatch(make_req("GET", "/readyz"));
    GOFORGE_ASSERT(recovered.status == 200);

    auto m_json = router.dispatch(make_req("GET", "/metrics"));
    GOFORGE_ASSERT(m_json.status == 200 && m_json.body.find("\"total_requests\":") != std::string::npos);

    auto m_prom = router.dispatch(make_req("GET", "/metrics?format=prometheus"));
    GOFORGE_ASSERT(m_prom.status == 200 && m_prom.body.find("goforge_http_requests_total") != std::string::npos);

    std::cout << "PASSED\n";
}

// Test 04: Live TCP production service with observability stack + graceful draining
void test_04_live_production_service_and_graceful_draining() {
    std::cout << "[TEST 04] Live TCP production service (/healthz, /readyz, /metrics) & graceful drain... ";

    std::ostringstream log_sink;

    auto app = goforge::AppBuilder{}
        .provide_instance(std::make_shared<goforge::obs::StructuredLogger>(goforge::obs::LogLevel::Info, log_sink))
        .provide_singleton<goforge::obs::MetricsRegistry>()
        .provide_singleton<goforge::obs::HealthRegistry>()
        .provide_singleton<goforge::web::Router>()
        .provide_instance(goforge::http::HttpServerConfig{.host = "127.0.0.1", .port = 0})
        .provide([](const goforge::http::HttpServerConfig& cfg,
                    goforge::web::Router& router,
                    goforge::ServiceProvider& sp,
                    goforge::Lifecycle& lc) -> std::shared_ptr<goforge::http::HttpServer> {
            auto srv = std::make_shared<goforge::http::HttpServer>(cfg, router.to_http_handler(&sp));
            srv->attach_lifecycle(lc, "prod-http");
            return srv;
        })
        .invoke([](goforge::web::Router& router,
                   goforge::obs::StructuredLogger& logger,
                   goforge::obs::MetricsRegistry& metrics,
                   goforge::obs::HealthRegistry& health,
                   goforge::http::HttpServer&) {
            router.use(metrics.middleware());
            router.use(goforge::web::middleware::recovery());
            router.use(logger.access_log_middleware());

            health.add_check("runtime", []() { return goforge::obs::HealthCheckResult{true, "gorun M:N ok"}; });
            health.mount(router, metrics);

            router.get("/api/work", [](goforge::web::HttpContext& c) {
                gorun::sleep_for(10ms);
                c.json(200, "{\"done\":true}");
            });
        })
        .build();

    app->start();
    auto port = app->services().resolve<goforge::http::HttpServer>().port();

    auto r1 = send_http(port, "GET /api/work HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
    GOFORGE_ASSERT(r1.status == 200 && r1.body == "{\"done\":true}");

    auto r2 = send_http(port, "GET /readyz HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
    GOFORGE_ASSERT(r2.status == 200);

    auto r3 = send_http(port, "GET /metrics HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
    GOFORGE_ASSERT(r3.status == 200 && r3.body.find("\"total_requests\":2") != std::string::npos);

    auto errs = app->stop();
    GOFORGE_ASSERT(errs.empty());
    GOFORGE_ASSERT(app->services().resolve<goforge::obs::MetricsRegistry>().snapshot().in_flight == 0);
    GOFORGE_ASSERT(!log_sink.str().empty());

    std::cout << "PASSED\n";
}

} // namespace

int main() {
    std::cout << std::unitbuf;
    std::cout << "======================================================================\n";
    std::cout << "  goforge Stage 4 Test Suite — Logging, Metrics, Health & Shutdown\n";
    std::cout << "======================================================================\n";

    test_01_structured_json_logger();
    test_02_metrics_registry_percentiles_and_prometheus();
    test_03_health_probes_and_metrics_mount();
    test_04_live_production_service_and_graceful_draining();

    std::cout << "======================================================================\n";
    std::cout << "  ALL 4 STAGE 4 PRODUCTION TESTS PASSED SUCCESSFULLY!\n";
    std::cout << "======================================================================\n";
    return 0;
}
