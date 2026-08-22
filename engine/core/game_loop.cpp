// engine/core/game_loop.cpp
// Headless game loop implementation
//
// Reference: Gen2Recomped OverworldController.lua
// - handleInput(): input gated on wWalkCounter == 0, A-button calls interact()
// - update(): script running blocks input, movement ticks each frame
// - interact(): NPC → counter → sign → hidden → field move → bookshelf

#include "engine/core/game_loop.hpp"
#include <functional>
#include <numeric>
#include <stdexcept>

namespace enginemon {

HeadlessGameLoop::HeadlessGameLoop() {
    // Set up movement completion callback to update state
    movement_manager_.set_completion_callback(
        [this](uint32_t actor_id, uint32_t coroutine_id) {
            if (actor_id == 0) {
                // Player movement completed
                complete_player_movement();
            }
        }
    );
}

HeadlessGameLoop::~HeadlessGameLoop() = default;

//=============================================================================
// INITIALIZATION
//=============================================================================

void HeadlessGameLoop::load_map(const RuntimeMap& map) {
    // Copy the map to owned storage to prevent dangling pointers
    // Caller's map may be temporary or have shorter lifetime than the game loop
    current_map_owned_ = map;
    current_map_ = &current_map_owned_.value();
    // Clear NPCs when loading new map
    npcs_.clear();
}

void HeadlessGameLoop::set_collision_data(std::function<CollisionClass(int32_t, int32_t)> get_collision) {
    get_collision_ = std::move(get_collision);
}

void HeadlessGameLoop::spawn_player(int32_t x, int32_t y, Direction facing) {
    player_.x = x;
    player_.y = y;
    player_.facing = facing;
    player_.is_moving = false;
    player_.target_x = x;
    player_.target_y = y;
    player_.frames_remaining = 0;
    state_ = LoopState::Idle;
    
    // F3: Keep GameState::player in sync as the authoritative persistent state.
    // player_ is the transient simulation scratchpad (is_moving/target/frames).
    // game_state_->player.x/y/facing are the canonical position — written here
    // so save, warp-memory, and connection paths always read current values.
    if (game_state_) {
        game_state_->player.x = x;
        game_state_->player.y = y;
        game_state_->player.facing = facing;
    }
}

void HeadlessGameLoop::add_npc(const NpcState& npc) {
    npcs_.push_back(npc);
}

void HeadlessGameLoop::clear_npcs() {
    npcs_.clear();
}

//=============================================================================
// INPUT PROCESSING
//=============================================================================

InputResult HeadlessGameLoop::process_input(InputAction action) {
    InputResult result;
    
    // Reference: Gen2Recomped handleInput() gates on wWalkCounter == 0
    // and !self.runner:isRunning()
    if (is_input_locked()) {
        result.accepted = false;
        result.block_reason = "input_locked";
        return result;
    }
    
    // Handle based on action type
    auto dir_opt = input_to_direction(action);
    if (dir_opt.has_value()) {
        return handle_movement(*dir_opt);
    }
    
    if (action == InputAction::Interact) {
        return handle_interact();
    }
    
    result.accepted = false;
    return result;
}

bool HeadlessGameLoop::is_input_locked() const {
    // Input locked when:
    // 1. Player is mid-step (moving)
    // 2. Script is active (running OR yielded)
    //    A yielded coroutine is still the active event script - player cannot
    //    move/interact while WaitFrames/WaitSeconds/dialog is pending
    return state_ == LoopState::Moving || 
           state_ == LoopState::ScriptRunning ||
           state_ == LoopState::ScriptYielded;
}

InputResult HeadlessGameLoop::handle_movement(Direction dir) {
    InputResult result;
    result.accepted = true;
    
    // Always update facing (even if blocked)
    // Reference: Gen2Recomped player:tryMove always sets facing
    player_.facing = dir;
    if (game_state_) {
        game_state_->player.facing = dir;
    }
    
    // Check collision
    CollisionResult collision = check_player_collision(dir);
    
    if (!collision.allowed) {
        result.blocked = true;
        switch (collision.reason) {
            case MoveBlockReason::Bounds:
                result.block_reason = "bounds";
                break;
            case MoveBlockReason::Tile:
                result.block_reason = "tile";
                break;
            case MoveBlockReason::Entity:
                result.block_reason = "entity";
                break;
            case MoveBlockReason::SideWall:
                result.block_reason = "side_wall";
                break;
            case MoveBlockReason::Ledge:
                result.block_reason = "ledge";
                break;
            default:
                result.block_reason = "unknown";
                break;
        }
        return result;
    }
    
    // Start movement
    start_player_movement(dir);
    return result;
}

InputResult HeadlessGameLoop::handle_interact() {
    InputResult result;
    result.accepted = true;
    result.interaction = true;
    
    if (!current_map_) {
        result.interaction = false;
        result.block_reason = "no_map";
        return result;
    }
    
    // Build interaction data
    auto objects = build_interactable_objects();
    auto bg_events = build_bg_events();
    
    InteractionMap imap;
    // CRITICAL: Use collision_width/height (blocks*2), NOT tile_width/height (blocks*4)
    // Player coordinates are in collision cells (16×16 pixel grid), not render tiles (8×8)
    imap.width = current_map_->collision_width();
    imap.height = current_map_->collision_height();
    imap.get_collision = get_collision_ ? get_collision_ : 
        [](int32_t, int32_t) -> CollisionClass { return CollisionClass::Floor; };
    
    // Check interaction
    // Pass flag checker if GameState is available (for IFSET/IFNOTSET/hidden item evaluation)
    Interaction::FlagChecker flag_checker = nullptr;
    if (game_state_) {
        flag_checker = [this](const std::string& flag_id) {
            return game_state_->check_flag(flag_id);
        };
    }
    
    InteractionResult interaction_result = interaction_.check(
        imap, objects, bg_events,
        player_.x, player_.y, player_.facing,
        flag_checker
    );
    
    if (on_interaction_) {
        on_interaction_(interaction_result);
    }
    
    if (interaction_result.found()) {
        result.script_id = interaction_result.script_id();
        
        // Try to start the script
        if (!result.script_id.empty() && lua_runtime_) {
            start_script(result.script_id);
        }
    } else {
        result.interaction = false;
    }
    
    return result;
}

CollisionResult HeadlessGameLoop::check_player_collision(Direction dir) {
    if (!current_map_ || !get_collision_) {
        return CollisionResult::blocked(MoveBlockReason::Bounds);
    }
    
    // Build collision map using semantic CollisionClass
    // CRITICAL: Use collision_width/height (blocks*2), NOT tile_width/height (blocks*4)
    // Player coordinates are in collision cells (16×16 pixel grid), not render tiles (8×8)
    CollisionMap cmap;
    cmap.width = current_map_->collision_width();
    cmap.height = current_map_->collision_height();
    cmap.get_collision = get_collision_;
    
    // Build entity list
    auto entities = build_collision_entities();
    
    // Player collision entity
    CollisionEntity player_entity;
    player_entity.id = 0;
    player_entity.x = player_.x;
    player_entity.y = player_.y;
    player_entity.target_x = player_.x;
    player_entity.target_y = player_.y;
    player_entity.is_moving = player_.is_moving;
    player_entity.is_passable = false;
    
    return collision_.can_move(cmap, entities, player_entity, dir);
}

std::vector<CollisionEntity> HeadlessGameLoop::build_collision_entities() const {
    std::vector<CollisionEntity> entities;
    
    for (const auto& npc : npcs_) {
        if (!npc.visible) continue;
        
        CollisionEntity entity;
        entity.id = npc.id;
        entity.x = npc.x;
        entity.y = npc.y;
        // FIX: Use actual target position from NPC state when moving
        // Reference: Gen2Recomped Collision.occupied() checks both cellX/Y AND targetX/Y
        // During movement, both source tile (x,y) and destination tile (target_x,target_y)
        // must be reserved to prevent other entities from moving into the destination
        entity.target_x = npc.target_x;
        entity.target_y = npc.target_y;
        entity.is_moving = npc.is_moving;
        entity.is_passable = false;
        entities.push_back(entity);
    }
    
    return entities;
}

std::vector<InteractableObject> HeadlessGameLoop::build_interactable_objects() const {
    std::vector<InteractableObject> objects;
    
    for (const auto& npc : npcs_) {
        if (!npc.visible) continue;
        
        InteractableObject obj;
        obj.local_id = npc.id;
        obj.x = npc.x;
        obj.y = npc.y;
        obj.is_moving = npc.is_moving;
        obj.is_trainer = npc.is_trainer;
        obj.script_id = npc.script_id;
        obj.visibility_flag = npc.visibility_flag;
        objects.push_back(obj);
    }
    
    return objects;
}

std::vector<InteractableBgEvent> HeadlessGameLoop::build_bg_events() const {
    std::vector<InteractableBgEvent> events;
    
    if (!current_map_) return events;
    
    for (const auto& bg : current_map_->bg_events) {
        InteractableBgEvent event;
        event.x = bg.x;
        event.y = bg.y;
        event.type = static_cast<uint8_t>(bg.type);
        event.script_id = bg.script_id;
        event.item_id = bg.item_id;
        event.quantity = bg.quantity;
        event.condition_flag = bg.condition_flag;  // Propagate condition flag
        events.push_back(event);
    }
    
    return events;
}

//=============================================================================
// MOVEMENT
//=============================================================================

void HeadlessGameLoop::start_player_movement(Direction dir) {
    // Calculate target position
    int dx = 0, dy = 0;
    direction_to_delta(dir, dx, dy);
    
    int32_t target_x = player_.x + dx;
    int32_t target_y = player_.y + dy;
    
    start_player_movement_to(target_x, target_y, dir);
}

void HeadlessGameLoop::start_player_movement_to(int32_t target_x, int32_t target_y, Direction dir) {
    player_.target_x = target_x;
    player_.target_y = target_y;
    player_.facing = dir;
    player_.is_moving = true;
    player_.frames_remaining = GameTiming::FRAMES_PER_STEP;
    
    state_ = LoopState::Moving;
    
    // Enqueue in movement manager for proper timing
    std::vector<MovementCmd> cmds;
    MovementCmd cmd;
    cmd.type = MovementCommandType::Step;
    cmd.direction = static_cast<MovementDirection>(static_cast<int>(dir));
    cmds.push_back(cmd);
    
    movement_manager_.enqueue_movement(
        0,  // player actor ID
        0,  // coroutine ID (not script-driven)
        cmds,
        player_.x, player_.y,
        static_cast<MovementDirection>(static_cast<int>(player_.facing))
    );
}

void HeadlessGameLoop::complete_player_movement() {
    // Update position to target
    player_.x = player_.target_x;
    player_.y = player_.target_y;
    player_.is_moving = false;
    player_.frames_remaining = 0;
    
    // F3: Commit confirmed position to canonical GameState immediately.
    // This is the definitive step-completion write — any prior read from
    // game_state_->player.x/y will now reflect the latest confirmed cell.
    if (game_state_) {
        game_state_->player.x = player_.x;
        game_state_->player.y = player_.y;
        // facing was already synced in handle_movement/start_player_movement_to
    }
    
    // Return to idle if no script running
    if (state_ == LoopState::Moving) {
        state_ = LoopState::Idle;
    }
    
    if (on_movement_complete_) {
        on_movement_complete_(player_.x, player_.y, player_.facing);
    }
}

bool HeadlessGameLoop::update_movement() {
    // Tick movement manager
    // NOTE: update() returns coroutine IDs, not actor IDs.
    // For non-script player movement, we use coroutine_id=0.
    // The callback handles the actual actor_id dispatch.
    auto completed_coroutine_ids = movement_manager_.update();
    
    // For non-script player movement (coroutine_id = 0), check if it completed
    for (uint32_t coroutine_id : completed_coroutine_ids) {
        if (coroutine_id == 0) {
            // Non-script player movement completed
            // (The callback already called complete_player_movement)
            return true;
        }
    }
    
    // Also check via state query (movement manager callback may have fired)
    if (!movement_manager_.is_actor_moving(0) && state_ == LoopState::Moving) {
        complete_player_movement();
        return true;
    }
    
    return false;
}

//=============================================================================
// SCRIPTING
//=============================================================================

void HeadlessGameLoop::set_lua_runtime(LuaRuntime* runtime) {
    lua_runtime_ = runtime;
    if (runtime) {
        // Wire deferred-script scheduling so ctx.game:behavior("Sdefer_<id>")
        // actually enqueues the target script on this game loop instance.
        runtime->get_stub_services().deferred_script_fn =
            [this](const std::string& script_id) {
                schedule_deferred_script(script_id);
            };
    }
}

bool HeadlessGameLoop::start_script(const std::string& script_id) {
    if (!lua_runtime_) return false;
    
    // Load script if loader is set
    if (script_loader_) {
        std::string code = script_loader_(script_id);
        if (code.empty()) {
            return false;
        }
        
        // Script loader is expected to return code that creates global "script" table
        try {
            lua_runtime_->execute_string(code, script_id);
        } catch (const std::exception&) {
            // Lua syntax error during load
            return false;
        }
    }
    
    // Start the script
    active_coroutine_ = lua_runtime_->start_script("script");
    active_script_id_ = script_id;
    
    // Check state
    ScriptState script_state = lua_runtime_->get_state(active_coroutine_);
    if (script_state == ScriptState::Yielded) {
        state_ = LoopState::ScriptYielded;
        return true;
    } else if (script_state == ScriptState::Running) {
        state_ = LoopState::ScriptRunning;
        return true;
    } else if (script_state == ScriptState::Finished) {
        // Finished immediately - successful completion
        active_coroutine_ = 0;
        active_script_id_.clear();
        state_ = LoopState::Idle;
        return true;
    } else {
        // Error state - script failed during start
        active_coroutine_ = 0;
        active_script_id_.clear();
        state_ = LoopState::Idle;
        return false;  // Return false on immediate error
    }
}

bool HeadlessGameLoop::resume_script() {
    if (!lua_runtime_ || active_coroutine_ == 0) return false;
    
    ScriptState script_state = lua_runtime_->get_state(active_coroutine_);
    if (script_state != ScriptState::Yielded) return false;
    
    lua_runtime_->resume(active_coroutine_);
    
    // Mark that a yielded script was actually resumed this tick
    script_resumed_this_tick_ = true;
    
    // Check new state
    script_state = lua_runtime_->get_state(active_coroutine_);
    if (script_state == ScriptState::Finished) {
        active_coroutine_ = 0;
        active_script_id_.clear();
        state_ = LoopState::Idle;
        // Normal completion - script_error_this_tick_ remains false
    } else if (script_state == ScriptState::Error) {
        script_error_this_tick_ = true;  // Track the error
        active_coroutine_ = 0;
        active_script_id_.clear();
        state_ = LoopState::Idle;
    } else if (script_state == ScriptState::Yielded) {
        state_ = LoopState::ScriptYielded;
    }
    
    return true;
}

bool HeadlessGameLoop::update_script() {
    if (!lua_runtime_ || active_coroutine_ == 0) return false;
    
    ScriptState script_state = lua_runtime_->get_state(active_coroutine_);
    
    // Handle different yield reasons
    if (script_state == ScriptState::Yielded) {
        YieldReason reason = lua_runtime_->get_yield_reason(active_coroutine_);
        
        // Define simulation delta: 1/60 second per tick (60 FPS fixed rate)
        // Note: delta_time is passed to update() but integer tick timing is used internally
        constexpr float SIMULATION_DELTA = 1.0f / 60.0f;
        
        switch (reason) {
            case YieldReason::WaitFrames:
            case YieldReason::WaitSeconds:
                // Let lua_runtime update handle the tick countdown
                // update() returns IDs of coroutines that were resumed
                lua_runtime_->update(SIMULATION_DELTA);
                
                // Check if OUR active coroutine was among the resumed IDs
                // This prevents false positives when unrelated coroutines resume
                {
                    const auto& resumed_ids = lua_runtime_->get_resumed_ids();
                    for (uint32_t id : resumed_ids) {
                        if (id == active_coroutine_) {
                            script_resumed_this_tick_ = true;
                            break;
                        }
                    }
                }
                break;
                
            case YieldReason::Dialog:
                // In headless mode, auto-advance dialog
                resume_script();
                break;
                
            case YieldReason::Movement:
                // Check if movement is complete
                if (!movement_manager_.is_actor_moving(0)) {
                    resume_script();
                }
                break;
                
            default:
                // Unknown yield, try to resume
                resume_script();
                break;
        }
    }
    
    // Check terminal state
    // If resume_script() or lua_runtime_->update() already finalized, active_coroutine_ is 0
    if (active_coroutine_ == 0) {
        // Script terminated during this update
        // script_error_this_tick_ was set by resume_script() if it was an error
        // Return true for normal completion, false for error
        return !script_error_this_tick_;
    }
    
    // Re-check state in case lua_runtime_->update() changed it
    script_state = lua_runtime_->get_state(active_coroutine_);
    
    if (script_state == ScriptState::Finished) {
        active_coroutine_ = 0;
        active_script_id_.clear();
        state_ = LoopState::Idle;
        return true;  // Normal completion
    }
    
    if (script_state == ScriptState::Error) {
        script_error_this_tick_ = true;
        active_coroutine_ = 0;
        active_script_id_.clear();
        state_ = LoopState::Idle;
        return false;  // Error - not a normal completion
    }
    
    return false;  // Still running or yielded
}

//=============================================================================
// TICK
//=============================================================================

TickResult HeadlessGameLoop::tick() {
    TickResult result;
    
    // Reset per-tick tracking
    script_resumed_this_tick_ = false;
    script_error_this_tick_ = false;
    
    // Update NPC autonomous movement
    // Reference: Gen2Recomped calls NPC:update() each frame
    // This must happen regardless of player/script state
    update_npcs();
    
    // Update movement
    if (state_ == LoopState::Moving) {
        result.movement_complete = update_movement();
    }
    
    // Update script
    if (state_ == LoopState::ScriptRunning || state_ == LoopState::ScriptYielded) {
        result.script_complete = update_script();
        
        // script_error = true if script failed (separate from completion)
        result.script_error = script_error_this_tick_;
        
        // script_resumed = true when resume_script() was actually called and succeeded
        // This is set by resume_script() itself, independent of resulting state
        result.script_resumed = script_resumed_this_tick_;
    }

    // Drain deferred-script queue when a slot opens (state is now Idle after
    // script completion above, or was already Idle before any script ran).
    // Source: pokecrystal Script_sdefer — deferred body runs after current script.
    if (state_ == LoopState::Idle && !deferred_scripts_.empty()) {
        std::string next_id = std::move(deferred_scripts_.front());
        deferred_scripts_.erase(deferred_scripts_.begin());
        start_script(next_id);  // errors here surface through start_script's return value
    }
    
    return result;
}

TickResult HeadlessGameLoop::tick(int count) {
    TickResult cumulative;
    
    for (int i = 0; i < count; i++) {
        TickResult r = tick();
        cumulative.movement_complete |= r.movement_complete;
        cumulative.script_complete |= r.script_complete;
        cumulative.script_error |= r.script_error;
        cumulative.script_resumed |= r.script_resumed;
        for (auto id : r.completed_coroutines) {
            cumulative.completed_coroutines.push_back(id);
        }
    }
    
    return cumulative;
}

//=============================================================================
// STATE
//=============================================================================

uint64_t HeadlessGameLoop::state_hash() const {
    // Simple hash for determinism checking
    uint64_t hash = 0;
    hash ^= static_cast<uint64_t>(player_.x) << 0;
    hash ^= static_cast<uint64_t>(player_.y) << 8;
    hash ^= static_cast<uint64_t>(player_.facing) << 16;
    hash ^= static_cast<uint64_t>(player_.is_moving) << 20;
    hash ^= static_cast<uint64_t>(state_) << 24;
    return hash;
}

void HeadlessGameLoop::reset() {
    // Cancel any active coroutine through LuaRuntime before clearing loop state
    // This ensures:
    // 1. Registry reference is released (coroutine won't prevent GC)
    // 2. Coroutine is removed from LuaRuntime::coroutines_
    // 3. No timed resume (WaitFrames/WaitSeconds) can occur later
    if (lua_runtime_ && active_coroutine_ != 0) {
        lua_runtime_->cancel(active_coroutine_);
    }
    
    state_ = LoopState::Idle;
    player_ = PlayerState{};
    current_map_owned_.reset();
    current_map_ = nullptr;
    get_collision_ = nullptr;
    npcs_.clear();
    movement_manager_.cancel_all();
    active_coroutine_ = 0;
    active_script_id_.clear();
    script_resumed_this_tick_ = false;
    script_error_this_tick_ = false;
    deferred_scripts_.clear();
    // Note: game_state_ pointer is NOT reset - caller owns that
    // Note: lua_runtime_ pointer is NOT reset - caller owns that and may reuse it
}

void HeadlessGameLoop::schedule_deferred_script(const std::string& script_id) {
    // Enqueue a script to run after the current active script finishes.
    // Source: pokecrystal Script_sdefer (opcode 0x86):
    //   writes target script address to wScriptQueue, which the engine
    //   processes after Script_end unwinds the current script.
    // We model this as a FIFO queue drained in tick() when state == Idle.
    deferred_scripts_.push_back(script_id);
}

//=============================================================================
// NPC STATE SNAPSHOT/RESTORE
// For deterministic save/load
//=============================================================================

void HeadlessGameLoop::snapshot_npc_states(const std::string& map_id) {
    if (!game_state_) return;
    
    std::vector<NpcSaveState> states;
    states.reserve(npcs_.size());
    
    for (const auto& npc : npcs_) {
        NpcSaveState save;
        save.id = npc.id;
        save.x = npc.x;
        save.y = npc.y;
        save.facing = npc.facing;
        save.is_moving = npc.is_moving;
        save.idle_timer = npc.idle_timer;
        save.target_x = npc.target_x;
        save.target_y = npc.target_y;
        save.move_progress = npc.move_progress;
        save.frozen = npc.frozen;
        save.visible = npc.visible;
        states.push_back(save);
    }
    
    game_state_->npc_states[map_id] = std::move(states);
}

void HeadlessGameLoop::restore_npc_states(const std::string& map_id) {
    if (!game_state_) return;
    
    auto it = game_state_->npc_states.find(map_id);
    if (it == game_state_->npc_states.end()) return;
    
    const auto& saved_states = it->second;
    
    for (auto& npc : npcs_) {
        // Find matching saved state by ID
        for (const auto& save : saved_states) {
            if (save.id == npc.id) {
                npc.x = save.x;
                npc.y = save.y;
                npc.facing = save.facing;
                npc.is_moving = save.is_moving;
                npc.idle_timer = save.idle_timer;
                npc.target_x = save.target_x;
                npc.target_y = save.target_y;
                npc.move_progress = save.move_progress;
                npc.frozen = save.frozen;
                npc.visible = save.visible;
                break;
            }
        }
    }
}

//=============================================================================
// NPC AUTONOMOUS MOVEMENT
// Reference: pokecrystal/engine/overworld/map_objects.asm
// Reference: Gen2Recomped/src/world/NPC.lua
//=============================================================================

// Get next random value — uses the MAP-LOCAL RNG for NPC movement.
// This is separate from the canonical gameplay RNG (game_state_->rng).
// Map-local RNG is seeded per map and does not affect save state.
uint32_t HeadlessGameLoop::next_random() {
    return map_rng_.next();
}

void HeadlessGameLoop::set_rng_seed(uint32_t seed) {
    // Seeds the MAP-LOCAL RNG for NPC movement and map-scoped randomness.
    // Does NOT touch the canonical gameplay RNG (game_state_->rng).
    // The canonical gameplay RNG is a continuous save-persisted stream.
    map_rng_.set_seed(seed);
}

void HeadlessGameLoop::freeze_npc(uint16_t npc_id) {
    for (auto& npc : npcs_) {
        if (npc.id == npc_id) {
            npc.frozen = true;
            return;
        }
    }
}

void HeadlessGameLoop::unfreeze_npc(uint16_t npc_id) {
    for (auto& npc : npcs_) {
        if (npc.id == npc_id) {
            npc.frozen = false;
            return;
        }
    }
}

bool HeadlessGameLoop::is_npc_frozen(uint16_t npc_id) const {
    for (const auto& npc : npcs_) {
        if (npc.id == npc_id) {
            return npc.frozen;
        }
    }
    return false;
}

NpcState* HeadlessGameLoop::get_npc(uint16_t npc_id) {
    for (auto& npc : npcs_) {
        if (npc.id == npc_id) {
            return &npc;
        }
    }
    return nullptr;
}

const NpcState* HeadlessGameLoop::get_npc(uint16_t npc_id) const {
    for (const auto& npc : npcs_) {
        if (npc.id == npc_id) {
            return &npc;
        }
    }
    return nullptr;
}

bool HeadlessGameLoop::check_npc_can_move(const NpcState& npc, Direction dir) {
    if (!current_map_ || !get_collision_) {
        return false;
    }
    
    // Calculate target position
    int dx = 0, dy = 0;
    direction_to_delta(dir, dx, dy);
    int32_t target_x = npc.x + dx;
    int32_t target_y = npc.y + dy;
    
    // Check radius bounds
    // Reference: pokecrystal OBJECT_RADIUS check in movement functions
    if (npc.radius_x > 0 || npc.radius_y > 0) {
        int32_t dist_x = std::abs(target_x - npc.init_x);
        int32_t dist_y = std::abs(target_y - npc.init_y);
        if (npc.radius_x > 0 && dist_x > npc.radius_x) return false;
        if (npc.radius_y > 0 && dist_y > npc.radius_y) return false;
    }
    
    // Check map bounds
    // CRITICAL: Use collision_width/height (blocks*2), NOT tile_width/height (blocks*4)
    // Player/NPC coordinates are in collision cells (16×16 pixel grid), not render tiles (8×8)
    if (target_x < 0 || target_y < 0 ||
        target_x >= current_map_->collision_width() ||
        target_y >= current_map_->collision_height()) {
        return false;
    }
    
    // Use the authoritative collision system - same as player movement
    // This ensures NPCs respect side walls, ledges, and all directional collision
    CollisionMap cmap;
    cmap.width = current_map_->collision_width();
    cmap.height = current_map_->collision_height();
    cmap.get_collision = get_collision_;
    
    // Build entity list for collision checking
    std::vector<CollisionEntity> entities;
    
    // Add player as collision entity
    CollisionEntity player_entity;
    player_entity.id = 0;
    player_entity.x = player_.x;
    player_entity.y = player_.y;
    player_entity.target_x = player_.target_x;
    player_entity.target_y = player_.target_y;
    player_entity.is_moving = player_.is_moving;
    player_entity.is_passable = false;
    entities.push_back(player_entity);
    
    // Add other NPCs as collision entities
    for (const auto& other : npcs_) {
        if (other.id == npc.id) continue;  // Skip self
        if (!other.visible) continue;
        
        CollisionEntity entity;
        entity.id = other.id;
        entity.x = other.x;
        entity.y = other.y;
        entity.target_x = other.target_x;
        entity.target_y = other.target_y;
        entity.is_moving = other.is_moving;
        entity.is_passable = false;
        entities.push_back(entity);
    }
    
    // Build NPC entity for collision check
    CollisionEntity npc_entity;
    npc_entity.id = npc.id;
    npc_entity.x = npc.x;
    npc_entity.y = npc.y;
    npc_entity.target_x = npc.x;
    npc_entity.target_y = npc.y;
    npc_entity.is_moving = false;
    npc_entity.is_passable = false;
    
    // Use the authoritative collision checker (same rules as player movement)
    CollisionResult result = collision_.can_move(cmap, entities, npc_entity, dir);
    if (!result.allowed) {
        return false;
    }
    
    // Don't walk onto warps (Reference: Gen2Recomped NPC.lua)
    for (const auto& warp : current_map_->warps) {
        if (target_x == warp.x && target_y == warp.y) {
            return false;
        }
    }
    
    return true;
}

std::optional<Direction> HeadlessGameLoop::choose_npc_direction(const NpcState& npc) {
    // Choose direction based on movement behavior
    // Reference: pokecrystal MovementFunction_RandomWalkY/X/XY
    
    switch (npc.behavior) {
        case NpcMovementBehavior::RandomWalkY: {
            // Up or down only
            uint32_t r = next_random();
            return (r & 1) ? Direction::Up : Direction::Down;
        }
        
        case NpcMovementBehavior::RandomWalkX: {
            // Left or right only
            uint32_t r = next_random();
            return (r & 1) ? Direction::Left : Direction::Right;
        }
        
        case NpcMovementBehavior::RandomWalkXY: {
            // Any direction
            uint32_t r = next_random();
            switch (r & 3) {
                case 0: return Direction::Down;
                case 1: return Direction::Up;
                case 2: return Direction::Left;
                case 3: return Direction::Right;
            }
            break;
        }
        
        case NpcMovementBehavior::RandomSpinSlow:
        case NpcMovementBehavior::RandomSpinFast: {
            // Any direction for spin
            uint32_t r = next_random();
            switch ((r >> 2) & 3) {  // Use different bits than walk
                case 0: return Direction::Down;
                case 1: return Direction::Up;
                case 2: return Direction::Left;
                case 3: return Direction::Right;
            }
            break;
        }
        
        default:
            break;
    }
    
    return std::nullopt;
}

void HeadlessGameLoop::start_npc_movement(NpcState& npc, Direction dir) {
    int dx = 0, dy = 0;
    direction_to_delta(dir, dx, dy);
    
    npc.target_x = npc.x + dx;
    npc.target_y = npc.y + dy;
    npc.is_moving = true;
    npc.move_progress = 0;
    npc.facing = dir;
}

void HeadlessGameLoop::complete_npc_movement(NpcState& npc) {
    npc.x = npc.target_x;
    npc.y = npc.target_y;
    npc.is_moving = false;
    npc.move_progress = 0;
    
    // Reset idle timer
    // Reference: pokecrystal RandomStepDuration_Slow = random & 0x7F (0-127)
    // Reference: Gen2Recomped: random(30, 180)
    uint32_t r = next_random();
    switch (npc.behavior) {
        case NpcMovementBehavior::RandomSpinFast:
            // Fast: 0-31 frames (pokecrystal RandomStepDuration_Fast = random & 0x1F)
            npc.idle_timer = static_cast<int32_t>(r & 0x1F);
            break;
        default:
            // Slow: 30-127 frames (compromise between pokecrystal 0-127 and Gen2Recomped 30-180)
            npc.idle_timer = 30 + static_cast<int32_t>(r % 98);
            break;
    }
}

void HeadlessGameLoop::update_npc_behavior(NpcState& npc) {
    // Skip if frozen (script interaction), invisible, or standing behavior
    if (npc.frozen || !npc.visible) {
        return;
    }
    
    if (npc.behavior == NpcMovementBehavior::Standing) {
        return;
    }
    
    // If currently moving, progress the movement
    if (npc.is_moving) {
        npc.move_progress++;
        
        // Movement complete after 16 frames (pokecrystal StepVectors normal step)
        if (npc.move_progress >= GameTiming::FRAMES_PER_STEP) {
            complete_npc_movement(npc);
        }
        return;
    }
    
    // Idle timer countdown
    if (npc.idle_timer > 0) {
        npc.idle_timer--;
        return;
    }
    
    // Time to attempt movement/turn
    auto dir_opt = choose_npc_direction(npc);
    if (!dir_opt.has_value()) {
        // Set new idle timer
        uint32_t r = next_random();
        npc.idle_timer = 30 + static_cast<int32_t>(r % 98);
        return;
    }
    
    Direction dir = *dir_opt;
    
    // For spin behaviors, just turn (no movement)
    if (npc.behavior == NpcMovementBehavior::RandomSpinSlow ||
        npc.behavior == NpcMovementBehavior::RandomSpinFast) {
        npc.facing = dir;
        // Set new idle timer
        uint32_t r = next_random();
        if (npc.behavior == NpcMovementBehavior::RandomSpinFast) {
            npc.idle_timer = static_cast<int32_t>(r & 0x1F);
        } else {
            npc.idle_timer = static_cast<int32_t>(r & 0x7F);
        }
        return;
    }
    
    // For walk behaviors, 50% chance to just turn (Reference: Gen2Recomped)
    uint32_t r = next_random();
    if ((r & 1) == 0) {
        // Just turn, don't move
        npc.facing = dir;
        npc.idle_timer = 30 + static_cast<int32_t>((next_random()) % 98);
        return;
    }
    
    // Try to move
    if (check_npc_can_move(npc, dir)) {
        start_npc_movement(npc, dir);
    } else {
        // Blocked - just update facing and wait
        npc.facing = dir;
        npc.idle_timer = 30 + static_cast<int32_t>((next_random()) % 98);
    }
}

void HeadlessGameLoop::update_npcs() {
    for (auto& npc : npcs_) {
        update_npc_behavior(npc);
    }
}

} // namespace enginemon
