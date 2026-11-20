#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
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

struct SimpleClientResponse {
    int status{0};
    std::string raw_headers;
    std::string body;
};

/// Minimal HTTP/1.1 response reader on top of non-blocking `AsyncTcpSocket` (supports Keep-Alive).
SimpleClientResponse read_http_response(goforge::net::AsyncTcpSocket& sock,
                                        std::string& buf,
                                        gorun::Context ctx = gorun::background()) {
    char chunk[4096];
    std::size_t header_end = std::string::npos;

    while ((header_end = buf.find("\r\n\r\n")) == std::string::npos) {
        auto [n, ec] = sock.read_some(chunk, sizeof(chunk), ctx);
        GOFORGE_ASSERT(!ec && n > 0);
        buf.append(chunk, n);
    }

    SimpleClientResponse resp;
    resp.raw_headers = buf.substr(0, header_end);

    // Status line: HTTP/1.1 200 OK
    auto sp1 = resp.raw_headers.find(' ');
    GOFORGE_ASSERT(sp1 != std::string::npos);
    auto sp2 = resp.raw_headers.find(' ', sp1 + 1);
    GOFORGE_ASSERT(sp2 != std::string::npos);
    resp.status = std::stoi(resp.raw_headers.substr(sp1 + 1, sp2 - (sp1 + 1)));

    // Parse Content-Length
    std::size_t content_length = 0;
    std::string lower_headers = goforge::http::detail::to_lower_ascii(resp.raw_headers);
    auto cl_pos = lower_headers.find("content-length:");
    if (cl_pos != std::string::npos) {
        auto line_end = lower_headers.find("\r\n", cl_pos);
        std::string_view val_sv = std::string_view(lower_headers).substr(
            cl_pos + 15,
            (line_end == std::string::npos ? lower_headers.size() : line_end) - (cl_pos + 15)
        );
        val_sv = goforge::http::detail::trim_ows(val_sv);
        auto [ptr, ec] = std::from_chars(val_sv.data(), val_sv.data() + val_sv.size(), content_length);
        GOFORGE_ASSERT(ec == std::errc{});
    }

    const std::size_t total_len = header_end + 4 + content_length;
    while (buf.size() < total_len) {
        auto [n, ec] = sock.read_some(chunk, sizeof(chunk), ctx);
        GOFORGE_ASSERT(!ec && n > 0);
        buf.append(chunk, n);
    }

    resp.body = buf.substr(header_end + 4, content_length);
    buf.erase(0, total_len);
    return resp;
}

SimpleClientResponse send_oneshot(goforge::net::AsioExecutor& client_io,
                                  std::uint16_t port,
                                  std::string_view raw_req) {
    goforge::net::AsyncTcpSocket sock(client_io.context());
    auto ec = sock.connect("127.0.0.1", port);
    GOFORGE_ASSERT(!ec);
    auto [written, wec] = sock.write_all(raw_req);
    GOFORGE_ASSERT(!wec && written == raw_req.size());
    std::string buf;
    auto resp = read_http_response(sock, buf);
    sock.close();
    return resp;
}

// Test 01: Basic HTTP/1.1 GET & POST with headers, query string, and body
void test_01_basic_get_and_post() {
    std::cout << "[TEST 01] Basic HTTP/1.1 GET & POST (headers, query, body, JSON)... ";

    goforge::http::HttpServer server(
        goforge::http::HttpServerConfig{.host = "127.0.0.1", .port = 0},
        [](const goforge::http::HttpRequest& req, gorun::Context) -> goforge::http::HttpResponse {
            if (req.method == "GET" && req.path == "/ping") {
                std::string token = req.headers.get_or("X-Trace-Id", "none");
                return goforge::http::HttpResponse::json(
                    200,
                    "{\"pong\":true,\"query\":\"" + req.raw_query + "\",\"trace\":\"" + token + "\"}"
                );
            }
            if (req.method == "POST" && req.path == "/echo") {
                auto resp = goforge::http::HttpResponse::text(201, req.body);
                resp.headers.set("X-Echo-Bytes", std::to_string(req.body.size()));
                return resp;
            }
            return goforge::http::HttpResponse::text(404, "not found");
        }
    );

    server.start();
    GORUN_DEFER { server.stop(); };

    goforge::net::AsioExecutor client_io(1);
    auto r1 = send_oneshot(
        client_io, server.port(),
        "GET /ping?lang=cpp20 HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "X-Trace-Id: trace-99\r\n"
        "Connection: close\r\n\r\n"
    );
    GOFORGE_ASSERT(r1.status == 200);
    GOFORGE_ASSERT(r1.body == "{\"pong\":true,\"query\":\"lang=cpp20\",\"trace\":\"trace-99\"}");

    std::string payload = "hello-goforge-payload-12345";
    auto r2 = send_oneshot(
        client_io, server.port(),
        "POST /echo HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Content-Length: " + std::to_string(payload.size()) + "\r\n"
        "Connection: close\r\n\r\n" + payload
    );
    GOFORGE_ASSERT(r2.status == 201);
    GOFORGE_ASSERT(r2.body == payload);
    GOFORGE_ASSERT(r2.raw_headers.find("X-Echo-Bytes: 27") != std::string::npos);

    std::cout << "PASSED\n";
}

