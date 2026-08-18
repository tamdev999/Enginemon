#pragma once
// engine/scripting/lua_runtime.hpp
// Lua orchestration layer
// Generated Crystal scripts and user mod scripts use the same system
// Lua yields for multi-frame operations; C++ handles core mechanics

#include "engine/core/types.hpp"
#include "engine/scripting/semantic_ir.hpp"  // For ScriptExecutionContext
#include "engine/world/movement_manager.hpp"  // For MovementManager (stub services)
#include <lua.hpp>
#include <string>
#include <filesystem>
#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>
#include <optional>
#include <any>

namespace enginemon {

// Forward declarations
class GameContext;
class World;
class BattleContext;
class Party;
class Inventory;
class UISystem;
class AudioSystem;
struct RuntimeTextSequence;  // Forward declaration for PresentationHooks

// =============================================================================
// PRESENTATION HOOKS - Per-Runtime UI/Rendering Integration
//
// These callbacks connect script execution to the visible textbox renderer.
// They are owned by each LuaRuntime instance, NOT process-global state.
//
// This enables proper multi-instance isolation:
// - Runtime A → presentation hooks A → renderer A
// - Runtime B → presentation hooks B → renderer B
//
// Without per-instance hooks, a process-global callback would cause:
// - Runtime A text operation → calls Runtime B's renderer (WRONG)
// =============================================================================
struct PresentationHooks {
    using VoidCallback = std::function<void()>;
    using TextCallback = std::function<void(const std::string&)>;
    using TextSequenceCallback = std::function<void(const RuntimeTextSequence&)>;
    
    VoidCallback open_text;
    VoidCallback close_text;
    TextCallback text;
    TextSequenceCallback text_sequence;
    
    // Clear all hooks (for cleanup or reassignment)
    void clear() {
        open_text = nullptr;
        close_text = nullptr;
        text = nullptr;
        text_sequence = nullptr;
    }
};

// =============================================================================
// STUB SERVICES - Per-Runtime Test/Stub State
//
// These structures hold stub implementation state for scripting API tests.
// They are owned by each LuaRuntime instance, NOT process-global state.
//
// When production World/GameState/Party subsystems are integrated, these stubs
// will be replaced with real implementations. Until then, they provide
// per-instance isolated state for testing.
//
// OWNERSHIP: Destroying LuaRuntime destroys its StubServices automatically.
// No process-global maps, no cleanup registries, no stale pointers.
// =============================================================================

// Actor state for world API stub
struct StubActorState {
    int x = 0;
    int y = 0;
    std::string facing = "down";
    bool visible = true;
};

// Field API test configuration
enum class StubStrengthResult : uint8_t {
    Available = 0,
    Unavailable = 1,
    AlreadyActive = 2
};

enum class StubRockSmashResult : uint8_t {
    Available = 0,
    Unavailable = 1
};

struct StubFieldConfig {
    StubStrengthResult strength_result = StubStrengthResult::Unavailable;
    uint8_t strength_slot = 0;
    SpeciesId strength_species = 0;
    
    StubRockSmashResult rock_smash_result = StubRockSmashResult::Unavailable;
    uint8_t rock_smash_slot = 0;
    SpeciesId rock_smash_species = 0;
    
    bool has_encounter = false;
    SpeciesId encounter_species = 0;
    uint8_t encounter_level = 0;
};

// Complete stub services owned by each runtime instance
struct StubServices {
    // World API stub state
    std::unordered_map<int, StubActorState> actors;
    StubActorState player = {5, 5, "down", true};
    std::vector<std::pair<std::string, std::string>> movement_calls;
    std::unique_ptr<MovementManager> movement_manager;
    bool async_movement_enabled = false;
    
    // Flag API stub state
    std::unordered_map<int, bool> flags;
    std::unordered_map<int, int> vars;
    std::vector<std::pair<std::string, int>> flag_calls;
    
    // Field API stub configuration
    StubFieldConfig field_config;
    
    // Reset all stub state
    void reset() {
        actors.clear();
        player = {5, 5, "down", true};
        movement_calls.clear();
        if (movement_manager) {
            movement_manager->cancel_all();
            movement_manager->clear_pending_completions();
        }
        flags.clear();
        vars.clear();
        flag_calls.clear();
        field_config = StubFieldConfig{};
    }
    
    // Get or create movement manager
    MovementManager& get_movement_manager() {
        if (!movement_manager) {
            movement_manager = std::make_unique<MovementManager>();
        }
        return *movement_manager;
    }
};

// Script execution state
enum class ScriptState {
    Ready,      // Not started or completed, can be started
    Running,    // Currently executing (within a resume)
    Yielded,    // Waiting for engine callback (movement, dialog, etc.)
    Finished,   // Completed normally
    Error       // Failed with error
};

// Yield reasons - what the script is waiting for
enum class YieldReason {
    None,
    WaitFrames,         // Wait N frames
    WaitSeconds,        // Wait N seconds
    Dialog,             // Waiting for dialog to complete
    Choice,             // Waiting for player choice
    Movement,           // Waiting for actor movement
    Fade,               // Waiting for screen fade
    Battle,             // Waiting for battle to complete
    Menu,               // Waiting for menu to close
    Warp,               // Waiting for warp transition
    Animation,          // Waiting for animation
    Custom              // Mod-defined yield
};

// Running script coroutine
struct ScriptCoroutine {
    lua_State* thread = nullptr;    // Lua coroutine
    ScriptState state = ScriptState::Ready;
    YieldReason yield_reason = YieldReason::None;
    int registry_ref = LUA_NOREF;   // luaL_ref handle for GC protection
    
