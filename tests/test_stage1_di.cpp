#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "goforge/goforge.hpp"

using namespace std::chrono_literals;

#define GOFORGE_ASSERT(cond)                                                              \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            std::cerr << "\n[FAIL] Assertion failed at " << __FILE__ << ":" << __LINE__  \
                      << " -> " << #cond << "\n";                                         \
            std::exit(1);                                                                 \
        }                                                                                 \
    } while (0)

namespace {

struct Config {
    std::string dsn{"postgres://localhost:5432/app"};
    int max_conns{16};
};

struct Logger {
    static inline std::atomic<int> constructions{0};
    gorun::Mutex mtx;
    std::vector<std::string> entries;

    Logger() { constructions.fetch_add(1, std::memory_order_relaxed); }

    void log(std::string msg) {
        std::lock_guard<gorun::Mutex> lk(mtx);
        entries.push_back(std::move(msg));
    }
};

struct IUserRepository {
    virtual ~IUserRepository() = default;
    virtual std::string find_name(int id) const = 0;
};

struct InMemoryUserRepository final : IUserRepository {
    const Config& cfg;
    Logger& logger;

    InMemoryUserRepository(const Config& c, Logger& l) : cfg(c), logger(l) {
        logger.log("repo_init:" + cfg.dsn);
    }

    std::string find_name(int id) const override {
        return "user-" + std::to_string(id);
    }
};

struct RequestId {
    static inline std::atomic<int> live_count{0};
    static inline std::atomic<int> next_id{1};
    int id;

    RequestId() : id(next_id.fetch_add(1, std::memory_order_relaxed)) {
        live_count.fetch_add(1, std::memory_order_relaxed);
    }
    ~RequestId() {
        live_count.fetch_sub(1, std::memory_order_relaxed);
    }
};

struct TransientToken {
    static inline std::atomic<int> next_seq{1};
    int seq;
    TransientToken() : seq(next_seq.fetch_add(1, std::memory_order_relaxed)) {}
};

struct ScopedUnitOfWork {
    RequestId& req_id;
    IUserRepository& repo;

