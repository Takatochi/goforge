<div align="center">
  <img src="./docs/logo.png" alt="goforge logo" width="160" style="border-radius: 24px;" />
  <h1><code>goforge</code> — Uber Fx + Gin Style C++20/23 Web Framework on <code>gorun</code></h1>
</div>

[![License: Apache 2.0](https://img.shields.io/badge/License-Apache_2.0-blue.svg)](./LICENSE)
[![C++20 / C++23](https://img.shields.io/badge/C%2B%2B-20%20%7C%2023-00599C?logo=c%2B%2B)](https://en.cppreference.com/w/cpp/20)
[![Header-Only](https://img.shields.io/badge/Library-Header--Only-success)](#quick-start--installation)
[![Built on gorun](https://img.shields.io/badge/Runtime-gorun%20M%3AN-34d399)](https://github.com/Takatochi/gorun)
[![Docs & Live Playground](https://img.shields.io/badge/Docs%20%26%20Live%20Playground-GitHub%20Pages-38bdf8)](https://takatochi.github.io/goforge/)

`goforge` is a standalone, header-only C++20/23 web and backend application framework built on top of the [`gorun`](https://github.com/Takatochi/gorun) M:N green threads runtime and header-only Standalone Asio.

It combines **Uber Fx-style Dependency Injection & Lifecycle**, **Gin-style Trie Routing & Middleware**, **Go-netpoll-style non-blocking TCP I/O**, and **Production Observability** — letting C++ developers write clean synchronous-looking handlers with zero callback hell and zero viral `co_await`.

Created by **Artur Havor** and licensed under the **Apache License 2.0**.

---

## Live Documentation & Interactive C++20 Playground

Explore the full documentation, visual benchmarks, and run real `goforge` C++20 code live on GCC 14.2 in the IntelliJ IDEA Darcula browser playground:

**https://takatochi.github.io/goforge/**

---

## Architecture

```text
+-------------------------------------------------------------------+
|                     Application (App / Fx-style)                  |
|  - Compile-Time Typed DI Container (Singleton, Transient, Scoped) |
|  - Lifecycle Hooks (FIFO OnStart, Rollback, LIFO OnStop)          |
|  - BlockingPool (Offloads synchronous DB/disk I/O off workers)    |
|  - Observability (StructuredLogger, MetricsRegistry, Healthz)     |
+---------------------------------+---------------------------------+
                                  | Dependency Injection
                                  v
+-------------------------------------------------------------------+
|                   HTTP / Web Layer (Gin-style)                    |
|  - Segment Trie Router (:id, *filepath, RouteGroup, 404/405)      |
|  - HttpContext (params, query, headers, body, RequestScope DI)    |
|  - Middleware Chain (Recovery, Timeout, RateLimit, Auth, Metrics) |
|  - 1 Connection / Request = 1 Green Routine: gorun::go(...)       |
+---------------------------------+---------------------------------+
                                  | Green Routines, Channels, Sync
                                  v
+-------------------------------------------------------------------+
|                       gorun Runtime (M:N)                         |
|  - Work-stealing scheduler, Channel<T>, Mutex, WaitGroup, Context |
|  - Go-netpoll-style non-blocking Standalone Asio <-> gorun bridge |
+-------------------------------------------------------------------+
```

---

## Why `goforge`? Key Advantages

| Capability | `goforge` (C++20 + `gorun`) | Go (Gin + Uber Fx) | C++20 `co_await` / Asio | Thread-per-Conn C++ |
| :--- | :--- | :--- | :--- | :--- |
| **Concurrency Model** | **1 green routine per connection** (fixed M:N OS workers) | 1 goroutine per connection | Viral `co_await` coroutines | 1 OS thread per connection |
| **Function Coloring** | **None** — sync-looking code with `gorun::Mutex` / `Channel` | None | Viral `Task<T>` everywhere | None |
| **DI Graph Validation** | **Compile-time typed + build-time Cycle & Captive checks** | Runtime reflection | Manual wiring | Manual wiring |
| **GC Pauses** | **Zero** (deterministic RAII + Scoped destruction) | GC stop-the-world / pacing | Zero | Zero |
| **Blocking C Drivers** | **Built-in `BlockingPool::run()`** (protects M:N workers) | `entersyscall` thread handoff | Manual thread pool | Blocks thread |
| **Built-In Observability** | **JSON Logger + Prometheus `/metrics` + `/healthz` / `/readyz`** | External libraries | Manual | Manual |

---

## Quick Start & Installation

### CMake `FetchContent` (Recommended)

```cmake
include(FetchContent)

FetchContent_Declare(
    goforge
    GIT_REPOSITORY https://github.com/Takatochi/goforge.git
    GIT_TAG        v0.2.0-alpha
)
FetchContent_MakeAvailable(goforge)

add_executable(my_service main.cpp)
target_link_libraries(my_service PRIVATE goforge::goforge)
```

### Complete REST Microservice Example (Fx DI + Gin Router + Typed JSON DTOs + Validation)

```cpp
#include "goforge/goforge.hpp"

using namespace std::chrono_literals;

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
};
GOFORGE_JSON_DTO(UserDto, id, name, email, age, bio)

struct IUserRepo {
    virtual ~IUserRepo() = default;
    virtual UserDto create(const CreateUserDto& in) = 0;
    virtual std::optional<UserDto> find(int id) = 0;
};

struct MemoryUserRepo final : IUserRepo {
    gorun::RwMutex rw;
    int next_id{2};
    std::unordered_map<int, UserDto> rows{{1, {1, "Artur Havor", "artur@goforge.dev", 25, "Creator"}}};

    UserDto create(const CreateUserDto& in) override {
        std::unique_lock lk(rw);
        int id = next_id++;
        UserDto u{id, in.name, in.email, in.age, in.bio};
        rows[id] = u;
        return u;
    }
    std::optional<UserDto> find(int id) override {
        std::shared_lock lk(rw);
        auto it = rows.find(id);
        return it == rows.end() ? std::nullopt : std::optional<UserDto>(it->second);
    }
};

struct RequestTrace {
    static inline std::atomic<int> seq{1000};
    std::string id{"req-" + std::to_string(seq.fetch_add(1))};
};

int main() {
    auto app = goforge::AppBuilder{}
        .provide_singleton_as<IUserRepo, MemoryUserRepo>()
        .provide_scoped<RequestTrace>()
        .provide_singleton<goforge::obs::StructuredLogger>()
        .provide_singleton<goforge::obs::MetricsRegistry>()
        .provide_singleton<goforge::obs::HealthRegistry>()
        .provide_singleton<goforge::web::Router>()
        .provide_instance(goforge::http::HttpServerConfig{.host = "127.0.0.1", .port = 8080, .io_threads = 2})
        .provide([](const goforge::http::HttpServerConfig& cfg,
                    goforge::web::Router& router,
                    goforge::ServiceProvider& sp,
                    goforge::Lifecycle& lc) {
            auto srv = std::make_shared<goforge::http::HttpServer>(cfg, router.to_http_handler(&sp));
            srv->attach_lifecycle(lc, "http-server");
            return srv;
        })
        .invoke([](goforge::web::Router& router,
                   goforge::obs::StructuredLogger& log,
                   goforge::obs::MetricsRegistry& metrics,
                   goforge::obs::HealthRegistry& health,
                   goforge::http::HttpServer&) {
            router.use(metrics.middleware());
            router.use(goforge::web::middleware::recovery());
            router.use(log.access_log_middleware());
            router.use(goforge::web::middleware::timeout(100ms));

            health.mount(router, metrics); // Mounts /healthz, /readyz, /metrics

            auto api = router.group("/api/v1");
            api.use(goforge::web::middleware::rate_limit(256));

            // POST /api/v1/users: auto JSON parse (400) + .validate() (422) + DTO response (201)
            api.post("/users", [](goforge::web::HttpContext& c, IUserRepo& repo) {
                auto input = c.bind_json<CreateUserDto>();
                if (!input) return;
                c.json(201, repo.create(*input));
            });

            // GET /api/v1/users/:id: Singleton (IUserRepo&) + Scoped (RequestTrace&) + c.error()
            api.get("/users/:id", [](goforge::web::HttpContext& c, IUserRepo& repo, RequestTrace& trace) {
                auto user = repo.find(c.param_as<int>("id").value_or(0));
                if (!user) {
                    c.error(404, "user_not_found", "Requested user does not exist");
                    return;
                }
                c.set_header("X-Trace-Id", trace.id);
                c.json(200, *user);
            });
        })
        .build();

    app->run_until_signal(); // Graceful SIGINT/SIGTERM draining + LIFO OnStop hooks!
}
```

---

## 4-Way HTTP/1.1 Performance Benchmarks (16 Logical Cores, Windows x64)

All four benchmark suites ([bench_goforge_http.cpp](./benchmarks/bench_goforge_http.cpp), [bench_asio_classic.cpp](./benchmarks/bench_asio_classic.cpp), [bench_go_http.go](./benchmarks/bench_go_http.go), [bench_std_http.cpp](./benchmarks/bench_std_http.cpp)) execute on the same machine (`GCC 16.2.0 -O3` vs `go1.27.4`) using identical client harnesses:

| Workload | `goforge` (Full Stack: Router + Scoped DI + Metrics + JSON) | Classic Async Asio (Raw Callbacks, 2 I/O threads) | Native Go 1.27 `net/http` | Standard C++20 (`std::jthread` per conn) |
| :--- | :--- | :--- | :--- | :--- |
| **1. Keep-Alive JSON API** (`32 x 500 = 16,000 reqs`) | **151,893 req/s**, **p50: 92 us**, **p99: 635 us** (`16000/16000`) | 122,001 req/s, p50: 239 us, p99: 448 us (`16000/16000`) | 42,504 req/s, p50: 524 us, p99: 4,234 us (`16000/16000`) | 165,680 req/s, p50: 36 us (raw socket) |
| **2. 300 Concurrent I/O Handlers** (`10ms` wait) | **300/300 OK**, **56.5 ms** (`5,309 req/s`), **fixed 16 M:N workers** | **300/300 OK**, **38.5 ms** (`7,793 req/s`), **2 I/O threads** | 214/300 OK (ephemeral ports), 42.6 ms (`5,024 req/s`), M:N | 300/300 OK, 61.6 ms, **spawns 300 OS threads** |

### `goforge` vs. Classic Async Standalone Asio: Engineering Breakdown

| Aspect | Classic Async Asio (Callbacks) | `goforge` (`gorun` M:N + Asio) |
| :--- | :--- | :--- |
| **Keep-Alive Throughput & Latency** | `122.0k req/s`, `p50: 239 us`, `p99: 448 us` (raw socket parser, no DI/Router/Metrics). | **`151.9k req/s` (+24.5% faster!)**, **`p50: 92 us` (2.6x lower!)**, **`p99: 635 us`** — with full Trie Router, Scoped DI, `nlohmann/json`, and lock-free Metrics! (And **3.57x faster RPS / 6.7x lower p99 than Go 1.27**.) |
| **300 Concurrent I/O Burst** | `38.5 ms` on 2 I/O threads (zero context switch). | **`56.5 ms` (`5,309 req/s`, 300/300 OK)** on fixed M:N workers — faster than `std::jthread` (`61.6 ms`) without spawning 300 OS threads! |
| **Handler Code Style** | `enable_shared_from_this`, manual `self` capture across nested lambdas, `asio::strand`, manual `steady_timer` state machine. | **Plain sequential C++**: `c.bind_json<Dto>()`, `gorun::Mutex`, `gorun::Channel`, `try/catch`, `GORUN_DEFER`, automatic DI injection. |
| **Exception & Lifetime Safety** | Unhandled exceptions inside callbacks escape to `io_context::run()`; stack variables cannot cross async boundaries. | Local stack variables live safely across every socket read/write/sleep (fixed 128 KB `gorun` stacks); `recovery()` isolates handler exceptions into `500 JSON`. |

#### Side-by-Side Code: Non-Blocking Wait + JSON Response

**Classic Async Standalone Asio (~35 lines of state machine & callback wiring per route):**
```cpp
class Session : public std::enable_shared_from_this<Session> {
    asio::ip::tcp::socket sock_;
    asio::steady_timer timer_;
    std::string write_buf_; // Must outlive async_write — cannot be a local stack variable!
    void handle_request(bool keep_alive) {
        auto self = shared_from_this();
        timer_.expires_after(10ms);
        timer_.async_wait([this, self, keep_alive](const std::error_code& ec) {
            if (ec) return;
            write_buf_ = HttpResponse::json(200, H{{"waited_ms", 10}}).serialize(keep_alive);
            asio::async_write(sock_, asio::buffer(write_buf_),
                [this, self, keep_alive](const std::error_code& wec, std::size_t) {
                    if (!wec && keep_alive) do_read();
                });
        });
    }
};
```

**`goforge` (4 lines — same non-blocking Asio engine under the hood, zero callbacks):**
```cpp
router.get("/api/v1/io-wait", [](goforge::web::HttpContext& c) {
    gorun::sleep_for(10ms); // Parks green routine without blocking OS workers!
    c.json(200, goforge::H{{"waited_ms", 10}});
});
```

---

## Verification Test Suites (35 Automated Tests Across 5 Suites)

| Test Suite | Coverage | Result |
| :--- | :--- | :---: |
| [test_stage1_di.cpp](./tests/test_stage1_di.cpp) | 10 tests: `Singleton`/`Transient`/`Scoped`, auto-DI, missing/cycle/captive validation, `Lifecycle` FIFO/LIFO + rollback, `BlockingPool`, 200 concurrent scopes | **10/10 PASSED** |
| [test_stage2_http.cpp](./tests/test_stage2_http.cpp) | 7 tests: HTTP/1.1 Keep-Alive, exception isolation (`500`), protocol bounds (`400`/`413`), 300 concurrent requests with `gorun` primitives, zero OS-thread blocking proof | **7/7 PASSED** |
| [test_stage3_web.cpp](./tests/test_stage3_web.cpp) | 8 tests: Trie Router (`:param`, `*wildcard`, `404`, `405` + `Allow`), URL decoding, nested `RouteGroup`, DI handlers, `Recovery`, `Timeout` (`504`), `RateLimit` (`429`/queue), live CRUD | **8/8 PASSED** |
| [test_stage4_prod.cpp](./tests/test_stage4_prod.cpp) | 4 tests: RFC-8259 `StructuredLogger`, `MetricsRegistry` (`p50`/`p95`/`p99`, Prometheus), `HealthRegistry` (`/healthz`, `/readyz` 200/503), live graceful draining | **4/4 PASSED** |
| [test_stage5_json.cpp](./tests/test_stage5_json.cpp) | 6 tests: `goforge::H`, `GOFORGE_JSON_DTO` / `GOFORGE_JSON_DTO_DEFAULT`, `std::optional<T>`, `c.bind_json<T>()` (`400` / `422` auto-validation), nested collections, live TCP DTO CRUD | **6/6 PASSED** |

---

## Building & Running Locally

```powershell
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure

# Examples:
.\build\goforge_example_01_di.exe
.\build\goforge_example_02_http.exe          # self-test (or --serve on :8080)
.\build\goforge_example_03_rest.exe          # full Fx + Gin REST API (or --serve on :8080)

# Benchmarks:
.\build\bench_goforge_http.exe
.\build\bench_asio_classic.exe
go run benchmarks\bench_go_http.go
.\build\bench_std_http.exe
```

---

## Author & License

Created by **Artur Havor**. Distributed under the **[Apache License, Version 2.0](./LICENSE)**.
