#pragma once

/**
 * @file app.hpp
 * @brief Uber Fx-style `AppBuilder` and `App` orchestrating DI resolution, `Lifecycle` startup/shutdown,
 *        tracked background `gorun` green routines, and `BlockingPool` integration.
 */

#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "gorun/gorun.hpp"
#include "goforge/blocking_pool.hpp"
#include "goforge/di.hpp"
#include "goforge/lifecycle.hpp"

namespace goforge {

/**
 * @struct AppConfig
 * @brief Configuration for `goforge::App` startup/shutdown timeouts and blocking offload pool.
 */
struct AppConfig {
    std::chrono::milliseconds start_timeout{std::chrono::seconds(15)};
    std::chrono::milliseconds stop_timeout{std::chrono::seconds(15)};
    std::size_t blocking_pool_threads{4};
};

/**
 * @class App
 * @brief Top-level application runtime owning the `ServiceProvider`, `Lifecycle`, and background routines.
 */
class App {
    friend class AppBuilder;

    AppConfig config_;
    ServiceProvider provider_;
    std::shared_ptr<Lifecycle> lifecycle_;
    std::shared_ptr<BlockingPool> blocking_pool_;
    std::vector<std::function<void(ServiceProvider&)>> invokers_;

    gorun::Context app_ctx_;
    gorun::CancelFunc app_cancel_;
    gorun::WaitGroup bg_wg_;
    std::atomic<bool> started_{false};
    std::atomic<bool> stopped_{false};
    gorun::Channel<int> shutdown_signal_ch_{1};

    App(AppConfig cfg,
        ServiceProvider provider,
        std::shared_ptr<Lifecycle> lifecycle,
        std::shared_ptr<BlockingPool> blocking_pool,
        std::vector<std::function<void(ServiceProvider&)>> invokers)
        : config_(cfg)
        , provider_(std::move(provider))
        , lifecycle_(std::move(lifecycle))
        , blocking_pool_(std::move(blocking_pool))
        , invokers_(std::move(invokers)) {
        auto [ctx, cancel] = gorun::with_cancel(gorun::background());
        app_ctx_ = std::move(ctx);
        app_cancel_ = std::move(cancel);
    }

public:
    App(const App&) = delete;
    App& operator=(const App&) = delete;

    ~App() {
        if (started_.load(std::memory_order_acquire) && !stopped_.load(std::memory_order_acquire)) {
            try {
                stop();
            } catch (...) {
            }
        }
    }

    /// Accesses the root `ServiceProvider`.
    [[nodiscard]] ServiceProvider& services() noexcept { return provider_; }

    /// Accesses the `Lifecycle` coordinator.
    [[nodiscard]] Lifecycle& lifecycle() noexcept { return *lifecycle_; }

    /// Accesses the dedicated `BlockingPool` for offloading blocking I/O.
    [[nodiscard]] BlockingPool& blocking_pool() noexcept { return *blocking_pool_; }

    /// Returns the root application `gorun::Context` (canceled when `stop()` begins).
    [[nodiscard]] gorun::Context context() const noexcept { return app_ctx_; }

    /// Creates a new `ServiceScope` (e.g., per HTTP request).
    [[nodiscard]] ServiceScope create_scope() noexcept { return provider_.create_scope(); }

    /**
     * @brief Spawns a managed background green routine on `gorun` that receives the application
     *        `gorun::Context` and is automatically awaited during `App::stop()`.
     */
    template <typename Fn>
    void spawn_background(Fn&& fn) {
        bg_wg_.add(1);
        gorun::Context ctx = app_ctx_;
        gorun::go([this, ctx, func = std::forward<Fn>(fn)]() mutable {
            GORUN_DEFER { bg_wg_.done(); };
            func(ctx);
        });
    }

    /**
     * @brief Executes all registered `invoke(...)` functions (instantiating services & hooks),
     *        then starts all `Lifecycle` hooks in FIFO order with `config_.start_timeout`.
     */
    void start() {
        bool expected = false;
        if (!started_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            return;
        }

        try {
            for (auto& inv : invokers_) {
                inv(provider_);
            }
            auto [start_ctx, cancel] = gorun::with_timeout(app_ctx_, config_.start_timeout);
            GORUN_DEFER { cancel(); };
            lifecycle_->start(start_ctx);
        } catch (...) {
            started_.store(false, std::memory_order_release);
            throw;
        }
    }

    /**
     * @brief Performs graceful shutdown:
     * 1. Cancels the root `app_ctx_` so background routines exit cooperatively.
     * 2. Waits for all `spawn_background(...)` green routines via `gorun::WaitGroup`.
     * 3. Runs `Lifecycle::stop()` hooks in reverse (LIFO) order.
     * 4. Shuts down the `BlockingPool`.
     */
    std::vector<std::string> stop() {
        bool expected = false;
        if (!stopped_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            return {};
        }

        if (app_cancel_) {
            app_cancel_();
        }
        shutdown_signal_ch_.close();

        bg_wg_.wait();

        auto [stop_ctx, cancel] = gorun::with_timeout(gorun::background(), config_.stop_timeout);
        GORUN_DEFER { cancel(); };
        auto errors = lifecycle_->stop(stop_ctx);

        if (blocking_pool_) {
            blocking_pool_->shutdown();
        }
        return errors;
    }

