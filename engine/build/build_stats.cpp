// engine/build/build_stats.cpp
// Build statistics implementation

#include "engine/build/build_stats.hpp"
#include <iostream>
#include <iomanip>

namespace enginemon::build {

void BuildStats::print_summary() const {
    std::cout << "\n=== Build Statistics ===\n";
    
    // Timing
    std::cout << "\nTiming:\n";
    std::cout << "  Discovery:      " << std::fixed << std::setprecision(1) 
              << discovery_time.elapsed_ms() << " ms\n";
    std::cout << "  Compilation:    " << compilation_time.elapsed_ms() << " ms\n";
    std::cout << "  Linker:         " << linker_time.elapsed_ms() << " ms\n";
    std::cout << "  Serialization:  " << serialization_time.elapsed_ms() << " ms\n";
    std::cout << "  --------------------------------\n";
    std::cout << "  Total:          " << total_time.elapsed_ms() << " ms ("
              << std::setprecision(2) << total_time.elapsed_seconds() << " s)\n";
    
    // Jobs
    std::cout << "\nJobs:\n";
    std::cout << "  Workers:        " << worker_count << "\n";
    std::cout << "  Total jobs:     " << total_jobs.load() << "\n";
    std::cout << "  Completed:      " << completed_jobs.load() << "\n";
    std::cout << "  Failed:         " << failed_jobs.load() << "\n";
    
    // Content
    std::cout << "\nContent:\n";
    std::cout << "  Maps:           " << maps_compiled.load() << "\n";
    std::cout << "  Tilesets:       " << tilesets_compiled.load() << "\n";
    std::cout << "  Sprites:        " << sprites_compiled.load() << "\n";
    std::cout << "  Scripts:        " << scripts_compiled.load() 
              << " (" << scripts_deduplicated.load() << " deduplicated)\n";
    
    // Cache
    std::cout << "\nCache:\n";
    std::cout << "  Hits:           " << cache_hits.load() << "\n";
    std::cout << "  Misses:         " << cache_misses.load() << "\n";
    uint64_t total = cache_hits.load() + cache_misses.load();
    if (total > 0) {
        double hit_rate = 100.0 * cache_hits.load() / total;
        std::cout << "  Hit rate:       " << std::fixed << std::setprecision(1) 
                  << hit_rate << "%\n";
    }
    
    // Size
    std::cout << "\nSize:\n";
    std::cout << "  Lua code:       " << total_lua_bytes.load() / 1024 << " KB\n";
    std::cout << "  Package:        " << package_bytes.load() / 1024 << " KB\n";
}

} // namespace enginemon::build
