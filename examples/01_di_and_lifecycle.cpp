#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

#include "goforge/goforge.hpp"

using namespace std::chrono_literals;

// ----------------------------------------------------------------------------
// 1. Domain Configuration & Singleton Services
// ----------------------------------------------------------------------------
struct ServiceConfig {
    std::string app_name{"orders-api"};
    std::string db_dsn{"postgres://127.0.0.1:5432/orders"};
};

class ConsoleLogger {
    gorun::Mutex mtx_;

public:
    void info(const std::string& tag, const std::string& msg) {
        std::lock_guard<gorun::Mutex> lk(mtx_);
        std::cout << "[" << tag << "] " << msg << "\n";
    }
};

class IOrderRepository {
public:
    virtual ~IOrderRepository() = default;
    virtual std::string fetch_order(int order_id) = 0;
};

class SimulatedDbOrderRepository final : public IOrderRepository {
    const ServiceConfig& cfg_;
    ConsoleLogger& log_;
    goforge::BlockingPool& blocking_pool_;

public:
    SimulatedDbOrderRepository(const ServiceConfig& cfg,
                               ConsoleLogger& log,
                               goforge::BlockingPool& pool,
                               goforge::Lifecycle& lc)
        : cfg_(cfg), log_(log), blocking_pool_(pool) {
        // Register Fx-style startup & shutdown hooks directly inside the constructor!
        lc.append({
            .name = "database-pool",
            .on_start = [this](gorun::Context) {
                log_.info("Lifecycle", "Connected DB pool to " + cfg_.db_dsn);
            },
            .on_stop = [this](gorun::Context) {
                log_.info("Lifecycle", "Closed DB pool cleanly");
            }
        });
    }

    std::string fetch_order(int order_id) override {
        // Offload blocking legacy C driver call onto BlockingPool so gorun workers never stall!
        return blocking_pool_.run([order_id]() {
            std::this_thread::sleep_for(5ms);
            return "Order#" + std::to_string(order_id) + "{status=PAID}";
        });
    }
};

// ----------------------------------------------------------------------------
// 2. Per-Request Scoped Service (Destroyed automatically when scope exits)
// ----------------------------------------------------------------------------
struct RequestSession {
    static inline std::atomic<int> counter{1000};
    std::string trace_id;

    RequestSession() : trace_id("req-" + std::to_string(counter.fetch_add(1))) {}
};

class OrderHandler {
    RequestSession& session_;
    IOrderRepository& repo_;
    ConsoleLogger& log_;

public:
    OrderHandler(RequestSession& session, IOrderRepository& repo, ConsoleLogger& log)
        : session_(session), repo_(repo), log_(log) {}

    void handle(int order_id) {
        std::string order = repo_.fetch_order(order_id);
        log_.info(session_.trace_id, "Served " + order);
    }
};

int main() {
    auto app = goforge::AppBuilder{}
        .with_config(goforge::AppConfig{
            .start_timeout = 5s,
            .stop_timeout = 5s,
            .blocking_pool_threads = 4
        })
        .provide_instance(ServiceConfig{})
        .provide_singleton<ConsoleLogger>()
        .provide_singleton_as<
            IOrderRepository,
            SimulatedDbOrderRepository,
            const ServiceConfig&,
            ConsoleLogger&,
            goforge::BlockingPool&,
            goforge::Lifecycle&>()
        .provide_scoped<RequestSession>()
        .provide_scoped<OrderHandler, RequestSession&, IOrderRepository&, ConsoleLogger&>()
        // Fx-style Invoke: instantiates the service graph and registers hooks on App::start()
        .invoke([](IOrderRepository&, ConsoleLogger& log, const ServiceConfig& cfg) {
            log.info("App", "Wired service graph for " + cfg.app_name);
        })
        .build();

    app->start();

    // Managed background heartbeat green routine (automatically canceled & awaited on app->stop())
    app->spawn_background([&](gorun::Context ctx) {
        auto& log = app->services().resolve<ConsoleLogger>();
        log.info("Heartbeat", "Background worker started on gorun M:N scheduler");
        gorun::Channel<int> never_ch(0);
        gorun::select(
            never_ch.on_recv([](int) {}),
            ctx.on_done([&]() {
                log.info("Heartbeat", "Received shutdown context signal, exiting cleanly");
            })
        );
    });

    // Simulate 8 concurrent incoming requests, each with its own Scoped DI container
    {
        gorun::Scope req_wg;
        for (int id = 1; id <= 8; ++id) {
            gorun::go([&, id]() {
                auto req_scope = app->create_scope();
                req_scope.invoke([id](OrderHandler& handler) {
                    handler.handle(id);
                });
            });
        }
    }

    // Graceful shutdown: cancels background routines, joins WaitGroup, runs OnStop in reverse order
    auto errors = app->stop();
    if (!errors.empty()) {
        std::cerr << "Shutdown encountered " << errors.size() << " error(s)\n";
        return 1;
    }

    return 0;
}
