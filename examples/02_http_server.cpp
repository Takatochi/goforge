#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

#include "goforge/goforge.hpp"

using namespace std::chrono_literals;

class RequestStatsService {
    gorun::Mutex mtx_;
    std::uint64_t hits_{0};

public:
    std::uint64_t record_hit() {
        std::lock_guard<gorun::Mutex> lk(mtx_);
        return ++hits_;
    }
};

int main(int argc, char** argv) {
    const bool stay_alive = (argc > 1 && std::string_view(argv[1]) == "--serve");
    const std::uint16_t desired_port = stay_alive ? 8080 : 0;

    auto app = goforge::AppBuilder{}
        .provide_singleton<RequestStatsService>()
        .provide_instance(goforge::http::HttpServerConfig{
            .host = "127.0.0.1",
            .port = desired_port,
            .io_threads = 2
        })
        .provide([](const goforge::http::HttpServerConfig& cfg,
                    RequestStatsService& stats,
                    goforge::Lifecycle& lc) -> std::shared_ptr<goforge::http::HttpServer> {
            auto server = std::make_shared<goforge::http::HttpServer>(
                cfg,
                [&stats](const goforge::http::HttpRequest& req, gorun::Context) -> goforge::http::HttpResponse {
                    // Simulate non-blocking downstream I/O latency inside green routine
                    gorun::sleep_for(5ms);
                    auto n = stats.record_hit();
                    return goforge::http::HttpResponse::json(
                        200,
                        "{\"framework\":\"goforge\",\"path\":\"" + req.path +
                        "\",\"hit\":" + std::to_string(n) + "}"
                    );
                }
            );
            server->attach_lifecycle(lc, "http-server");
            return server;
        })
        .invoke([](goforge::http::HttpServer&) {})
        .build();

    app->start();
    auto& server = app->services().resolve<goforge::http::HttpServer>();
    std::cout << "[goforge] HTTP/1.1 server listening on http://127.0.0.1:" << server.port() << "\n";

    if (stay_alive) {
        std::cout << "[goforge] Press Ctrl+C to stop...\n";
        app->run_until_signal();
        return 0;
    }

    // Self-test mode: fire 16 concurrent green-routine HTTP clients using native gorun::net and gracefully shut down
    gorun::Mutex cout_mtx;
    {
        gorun::Scope scope;
        for (int i = 1; i <= 16; ++i) {
            gorun::go([&, i]() {
                auto sock = gorun::net::TcpStream::connect("127.0.0.1", server.port());
                std::string raw =
                    "GET /api/item/" + std::to_string(i) + " HTTP/1.1\r\n"
                    "Host: localhost\r\n"
                    "Connection: close\r\n\r\n";
                (void)sock.write_all(raw);

                std::string resp;
                char buf[1024];
                for (;;) {
                    auto rd = sock.read(buf, sizeof(buf));
                    if (!rd.ok() || rd.eof()) break;
                    resp.append(buf, static_cast<std::size_t>(rd.bytes));
                }
                auto body_pos = resp.find("\r\n\r\n");
                std::string body = (body_pos != std::string::npos) ? resp.substr(body_pos + 4) : resp;
                std::lock_guard<gorun::Mutex> lk(cout_mtx);
                std::cout << "[Client #" << i << "] " << body << "\n";
            });
        }
    }

    std::cout << "[goforge] Served " << server.total_requests() << " requests. Shutting down gracefully...\n";
    (void)app->stop();
    std::cout << "[goforge] Shutdown complete.\n";
    return 0;
}
