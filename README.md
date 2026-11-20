# goforge — C++20/23 Fx + Gin Style Application & Web Framework on `gorun`

`goforge` is a standalone, header-only C++20/23 web and backend application framework built on top of the [`gorun`](https://github.com/Takatochi/gorun) M:N green threads runtime and Standalone Asio.

Created by **Artur Havor** and licensed under the **Apache License 2.0**.

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

## Completed Implementation Stages (Stages 1–4)

- **Stage 1: Fx-Style DI Container, Lifecycle & BlockingPool**
  - `ServiceCollection`, `ServiceProvider`, `ServiceScope` (`Singleton`, `Transient`, `Scoped`).
  - Automatic argument injection for constructors (`add_singleton<Impl, Deps...>`) and factories (`R(Deps...)`), plus interface bindings (`add_singleton_as<Interface, Impl, Deps...>`).
  - Build-time graph validation: missing dependency checks, 3-color DFS circular dependency detection (reporting the full cycle path), and transitive captive-dependency checks (`Singleton` cannot capture `Scoped`).
  - Zero OS-thread blocking on lazy singleton initialization (`gorun::Mutex` + lock-free atomic fast path).
  - `Lifecycle` (FIFO `OnStart` with automatic reverse rollback on failure; LIFO `OnStop`).
  - `BlockingPool::run(...)` for offloading synchronous C drivers (`libpq`, `sqlite3`, disk I/O) without stalling `gorun` workers.
- **Stage 2: Non-Blocking HTTP/1.1 Server on Standalone Asio + `gorun`**
  - `AsioExecutor`, `AsyncTcpSocket`, and `AsyncTcpAcceptor` with non-blocking fast-path syscalls and reusable per-socket `gorun::Channel` parking + `gorun::Context` cancellation and `asio::steady_timer` timeouts.
  - Incremental HTTP/1.1 parser & serializer with Keep-Alive reuse and DoS bounds (`400` / `413` / `431`).
  - `HttpServer` running 1 green routine per connection, bounded by `gorun::Semaphore`, with exception isolation (`500`) and graceful `Lifecycle` draining.
- **Stage 3: Gin-Style Router, `HttpContext` & Middleware Chain**
  - Segment trie `Router` with static > `:param` > `*wildcard` priority, nested `RouteGroup`s, and automatic `405 Method Not Allowed` with `Allow` header.
  - `HttpContext` with URL-decoded query params, typed `param_as<T>` / `query_as<T>`, key-value store, `gorun::Context`, and per-request DI `ServiceScope`.
  - DI-injected route handlers: `[](HttpContext& c, IUserRepository& repo, RequestTrace& trace) { ... }`.
  - Built-in middleware: `recovery`, `request_logger`, `timeout` (`504`), `rate_limit` (`gorun::Semaphore` backpressure / `429`), and `bearer_auth` (`401`).
- **Stage 4: Production Observability, Signal Shutdown & 3-Way Benchmarks**
  - `StructuredLogger` (RFC-8259 JSON line logging with `gorun::Mutex` and `access_log_middleware()`).
  - `MetricsRegistry` (RPS, status class counters, panic-safe `in_flight` gauge, sliding-window `p50`/`p95`/`p99`/`max` latency in JSON and Prometheus exposition formats).
  - `HealthRegistry` mounting `/healthz` (liveness), `/readyz` (readiness returning `200` or `503`), and `/metrics`.
  - `App::run_until_signal()` for graceful `SIGINT` / `SIGTERM` shutdown with connection draining and `OnStop` hooks.

---

## 3-Way HTTP/1.1 Benchmark Results (16 Logical Cores, Windows x64)

All three benchmark suites (`benchmarks/bench_goforge_http.cpp`, `benchmarks/bench_go_http.go`, `benchmarks/bench_std_http.cpp`) run on the same machine (`GCC 16.2.0 -O3` vs `go1.27.4`):

| Workload | `goforge` (C++20 + `gorun` M:N + Asio) | Native Go 1.27 `net/http` | Standard C++20 (`std::jthread` per conn) |
| :--- | :--- | :--- | :--- |
| **1. Keep-Alive JSON API** (`32 x 500 = 16,000 reqs`) | **32,226 req/s**, **p50: 149 us** (`16000/16000 OK`) | 42,181 req/s, p50: 525 us (`16000/16000 OK`) | 255,517 req/s, p50: 33 us (no router/DI/metrics) |
| **2. 300 Concurrent I/O Handlers** (`10ms` wait) | **300/300 OK**, **fixed 16 OS workers** (zero thread explosion) | 213/300 OK (ephemeral port contention), M:N goroutines | 300/300 OK, **spawns 300 OS kernel threads** |

Notes:
- `goforge` achieves **3.5x lower median latency (`p50 = 149 us` vs `525 us`)** than Go `net/http` while executing the full framework stack (`Router` + per-request DI `ServiceScope` + `MetricsRegistry` middleware).
- Under 300 simultaneous I/O-bound connections, `goforge` uses a fixed M:N worker pool (0 extra OS threads), whereas thread-per-connection C++ spawns 300 OS kernel threads (which exhausts memory and scheduler capacity at thousands of concurrent clients).

---

## Project Layout

- `include/goforge/goforge.hpp` — Single umbrella header.
- `include/goforge/di.hpp` — `ServiceCollection`, `ServiceProvider`, `ServiceScope`, cycle & captive-dependency validation.
- `include/goforge/lifecycle.hpp` — `Lifecycle` & `LifecycleHook` (FIFO start + rollback, LIFO stop).
- `include/goforge/blocking_pool.hpp` — Dedicated OS-thread `BlockingPool` for legacy synchronous drivers.
- `include/goforge/app.hpp` — `AppBuilder` & `App` (`run()`, `run_until_signal()`, `spawn_background()`).
- `include/goforge/net/asio_bridge.hpp` — Non-blocking Standalone Asio ↔ `gorun` bridge (`AsioExecutor`, `AsyncTcpSocket`, `AsyncTcpAcceptor`).
- `include/goforge/http/http_types.hpp` & `http_server.hpp` — HTTP/1.1 parser/serializer and green-routine `HttpServer`.
- `include/goforge/web/context.hpp`, `router.hpp`, `middleware.hpp` — Gin-style `HttpContext`, trie `Router`/`RouteGroup`, and built-in middleware.
- `include/goforge/obs/logger.hpp` & `metrics.hpp` — `StructuredLogger`, `MetricsRegistry`, and `HealthRegistry`.
- `tests/` — 4 test suites (29 tests total across Stages 1–4).
- `examples/` — 3 runnable examples (`01_di_and_lifecycle`, `02_http_server`, `03_rest_api_gin_fx`).
- `benchmarks/` — 3-way HTTP benchmark suite (`bench_goforge_http.cpp`, `bench_go_http.go`, `bench_std_http.cpp`).

---

## Building & Running

`CMakeLists.txt` automatically detects a sibling `../Rutine` checkout of `gorun` for instant offline builds (or fetches `gorun v0.3.0-alpha` from GitHub via `FetchContent`), and fetches header-only Standalone Asio (`asio-1-32-0`).

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
