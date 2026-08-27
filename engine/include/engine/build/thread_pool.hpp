#pragma once
// engine/build/thread_pool.hpp
// Generic bounded thread pool for parallel compilation
//
// Not Crystal-specific - usable by any frontend compiler.

#include <atomic>
#include <condition_variable>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace enginemon::build {

// Bounded thread pool that reuses worker threads
// Workers pull jobs from a shared queue
class ThreadPool {
public:
    explicit ThreadPool(size_t worker_count);
    ~ThreadPool();
    
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
    
    // Submit a job and get a future for the result
    template<typename F, typename... Args>
    auto submit(F&& f, Args&&... args) -> std::future<std::invoke_result_t<F, Args...>> {
        using ReturnType = std::invoke_result_t<F, Args...>;
        
        auto task = std::make_shared<std::packaged_task<ReturnType()>>(
            std::bind(std::forward<F>(f), std::forward<Args>(args)...)
        );
        
        std::future<ReturnType> result = task->get_future();
        
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            if (stop_) {
                throw std::runtime_error("ThreadPool is stopped");
            }
            jobs_.emplace([task]() { (*task)(); });
        }
        
        condition_.notify_one();
        return result;
    }
    
    // Wait for all submitted jobs to complete
    void wait_all();
    
    // Get worker count
    size_t worker_count() const { return workers_.size(); }
    
    // Get number of jobs currently queued
    size_t queued_jobs() const {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        return jobs_.size();
    }
    
    // Statistics
    struct Stats {
        std::atomic<uint64_t> jobs_submitted{0};
        std::atomic<uint64_t> jobs_completed{0};
    };
    const Stats& stats() const { return stats_; }

private:
    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> jobs_;
    
    mutable std::mutex queue_mutex_;
    std::condition_variable condition_;
    std::condition_variable done_condition_;
    std::atomic<bool> stop_{false};
    std::atomic<size_t> active_jobs_{0};
    
    Stats stats_;
    
    void worker_loop();
};

// Get sensible default worker count
inline size_t default_worker_count() {
    size_t hw = std::thread::hardware_concurrency();
    if (hw == 0) hw = 4;  // Fallback
    return std::min(hw, size_t{16});  // Cap at 16
}

} // namespace enginemon::build
