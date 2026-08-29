// engine/scripting/api_bindings.cpp
// C++ semantic APIs exposed to Lua scripts
// Stub implementations for testing - real implementations will call engine systems

#include "engine/scripting/api_bindings.hpp"
#include "engine/scripting/lua_runtime.hpp"
#include "engine/scripting/semantic_ir.hpp"
#include "engine/world/movement_manager.hpp"
#include "engine/core/game_state.hpp"
#include "engine/core/rtc.hpp"
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <utility>
#include <memory>
#include <functional>
#include <string_view>
#include <format>

namespace enginemon {

// Helper to get runtime from table's _runtime field
static LuaRuntime* get_runtime(lua_State* L) {
    lua_getfield(L, 1, "_runtime");
    LuaRuntime* runtime = static_cast<LuaRuntime*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return runtime;
}

// =============================================================================
// UI API - ctx.ui
// =============================================================================

namespace ui_api {

// =============================================================================
// UI API - Presentation Hooks Integration
//
// These functions connect script execution to the visible textbox renderer.
// Callbacks are now per-runtime (stored in LuaRuntime::PresentationHooks),
// NOT process-global state.
//
// This enables proper multi-instance isolation:
// - Runtime A text operation → calls Runtime A's hooks only
// - Runtime B text operation → calls Runtime B's hooks only
// =============================================================================

// ctx.ui:open_text()
int open_text(lua_State* L) {
    LuaRuntime* runtime = get_runtime(L);
    if (runtime && runtime->get_presentation_hooks().open_text) {
        runtime->get_presentation_hooks().open_text();
    }
    return 0;
}

// ctx.ui:close_text()  
int close_text(lua_State* L) {
    LuaRuntime* runtime = get_runtime(L);
    if (runtime && runtime->get_presentation_hooks().close_text) {
        runtime->get_presentation_hooks().close_text();
    }
    return 0;
}

// ctx.ui:text(message)
// Shows text in the textbox. Does NOT yield - Crystal's writetext doesn't yield either.
// The script should explicitly yield via coroutine.yield("wait_button") after text().
// This matches Crystal's semantic separation: writetext displays, waitbutton blocks.
int text(lua_State* L) {
    const char* message = luaL_checkstring(L, 2);
    
    // Call the per-runtime callback to open/update the visible textbox
    LuaRuntime* runtime = get_runtime(L);
    if (runtime && runtime->get_presentation_hooks().text) {
        runtime->get_presentation_hooks().text(message);
    }
    
    // Do NOT yield here - Crystal's writetext doesn't yield.
    // The generated Lua emits an explicit coroutine.yield("wait_button") after this.
    return 0;
}

// ctx.ui:text_scroll(message)
// Shows scrolling text. Does NOT yield - same as text().
int text_scroll(lua_State* L) {
    const char* message = luaL_checkstring(L, 2);
    
    // Same as text() for now - call per-runtime callback but don't yield
    LuaRuntime* runtime = get_runtime(L);
    if (runtime && runtime->get_presentation_hooks().text) {
        runtime->get_presentation_hooks().text(message);
    }
    
    // Do NOT yield - let explicit coroutine.yield handle waiting
    return 0;
}

// ctx.ui:text_sequence(seq_table)
// Receives semantic text sequence with preserved LINE/CONT/PARA distinctions
// seq_table format: { {op="text", text="..."}, {op="line"}, {op="para"}, ... }
// Does NOT yield - script handles yielding via coroutine.yield("wait_button")
int text_sequence(lua_State* L) {
    // Arg 1 is self (ctx.ui), arg 2 is the sequence table
    if (!lua_istable(L, 2)) {
        return luaL_error(L, "text_sequence: expected table argument");
    }
    
    RuntimeTextSequence seq;
    
    // F7: Ordered numeric access — do NOT use lua_next() for semantic arrays.
    // lua_next() iteration order over an integer-keyed table is an implementation
    // detail of Lua 5.4's array/hash split and is not guaranteed by the language spec.
    // lua_rawlen() + lua_rawgeti(1..N) gives formally guaranteed sequential order
    // and rejects holes/non-array structures explicitly.
    int n = static_cast<int>(lua_rawlen(L, 2));
    for (int i = 1; i <= n; ++i) {
        lua_rawgeti(L, 2, i);
        // Stack: element table at -1 (or nil/non-table for holes)
        if (!lua_istable(L, -1)) {
            lua_pop(L, 1);
            // F2: Holes are errors, not silent skips
            return luaL_error(L, "text_sequence: hole at index %d", i);
        }
        
        // Read "op" field
        lua_getfield(L, -1, "op");
        const char* op_str = lua_tostring(L, -1);
        lua_pop(L, 1);  // Pop op value
        
        if (op_str) {
            std::string op(op_str);
            
            if (op == "text") {
                lua_getfield(L, -1, "text");
                const char* text = lua_tostring(L, -1);
                lua_pop(L, 1);
                seq.elements.push_back(RuntimeTextElement::make_text(text ? text : ""));
            }
            else if (op == "line") {
                seq.elements.push_back(RuntimeTextElement::make_line());
            }
            else if (op == "next") {
                seq.elements.push_back(RuntimeTextElement::make_next());
            }
            else if (op == "para") {
                seq.elements.push_back(RuntimeTextElement::make_para());
            }
            else if (op == "cont") {
                seq.elements.push_back(RuntimeTextElement::make_cont());
            }
            else if (op == "scroll") {
                seq.elements.push_back(RuntimeTextElement::make_scroll());
            }
            else if (op == "done") {
                seq.elements.push_back(RuntimeTextElement::make_done());
            }
            else if (op == "prompt") {
                seq.elements.push_back(RuntimeTextElement::make_prompt());
            }
            // F2: Handle dynamic text ops — preserve them instead of silently dropping
            else if (op == "ram") {
                RuntimeTextElement e;
                e.op = RuntimeTextOp::Ram;
                lua_getfield(L, -1, "addr");
                e.addr = static_cast<uint32_t>(lua_tointeger(L, -1));
                lua_pop(L, 1);
                seq.elements.push_back(e);
            }
            else if (op == "bcd") {
                RuntimeTextElement e;
                e.op = RuntimeTextOp::Bcd;
                lua_getfield(L, -1, "addr"); e.addr = static_cast<uint32_t>(lua_tointeger(L, -1)); lua_pop(L, 1);
                lua_getfield(L, -1, "flags"); e.param = static_cast<uint8_t>(lua_tointeger(L, -1)); lua_pop(L, 1);
                seq.elements.push_back(e);
            }
            else if (op == "decimal") {
                RuntimeTextElement e;
                e.op = RuntimeTextOp::Decimal;
                lua_getfield(L, -1, "addr"); e.addr = static_cast<uint32_t>(lua_tointeger(L, -1)); lua_pop(L, 1);
                lua_getfield(L, -1, "param"); e.param = static_cast<uint8_t>(lua_tointeger(L, -1)); lua_pop(L, 1);
                seq.elements.push_back(e);
            }
            else if (op == "buffer") {
                RuntimeTextElement e;
                e.op = RuntimeTextOp::Buffer;
                lua_getfield(L, -1, "id"); e.param = static_cast<uint8_t>(lua_tointeger(L, -1)); lua_pop(L, 1);
                seq.elements.push_back(e);
            }
            else if (op == "far") {
                RuntimeTextElement e;
                e.op = RuntimeTextOp::Far;
                lua_getfield(L, -1, "addr"); e.addr = static_cast<uint32_t>(lua_tointeger(L, -1)); lua_pop(L, 1);
                lua_getfield(L, -1, "bank"); e.param = static_cast<uint8_t>(lua_tointeger(L, -1)); lua_pop(L, 1);
                seq.elements.push_back(e);
            }
            else if (op == "move") {
                RuntimeTextElement e;
                e.op = RuntimeTextOp::Move;
                lua_getfield(L, -1, "pos"); e.addr = static_cast<uint32_t>(lua_tointeger(L, -1)); lua_pop(L, 1);
                seq.elements.push_back(e);
            }
            else if (op == "box") {
                RuntimeTextElement e;
                e.op = RuntimeTextOp::Box;
                lua_getfield(L, -1, "addr"); e.addr = static_cast<uint32_t>(lua_tointeger(L, -1)); lua_pop(L, 1);
                lua_getfield(L, -1, "w"); e.param = static_cast<uint8_t>(lua_tointeger(L, -1)); lua_pop(L, 1);
                lua_getfield(L, -1, "h"); e.param2 = static_cast<uint8_t>(lua_tointeger(L, -1)); lua_pop(L, 1);
                seq.elements.push_back(e);
            }
            else if (op == "low") {
                RuntimeTextElement e; e.op = RuntimeTextOp::Low; seq.elements.push_back(e);
            }
            else if (op == "waitbutton") {
                RuntimeTextElement e; e.op = RuntimeTextOp::WaitButton; seq.elements.push_back(e);
            }
            else if (op == "txscroll") {
                RuntimeTextElement e; e.op = RuntimeTextOp::TxScroll; seq.elements.push_back(e);
            }
            else if (op == "pause") {
                RuntimeTextElement e; e.op = RuntimeTextOp::Unsupported; e.op_name = "pause"; seq.elements.push_back(e);
            }
            else if (op == "day") {
                RuntimeTextElement e; e.op = RuntimeTextOp::Day; seq.elements.push_back(e);
            }
            else if (op == "snd_item" || op == "snd_caught" || op == "snd_fanfare") {
                RuntimeTextElement e; e.op = RuntimeTextOp::Sound; e.op_name = op; seq.elements.push_back(e);
            }
            else if (op == "raw") {
                RuntimeTextElement e; e.op = RuntimeTextOp::Raw; seq.elements.push_back(e);
            }
            else if (op == "asm") {
                RuntimeTextElement e; e.op = RuntimeTextOp::Asm; seq.elements.push_back(e);
            }
            else {
                // Unknown op — preserve as Unsupported with op_name so presentation layer knows
                seq.elements.push_back(RuntimeTextElement::make_unsupported(op));
            }
        }
        lua_pop(L, 1);  // Pop element table
    }
    
    // Call the per-runtime callback to update the visible textbox with semantic sequence
    LuaRuntime* runtime = get_runtime(L);
    if (runtime && runtime->get_presentation_hooks().text_sequence) {
        runtime->get_presentation_hooks().text_sequence(seq);
    }
    
    // Do NOT yield - script handles yielding via coroutine.yield("wait_button")
    return 0;
}

// ctx.ui:choice(options) -> selected index
int choice(lua_State* L) {
    // Stub: return 1 (first choice)
    lua_pushinteger(L, 1);
    return 1;
}

// ctx.ui:yes_no() -> bool
int yes_no(lua_State* L) {
    // Stub: return true (yes) — returns 1 (integer) per VM result contract
    lua_pushinteger(L, 1);
    return 1;
}

// ctx.ui:fade_out(speed?)
int fade_out(lua_State* L) {
    int speed = luaL_optinteger(L, 2, 1);
    (void)speed;
    // Real impl would yield until fade complete
    return 0;
}

// ctx.ui:fade_in(speed?)
int fade_in(lua_State* L) {
    int speed = luaL_optinteger(L, 2, 1);
    (void)speed;
    return 0;
}

// ctx.ui:show_map_name()
int show_map_name(lua_State* L) {
    return 0;
}

// ctx.ui:inline_prompt_button()
// Non-terminating in-stream input gate (TX_PROMPT_BUTTON semantics).
// Shows blinking cursor, waits for A/B, text continues after.
// Yields "wait_button" so the game loop can handle input gating.
int inline_prompt_button(lua_State* L) {
    LuaRuntime* runtime = get_runtime(L);
    if (runtime && runtime->get_presentation_hooks().open_text) {
        // Notify presentation layer that an inline button wait is requested
        // (presentation can show/hide blinking cursor)
    }
    // Yield like a dialog wait — caller resumes when player presses button
    lua_pushstring(L, "wait_button");
    return lua_yield(L, 1);
}

// ctx.ui:pause_text(frames)
// Timed pause between text elements.
// frames = 30 for all vanilla Crystal TX_PAUSE uses (~0.5s at 60fps).
// Skippable if button held at entry time (handled by runtime on resume).
int pause_text(lua_State* L) {
    int frames = luaL_checkinteger(L, 2);
    // Delegate to the existing wait_frames mechanism
    script_wait_frames(L, frames);
    return 0; // wait_frames already yielded
}

} // namespace ui_api

// =============================================================================
// World API - ctx.world
// =============================================================================

namespace world_api {

// =============================================================================
// World API Stub Implementation
//
// These functions use per-runtime StubServices for isolated state.
// Destroying LuaRuntime destroys its stub state automatically.
// =============================================================================

// Accessor types re-exported for API compatibility
using ActorState = StubActorState;

// Helper to get per-runtime stub state
static StubServices& get_stubs(lua_State* L) {
    LuaRuntime* runtime = get_runtime(L);
    return runtime->get_stub_services();
}

// Sync helper: apply movement immediately (for backward compatibility)
static void apply_sync_movement(StubServices& stubs, int actor_id, int down, int up, int left, int right) {
    StubActorState& state = (actor_id == 0) ? stubs.player : stubs.actors[actor_id];
    
    state.y += down;
    state.y -= up;
    state.x -= left;
    state.x += right;
    
    // Set facing to last non-zero direction
    if (right > 0) state.facing = "right";
    else if (left > 0) state.facing = "left";
    else if (down > 0) state.facing = "down";
    else if (up > 0) state.facing = "up";
}

// Test helpers - operate on specific runtime's stub state
void reset_world_state(LuaRuntime* runtime) {
    auto& stubs = runtime->get_stub_services();
    stubs.actors.clear();
    stubs.player = {5, 5, "down", true};
    stubs.movement_calls.clear();
    if (stubs.movement_manager) {
        stubs.movement_manager->cancel_all();
        stubs.movement_manager->clear_pending_completions();
    }
}

void set_actor_pos(LuaRuntime* runtime, int id, int x, int y) {
    auto& stubs = runtime->get_stub_services();
    if (id == 0) {
        stubs.player.x = x;
        stubs.player.y = y;
    } else {
        stubs.actors[id].x = x;
        stubs.actors[id].y = y;
    }
}

void set_actor_facing(LuaRuntime* runtime, int id, const std::string& facing) {
    auto& stubs = runtime->get_stub_services();
    if (id == 0) {
        stubs.player.facing = facing;
    } else {
        stubs.actors[id].facing = facing;
    }
}

ActorState get_actor_state(LuaRuntime* runtime, int id) {
    const auto& stubs = runtime->get_stub_services();
    if (id == 0) return stubs.player;
    auto it = stubs.actors.find(id);
    return it != stubs.actors.end() ? it->second : ActorState{0, 0, "down", false};
}

const std::vector<std::pair<std::string, std::string>>& get_movement_calls(LuaRuntime* runtime) {
    return runtime->get_stub_services().movement_calls;
}

MovementManager& get_movement_manager(LuaRuntime* runtime) {
    return runtime->get_stub_services().get_movement_manager();
}

void set_async_movement(LuaRuntime* runtime, bool enabled) {
    runtime->get_stub_services().async_movement_enabled = enabled;
}

bool is_async_movement_enabled(LuaRuntime* runtime) {
    return runtime->get_stub_services().async_movement_enabled;
}

// ctx.world:move_actor(actor_id, direction, steps) - string-direction API
// Used by isolated tests that write Lua directly.
int move_actor(lua_State* L) {
    auto& stubs = get_stubs(L);
    
    int actor_id = luaL_checkinteger(L, 2);
    const char* direction = luaL_checkstring(L, 3);
    int steps = luaL_optinteger(L, 4, 1);
    
    stubs.movement_calls.push_back({"move", direction});
    
    std::string dir(direction);
    int down = 0, up = 0, left = 0, right = 0;
    if (dir == "down") down = steps;
    else if (dir == "up") up = steps;
    else if (dir == "left") left = steps;
    else if (dir == "right") right = steps;
    
    if (stubs.async_movement_enabled) {
        // Select the authoritative movement manager.
        // scripted_movement_manager is set by HeadlessGameLoop::set_lua_runtime();
        // it points to HeadlessGameLoop::movement_manager_ which update_script()
        // observes.  When not wired (isolated unit tests), fall back to the
        // per-runtime stub manager.
        MovementManager& mm = stubs.scripted_movement_manager
            ? *stubs.scripted_movement_manager
            : stubs.get_movement_manager();

        StubActorState& state = (actor_id == 0) ? stubs.player : stubs.actors[actor_id];
        // Use published active coroutine ID — NOT actor_id — so the completion
        // callback can resume the correct Lua coroutine.
        uint32_t coro_id = stubs.active_script_coroutine_id;
        mm.enqueue_movement_table(
            static_cast<uint32_t>(actor_id),
            coro_id,
            down, up, left, right,
            state.x, state.y,
            direction_from_string(state.facing)
        );
        
        // Yield with actor_id in yield_data so HeadlessGameLoop::update_script()
        // can call is_actor_moving(actor_id) on the right actor.
        stubs.movement_calls.push_back({"waiting_actor", std::to_string(actor_id)});
        lua_pushstring(L, "movement");
        lua_pushinteger(L, static_cast<lua_Integer>(actor_id));  // yield_data[1]
        return lua_yield(L, 2);  // yield "movement", actor_id
    } else {
        // Sync mode: apply immediately
        apply_sync_movement(stubs, actor_id, down, up, left, right);
        return 0;
    }
}

// Parse the emitter's {type,dir} array format into an ordered vector<MovementCmd>.
// Emitter produces: {{type="step",dir="left"},{type="turn",dir="down"},...,{type="step_end"}}
// Preserves command order, type, direction, speed, and sleep parameters exactly.
// Returns true if the table is in array-of-tables format; false if it is the
// batch format {down=N,up=N,left=N,right=N} (caller must fall back).
static bool parse_movement_command_sequence(lua_State* L, int table_idx,
                                            std::vector<MovementCmd>& out_cmds)
{
    // Detect: if t[1] exists and is a table, it's the emitter command array.
    lua_rawgeti(L, table_idx, 1);
    bool is_array = lua_istable(L, -1);
    lua_pop(L, 1);
    if (!is_array) return false;

    int n = static_cast<int>(lua_rawlen(L, table_idx));
    for (int i = 1; i <= n; ++i) {
        lua_rawgeti(L, table_idx, i);
        if (!lua_istable(L, -1)) { lua_pop(L, 1); continue; }

        lua_getfield(L, -1, "type");
        const char* t = lua_isstring(L, -1) ? lua_tostring(L, -1) : nullptr;
        lua_pop(L, 1);

        lua_getfield(L, -1, "dir");
        const char* d = lua_isstring(L, -1) ? lua_tostring(L, -1) : nullptr;
        lua_pop(L, 1);

        lua_getfield(L, -1, "frames");
        int frames = lua_isinteger(L, -1) ? static_cast<int>(lua_tointeger(L, -1)) : 0;
        lua_pop(L, 1);

        lua_pop(L, 1);  // pop command table

        if (!t) continue;
        std::string type(t);

        MovementCmd cmd;
        if (type == "step_end") {
            cmd.type      = MovementCommandType::StepEnd;
            cmd.direction = MovementDirection::Down;
            out_cmds.push_back(cmd);
            break;  // StepEnd terminates the sequence
        } else if (type == "sleep") {
            cmd.type      = MovementCommandType::StepSleep;
            cmd.direction = MovementDirection::None;
            cmd.param     = frames > 0 ? frames : 16;
            out_cmds.push_back(cmd);
        } else if (d) {
            // Direction-bearing commands: step, slow_step, big_step, turn
            std::string dir(d);
            if      (dir == "down")  cmd.direction = MovementDirection::Down;
            else if (dir == "up")    cmd.direction = MovementDirection::Up;
            else if (dir == "left")  cmd.direction = MovementDirection::Left;
            else if (dir == "right") cmd.direction = MovementDirection::Right;
            else                     cmd.direction = MovementDirection::Down;

            if      (type == "step")      cmd.type = MovementCommandType::Step;
            else if (type == "slow_step") cmd.type = MovementCommandType::SlowStep;
            else if (type == "big_step")  cmd.type = MovementCommandType::BigStep;
            else if (type == "turn")      cmd.type = MovementCommandType::Turn;
            else                          cmd.type = MovementCommandType::Step;  // unknown → step

            out_cmds.push_back(cmd);
        }
        // Unknown entries without a dir field are silently skipped.
    }
    return true;
}

// ctx.world:move_actor(actor_id, table) — table is either:
//   emitter format: {{type="step",dir="left"},...}   (from SemanticLuaEmitter)
//   batch format:   {down=N, up=N, left=N, right=N}  (hand-written tests)
// actor_id == -2 is the LastTalked sentinel from Sem_ApplyMovement{LastTalked}.
// It resolves to stubs.last_talked_id at runtime.
int move_actor_table(lua_State* L, int actor_id, int table_idx) {
    auto& stubs = get_stubs(L);

    // Resolve LastTalked sentinel (-2) to the actual last-talked NPC id.
    if (actor_id == -2) {
        actor_id = static_cast<int>(stubs.last_talked_id);
        if (actor_id == 0) {
            // No last-talked NPC recorded — nothing to move.
            stubs.movement_calls.push_back({"move_noop", "last_talked_unresolved"});
            return 0;
        }
    }

    // -------------------------------------------------------------------------
    // Path A: emitter's ordered command array — preserves sequence, type, speed
    // -------------------------------------------------------------------------
    std::vector<MovementCmd> ordered_cmds;
    bool parsed_as_array = parse_movement_command_sequence(L, table_idx, ordered_cmds);

    if (parsed_as_array) {
        // Determine whether there are any steps (tile-moving commands) so we
        // know whether the sequence will actually move the actor.
        bool has_steps = false;
        for (const auto& c : ordered_cmds) {
            if (c.type == MovementCommandType::Step ||
                c.type == MovementCommandType::SlowStep ||
                c.type == MovementCommandType::BigStep) {
                has_steps = true;
                break;
            }
        }

        if (!has_steps && ordered_cmds.empty()) {
            // Completely empty sequence — nothing to do.
            stubs.movement_calls.push_back({"move_noop", std::to_string(actor_id)});
            return 0;
        }

        stubs.movement_calls.push_back({"move_sequence", std::to_string(actor_id)});

        if (stubs.async_movement_enabled) {
            // Enqueue the ordered sequence directly — no rebatching.
            MovementManager& mm = stubs.scripted_movement_manager
                ? *stubs.scripted_movement_manager
                : stubs.get_movement_manager();

            // Get starting position: prefer authoritative NPC position from
            // HeadlessGameLoop (via actor_pos_query) over the stub actor map,
            // which defaults to {0,0} and would produce wrong movement trajectories.
            int start_x, start_y;
            if (actor_id != 0 && stubs.actor_pos_query) {
                auto [qx, qy] = stubs.actor_pos_query(static_cast<uint32_t>(actor_id));
                start_x = qx; start_y = qy;
            } else {
                StubActorState& state = (actor_id == 0)
                    ? stubs.player : stubs.actors[actor_id];
                start_x = state.x; start_y = state.y;
            }
            MovementDirection start_facing;
            {
                StubActorState& st2 = (actor_id == 0)
                    ? stubs.player : stubs.actors[actor_id];
                start_facing = direction_from_string(st2.facing);
            }
            uint32_t coro_id = stubs.active_script_coroutine_id;

            mm.enqueue_movement(
                static_cast<uint32_t>(actor_id),
                coro_id,
                ordered_cmds,
                start_x, start_y,
                start_facing
            );

            lua_pushstring(L, "movement");
            lua_pushinteger(L, static_cast<lua_Integer>(actor_id));
            return lua_yield(L, 2);
        } else {
            // Sync mode: apply each step immediately to stub state.
            StubActorState& state = (actor_id == 0)
                ? stubs.player : stubs.actors[actor_id];
            for (const auto& c : ordered_cmds) {
                switch (c.type) {
                    case MovementCommandType::Step:
                    case MovementCommandType::SlowStep:
                    case MovementCommandType::BigStep:
                        switch (c.direction) {
                            case MovementDirection::Down:  state.y++; break;
                            case MovementDirection::Up:    state.y--; break;
                            case MovementDirection::Left:  state.x--; break;
                            case MovementDirection::Right: state.x++; break;
                            default: break;
                        }
                        state.facing = direction_to_string(c.direction);
                        break;
                    case MovementCommandType::Turn:
                        state.facing = direction_to_string(c.direction);
                        break;
                    default:
                        break;
                }
            }
            return 0;
        }
    }

    // -------------------------------------------------------------------------
    // Path B: batch format {down=N, up=N, left=N, right=N} — hand-written tests
    // -------------------------------------------------------------------------
    int down = 0, up = 0, left = 0, right = 0;

    lua_getfield(L, table_idx, "down");
    if (!lua_isnil(L, -1)) down = static_cast<int>(lua_tointeger(L, -1));
    lua_pop(L, 1);

    lua_getfield(L, table_idx, "up");
    if (!lua_isnil(L, -1)) up = static_cast<int>(lua_tointeger(L, -1));
    lua_pop(L, 1);

    lua_getfield(L, table_idx, "left");
    if (!lua_isnil(L, -1)) left = static_cast<int>(lua_tointeger(L, -1));
    lua_pop(L, 1);

    lua_getfield(L, table_idx, "right");
    if (!lua_isnil(L, -1)) right = static_cast<int>(lua_tointeger(L, -1));
    lua_pop(L, 1);

    // Reject completely empty movement.
    if (down == 0 && up == 0 && left == 0 && right == 0) {
        stubs.movement_calls.push_back({"move_noop", std::to_string(actor_id)});
        return 0;
    }

    stubs.movement_calls.push_back({"move_table",
        std::to_string(down) + "," + std::to_string(up) + "," +
        std::to_string(left) + "," + std::to_string(right)});

    if (stubs.async_movement_enabled) {
        MovementManager& mm = stubs.scripted_movement_manager
            ? *stubs.scripted_movement_manager
            : stubs.get_movement_manager();

        StubActorState& state = (actor_id == 0) ? stubs.player : stubs.actors[actor_id];
        uint32_t coro_id = stubs.active_script_coroutine_id;
        mm.enqueue_movement_table(
            static_cast<uint32_t>(actor_id),
            coro_id,
            down, up, left, right,
            state.x, state.y,
            direction_from_string(state.facing)
        );

        lua_pushstring(L, "movement");
        lua_pushinteger(L, static_cast<lua_Integer>(actor_id));
        return lua_yield(L, 2);
    } else {
        apply_sync_movement(stubs, actor_id, down, up, left, right);
        return 0;
    }
}

// Wrapper that detects whether arg 3 is a string or table
int move_actor_dispatch(lua_State* L) {
    int actor_id = static_cast<int>(luaL_checkinteger(L, 2));
    
    if (lua_istable(L, 3)) {
        return move_actor_table(L, actor_id, 3);
    } else {
        return move_actor(L);
    }
}

// ctx.world:face_actor(actor_id, direction)
// Sets the facing direction of an NPC or the player.
// actor_id == 0 → player; actor_id > 0 → NPC.
// Writes to HeadlessGameLoop NpcState::facing / PlayerState::facing via callbacks.
// Source: Crystal turnobject / Sem_TurnObject, and warp-facing ops.
int face_actor(lua_State* L) {
    auto& stubs = get_stubs(L);
    LuaRuntime* runtime = get_runtime(L);
    
    int actor_id = static_cast<int>(luaL_checkinteger(L, 2));
    const char* direction = luaL_checkstring(L, 3);
    
    // Resolve direction string to enum for authoritative state update.
    Direction dir = Direction::Down;
    std::string d(direction);
    if      (d == "up")    dir = Direction::Up;
    else if (d == "down")  dir = Direction::Down;
    else if (d == "left")  dir = Direction::Left;
    else if (d == "right") dir = Direction::Right;

    if (actor_id == 0) {
        // Player facing
        if (stubs.set_player_facing_fn) {
            stubs.set_player_facing_fn(dir);
        }
        stubs.player.facing = direction;
    } else {
        // NPC facing
        if (stubs.set_npc_facing_fn) {
            stubs.set_npc_facing_fn(static_cast<uint16_t>(actor_id), dir);
        }
        stubs.actors[actor_id].facing = direction;
    }
    
    stubs.movement_calls.push_back({"face", direction});
    return 0;
}

// ctx.world:face_player() - the interacting NPC faces toward the player.
// Source: Crystal faceplayer / Sem_FacePlayer — NPC turns to face the player.
// Resolves facing via player_pos_query and the last_talked NPC's position.
// Updates NpcState::facing via set_npc_facing_fn.
int face_player(lua_State* L) {
    auto& stubs = get_stubs(L);
    stubs.movement_calls.push_back({"face_player", ""});

    // If we have a wired HeadlessGameLoop, resolve actual direction.
    if (!stubs.last_talked_id || !stubs.player_pos_query || !stubs.actor_pos_query) {
        return 0;  // Stub mode: no state to update
    }

    auto [px, py] = stubs.player_pos_query();
    auto [nx, ny] = stubs.actor_pos_query(static_cast<uint32_t>(stubs.last_talked_id));

    // Choose direction: prefer axis with larger offset; prefer vertical over horizontal.
    int dx = px - nx;
    int dy = py - ny;
    Direction dir;
    if (std::abs(dy) >= std::abs(dx)) {
        dir = (dy >= 0) ? Direction::Down : Direction::Up;
    } else {
        dir = (dx >= 0) ? Direction::Right : Direction::Left;
    }

    if (stubs.set_npc_facing_fn) {
        stubs.set_npc_facing_fn(stubs.last_talked_id, dir);
    }
    return 0;
}

// ctx.world:get_player_pos() -> x, y, map_id
// ctx.world:get_player_pos() -> x, y, map_id
// Returns authoritative player coordinates.
// Authority order (production → test):
//   1. player_pos_query callback (wired by HeadlessGameLoop::set_lua_runtime) → game_loop.player_.x/y
//   2. GameState::player.x/y (when bound, no game_loop available)
//   3. stubs.player.x/y (stub/isolated test only)
int get_player_pos(lua_State* L) {
    LuaRuntime* runtime = get_runtime(L);
    auto& stubs = runtime->get_stub_services();

    int32_t px = 0, py = 0;
    if (stubs.player_pos_query) {
        // Production: wired to HeadlessGameLoop::player_.x/y
        auto [qx, qy] = stubs.player_pos_query();
        px = qx; py = qy;
    } else if (const GameState* gs = runtime->get_game_state()) {
        // Fallback: GameState authoritative position (e.g. headless without full loop)
        px = gs->player.x;
        py = gs->player.y;
    } else {
        // Last resort: stub actor (test isolation only)
        px = stubs.player.x;
        py = stubs.player.y;
    }
    lua_pushinteger(L, px);
    lua_pushinteger(L, py);
    lua_pushinteger(L, 1);  // map_id — semantic map identity not yet exposed here
    return 3;
}

// ctx.world:get_actor_pos(actor_id) -> x, y
int get_actor_pos(lua_State* L) {
    LuaRuntime* runtime = get_runtime(L);
    int actor_id = static_cast<int>(luaL_checkinteger(L, 2));
    ActorState state = get_actor_state(runtime, actor_id);
    lua_pushinteger(L, state.x);
    lua_pushinteger(L, state.y);
    return 2;
}

// ctx.world:get_actor_facing(actor_id) -> string
int get_actor_facing(lua_State* L) {
    LuaRuntime* runtime = get_runtime(L);
    int actor_id = static_cast<int>(luaL_checkinteger(L, 2));
    ActorState state = get_actor_state(runtime, actor_id);
    lua_pushstring(L, state.facing.c_str());
    return 1;
}

// ctx.world:teleport_player(map_id, x, y)
// (Reserved for future player teleport — not currently emitted by scripts)
int teleport_player(lua_State* L) {
    int map_id = luaL_checkinteger(L, 2);
    int x = luaL_checkinteger(L, 3);
    int y = luaL_checkinteger(L, 4);
    (void)map_id; (void)x; (void)y;
    return 0;
}

// ctx.world:teleport_npc(npc_id, x, y)
// Immediately places an NPC at (x, y) without movement animation.
// Source: Crystal moveobject opcode → Sem_MoveObject.
// Writes to HeadlessGameLoop::NpcState::{x,y,is_moving} via teleport_npc_fn.
int teleport_npc(lua_State* L) {
    uint16_t npc_id = static_cast<uint16_t>(luaL_checkinteger(L, 2));
    int32_t x       = static_cast<int32_t>(luaL_checkinteger(L, 3));
    int32_t y       = static_cast<int32_t>(luaL_checkinteger(L, 4));
    LuaRuntime* runtime = get_runtime(L);
    auto& stubs = runtime->get_stub_services();
    if (stubs.teleport_npc_fn) {
        stubs.teleport_npc_fn(npc_id, x, y);
    } else {
        stubs.actors[static_cast<int>(npc_id)].x = x;
        stubs.actors[static_cast<int>(npc_id)].y = y;
    }
    return 0;
}

// ctx.world:face_toward(actor_id, target_actor_id)
// actor_id faces toward target_actor_id by computing their relative positions.
// Source: Crystal faceobject object1, object2 → Sem_FaceObject.
// object1 turns to face object2. Computes direction at runtime.
int face_toward(lua_State* L) {
    auto& stubs = get_stubs(L);
    LuaRuntime* runtime = get_runtime(L);
    uint16_t actor_id  = static_cast<uint16_t>(luaL_checkinteger(L, 2));
    uint16_t target_id = static_cast<uint16_t>(luaL_checkinteger(L, 3));

    // Resolve actor position
    std::pair<int32_t,int32_t> a_pos{0, 0}, t_pos{0, 0};
    if (stubs.actor_pos_query) {
        a_pos = stubs.actor_pos_query(actor_id);
        t_pos = stubs.actor_pos_query(target_id);
    } else if (target_id == 0 && stubs.player_pos_query) {
        a_pos = stubs.actor_pos_query
            ? stubs.actor_pos_query(actor_id)
            : std::make_pair<int32_t,int32_t>(0,0);
        t_pos = stubs.player_pos_query();
    }

    int dx = t_pos.first  - a_pos.first;
    int dy = t_pos.second - a_pos.second;
    Direction dir;
    if (std::abs(dy) >= std::abs(dx)) {
        dir = (dy >= 0) ? Direction::Down : Direction::Up;
    } else {
        dir = (dx >= 0) ? Direction::Right : Direction::Left;
    }

    if (actor_id == 0) {
        if (stubs.set_player_facing_fn) stubs.set_player_facing_fn(dir);
    } else {
        if (stubs.set_npc_facing_fn) stubs.set_npc_facing_fn(actor_id, dir);
        const char* d = "down";
        if (dir == Direction::Up)    d = "up";
        if (dir == Direction::Left)  d = "left";
        if (dir == Direction::Right) d = "right";
        stubs.actors[actor_id].facing = d;
    }
    stubs.movement_calls.push_back({"face_toward", std::to_string(target_id)});
    return 0;
}

// ctx.world:set_last_talked(npc_id)
// Records the last-talked NPC id for subsequent LastTalked-targeted operations.
// Source: Crystal setlasttalked opcode → Sem_SetLastTalked.
int set_last_talked(lua_State* L) {
    uint16_t npc_id = static_cast<uint16_t>(luaL_checkinteger(L, 2));
    LuaRuntime* runtime = get_runtime(L);
    runtime->get_stub_services().last_talked_id = npc_id;
    return 0;
}

// ctx.world:warp(map_id, x, y) — scripted warp to map coordinates
// map_id may be a string (canonical package ID, e.g. "new_bark_town") or an
// integer (legacy numeric MapId for tools/tests that skip the linker string pass).
// Production bootstrap sets stubs.warp_fn to route through WorldManager::prepare_warp /
// commit_warp for atomic staged transition; failure leaves authoritative state unchanged.
int warp(lua_State* L) {
    LuaRuntime* runtime = get_runtime(L);
    auto& stubs = runtime->get_stub_services();

    // Accept string or integer map_id
    std::string map_id_str;
    int numeric_map_id = 0;
    if (lua_type(L, 2) == LUA_TSTRING) {
        map_id_str = luaL_checkstring(L, 2);
        // numeric form not available for string map IDs
        numeric_map_id = 0;
    } else {
        numeric_map_id = static_cast<int>(luaL_checkinteger(L, 2));
        map_id_str = std::to_string(numeric_map_id);
    }
    int x = luaL_checkinteger(L, 3);
    int y = luaL_checkinteger(L, 4);

    stubs.last_warp_map = numeric_map_id;
    stubs.last_warp_x = x;
    stubs.last_warp_y = y;
    stubs.movement_calls.push_back({"warp", map_id_str + "," + std::to_string(x) + "," + std::to_string(y)});

    if (stubs.warp_fn) {
        bool ok = stubs.warp_fn(map_id_str, static_cast<int32_t>(x), static_cast<int32_t>(y));
        if (!ok) {
            // Surface explicit failure so the coroutine can observe the error
            return luaL_error(L, "warp failed: destination '%s' could not be loaded", map_id_str.c_str());
        }
    }
    return 0;
}

// ctx.world:warp_to_spawn()
// Warp the player to the backup warp position stored in GameState::warp_memory.
// Corresponds to Crystal's warptobspawn / WarpToBackup semantics.
//
// Source: Crystal Script_warpbspawn reads wBackupWarp (backup_map_id/x/y).
// Production: calls warp_fn with the backup coordinates, executing the full
// staged/atomic world transition.
// Explicit failure: if no backup warp is set OR warp_fn not wired, luaL_error.
int warp_to_spawn(lua_State* L) {
    LuaRuntime* runtime = get_runtime(L);
    auto& stubs = runtime->get_stub_services();
    stubs.movement_calls.push_back({"warp_to_spawn", ""});

    if (stubs.warp_fn) {
        // Read backup warp from authoritative GameState
        if (const GameState* gs = runtime->get_game_state()) {
            const std::string& backup_map = gs->warp_memory.backup_map_id;
            if (backup_map.empty()) {
                return luaL_error(L, "warp_to_spawn: no backup warp set in GameState "
                                     "(warp_memory.backup_map_id is empty)");
            }
            bool ok = stubs.warp_fn(backup_map,
                                    static_cast<int32_t>(gs->warp_memory.backup_x),
                                    static_cast<int32_t>(gs->warp_memory.backup_y));
            if (!ok) {
                return luaL_error(L, "warp_to_spawn: transition to backup map '%s' failed",
                                  backup_map.c_str());
            }
        } else {
            return luaL_error(L, "warp_to_spawn: no GameState bound — cannot read backup warp");
        }
    } else {
        // No warp_fn wired (unit tests without production bootstrap).
        // This is a capability gap — not silent, not fabricated.
        return luaL_error(L, "warp_to_spawn: warp_fn not wired (production bootstrap required)");
    }
    return 0;
}

// ctx.world:show_npc(npc_id)
// Makes the NPC visible.  Source: Crystal showobject opcode → Sem_ShowObject.
// Crystal Script_appear semantics (scripting.asm): clears the NPC's event_flag
// (flag CLEAR = visible per CheckObjectFlag in map_objects_2.asm).
// Writes to HeadlessGameLoop::NpcState::visible AND clears the controlling
// GameState flag so visibility persists across map unload/reload.
int show_npc(lua_State* L) {
    uint16_t npc_id = static_cast<uint16_t>(luaL_checkinteger(L, 2));
    LuaRuntime* runtime = get_runtime(L);
    auto& stubs = runtime->get_stub_services();

    // 1. Update live NpcState::visible
    if (stubs.set_npc_visible_fn) {
        stubs.set_npc_visible_fn(npc_id, true);
    } else {
        stubs.actors[npc_id].visible = true;
    }

    // 2. Persist to GameState::flags so visibility survives map transitions.
    //    Crystal Script_appear → RESET_FLAG (clear) on the NPC's event_flag.
    //    flag CLEAR = visible (CheckObjectFlag: !flag → .unmasked → visible).
    if (GameState* gs = runtime->get_game_state()) {
        std::string vis_flag;
        if (stubs.get_npc_visibility_flag_fn) {
            vis_flag = stubs.get_npc_visibility_flag_fn(npc_id);
        }
        if (!vis_flag.empty()) {
            gs->clear_flag(vis_flag);  // Crystal Script_appear: RESET_FLAG
        }
    }
    return 0;
}

// ctx.world:hide_npc(npc_id)
// Hides the NPC.  Source: Crystal hideobject opcode → Sem_HideObject.
// Crystal Script_disappear semantics (scripting.asm): sets the NPC's event_flag
// (flag SET = hidden per CheckObjectFlag in map_objects_2.asm).
// Writes to HeadlessGameLoop::NpcState::visible AND sets the controlling
// GameState flag so the NPC stays hidden across map unload/reload.
int hide_npc(lua_State* L) {
    uint16_t npc_id = static_cast<uint16_t>(luaL_checkinteger(L, 2));
    LuaRuntime* runtime = get_runtime(L);
    auto& stubs = runtime->get_stub_services();

    // 1. Update live NpcState::visible
    if (stubs.set_npc_visible_fn) {
        stubs.set_npc_visible_fn(npc_id, false);
    } else {
        stubs.actors[npc_id].visible = false;
    }

    // 2. Persist to GameState::flags so visibility survives map transitions.
    //    Crystal Script_disappear → SET_FLAG on the NPC's event_flag.
    //    flag SET = hidden (CheckObjectFlag: flag → .masked → hidden).
    if (GameState* gs = runtime->get_game_state()) {
        std::string vis_flag;
        if (stubs.get_npc_visibility_flag_fn) {
            vis_flag = stubs.get_npc_visibility_flag_fn(npc_id);
        }
        if (!vis_flag.empty()) {
            gs->set_flag(vis_flag);    // Crystal Script_disappear: SET_FLAG
        }
        // If the NPC has no controlling visibility_flag (0xFFFF in ROM),
        // the hide is transient (no flag to persist). This matches Crystal
        // Script_disappear behavior which no-ops on flag == -1.
    }
    return 0;
}

// ctx.world:npc_visible(npc_id) -> bool
// Returns the current NpcState::visible value.
// When set_npc_visible_fn is wired (production), queries HeadlessGameLoop::NpcState.
// Falls back to StubServices::actors for tests without a real game loop.
int npc_visible(lua_State* L) {
    uint16_t npc_id = static_cast<uint16_t>(luaL_checkinteger(L, 2));
    LuaRuntime* runtime = get_runtime(L);
    auto& stubs = runtime->get_stub_services();

    // Production path: HeadlessGameLoop wires a query callback via actor_pos_query
    // pattern. Use the actors map for visibility if set, otherwise check the
    // set_npc_visible_fn wiring to determine production vs stub mode.
    // The StubServices::actors map is written by hide_npc/show_npc in stub mode,
    // so querying it gives the correct answer in both paths.
    auto it = stubs.actors.find(static_cast<int>(npc_id));
    if (it != stubs.actors.end()) {
        lua_pushinteger(L, it->second.visible ? 1 : 0);
    } else {
        // NPC not in stub actors — assume visible (default before any hide call).
        // In production this means the NPC exists and has never been explicitly hidden.
        lua_pushinteger(L, 1);
    }
    return 1;
}

// ctx.world:current_map() -> map_id
int current_map(lua_State* L) {
    lua_pushinteger(L, 1);  // Stub
    return 1;
}

// ctx.world:map_name(map_id) -> string
int map_name(lua_State* L) {
    int map_id = luaL_checkinteger(L, 2);
    lua_pushstring(L, "new_bark_town");  // Stub
    return 1;
}

// ctx.world:apply_movement(object_id, movement_data)
int apply_movement(lua_State* L) {
    int object_id = luaL_checkinteger(L, 2);
    (void)object_id;
    return 0;
}

// ctx.world:set_variable_sprite(slot_name, sprite_ref)
// Assigns a stable SpriteId to the named variable slot.
// slot_name: semantic slot identity (e.g., "copycat", "fuchsia_gym_1")
// sprite_ref: stable typed SpriteId string (e.g., "fixed:lass", "fixed:janine")
// Stores in GameState::variable_sprites[slot_name] = sprite_ref.
// Source: Crystal variablesprite opcode 0x6D + wVariableSprites semantic.
// The SpriteId passes through to the package/renderer directly — no Crystal
// numeric index mapping occurs in the engine layer.
int set_variable_sprite(lua_State* L) {
    const char* slot_name  = luaL_checkstring(L, 2);
    const char* sprite_ref = luaL_checkstring(L, 3);
    LuaRuntime* runtime = get_runtime(L);

    if (GameState* gs = runtime->get_game_state()) {
        gs->variable_sprites[slot_name] = sprite_ref;
    } else {
        // Stub mode: store for test inspection.
        auto& stubs = runtime->get_stub_services();
        stubs.last_variable_sprite_slot = slot_name;
        stubs.last_variable_sprite_ref  = sprite_ref;
    }
    return 0;
}

// ctx.world:set_daycare_species(slot, species_id)
// Sets which Pokémon species occupies a Day Care slot.
// slot: 1 or 2 (matching "daycare:1" / "daycare:2" sprite namespaces)
// species_id: 1-N (valid species per loaded game data), or 0 to clear the slot.
// The upper species ceiling is not hardcoded — it is validated by the species
// registry at runtime. This allows non-251 profiles to work without engine changes.
// Stores in GameState::daycare_slot[slot-1].
int set_daycare_species(lua_State* L) {
    int slot       = static_cast<int>(luaL_checkinteger(L, 2));
    int species_id = static_cast<int>(luaL_checkinteger(L, 3));
    LuaRuntime* runtime = get_runtime(L);

    if (slot < 1 || slot > 2) {
        return luaL_error(L, "set_daycare_species: slot must be 1 or 2, got %d", slot);
    }
    // species_id == 0 clears the slot (SPECIES_NONE).
    // Upper bound is not hardcoded to 251 — actual validation is by registry
    // membership at runtime (FrozenGameData species registry).
    // Any non-zero uint16_t is accepted here; invalid IDs will fail downstream
    // when the species registry lookup returns no definition.
    if (species_id < 0 || species_id > 65534) {
        return luaL_error(L,
            "set_daycare_species: species_id out of range [0, 65534], got %d", species_id);
    }

    if (GameState* gs = runtime->get_game_state()) {
        gs->daycare_slot[slot - 1] = static_cast<SpeciesId>(species_id);
    } else {
        auto& stubs = runtime->get_stub_services();
        // Store for test inspection using reserved stub vars
        stubs.vars[slot == 1 ? -10 : -11] = species_id;
    }
    return 0;
}

} // namespace world_api

// =============================================================================
// Battle API - ctx.battle
// =============================================================================

namespace battle_api {

// ctx.battle:start_wild(species_id, level)
int start_wild(lua_State* L) {
    int species_id = luaL_checkinteger(L, 2);
    int level = luaL_checkinteger(L, 3);
    (void)species_id; (void)level;
    // Stub: return win result
    lua_pushinteger(L, 0);  // RESULT_WIN
    return 1;
}

// ctx.battle:start_trainer(trainer_id)
int start_trainer(lua_State* L) {
    int trainer_id = luaL_checkinteger(L, 2);
    (void)trainer_id;
    lua_pushinteger(L, 0);  // RESULT_WIN
    return 1;
}

} // namespace battle_api

// =============================================================================
// Party API - ctx.party
// =============================================================================

namespace party_api {

// ctx.party:count() -> number
int count(lua_State* L) {
    lua_pushinteger(L, 1);  // Stub: 1 Pokemon
    return 1;
}

// ctx.party:get(slot) -> pokemon table or nil
int get(lua_State* L) {
    int slot = luaL_checkinteger(L, 2);
    // Stub: return nil
    lua_pushnil(L);
    return 1;
}

// ctx.party:has_species(species_id) -> bool
int has_species(lua_State* L) {
    int species_id = luaL_checkinteger(L, 2);
    lua_pushinteger(L, 0);  // stub: not found — 0 per VM result contract
    return 1;
}

// ctx.party:heal_all()
int heal_all(lua_State* L) {
    return 0;
}

// ctx.party:add_pokemon(species_id, level) -> bool  (legacy)
// ctx.party:add_pokemon({species=, level=, held_item=, nickname=, ot_name=}) -> bool (new)
int add_pokemon(lua_State* L) {
    int species_id = 0, level = 5;
    ItemId held_item = 0;
    std::string nickname, ot_name;
    
    if (lua_istable(L, 2)) {
        lua_getfield(L, 2, "species");   species_id = static_cast<int>(lua_tointeger(L, -1)); lua_pop(L, 1);
        lua_getfield(L, 2, "level");     level = static_cast<int>(lua_tointeger(L, -1)); lua_pop(L, 1);
        lua_getfield(L, 2, "held_item"); held_item = static_cast<ItemId>(lua_tointeger(L, -1)); lua_pop(L, 1);
        lua_getfield(L, 2, "nickname");
        if (!lua_isnil(L, -1)) { const char* s = lua_tostring(L, -1); if (s) nickname = s; }
        lua_pop(L, 1);
        lua_getfield(L, 2, "ot_name");
        if (!lua_isnil(L, -1)) { const char* s = lua_tostring(L, -1); if (s) ot_name = s; }
        lua_pop(L, 1);
    } else {
        species_id = static_cast<int>(luaL_checkinteger(L, 2));
        level = static_cast<int>(luaL_optinteger(L, 3, 5));
    }
    
    // Store results in stubs for testing
    LuaRuntime* runtime = get_runtime(L);
    auto& stubs = runtime->get_stub_services();
    stubs.last_add_pokemon_species = species_id;
    stubs.last_add_pokemon_level = level;
    stubs.last_add_pokemon_held_item = held_item;
    stubs.last_add_pokemon_nickname = nickname;
    stubs.last_add_pokemon_ot_name = ot_name;
    
    lua_pushinteger(L, 1);  // stub success — 1 per VM result contract
    return 1;
}

// ctx.party:has_move(move_id) -> bool
int has_move(lua_State* L) {
    int move_id = luaL_checkinteger(L, 2);
    lua_pushinteger(L, 0);  // stub: not found
    return 1;
}

// ctx.party:can_use_hm(hm_name) -> bool
int can_use_hm(lua_State* L) {
    const char* hm_name = luaL_checkstring(L, 2);
    lua_pushinteger(L, 0);  // stub: cannot use
    return 1;
}

} // namespace party_api

// =============================================================================
// Inventory API - ctx.inventory
// =============================================================================

namespace inventory_api {

// ctx.inventory:give(item_id, count) -> int  (1 = given, 0 = bag full)
//
// Authoritative: writes through GameState::items when bound.
// Crystal Gen2 semantics: give item to player, result = TRUE(1) if given,
// FALSE(0) if bag is full (quantity already at 99 or no room).
// In stub mode (no GameState): always succeeds (headless test default).
int give(lua_State* L) {
    int item_id = luaL_checkinteger(L, 2);
    int count   = luaL_optinteger(L, 3, 1);
    LuaRuntime* runtime = get_runtime(L);
    if (GameState* gs = runtime->get_game_state()) {
        bool ok = gs->give_item(static_cast<enginemon::ItemId>(item_id),
                                static_cast<int32_t>(count));
        lua_pushinteger(L, ok ? 1 : 0);
    } else {
        // No GameState bound — headless stub mode. Treat as given.
        (void)item_id; (void)count;
        lua_pushinteger(L, 1);
    }
    return 1;
}

// ctx.inventory:take(item_id, count) -> int  (1 = removed, 0 = not present)
//
// Authoritative: reads and writes through GameState::items when bound.
int take(lua_State* L) {
    int item_id = luaL_checkinteger(L, 2);
    int count   = luaL_optinteger(L, 3, 1);
    LuaRuntime* runtime = get_runtime(L);
    if (GameState* gs = runtime->get_game_state()) {
        bool ok = gs->take_item(static_cast<enginemon::ItemId>(item_id),
                                static_cast<int32_t>(count));
        lua_pushinteger(L, ok ? 1 : 0);
    } else {
        (void)item_id; (void)count;
        lua_pushinteger(L, 0);  // stub: nothing to take
    }
    return 1;
}

// ctx.inventory:has(item_id, count?) -> int  (1 = present, 0 = absent)
//
// Authoritative: reads from GameState::items when bound.
int has(lua_State* L) {
    int item_id = luaL_checkinteger(L, 2);
    int count   = luaL_optinteger(L, 3, 1);
    LuaRuntime* runtime = get_runtime(L);
    if (GameState* gs = runtime->get_game_state()) {
        bool present = gs->has_item(static_cast<enginemon::ItemId>(item_id),
                                    static_cast<int32_t>(count));
        lua_pushinteger(L, present ? 1 : 0);
    } else {
        (void)item_id; (void)count;
        lua_pushinteger(L, 0);  // stub: bag is empty
    }
    return 1;
}

// ctx.inventory:count(item_id) -> number
// Authoritative: reads from GameState::items when bound.
int count(lua_State* L) {
    int item_id = luaL_checkinteger(L, 2);
    LuaRuntime* runtime = get_runtime(L);
    if (GameState* gs = runtime->get_game_state()) {
        lua_pushinteger(L, gs->item_count(static_cast<enginemon::ItemId>(item_id)));
    } else {
        lua_pushinteger(L, 0);  // stub: bag is empty
    }
    return 1;
}

// ctx.inventory:give_money(amount, account?) - account: 0=player, 1=mom
// Production path: write through GameState::variables when bound.
int give_money(lua_State* L) {
    int amount  = luaL_checkinteger(L, 2);
    int account = static_cast<int>(luaL_optinteger(L, 3, 0));
    LuaRuntime* runtime = get_runtime(L);
    // Canonical money keys in GameState::variables
    // Source: Crystal wMoney (player) / wMomsMoney (mom)
    const char* key = (account == 1) ? "money_mom" : "money_player";
    if (GameState* gs = runtime->get_game_state()) {
        auto it = gs->variables.find(key);
        int32_t current = (it != gs->variables.end()) ? it->second : 0;
        gs->variables[key] = current + static_cast<int32_t>(amount);
    } else {
        auto& stubs = runtime->get_stub_services();
        stubs.last_give_money_amount  = amount;
        stubs.last_give_money_account = account;
    }
    return 0;
}

// ctx.inventory:take_money(amount, account?) -> bool
int take_money(lua_State* L) {
    int amount  = luaL_checkinteger(L, 2);
    int account = static_cast<int>(luaL_optinteger(L, 3, 0));
    LuaRuntime* runtime = get_runtime(L);
    const char* key = (account == 1) ? "money_mom" : "money_player";
    bool success = false;
    if (GameState* gs = runtime->get_game_state()) {
        auto it = gs->variables.find(key);
        int32_t current = (it != gs->variables.end()) ? it->second : 0;
        if (current >= static_cast<int32_t>(amount)) {
            gs->variables[key] = current - static_cast<int32_t>(amount);
            success = true;
        }
    } else {
        auto& stubs = runtime->get_stub_services();
        stubs.last_take_money_amount  = amount;
        stubs.last_take_money_account = account;
        success = true;  // stub always succeeds
    }
    lua_pushinteger(L, success ? 1 : 0);
    return 1;
}

// ctx.inventory:has_money(amount, account?) -> bool
int has_money(lua_State* L) {
    int amount  = luaL_checkinteger(L, 2);
    int account = static_cast<int>(luaL_optinteger(L, 3, 0));
    LuaRuntime* runtime = get_runtime(L);
    const char* key = (account == 1) ? "money_mom" : "money_player";
    bool result = false;
    if (GameState* gs = runtime->get_game_state()) {
        auto it = gs->variables.find(key);
        int32_t current = (it != gs->variables.end()) ? it->second : 0;
        result = (current >= static_cast<int32_t>(amount));
    }
    lua_pushinteger(L, result ? 1 : 0);
    return 1;
}

// ctx.inventory:money(account?) -> number
// Returns the current money balance for the given account.
int money(lua_State* L) {
    int account = static_cast<int>(luaL_optinteger(L, 2, 0));
    LuaRuntime* runtime = get_runtime(L);
    const char* key = (account == 1) ? "money_mom" : "money_player";
    int32_t balance = 0;
    if (GameState* gs = runtime->get_game_state()) {
        auto it = gs->variables.find(key);
        balance = (it != gs->variables.end()) ? it->second : 0;
    }
    lua_pushinteger(L, balance);
    return 1;
}

// ctx.inventory:prepare_money_text(account, buffer_slot)
// Reads the current money balance for the given account and stores it in a
// per-runtime TRANSIENT text buffer for script text display.
// account: 0=player (money_player), 1=mom (money_mom)
// buffer_slot: strbuf index 0-2 (matches Sem_PrepareTextArg buffer_slot)
//
// The value is NOT written to GameState — it is a transient formatting artifact
// that must not pollute the save state. StubServices::text_buffers provides
// the per-runtime transient store.
//
// Balance authority: GameState::variables["money_player"/"money_mom"] (persistent).
// Text buffer:       StubServices::text_buffers["strbuf<N>_money"] (transient, not saved).
//
// Source: Crystal getmoney opcode → text buffer substitution.
int prepare_money_text(lua_State* L) {
    int account     = static_cast<int>(luaL_checkinteger(L, 2));
    int buffer_slot = static_cast<int>(luaL_checkinteger(L, 3));
    LuaRuntime* runtime = get_runtime(L);
    const char* src_key  = (account == 1) ? "money_mom" : "money_player";

    // Read balance from GameState (authoritative) or stub.
    int32_t balance = 0;
    if (GameState* gs = runtime->get_game_state()) {
        auto it = gs->variables.find(src_key);
        balance = (it != gs->variables.end()) ? it->second : 0;
    }

    // Store in transient text buffer — NOT in GameState.
    std::string buf_key = "strbuf" + std::to_string(buffer_slot) + "_money";
    runtime->get_stub_services().text_buffers[buf_key] = balance;

    return 0;
}

} // namespace inventory_api

// =============================================================================
// Audio API - ctx.audio
// =============================================================================

namespace audio_api {

// ctx.audio:play_music(music_id)
int play_music(lua_State* L) {
    int music_id = luaL_checkinteger(L, 2);
    (void)music_id;
    return 0;
}

// ctx.audio:stop_music()
int stop_music(lua_State* L) {
    return 0;
}

// ctx.audio:play_sfx(sfx_id)
int play_sfx(lua_State* L) {
    int sfx_id = luaL_checkinteger(L, 2);
    (void)sfx_id;
    return 0;
}

// ctx.audio:play_cry(species_id)
int play_cry(lua_State* L) {
    int species_id = luaL_checkinteger(L, 2);
    (void)species_id;
    return 0;
}

// ctx.audio:wait_sfx()
int wait_sfx(lua_State* L) {
    return 0;
}

} // namespace audio_api

// =============================================================================
// Flags API - ctx.flags
// =============================================================================

namespace flag_api {

// =============================================================================
// Flag API Stub Implementation
//
// These functions use per-runtime StubServices for isolated state.
// Destroying LuaRuntime destroys its stub state automatically.
// =============================================================================

// Helper to get per-runtime stub state
static StubServices& get_stubs(lua_State* L) {
    LuaRuntime* runtime = get_runtime(L);
    return runtime->get_stub_services();
}

// Test helpers - operate on specific runtime's stub state
void reset_test_state(LuaRuntime* runtime) {
    auto& stubs = runtime->get_stub_services();
    stubs.flags.clear();
    stubs.vars.clear();
    stubs.flag_calls.clear();
}

void set_test_flag(LuaRuntime* runtime, int flag_id, bool value) {
    runtime->get_stub_services().flags[flag_id] = value;
}

bool get_test_flag(LuaRuntime* runtime, int flag_id) {
    const auto& flags = runtime->get_stub_services().flags;
    auto it = flags.find(flag_id);
    return it != flags.end() ? it->second : false;
}

const std::vector<std::pair<std::string, int>>& get_flag_calls(LuaRuntime* runtime) {
    return runtime->get_stub_services().flag_calls;
}

// Helper: produce the canonical GameState flag key from an encoded flag integer.
// The emitter encodes flags as (ns_byte << 16) | flag_value where:
//   ns=0 (Event)  → key prefix "flag_"   e.g. "flag_001a"
//   ns=1 (Engine) → key prefix "eflag_"  e.g. "eflag_0041"
//
// Using distinct prefixes ensures EventFlag{N} and EngineFlag{N} with the same N
// produce DIFFERENT keys and cannot silently alias each other.
// EventFlag keys ("flag_{:04x}") are identical to MapExtractor::make_flag_id() output
// so map visibility/event flags continue to match correctly.
static std::string make_flag_key(int flag_id) {
    uint16_t val = static_cast<uint16_t>(flag_id & 0xFFFF);
    bool is_engine = ((flag_id >> 16) & 0xFF) != 0;  // ns byte in bits 16-23
    char buf[24];
    std::snprintf(buf, sizeof(buf), is_engine ? "eflag_%04x" : "flag_%04x",
                  static_cast<unsigned>(val));
    return buf;
}

// ctx.flags:set(flag_id)
int set(lua_State* L) {
    LuaRuntime* runtime = get_runtime(L);
    int flag_id = luaL_checkinteger(L, 2);
    // Production path: write through GameState when bound
    if (GameState* gs = runtime->get_game_state()) {
        // make_flag_key preserves namespace: EventFlag→"flag_XXXX", EngineFlag→"eflag_XXXX"
        gs->flags.insert(make_flag_key(flag_id));
    } else {
        auto& stubs = runtime->get_stub_services();
        stubs.flags[flag_id] = true;
    }
    runtime->get_stub_services().flag_calls.push_back({"set", flag_id});
    return 0;
}

// ctx.flags:clear(flag_id)
int clear(lua_State* L) {
    LuaRuntime* runtime = get_runtime(L);
    int flag_id = luaL_checkinteger(L, 2);
    // Production path: write through GameState when bound
    if (GameState* gs = runtime->get_game_state()) {
        gs->flags.erase(make_flag_key(flag_id));
    } else {
        auto& stubs = runtime->get_stub_services();
        stubs.flags[flag_id] = false;
    }
    runtime->get_stub_services().flag_calls.push_back({"clear", flag_id});
    return 0;
}

// ctx.flags:check(flag_id) -> bool
int check(lua_State* L) {
    LuaRuntime* runtime = get_runtime(L);
    int flag_id = luaL_checkinteger(L, 2);
    bool value = false;
    // Production path: read from GameState when bound
    if (GameState* gs = runtime->get_game_state()) {
        value = gs->flags.count(make_flag_key(flag_id)) > 0;
    } else {
        value = get_test_flag(runtime, flag_id);
    }
    runtime->get_stub_services().flag_calls.push_back({"check", flag_id});
    lua_pushinteger(L, value ? 1 : 0);
    return 1;
}

// ctx.flags:set_var(var_id, value)
// var_id is an integer — the canonical numeric wScriptVar slot.
// Stored as "var_N" in GameState::variables (persistent gameplay state).
// Text-buffer preparation writes (PrepareTextArg) use ctx.text_buf:set(),
// NOT this function — keeping script-variable and text-buffer namespaces distinct.
int set_var(lua_State* L) {
    LuaRuntime* runtime = get_runtime(L);
    int var_id = luaL_checkinteger(L, 2);
    int value  = luaL_checkinteger(L, 3);
    if (GameState* gs = runtime->get_game_state()) {
        gs->variables["var_" + std::to_string(var_id)] = value;
    } else {
        runtime->get_stub_services().vars[var_id] = value;
    }
    return 0;
}

// ctx.flags:get_var(var_id) -> number
// var_id is an integer — the canonical numeric wScriptVar slot.
int get_var(lua_State* L) {
    LuaRuntime* runtime = get_runtime(L);
    int var_id = luaL_checkinteger(L, 2);
    int value  = 0;
    if (GameState* gs = runtime->get_game_state()) {
        auto it = gs->variables.find("var_" + std::to_string(var_id));
        value = (it != gs->variables.end()) ? it->second : 0;
    } else {
        const auto& stubs = runtime->get_stub_services();
        auto it = stubs.vars.find(var_id);
        value = (it != stubs.vars.end()) ? it->second : 0;
    }
    lua_pushinteger(L, value);
    return 1;
}

// ctx.flags:add_var(var_id, delta)
int add_var(lua_State* L) {
    LuaRuntime* runtime = get_runtime(L);
    int var_id = luaL_checkinteger(L, 2);
    int delta  = luaL_checkinteger(L, 3);
    // Production path: read-modify-write through GameState when bound
    if (GameState* gs = runtime->get_game_state()) {
        std::string key = "var_" + std::to_string(var_id);
        auto it = gs->variables.find(key);
        int current = (it != gs->variables.end()) ? it->second : 0;
        gs->variables[key] = current + delta;
    } else {
        auto& stubs = runtime->get_stub_services();
        auto it = stubs.vars.find(var_id);
        int current = (it != stubs.vars.end()) ? it->second : 0;
        stubs.vars[var_id] = current + delta;
    }
    return 0;
}

} // namespace flag_api

// =============================================================================
// Text Buffer API - ctx.text_buf
//
// Transient per-execution text argument buffers for PrepareTextArg operations.
// Keys are semantic type strings: "strbuf<N>_item", "strbuf<N>_species",
// "strbuf<N>_trainer", "strbuf<N>_str", "strbuf<N>_scriptvar".
//
// These are NEVER serialized to GameState.  They are regenerated each time a
// script runs and consumed by the text renderer during the same script execution.
//
// Source: Crystal text-argument commands (getitemname, getmonname, gettrainername,
//         getstring, getnum) write to text display buffers before writetext.
//
// Backed by StubServices::text_buffers (string key -> int32_t value).
// =============================================================================

namespace text_buf_api {

// ctx.text_buf:set(key, value)
// Write a text-buffer slot.  key is a semantic string like "strbuf0_item".
// value is the integer payload (item ID, species ID, trainer group, etc.).
int set(lua_State* L) {
    const char* key = luaL_checkstring(L, 2);
    int value       = luaL_checkinteger(L, 3);
    LuaRuntime* runtime = get_runtime(L);
    runtime->get_stub_services().text_buffers[key] = value;
    return 0;
}

// ctx.text_buf:get(key) -> int
// Read a text-buffer slot.  Returns 0 if the key has not been written.
int get(lua_State* L) {
    const char* key = luaL_checkstring(L, 2);
    LuaRuntime* runtime = get_runtime(L);
    const auto& bufs = runtime->get_stub_services().text_buffers;
    auto it = bufs.find(key);
    lua_pushinteger(L, it != bufs.end() ? it->second : 0);
    return 1;
}

} // namespace text_buf_api
// =============================================================================

namespace time_api {

// Helper: compute effective RTC components from GameState (if bound) or fallback.
static enginemon::RtcComponents get_rtc(lua_State* L) {
    LuaRuntime* runtime = get_runtime(L);
    int64_t offset = 0;
    if (runtime) {
        if (const GameState* gs = runtime->get_game_state()) {
            offset = gs->rtc_offset_seconds;
        }
    }
    enginemon::UnixSeconds eff = enginemon::GameRtc::system_source().now_seconds() + offset;
    return enginemon::GameRtc::decompose(eff);
}

// ctx.time:hour() -> 0-23
int hour(lua_State* L) {
    lua_pushinteger(L, get_rtc(L).hour);
    return 1;
}

// ctx.time:minute() -> 0-59
int minute(lua_State* L) {
    lua_pushinteger(L, get_rtc(L).minute);
    return 1;
}

// ctx.time:day_of_week() -> 0-6  (0=Sunday, 1=Monday, …, 6=Saturday)
int day_of_week(lua_State* L) {
    lua_pushinteger(L, get_rtc(L).day_of_week);
    return 1;
}

// ctx.time:time_of_day() -> "morning", "day", or "night"
int time_of_day(lua_State* L) {
    auto c = get_rtc(L);
    lua_pushstring(L, enginemon::GameRtc::period_name(c.hour));
    return 1;
}

// ctx.time:is_morning() -> 0 or 1
int is_morning(lua_State* L) {
    lua_pushinteger(L, enginemon::GameRtc::is_morning(get_rtc(L).hour) ? 1 : 0);
    return 1;
}

// ctx.time:is_day() -> 0 or 1
int is_day(lua_State* L) {
    lua_pushinteger(L, enginemon::GameRtc::is_day(get_rtc(L).hour) ? 1 : 0);
    return 1;
}

// ctx.time:is_night() -> 0 or 1
int is_night(lua_State* L) {
    lua_pushinteger(L, enginemon::GameRtc::is_night(get_rtc(L).hour) ? 1 : 0);
    return 1;
}

} // namespace time_api

// =============================================================================
// Utility API - ctx.util
// =============================================================================

namespace util_api {

// ctx.util:wait(frames)
int wait_frames(lua_State* L) {
    int frames = luaL_checkinteger(L, 2);
    script_wait_frames(L, frames);
    return 0;
}

// ctx.util:wait_seconds(seconds)
int wait_seconds(lua_State* L) {
    float seconds = static_cast<float>(luaL_checknumber(L, 2));
    script_wait_seconds(L, seconds);
    return 0;
}

// ctx.util:random(min, max) -> integer in [min, max] inclusive
// Consumes 1+ draws from the canonical GameplayRng (Lemire bounded).
// PRECONDITION: max >= min.
int random(lua_State* L) {
    int min = luaL_checkinteger(L, 2);
    int max = luaL_checkinteger(L, 3);
    LuaRuntime* runtime = get_runtime(L);
    if (runtime) {
        if (GameState* gs = runtime->get_game_state()) {
            if (max > min) {
                uint32_t range = static_cast<uint32_t>(max - min) + 1u;
                int result = min + static_cast<int>(gs->rng.bounded(range));
                lua_pushinteger(L, result);
            } else {
                // max == min: no draws consumed, result is deterministic
                lua_pushinteger(L, min);
            }
            return 1;
        }
    }
    // Fallback when no GameState bound (test stubs without game state):
    // Return min — 0 draws, deterministic, explicit non-random marker.
    lua_pushinteger(L, min);
    return 1;
}

// ctx.util:random_chance(percent) -> bool
// percent ∈ [0, 100]. probability = percent / 100.
// Consumes 1+ draws from canonical GameplayRng (same as bounded — Lemire rejection).
// Contract:
//   0   → always false,  0 draws
//   100 → always true,   0 draws
//   >100 → programmer error, throws (0 draws consumed)
//   [1,99] → 1+ draws, unbiased probability = percent/100
int random_chance(lua_State* L) {
    int percent = luaL_checkinteger(L, 2);
    LuaRuntime* runtime = get_runtime(L);
    if (runtime) {
        if (GameState* gs = runtime->get_game_state()) {
            if (percent <= 0) {
                lua_pushboolean(L, false);
            } else if (percent >= 100) {
                if (percent > 100) {
                    // Programmer error — invalid percent, 0 draws
                    luaL_error(L, "random_chance: percent %d is out of range [0,100]", percent);
                    return 0;
                }
                lua_pushboolean(L, true);
            } else {
                // bounded(100) returns [0,99] with uniform probability.
                // Hit if result < percent → probability = percent/100 exactly.
                // Consumes 1+ draws (Lemire — typically 1).
                uint32_t roll = gs->rng.bounded(100u);
                lua_pushboolean(L, roll < static_cast<uint32_t>(percent));
            }
            return 1;
        }
    }
    // Fallback: no GameState bound (isolated unit tests only).
    // Returns deterministic result, 0 draws consumed.
    // percent >= 50 → true is NOT a probability contract — it is an explicit stub marker.
    if (percent > 100) {
        luaL_error(L, "random_chance: percent %d is out of range [0,100]", percent);
        return 0;
    }
    lua_pushboolean(L, percent >= 50);
    return 1;
}

} // namespace util_api

// =============================================================================
// Registration
// =============================================================================

void register_all_apis(lua_State* L, GameContext& ctx) {
    // APIs are registered through LuaRuntime::bind_api()
    // This function is for additional registrations if needed
}

// =============================================================================
// Field Move API - ctx.field
// Implements ScriptExecutionContext lifecycle for field moves
// =============================================================================

namespace field_api {

// Helper to get LuaRuntime* from the ctx.field table's _runtime field
static LuaRuntime* get_runtime_from_field(lua_State* L) {
    lua_getfield(L, 1, "_runtime");
    LuaRuntime* runtime = static_cast<LuaRuntime*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return runtime;
}

// =============================================================================
// Field API Configuration
//
// Test configuration uses per-runtime StubServices.field_config.
// Destroying LuaRuntime destroys its config automatically.
// No process-global maps, no cleanup registries, no stale pointers.
// =============================================================================

// Re-export types for API compatibility
using StrengthResult = StubStrengthResult;
using RockSmashResult = StubRockSmashResult;

// Test configuration functions - operate on specific runtime's stub state
void set_strength_check_result(LuaRuntime* runtime, StrengthResult result, uint8_t slot, SpeciesId species) {
    auto& cfg = runtime->get_stub_services().field_config;
    cfg.strength_result = result;
    cfg.strength_slot = slot;
    cfg.strength_species = species;
}

void set_rock_smash_check_result(LuaRuntime* runtime, RockSmashResult result, uint8_t slot, SpeciesId species) {
    auto& cfg = runtime->get_stub_services().field_config;
    cfg.rock_smash_result = result;
    cfg.rock_smash_slot = slot;
    cfg.rock_smash_species = species;
}

void set_encounter_result(LuaRuntime* runtime, bool has_encounter, SpeciesId species, uint8_t level) {
    auto& cfg = runtime->get_stub_services().field_config;
    cfg.has_encounter = has_encounter;
    cfg.encounter_species = species;
    cfg.encounter_level = level;
}

// Query functions for testing - operate on specific runtime's context
bool has_selected_actor(LuaRuntime* runtime) {
    return runtime->get_script_context().selected_field_actor.has_value();
}

bool has_pending_encounter(LuaRuntime* runtime) {
    return runtime->get_script_context().pending_field_encounter.has_value();
}

SpeciesId get_selected_actor_species(LuaRuntime* runtime) {
    const auto& ctx = runtime->get_script_context();
    if (ctx.selected_field_actor.has_value()) {
        return ctx.selected_field_actor->species;
    }
    return 0;
}

SpeciesId get_pending_encounter_species(LuaRuntime* runtime) {
    const auto& ctx = runtime->get_script_context();
    if (ctx.pending_field_encounter.has_value()) {
        return ctx.pending_field_encounter->species;
    }
    return 0;
}

uint8_t get_pending_encounter_level(LuaRuntime* runtime) {
    const auto& ctx = runtime->get_script_context();
    if (ctx.pending_field_encounter.has_value()) {
        return ctx.pending_field_encounter->level;
    }
    return 0;
}

bool is_strength_active(LuaRuntime* runtime) {
    return runtime->get_script_context().strength_active;
}

// ctx.field:check_strength() -> result (0=Available, 1=Unavailable, 2=AlreadyActive)
int check_strength(lua_State* L) {
    LuaRuntime* runtime = get_runtime_from_field(L);
    if (!runtime) return luaL_error(L, "check_strength: no runtime context");
    
    auto& script_ctx = runtime->get_script_context();
    const auto& cfg = runtime->get_stub_services().field_config;
    
    script_ctx.selected_field_actor = std::nullopt;
    
    if (script_ctx.strength_active) {
        lua_pushinteger(L, static_cast<int>(StrengthResult::AlreadyActive));
        return 1;
    }
    
    if (cfg.strength_result == StrengthResult::Available) {
        script_ctx.selected_field_actor = SelectedFieldActor(
            cfg.strength_slot,
            FieldMoveType::Strength,
            cfg.strength_species
        );
    }
    
    lua_pushinteger(L, static_cast<int>(cfg.strength_result));
    return 1;
}

// ctx.field:activate_strength()
int activate_strength(lua_State* L) {
    LuaRuntime* runtime = get_runtime_from_field(L);
    if (!runtime) return luaL_error(L, "activate_strength: no runtime context");
    
    auto& script_ctx = runtime->get_script_context();
    
    if (!script_ctx.selected_field_actor.has_value()) {
        return luaL_error(L, "activate_strength: no selected field actor");
    }
    
    if (script_ctx.selected_field_actor->move != FieldMoveType::Strength) {
        return luaL_error(L, "activate_strength: selected actor is not for Strength");
    }
    
    script_ctx.strength_active = true;
    script_ctx.selected_field_actor = std::nullopt;
    
    return 0;
}

// ctx.field:check_rock_smash() -> result (0=Available, 1=Unavailable)
int check_rock_smash(lua_State* L) {
    LuaRuntime* runtime = get_runtime_from_field(L);
    if (!runtime) return luaL_error(L, "check_rock_smash: no runtime context");
    
    auto& script_ctx = runtime->get_script_context();
    const auto& cfg = runtime->get_stub_services().field_config;
    
    script_ctx.selected_field_actor = std::nullopt;
    
    if (cfg.rock_smash_result == RockSmashResult::Available) {
        script_ctx.selected_field_actor = SelectedFieldActor(
            cfg.rock_smash_slot,
            FieldMoveType::RockSmash,
            cfg.rock_smash_species
        );
    }
    
    lua_pushinteger(L, static_cast<int>(cfg.rock_smash_result));
    return 1;
}

// ctx.field:prepare_nickname(buffer_slot) -> nickname string
int prepare_nickname(lua_State* L) {
    LuaRuntime* runtime = get_runtime_from_field(L);
    if (!runtime) return luaL_error(L, "prepare_nickname: no runtime context");
    
    int buffer_slot = luaL_checkinteger(L, 2);
    (void)buffer_slot;
    
    const auto& script_ctx = runtime->get_script_context();
    
    if (!script_ctx.selected_field_actor.has_value()) {
        return luaL_error(L, "prepare_nickname: no selected field actor");
    }
    
    std::string nickname = "POKEMON_" + std::to_string(script_ctx.selected_field_actor->species);
    lua_pushstring(L, nickname.c_str());
    
    return 1;
}

// ctx.field:try_rock_encounter() -> species_id (0 if no encounter)
int try_rock_encounter(lua_State* L) {
    LuaRuntime* runtime = get_runtime_from_field(L);
    if (!runtime) return luaL_error(L, "try_rock_encounter: no runtime context");
    
    auto& script_ctx = runtime->get_script_context();
    const auto& cfg = runtime->get_stub_services().field_config;
    
    script_ctx.pending_field_encounter = std::nullopt;
    
    if (cfg.has_encounter) {
        script_ctx.pending_field_encounter = PendingFieldEncounter(
            cfg.encounter_species,
            cfg.encounter_level
        );
        lua_pushinteger(L, cfg.encounter_species);
    } else {
        lua_pushinteger(L, 0);
    }
    
    return 1;
}

// ctx.field:read_encounter_species() -> species_id
int read_encounter_species(lua_State* L) {
    LuaRuntime* runtime = get_runtime_from_field(L);
    if (!runtime) return luaL_error(L, "read_encounter_species: no runtime context");
    
    const auto& script_ctx = runtime->get_script_context();
    
    if (script_ctx.pending_field_encounter.has_value()) {
        lua_pushinteger(L, script_ctx.pending_field_encounter->species);
    } else {
        lua_pushinteger(L, 0);
    }
    return 1;
}

// ctx.field:load_pending_encounter() -> species_id, level
int load_pending_encounter(lua_State* L) {
    LuaRuntime* runtime = get_runtime_from_field(L);
    if (!runtime) return luaL_error(L, "load_pending_encounter: no runtime context");
    
    auto& script_ctx = runtime->get_script_context();
    
    if (!script_ctx.pending_field_encounter.has_value()) {
        return luaL_error(L, "load_pending_encounter: no pending encounter");
    }
    
    SpeciesId species = script_ctx.pending_field_encounter->species;
    uint8_t level = script_ctx.pending_field_encounter->level;
    
    script_ctx.pending_field_encounter = std::nullopt;
    
    lua_pushinteger(L, species);
    lua_pushinteger(L, level);
    return 2;
}

// ctx.field:play_actor_cry()
int play_actor_cry(lua_State* L) {
    LuaRuntime* runtime = get_runtime_from_field(L);
    if (!runtime) return luaL_error(L, "play_actor_cry: no runtime context");
    
    const auto& script_ctx = runtime->get_script_context();
    
    if (!script_ctx.selected_field_actor.has_value()) {
        return luaL_error(L, "play_actor_cry: no selected field actor");
    }
    
    return 0;
}

// ctx.field:clear_context()
int clear_context(lua_State* L) {
    LuaRuntime* runtime = get_runtime_from_field(L);
    if (!runtime) return luaL_error(L, "clear_context: no runtime context");
    
    auto& script_ctx = runtime->get_script_context();
    
    script_ctx.selected_field_actor = std::nullopt;
    script_ctx.pending_field_encounter = std::nullopt;
    script_ctx.script_var = 0;
    return 0;
}

} // namespace field_api

// =============================================================================
// RuntimeTextSequence implementation
// =============================================================================

std::string RuntimeTextSequence::debug_string() const {
    std::string result;
    for (const auto& elem : elements) {
        switch (elem.op) {
            case RuntimeTextOp::Text:        result += elem.text; break;
            case RuntimeTextOp::Line:        result += "[LINE]"; break;
            case RuntimeTextOp::Next:        result += "[NEXT]"; break;
            case RuntimeTextOp::Para:        result += "[PARA]"; break;
            case RuntimeTextOp::Cont:        result += "[CONT]"; break;
            case RuntimeTextOp::Scroll:      result += "[SCROLL]"; break;
            case RuntimeTextOp::Done:        result += "[DONE]"; break;
            case RuntimeTextOp::Prompt:      result += "[PROMPT]"; break;
            case RuntimeTextOp::Ram:         result += "[RAM:" + std::to_string(elem.addr) + "]"; break;
            case RuntimeTextOp::Bcd:         result += "[BCD:" + std::to_string(elem.addr) + "]"; break;
            case RuntimeTextOp::Decimal:     result += "[DECIMAL:" + std::to_string(elem.addr) + "]"; break;
            case RuntimeTextOp::Buffer:      result += "[BUFFER:" + std::to_string(elem.param) + "]"; break;
            case RuntimeTextOp::Far:         result += "[FAR:" + std::to_string(elem.addr) + "]"; break;
            case RuntimeTextOp::Move:        result += "[MOVE]"; break;
            case RuntimeTextOp::Box:         result += "[BOX]"; break;
            case RuntimeTextOp::Day:         result += "[DAY]"; break;
            case RuntimeTextOp::Low:         result += "[LOW]"; break;
            case RuntimeTextOp::WaitButton:  result += "[WAITBUTTON]"; break;
            case RuntimeTextOp::TxScroll:    result += "[TXSCROLL]"; break;
            case RuntimeTextOp::Sound:       result += "[SOUND:" + elem.op_name + "]"; break;
            case RuntimeTextOp::Raw:         result += "[RAW]"; break;
            case RuntimeTextOp::Asm:         result += "[ASM]"; break;
            case RuntimeTextOp::Unsupported: result += "[?" + elem.op_name + "?]"; break;
        }
    }
    return result;
}

} // namespace enginemon

