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

#include <asio.hpp>
#include "goforge/http/http_types.hpp"

using namespace std::chrono_literals;

namespace {

bool read_sync_response(asio::ip::tcp::socket& sock, std::string& buf) {
    char chunk[4096];
    std::error_code ec;
    std::size_t sep = std::string::npos;
    while ((sep = buf.find("\r\n\r\n")) == std::string::npos) {
        std::size_t n = sock.read_some(asio::buffer(chunk, sizeof(chunk)), ec);
        if (ec || n == 0) return false;
        buf.append(chunk, n);
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
        std::size_t n = sock.read_some(asio::buffer(chunk, sizeof(chunk)), ec);
        if (ec || n == 0) return false;
        buf.append(chunk, n);
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

void handle_sync_connection(asio::ip::tcp::socket sock) {
    std::error_code ec;
    sock.set_option(asio::ip::tcp::no_delay(true), ec);

    std::string buf;
    buf.reserve(4096);
    char chunk[4096];

    for (;;) {
        goforge::http::ParseHeaderResult parsed{};
        for (;;) {
            parsed = goforge::http::parse_http_headers(buf, 16 * 1024, 1024 * 1024);
            if (parsed.status != goforge::http::ParseStatus::Incomplete) break;
            std::size_t n = sock.read_some(asio::buffer(chunk, sizeof(chunk)), ec);
            if (ec || n == 0) return;
            buf.append(chunk, n);
        }
        if (parsed.status != goforge::http::ParseStatus::Ok) return;

        const std::size_t total = parsed.header_end_pos + parsed.content_length;
        while (buf.size() < total) {
            std::size_t n = sock.read_some(asio::buffer(chunk, sizeof(chunk)), ec);
            if (ec || n == 0) return;
            buf.append(chunk, n);
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
        asio::write(sock, asio::buffer(wire), ec);
        if (ec || !ka) return;
    }
}

} // namespace

int main() {
    std::cout << std::unitbuf;
    asio::io_context io;
    asio::ip::tcp::acceptor acceptor(io, asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const std::uint16_t port = acceptor.local_endpoint().port();

    std::atomic<bool> stop_flag{false};
    std::atomic<int> os_threads_spawned{0};
    std::mutex threads_mtx;
    std::vector<std::jthread> conn_threads;

    std::jthread accept_thread([&]() {
        while (!stop_flag.load(std::memory_order_acquire)) {
            std::error_code ec;
            asio::ip::tcp::socket peer(io);
            acceptor.accept(peer, ec);
            if (ec) break;
            os_threads_spawned.fetch_add(1, std::memory_order_relaxed);
            std::lock_guard lk(threads_mtx);
            conn_threads.emplace_back([s = std::move(peer)]() mutable {
                handle_sync_connection(std::move(s));
            });
        }
    });

    std::cout << "====================================================================================================\n";
    std::cout << "  Standard C++20 Thread-Per-Connection (std::jthread + blocking sockets) HTTP/1.1 Benchmark Suite\n";
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
                    asio::io_context cio;
                    asio::ip::tcp::socket sock(cio);
                    std::error_code ec;
                    sock.connect(asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), port), ec);
                    if (ec) return;
                    sock.set_option(asio::ip::tcp::no_delay(true), ec);

                    std::string buf;
                    buf.reserve(4096);
                    for (int r = 0; r < kReqsPerClient; ++r) {
                        auto r0 = std::chrono::steady_clock::now();
                        asio::write(sock, asio::buffer(raw_req), ec);
                        if (ec || !read_sync_response(sock, buf)) break;
                        auto dt = static_cast<std::uint64_t>(
                            std::chrono::duration_cast<std::chrono::microseconds>(
                                std::chrono::steady_clock::now() - r0
                            ).count()
                        );
                        per_client_lat[c].push_back(dt);
                        ok_reqs.fetch_add(1, std::memory_order_relaxed);
                    }
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

    // Workload 2: 300 concurrent clients hitting 10ms handler (forces 300 OS threads!)
    {
        constexpr int kClients = 300;
        const std::string raw_req = "GET /api/v1/io-wait HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
        std::atomic<int> ok_reqs{0};
        int before_threads = os_threads_spawned.load();

        auto t0 = std::chrono::steady_clock::now();
        {
            std::vector<std::jthread> clients;
            clients.reserve(kClients);
            for (int c = 0; c < kClients; ++c) {
                clients.emplace_back([&]() {
                    asio::io_context cio;
                    asio::ip::tcp::socket sock(cio);
                    std::error_code ec;
                    sock.connect(asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), port), ec);
                    if (ec) return;
                    asio::write(sock, asio::buffer(raw_req), ec);
                    std::string buf;
                    if (!ec && read_sync_response(sock, buf)) {
                        ok_reqs.fetch_add(1, std::memory_order_relaxed);
                    }
                });
            }
        }
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        double rps = static_cast<double>(ok_reqs.load()) / (ms / 1000.0);
        int spawned = os_threads_spawned.load() - before_threads;
        std::cout << "2. 300 Concurrent I/O Handlers (10ms sleep)    | "
                  << std::setw(8) << rps << " req/s | "
                  << "wall: " << std::setw(6) << ms << " ms | "
                  << "OS threads spawned: " << spawned << " | ok: " << ok_reqs.load() << "/" << kClients << "\n";
    }

    std::cout << "====================================================================================================\n";

    stop_flag.store(true, std::memory_order_release);
    std::error_code ec;
    acceptor.close(ec);
    return 0;
}
