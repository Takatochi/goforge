#pragma once

/**
 * @file web_app.hpp
 * @brief Lightweight, zero-boilerplate `WebApp` facade over `AppBuilder`, `Router`, `Lifecycle`,
 *        Observability, and `HttpServer`, with fluent route groups and zero-lambda `ControllerGroup`.
 */

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "goforge/app.hpp"
#include "goforge/obs/logger.hpp"
#include "goforge/obs/metrics.hpp"
#include "goforge/web/middleware.hpp"
#include "goforge/web/router.hpp"

#if !defined(GORUN_DISABLE_NET)
#include "goforge/http/http_server.hpp"
#endif

namespace goforge {

/**
 * @struct WebAppConfig
 * @brief Unified configuration for `goforge::WebApp` (HTTP server + App lifecycle + BlockingPool).
 */
struct WebAppConfig {
    std::string host{"127.0.0.1"};
    std::uint16_t port{8080};
    std::size_t io_threads{2};
    std::size_t max_connections{10000};
    std::size_t blocking_pool_threads{4};
    std::chrono::milliseconds read_timeout{std::chrono::seconds(15)};
    std::chrono::milliseconds write_timeout{std::chrono::seconds(15)};
    std::chrono::milliseconds start_timeout{std::chrono::seconds(15)};
    std::chrono::milliseconds stop_timeout{std::chrono::seconds(15)};
};

class WebApp;

/**
 * @class ControllerGroup
 * @brief Zero-lambda fluent route binder for a DI-managed controller class `Controller`.
 *
 * Example:
 * ```cpp
 * app.controller<UserController, IUserRepo>("/api/v1/users")
 *    .post("", &UserController::create)
 *    .get("/:id", &UserController::get_by_id);
 * ```
 */
template <typename Controller>
class ControllerGroup {
    web::RouteGroup group_;

public:
    explicit ControllerGroup(web::RouteGroup group) : group_(std::move(group)) {}

    ControllerGroup& use(web::HandlerFunc mw) {
        group_.use(std::move(mw));
        return *this;
    }

    ControllerGroup& timeout(std::chrono::milliseconds dur) {
        group_.use(web::middleware::timeout(dur));
        return *this;
    }

    ControllerGroup& rate_limit(std::size_t max_concurrent) {
        group_.use(web::middleware::rate_limit(max_concurrent));
        return *this;
    }

    template <typename MethodPtr>
        requires std::is_member_function_pointer_v<std::decay_t<MethodPtr>>
    ControllerGroup& handle(std::string_view method, std::string_view path, MethodPtr fn) {
        using Traits = detail::FunctionTraits<std::decay_t<MethodPtr>>;
        static_assert(
            std::is_base_of_v<typename Traits::class_type, Controller>,
            "Member function pointer must belong to this Controller class."
        );
        group_.handle(method, path, fn);
        return *this;
    }

    template <typename MethodPtr>
    ControllerGroup& get(std::string_view path, MethodPtr fn) { return handle("GET", path, fn); }

    template <typename MethodPtr>
    ControllerGroup& post(std::string_view path, MethodPtr fn) { return handle("POST", path, fn); }

    template <typename MethodPtr>
    ControllerGroup& put(std::string_view path, MethodPtr fn) { return handle("PUT", path, fn); }

    template <typename MethodPtr>
    ControllerGroup& del(std::string_view path, MethodPtr fn) { return handle("DELETE", path, fn); }

    template <typename MethodPtr>
    ControllerGroup& patch(std::string_view path, MethodPtr fn) { return handle("PATCH", path, fn); }
};

/**
 * @class WebGroup
 * @brief Fluent wrapper over `web::RouteGroup` adding middleware shortcuts and nested `.controller<C>()`.
 */
class WebGroup {
    std::shared_ptr<void> app_state_;
    WebApp* app_owner_{nullptr};
    web::RouteGroup group_;

    friend class WebApp;
    WebGroup(std::shared_ptr<void> app_state, WebApp* owner, web::RouteGroup group)
        : app_state_(std::move(app_state)), app_owner_(owner), group_(std::move(group)) {}

public:
    WebGroup& use(web::HandlerFunc mw) {
        group_.use(std::move(mw));
        return *this;
    }

