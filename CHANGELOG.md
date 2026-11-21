# Changelog

All notable changes to `goforge` are documented in this file.

## [v0.4.0] - 2026-11-22

### Changed / Breaking
- **100% Native `gorun::net` Networking (Standalone Asio Removed):**
  - Aligned release version with `gorun v0.4.0`.
  - Removed Standalone Asio completely (`include/goforge/net/asio_bridge.hpp`, `benchmarks/bench_asio_classic.cpp`, `GOFORGE_ENABLE_ASIO`, and Asio `FetchContent`).
  - Rewrote `HttpServer` (`include/goforge/http/http_server.hpp`), `WebApp`, integration tests (`test_stage2`–`test_stage6`), examples (`02`, `03`), and benchmarks onto native `gorun::net` (`TcpListener` / `TcpStream` over Windows IOCP, Linux `epoll`, and macOS/BSD `kqueue`).
  - Added configurable per-connection green routine stack size (`ServerOptions::connection_stack_size`) and graceful connection wake/drain via `TcpStream::cancel_handle()`.
  - Benchmarks on 16 cores (Windows x64, GCC 16.2 `-O3`): **142,643 req/s**, **p50: 69 us** (33% lower median latency than the previous Asio bridge) and **48.4 ms** for 300 concurrent 10 ms I/O handlers (14.3% faster than Asio, zero external networking dependencies).
- Added macOS Apple Silicon (ARM64) job to GitHub Actions CI.

### Added
- **EF-Style ORM Layer (`goforge::data` — Stage 7):**
  - `DbContext` and `DbSet<T>` with Unit of Work (`add`, `update`, `remove`, `save_changes` with atomic transaction and auto-increment ID backfill).
  - Multi-dialect SQL AST Compiler (`SqlCompiler`) translating `goforge::query` compile-time Expression Trees directly into parameterized SQL for **SQLite**, **PostgreSQL**, and **MySQL** (zero SQL injection).
  - Embedded **SQLite 3** driver (`SqliteDriver`, WAL mode, foreign keys, thread-safe transactional locking) offloaded automatically onto `goforge::BlockingPool`.
  - Schema auto-creation (`ensure_created<T>`) and versioned `MigrationRunner` (`__goforge_migrations`).
  - Stage 7 test suite (`tests/test_stage7_orm_data.cpp`, 5 tests) and full SQLite REST API example (`examples/05_orm_sqlite_rest_api.cpp`).
- **Ergonomic `WebApp`, Zero-Lambda Controllers & Fluent Query AST (Stage 6):**
  - `WebApp::create()` fluent facade with `.singleton<>()`, `.scoped<>()`, `.use_defaults()`, and `.controller<Controller, Deps...>("/prefix")`.
  - Compile-time typed extractors (`Body<T>`, `Path<"id", int>`, `Query<"q">`, `Header<"X-Api-Key">`) and smart return types (`T`, `std::optional<T>`, `Result<T>`, `Page<T>`, `ApiResponse`).
  - Fluent LINQ-style query engine (`goforge::query`) with compile-time Expression Trees (`col(&User::age) >= 18`, `.in(...)`, `.icontains(...)`, `.is_not_null()`, identity placeholder `it`, `GF_FN` shorthand, multi-key sorting, projections, and pagination) plus `GOFORGE_MODEL_TABLE` reflection macro.
  - Stage 6 test suite (`tests/test_stage6_ergonomics_query.cpp`, 5 tests) and example (`examples/04_ergonomic_webapp_and_query.cpp`).
- **First-Class Typed JSON & DTO Validation (Stage 5):**
  - Integrated `nlohmann/json` (`v3.11.3`) with `goforge::Json`, `goforge::H`, `GOFORGE_JSON_DTO`, `GOFORGE_JSON_DTO_DEFAULT`, automatic `std::optional<T>` serialization, `c.bind_json<T>()` (`400 Bad Request` / `422 Unprocessable Entity` with `.validate()`), and `c.error(...)`.
  - Stage 5 test suite (`tests/test_stage5_json.cpp`, 6 tests).
- **Packaging & Ecosystem:**
  - Added **vcpkg** port files (`packaging/vcpkg/vcpkg.json`, `packaging/vcpkg/portfile.cmake` with optional `sqlite` feature) and **Conan 2.0** recipe (`packaging/conan/conanfile.py` + `packaging/conan/test_package/`).
  - Added CMake `find_package(goforge CONFIG REQUIRED)` export/install configuration (`GOFORGE_INSTALL=ON`, `cmake/goforgeConfig.cmake.in`) with automatic `find_package` detection for `gorun`, `nlohmann_json`, and `SQLite3`.
- **Performance & Documentation:**
  - Zero-copy router middleware chain and lock-free `MetricsRegistry`.
  - Single-header distribution (`single_include/goforge/goforge.hpp`, 19 inlined headers) and interactive GitHub Pages documentation with live GCC 14.2 Compiler Explorer playground (`docs/index.html`).

## [v0.2.0-alpha] - 2026-11-21

### Added
- Production Observability (`StructuredLogger` RFC-8259 JSON, `MetricsRegistry` with `p50`/`p95`/`p99` & Prometheus `/metrics`, `HealthRegistry` `/healthz` & `/readyz`, graceful draining on `SIGINT`/`SIGTERM`).
- Gin-style Segment Trie `Router` (`:param`, `*wildcard`, `RouteGroup`, `404`/`405` + `Allow`), DI-injected route handlers with per-request `ServiceScope`, and production middleware (`recovery`, `timeout`, `rate_limit`, `bearer_auth`).
- Interactive documentation site (`docs/index.html`) and initial HTTP benchmarks (`goforge` vs Go 1.27 vs `std::jthread`).

## [v0.1.0-alpha] - 2026-11-20

### Added
- Initial Uber Fx-style Dependency Injection container (`ServiceCollection`, `ServiceProvider`, `ServiceScope`, `Singleton`/`Transient`/`Scoped`, compile-time typed auto-DI, cycle & captive scope validation).
- Ordered `Lifecycle` hooks (FIFO `on_start` with rollback, LIFO `on_stop`), `AppBuilder` / `App`, and `BlockingPool` for synchronous C drivers.
- HTTP/1.1 parser, response builder, and initial server implementation.