    /// Triggers shutdown for an application blocked inside `run()`.
    void request_shutdown() {
        shutdown_signal_ch_.close();
    }

    /// Starts the application, parks until `request_shutdown()` is called, then executes graceful `stop()`.
    void run() {
        start();
        (void)shutdown_signal_ch_.recv();
        stop();
    }
};

/**
 * @class AppBuilder
 * @brief Fluent Uber Fx-style builder for wiring DI providers, invoke callbacks, and lifecycle hooks.
 */
class AppBuilder {
    AppConfig config_{};
    ServiceCollection services_{};
    std::vector<std::function<void(ServiceProvider&)>> invokers_;

public:
    AppBuilder() = default;

    /// Sets application startup/shutdown timeouts and blocking pool sizing.
    AppBuilder& with_config(AppConfig cfg) {
        config_ = cfg;
        return *this;
    }

    /// Direct access to the underlying `ServiceCollection`.
    ServiceCollection& services() noexcept { return services_; }

    /// Registers a pre-built instance as a `Singleton`.
    template <typename T>
    AppBuilder& provide_instance(T&& value) {
        services_.add_instance(std::forward<T>(value));
        return *this;
    }

    /// Registers a `Singleton` constructed with `Impl(Deps...)`.
    template <typename Impl, typename... Deps>
    AppBuilder& provide_singleton() {
        services_.add_singleton<Impl, Deps...>();
        return *this;
    }

    /// Registers a `Singleton` implementing `Interface` via `Impl(Deps...)`.
    template <typename Interface, typename Impl, typename... Deps>
    AppBuilder& provide_singleton_as() {
        services_.add_singleton_as<Interface, Impl, Deps...>();
        return *this;
    }

    /// Registers a `Transient` constructed with `Impl(Deps...)`.
    template <typename Impl, typename... Deps>
    AppBuilder& provide_transient() {
        services_.add_transient<Impl, Deps...>();
        return *this;
    }

    /// Registers a `Transient` implementing `Interface` via `Impl(Deps...)`.
    template <typename Interface, typename Impl, typename... Deps>
    AppBuilder& provide_transient_as() {
        services_.add_transient_as<Interface, Impl, Deps...>();
        return *this;
    }

    /// Registers a `Scoped` service constructed with `Impl(Deps...)`.
    template <typename Impl, typename... Deps>
    AppBuilder& provide_scoped() {
        services_.add_scoped<Impl, Deps...>();
        return *this;
    }

    /// Registers a `Scoped` service implementing `Interface` via `Impl(Deps...)`.
    template <typename Interface, typename Impl, typename... Deps>
    AppBuilder& provide_scoped_as() {
        services_.add_scoped_as<Interface, Impl, Deps...>();
        return *this;
    }

    /// Registers a factory `R(Deps...)` with the given `Lifetime` (default: `Singleton`).
    template <typename Factory>
    AppBuilder& provide(Factory&& factory, Lifetime lifetime = Lifetime::Singleton) {
        services_.provide(lifetime, std::forward<Factory>(factory));
        return *this;
    }

    /// Registers a factory explicitly bound to `ServiceType` (e.g., an interface).
    template <typename ServiceType, typename Factory>
    AppBuilder& provide_as(Factory&& factory, Lifetime lifetime = Lifetime::Singleton) {
        services_.add_factory<ServiceType>(lifetime, std::forward<Factory>(factory));
        return *this;
    }

    /**
     * @brief Registers an `invoke(Deps...)` function executed during `App::start()` to trigger eager
     *        service instantiation and register `Lifecycle` hooks (mirrors `fx.Invoke`).
     */
    template <typename Fn>
    AppBuilder& invoke(Fn&& fn) {
        auto holder = std::make_shared<std::decay_t<Fn>>(std::forward<Fn>(fn));
        invokers_.push_back([holder](ServiceProvider& sp) {
            sp.invoke(*holder);
        });
        return *this;
    }

    /**
     * @brief Validates the full dependency graph (missing deps, cycles, captive scopes) and builds `App`.
     */
    [[nodiscard]] std::unique_ptr<App> build() {
        auto lifecycle = std::make_shared<Lifecycle>();
        auto blocking_pool = std::make_shared<BlockingPool>(config_.blocking_pool_threads);

        services_.add_instance<Lifecycle>(lifecycle);
        services_.add_instance<BlockingPool>(blocking_pool);

        ServiceProvider provider = services_.build();
        return std::unique_ptr<App>(new App(
            config_,
            std::move(provider),
            std::move(lifecycle),
            std::move(blocking_pool),
            std::move(invokers_)
        ));
    }
};

} // namespace goforge
