// engine/core/timing.hpp
// Fixed-timestep simulation scheduler
//
// Decouples simulation from render rate:
//   - Simulation advances in fixed ticks (e.g., 60 Hz)
//   - Rendering may run at any rate (VSync, VRR, uncapped)
//   - Simulation is deterministic regardless of render FPS
//
// Usage:
//   SimulationScheduler scheduler(SIM_TICK_NS);  // 60 Hz = 16666667 ns
//   while (running) {
//       int64_t now = get_monotonic_time_ns();
//       auto result = scheduler.update(now);
//       for (int i = 0; i < result.ticks_to_run; ++i) {
//           game_loop.tick();
//       }
//       render();  // may interpolate using result.interpolation_alpha
//   }

#pragma once

#include <cstdint>

namespace enginemon {

// Result of scheduler update
struct SchedulerTickResult {
    int32_t ticks_to_run = 0;       // Number of simulation ticks to execute
    float interpolation_alpha = 0;  // 0.0-1.0 for render interpolation (optional)
    bool capped = false;            // True if tick count was capped to prevent spiral
};

// Fixed-timestep simulation scheduler
// Maintains an accumulator to decouple simulation from render rate
class SimulationScheduler {
public:
    // Default: 60 Hz simulation (16.666... ms per tick)
    static constexpr int64_t DEFAULT_TICK_NS = 16666667;
    static constexpr int32_t DEFAULT_MAX_TICKS_PER_FRAME = 10;
    
    SimulationScheduler() = default;
    explicit SimulationScheduler(int64_t tick_duration_ns, int32_t max_ticks = DEFAULT_MAX_TICKS_PER_FRAME);
    
    // Set tick duration (nanoseconds per simulation tick)
    void set_tick_duration(int64_t ns);
    int64_t tick_duration_ns() const { return tick_duration_ns_; }
    
    // Set maximum ticks per update (prevents death spiral)
    void set_max_ticks_per_update(int32_t max_ticks);
    int32_t max_ticks_per_update() const { return max_ticks_per_update_; }
    
    // Reset scheduler state (call when starting new game/level)
    void reset();
    void reset(int64_t current_time_ns);
    
    // Update scheduler with current monotonic time
    // Returns number of simulation ticks to execute
    SchedulerTickResult update(int64_t current_time_ns);
    
    // Advance by a specific delta (for testing without real clock)
    // delta_ns: elapsed time in nanoseconds
    SchedulerTickResult advance(int64_t delta_ns);
    
    // Get current accumulator value (for debugging)
    int64_t accumulator_ns() const { return accumulator_ns_; }
    
    // Get total simulation ticks executed
    uint64_t total_ticks() const { return total_ticks_; }

private:
    int64_t tick_duration_ns_ = DEFAULT_TICK_NS;
    int32_t max_ticks_per_update_ = DEFAULT_MAX_TICKS_PER_FRAME;
    int64_t last_time_ns_ = 0;
    int64_t accumulator_ns_ = 0;
    uint64_t total_ticks_ = 0;
    bool initialized_ = false;
};

// Helper to convert Hz to nanoseconds per tick
constexpr int64_t hz_to_tick_ns(double hz) {
    return static_cast<int64_t>(1'000'000'000.0 / hz);
}

// Common tick rates
constexpr int64_t TICK_60HZ = hz_to_tick_ns(60.0);   // ~16.67 ms
constexpr int64_t TICK_30HZ = hz_to_tick_ns(30.0);   // ~33.33 ms
constexpr int64_t TICK_120HZ = hz_to_tick_ns(120.0); // ~8.33 ms

} // namespace enginemon
