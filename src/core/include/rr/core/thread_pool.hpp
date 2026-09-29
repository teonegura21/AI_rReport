// rr/core/thread_pool.hpp — a fixed pool of worker threads (ADR-0010).
//
// Why this exists
//   The hub runs blocking work — HTTP calls to the runtime, hashing, SQLite —
//   concurrently. Instead of spawning a thread per task, work is queued on a
//   small number of long-lived threads, one pool per resource class (CPU,
//   network), so a burst of network calls cannot starve CPU work.
//
// Semantics
//   * Tasks run in FIFO order. A task may accept a std::stop_token; it is
//     signalled when the pool shuts down, so long tasks can finish early.
//   * submit() returns a std::future. Exceptions thrown by the task are stored
//     in the future (standard behavior) — nothing is lost.
//   * post() is fire-and-forget. An escaping exception is caught at the
//     worker's entry point and reported to the pool's error handler, so a bug
//     in one task never terminates the process (ADR-0007).
//   * The destructor requests stop, lets running tasks return, and *drops*
//     queued tasks: their futures report std::future_errc::broken_promise.
//
// Thread-safety: all public members are thread-safe.
#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <exception>
#include <functional>
#include <future>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

namespace rr {

namespace detail {

// The result type of a pool task, which may be callable as task() or
// task(std::stop_token). `if constexpr` evaluates only the branch that
// applies; a conditional_t over two invoke_result_t would instantiate both
// and fail to compile for whichever signature the task does not have.
template <class Callable>
auto task_result_identity() {
    if constexpr (std::is_invocable_v<Callable&, std::stop_token>) {
        return std::type_identity<std::invoke_result_t<Callable&, std::stop_token>>{};
    } else {
        return std::type_identity<std::invoke_result_t<Callable&>>{};
    }
}

template <class Callable>
using TaskResult = typename decltype(task_result_identity<Callable>())::type;

}  // namespace detail

class ThreadPool {
public:
    using ErrorHandler = std::function<void(std::exception_ptr)>;

    // `thread_count` must be at least 1. `name` appears in error reports.
    explicit ThreadPool(std::size_t thread_count, std::string name = "pool", ErrorHandler on_error = {});
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
    ThreadPool(ThreadPool&&) = delete;
    ThreadPool& operator=(ThreadPool&&) = delete;
    ~ThreadPool();

    // Queues `task` (callable as task() or task(std::stop_token)) and returns
    // a future for its result.
    template <class F>
    auto submit(F&& task);

    // Queues `task` without a result channel. Exceptions go to the error handler.
    template <class F>
    void post(F&& task);

    // Blocks until the queue is empty and no task is running. For tests and
    // orderly shutdown; new tasks posted meanwhile are waited for as well.
    void wait_idle();

    [[nodiscard]] std::size_t thread_count() const noexcept { return threads_.size(); }
    [[nodiscard]] const std::string& name() const noexcept { return name_; }

private:
    using Task = std::move_only_function<void(std::stop_token)>;

    void enqueue(Task task);
    void worker_loop(const std::stop_token& stop);

    std::string name_;
    ErrorHandler on_error_;

    std::mutex mutex_;                     // guards queue_ and active_
    std::condition_variable_any work_cv_;  // signalled when a task is queued
    std::condition_variable_any idle_cv_;  // signalled when the pool may have become idle
    std::deque<Task> queue_;
    std::size_t active_ = 0;

    std::vector<std::jthread> threads_;  // last member: threads start after everything above exists
};

template <class F>
auto ThreadPool::submit(F&& task) {
    using Callable = std::decay_t<F>;
    constexpr bool kTakesStop = std::is_invocable_v<Callable&, std::stop_token>;
    using R = detail::TaskResult<Callable>;

    Callable callable(std::forward<F>(task));
    std::packaged_task<R(std::stop_token)> packaged{
        [callable = std::move(callable)](const std::stop_token& stop) mutable -> R {
            if constexpr (kTakesStop) {
                return callable(stop);
            } else {
                return callable();
            }
        }};
    std::future<R> future = packaged.get_future();
    enqueue([packaged = std::move(packaged)](std::stop_token stop) mutable { packaged(std::move(stop)); });
    return future;
}

template <class F>
void ThreadPool::post(F&& task) {
    using Callable = std::decay_t<F>;
    Callable callable(std::forward<F>(task));
    enqueue([this, callable = std::move(callable)](const std::stop_token& stop) mutable {
        try {
            if constexpr (std::is_invocable_v<Callable&, std::stop_token>) {
                callable(stop);
            } else {
                callable();
            }
        } catch (...) {
            if (on_error_) {
                on_error_(std::current_exception());
            }
        }
    });
}

}  // namespace rr
