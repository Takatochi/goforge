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

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "goforge/http/http_types.hpp"

using namespace std::chrono_literals;

namespace {

#if defined(_WIN32)
using RawSock = SOCKET;
inline constexpr RawSock kInvalidSock = INVALID_SOCKET;
inline void close_raw(RawSock s) { if (s != kInvalidSock) ::closesocket(s); }
struct WsaGuard {
    WsaGuard() { WSADATA w{}; ::WSAStartup(MAKEWORD(2, 2), &w); }
    ~WsaGuard() { ::WSACleanup(); }
};
#else
using RawSock = int;
inline constexpr RawSock kInvalidSock = -1;
inline void close_raw(RawSock s) { if (s >= 0) ::close(s); }
struct WsaGuard {};
#endif

RawSock connect_sync(std::uint16_t port) {
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

bool send_all(RawSock s, std::string_view data) {
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

void handle_sync_connection(RawSock sock) {
    int nodelay = 1;
    ::setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));

    std::string buf;
    buf.reserve(4096);
    char chunk[4096];

    for (;;) {
        goforge::http::ParseHeaderResult parsed{};
        for (;;) {
            parsed = goforge::http::parse_http_headers(buf, 16 * 1024, 1024 * 1024);
            if (parsed.status != goforge::http::ParseStatus::Incomplete) break;
            auto n = ::recv(sock, chunk, static_cast<int>(sizeof(chunk)), 0);
            if (n <= 0) { close_raw(sock); return; }
            buf.append(chunk, static_cast<std::size_t>(n));
        }
        if (parsed.status != goforge::http::ParseStatus::Ok) { close_raw(sock); return; }

        const std::size_t total = parsed.header_end_pos + parsed.content_length;
        while (buf.size() < total) {
            auto n = ::recv(sock, chunk, static_cast<int>(sizeof(chunk)), 0);
            if (n <= 0) { close_raw(sock); return; }
            buf.append(chunk, static_cast<std::size_t>(n));
        }
        auto req = std::move(parsed.request);
        buf.erase(0, total);

        bool ka = req.keep_alive();
        goforge::http::HttpResponse resp;
        if (req.path == "/api/v1/io-wait") {
            std::this_thread::sleep_for(10ms);
            resp = goforge::http::HttpResponse::json(200, "{\"waited_ms\":10}");
        } else {
            resp = goforge::http::HttpResponse::json(200, "{\"id\":42,\"status\":\"ok\"}");
        }

        std::string wire = resp.serialize(ka);
        if (!send_all(sock, wire) || !ka) { close_raw(sock); return; }
    }
}

} // namespace

int main() {
    WsaGuard wsa;
    std::cout << std::unitbuf;

    RawSock srv = ::socket(AF_INET, SOCK_STREAM, 0);
    int reuse = 1;
    ::setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = 0;
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    ::bind(srv, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
    ::listen(srv, 512);

    sockaddr_in actual{};
#if defined(_WIN32)
    int alen = sizeof(actual);
#else
    socklen_t alen = sizeof(actual);
#endif
    ::getsockname(srv, reinterpret_cast<sockaddr*>(&actual), &alen);
    const std::uint16_t port = ntohs(actual.sin_port);

    std::atomic<bool> stop_flag{false};
    std::atomic<int> os_threads_spawned{0};
    std::mutex threads_mtx;
    std::vector<std::jthread> conn_threads;

    std::jthread accept_thread([&]() {
        while (!stop_flag.load(std::memory_order_acquire)) {
            RawSock peer = ::accept(srv, nullptr, nullptr);
            if (peer == kInvalidSock) break;
            os_threads_spawned.fetch_add(1, std::memory_order_relaxed);
            std::lock_guard lk(threads_mtx);
            conn_threads.emplace_back([peer]() { handle_sync_connection(peer); });
        }
    });

    std::cout << "====================================================================================================\n";
    std::cout << "  Standard C++20 Thread-Per-Connection (std::jthread + raw OS sockets) HTTP/1.1 Benchmark Suite\n";
    std::cout << "====================================================================================================\n";

    // Workload 1: 32 Keep-Alive clients x 500 requests = 16,000 requests
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
                    RawSock sock = connect_sync(port);
                    if (sock == kInvalidSock) return;
                    std::string buf;
                    buf.reserve(4096);
                    for (int r = 0; r < kReqsPerClient; ++r) {
                        auto r0 = std::chrono::steady_clock::now();
                        if (!send_all(sock, raw_req) || !read_sync_response(sock, buf)) break;
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

    // Workload 2: 300 concurrent clients hitting 10ms handler
    {
        os_threads_spawned.store(0, std::memory_order_relaxed);
        constexpr int kClients = 300;
        const std::string raw_req = "GET /api/v1/io-wait HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
        std::atomic<int> ok_reqs{0};

        auto t0 = std::chrono::steady_clock::now();
        {
            std::vector<std::jthread> clients;
            clients.reserve(kClients);
            for (int c = 0; c < kClients; ++c) {
                clients.emplace_back([&]() {
                    RawSock sock = connect_sync(port);
                    if (sock == kInvalidSock) return;
                    std::string buf;
                    if (send_all(sock, raw_req) && read_sync_response(sock, buf)) {
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
                  << "OS threads spawned: " << os_threads_spawned.load()
                  << " | ok: " << ok_reqs.load() << "/" << kClients << "\n";
    }

    std::cout << "====================================================================================================\n";
    stop_flag.store(true, std::memory_order_release);
    close_raw(srv);
    if (accept_thread.joinable()) accept_thread.join();
    return 0;
}
