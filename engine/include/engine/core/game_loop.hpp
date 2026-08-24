#pragma once
// engine/core/game_loop.hpp
// Headless game loop for testing and simulation
//
// Wires together existing systems:
// - Input handling → facing update → collision query → MovementManager → ticks
// - Interaction → InteractionSystem → script ID → LuaRuntime
// - Script yields/resumes for text/movement
//
// Reference: Gen2Recomped OverworldController.lua handleInput/update/interact

#include "engine/core/types.hpp"
#include "engine/core/game_state.hpp"
#include "engine/world/runtime_map.hpp"
#include "engine/world/collision.hpp"
#include "engine/world/collision_types.hpp"
#include "engine/world/interaction.hpp"
#include "engine/world/movement_manager.hpp"
#include "engine/scripting/lua_runtime.hpp"

#include <cstdint>
#include <string>
#include <vector>
#include <optional>
#include <functional>
#include <unordered_map>

namespace enginemon {

//=============================================================================
// INPUT TYPES
// Native input intents - no framework, just semantic actions
//=============================================================================

enum class InputAction {
    None,
    MoveUp,
    MoveDown,
    MoveLeft,
    MoveRight,
    Interact,       // A-button
};

// Convert input action to direction (for movement inputs)
inline std::optional<Direction> input_to_direction(InputAction action) {
    switch (action) {
        case InputAction::MoveUp: return Direction::Up;
        case InputAction::MoveDown: return Direction::Down;
        case InputAction::MoveLeft: return Direction::Left;
        case InputAction::MoveRight: return Direction::Right;
        default: return std::nullopt;
    }
}

//=============================================================================
// PLAYER STATE
// Minimal player state for simulation
//=============================================================================

struct PlayerState {
    int32_t x = 0;              // Current tile X
    int32_t y = 0;              // Current tile Y
    Direction facing = Direction::Down;
    bool is_moving = false;     // True if mid-step
    bool surfing = false;       // On water
    