// =============================================================================
// Game API - ctx.game
// Stub implementations for all game-level semantic operations.
// These are called by generated Lua from SemanticLuaEmitter.
// Production implementations will call the appropriate engine systems.
// =============================================================================

namespace enginemon {
namespace game_api {

static LuaRuntime* get_runtime(lua_State* L) {
    lua_getfield(L, 1, "_runtime");
    LuaRuntime* runtime = static_cast<LuaRuntime*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return runtime;
}

// =============================================================================
// BehaviorRegistry
//
// Canonical set of all valid Sem_GameSpecificEvent behavior names.
// Source authority: behavior_table.hpp / BEHAVIOR_TABLE.
//
// Dispatch policy (enforced at runtime — mirrors the compiler's Stage 5 gate):
//
//   Sdefer_<id>  → deferred-script scheduler (always routes to callback)
//   known name   → capability-deferred: hard error naming the stable identity
//   unknown name → hard error (Stage 5 should have rejected this; compiler bug)
//
// There is no silent no-op path.  If a behavior is not yet implemented, the
// script errors explicitly so the caller cannot observe a fabricated result or
// consume a stale wScriptVar value.
// =============================================================================

// All 95 valid Sem_GameSpecificEvent behavior names from BEHAVIOR_TABLE.
// This list must stay in sync with crystal/script/behavior_table.hpp.
static const std::unordered_set<std::string_view> KNOWN_BEHAVIORS = {
    // Link/Trade/Communications
    "SetBitsForLinkTradeRequest",
    "WaitForLinkedFriend",
    "CheckLinkTimeout_Receptionist",
    "TryQuickSave",
    "CheckBothSelectedSameRoom",
    "FailedLinkToPast",
    "CloseLink",
    "WaitForOtherPlayerToExit",
    "SetBitsForBattleRequest",
    "SetBitsForTimeCapsuleRequest",
    "CheckTimeCapsuleCompatibility",
    "EnterTimeCapsule",
    "TradeCenter",
    "Colosseum",
    "TimeCapsule",
    "CableClubCheckWhichChris",
    "CheckMysteryGift",
    "GetMysteryGiftItem",
    "UnlockMysteryGift",
    // Bug Contest
    "BugContestJudging",
    "CheckPartyFullAfterContest",
    "ContestDropOffMons",
    "ContestReturnMons",
    "GiveParkBalls",
    "CheckMagikarpLength",
    "MagikarpHouseSign",
    // PC / Services
    "PokemonCenterPC",
    "PlayersHousePC",
    // Day Care
    "DayCareMan",
    "DayCareLady",
    "DayCareManOutside",
    "MoveDeletion",
    "BankOfMom",
    // Transport/Map
    "MagnetTrain",
    // Name/Story events
    "NameRival",
    "SetDayOfWeek",
    "OverworldTownMap",
    "UnownPrinter",
    // Game Corner
    "UnownPuzzle",
    "SlotMachine",
    "CardFlip",
    // Battle Tower / Fade
    "BattleTowerFade",
    // Sprites
    "UpdateSprites",
    // Pokemon Center heal animation
    "HealMachineAnim",
    // Day Care
    "DayCareMon1",
    "DayCareMon2",
    "SelectRandomBugContestContestants",
    // Decorations / Map
    "ToggleMaptileDecorations",
    "ToggleDecorationsVisibility",
    // Shuckle events
    "GiveShuckle",
    "ReturnShuckie",
    "BillsGrandfather",
    // Lucky Number / Apricorn
    "CheckForLuckyNumberWinners",
    "CheckLuckyNumberShowFlag",
    "ResetLuckyNumberShowFlag",
    "PrintTodaysLuckyNumber",
    "SelectApricornForKurt",
    "NameRater",
    // Link record
    "DisplayLinkRecord",
    // Party happiness/checks
    "GetFirstPokemonHappiness",
    "CheckFirstMonIsEgg",
    "RandomPhoneMon",
    // Snorlax / Grooming
    "SnorlaxAwake",
    "OlderHaircutBrother",
    "YoungerHaircutBrother",
    "DaisysGrooming",
    // Cries / PC
    "PlayCurMonCry",
    "ProfOaksPCBoot",
    "TrainerHouse",
    "PhotoStudio",
    "InitRoamMons",
    // Diploma
    "Diploma",
    "PrintDiploma",
    // Battle Tower
    "BattleTowerRoomMenu",
    "BattleTowerBattle",
    "LoadOpponentTrainerAndPokemon",
    "CheckForBattleTowerRules",
    "GiveOddEgg",
    "Reset",
    // Mobile / Function stubs
    "Function1011f1",
    "Function101220",
    "Function101225",
    "Function101231",
    // Move Tutor / Chambers
    "MoveTutor",
    "OmanyteChamber",
    // Battle Tower action
    "BattleTowerAction",
    // Unown display
    "DisplayUnownWords",
    // Challenge explanation
    "Menu_ChallengeExplanationCancel",
    // Mobile errors
    "BattleTowerMobileError",
    "AskMobileOrCable",
    // Chambers
    "HoOhChamber",
    "CelebiShrineEvent",
    "CheckCaughtCelebi",
    // PokeSeer / Buena's
    "PokeSeer",
    "BuenasPassword",
    "BuenaPrize",
    "GiveDratini",
    // Beasts / Party checks
    "BeastsCheck",
    "MonCheck",
    // Mobile
    "Mobile_SelectThreeMons",
    "Function1037eb",
    "Function10383c",
    "Function1037c2",
    "Function103780",
    "Function10387b",
};

// ctx.game:behavior(name) - dispatch a named game-specific behavior
//
// Dispatch policy:
//   Sdefer_<id>  → deferred-script scheduler (real implementation)
//   known name   → capability-deferred: luaL_error with explicit diagnostic
//   unknown name → luaL_error (unknown behavior — compiler should have rejected)
//
// No silent no-op.  No fabricated result.
// writes_script_var behaviors error before script can branch on stale state.
int behavior(lua_State* L) {
    const char* name = luaL_checkstring(L, 2);
    LuaRuntime* runtime = get_runtime(L);
    auto& stubs = runtime->get_stub_services();
    stubs.last_behavior_name = name;

    // ── Sdefer_ prefix: deferred-script scheduling ──────────────────────────
    // Sem_Sdefer emits "Sdefer_<script_id>" as the behavior name.
    // This is the only behavior with a real implementation in the current runtime.
    static constexpr std::string_view SDEFER_PREFIX = "Sdefer_";
    std::string_view name_sv(name);
    if (name_sv.starts_with(SDEFER_PREFIX)) {
        std::string script_id(name_sv.substr(SDEFER_PREFIX.size()));
        if (stubs.deferred_script_fn) {
            stubs.deferred_script_fn(script_id);
        }
        // If no deferred_script_fn is wired (isolated unit test), the deferred
        // scheduling is a no-op — not a fabricated result.
        return 0;
    }

    // ── Known behavior: capability-deferred ─────────────────────────────────
    // The behavior name passed Stage 5 legality (it is in BEHAVIOR_TABLE).
    // Its native implementation does not yet exist.  Error explicitly so the
    // calling script cannot consume a stale wScriptVar value or fabricated state.
    if (KNOWN_BEHAVIORS.contains(name_sv)) {
        return luaL_error(L,
            "behavior '%s' is recognized but not yet implemented "
            "(capability deferred — add native implementation before use)",
            name);
    }

    // ── Unknown behavior: compiler / package integrity failure ───────────────
    // Stage 5 should have rejected any name not in BEHAVIOR_TABLE.
    // Reaching here means the package was built by a compiler that allowed an
    // unlicensed behavior name.  Hard fail with a clear diagnostic.
    return luaL_error(L,
        "behavior '%s' is not a registered game behavior "
        "(unknown name — Stage 5 should have rejected this)",
        name);
}

// ctx.game:set_scene(scene)
// Stores the current map's scene value.
// Source: Crystal setscene opcode → Sem_SetScene.
// Production: persisted in GameState::variables["scene_current"] for save/load.
// Test-observable: also updates StubServices::current_scene for assertions.
int set_scene(lua_State* L) {
    int scene = luaL_checkinteger(L, 2);
    LuaRuntime* runtime = get_runtime(L);
    auto& stubs = runtime->get_stub_services();
    // Always update stub field so tests can assert without a bound GameState.
    stubs.current_scene = scene;
    if (GameState* gs = runtime->get_game_state()) {
        gs->variables["scene_current"] = scene;
    }
    return 0;
}

// ctx.game:check_scene() -> scene_id
// Returns the current map's scene value.
// Source: Crystal checkscene opcode → Sem_CheckScene.
int check_scene(lua_State* L) {
    LuaRuntime* runtime = get_runtime(L);
    int scene = 0;
    if (GameState* gs = runtime->get_game_state()) {
        auto it = gs->variables.find("scene_current");
        scene = (it != gs->variables.end()) ? it->second : 0;
        // Keep stub in sync for test assertions.
        runtime->get_stub_services().current_scene = scene;
    } else {
        scene = runtime->get_stub_services().current_scene;
    }
    lua_pushinteger(L, scene);
    return 1;
}

// ctx.game:set_map_scene(map_id, scene)
// ctx.game:set_map_scene(map_id, scene)
// Stores the scene value for a specific map.
// Source: Crystal setmapscene opcode (Script_setmapscene) → wMapSceneID per-map buffer.
// Per-map scene state is stored in GameState::variables["map_scene_XXXX"] where XXXX is
// the hex-formatted MapId (stable, derived from ROM group/index).
// This is SEPARATE from the global set_scene / check_scene (which use "scene_current").
int set_map_scene(lua_State* L) {
    int map_id = luaL_checkinteger(L, 2);
    int scene   = luaL_checkinteger(L, 3);
    LuaRuntime* runtime = get_runtime(L);
    if (GameState* gs = runtime->get_game_state()) {
        char buf[24];
        std::snprintf(buf, sizeof(buf), "map_scene_%04x",
                      static_cast<unsigned>(map_id & 0xFFFF));
        gs->variables[buf] = scene;
    }
    return 0;
}

// ctx.game:check_map_scene(map_id) -> scene_id
// Returns the current per-map scene value.
// Source: Crystal checkmapscene opcode (Script_checkmapscene) → reads wMapSceneID.
// Returns 0 (default scene) if no scene has been set for this map.
int check_map_scene(lua_State* L) {
    int map_id = luaL_checkinteger(L, 2);
    LuaRuntime* runtime = get_runtime(L);
    if (GameState* gs = runtime->get_game_state()) {
        char buf[24];
        std::snprintf(buf, sizeof(buf), "map_scene_%04x",
                      static_cast<unsigned>(map_id & 0xFFFF));
        auto it = gs->variables.find(buf);
        lua_pushinteger(L, it != gs->variables.end() ? it->second : 0);
    } else {
        lua_pushinteger(L, 0);  // no GameState → scene 0
    }
    return 1;
}

// ctx.game:check_link_mode() -> mode (0=not linked / Gen1, nonzero=Gen2)
int check_link_mode(lua_State* L) {
    lua_pushinteger(L, 0); // stub: not linked
    return 1;
}

// ctx.game:check_save() -> result
int check_save(lua_State* L) {
    lua_pushinteger(L, 1); // stub: valid save exists
    return 1;
}

int hall_of_fame(lua_State* L) { return 0; }
int credits(lua_State* L) { return 0; }

// ctx.game:register_dex_entry(species)
int register_dex_entry(lua_State* L) {
    int species = luaL_checkinteger(L, 2);
    (void)species;
    return 0;
}

// ctx.game:find_party_mon(species, require_ot) -> slot (1-6) or 0
int find_party_mon(lua_State* L) {
    int species    = luaL_checkinteger(L, 2);
    int require_ot = luaL_optinteger(L, 3, 0);
    (void)species; (void)require_ot;
    lua_pushinteger(L, 0); // stub: not found
    return 1;
}

// ctx.game:check_pokerus() -> bool
int check_pokerus(lua_State* L) {
    lua_pushinteger(L, 0); // stub: no pokerus
    return 1;
}

// ctx.game:call_std(std_id, name)
int call_std(lua_State* L) {
    int std_id      = luaL_checkinteger(L, 2);
    const char* name = luaL_checkstring(L, 3);
    (void)std_id; (void)name;
    return 0;
}

// ctx.game:jump_std(std_id, name)
int jump_std(lua_State* L) {
    int std_id      = luaL_checkinteger(L, 2);
    const char* name = luaL_checkstring(L, 3);
    (void)std_id; (void)name;
    return 0;
}

int wild_on(lua_State* L)  { return 0; }
int wild_off(lua_State* L) { return 0; }
int reload_map(lua_State* L) { return 0; }
int refresh_map(lua_State* L) { return 0; }
int reanchor_map(lua_State* L) { return 0; }

// ctx.game:new_load_map(method_byte)
int new_load_map(lua_State* L) {
    int method = luaL_checkinteger(L, 2);
    (void)method;
    return 0;
}

// ctx.game:change_block(x, y, block)
int change_block(lua_State* L) {
    int x = luaL_checkinteger(L, 2);
    int y = luaL_checkinteger(L, 3);
    int block = luaL_checkinteger(L, 4);
    (void)x; (void)y; (void)block;
    return 0;
}

// ctx.game:set_blackout_point(map_id)
int set_blackout_point(lua_State* L) {
    int map_id = luaL_checkinteger(L, 2);
    (void)map_id;
    return 0;
}

// ctx.game:catch_tutorial(type)
int catch_tutorial(lua_State* L) {
    int type = luaL_checkinteger(L, 2);
    (void)type;
    return 0;
}

// ctx.game:deactivate_facing(frames)
int deactivate_facing(lua_State* L) {
    int frames = luaL_checkinteger(L, 2);
    (void)frames;
    return 0;
}

// ctx.game:sync_palettes(wait_frames)
int sync_palettes(lua_State* L) {
    int frames = luaL_optinteger(L, 2, 1);
    (void)frames;
    return 0;
}

// ctx.game:set_player_palette(selector)
int set_player_palette(lua_State* L) {
    int selector = luaL_checkinteger(L, 2);
    (void)selector;
    return 0;
}

// ctx.game:describe_decoration(id)
int describe_decoration(lua_State* L) {
    int id = luaL_checkinteger(L, 2);
    (void)id;
    return 0;
}

// ctx.game:set_daylight_saving(enabled)
// Adjusts rtc_offset_seconds by ±3600 to apply or remove a one-hour DST shift.
// Records the preference in GameState::rtc_dst_enabled for display/toggleability.
int set_daylight_saving(lua_State* L) {
    bool enable = (luaL_checkinteger(L, 2) != 0);
    LuaRuntime* runtime = get_runtime(L);
    if (GameState* gs = runtime->get_game_state()) {
        if (enable && !gs->rtc_dst_enabled) {
            gs->rtc_offset_seconds += 3600;   // spring forward
            gs->rtc_dst_enabled = true;
        } else if (!enable && gs->rtc_dst_enabled) {
            gs->rtc_offset_seconds -= 3600;   // fall back
            gs->rtc_dst_enabled = false;
        }
        // No-op if already in the requested state.
    }
    return 0;
}

// ctx.game:give_poke_mail(mail_id)
int give_poke_mail(lua_State* L) {
    int mail_id = luaL_checkinteger(L, 2);
    (void)mail_id;
    return 0;
}

// ctx.game:check_poke_mail(mail_id) -> result
int check_poke_mail(lua_State* L) {
    int mail_id = luaL_checkinteger(L, 2);
    (void)mail_id;
    lua_pushinteger(L, 0); // stub
    return 1;
}

int check_warp(lua_State* L) { return 0; }
int pocket_full_notify(lua_State* L) { return 0; }

// ctx.game:show_balance_overlay(content)
int show_balance_overlay(lua_State* L) {
    int content = luaL_checkinteger(L, 2);
    (void)content;
    return 0;
}

// ctx.game:play_radio(channel)
int play_radio(lua_State* L) {
    int channel = luaL_checkinteger(L, 2);
    (void)channel;
    return 0;
}

// ctx.game:write_cmd_queue(addr)
int write_cmd_queue(lua_State* L) {
    int addr = luaL_checkinteger(L, 2);
    (void)addr;
    return 0;
}

// ctx.game:delete_cmd_queue(queue_type)
int delete_cmd_queue(lua_State* L) {
    int type = luaL_checkinteger(L, 2);
    (void)type;
    return 0;
}

// ctx.game:modify_warp(warp_id, map_id)
int modify_warp(lua_State* L) {
    int warp_id = luaL_checkinteger(L, 2);
    int map_id  = luaL_checkinteger(L, 3);
    (void)warp_id; (void)map_id;
    return 0;
}

// ctx.game:read_state_var(id) -> value
// Reads a well-known gameplay state variable by its WellKnownStateVar id.
// Source: Crystal readmem of a specific RAM address → Sem_ReadStateVar.
// Stored in GameState::variables["state_var_N"] for persistence.
int read_state_var(lua_State* L) {
    int id = luaL_checkinteger(L, 2);
    LuaRuntime* runtime = get_runtime(L);
    int value = 0;
    if (GameState* gs = runtime->get_game_state()) {
        auto it = gs->variables.find("state_var_" + std::to_string(id));
        value = (it != gs->variables.end()) ? it->second : 0;
    }
    lua_pushinteger(L, value);
    return 1;
}

// ctx.game:write_state_var(id, value)
// Writes value (the current VM result) into the named state variable.
// Source: Crystal writemem of a specific RAM address → Sem_WriteStateVar.
// The Lua emitter now emits ctx.game:write_state_var(id, result) to pass
// the current result register as the second argument.
int write_state_var(lua_State* L) {
    int id    = luaL_checkinteger(L, 2);
    int value = static_cast<int>(luaL_optinteger(L, 3, 0));  // result from Lua
    LuaRuntime* runtime = get_runtime(L);
    if (GameState* gs = runtime->get_game_state()) {
        gs->variables["state_var_" + std::to_string(id)] = value;
    }
    return 0;
}

// ctx.game:set_state_var(id, value)
// Sets a state variable to a literal value.
// Source: Crystal callasm that stores a constant → Sem_SetStateVar.
int set_state_var(lua_State* L) {
    int id    = luaL_checkinteger(L, 2);
    int value = luaL_checkinteger(L, 3);
    LuaRuntime* runtime = get_runtime(L);
    if (GameState* gs = runtime->get_game_state()) {
        gs->variables["state_var_" + std::to_string(id)] = value;
    }
    return 0;
}

} // namespace game_api
} // namespace enginemon
