#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace deflate {

/*
 * Simple work-queue thread pool.
 * Tasks are std::function<void()> submitted via submit().
 * wait_all() blocks until the queue is empty and all workers idle.
 */
class ThreadPool {
public:
    explicit ThreadPool(int num_threads = 0) {
        int n = num_threads;
        if (n <= 0) n = static_cast<int>(std::thread::hardware_concurrency());
        if (n <= 0) n = 1;

        workers_.reserve(static_cast<size_t>(n));
        for (int i = 0; i < n; ++i) {
            workers_.emplace_back([this] { worker_loop(); });
        }
    }

    ~ThreadPool() {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            stop_ = true;
        }
        cv_work_.notify_all();
        for (auto& t : workers_) t.join();
    }

    ThreadPool(const ThreadPool&)            = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    void submit(std::function<void()> task) {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            queue_.push_back(std::move(task));
            ++pending_;
        }
        cv_work_.notify_one();
    }

    void wait_all() {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_done_.wait(lock, [this] { return pending_ == 0; });
    }

    int thread_count() const noexcept {
        return static_cast<int>(workers_.size());
    }

private:
    void worker_loop() {
        for (;;) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_work_.wait(lock, [this] {
                    return stop_ || !queue_.empty();
                });
                if (stop_ && queue_.empty()) return;
                task = std::move(queue_.front());
                queue_.erase(queue_.begin());
            }
            task();
            {
                std::unique_lock<std::mutex> lock(mutex_);
                if (--pending_ == 0) cv_done_.notify_all();
            }
        }
    }

    std::vector<std::thread>            workers_;
    std::vector<std::function<void()>>  queue_;
    std::mutex                          mutex_;
    std::condition_variable             cv_work_;
    std::condition_variable             cv_done_;
    size_t                              pending_ = 0;
    bool                                stop_    = false;
};

} /* namespace deflate */
