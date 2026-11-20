#pragma once

/**
 * @file blocking_pool.hpp
 * @brief Dedicated OS-thread pool for offloading synchronous/blocking I/O (e.g., libpq, sqlite3,
 *        disk I/O) without blocking `gorun` M:N green-thread worker threads.
 */

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <exception>
#include <functional>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "gorun/gorun.hpp"

namespace goforge {

/**
 * @class BlockingPool
 * @brief Offloads blocking syscalls / legacy synchronous drivers onto dedicated OS threads while
 *        parking the calling `gorun` green routine on a `gorun::Channel` (zero worker starvation).
 */
class BlockingPool {
    std::mutex mtx_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> tasks_;
    std::vector<std::jthread> threads_;
    bool stopping_{false};

    void worker_loop() {
        for (;;) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lk(mtx_);
                cv_.wait(lk, [this]() { return stopping_ || !tasks_.empty(); });
                if (stopping_ && tasks_.empty()) {
                    return;
                }
                task = std::move(tasks_.front());
                tasks_.pop_front();
            }
            if (task) {
                task();
            }
        }
    }

public:
    explicit BlockingPool(std::size_t thread_count = 4) {
        if (thread_count == 0) {
            thread_count = 1;
        }
        threads_.reserve(thread_count);
        for (std::size_t i = 0; i < thread_count; ++i) {
            threads_.emplace_back([this]() { worker_loop(); });
        }
    }

    ~BlockingPool() {
        shutdown();
    }

    BlockingPool(const BlockingPool&) = delete;
    BlockingPool& operator=(const BlockingPool&) = delete;

    /// Returns the number of dedicated blocking OS threads.
    [[nodiscard]] std::size_t thread_count() const noexcept {
        return threads_.size();
    }

    /// Gracefully drains pending tasks and joins all blocking OS worker threads.
    void shutdown() {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (stopping_) return;
            stopping_ = true;
        }
        cv_.notify_all();
        for (auto& t : threads_) {
            if (t.joinable()) {
                t.join();
            }
        }
    }

    /**
     * @brief Executes a blocking callable `fn()` on the dedicated OS pool and parks the calling
     *        `gorun` green routine until completion (rethrows any exception thrown by `fn`).
     */
    template <typename Fn>
    auto run(Fn&& fn) -> std::invoke_result_t<std::decay_t<Fn>&> {
        using R = std::invoke_result_t<std::decay_t<Fn>&>;

        if constexpr (std::is_void_v<R>) {
            auto done_ch = std::make_shared<gorun::Channel<std::exception_ptr>>(1);
            auto fn_ptr = std::make_shared<std::decay_t<Fn>>(std::forward<Fn>(fn));
            {
                std::lock_guard<std::mutex> lk(mtx_);
                if (stopping_) {
                    throw std::runtime_error("BlockingPool is shutting down");
                }
                tasks_.emplace_back([done_ch, fn_ptr]() {
                    std::exception_ptr ep;
                    try {
                        (*fn_ptr)();
                    } catch (...) {
                        ep = std::current_exception();
                    }
                    done_ch->send(std::move(ep));
                });
            }
            cv_.notify_one();

            auto res = done_ch->recv();
            if (res && *res) {
                std::rethrow_exception(*res);
            }
        } else {
            struct Box {
                std::optional<R> value;
                std::exception_ptr error;
            };
            auto done_ch = std::make_shared<gorun::Channel<Box>>(1);
            auto fn_ptr = std::make_shared<std::decay_t<Fn>>(std::forward<Fn>(fn));
            {
                std::lock_guard<std::mutex> lk(mtx_);
                if (stopping_) {
                    throw std::runtime_error("BlockingPool is shutting down");
                }
                tasks_.emplace_back([done_ch, fn_ptr]() {
                    Box box;
                    try {
                        box.value.emplace((*fn_ptr)());
                    } catch (...) {
                        box.error = std::current_exception();
                    }
                    done_ch->send(std::move(box));
                });
            }
            cv_.notify_one();

            auto res = done_ch->recv();
            if (!res) {
                throw std::runtime_error("BlockingPool channel closed unexpectedly");
            }
            if (res->error) {
                std::rethrow_exception(res->error);
            }
            return std::move(*res->value);
        }
    }
};

} // namespace goforge