    ScopedUnitOfWork(RequestId& r, IUserRepository& u) : req_id(r), repo(u) {}
};

// Types for cycle & captive dependency tests
struct CycleA {};
struct CycleB {};
struct CycleC {};

struct BadSingleton {};
struct TransientBridge {};

// Test 01: Lifetimes (Singleton, Transient, Scoped) & deterministic scope destruction
void test_01_lifetimes_and_scope_isolation() {
    std::cout << "[TEST 01] Lifetimes (Singleton, Transient, Scoped) & scope destruction... ";

    Logger::constructions.store(0);
    RequestId::live_count.store(0);
    TransientToken::next_seq.store(1);

    goforge::ServiceCollection sc;
    sc.add_instance(Config{})
      .add_singleton<Logger>()
      .add_singleton_as<IUserRepository, InMemoryUserRepository, const Config&, Logger&>()
      .add_transient<TransientToken>()
      .add_scoped<RequestId>()
      .add_scoped<ScopedUnitOfWork, RequestId&, IUserRepository&>();

    auto provider = sc.build();

    // Singleton identity
    auto l1 = provider.resolve_ptr<Logger>();
    auto l2 = provider.resolve_ptr<Logger>();
    GOFORGE_ASSERT(l1.get() == l2.get());
    GOFORGE_ASSERT(Logger::constructions.load() == 1);

    // Transient creates fresh instances every time
    auto t1 = provider.resolve_ptr<TransientToken>();
    auto t2 = provider.resolve_ptr<TransientToken>();
    GOFORGE_ASSERT(t1.get() != t2.get());
    GOFORGE_ASSERT(t1->seq != t2->seq);

    // Scoped: same within one scope, distinct across two scopes, destroyed on scope exit
    {
        auto scope_a = provider.create_scope();
        auto scope_b = provider.create_scope();

        auto& uow_a1 = scope_a.resolve<ScopedUnitOfWork>();
        auto& uow_a2 = scope_a.resolve<ScopedUnitOfWork>();
        auto& uow_b = scope_b.resolve<ScopedUnitOfWork>();

        GOFORGE_ASSERT(&uow_a1 == &uow_a2);
        GOFORGE_ASSERT(&uow_a1.req_id == &scope_a.resolve<RequestId>());
        GOFORGE_ASSERT(&uow_a1.req_id != &uow_b.req_id);
        GOFORGE_ASSERT(&uow_a1.repo == &uow_b.repo); // Shared Singleton across scopes
        GOFORGE_ASSERT(RequestId::live_count.load() == 2);
    }
    GOFORGE_ASSERT(RequestId::live_count.load() == 0);

    std::cout << "PASSED\n";
}

// Test 02: Factory auto-injection, interface bindings & ServiceProvider/ServiceScope invoke()
void test_02_factory_auto_injection_and_invoke() {
    std::cout << "[TEST 02] Factory auto-injection (R(Deps...)) & invoke()... ";

    goforge::ServiceCollection sc;
    sc.add_instance(Config{.dsn = "pg://prod", .max_conns = 32})
      .add_singleton<Logger>()
      .add_factory<IUserRepository>(goforge::Lifetime::Singleton,
          [](const Config& cfg, Logger& logger) -> std::shared_ptr<IUserRepository> {
              return std::make_shared<InMemoryUserRepository>(cfg, logger);
          })
      .add_scoped_factory([](Logger& l) {
          l.log("req_created");
          return std::make_shared<RequestId>();
      });

    auto provider = sc.build();

    std::string found = provider.invoke([](IUserRepository& repo, const Config& cfg) {
        return repo.find_name(cfg.max_conns);
    });
    GOFORGE_ASSERT(found == "user-32");

    auto scope = provider.create_scope();
    int scoped_id = scope.invoke([](RequestId& r, IUserRepository& repo) {
        GOFORGE_ASSERT(repo.find_name(1) == "user-1");
        return r.id;
    });
    GOFORGE_ASSERT(scoped_id == scope.resolve<RequestId>().id);

    std::cout << "PASSED\n";
}

// Test 03: Graph validation — missing dependencies & circular dependencies
void test_03_missing_and_circular_dependency_detection() {
    std::cout << "[TEST 03] Build-time missing & circular dependency detection... ";

    // 3.1 Missing dependency
    {
        goforge::ServiceCollection sc;
        sc.add_singleton_as<IUserRepository, InMemoryUserRepository, const Config&, Logger&>();
        bool caught = false;
        try {
            (void)sc.build();
        } catch (const goforge::ServiceNotFoundError&) {
            caught = true;
        }
        GOFORGE_ASSERT(caught);
    }

    // 3.2 3-node cycle: CycleA -> CycleB -> CycleC -> CycleA
    {
        goforge::ServiceCollection sc;
        sc.add_singleton_factory([](CycleB&) { return CycleA{}; })
          .add_singleton_factory([](CycleC&) { return CycleB{}; })
          .add_singleton_factory([](CycleA&) { return CycleC{}; });

        bool caught = false;
        std::string msg;
        try {
            (void)sc.build();
        } catch (const goforge::CircularDependencyError& ex) {
            caught = true;
            msg = ex.what();
        }
        GOFORGE_ASSERT(caught);
        GOFORGE_ASSERT(msg.find("Circular dependency detected") != std::string::npos);
        GOFORGE_ASSERT(msg.find("->") != std::string::npos);
    }

    std::cout << "PASSED\n";
}

// Test 04: Captive dependency (Singleton -> Scoped) & ScopeRequiredError
void test_04_captive_dependency_and_scope_guard() {
    std::cout << "[TEST 04] Captive dependency (Singleton -> Transient -> Scoped) & root scope guard... ";

    // 4.1 Transitive captive dependency: Singleton -> Transient -> Scoped
    {
        goforge::ServiceCollection sc;
        sc.add_scoped<RequestId>()
          .add_transient_factory([](RequestId&) { return TransientBridge{}; })
          .add_singleton_factory([](TransientBridge) { return BadSingleton{}; });

        bool caught = false;
        try {
            (void)sc.build();
        } catch (const goforge::CaptiveDependencyError&) {
            caught = true;
        }
        GOFORGE_ASSERT(caught);
    }

    // 4.2 Resolving Scoped directly from root provider throws ScopeRequiredError
    {
        goforge::ServiceCollection sc;
        sc.add_scoped<RequestId>();
        auto provider = sc.build();
        bool caught = false;
        try {
            (void)provider.resolve<RequestId>();
        } catch (const goforge::ScopeRequiredError&) {
            caught = true;
        }
        GOFORGE_ASSERT(caught);
    }

    std::cout << "PASSED\n";
}

// Test 05: Concurrent Singleton initialization under 64 green routines on a 1-worker runtime
void test_05_concurrent_singleton_init_single_worker() {
    std::cout << "[TEST 05] Concurrent Singleton init (64 routines, 1-worker Runtime, zero OS blocking)... ";

    struct SlowInitSingleton {
        int magic{777};
    };

    std::atomic<int> init_calls{0};
    goforge::ServiceCollection sc;
    sc.add_singleton_factory([&]() {
        gorun::sleep_for(5ms); // Yields green routine inside gorun::Mutex!
        init_calls.fetch_add(1, std::memory_order_relaxed);
        return std::make_shared<SlowInitSingleton>();
    });

    auto provider = sc.build();
    gorun::Runtime single_rt(gorun::Config{.worker_count = 1});

    std::atomic<const SlowInitSingleton*> first_ptr{nullptr};
    std::atomic<int> matched{0};

    single_rt.block_on([&]() {
        gorun::Scope scope(single_rt);
        for (int i = 0; i < 64; ++i) {
            scope.spawn([&]() {
                auto ptr = provider.resolve_ptr<SlowInitSingleton>();
                const SlowInitSingleton* expected = nullptr;
                first_ptr.compare_exchange_strong(expected, ptr.get(), std::memory_order_acq_rel);
                if (ptr.get() == first_ptr.load(std::memory_order_acquire) && ptr->magic == 777) {
                    matched.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }
    });

    GOFORGE_ASSERT(init_calls.load() == 1);
    GOFORGE_ASSERT(matched.load() == 64);

    std::cout << "PASSED\n";
}

// Test 06: Lifecycle FIFO startup and LIFO reverse shutdown order
void test_06_lifecycle_ordering() {
    std::cout << "[TEST 06] Lifecycle FIFO OnStart & LIFO OnStop ordering... ";

    goforge::Lifecycle lc;
    std::vector<std::string> events;

    lc.append({
        .name = "db",
        .on_start = [&](gorun::Context) { events.push_back("start:db"); },
        .on_stop = [&](gorun::Context) { events.push_back("stop:db"); }
    });
    lc.append({
        .name = "cache",
        .on_start = [&](gorun::Context) { events.push_back("start:cache"); },
        .on_stop = [&](gorun::Context) { events.push_back("stop:cache"); }
    });
    lc.append({
        .name = "http",
        .on_start = [&](gorun::Context) { events.push_back("start:http"); },
        .on_stop = [&](gorun::Context) { events.push_back("stop:http"); }
    });

    lc.start(gorun::background());
    auto errors = lc.stop(gorun::background());
    GOFORGE_ASSERT(errors.empty());

    std::vector<std::string> expected{
        "start:db", "start:cache", "start:http",
        "stop:http", "stop:cache", "stop:db"
    };
    GOFORGE_ASSERT(events == expected);

    std::cout << "PASSED\n";
}

// Test 07: Lifecycle rollback when a startup hook fails
void test_07_lifecycle_startup_failure_rollback() {
    std::cout << "[TEST 07] Lifecycle automatic reverse rollback on OnStart failure... ";

    goforge::Lifecycle lc;
    std::vector<std::string> events;

    lc.append({
        .name = "step1",
        .on_start = [&](gorun::Context) { events.push_back("start:1"); },
        .on_stop = [&](gorun::Context) { events.push_back("stop:1"); }
    });
    lc.append({
        .name = "step2",
        .on_start = [&](gorun::Context) { events.push_back("start:2"); },
        .on_stop = [&](gorun::Context) { events.push_back("stop:2"); }
    });
    lc.append({
        .name = "step3-fails",
        .on_start = [&](gorun::Context) {
            events.push_back("start:3-throw");
            throw std::runtime_error("bind port failed");
        },
        .on_stop = [&](gorun::Context) { events.push_back("stop:3-must-not-run"); }
    });

    bool caught = false;
    try {
        lc.start(gorun::background());
    } catch (const std::runtime_error&) {
        caught = true;
    }

    GOFORGE_ASSERT(caught);
    std::vector<std::string> expected{
        "start:1", "start:2", "start:3-throw",
        "stop:2", "stop:1"
    };
    GOFORGE_ASSERT(events == expected);

    std::cout << "PASSED\n";
}

// Test 08: BlockingPool offloading blocking calls off gorun workers + exception propagation
void test_08_blocking_pool_offload_and_exceptions() {
    std::cout << "[TEST 08] BlockingPool offload without blocking gorun worker + exceptions... ";

    goforge::BlockingPool pool(4);
    gorun::Runtime single_rt(gorun::Config{.worker_count = 1});

    std::atomic<int> fast_ticks{0};
    int blocking_result = 0;
    bool caught_blocking_err = false;

    single_rt.block_on([&]() {
        gorun::Scope scope(single_rt);

        // Routine 1: runs a blocking OS sleep inside BlockingPool (must NOT freeze single_rt!)
        scope.spawn([&]() {
            blocking_result = pool.run([]() {
                std::this_thread::sleep_for(25ms);
                return 42;
            });
        });

        // Routine 2: fast green routine must keep progressing on the single gorun worker!
        scope.spawn([&]() {
            for (int i = 0; i < 10; ++i) {
                fast_ticks.fetch_add(1, std::memory_order_relaxed);
                gorun::sleep_for(1ms);
            }
        });

        // Routine 3: exception thrown on blocking OS thread propagates back to green routine
        scope.spawn([&]() {
            try {
                pool.run([]() -> int {
                    throw std::runtime_error("legacy db connection dropped");
                });
            } catch (const std::runtime_error&) {
                caught_blocking_err = true;
            }
        });
    });

    GOFORGE_ASSERT(blocking_result == 42);
    GOFORGE_ASSERT(fast_ticks.load() == 10);
    GOFORGE_ASSERT(caught_blocking_err);

    std::cout << "PASSED\n";
}

// Test 09: Full AppBuilder / App lifecycle + managed background green routines + graceful shutdown
void test_09_app_builder_background_and_graceful_shutdown() {
    std::cout << "[TEST 09] AppBuilder + invoke + background gorun routines + graceful stop... ";

    std::atomic<bool> bg_stopped_cleanly{false};
    std::atomic<int> hook_events{0};

    auto app = goforge::AppBuilder{}
        .with_config(goforge::AppConfig{
            .start_timeout = 2s,
            .stop_timeout = 2s,
            .blocking_pool_threads = 2
        })
        .provide_instance(Config{.dsn = "sqlite://memory", .max_conns = 4})
        .provide_singleton<Logger>()
        .provide_singleton_as<IUserRepository, InMemoryUserRepository, const Config&, Logger&>()
        .invoke([&](IUserRepository& repo, Logger& logger, goforge::Lifecycle& lc) {
            lc.append({
                .name = "user-service",
                .on_start = [&, name = repo.find_name(7)](gorun::Context) {
                    logger.log("started:" + name);
                    hook_events.fetch_add(1, std::memory_order_relaxed);
                },
                .on_stop = [&](gorun::Context) {
                    logger.log("stopped");
                    hook_events.fetch_add(10, std::memory_order_relaxed);
                }
            });
        })
        .build();

    app->spawn_background([&](gorun::Context ctx) {
        gorun::Channel<int> idle_ch(0);
        gorun::select(
            idle_ch.on_recv([](int) {}),
            ctx.on_done([&]() {
                bg_stopped_cleanly.store(true, std::memory_order_release);
            })
        );
    });

    // Trigger shutdown asynchronously while app->run() is active
    gorun::go([&]() {
        gorun::sleep_for(15ms);
        app->request_shutdown();
    });

    app->run();

    GOFORGE_ASSERT(bg_stopped_cleanly.load(std::memory_order_acquire));
    GOFORGE_ASSERT(hook_events.load() == 11);

    std::cout << "PASSED\n";
}

// Test 10: 200 concurrent RequestScopes across gorun workers (zero leaks, strict isolation)
void test_10_concurrent_request_scopes_stress() {
    std::cout << "[TEST 10] 200 concurrent RequestScopes across gorun workers (zero leaks)... ";

    RequestId::live_count.store(0);

    auto app = goforge::AppBuilder{}
        .provide_instance(Config{})
        .provide_singleton<Logger>()
        .provide_singleton_as<IUserRepository, InMemoryUserRepository, const Config&, Logger&>()
        .provide_scoped<RequestId>()
        .provide_scoped<ScopedUnitOfWork, RequestId&, IUserRepository&>()
        .build();

    app->start();
    std::atomic<int> handled{0};

    {
        gorun::Scope scope;
        for (int i = 0; i < 200; ++i) {
            gorun::go([&, i]() {
                auto req_scope = app->create_scope();
                auto& uow = req_scope.resolve<ScopedUnitOfWork>();
                int rid = uow.req_id.id;
                gorun::yield();
                GOFORGE_ASSERT(req_scope.resolve<RequestId>().id == rid);
                GOFORGE_ASSERT(uow.repo.find_name(i) == "user-" + std::to_string(i));
                handled.fetch_add(1, std::memory_order_relaxed);
            });
        }
    }

    auto errs = app->stop();
    GOFORGE_ASSERT(errs.empty());
    GOFORGE_ASSERT(handled.load() == 200);
    GOFORGE_ASSERT(RequestId::live_count.load() == 0);

    std::cout << "PASSED\n";
}

} // namespace

int main() {
    std::cout << std::unitbuf;
    std::cout << "======================================================================\n";
    std::cout << "  goforge Stage 1 Test Suite — Fx-Style DI, Lifecycle & BlockingPool\n";
    std::cout << "======================================================================\n";

    test_01_lifetimes_and_scope_isolation();
    test_02_factory_auto_injection_and_invoke();
    test_03_missing_and_circular_dependency_detection();
    test_04_captive_dependency_and_scope_guard();
    test_05_concurrent_singleton_init_single_worker();
    test_06_lifecycle_ordering();
    test_07_lifecycle_startup_failure_rollback();
    test_08_blocking_pool_offload_and_exceptions();
    test_09_app_builder_background_and_graceful_shutdown();
    test_10_concurrent_request_scopes_stress();

    std::cout << "======================================================================\n";
    std::cout << "  ALL 10 STAGE 1 TESTS PASSED SUCCESSFULLY!\n";
    std::cout << "======================================================================\n";
    return 0;
}
