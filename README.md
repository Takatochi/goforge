# goforge — C++20/23 Fx + Gin Style Application & Web Framework on `gorun`

`goforge` is a standalone C++20/23 web and backend application framework built on top of the [`gorun`](https://github.com/Takatochi/gorun) M:N green threads runtime.

Created by **Artur Havor** and licensed under the **Apache License 2.0**.

---

## Target Architecture

```text
+-------------------------------------------------------------------+
|                     Application (App / Fx-style)                  |
|  - Compile-Time Typed DI Container (Singleton, Transient, Scoped) |
|  - Lifecycle Hooks (FIFO OnStart, Rollback, LIFO OnStop)          |
|  - BlockingPool (Offloads synchronous DB/disk I/O off workers)    |
+---------------------------------+---------------------------------+
                                  | Dependency Injection
                                  v
+-------------------------------------------------------------------+
|                   HTTP / Web Layer (Gin-style)                    |
|  - Radix-Tree Router (:id, *filepath, RouteGroup) + Middleware    |
|  - HttpContext (params, query, headers, body, RequestScope DI)    |
|  - 1 Request = 1 Green Routine: gorun::go(...)                    |
+---------------------------------+---------------------------------+
                                  | Green Routines, Channels, Sync
                                  v
+-------------------------------------------------------------------+
|                       gorun Runtime (M:N)                         |
|  - Work-stealing scheduler, Channel<T>, Mutex, WaitGroup, Context |
|  - Non-blocking I/O via standalone Asio integrated with gorun     |
+-------------------------------------------------------------------+
```

---

## Implementation Roadmap

- **Stage 1 (Completed): Fx-Style DI Container, Lifecycle & BlockingPool**
  - `ServiceCollection`, `ServiceProvider`, `ServiceScope` (`Singleton`, `Transient`, `Scoped`).
  - Automatic argument resolution for constructors (`add_singleton<Impl, Deps...>`) and factories (`R(Deps...)`), plus interface bindings (`add_singleton_as<I, Impl, Deps...>`).
  - Up-front graph validation in `build()`: missing dependency checks, 3-color DFS circular dependency detection (with full cycle trace), and transitive captive-dependency validation (`Singleton` cannot capture `Scoped`).
  - Zero OS-thread blocking on lazy singleton initialization via `gorun::Mutex` + lock-free atomic fast path.
  - `Lifecycle` (`OnStart` in FIFO order with automatic reverse rollback on failure; `OnStop` in LIFO order).
  - `BlockingPool::run(...)` for offloading synchronous C drivers (e.g., `libpq`, `sqlite3`) onto dedicated OS threads while parking the calling `gorun` routine on a `gorun::Channel`.
  - `AppBuilder` / `App` with `spawn_background(ctx)`, `run()`, `request_shutdown()`, and graceful `stop()`.
- **Stage 2 (Completed): Non-Blocking HTTP/1.1 Server on Standalone Asio + `gorun`**
  - `goforge::net::AsioExecutor`, `AsyncTcpSocket`, and `AsyncTcpAcceptor` bridge Asio async completions (`async_accept`, `async_read_some`, `async_write`, `async_connect`) into `gorun::Channel` + `gorun::select` with `gorun::Context` cancellation/timeout support and zero OS-thread blocking on `gorun` workers.
  - Incremental HTTP/1.1 parser & serializer (`HttpRequest`, `HttpResponse`, case-insensitive `HeaderMap`, `Content-Length` bounds, Keep-Alive connection reuse).
  - `HttpServer` executes every accepted connection on its own `gorun::go(...)` green routine, isolates handler exceptions into `500 Internal Server Error`, limits concurrency via `gorun::Semaphore`, and integrates with `goforge::Lifecycle` for graceful draining.
- **Stage 3 (Planned): Gin-Style Router, `HttpContext` & Middleware Chain**
  - Radix-tree router (`:id`, `*filepath`), `RouteGroup`, `next()` middleware pipeline, `Recovery` & `Timeout` middleware, and automatic `ServiceScope` per HTTP request.
- **Stage 4 (Planned): Production Features & Benchmarks**
  - Graceful HTTP connection draining, structured logging, health/metrics endpoints, and RPS/p99 benchmarks against Go/Gin and thread-per-request C++.

---

## Building & Running (Stages 1 & 2)

`CMakeLists.txt` automatically detects a sibling `../Rutine` checkout of `gorun` for instant offline builds (or fetches `gorun v0.3.0-alpha` from GitHub via `FetchContent`), and fetches header-only Standalone Asio (`asio-1-32-0`) for non-blocking networking.

```powershell
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure

# Run examples:
.\build\goforge_example_01_di.exe
.\build\goforge_example_02_http.exe          # self-test with 16 concurrent clients
.\build\goforge_example_02_http.exe --serve  # listen on http://127.0.0.1:8080
```