    WebGroup& timeout(std::chrono::milliseconds dur) {
        group_.use(web::middleware::timeout(dur));
        return *this;
    }

    WebGroup& rate_limit(std::size_t max_concurrent) {
        group_.use(web::middleware::rate_limit(max_concurrent));
        return *this;
    }

    template <typename Validator>
    WebGroup& bearer_auth(Validator&& validator) {
        group_.use(web::middleware::bearer_auth(std::forward<Validator>(validator)));
        return *this;
    }

    [[nodiscard]] WebGroup group(std::string_view sub_prefix) {
        return WebGroup(app_state_, app_owner_, group_.group(sub_prefix));
    }

    template <typename Controller, typename... Deps>
    [[nodiscard]] ControllerGroup<Controller> controller(std::string_view sub_prefix = "");

    template <typename Fn>
    WebGroup& handle(std::string_view method, std::string_view path, Fn&& fn) {
        group_.handle(method, path, std::forward<Fn>(fn));
        return *this;
    }

    template <typename Fn>
    WebGroup& get(std::string_view path, Fn&& fn) { return handle("GET", path, std::forward<Fn>(fn)); }

    template <typename Fn>
    WebGroup& post(std::string_view path, Fn&& fn) { return handle("POST", path, std::forward<Fn>(fn)); }

    template <typename Fn>
    WebGroup& put(std::string_view path, Fn&& fn) { return handle("PUT", path, std::forward<Fn>(fn)); }

    template <typename Fn>
    WebGroup& del(std::string_view path, Fn&& fn) { return handle("DELETE", path, std::forward<Fn>(fn)); }

    template <typename Fn>
    WebGroup& patch(std::string_view path, Fn&& fn) { return handle("PATCH", path, std::forward<Fn>(fn)); }
};

/**
 * @class WebApp
 * @brief Lightweight ergonomic application handle unifying DI, Router, Observability,
 *        and `HttpServer` in ~10 lines of C++20. Uses shared handle semantics so fluent chaining
 *        like `auto app = WebApp::create({.port = 8080}).singleton<...>().use_defaults();` works seamlessly.
 */
class WebApp {
    struct State {
        WebAppConfig config;
        AppBuilder builder;
        std::shared_ptr<web::Router> router;
        std::shared_ptr<obs::StructuredLogger> logger;
        std::shared_ptr<obs::MetricsRegistry> metrics;
        std::shared_ptr<obs::HealthRegistry> health;
        std::unique_ptr<App> app;
        bool defaults_applied{false};

        explicit State(WebAppConfig cfg)
            : config(std::move(cfg))
            , router(std::make_shared<web::Router>())
            , logger(std::make_shared<obs::StructuredLogger>())
            , metrics(std::make_shared<obs::MetricsRegistry>())
            , health(std::make_shared<obs::HealthRegistry>()) {}

        void ensure_built() {
            if (app) return;

            builder.with_config(AppConfig{
                .start_timeout = config.start_timeout,
                .stop_timeout = config.stop_timeout,
                .blocking_pool_threads = config.blocking_pool_threads
            });

            builder.provide_instance(router);
            builder.provide_instance(logger);
            builder.provide_instance(metrics);
            builder.provide_instance(health);

#if !defined(GORUN_DISABLE_NET)
            http::HttpServerConfig srv_cfg;
            srv_cfg.host = config.host;
            srv_cfg.port = config.port;
            srv_cfg.io_threads = config.io_threads;
            srv_cfg.max_connections = config.max_connections;
            srv_cfg.read_timeout = config.read_timeout;
            srv_cfg.write_timeout = config.write_timeout;

            builder.provide_instance(srv_cfg);
            builder.provide([](const http::HttpServerConfig& cfg,
                               web::Router& r,
                               ServiceProvider& sp,
                               Lifecycle& lc) {
                auto srv = std::make_shared<http::HttpServer>(cfg, r.to_http_handler(&sp));
                srv->attach_lifecycle(lc, "http-server");
                return srv;
            });
            builder.invoke([](http::HttpServer&) {});
#endif

            app = builder.build();
        }
    };