// Test 02: HTTP/1.1 Keep-Alive (10 sequential requests over a single TCP connection)
void test_02_keep_alive_connection_reuse() {
    std::cout << "[TEST 02] HTTP/1.1 Keep-Alive (10 sequential requests on 1 socket)... ";

    std::atomic<int> counter{0};
    goforge::http::HttpServer server(
        goforge::http::HttpServerConfig{.host = "127.0.0.1", .port = 0},
        [&](const goforge::http::HttpRequest& req, gorun::Context) {
            int n = counter.fetch_add(1, std::memory_order_relaxed) + 1;
            return goforge::http::HttpResponse::text(200, req.path + ":" + std::to_string(n));
        }
    );

    server.start();
    GORUN_DEFER { server.stop(); };

    goforge::net::AsioExecutor client_io(1);
    goforge::net::AsyncTcpSocket sock(client_io.context());
    GOFORGE_ASSERT(!sock.connect("127.0.0.1", server.port()));

    std::string buf;
    for (int i = 1; i <= 10; ++i) {
        std::string req =
            "GET /step/" + std::to_string(i) + " HTTP/1.1\r\n"
            "Host: localhost\r\n" +
            std::string(i == 10 ? "Connection: close\r\n" : "") +
            "\r\n";
        auto [written, wec] = sock.write_all(req);
        GOFORGE_ASSERT(!wec && written == req.size());
        auto resp = read_http_response(sock, buf);
        GOFORGE_ASSERT(resp.status == 200);
        GOFORGE_ASSERT(resp.body == "/step/" + std::to_string(i) + ":" + std::to_string(i));
    }
    sock.close();
    GOFORGE_ASSERT(server.total_requests() == 10);

    std::cout << "PASSED\n";
}

