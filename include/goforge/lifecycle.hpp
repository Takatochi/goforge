#pragma once

/**
 * @file lifecycle.hpp
 * @brief Uber Fx-style ordered application lifecycle (`Lifecycle`, `LifecycleHook`) with
 *        automatic rollback on startup failure and reverse-order graceful shutdown.
 */

#include <cstddef>
#include <exception>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "gorun/gorun.hpp"

namespace goforge {

/**
 * @struct LifecycleHook
 * @brief A pair of `on_start` / `on_stop` callbacks registered with `Lifecycle::append(...)`.
 */
struct LifecycleHook {
    std::string name;                                 ///< Human-readable hook identifier for diagnostics
    std::function<void(gorun::Context)> on_start{};   ///< Invoked in registration order during `App::start()`
    std::function<void(gorun::Context)> on_stop{};    ///< Invoked in reverse order during `App::stop()`
};

/**
 * @class Lifecycle
 * @brief Manages ordered startup and reverse-order shutdown of application components.
 *
 * Mirrors Uber Fx's `fx.Lifecycle`:
 * - Services can inject `goforge::Lifecycle&` in their constructors/factories and call `append(...)`.
 * - `start(ctx)` runs hooks `[0 .. N-1]`. If hook `k` throws, `on_stop` is automatically invoked in
 *   reverse order for hooks `[k-1 .. 0]` before rethrowing the original startup error.
 * - `stop(ctx)` runs `on_stop` in reverse order `[N-1 .. 0]`, ensuring every component gets a chance
 *   to clean up even if an earlier `on_stop` throws.
 */
class Lifecycle {
    gorun::Mutex mtx_;
    std::vector<LifecycleHook> hooks_;
    std::size_t started_count_{0};

public:
    Lifecycle() = default;
    Lifecycle(const Lifecycle&) = delete;
    Lifecycle& operator=(const Lifecycle&) = delete;

    /// Appends a lifecycle hook. Safe to call from constructors, factories, or invoke callbacks.
    void append(LifecycleHook hook) {
        std::lock_guard<gorun::Mutex> lk(mtx_);
        hooks_.push_back(std::move(hook));
    }

    /// Returns the total number of registered hooks.
    [[nodiscard]] std::size_t hook_count() {
        std::lock_guard<gorun::Mutex> lk(mtx_);
        return hooks_.size();
    }

    /// Starts all registered hooks in FIFO order. Automatically rolls back already-started hooks on failure.
    void start(gorun::Context ctx) {
        std::vector<LifecycleHook> snapshot;
        {
            std::lock_guard<gorun::Mutex> lk(mtx_);
            snapshot = hooks_;
            started_count_ = 0;
        }

        std::size_t started = 0;
        try {
            for (std::size_t i = 0; i < snapshot.size(); ++i) {
                if (ctx.is_canceled()) {
                    throw std::runtime_error(
                        "Lifecycle start aborted before hook '" + snapshot[i].name + "': " +
                        std::string(gorun::to_string(*ctx.err()))
                    );
                }
                if (snapshot[i].on_start) {
                    snapshot[i].on_start(ctx);
                }
                started = i + 1;
            }
            std::lock_guard<gorun::Mutex> lk(mtx_);
            started_count_ = started;
        } catch (...) {
            // Rollback hooks [started-1 .. 0] in reverse order
            auto [rb_ctx, rb_cancel] = gorun::with_cancel(gorun::background());
            GORUN_DEFER { rb_cancel(); };
            for (std::size_t i = started; i > 0; --i) {
                const auto& hook = snapshot[i - 1];
                if (hook.on_stop) {
                    try {
                        hook.on_stop(rb_ctx);
                    } catch (...) {
                    }
                }
            }
            {
                std::lock_guard<gorun::Mutex> lk(mtx_);
                started_count_ = 0;
            }
            throw;
        }
    }

    /**
     * @brief Stops all started hooks in reverse (LIFO) order.
     * @return List of error messages from hooks whose `on_stop` threw (empty if clean shutdown).
     */
    std::vector<std::string> stop(gorun::Context ctx) {
        std::vector<LifecycleHook> snapshot;
        std::size_t count = 0;
        {
            std::lock_guard<gorun::Mutex> lk(mtx_);
            snapshot = hooks_;
            count = started_count_;
            started_count_ = 0;
        }

        std::vector<std::string> errors;
        for (std::size_t i = count; i > 0; --i) {
            const auto& hook = snapshot[i - 1];
            if (!hook.on_stop) continue;
            try {
                hook.on_stop(ctx);
            } catch (const std::exception& ex) {
                errors.push_back("Hook '" + hook.name + "' on_stop failed: " + ex.what());
            } catch (...) {
                errors.push_back("Hook '" + hook.name + "' on_stop failed with unknown exception");
            }
        }
        return errors;
    }
};

} // namespace goforge
