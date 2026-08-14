#pragma once
// engine/core/timing.hpp
// Independent clocks for simulation, rendering, and audio
//
// TIMING MODEL:
// - Simulation: 1×, 2×, 4×, 8× speed (gameplay, scripts, battles, movement)
// - Rendering: VSync / uncapped / frame cap
// - Audio: Real-time 1× (music doesn't speed up during fast-forward)
//
// Fast-forward accelerates gameplay without forcing music speedup.

#include <chrono>
#include <cstdint>

namespace enginemon {

using Clock = std::chrono::steady_clock;
using Duration = std::chrono::duration<double>;
using TimePoint = Clock::time_point;

// Simulation speed multiplier
enum class SimSpeed : uint8_t {
    Normal = 1,     // 1×
    Fast = 2,       // 2×
    Faster = 4,     // 4×
    Fastest = 8     // 8×
};

// VSync / present mode
enum class PresentMode : uint8_t {
    VSync,              // Wait for VBlank (60Hz typical)
    VRR,                // Variable refresh rate (G-Sync/FreeSync)
    Immediate,          // No sync, lowest latency
    Mailbox,            // Triple buffer, no tearing, low latency
    FrameCapped         // Custom frame cap (e.g., 120fps)
};

// Timing configuration
struct TimingConfig {
    SimSpeed sim_speed = SimSpeed::Normal;
    PresentMode present_mode = PresentMode::VSync;
    uint32_t frame_cap = 0;         // 0 = no cap (when not VSync)
    bool allow_frame_skip = true;   // Skip rendering if behind
    uint32_t max_sim_steps = 4;     // Max sim steps per frame (catch-up limit)
};

// Simulation clock - can be sped up
class SimulationClock {
public:
    SimulationClock();
    
    // Call each real-time frame
    void update();
    
    // Get simulation delta time (affected by speed multiplier)
    double delta_time() const { return sim_delta_; }
    
    // Get simulation time scale
    double time_scale() const;
    
    // Speed control
    void set_speed(SimSpeed speed);
    SimSpeed speed() const { return speed_; }
    
    // Total simulated time
    double total_time() const { return total_sim_time_; }
    
    // Pause/resume
    void pause();
    void resume();
    bool is_paused() const { return paused_; }
    
    // Fixed timestep helpers
    static constexpr double FIXED_TIMESTEP = 1.0 / 60.0;  // 60 Hz
    double accumulator() const { return accumulator_; }
    void consume_accumulator(double dt) { accumulator_ -= dt; }

private:
    TimePoint last_update_;
    double sim_delta_ = 0.0;
    double total_sim_time_ = 0.0;
    double accumulator_ = 0.0;
    SimSpeed speed_ = SimSpeed::Normal;
    bool paused_ = false;
};

// Render clock - independent of simulation
class RenderClock {
public:
    RenderClock();
    
    void update();
    
    // Real delta time (for interpolation, animations)
    double delta_time() const { return delta_; }
    
    // Frame rate
    double fps() const { return fps_; }
    double average_fps() const { return avg_fps_; }
    
    // Frame timing stats
    double frame_time_ms() const { return delta_ * 1000.0; }
    
    // Total real time
    double total_time() const { return total_time_; }
    
    // Present mode
    void set_present_mode(PresentMode mode);
    PresentMode present_mode() const { return present_mode_; }
    
    // Frame cap (for FrameCapped mode)
    void set_frame_cap(uint32_t fps);
    bool should_render() const;  // Returns false if ahead of frame cap

private:
    TimePoint last_update_;
    double delta_ = 0.0;
    double total_time_ = 0.0;
    double fps_ = 0.0;
    double avg_fps_ = 0.0;
    double fps_accumulator_ = 0.0;
    int fps_frame_count_ = 0;
    
    PresentMode present_mode_ = PresentMode::VSync;
    uint32_t frame_cap_ = 0;
    TimePoint last_frame_;
};

// Audio clock - always real-time
class AudioClock {
public:
    AudioClock();
    
    void update();
    
    // Always real-time delta
    double delta_time() const { return delta_; }
    
    // Sample position for audio sync
    uint64_t sample_position(uint32_t sample_rate) const;
    
    // Total audio time
    double total_time() const { return total_time_; }

private:
    TimePoint last_update_;
    double delta_ = 0.0;
    double total_time_ = 0.0;
};

// Master timing coordinator
class TimingSystem {
public:
    TimingSystem();
    
    // Update all clocks
    void update();
    
    // Access individual clocks
    SimulationClock& simulation() { return sim_clock_; }
    RenderClock& render() { return render_clock_; }
    AudioClock& audio() { return audio_clock_; }
    
    const SimulationClock& simulation() const { return sim_clock_; }
    const RenderClock& render() const { return render_clock_; }
    const AudioClock& audio() const { return audio_clock_; }
    
    // Configuration
    void apply_config(const TimingConfig& config);
    const TimingConfig& config() const { return config_; }
    
    // Convenience: fast-forward control
    void set_fast_forward(bool enabled, SimSpeed speed = SimSpeed::Fastest);
    bool is_fast_forwarding() const { return fast_forwarding_; }

private:
    SimulationClock sim_clock_;
    RenderClock render_clock_;
    AudioClock audio_clock_;
    TimingConfig config_;
    bool fast_forwarding_ = false;
};

} // namespace enginemon
