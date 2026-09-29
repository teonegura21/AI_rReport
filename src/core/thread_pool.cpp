#include <rr/core/thread_pool.hpp>

#include <cstdio>

#include <rr/core/assert.hpp>

namespace rr {
namespace {

void default_error_handler(const std::string& pool_name, std::exception_ptr error) {
    try {
        std::rethrow_exception(std::move(error));
    } catch (const std::exception& exception) {
        (void)std::fprintf(stderr, "[rReport] uncaught exception in thread pool '%s': %s\n",
                           pool_name.c_str(), exception.what());
    } catch (...) {
        (void)std::fprintf(stderr, "[rReport] uncaught non-standard exception in thread pool '%s'\n",
                           pool_name.c_str());
    }
}

}  // namespace

ThreadPool::ThreadPool(std::size_t thread_count, std::string name, ErrorHandler on_error)
    : name_(std::move(name))
    , on_error_(std::move(on_error)) {
    RR_ASSERT(thread_count > 0, "a thread pool needs at least one thread");
    if (!on_error_) {
        on_error_ = [pool_name = name_](std::exception_ptr error) {
            default_error_handler(pool_name, std::move(error));
        };
    }
    threads_.reserve(thread_count);
    for (std::size_t index = 0; index < thread_count; ++index) {
        threads_.emplace_back([this](const std::stop_token& stop) { worker_loop(stop); });
    }
}

ThreadPool::~ThreadPool() {
    // Stop every worker; jthread's destructor would do this one thread at a
    // time, so request all stops first to shut down in parallel.
    for (std::jthread& thread : threads_) {
        thread.request_stop();
    }
    work_cv_.notify_all();
    threads_.clear();  // joins
    // Queued tasks are dropped here, when queue_ is destroyed; each dropped
    // packaged_task reports broken_promise to its future.
}

void ThreadPool::enqueue(Task task) {
    {
        const std::lock_guard lock(mutex_);
        queue_.push_back(std::move(task));
    }
    work_cv_.notify_one();
}

void ThreadPool::wait_idle() {
    std::unique_lock lock(mutex_);
    idle_cv_.wait(lock, [this] { return queue_.empty() && active_ == 0; });
}

void ThreadPool::worker_loop(const std::stop_token& stop) {
    while (true) {
        Task task;
        {
            std::unique_lock lock(mutex_);
            // Note: wait(lock, stop, pred) returns pred() — so it returns *true*
            // when stop is requested while work is still queued. Checking the
            // stop explicitly is what makes shutdown drop queued tasks instead
            // of draining them.
            const bool has_work = work_cv_.wait(lock, stop, [this] { return !queue_.empty(); });
            if (!has_work || stop.stop_requested()) {
                return;
            }
            task = std::move(queue_.front());
            queue_.pop_front();
            ++active_;
        }

        // post() wraps tasks in their own try/catch; submit() stores exceptions
        // in the future. This catch is the last line of defence for anything else.
        try {
            task(stop);
        } catch (...) {
            on_error_(std::current_exception());
        }

        {
            const std::lock_guard lock(mutex_);
            --active_;
        }
        idle_cv_.notify_all();
    }
}

}  // namespace rr