    std::shared_ptr<State> state_;

    friend class WebGroup;

    template <typename Controller, typename... Deps>
    static void register_controller_in_state(State& st) {
        if constexpr (sizeof...(Deps) > 0) {
            st.builder.provide_scoped<Controller, Deps...>();
        } else if (!st.builder.services().template has<Controller>()) {
            static_assert(
                std::is_default_constructible_v<Controller>,
                "Controller with constructor dependencies must either be registered via "
                "app.scoped<MyController, Dep1, ...>() or app.controller<MyController, Dep1, ...>(prefix)."
            );
            st.builder.provide_scoped<Controller>();
        }
    }

public:
    explicit WebApp(WebAppConfig cfg = {})
        : state_(std::make_shared<State>(std::move(cfg))) {}

    WebApp(const WebApp&) = default;
    WebApp& operator=(const WebApp&) = default;
    WebApp(WebApp&&) noexcept = default;
    WebApp& operator=(WebApp&&) noexcept = default;

    /// Factory entry point: `auto app = goforge::WebApp::create({.port = 8080});`
    [[nodiscard]] static WebApp create(WebAppConfig cfg = {}) {
        return WebApp(std::move(cfg));
    }

    // ---- Concise DI Registration (auto-detects Interface -> Impl bindings!) ----

    template <typename T>
    WebApp& singleton() {
        state_->builder.provide_singleton<T>();
        return *this;
    }

    template <typename T1, typename T2, typename... Rest>
    WebApp& singleton() {
        if constexpr (!std::is_same_v<T1, T2> && std::is_base_of_v<T1, T2>) {
            state_->builder.provide_singleton_as<T1, T2, Rest...>();
        } else {
            state_->builder.provide_singleton<T1, T2, Rest...>();
        }
        return *this;
    }

    template <typename T>
    WebApp& scoped() {
        state_->builder.provide_scoped<T>();
        return *this;
    }

    template <typename T1, typename T2, typename... Rest>
    WebApp& scoped() {
        if constexpr (!std::is_same_v<T1, T2> && std::is_base_of_v<T1, T2>) {
            state_->builder.provide_scoped_as<T1, T2, Rest...>();
        } else {
            state_->builder.provide_scoped<T1, T2, Rest...>();
        }
        return *this;
    }

    template <typename T>
    WebApp& transient() {
        state_->builder.provide_transient<T>();
        return *this;
    }

    template <typename T1, typename T2, typename... Rest>
    WebApp& transient() {
        if constexpr (!std::is_same_v<T1, T2> && std::is_base_of_v<T1, T2>) {
            state_->builder.provide_transient_as<T1, T2, Rest...>();
        } else {
            state_->builder.provide_transient<T1, T2, Rest...>();
        }
        return *this;
    }

    template <typename Interface, typename Impl>
        requires std::is_base_of_v<Interface, Impl>
    WebApp& instance(std::shared_ptr<Impl> ptr) {
        state_->builder.services().template add_instance<Interface>(
            std::static_pointer_cast<Interface>(std::move(ptr))
        );
        return *this;
    }

    template <typename T>
    WebApp& instance(T&& value) {
        state_->builder.provide_instance(std::forward<T>(value));
        return *this;
    }

    template <typename Factory>
    WebApp& provide(Factory&& factory, Lifetime lifetime = Lifetime::Singleton) {
        state_->builder.provide(std::forward<Factory>(factory), lifetime);
        return *this;
    }

    template <typename Fn>
    WebApp& invoke(Fn&& fn) {
        state_->builder.invoke(std::forward<Fn>(fn));
        return *this;
    }

    // ---- Middleware & Built-in Observability ----

    WebApp& use(web::HandlerFunc mw) {
        state_->router->use(std::move(mw));
        return *this;
    }

