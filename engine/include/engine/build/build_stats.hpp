#pragma once
// engine/build/build_stats.hpp
// Build statistics and timing for compiler

#include <chrono>
#include <cstdint>
#include <string>
#include <atomic>

namespace enginemon::build {

// High-resolution timer for build phases
class PhaseTimer {
public:
    using Clock = std::chrono::steady_clock;
    using Duration = std::chrono::duration<double, std::milli>;
    
    void start() {
        start_ = Clock::now();
    }
    
    void stop() {
        end_ = Clock::now();
    }
    
    double elapsed_ms() const {
        return Duration(end_ - start_).count();
    }
    
    double elapsed_seconds() const {
        return elapsed_ms() / 1000.0;
    }

private:
    Clock::time_point start_;
    Clock::time_point end_;
};

// Build statistics (thread-safe counters)
struct BuildStats {
    // Timing
    PhaseTimer total_time;
    PhaseTimer discovery_time;
    PhaseTimer compilation_time;
    PhaseTimer linker_time;
    PhaseTimer serialization_time;
    
    // Job counts
    std::atomic<uint32_t> total_jobs{0};
    std::atomic<uint32_t> completed_jobs{0};
    std::atomic<uint32_t> failed_jobs{0};
    
    // Content counts
    std::atomic<uint32_t> maps_compiled{0};
    std::atomic<uint32_t> scripts_compiled{0};
    std::atomic<uint32_t> scripts_deduplicated{0};
    std::atomic<uint32_t> tilesets_compiled{0};
    std::atomic<uint32_t> sprites_compiled{0};
    
    // Cache stats
    std::atomic<uint64_t> cache_hits{0};
    std::atomic<uint64_t> cache_misses{0};
    
    // Size stats
    std::atomic<uint64_t> total_lua_bytes{0};
    std::atomic<uint64_t> package_bytes{0};
    
    // Worker count
    uint32_t worker_count{0};
    
    // Print summary
    void print_summary() const;
};

} // namespace enginemon::build
