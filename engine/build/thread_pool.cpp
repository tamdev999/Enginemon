// engine/build/thread_pool.cpp
// Generic bounded thread pool implementation

#include "engine/build/thread_pool.hpp"

namespace enginemon::build {

ThreadPool::ThreadPool(size_t worker_count) {
    workers_.reserve(worker_count);
    for (size_t i = 0; i < worker_count; ++i) {
        workers_.emplace_back([this] { worker_loop(); });
    }
}

ThreadPool::~ThreadPool() {
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        stop_ = true;
    }
    condition_.notify_all();
    
    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}

void ThreadPool::worker_loop() {
    while (true) {
        std::function<void()> job;
        
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            condition_.wait(lock, [this] {
                return stop_ || !jobs_.empty();
            });
            
            if (stop_ && jobs_.empty()) {
                return;
            }
            
            job = std::move(jobs_.front());
            jobs_.pop();
            ++active_jobs_;
        }
        
        // Execute job - catch any exception to prevent worker death
        try {
            job();
        } catch (...) {
            // Job threw exception - continue to next job
            // Error should be reported through the job's own error mechanism
        }
        
        ++stats_.jobs_completed;
        
        // Signal completion
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            --active_jobs_;
        }
        done_condition_.notify_all();
    }
}

void ThreadPool::wait_all() {
    std::unique_lock<std::mutex> lock(queue_mutex_);
    done_condition_.wait(lock, [this] {
        return jobs_.empty() && active_jobs_ == 0;
    });
}

} // namespace enginemon::build