// Test 03: Exception isolation — uncaught handler exception returns 500 without crashing server
void test_03_exception_isolation() {
    std::cout << "[TEST 03] Handler exception isolation (500 response, server stays healthy)... ";

    goforge::http::HttpServer server(
        goforge::http::HttpServerConfig{.host = "127.0.0.1", .port = 0},
        [](const goforge::http::HttpRequest& req, gorun::Context) -> goforge::http::HttpResponse {
            if (req.path == "/boom") {
                throw std::runtime_error("simulated handler panic");
            }
            return goforge::http::HttpResponse::text(200, "healthy");
        }
    );

    server.start();
    GORUN_DEFER { server.stop(); };

    goforge::net::AsioExecutor client_io(1);
    auto bad = send_oneshot(client_io, server.port(), "GET /boom HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
    GOFORGE_ASSERT(bad.status == 500);
    GOFORGE_ASSERT(bad.body.find("simulated handler panic") != std::string::npos);

    auto ok = send_oneshot(client_io, server.port(), "GET /ok HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
    GOFORGE_ASSERT(ok.status == 200 && ok.body == "healthy");

    std::cout << "PASSED\n";
}

// Test 04: Protocol hardening — malformed request (400) & oversized payload (413)
void test_04_malformed_and_oversized_requests() {
    std::cout << "[TEST 04] Protocol hardening (400 Bad Request & 413 Payload Too Large)... ";

    goforge::http::HttpServer server(
        goforge::http::HttpServerConfig{
            .host = "127.0.0.1",
            .port = 0,
            .max_body_bytes = 64
        },
        [](const goforge::http::HttpRequest&, gorun::Context) {
            return goforge::http::HttpResponse::text(200, "ok");
        }
    );

    server.start();
    GORUN_DEFER { server.stop(); };

    goforge::net::AsioExecutor client_io(1);

    auto bad = send_oneshot(client_io, server.port(), "INVALID_REQUEST_LINE\r\n\r\n");
    GOFORGE_ASSERT(bad.status == 400);

    auto too_big = send_oneshot(
        client_io, server.port(),
        "POST /upload HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Content-Length: 9999\r\n"
        "Connection: close\r\n\r\n"
    );
    GOFORGE_ASSERT(too_big.status == 413);

    std::cout << "PASSED\n";
}

// Test 05: High-concurrency load with gorun::sleep_for, gorun::Mutex, and gorun::Channel inside handlers
void test_05_concurrent_load_with_gorun_primitives() {
    std::cout << "[TEST 05] 300 concurrent HTTP requests with gorun::sleep_for + Mutex + Channel... ";

    gorun::Mutex shared_mtx;
    int protected_sum = 0;

    // Background worker consuming jobs from a gorun::Channel inside handlers!
    struct Job {
        int value;
        std::shared_ptr<gorun::Channel<int>> reply;
    };
    gorun::Channel<Job> job_ch(128);
    gorun::WaitGroup worker_wg;
    worker_wg.add(1);
    gorun::go([&]() {
        GORUN_DEFER { worker_wg.done(); };
        while (auto job = job_ch.recv()) {
            job->reply->send(job->value * 2);
        }
    });

    goforge::http::HttpServer server(
        goforge::http::HttpServerConfig{.host = "127.0.0.1", .port = 0, .io_threads = 2},
        [&](const goforge::http::HttpRequest&, gorun::Context) {
            // 1. Non-blocking green routine sleep (simulating external API latency)
            gorun::sleep_for(15ms);

            // 2. Non-blocking gorun::Channel round-trip to background worker
            auto reply = std::make_shared<gorun::Channel<int>>(1);
            job_ch.send(Job{21, reply});
            auto doubled = reply->recv();

            // 3. Non-blocking gorun::Mutex critical section
            {
                std::lock_guard<gorun::Mutex> lk(shared_mtx);
                protected_sum += doubled.value_or(0);
            }
            return goforge::http::HttpResponse::text(200, std::to_string(*doubled));
        }
    );

    server.start();
    goforge::net::AsioExecutor client_io(2);

    constexpr int kClients = 300;
    std::atomic<int> ok_count{0};
    auto t0 = std::chrono::steady_clock::now();

    {
        gorun::Scope client_scope;
        for (int i = 0; i < kClients; ++i) {
            gorun::go([&]() {
                auto resp = send_oneshot(
                    client_io, server.port(),
                    "GET /work HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n"
                );
                if (resp.status == 200 && resp.body == "42") {
                    ok_count.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }
    }

    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0
    ).count();

    server.stop();
    job_ch.close();
    worker_wg.wait();

    GOFORGE_ASSERT(ok_count.load() == kClients);
    GOFORGE_ASSERT(protected_sum == kClients * 42);
    GOFORGE_ASSERT(server.total_requests() == kClients);

    std::cout << "PASSED (" << kClients << "/300 OK in " << elapsed_ms << " ms)\n";
}

// Test 06: Slow green-routine handlers do NOT block fast handlers (zero OS-thread blocking proof)
void test_06_slow_handlers_do_not_block_fast_handlers() {
    std::cout << "[TEST 06] Slow handlers (100ms sleep) do not block fast handlers... ";

    goforge::http::HttpServer server(
        goforge::http::HttpServerConfig{.host = "127.0.0.1", .port = 0},
        [](const goforge::http::HttpRequest& req, gorun::Context) {
            if (req.path == "/slow") {
                gorun::sleep_for(100ms);
                return goforge::http::HttpResponse::text(200, "slow-done");
            }
            return goforge::http::HttpResponse::text(200, "fast-done");
        }
    );

    server.start();
    GORUN_DEFER { server.stop(); };

    goforge::net::AsioExecutor client_io(2);
    std::atomic<int> slow_done{0};
    std::atomic<int> fast_done{0};
    long long fast_elapsed_ms = 0;

    {
        gorun::Scope scope;
        // Launch 40 slow requests (more than CPU core count!)
        for (int i = 0; i < 40; ++i) {
            gorun::go([&]() {
                auto r = send_oneshot(client_io, server.port(), "GET /slow HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
                if (r.status == 200 && r.body == "slow-done") {
                    slow_done.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }

        gorun::sleep_for(10ms);

        // Now fire 20 fast requests while all 40 slow requests are parked in gorun::sleep_for(100ms)
        auto tf0 = std::chrono::steady_clock::now();
        {
            gorun::Scope fast_scope;
            for (int i = 0; i < 20; ++i) {
                gorun::go([&]() {
                    auto r = send_oneshot(client_io, server.port(), "GET /fast HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
                    if (r.status == 200 && r.body == "fast-done") {
                        fast_done.fetch_add(1, std::memory_order_relaxed);
                    }
                });
            }
        }
        fast_elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - tf0
        ).count();
    }

    GOFORGE_ASSERT(slow_done.load() == 40);
    GOFORGE_ASSERT(fast_done.load() == 20);
    GOFORGE_ASSERT(fast_elapsed_ms < 90); // Finished well before the 100ms slow handlers!

    std::cout << "PASSED (20 fast requests finished in " << fast_elapsed_ms << " ms while 40 slow slept)\n";
}

// Test 07: Integration with goforge::App + DI + Lifecycle graceful shutdown
void test_07_app_lifecycle_http_integration() {
    std::cout << "[TEST 07] App + DI + Lifecycle HttpServer graceful startup & shutdown... ";

    struct GreetingService {
        std::string prefix{"Hello from goforge DI: "};
        std::string greet(std::string_view path) const {
            return prefix + std::string(path);
        }
    };

    auto app = goforge::AppBuilder{}
        .provide_singleton<GreetingService>()
        .provide_instance(goforge::http::HttpServerConfig{.host = "127.0.0.1", .port = 0})
        .provide([](const goforge::http::HttpServerConfig& cfg,
                    GreetingService& greeter,
                    goforge::Lifecycle& lc) -> std::shared_ptr<goforge::http::HttpServer> {
            auto srv = std::make_shared<goforge::http::HttpServer>(
                cfg,
                [&greeter](const goforge::http::HttpRequest& req, gorun::Context) {
                    return goforge::http::HttpResponse::text(200, greeter.greet(req.path));
                }
            );
            srv->attach_lifecycle(lc, "http-listener");
            return srv;
        })
        .invoke([](goforge::http::HttpServer&) {
            // Triggers HttpServer creation & Lifecycle hook registration
        })
        .build();

    app->start();
    auto& srv = app->services().resolve<goforge::http::HttpServer>();
    GOFORGE_ASSERT(srv.port() != 0);

    goforge::net::AsioExecutor client_io(1);
    auto r = send_oneshot(client_io, srv.port(), "GET /stage2 HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
    GOFORGE_ASSERT(r.status == 200);
    GOFORGE_ASSERT(r.body == "Hello from goforge DI: /stage2");

    auto errs = app->stop();
    GOFORGE_ASSERT(errs.empty());
    GOFORGE_ASSERT(srv.active_connections() == 0);

    std::cout << "PASSED\n";
}

} // namespace

int main() {
    std::cout << std::unitbuf;
    std::cout << "======================================================================\n";
    std::cout << "  goforge Stage 2 Test Suite — Standalone Asio + gorun HTTP/1.1 Server\n";
    std::cout << "======================================================================\n";

    test_01_basic_get_and_post();
    test_02_keep_alive_connection_reuse();
    test_03_exception_isolation();
    test_04_malformed_and_oversized_requests();
    test_05_concurrent_load_with_gorun_primitives();
    test_06_slow_handlers_do_not_block_fast_handlers();
    test_07_app_lifecycle_http_integration();

    std::cout << "======================================================================\n";
    std::cout << "  ALL 7 STAGE 2 HTTP TESTS PASSED SUCCESSFULLY!\n";
    std::cout << "======================================================================\n";
    return 0;
}
