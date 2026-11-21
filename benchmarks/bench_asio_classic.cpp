#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <asio.hpp>
#include "goforge/http/http_types.hpp"

using namespace std::chrono_literals;

namespace {

std::uint64_t pct(const std::vector<std::uint64_t>& sorted, double q) {
    if (sorted.empty()) return 0;
    std::size_t idx = static_cast<std::size_t>(std::ceil(q * static_cast<double>(sorted.size()))) - 1;
    if (idx >= sorted.size()) idx = sorted.size() - 1;
    return sorted[idx];
}

// ============================================================================
// Classic Callback-Driven Standalone Asio HTTP/1.1 Server
// Requires: enable_shared_from_this, nested async callbacks, explicit buffer
//           lifetime management, and asio::steady_timer for non-blocking wait.
// ============================================================================
class ClassicAsioSession : public std::enable_shared_from_this<ClassicAsioSession> {
    asio::ip::tcp::socket sock_;
    asio::steady_timer timer_;
    char chunk_[4096];
    std::string recv_buf_;
    std::string write_buf_;

    void do_read() {
        auto self = shared_from_this();
        sock_.async_read_some(asio::buffer(chunk_, sizeof(chunk_)),
            [this, self](const std::error_code& ec, std::size_t n) {
                if (ec || n == 0) return;
                recv_buf_.append(chunk_, n);
                process_buffer();
            });
    }

    void process_buffer() {
        auto parsed = goforge::http::parse_http_headers(recv_buf_, 16 * 1024, 1024 * 1024);
        if (parsed.status == goforge::http::ParseStatus::Incomplete) {
            do_read();
            return;
        }
        if (parsed.status != goforge::http::ParseStatus::Ok) {
            return;
        }

        const std::size_t total = parsed.header_end_pos + parsed.content_length;
        if (recv_buf_.size() < total) {
            do_read();
            return;
        }

        const bool keep_alive = parsed.request.keep_alive();
        const bool is_io_wait = (parsed.request.path == "/api/v1/io-wait");
        recv_buf_.erase(0, total);

        if (is_io_wait) {
            auto self = shared_from_this();
            timer_.expires_after(10ms);
            timer_.async_wait([this, self, keep_alive](const std::error_code& ec) {
                if (ec) return;
                write_buf_ = goforge::http::HttpResponse::json(200, "{\"waited_ms\":10}").serialize(keep_alive);
                do_write(keep_alive);
            });
        } else {
            write_buf_ = goforge::http::HttpResponse::json(200, "{\"id\":42,\"status\":\"ok\"}").serialize(keep_alive);
            do_write(keep_alive);
        }
    }

    void do_write(bool keep_alive) {
        auto self = shared_from_this();
        asio::async_write(sock_, asio::buffer(write_buf_),
            [this, self, keep_alive](const std::error_code& ec, std::size_t) {
                if (ec || !keep_alive) return;
                if (!recv_buf_.empty()) {
                    process_buffer();
                } else {
                    do_read();
                }
            });
    }

public:
    explicit ClassicAsioSession(asio::ip::tcp::socket sock)
        : sock_(std::move(sock))
        , timer_(sock_.get_executor()) {
        std::error_code ignored;
        sock_.set_option(asio::ip::tcp::no_delay(true), ignored);
        recv_buf_.reserve(4096);
    }

    void start() {
        do_read();
    }
};

class ClassicAsioServer {
    asio::io_context& io_;
    asio::ip::tcp::acceptor acceptor_;

    void do_accept() {
        acceptor_.async_accept([this](const std::error_code& ec, asio::ip::tcp::socket peer) {
            if (!ec) {
                std::make_shared<ClassicAsioSession>(std::move(peer))->start();
                do_accept();
            }
        });
    }

public:
    explicit ClassicAsioServer(asio::io_context& io)
        : io_(io)
        , acceptor_(io, asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0)) {
        (void)io_;
        do_accept();
    }

    std::uint16_t port() const { return acceptor_.local_endpoint().port(); }
    void close() {
        std::error_code ignored;
        acceptor_.close(ignored);
    }
};

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

} // namespace

int main() {
    std::cout << std::unitbuf;

    constexpr std::size_t kIoThreads = 2;
    asio::io_context server_io(static_cast<int>(kIoThreads));
    auto work = asio::make_work_guard(server_io);
    ClassicAsioServer server(server_io);
    const std::uint16_t port = server.port();

    std::vector<std::jthread> io_threads;
    io_threads.reserve(kIoThreads);
    for (std::size_t i = 0; i < kIoThreads; ++i) {
        io_threads.emplace_back([&]() { server_io.run(); });
    }

    std::cout << "====================================================================================================\n";
    std::cout << "  Classic Async Standalone Asio (callbacks + steady_timer, " << kIoThreads << " I/O threads) HTTP/1.1 Benchmark\n";
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

    // Workload 2: 300 concurrent clients hitting 10ms async steady_timer handler
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
        std::cout << "2. 300 Concurrent I/O Handlers (10ms timer)    | "
                  << std::setw(8) << rps << " req/s | "
                  << "wall: " << std::setw(6) << ms << " ms | "
                  << "OS threads: " << kIoThreads << " I/O | ok: " << ok_reqs.load() << "/" << kClients << "\n";
    }

    std::cout << "====================================================================================================\n";

    server.close();
    work.reset();
    server_io.stop();
    return 0;
}
