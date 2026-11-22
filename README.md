<div align="center">
  <img src="./docs/logo.png" alt="goforge logo" width="160" style="border-radius: 24px;" />
  <h1><code>goforge</code> — Uber Fx + Gin Style C++20/23 Web Framework on <code>gorun</code></h1>
</div>

[![License: Apache 2.0](https://img.shields.io/badge/License-Apache_2.0-blue.svg)](./LICENSE)
[![C++20 / C++23](https://img.shields.io/badge/C%2B%2B-20%20%7C%2023-00599C?logo=c%2B%2B)](https://en.cppreference.com/w/cpp/20)
[![Header-Only](https://img.shields.io/badge/Library-Header--Only-success)](#quick-start--installation)
[![Built on gorun](https://img.shields.io/badge/Runtime-gorun%20M%3AN-34d399)](https://github.com/Takatochi/gorun)
[![Docs & Live Playground](https://img.shields.io/badge/Docs%20%26%20Live%20Playground-GitHub%20Pages-38bdf8)](https://takatochi.github.io/goforge/)

`goforge` is a standalone, header-only C++20/23 web, DI, query, and EF-style ORM framework built **100% natively on top of [`gorun`](https://github.com/Takatochi/gorun)** — its M:N green threads runtime and built-in IOCP / epoll / kqueue network poller (`gorun::net`, **zero Asio dependency**).

It combines **Uber Fx-style Dependency Injection & Lifecycle**, **Gin-style Trie Routing & Middleware**, **Zero-Lambda Controllers & Fluent Query AST**, **EF-Style `DbContext` / `DbSet<T>` ORM**, **Native `gorun::net` Async TCP I/O**, and **Production Observability** — letting C++ developers write clean synchronous-looking handlers with zero callback hell and zero viral `co_await`.

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
|  - Native Async NetPoller (gorun::net: IOCP / epoll / kqueue)     |
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

## Quick Start & Installation (6 Ways to Use `goforge`)

`goforge` is header-only and runs **100% natively on `gorun` (`gorun::net`)** with **zero Asio dependency**. Choose any of the 6 integration methods below:

| Method | Best For | Auto-Fetches `gorun`, JSON & SQLite? |
| :--- | :--- | :---: |
| **1. CMake `FetchContent`** | Most CMake projects (zero manual setup) | Yes (automatic) |
| **2. Single-Header Drop-In** | Makefiles, Meson, Bazel, or fast prototypes | 3 header files via `curl` |
| **3. Git Submodule** | Pinned vendored deps & offline builds | Supports local `GOFORGE_LOCAL_GORUN_DIR` |
| **4. System CMake Install** | Sharing `goforge` across local repos via `find_package` | Uses installed deps |
| **5. vcpkg (Overlay Ports)** | Teams using `vcpkg` manifests & optional `sqlite` feature | Managed by `vcpkg` |
| **6. Conan 2.0** | Teams using Conan 2.0 profiles & `CMakeDeps` | Managed by `Conan` |

### Option 1: CMake `FetchContent` (Recommended — Zero Manual Setup)

Add this to your `CMakeLists.txt`. CMake automatically fetches `goforge v0.4.0`, `gorun v0.4.0`, `nlohmann_json v3.11.3`, and embedded `SQLite 3`:

```cmake
cmake_minimum_required(VERSION 3.20)
project(my_service LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)

# Optional: disable embedded SQLite 3 if you only need HTTP/DI/JSON/Query (no SQLite ORM):
# set(GOFORGE_ENABLE_SQLITE OFF CACHE BOOL "" FORCE)

include(FetchContent)
FetchContent_Declare(
    goforge
    GIT_REPOSITORY https://github.com/Takatochi/goforge.git
    GIT_TAG        v0.4.0 # or main
)
FetchContent_MakeAvailable(goforge)

add_executable(my_service main.cpp)
target_link_libraries(my_service PRIVATE goforge::goforge)
```

### Option 2: Single-Header Drop-In (No CMake Required)

Download the 3 header-only files (`goforge.hpp`, `gorun.hpp`, `nlohmann/json.hpp`) and compile directly:

```bash
mkdir -p include/goforge include/gorun include/nlohmann
curl -fLo include/goforge/goforge.hpp \
  https://raw.githubusercontent.com/Takatochi/goforge/v0.4.0/single_include/goforge/goforge.hpp
curl -fLo include/gorun/gorun.hpp \
  https://raw.githubusercontent.com/Takatochi/gorun/v0.4.0/single_include/gorun/gorun.hpp
curl -fLo include/nlohmann/json.hpp \
  https://raw.githubusercontent.com/nlohmann/json/v3.11.3/single_include/nlohmann/json.hpp

# Linux / macOS (GCC or Clang):
g++ -std=c++20 -O2 -pthread -Iinclude main.cpp -o my_service

# Windows MinGW / GCC:
g++ -std=c++20 -O2 -pthread -Iinclude main.cpp -o my_service.exe -lws2_32 -lmswsock

# Windows MSVC (Developer Command Prompt):
cl /std:c++20 /O2 /EHsc /Iinclude main.cpp ws2_32.lib mswsock.lib
```

*(To also enable `SqliteDriver` in single-header mode, compile with `-DGOFORGE_HAS_SQLITE=1` and link `-lsqlite3`.)*

### Option 3: Git Submodule + `add_subdirectory`

```bash
git submodule add https://github.com/Takatochi/gorun.git external/gorun
git -C external/gorun checkout v0.4.0
git submodule add https://github.com/Takatochi/goforge.git external/goforge
git -C external/goforge checkout v0.4.0
```
Then in your `CMakeLists.txt`:
```cmake
set(GOFORGE_LOCAL_GORUN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/gorun" CACHE PATH "" FORCE)
add_subdirectory(external/goforge)
target_link_libraries(my_service PRIVATE goforge::goforge)
```

### Option 4: System-Wide CMake Install + `find_package(goforge)`

When `gorun`, `nlohmann_json`, and (optionally) `SQLite3` are installed in your prefix (e.g. `/usr/local`), install `goforge` once with `-DGOFORGE_INSTALL=ON`:

```bash
git clone --branch v0.4.0 https://github.com/Takatochi/goforge.git
cmake -S goforge -B goforge/build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=/usr/local \
  -DGOFORGE_BUILD_TESTS=OFF -DGOFORGE_BUILD_EXAMPLES=OFF -DGOFORGE_BUILD_BENCHMARKS=OFF \
  -DGOFORGE_INSTALL=ON
cmake --install goforge/build --prefix /usr/local
```
Then in any downstream `CMakeLists.txt`:
```cmake
find_package(goforge 0.4.0 REQUIRED CONFIG)
target_link_libraries(my_service PRIVATE goforge::goforge)
```

### Option 5: vcpkg (Overlay Ports & Manifest Mode)

Ready-to-use port files (with verified `v0.4.0` SHA512 and optional `sqlite` feature) live in [`packaging/vcpkg/`](./packaging/vcpkg/):

```bash
git clone https://github.com/Takatochi/gorun.git
git clone https://github.com/Takatochi/goforge.git

# Install with default SQLite 3 ORM support:
vcpkg install goforge \
  --overlay-ports=./gorun/packaging/vcpkg \
  --overlay-ports=./goforge/packaging/vcpkg

# Or without SQLite 3:
vcpkg install "goforge[core]" \
  --overlay-ports=./gorun/packaging/vcpkg \
  --overlay-ports=./goforge/packaging/vcpkg
```

Or in your project's `vcpkg.json` manifest:
```json
{
  "name": "my-service",
  "version-string": "1.0.0",
  "dependencies": ["goforge"]
}
```
And in `CMakeLists.txt`: `find_package(goforge CONFIG REQUIRED)` + `target_link_libraries(my_service PRIVATE goforge::goforge)`.

### Option 6: Conan 2.0

Conan 2.0 recipes (with `conandata.yml`, `with_sqlite` option, and `test_package/`) live in [`packaging/conan/`](./packaging/conan/):

```bash
git clone https://github.com/Takatochi/gorun.git
git clone https://github.com/Takatochi/goforge.git

conan create gorun/packaging/conan --version=0.4.0 --build=missing
conan create goforge/packaging/conan --version=0.4.0 --build=missing
# Optional: disable SQLite with -o goforge/*:with_sqlite=False
```

Then add `goforge/0.4.0` to your project's `conanfile.txt`:
```ini
[requires]
goforge/0.4.0

[generators]
CMakeDeps
CMakeToolchain
```
And in `CMakeLists.txt`: `find_package(goforge CONFIG REQUIRED)` + `target_link_libraries(my_service PRIVATE goforge::goforge)`.

### Lightweight REST Microservice (`WebApp` + Zero-Lambda Controllers + Fluent Query AST)

No boilerplate `AppBuilder` wiring, no raw JSON string literals, no verbose lambdas:

```cpp
#include "goforge/goforge.hpp"

using namespace goforge;
using namespace goforge::query;

// Unified Model: JSON serialization + compile-time table/column reflection (ready for future ORM!)
struct User {
    int id{0};
    std::string name;
    std::string email;
    std::string role{"user"};
    int age{0};
    bool active{true};
    std::optional<std::string> bio;
};
GOFORGE_MODEL_TABLE(User, "users", id, name, email, role, age, active, bio)

struct CreateUserDto {
    std::string name;
    std::string email;
    std::string role{"user"};
    int age{18};
    std::optional<std::string> bio;

    std::optional<std::string> validate() const {
        if (name.empty()) return "name must not be empty";
        if (email.find('@') == std::string::npos) return "email must contain '@'";
        if (age < 13) return "age must be >= 13";
        return std::nullopt;
    }
};
GOFORGE_JSON_DTO_DEFAULT(CreateUserDto, name, email, role, age, bio)

struct IUserRepo {
    virtual ~IUserRepo() = default;
    virtual User create(const CreateUserDto& in) = 0;
    virtual std::optional<User> find(int id) const = 0;
    virtual Page<User> search(std::string_view q, int min_age, std::size_t page, std::size_t size) const = 0;
};

struct MemoryUserRepo final : IUserRepo {
    mutable gorun::RwMutex rw;
    int next_id{2};
    std::unordered_map<int, User> rows{{1, {1, "Artur Havor", "artur@goforge.dev", "admin", 25, true, "Creator"}}};

    User create(const CreateUserDto& in) override {
        std::unique_lock lk(rw);
        int id = next_id++;
        User u{id, in.name, in.email, in.role, in.age, true, in.bio};
        rows[id] = u;
        return u;
    }

    std::optional<User> find(int id) const override {
        std::shared_lock lk(rw);
        auto it_row = rows.find(id);
        return it_row == rows.end() ? std::nullopt : std::optional<User>(it_row->second);
    }

    // Fluent C#/EF-style query using compile-time Expression Trees (zero lambdas!)
    Page<User> search(std::string_view q, int min_age, std::size_t page, std::size_t size) const override {
        std::shared_lock lk(rw);
        auto seq = from_values(rows)
            .where(col(&User::active) == true && col(&User::age) >= min_age);
        if (!q.empty()) {
            seq = seq.where(col(&User::name).icontains(q) || col(&User::email).icontains(q));
        }
        return seq.order_by(&User::role).then_by_desc(&User::age).paginate(page, size);
    }
};

// Zero-lambda Controller: resolved via Scoped DI per request, auto-extracts Body/Path/Query & serializes returns!
struct UserController {
    IUserRepo& repo;
    explicit UserController(IUserRepo& r) : repo(r) {}

    ApiResponse create(Body<CreateUserDto> dto) { return created(repo.create(*dto)); }      // 400/422 -> 201 JSON
    std::optional<User> get_by_id(Path<"id", int> id) const { return repo.find(*id); }      // 200 JSON or 404 JSON
    Page<User> list(Query<"q"> q, Query<"min_age", int> age, Query<"page", int> p) const {  // Auto paginated JSON
        return repo.search(q.value_or(""), age.value_or(0), p.value_or(1), 20);
    }
};

int main() {
    auto app = WebApp::create({.port = 8080})
        .singleton<IUserRepo, MemoryUserRepo>()
        .use_defaults(); // recovery + metrics + JSON access log + /healthz, /readyz, /metrics

    app.controller<UserController, IUserRepo>("/api/v1/users")
       .post("", &UserController::create)
       .get("", &UserController::list)
       .get("/:id", &UserController::get_by_id);

    app.run(); // Validates DI graph, starts Lifecycle hooks, drains gracefully on SIGINT/SIGTERM!
}
```

### Fluent LINQ-Style Query Engine (`goforge::query`) & SQL-Ready Expression Trees

Write expressive queries in C++20 without verbose `[](const User& u) { return ...; }` lambdas. Every `col(&T::field)` predicate builds a **compile-time Expression Tree (AST)** that evaluates at zero overhead in memory today and compiles directly to parameterized SQL in the upcoming EF-style `goforge::data` (`DbContext`/`DbSet<T>`) layer:

```cpp
using namespace goforge::query;

// 1. Zero-lambda AST predicates, multi-key sorting, projections & pagination
auto admins = from(users)
    .where(col(&User::age) >= 18 && col(&User::role).in({"admin", "mod"}) && col(&User::bio).is_not_null())
    .order_by(&User::role)
    .then_by_desc(&User::age)
    .select(&User::email)
    .to_vec();

// 2. Identity placeholder `it` for primitive collections (zero macros, zero lambdas)
auto doubled = from({3, 12, 7, 25, 18})
    .where(it >= 10 && it <= 20)
    .order_by(it)
    .select(it * 2)
    .to_vec(); // -> {24, 36}

// 3. Opt-in concise lambda shorthand `GF_FN` / `GF_FN2` for custom C++ logic
auto power_users = from(users)
    .where(GF_FN(u, u.tags.size() >= 3 && u.active))
    .to_vec();
```

### EF-Style ORM (`goforge::data`: `DbContext`, `DbSet<T>`, SQL AST Compiler & SQLite 3)

Write **one unified query syntax** for both in-memory collections and SQL databases (`SQLite`, `PostgreSQL`, `MySQL`). Every synchronous DB call automatically offloads onto `goforge::BlockingPool` so `gorun` M:N worker threads never block:

```cpp
using namespace goforge;
using namespace goforge::query;
using namespace goforge::data;

struct AppDbContext final : DbContext {
    AppDbContext(std::shared_ptr<IDbDriver> drv, BlockingPool& pool)
        : DbContext(std::move(drv), pool) {}

    DbSet<User> users() noexcept { return set<User>(); }
};

auto sqlite = SqliteDriver::open("app.db"); // WAL mode + foreign keys + thread-safe transactions
AppDbContext db(sqlite, pool);
db.ensure_created<User>();                  // Auto CREATE TABLE IF NOT EXISTS from GOFORGE_MODEL_TABLE!

// Unit of Work: staged INSERT/UPDATE/DELETE executed atomically inside one transaction
User u{0, "Artur Havor", "artur@goforge.dev", "admin", 25, true, "Creator"};
db.add(u);
db.save_changes();                          // Auto-backfills u.id from last_insert_rowid()!

// Exact same compile-time AST compiles directly to parameterized SQL (zero SQL injection!):
// SELECT "id", ... FROM "users" WHERE ("age" >= ?1 AND "role" IN (?2, ?3)) ORDER BY "age" DESC LIMIT 20 OFFSET 0
Page<User> page = db.users()
    .where(col(&User::age) >= 18 && col(&User::role).in({"admin", "mod"}))
    .order_by_desc(&User::age)
    .paginate(1, 20);                       // Runs COUNT(*) + LIMIT/OFFSET -> ready for HTTP JSON!
```

---

## 4-Way HTTP/1.1 Performance Benchmarks (16 Logical Cores, Windows x64)

All benchmark suites ([bench_goforge_http.cpp](./benchmarks/bench_goforge_http.cpp), [bench_go_http.go](./benchmarks/bench_go_http.go), [bench_std_http.cpp](./benchmarks/bench_std_http.cpp), plus historical Classic Async Asio reference) execute on the same machine (`GCC 16.2.0 -O3` vs `go1.27.4`) using identical raw-socket client harnesses. `goforge` runs **100% on native `gorun::net` (IOCP / epoll / kqueue)** with **zero Asio dependency**:

| Workload | `goforge` (100% Native `gorun::net` + Router + DI + Metrics + JSON) | Classic Async Asio Reference (Raw Callbacks, 2 I/O threads) | Native Go 1.27 `net/http` | Standard C++20 (`std::jthread` per conn) |
| :--- | :--- | :--- | :--- | :--- |
| **1. Keep-Alive JSON API** (`32 x 500 = 16,000 reqs`) | **142,643 req/s**, **p50: 69 us**, **p99: 1,033 us** (`16000/16000`) | 122,001 req/s, p50: 239 us, p99: 448 us (`16000/16000`) | 42,504 req/s, p50: 524 us, p99: 4,234 us (`16000/16000`) | 165,680 req/s, p50: 36 us (raw socket) |
| **2. 300 Concurrent I/O Handlers** (`10ms` wait) | **300/300 OK**, **48.4 ms** (`6,197 req/s`), **fixed 16 M:N workers** | **300/300 OK**, **38.5 ms** (`7,793 req/s`), **2 I/O threads** | 214/300 OK (ephemeral ports), 42.6 ms (`5,024 req/s`), M:N | 300/300 OK, 61.6 ms, **spawns 300 OS threads** |

### `goforge` (Native `gorun::net`) vs. Classic Async Standalone Asio: Engineering Breakdown

| Aspect | Classic Async Asio (Callbacks) | `goforge` (100% Native `gorun` M:N + `gorun::net`) |
| :--- | :--- | :--- |
| **Keep-Alive Throughput & Latency** | `122.0k req/s`, `p50: 239 us`, `p99: 448 us` (raw socket parser, no DI/Router/Metrics). | **`142.6k req/s` (+16.9% faster!)**, **`p50: 69 us` (3.46x lower!)**, **`p99: 1,033 us`** — with full Trie Router, Scoped DI, `nlohmann/json`, and lock-free Metrics! (And **3.36x faster RPS / 7.6x lower median latency than Go 1.27**.) |
| **300 Concurrent I/O Burst** | `38.5 ms` on 2 I/O threads (zero context switch). | **`48.4 ms` (`6,197 req/s`, 300/300 OK)** on fixed M:N workers — **14.3% faster** than the previous Asio bridge (`56.5 ms`) and **21.4% faster** than 300-thread `std::jthread` (`61.6 ms`)! |
| **External Network Dependency** | Requires Standalone Asio or Boost.Asio. | **Zero external networking dependencies** — powered natively by `gorun::net` (Windows IOCP `GetQueuedCompletionStatusEx`, Linux `epoll`, macOS/BSD `kqueue`). |
| **Handler Code Style** | `enable_shared_from_this`, manual `self` capture across nested lambdas, `asio::strand`, manual `steady_timer` state machine. | **Plain sequential C++**: `c.bind_json<Dto>()`, `gorun::Mutex`, `gorun::Channel`, `try/catch`, `GORUN_DEFER`, automatic DI injection. |
| **Exception & Lifetime Safety** | Unhandled exceptions inside callbacks escape to `io_context::run()`; stack variables cannot cross async boundaries. | Local stack variables live safely across every socket read/write/sleep (configurable `gorun::StackSize`); `recovery()` isolates handler exceptions into `500 JSON`. |

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

**`goforge` (4 lines — 100% native `gorun::net` IOCP/epoll/kqueue under the hood, zero Asio, zero callbacks):**
```cpp
router.get("/api/v1/io-wait", [](goforge::web::HttpContext& c) {
    gorun::sleep_for(10ms); // Parks green routine without blocking OS workers!
    c.json(200, goforge::H{{"waited_ms", 10}});
});
```

---

## Verification Test Suites (45 Automated Tests Across 7 Suites)

| Test Suite | Coverage | Result |
| :--- | :--- | :---: |
| [test_stage1_di.cpp](./tests/test_stage1_di.cpp) | 10 tests: `Singleton`/`Transient`/`Scoped`, auto-DI, missing/cycle/captive validation, `Lifecycle` FIFO/LIFO + rollback, `BlockingPool`, 200 concurrent scopes | **10/10 PASSED** |
| [test_stage2_http.cpp](./tests/test_stage2_http.cpp) | 7 tests: HTTP/1.1 Keep-Alive, exception isolation (`500`), protocol bounds (`400`/`413`), 300 concurrent requests with `gorun` primitives, zero OS-thread blocking proof | **7/7 PASSED** |
| [test_stage3_web.cpp](./tests/test_stage3_web.cpp) | 8 tests: Trie Router (`:param`, `*wildcard`, `404`, `405` + `Allow`), URL decoding, nested `RouteGroup`, DI handlers, `Recovery`, `Timeout` (`504`), `RateLimit` (`429`/queue), live CRUD | **8/8 PASSED** |
| [test_stage4_prod.cpp](./tests/test_stage4_prod.cpp) | 4 tests: RFC-8259 `StructuredLogger`, `MetricsRegistry` (`p50`/`p95`/`p99`, Prometheus), `HealthRegistry` (`/healthz`, `/readyz` 200/503), live graceful draining | **4/4 PASSED** |
| [test_stage5_json.cpp](./tests/test_stage5_json.cpp) | 6 tests: `goforge::H`, `GOFORGE_JSON_DTO` / `GOFORGE_JSON_DTO_DEFAULT`, `std::optional<T>`, `c.bind_json<T>()` (`400` / `422` auto-validation), nested collections, live TCP DTO CRUD | **6/6 PASSED** |
| [test_stage6_ergonomics_query.cpp](./tests/test_stage6_ergonomics_query.cpp) | 5 tests: `WebApp` facade, zero-lambda `ControllerGroup`, typed `Body`/`Path`/`Query`/`Header` extractors, smart returns (`Result<T>`, `optional`, `Page<T>`), fluent `goforge::query` AST + `GOFORGE_MODEL_TABLE` reflection + live TCP roundtrip | **5/5 PASSED** |
| [test_stage7_orm_data.cpp](./tests/test_stage7_orm_data.cpp) | 5 tests: Multi-dialect SQL AST compiler (`SQLite`/`Postgres`/`MySQL`), embedded SQLite 3 `DbContext` + Unit of Work + rollback, `DbSet<T>` queries & pagination, versioned migrations, concurrent `WebApp` + `BlockingPool` + SQLite 3 REST API | **5/5 PASSED** |

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
.\build\goforge_example_04_ergonomic.exe     # lightweight WebApp + Controllers + Fluent Query AST
.\build\goforge_example_05_orm_rest.exe      # EF-style DbContext + SQLite 3 + WebApp REST API

# Benchmarks:
.\build\bench_goforge_http.exe
go run benchmarks\bench_go_http.go
.\build\bench_std_http.exe
```

---

## Author & License

Created by **Artur Havor**. Distributed under the **[Apache License, Version 2.0](./LICENSE)**.