    // Yield-specific data - INTEGER TICKS for deterministic timing
    // WaitFrames: remaining ticks (1 tick = 1 frame)
    // WaitSeconds: converted to integer ticks using ceil(seconds * SIM_TICKS_PER_SECOND)
    int wait_ticks = 0;             // Unified integer tick counter for WaitFrames/WaitSeconds
    std::any yield_data;            // Custom yield context
    
    // Source info for debugging
    ScriptId script_id = 0;
    std::string source_file;
    int source_line = 0;
};

// Simulation timing constants
// All timing uses integer ticks for deterministic behavior
// No wall clock, no renderer frame rate, no floating-point subtraction
constexpr int SIM_TICKS_PER_SECOND = 60;  // 60 Hz simulation rate

// Manages the Lua environment and script execution
class LuaRuntime {
public:
    LuaRuntime();
    ~LuaRuntime();
    
    // Non-copyable
    LuaRuntime(const LuaRuntime&) = delete;
    LuaRuntime& operator=(const LuaRuntime&) = delete;
    
    // Initialize with game context (binds C++ APIs)
    void initialize(GameContext& ctx);
    
    // Load scripts
    void load_script_file(const std::filesystem::path& path);
    void load_script_directory(const std::filesystem::path& dir);
    void execute_string(const std::string& code, const std::string& name = "string");
    
    // Start a script (creates coroutine, begins execution)
    // Returns coroutine ID
    uint32_t start_script(const std::string& function_name);
    uint32_t start_script(ScriptId id);
    uint32_t start_map_script(MapId map, const std::string& event_name);
    
    // Resume a yielded script
    // Call when the yield condition is satisfied
    void resume(uint32_t coroutine_id);
    void resume_with_result(uint32_t coroutine_id, int result);
    void resume_with_error(uint32_t coroutine_id, const std::string& error);
    
    // Update all running scripts (call each frame)
    // Handles WaitFrames, WaitSeconds automatically
    // Returns set of coroutine IDs that were resumed during this update
    std::vector<uint32_t> update(float delta_time);
    
    // Get IDs of coroutines resumed during the last update() call
    // This allows external code to check if a specific coroutine was resumed
    const std::vector<uint32_t>& get_resumed_ids() const { return resumed_ids_this_update_; }
    
    // Query script state
    ScriptState get_state(uint32_t coroutine_id) const;
    YieldReason get_yield_reason(uint32_t coroutine_id) const;
    bool has_active_scripts() const;
    
    // Cancel a running script
    void cancel(uint32_t coroutine_id);
    void cancel_all();
    
    // Get raw Lua state (for advanced bindings)
    lua_State* get_state() { return L_; }
    
    // Get script execution context (owned by this runtime instance)
    ScriptExecutionContext& get_script_context() { return script_context_; }
    const ScriptExecutionContext& get_script_context() const { return script_context_; }
    
    // Get presentation hooks (owned by this runtime instance)
    // NOT process-global - each runtime has its own hooks
    PresentationHooks& get_presentation_hooks() { return presentation_hooks_; }
    const PresentationHooks& get_presentation_hooks() const { return presentation_hooks_; }
    
    // Get stub services (owned by this runtime instance)
    // NOT process-global - each runtime has its own stub state
    // Used by scripting API stub implementations for testing
    StubServices& get_stub_services() { return stub_services_; }
    const StubServices& get_stub_services() const { return stub_services_; }
    
    // Error handler
    using ErrorHandler = std::function<void(const std::string& error, const std::string& traceback)>;
    void set_error_handler(ErrorHandler handler);

private:
    lua_State* L_ = nullptr;
    GameContext* ctx_ = nullptr;
    
    // Script execution context - owned by THIS runtime instance
    // NOT global/shared state - each LuaRuntime has its own context
    ScriptExecutionContext script_context_;
    
    // Presentation hooks - owned by THIS runtime instance
    // NOT global/shared state - each LuaRuntime has its own hooks
    PresentationHooks presentation_hooks_;
    
    // Stub services - owned by THIS runtime instance
    // NOT global/shared state - each LuaRuntime has its own stub state
    // Used by scripting API stub implementations for testing
    StubServices stub_services_;
    
    uint32_t next_coroutine_id_ = 1;
    std::unordered_map<uint32_t, ScriptCoroutine> coroutines_;
    std::unordered_map<uint32_t, ScriptState> completed_states_;  // Final states for cleaned-up coroutines
    
    // Tracks which coroutine IDs were resumed during the last update() call
    // Enables identity-preserving resume reporting instead of a global bool
    std::vector<uint32_t> resumed_ids_this_update_;
    
    ErrorHandler error_handler_;
    
    void bind_api();
    void bind_world_api();
    void bind_battle_api();
    void bind_party_api();
    void bind_inventory_api();
    void bind_ui_api();
    void bind_audio_api();
    void bind_flag_api();
    void bind_time_api();
    void bind_util_api();
    void bind_field_api();
    
    void resume_first(uint32_t coroutine_id);  // First resume with ctx argument
    void cleanup_coroutine(uint32_t coroutine_id);  // Release registry ref
    
    static int lua_error_handler(lua_State* L);
};

// Script yield helpers (called from C++ API bindings)
// These cause the current Lua coroutine to yield

// Wait for N frames
void script_wait_frames(lua_State* L, int frames);

// Wait for N seconds
void script_wait_seconds(lua_State* L, float seconds);

// Wait for dialog completion
void script_wait_dialog(lua_State* L);

// Wait for player choice (returns chosen index when resumed)
void script_wait_choice(lua_State* L, const std::vector<std::string>& options);

// Wait for actor movement
void script_wait_movement(lua_State* L);

// Wait for screen fade
void script_wait_fade(lua_State* L);

// Wait for battle (returns battle result when resumed)
void script_wait_battle(lua_State* L);

} // namespace enginemon
