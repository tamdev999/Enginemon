// engine/core/timing.cpp
// Fixed-timestep simulation scheduler implementation

#include "engine/core/timing.hpp"
#include <algorithm>

namespace enginemon {

SimulationScheduler::SimulationScheduler(int64_t tick_duration_ns, int32_t max_ticks)
    : tick_duration_ns_(tick_duration_ns)
    , max_ticks_per_update_(max_ticks)
{}

void SimulationScheduler::set_tick_duration(int64_t ns) {
    tick_duration_ns_ = ns > 0 ? ns : DEFAULT_TICK_NS;
}

void SimulationScheduler::set_max_ticks_per_update(int32_t max_ticks) {
    max_ticks_per_update_ = max_ticks > 0 ? max_ticks : DEFAULT_MAX_TICKS_PER_FRAME;
}

void SimulationScheduler::reset() {
    last_time_ns_ = 0;
    accumulator_ns_ = 0;
    total_ticks_ = 0;
    initialized_ = false;
}

void SimulationScheduler::reset(int64_t current_time_ns) {
    last_time_ns_ = current_time_ns;
    accumulator_ns_ = 0;
    total_ticks_ = 0;
    initialized_ = true;
}

SchedulerTickResult SimulationScheduler::update(int64_t current_time_ns) {
    SchedulerTickResult result;
    
    // First call: initialize and return 0 ticks
    if (!initialized_) {
        last_time_ns_ = current_time_ns;
        initialized_ = true;
        result.ticks_to_run = 0;
        result.interpolation_alpha = 0.0f;
        result.capped = false;
        return result;
    }
    
    // Calculate elapsed time since last update
    int64_t delta_ns = current_time_ns - last_time_ns_;
    last_time_ns_ = current_time_ns;
    
    // Clamp negative deltas (clock adjustment, etc.)
    if (delta_ns < 0) {
        delta_ns = 0;
    }
    
    return advance(delta_ns);
}

SchedulerTickResult SimulationScheduler::advance(int64_t delta_ns) {
    SchedulerTickResult result;
    result.capped = false;
    
    // Accumulate time
    accumulator_ns_ += delta_ns;
    
    // Count how many full ticks we can run
    int32_t ticks = 0;
    while (accumulator_ns_ >= tick_duration_ns_ && ticks < max_ticks_per_update_) {
        accumulator_ns_ -= tick_duration_ns_;
        ticks++;
        total_ticks_++;
    }
    
    // Cap if we hit max ticks (prevent death spiral)
    if (accumulator_ns_ >= tick_duration_ns_) {
        result.capped = true;
        // Drain excess accumulator to prevent perpetual spiral
        accumulator_ns_ = accumulator_ns_ % tick_duration_ns_;
    }
    
    result.ticks_to_run = ticks;
    
    // Calculate interpolation alpha (how far into the next tick we are)
    // 0.0 = just finished a tick, 1.0 = about to complete next tick
    result.interpolation_alpha = static_cast<float>(accumulator_ns_) / 
                                  static_cast<float>(tick_duration_ns_);
    
    return result;
}

} // namespace enginemon
