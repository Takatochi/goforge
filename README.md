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
    GIT_TAG        v0.1.0-alpha
)
FetchContent_MakeAvailable(goforge)

add_executable(my_service main.cpp)
target_link_libraries(my_service PRIVATE goforge::goforge)
```

### Complete REST Microservice Example (Fx DI + Gin Router + Observability)

```cpp
#include "goforge/goforge.hpp"

using namespace std::chrono_literals;

struct IUserRepo {
    virtual ~IUserRepo() = default;
    virtual std::string find_json(int id) = 0;
};

struct MemoryUserRepo final : IUserRepo {
    gorun::RwMutex rw;
    std::string find_json(int id) override {
        std::shared_lock lk(rw);
        return "{\"id\":" + std::to_string(id) + ",\"name\":\"Artur Havor\"}";
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

            // DI-injected route handler: Singleton (IUserRepo&) + Scoped (RequestTrace&)
            api.get("/users/:id", [](goforge::web::HttpContext& c, IUserRepo& repo, RequestTrace& trace) {
                int id = c.param_as<int>("id").value_or(0);
                c.set_header("X-Trace-Id", trace.id);
                c.json(200, repo.find_json(id));
            });
        })
        .build();

    app->run_until_signal(); // Graceful SIGINT/SIGTERM draining + LIFO OnStop hooks!
}
```

---

## 3-Way HTTP/1.1 Performance Benchmarks (16 Logical Cores, Windows x64)

All three benchmark suites ([bench_goforge_http.cpp](./benchmarks/bench_goforge_http.cpp), [bench_go_http.go](./benchmarks/bench_go_http.go), [bench_std_http.cpp](./benchmarks/bench_std_http.cpp)) execute on the same machine (`GCC 16.2.0 -O3` vs `go1.27.4`):

| Workload | `goforge` (C++20 + `gorun` M:N + Asio) | Native Go 1.27 `net/http` | Standard C++20 (`std::jthread` per conn) |
| :--- | :--- | :--- | :--- |
| **1. Keep-Alive JSON API** (`32 x 500 = 16,000 reqs`) | **32,226 req/s**, **p50: 149 us** (`16000/16000 OK`) | 42,181 req/s, p50: 525 us (`16000/16000 OK`) | 255,517 req/s, p50: 33 us (no router/DI/metrics) |
| **2. 300 Concurrent I/O Handlers** (`10ms` wait) | **300/300 OK**, **fixed 16 OS workers** (zero thread explosion) | 213/300 OK (Windows ephemeral port burst), M:N goroutines | 300/300 OK, **spawns 300 OS kernel threads** |

Key takeaways:
- `goforge` delivers **3.5x lower median latency (`p50 = 149 us` vs `525 us`)** than Go 1.27 `net/http` while executing the full framework pipeline (`Router` + per-request DI `ServiceScope` + `MetricsRegistry`).
- Under 300 concurrent I/O-bound connections, `goforge` maintains a **fixed 16-worker M:N pool** with 0 extra OS threads, while thread-per-connection C++ spawns 300 OS kernel threads.

---

## Verification Test Suites (29 Automated Tests Across 4 Stages)

| Test Suite | Coverage | Result |
| :--- | :--- | :---: |
| [test_stage1_di.cpp](./tests/test_stage1_di.cpp) | 10 tests: `Singleton`/`Transient`/`Scoped`, auto-DI, missing/cycle/captive validation, `Lifecycle` FIFO/LIFO + rollback, `BlockingPool`, 200 concurrent scopes | **10/10 PASSED** |
| [test_stage2_http.cpp](./tests/test_stage2_http.cpp) | 7 tests: HTTP/1.1 Keep-Alive, exception isolation (`500`), protocol bounds (`400`/`413`), 300 concurrent requests with `gorun` primitives, zero OS-thread blocking proof | **7/7 PASSED** |
| [test_stage3_web.cpp](./tests/test_stage3_web.cpp) | 8 tests: Trie Router (`:param`, `*wildcard`, `404`, `405` + `Allow`), URL decoding, nested `RouteGroup`, DI handlers, `Recovery`, `Timeout` (`504`), `RateLimit` (`429`/queue), live CRUD | **8/8 PASSED** |
| [test_stage4_prod.cpp](./tests/test_stage4_prod.cpp) | 4 tests: RFC-8259 `StructuredLogger`, `MetricsRegistry` (`p50`/`p95`/`p99`, Prometheus), `HealthRegistry` (`/healthz`, `/readyz` 200/503), live graceful draining | **4/4 PASSED** |

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
go run benchmarks\bench_go_http.go
.\build\bench_std_http.exe
```

---

## Author & License

Created by **Artur Havor**. Distributed under the **[Apache License, Version 2.0](./LICENSE)**.