    // Movement target during step
    int32_t target_x = 0;
    int32_t target_y = 0;
    int32_t frames_remaining = 0;
};

//=============================================================================
// GAME LOOP STATE
//=============================================================================

enum class LoopState {
    Idle,               // Ready for input
    Moving,             // Player mid-step (movement in progress)
    ScriptRunning,      // Script executing (blocks input)
    ScriptYielded,      // Script yielded for movement/dialog
};

// Result of processing an input
struct InputResult {
    bool accepted = false;      // Input was processed
    bool blocked = false;       // Movement was blocked (collision)
    bool interaction = false;   // Interaction triggered
    std::string script_id;      // Script triggered (if any)
    std::string block_reason;   // Why movement blocked
};

// Result of ticking the loop
struct TickResult {
    bool movement_complete = false;     // A movement finished this tick
    bool script_complete = false;       // A script finished normally this tick
    bool script_error = false;          // A script failed with error this tick
    bool script_resumed = false;        // A yielded script was resumed
    std::vector<uint32_t> completed_coroutines;
};

//=============================================================================
// NPC MOVEMENT BEHAVIOR
// Reference: pokecrystal/constants/map_object_constants.asm (SPRITEMOVEDATA_*)
// Reference: pokecrystal/engine/overworld/map_objects.asm (MovementFunction_*)
//=============================================================================

// NPC movement behavior types (maps to Crystal SPRITEMOVEFN_*)
// These are the FUNCTIONS, not the MOVEDATA (data maps to function)
enum class NpcMovementBehavior : uint8_t {
    Standing = 0,           // Stands still, maintains initial facing
    RandomWalkY = 1,        // Walks up/down within radius
    RandomWalkX = 2,        // Walks left/right within radius
    RandomWalkXY = 3,       // Walks in any direction within radius
    RandomSpinSlow = 4,     // Turns randomly (slow timer)
    RandomSpinFast = 5,     // Turns randomly (fast timer)
    // Future: Follow, Scripted, etc.
};

// Convert Crystal SPRITEMOVEDATA_* byte to our behavior enum
// Reference: pokecrystal/data/sprites/map_objects.asm SpriteMovementData table
inline NpcMovementBehavior movement_data_to_behavior(uint8_t movement_type) {
    // Crystal SPRITEMOVEDATA constants (from map_object_constants.asm):
    // 0x01 = STILL (standing)
    // 0x02 = WANDER (random walk any direction)
    // 0x03 = SPINRANDOM_SLOW
    // 0x04 = WALK_UP_DOWN
    // 0x05 = WALK_LEFT_RIGHT
    // 0x06-0x09 = STANDING_DOWN/UP/LEFT/RIGHT
    // 0x0A = SPINRANDOM_FAST
    // 0x14 = SCRIPTED (standing, controlled by scripts)
    switch (movement_type) {
        case 0x01: return NpcMovementBehavior::Standing;           // STILL
        case 0x02: return NpcMovementBehavior::RandomWalkXY;       // WANDER
        case 0x03: return NpcMovementBehavior::RandomSpinSlow;     // SPINRANDOM_SLOW
        case 0x04: return NpcMovementBehavior::RandomWalkY;        // WALK_UP_DOWN
        case 0x05: return NpcMovementBehavior::RandomWalkX;        // WALK_LEFT_RIGHT
        case 0x06:  // STANDING_DOWN
        case 0x07:  // STANDING_UP
        case 0x08:  // STANDING_LEFT
        case 0x09:  // STANDING_RIGHT
            return NpcMovementBehavior::Standing;
        case 0x0A: return NpcMovementBehavior::RandomSpinFast;     // SPINRANDOM_FAST
        case 0x14: return NpcMovementBehavior::Standing;           // SCRIPTED
        default:   return NpcMovementBehavior::Standing;           // Unknown = standing
    }
}

// Get initial facing direction from movement_type
// Reference: pokecrystal SpriteMovementData table SPRITEMOVEATTR_FACING
inline Direction movement_data_to_facing(uint8_t movement_type) {
    // STANDING_* types have explicit facing
    switch (movement_type) {
        case 0x06: return Direction::Down;   // STANDING_DOWN
        case 0x07: return Direction::Up;     // STANDING_UP
        case 0x08: return Direction::Left;   // STANDING_LEFT
        case 0x09: return Direction::Right;  // STANDING_RIGHT
        default:   return Direction::Down;   // Default facing
    }
}

//=============================================================================
// NPC STATE (for collision/interaction/movement)
//=============================================================================

struct NpcState {
    uint16_t id = 0;            // Object local ID (1-indexed)
    int32_t x = 0;              // Current tile X
    int32_t y = 0;              // Current tile Y
    Direction facing = Direction::Down;
    bool is_moving = false;
    bool is_trainer = false;
    std::string script_id;
    std::string visibility_flag;
    bool visible = true;
    
    // Movement behavior (compiled from Crystal movement_type)
    NpcMovementBehavior behavior = NpcMovementBehavior::Standing;
    int8_t radius_x = 0;        // Movement radius X (tiles from initial pos)
    int8_t radius_y = 0;        // Movement radius Y
    int32_t init_x = 0;         // Initial/home position X
    int32_t init_y = 0;         // Initial/home position Y
    
    // Movement state machine
    int32_t idle_timer = 0;     // Frames until next movement attempt
    int32_t target_x = 0;       // Movement target X (during move)
    int32_t target_y = 0;       // Movement target Y
    int32_t move_progress = 0;  // Ticks into current movement
    
    // Frozen state (during script interaction)
    bool frozen = false;        // Script is interacting with this NPC
};

//=============================================================================
// HEADLESS GAME LOOP
// Wires collision, interaction, movement, and scripting together
//=============================================================================

class HeadlessGameLoop {
public:
    HeadlessGameLoop();
    ~HeadlessGameLoop();
    
    //=========================================================================
    // INITIALIZATION
    //=========================================================================
    
    // Load a map from package (runtime-native types only)
    void load_map(const RuntimeMap& map);
    
    // Set collision data (semantic CollisionClass for the current map)
    void set_collision_data(std::function<CollisionClass(int32_t, int32_t)> get_collision);
    
    // Spawn player at position
    void spawn_player(int32_t x, int32_t y, Direction facing = Direction::Down);
    
    // Start player movement to explicit target (for connection seam step)
    // Reference: Gen2Recomped crossConnection - non-scripted movement to landing
    // Uses the authoritative movement path (movement_manager + state_=Moving)
    void start_player_movement_to(int32_t target_x, int32_t target_y, Direction facing);
    