    /// Installs metrics, panic recovery, JSON access logging, and mounts `/healthz`, `/readyz`, `/metrics`.
    WebApp& use_defaults(bool enable_access_log = true) {
        if (!state_->defaults_applied) {
            state_->defaults_applied = true;
            state_->router->use(state_->metrics->middleware());
            state_->router->use(web::middleware::recovery());
            if (enable_access_log) {
                state_->router->use(state_->logger->access_log_middleware());
            }
            state_->health->mount(*state_->router, *state_->metrics);
        }
        return *this;
    }

    WebApp& use_recovery() { return use(web::middleware::recovery()); }
    WebApp& use_metrics() { return use(state_->metrics->middleware()); }
    WebApp& use_access_log() { return use(state_->logger->access_log_middleware()); }
    WebApp& use_timeout(std::chrono::milliseconds dur) { return use(web::middleware::timeout(dur)); }
    WebApp& use_rate_limit(std::size_t max_concurrent) { return use(web::middleware::rate_limit(max_concurrent)); }

    // ---- Direct Routes, Groups & Controllers ----

    [[nodiscard]] WebGroup group(std::string_view prefix) {
        return WebGroup(state_, this, state_->router->group(prefix));
    }

    template <typename Controller, typename... Deps>
    [[nodiscard]] ControllerGroup<Controller> controller(std::string_view prefix = "") {
        register_controller_in_state<Controller, Deps...>(*state_);
        return ControllerGroup<Controller>(state_->router->group(prefix));
    }

    template <typename Fn>
    WebApp& handle(std::string_view method, std::string_view path, Fn&& fn) {
        state_->router->handle(method, path, std::forward<Fn>(fn));
        return *this;
    }

    template <typename Fn>
    WebApp& get(std::string_view path, Fn&& fn) { return handle("GET", path, std::forward<Fn>(fn)); }

    template <typename Fn>
    WebApp& post(std::string_view path, Fn&& fn) { return handle("POST", path, std::forward<Fn>(fn)); }

    template <typename Fn>
    WebApp& put(std::string_view path, Fn&& fn) { return handle("PUT", path, std::forward<Fn>(fn)); }

    template <typename Fn>
    WebApp& del(std::string_view path, Fn&& fn) { return handle("DELETE", path, std::forward<Fn>(fn)); }

    template <typename Fn>
    WebApp& patch(std::string_view path, Fn&& fn) { return handle("PATCH", path, std::forward<Fn>(fn)); }

    // ---- Component Accessors ----
    [[nodiscard]] web::Router& router() noexcept { return *state_->router; }
    [[nodiscard]] obs::StructuredLogger& logger() noexcept { return *state_->logger; }
    [[nodiscard]] obs::MetricsRegistry& metrics() noexcept { return *state_->metrics; }
    [[nodiscard]] obs::HealthRegistry& health() noexcept { return *state_->health; }

    [[nodiscard]] App& app() {
        state_->ensure_built();
        return *state_->app;
    }

    // ---- Execution & In-Memory Dispatch (works with or without Asio!) ----

    void start() {
        state_->ensure_built();
        state_->app->start();
    }

    std::vector<std::string> stop() {
        if (!state_->app) return {};
        return state_->app->stop();
    }

    void run() {
        state_->ensure_built();
        state_->app->run_until_signal();
    }

    /// Dispatches a synthetic `HttpRequest` directly through the DI + Router pipeline (ideal for tests & playgrounds).
    [[nodiscard]] http::HttpResponse dispatch(const http::HttpRequest& req,
                                              gorun::Context go_ctx = gorun::background()) {
        state_->ensure_built();
        return state_->router->dispatch(req, std::move(go_ctx), &state_->app->services());
    }

#if !defined(GORUN_DISABLE_NET)
    [[nodiscard]] std::uint16_t bound_port() {
        state_->ensure_built();
        return state_->app->services().resolve<http::HttpServer>().port();
    }
#endif
};

template <typename Controller, typename... Deps>
inline ControllerGroup<Controller> WebGroup::controller(std::string_view sub_prefix) {
    auto st = std::static_pointer_cast<WebApp::State>(app_state_);
    WebApp::register_controller_in_state<Controller, Deps...>(*st);
    return ControllerGroup<Controller>(group_.group(sub_prefix));
}

} // namespace goforge
