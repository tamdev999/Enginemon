// engine/world/movement_manager.cpp
// Asynchronous scripted movement manager implementation
// Reference: pokecrystal OBJECT_STEP_DURATION = 16 frames
// Reference: Gen2Recomped scriptMove/updateScriptMoves

#include "engine/world/movement_manager.hpp"
#include <algorithm>

namespace enginemon {

bool MovementManager::enqueue_movement(
    uint32_t actor_id,
    uint32_t coroutine_id,
    const std::vector<MovementCmd>& commands,
    int start_x, int start_y,
    MovementDirection start_facing
) {
    // Don't allow multiple simultaneous movements for same actor
    if (active_movements_.contains(actor_id)) {
        return false;
    }
    
    if (commands.empty()) {
        // Empty movement completes immediately
        pending_completions_.push_back({actor_id, coroutine_id});
        if (completion_callback_) {
            completion_callback_(actor_id, coroutine_id);
        }
        return true;
    }
    
    ActiveMovement mv;
    mv.actor_id = actor_id;
    mv.coroutine_id = coroutine_id;
    mv.commands = commands;
    mv.current_index = 0;
    mv.frames_remaining = 0;
    mv.start_x = start_x;
    mv.start_y = start_y;
    mv.current_x = start_x;
    mv.current_y = start_y;
    mv.facing = start_facing;
    mv.started = false;
    mv.completed = false;
    
    active_movements_[actor_id] = std::move(mv);
    return true;
}

bool MovementManager::enqueue_movement_table(
    uint32_t actor_id,
    uint32_t coroutine_id,
    int down, int up, int left, int right,
    int start_x, int start_y,
    MovementDirection start_facing
) {
    auto commands = batch_to_commands(down, up, left, right);
    return enqueue_movement(actor_id, coroutine_id, commands, start_x, start_y, start_facing);
}

std::vector<uint32_t> MovementManager::update() {
    std::vector<uint32_t> completed_coroutines;
    std::vector<uint32_t> to_remove;
    
    // Phase 1: Retire completed movements
    for (auto& [actor_id, mv] : active_movements_) {
        if (mv.completed) {
            completed_coroutines.push_back(mv.coroutine_id);
            pending_completions_.push_back({actor_id, mv.coroutine_id});
            to_remove.push_back(actor_id);
            if (completion_callback_) {
                completion_callback_(actor_id, mv.coroutine_id);
            }
        }
    }
    
    for (uint32_t actor_id : to_remove) {
        active_movements_.erase(actor_id);
    }
    to_remove.clear();
    
    // Phase 2: Tick all active movements
    for (auto& [actor_id, mv] : active_movements_) {
        if (tick_movement(mv)) {
            // Movement just completed
            completed_coroutines.push_back(mv.coroutine_id);
            pending_completions_.push_back({actor_id, mv.coroutine_id});
            to_remove.push_back(actor_id);
            if (completion_callback_) {
                completion_callback_(actor_id, mv.coroutine_id);
            }
        }
    }
    
    for (uint32_t actor_id : to_remove) {
        active_movements_.erase(actor_id);
    }
    
    return completed_coroutines;
}

std::vector<uint32_t> MovementManager::update(int ticks) {
    std::vector<uint32_t> all_completed;
    for (int i = 0; i < ticks; ++i) {
        auto completed = update();
        all_completed.insert(all_completed.end(), completed.begin(), completed.end());
    }
    return all_completed;
}

bool MovementManager::tick_movement(ActiveMovement& mv) {
    if (mv.completed) return true;
    
    // =========================================================================
    // pokecrystal Movement Timing Contract (verified from engine/overworld/)
    // =========================================================================
    //
    // HandleStepType (map_objects.asm):
    //   - Called once per frame for each object
    //   - If STEP_TYPE == FROM_MOVEMENT: call StepFunction_FromMovement
    //   - StepFunction_FromMovement -> MovementFunction_Script -> HandleMovementData
    //
    // HandleMovementData.loop:
    //   - Reads movement command, calls its function
    //   - If function calls ContinueReadingMovement: loop again (same frame)
    //   - Otherwise: exit loop, return
    //
    // Step commands (NormalStep in movement.asm):
    //   - InitStep -> GetNextTile: sets MAP_X/Y to destination, STEP_DURATION=16
    //   - Sets STEP_TYPE=NPC_WALK (does NOT call ContinueReadingMovement)
    //   - Loop exits, frame ends
    //
    // StepFunction_NPCWalk (map_objects.asm):
    //   - Called each frame while STEP_TYPE=NPC_WALK
    //   - AddStepVector: updates SPRITE_X/Y (pixel interpolation)
    //   - Decrements STEP_DURATION
    //   - When STEP_DURATION hits 0: sets STEP_TYPE=FROM_MOVEMENT
    //
    // Turn commands (TurnHead in movement.asm):
    //   - Sets OBJECT_DIRECTION (facing), ACTION=STAND, WALKING=STANDING
    //   - Does NOT call ContinueReadingMovement
    //   - Does NOT set STEP_TYPE or STEP_DURATION
    //   - Loop exits, frame ends, STEP_TYPE remains FROM_MOVEMENT
    //   - Next frame: HandleStepType sees FROM_MOVEMENT, reads next command
    //
    // KEY TIMING:
    //   - Step: 16 frames (destination committed on frame 1, completes frame 16)
    //   - Turn: 1 frame (executed, next command read on following frame)
    //   - step + step + turn + step = 16 + 16 + 1 + 16 = 49 frames
    // =========================================================================
    
    // If not started, this is the first tick - read and start the first command
    if (!mv.started) {
        mv.started = true;
        
        if (mv.current_index >= mv.commands.size()) {
            mv.completed = true;
            return true;
        }
        
        const auto& cmd = mv.commands[mv.current_index];
        
        if (cmd.type == MovementCommandType::StepEnd) {
            mv.completed = true;
            return true;
        }
        
        // Apply command effect at START (pokecrystal: InitStep -> GetNextTile)
        if (cmd.is_movement()) {
            // Destination tile is committed immediately
            apply_direction(mv.current_x, mv.current_y, cmd.direction);
            mv.facing = cmd.direction;
        } else if (cmd.type == MovementCommandType::Turn) {
            mv.facing = cmd.direction;
        }
        
        // Set duration (steps=16, turns=0)
        mv.frames_remaining = cmd.get_duration_frames();
        
        // This tick counts as the first frame of the command
        // For steps: 16 -> 15 (first decrement happens this tick)
        // For turns: 0 -> -1 (will trigger advance next check)
        if (mv.frames_remaining > 0) {
            mv.frames_remaining--;
        }
        
        // Check if command completed this tick (turns have 0 duration)
        if (mv.frames_remaining <= 0) {
            // Turn completed, but we don't read the next command until next tick
            // This matches pokecrystal where TurnHead returns, loop exits
        }
        
        return false;
    }
    
    // Continuing a movement in progress
    
    // Check if current command is done and we need to advance
    if (mv.frames_remaining <= 0) {
        // Previous command completed, advance to next
        // This matches pokecrystal: STEP_TYPE=FROM_MOVEMENT, StepFunction_FromMovement reads next
        mv.current_index++;
        
        if (mv.current_index >= mv.commands.size()) {
            mv.completed = true;
            return true;
        }
        
        const auto& cmd = mv.commands[mv.current_index];
        
        if (cmd.type == MovementCommandType::StepEnd) {
            mv.completed = true;
            return true;
        }
        
        // Apply command effect at START
        if (cmd.is_movement()) {
            apply_direction(mv.current_x, mv.current_y, cmd.direction);
            mv.facing = cmd.direction;
        } else if (cmd.type == MovementCommandType::Turn) {
            mv.facing = cmd.direction;
        }
        
        mv.frames_remaining = cmd.get_duration_frames();
        
        // This tick is the first frame of the new command
        if (mv.frames_remaining > 0) {
            mv.frames_remaining--;
        }
        
        return false;
    }
    
    // Middle of a timed command (step in progress)
    // Decrement remaining frames (pokecrystal: StepFunction_NPCWalk decrements STEP_DURATION)
    mv.frames_remaining--;
    
    // Check for completion
    if (mv.frames_remaining <= 0) {
        // Step completed this tick
        // pokecrystal sets STEP_TYPE=FROM_MOVEMENT here
        // Next tick will read the next command
        
        // Check if this was the last command
        if (mv.current_index + 1 >= mv.commands.size()) {
            mv.completed = true;
            return true;
        }
        
        // Check if next is step_end
        if (mv.commands[mv.current_index + 1].type == MovementCommandType::StepEnd) {
            mv.completed = true;
            return true;
        }
    }
    
    return false;
}

void MovementManager::start_next_command(ActiveMovement& mv) {
    if (mv.current_index >= mv.commands.size()) {
        mv.frames_remaining = 0;
        return;
    }
    
    const auto& cmd = mv.commands[mv.current_index];
    mv.frames_remaining = cmd.get_duration_frames();
}

void MovementManager::apply_direction(int& x, int& y, MovementDirection dir) {
    switch (dir) {
        case MovementDirection::Down:  y++; break;
        case MovementDirection::Up:    y--; break;
        case MovementDirection::Left:  x--; break;
        case MovementDirection::Right: x++; break;
        default: break;
    }
}

bool MovementManager::is_actor_moving(uint32_t actor_id) const {
    return active_movements_.contains(actor_id);
}

std::optional<ActiveMovement> MovementManager::get_movement(uint32_t actor_id) const {
    auto it = active_movements_.find(actor_id);
    if (it == active_movements_.end()) return std::nullopt;
    return it->second;
}

std::optional<MovementManager::ActorMovementState> MovementManager::get_actor_state(uint32_t actor_id) const {
    auto it = active_movements_.find(actor_id);
    if (it == active_movements_.end()) return std::nullopt;
    
    const auto& mv = it->second;
    ActorMovementState state;
    state.x = mv.current_x;
    state.y = mv.current_y;
    state.facing = mv.facing;
    
    // Calculate progress within current step
    if (mv.current_index < mv.commands.size()) {
        int duration = mv.commands[mv.current_index].get_duration_frames();
        if (duration > 0) {
            state.progress = 1.0f - (static_cast<float>(mv.frames_remaining) / duration);
        } else {
            state.progress = 1.0f;
        }
    } else {
        state.progress = 1.0f;
    }
    
    return state;
}

void MovementManager::cancel_movement(uint32_t actor_id) {
    active_movements_.erase(actor_id);
}

void MovementManager::cancel_all() {
    active_movements_.clear();
}

void MovementManager::set_completion_callback(MovementCompleteCallback cb) {
    completion_callback_ = std::move(cb);
}

// Helper functions

std::vector<MovementCmd> batch_to_commands(int down, int up, int left, int right) {
    std::vector<MovementCmd> commands;
    
    // Add steps in each direction
    for (int i = 0; i < down; ++i) {
        commands.push_back({MovementCommandType::Step, MovementDirection::Down, 0});
    }
    for (int i = 0; i < up; ++i) {
        commands.push_back({MovementCommandType::Step, MovementDirection::Up, 0});
    }
    for (int i = 0; i < left; ++i) {
        commands.push_back({MovementCommandType::Step, MovementDirection::Left, 0});
    }
    for (int i = 0; i < right; ++i) {
        commands.push_back({MovementCommandType::Step, MovementDirection::Right, 0});
    }
    
    return commands;
}

MovementDirection direction_from_string(const std::string& dir) {
    if (dir == "down") return MovementDirection::Down;
    if (dir == "up") return MovementDirection::Up;
    if (dir == "left") return MovementDirection::Left;
    if (dir == "right") return MovementDirection::Right;
    return MovementDirection::Down;
}

std::string direction_to_string(MovementDirection dir) {
    switch (dir) {
        case MovementDirection::Down: return "down";
        case MovementDirection::Up: return "up";
        case MovementDirection::Left: return "left";
        case MovementDirection::Right: return "right";
        default: return "down";
    }
}

} // namespace enginemon
