#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "goforge/goforge.hpp"

using namespace std::chrono_literals;

namespace {

#if defined(_WIN32)
using RawSock = SOCKET;
inline constexpr RawSock kInvalidSock = INVALID_SOCKET;
inline void close_raw(RawSock s) { if (s != kInvalidSock) ::closesocket(s); }
#else
using RawSock = int;
inline constexpr RawSock kInvalidSock = -1;
inline void close_raw(RawSock s) { if (s >= 0) ::close(s); }
#endif

RawSock connect_sync_client(std::uint16_t port) {
    RawSock s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s == kInvalidSock) return kInvalidSock;
    int nodelay = 1;
    ::setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (::connect(s, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
        close_raw(s);
        return kInvalidSock;
    }
    return s;
}

bool send_all_sync(RawSock s, std::string_view data) {
    std::size_t off = 0;
    while (off < data.size()) {
        auto n = ::send(s, data.data() + off, static_cast<int>(data.size() - off), 0);
        if (n <= 0) return false;
        off += static_cast<std::size_t>(n);
    }
    return true;
}

bool read_sync_response(RawSock sock, std::string& buf) {
    char chunk[4096];
    std::size_t sep = std::string::npos;
    while ((sep = buf.find("\r\n\r\n")) == std::string::npos) {
        auto n = ::recv(sock, chunk, static_cast<int>(sizeof(chunk)), 0);
        if (n <= 0) return false;
        buf.append(chunk, static_cast<std::size_t>(n));
    }

    std::size_t content_length = 0;
    std::string lower = goforge::http::detail::to_lower_ascii(std::string_view(buf.data(), sep));
    auto cl_pos = lower.find("content-length:");
    if (cl_pos != std::string::npos) {
        auto eol = lower.find("\r\n", cl_pos);
        auto sv = goforge::http::detail::trim_ows(
            std::string_view(lower).substr(cl_pos + 15, (eol == std::string::npos ? lower.size() : eol) - (cl_pos + 15))
        );
        std::from_chars(sv.data(), sv.data() + sv.size(), content_length);
    }

    const std::size_t total = sep + 4 + content_length;
    while (buf.size() < total) {
        auto n = ::recv(sock, chunk, static_cast<int>(sizeof(chunk)), 0);
        if (n <= 0) return false;
        buf.append(chunk, static_cast<std::size_t>(n));
    }
    buf.erase(0, total);
    return true;
}

std::uint64_t pct(const std::vector<std::uint64_t>& sorted, double q) {
    if (sorted.empty()) return 0;
    std::size_t idx = static_cast<std::size_t>(std::ceil(q * static_cast<double>(sorted.size()))) - 1;
    if (idx >= sorted.size()) idx = sorted.size() - 1;
    return sorted[idx];
}

} // namespace

int main() {
    std::cout << std::unitbuf;

    auto app = goforge::WebApp::create({.host = "127.0.0.1", .port = 0})
        .use_metrics();

    app.get("/api/v1/users/:id", [](goforge::web::HttpContext& c) {
        c.json(200, goforge::H{{"id", c.param_or("id")}, {"status", "ok"}});
    });
    app.get("/api/v1/io-wait", [](goforge::web::HttpContext& c) {
        gorun::sleep_for(10ms);
        c.json(200, goforge::H{{"waited_ms", 10}});
    });

    app.start();
    const std::uint16_t port = app.bound_port();

    std::cout << "====================================================================================================\n";
    std::cout << "  goforge (100% Native gorun M:N + gorun::net IOCP/epoll/kqueue — ZERO Asio!) HTTP/1.1 Benchmark\n";
    std::cout << "  Arch: " << gorun::detail::kArchitectureName
              << " | Active gorun Workers: " << gorun::default_runtime().worker_count() << "\n";
    std::cout << "====================================================================================================\n";

    // Workload 1: High-throughput Keep-Alive JSON API (32 concurrent clients x 500 requests = 16,000 reqs)
    {
        constexpr int kClients = 32;
        constexpr int kReqsPerClient = 500;
        constexpr int kTotal = kClients * kReqsPerClient;
        const std::string raw_req = "GET /api/v1/users/42 HTTP/1.1\r\nHost: localhost\r\n\r\n";

        std::vector<std::vector<std::uint64_t>> per_client_lat(kClients);
        std::atomic<int> ok_reqs{0};

        auto t0 = std::chrono::steady_clock::now();
        {
            std::vector<std::jthread> clients;
            clients.reserve(kClients);
            for (int c = 0; c < kClients; ++c) {
                per_client_lat[c].reserve(kReqsPerClient);
                clients.emplace_back([&, c]() {
                    RawSock sock = connect_sync_client(port);
                    if (sock == kInvalidSock) return;

                    std::string buf;
                    buf.reserve(4096);
                    for (int r = 0; r < kReqsPerClient; ++r) {
                        auto r0 = std::chrono::steady_clock::now();
                        if (!send_all_sync(sock, raw_req) || !read_sync_response(sock, buf)) break;
                        auto dt = static_cast<std::uint64_t>(
                            std::chrono::duration_cast<std::chrono::microseconds>(
                                std::chrono::steady_clock::now() - r0
                            ).count()
                        );
                        per_client_lat[c].push_back(dt);
                        ok_reqs.fetch_add(1, std::memory_order_relaxed);
                    }
                    close_raw(sock);
                });
            }
        }
        double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

        std::vector<std::uint64_t> all_lat;
        all_lat.reserve(kTotal);
        for (const auto& v : per_client_lat) all_lat.insert(all_lat.end(), v.begin(), v.end());
        std::sort(all_lat.begin(), all_lat.end());

        double rps = static_cast<double>(ok_reqs.load()) / sec;
        std::cout << std::fixed << std::setprecision(1);
        std::cout << "1. Keep-Alive JSON API (32x500 = 16,000 reqs)  | "
                  << std::setw(8) << rps << " req/s | "
                  << "p50: " << std::setw(4) << pct(all_lat, 0.50) << " us | "
                  << "p99: " << std::setw(5) << pct(all_lat, 0.99) << " us | "
                  << "ok: " << ok_reqs.load() << "/" << kTotal << "\n";
    }

    // Workload 2: High-concurrency I/O Wait (300 concurrent clients hitting 10ms handler)
    {
        constexpr int kClients = 300;
        const std::string raw_req = "GET /api/v1/io-wait HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
        std::atomic<int> ok_reqs{0};

        auto t0 = std::chrono::steady_clock::now();
        {
            std::vector<std::jthread> clients;
            clients.reserve(kClients);
            for (int c = 0; c < kClients; ++c) {
                clients.emplace_back([&]() {
                    RawSock sock = connect_sync_client(port);
                    if (sock == kInvalidSock) return;
                    std::string buf;
                    if (send_all_sync(sock, raw_req) && read_sync_response(sock, buf)) {
                        ok_reqs.fetch_add(1, std::memory_order_relaxed);
                    }
                    close_raw(sock);
                });
            }
        }
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        double rps = static_cast<double>(ok_reqs.load()) / (ms / 1000.0);
        std::cout << "2. 300 Concurrent I/O Handlers (10ms sleep)    | "
                  << std::setw(8) << rps << " req/s | "
                  << "wall: " << std::setw(6) << ms << " ms | "
                  << "OS threads: fixed M:N | ok: " << ok_reqs.load() << "/" << kClients << "\n";
    }

    std::cout << "====================================================================================================\n";
    (void)app.stop();
    return 0;
}