    // Add NPC for collision/interaction
    void add_npc(const NpcState& npc);
    void clear_npcs();
    
    //=========================================================================
    // INPUT PROCESSING
    // Reference: Gen2Recomped handleInput()
    //=========================================================================
    
    // Process a single input action
    // Returns immediately - movement/scripts run over ticks
    InputResult process_input(InputAction action);
    
    // Check if input is currently locked (moving or script running)
    bool is_input_locked() const;
    
    //=========================================================================
    // SIMULATION TICKS
    // Reference: Gen2Recomped update()
    //=========================================================================
    
    // Advance simulation by one tick (frame)
    // Progresses movement, resumes yielded scripts
    TickResult tick();
    
    // Advance simulation by N ticks
    TickResult tick(int count);
    
    //=========================================================================
    // STATE QUERIES
    //=========================================================================
    
    // Player state
    const PlayerState& player() const { return player_; }
    PlayerState& player() { return player_; }
    
    // Loop state
    LoopState state() const { return state_; }
    bool is_idle() const { return state_ == LoopState::Idle; }
    bool is_moving() const { return state_ == LoopState::Moving; }
    bool is_script_running() const { return state_ == LoopState::ScriptRunning || 
                                            state_ == LoopState::ScriptYielded; }
    
    // Current map
    const RuntimeMap* current_map() const { return current_map_; }
    
    // Active script
    uint32_t active_coroutine() const { return active_coroutine_; }
    
    //=========================================================================
    // SCRIPTING
    //=========================================================================
    
    // Set the Lua runtime for script execution.
    // Also wires the deferred-script scheduling callback so Sem_Sdefer
    // (emitted as ctx.game:behavior("Sdefer_<id>")) actually schedules.
    void set_lua_runtime(LuaRuntime* runtime);
    
    // Load and start a script by semantic ID
    bool start_script(const std::string& script_id);
    
    // Resume a yielded script
    bool resume_script();
    
    // Schedule a script to run after the current active script completes.
    // If no script is running the deferred script starts immediately on the next tick.
    // Source: pokecrystal Script_sdefer — deferred target runs after current script.
    void schedule_deferred_script(const std::string& script_id);
    
    // Get script code callback (to be set by test/game code)
    using ScriptLoader = std::function<std::string(const std::string& script_id)>;
    void set_script_loader(ScriptLoader loader) { script_loader_ = std::move(loader); }
    
    //=========================================================================
    // CALLBACKS
    //=========================================================================
    
    using InteractionCallback = std::function<void(const InteractionResult&)>;
    using MovementCallback = std::function<void(int32_t x, int32_t y, Direction dir)>;
    
    void set_interaction_callback(InteractionCallback cb) { on_interaction_ = std::move(cb); }
    void set_movement_callback(MovementCallback cb) { on_movement_complete_ = std::move(cb); }
    
    //=========================================================================
    // DETERMINISM
    //=========================================================================
    
    // Get a hash of current state (for determinism checks)
    uint64_t state_hash() const;
    
    // Reset to clean state
    void reset();
    
    //=========================================================================
    // GAME STATE INTEGRATION
    // HeadlessGameLoop does not own gameplay state - it receives it
    //=========================================================================
    
    // Set the GameState for RNG and other gameplay state
    // HeadlessGameLoop does NOT own this - caller maintains ownership
    void set_game_state(GameState* state) { game_state_ = state; }
    GameState* game_state() { return game_state_; }
    const GameState* game_state() const { return game_state_; }
    
    //=========================================================================
    // NPC STATE SNAPSHOT/RESTORE
    // For deterministic save/load
    //=========================================================================
    
    // Snapshot current NPC states into GameState
    // Must be called before serializing GameState
    void snapshot_npc_states(const std::string& map_id);
    
    // Restore NPC states from GameState
    // Must be called after loading a map and before simulation resumes
    void restore_npc_states(const std::string& map_id);
    
    //=========================================================================
    // NPC AUTONOMOUS MOVEMENT
    // Reference: pokecrystal/engine/overworld/map_objects.asm
    // Reference: Gen2Recomped/src/world/NPC.lua
    //=========================================================================
    
