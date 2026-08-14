#pragma once
// engine/world/movement_manager.hpp
// Manages asynchronous scripted movements for actors
// Reference: pokecrystal OBJECT_STEP_DURATION (16 frames per step)
// Reference: Gen2Recomped scriptMove/updateScriptMoves pattern

#include "engine/core/types.hpp"
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>
#include <optional>

namespace enginemon {

// Semantic movement direction (matches Crystal encoding)
// Direction encoding: DOWN=0, UP=1, LEFT=2, RIGHT=3
enum class MovementDirection {
    Down = 0,
    Up = 1,
    Left = 2,
    Right = 3,
    None = 4
};

// Movement command types (semantic, not byte values)
enum class MovementCommandType {
    Step,           // Walk one tile in direction
    SlowStep,       // Slow walk (8 frames)
    BigStep,        // Jump-walk (32 frames)
    JumpStep,       // Ledge hop (32 frames)
    Turn,           // Change facing without moving
    StepEnd,        // End marker
    StepSleep,      // Pause in place
    Unknown
};

// Single movement command in a sequence
struct MovementCmd {
    MovementCommandType type = MovementCommandType::Step;
    MovementDirection direction = MovementDirection::Down;
    int param = 0;  // Optional parameter (e.g., sleep frames)
    
    // Frame durations from pokecrystal
    int get_duration_frames() const {
        switch (type) {
            case MovementCommandType::Step: return 16;      // Normal walk
            case MovementCommandType::SlowStep: return 32;  // Slow walk
            case MovementCommandType::BigStep: return 8;    // Fast step
            case MovementCommandType::JumpStep: return 32;  // Ledge hop
            case MovementCommandType::Turn: return 0;       // Instant
            case MovementCommandType::StepSleep: return param > 0 ? param : 16;
            case MovementCommandType::StepEnd: return 0;
            default: return 16;
        }
    }
    
    bool is_movement() const {
        return type == MovementCommandType::Step ||
               type == MovementCommandType::SlowStep ||
               type == MovementCommandType::BigStep ||
               type == MovementCommandType::JumpStep;
    }
};

// Active movement sequence for an actor
struct ActiveMovement {
    uint32_t actor_id;
    uint32_t coroutine_id;  // For resuming Lua script
    
    std::vector<MovementCmd> commands;
    size_t current_index = 0;
    int frames_remaining = 0;
    
    // Actor state during movement
    int start_x = 0;
    int start_y = 0;
    int current_x = 0;
    int current_y = 0;
    MovementDirection facing = MovementDirection::Down;
    
    // Status
    bool started = false;
    bool completed = false;
};

// Callback types
using MovementCompleteCallback = std::function<void(uint32_t actor_id, uint32_t coroutine_id)>;

// MovementManager tracks and advances all active scripted movements
// Independent from render FPS - uses simulation ticks
class MovementManager {
public:
    MovementManager() = default;
    ~MovementManager() = default;
    
    // Enqueue a movement sequence for an actor
    // Returns true if successfully enqueued
    // actor_id = 0 is player, 1+ are NPCs
    bool enqueue_movement(
        uint32_t actor_id,
        uint32_t coroutine_id,
        const std::vector<MovementCmd>& commands,
        int start_x, int start_y,
        MovementDirection start_facing
    );
    
    // Enqueue from batched table format {down=N, up=N, left=N, right=N}
    bool enqueue_movement_table(
        uint32_t actor_id,
        uint32_t coroutine_id,
        int down, int up, int left, int right,
        int start_x, int start_y,
        MovementDirection start_facing
    );
    
    // Advance all movements by one simulation tick
    // Call this each simulation frame (not render frame)
    // Returns list of completed coroutine_ids to resume
    std::vector<uint32_t> update();
    
    // Fast-forward: run N ticks at once
    // Used for simulation speed-up
    std::vector<uint32_t> update(int ticks);
    
    // Query state
    bool is_actor_moving(uint32_t actor_id) const;
    std::optional<ActiveMovement> get_movement(uint32_t actor_id) const;
    
    // Get current position/facing during movement
    // Returns nullopt if actor has no active movement
    struct ActorMovementState {
        int x;
        int y;
        MovementDirection facing;
        float progress;  // 0.0-1.0 within current step
    };
    std::optional<ActorMovementState> get_actor_state(uint32_t actor_id) const;
    
    // Cancel a movement
    void cancel_movement(uint32_t actor_id);
    void cancel_all();
    
    // Set callback for when movements complete
    void set_completion_callback(MovementCompleteCallback cb);
    
    // Get all pending completions (for testing without callback)
    const std::vector<std::pair<uint32_t, uint32_t>>& get_pending_completions() const {
        return pending_completions_;
    }
    void clear_pending_completions() { pending_completions_.clear(); }

private:
    // Active movements by actor_id
    std::unordered_map<uint32_t, ActiveMovement> active_movements_;
    
    // Callback for completion
    MovementCompleteCallback completion_callback_;
    
    // Track completions for testing/polling
    std::vector<std::pair<uint32_t, uint32_t>> pending_completions_;  // {actor_id, coroutine_id}
    
    // Process a single tick for one movement
    // Returns true if movement completed
    bool tick_movement(ActiveMovement& mv);
    
    // Apply direction offset to position
    static void apply_direction(int& x, int& y, MovementDirection dir);
    
    // Start next command in sequence
    void start_next_command(ActiveMovement& mv);
};

// Helper to convert batched counts to command sequence
std::vector<MovementCmd> batch_to_commands(int down, int up, int left, int right);

// Helper to convert direction string to enum
MovementDirection direction_from_string(const std::string& dir);
std::string direction_to_string(MovementDirection dir);

} // namespace enginemon