    // Get/set NPC frozen state (for script interaction)
    void freeze_npc(uint16_t npc_id);
    void unfreeze_npc(uint16_t npc_id);
    bool is_npc_frozen(uint16_t npc_id) const;
    
    // Get NPC by ID
    NpcState* get_npc(uint16_t npc_id);
    const NpcState* get_npc(uint16_t npc_id) const;
    
    // Get all NPCs
    const std::vector<NpcState>& npcs() const { return npcs_; }

private:
    //=========================================================================
    // STATE
    //=========================================================================
    
    LoopState state_ = LoopState::Idle;
    PlayerState player_;
    
    // Map data - owned by the loop to prevent dangling pointers
    std::optional<RuntimeMap> current_map_owned_;
    const RuntimeMap* current_map_ = nullptr;  // Points to current_map_owned_ when set
    std::function<CollisionClass(int32_t, int32_t)> get_collision_;
    
    // NPCs
    std::vector<NpcState> npcs_;
    
    // Systems
    Collision collision_;
    Interaction interaction_;
    MovementManager movement_manager_;
    
    // Scripting
    LuaRuntime* lua_runtime_ = nullptr;
    ScriptLoader script_loader_;
    uint32_t active_coroutine_ = 0;
    std::string active_script_id_;
    bool script_resumed_this_tick_ = false;  // Set by resume_script(), reset each tick
    bool script_error_this_tick_ = false;    // Set when script errors, reset each tick

    // Deferred scripts — scheduled by Sem_Sdefer while a script is running.
    // Executed in FIFO order after the current active script finishes (or errors).
    // Source-proven from pokecrystal Script_sdefer: deferred script runs after
    // the current script returns, not concurrently.
    std::vector<std::string> deferred_scripts_;
    
    // Callbacks
    InteractionCallback on_interaction_;
    MovementCallback on_movement_complete_;
    
    // Game state (not owned - receives pointer from caller)
    // Used for RNG and other gameplay state
    // REQUIRED for gameplay simulation - no fallback RNG
    GameState* game_state_ = nullptr;
    
    //=========================================================================
    // INTERNAL HELPERS
    //=========================================================================
    
    // RNG helper — draws from game_state_->rng (canonical authoritative stream).
    // NPC movement is authoritative gameplay state (positions saved to GameState).
    // REQUIRES: game_state_ to be non-null before any NPC movement tick.
    uint32_t next_random();
    
    // Handle movement input (updates facing, checks collision, starts movement)
    InputResult handle_movement(Direction dir);
    
    // Handle A-button interaction
    InputResult handle_interact();
    
    // Check if player can move in direction
    CollisionResult check_player_collision(Direction dir);
    
    // Check if NPC can move in direction (collision + radius bounds)
    bool check_npc_can_move(const NpcState& npc, Direction dir);
    
    // Build collision entities from NPCs
    std::vector<CollisionEntity> build_collision_entities() const;
    
    // Build interactable objects from NPCs
    std::vector<InteractableObject> build_interactable_objects() const;
    
    // Build interactable bg events from map
    std::vector<InteractableBgEvent> build_bg_events() const;
    
    // Start player movement (called after collision check passes)
    void start_player_movement(Direction dir);
    
    // Complete player movement (called when movement finishes)
    void complete_player_movement();
    
    // Update movement state
    bool update_movement();
    
    // Update script state
    bool update_script();
    
    // Update NPC autonomous movement (called each tick)
    void update_npcs();
    
    // Update a single NPC's behavior
    void update_npc_behavior(NpcState& npc);
    
    // Choose random direction for NPC based on behavior
    std::optional<Direction> choose_npc_direction(const NpcState& npc);
    
    // Start NPC movement toward target
    void start_npc_movement(NpcState& npc, Direction dir);
    
    // Complete NPC movement
    void complete_npc_movement(NpcState& npc);
};

//=============================================================================
// TIMING CONSTANTS
// From pokecrystal OBJECT_STEP_DURATION
//=============================================================================

namespace GameTiming {
    constexpr int FRAMES_PER_STEP = 16;     // Normal walking
    constexpr int FRAMES_PER_TURN = 0;      // Turns are instant
    constexpr int FRAMES_PER_LEDGE = 32;    // Ledge hop
    constexpr int FRAMES_PER_SLOW_STEP = 32;// Slow walk
}

} // namespace enginemon
