// engine/scripting/api_bindings.cpp
// C++ semantic APIs exposed to Lua scripts
// Stub implementations for testing - real implementations will call engine systems

#include "engine/scripting/api_bindings.hpp"
#include "engine/scripting/lua_runtime.hpp"
#include "engine/scripting/semantic_ir.hpp"
#include "engine/world/movement_manager.hpp"
#include "engine/core/game_state.hpp"
#include <unordered_map>
#include <vector>
#include <utility>
#include <memory>
#include <functional>
#include <string_view>

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
    // Stub: return true (yes)
    lua_pushboolean(L, true);
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

// ctx.world:move_actor(actor_id, direction, steps) - legacy API
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
        // Async mode: enqueue movement and yield
        StubActorState& state = (actor_id == 0) ? stubs.player : stubs.actors[actor_id];
        auto& mm = stubs.get_movement_manager();
        
        mm.enqueue_movement_table(
            actor_id, 
            static_cast<uint32_t>(actor_id),
            down, up, left, right,
            state.x, state.y,
            direction_from_string(state.facing)
        );
        
        lua_pushstring(L, "movement");
        return lua_yield(L, 1);
    } else {
        // Sync mode: apply immediately
        apply_sync_movement(stubs, actor_id, down, up, left, right);
        return 0;
    }
}

// ctx.world:move_actor(actor_id, {down=N, up=N, left=N, right=N}) - new semantic API
int move_actor_table(lua_State* L, int actor_id, int table_idx) {
    auto& stubs = get_stubs(L);
    
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
    
    stubs.movement_calls.push_back({"move_table", std::to_string(down) + "," + std::to_string(up) + 
                             "," + std::to_string(left) + "," + std::to_string(right)});
    
    if (stubs.async_movement_enabled) {
        StubActorState& state = (actor_id == 0) ? stubs.player : stubs.actors[actor_id];
        auto& mm = stubs.get_movement_manager();
        
        mm.enqueue_movement_table(
            actor_id,
            static_cast<uint32_t>(actor_id),
            down, up, left, right,
            state.x, state.y,
            direction_from_string(state.facing)
        );
        
        lua_pushstring(L, "movement");
        return lua_yield(L, 1);
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
int face_actor(lua_State* L) {
    auto& stubs = get_stubs(L);
    
    int actor_id = static_cast<int>(luaL_checkinteger(L, 2));
    const char* direction = luaL_checkstring(L, 3);
    
    StubActorState& state = (actor_id == 0) ? stubs.player : stubs.actors[actor_id];
    state.facing = direction;
    
    stubs.movement_calls.push_back({"face", direction});
    return 0;
}

// ctx.world:face_player() - NPC faces the player
int face_player(lua_State* L) {
    auto& stubs = get_stubs(L);
    stubs.movement_calls.push_back({"face_player", ""});
    return 0;
}

// ctx.world:get_player_pos() -> x, y, map_id
int get_player_pos(lua_State* L) {
    const auto& stubs = get_stubs(L);
    lua_pushinteger(L, stubs.player.x);
    lua_pushinteger(L, stubs.player.y);
    lua_pushinteger(L, 1);  // map_id stub
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
int teleport_player(lua_State* L) {
    int map_id = luaL_checkinteger(L, 2);
    int x = luaL_checkinteger(L, 3);
    int y = luaL_checkinteger(L, 4);
    (void)map_id; (void)x; (void)y;
    return 0;
}

// ctx.world:warp(map_id, x, y) — scripted warp to map coordinates
// Source: Script_warp passes (group, map, x, y) → semantic Sem_Warp{map, x, y}
// The emitter passes map ID + tile coordinates, matching Crystal's actual warp semantics.
int warp(lua_State* L) {
    auto& stubs = get_stubs(L);
    int map_id = luaL_checkinteger(L, 2);
    int x = luaL_checkinteger(L, 3);
    int y = luaL_checkinteger(L, 4);
    stubs.last_warp_map = map_id;
    stubs.last_warp_x = x;
    stubs.last_warp_y = y;
    stubs.movement_calls.push_back({"warp", std::to_string(map_id) + "," + std::to_string(x) + "," + std::to_string(y)});
    return 0;
}

// ctx.world:warp_to_spawn()
// Warp the player to their last-set backup/spawn warp position.
// Corresponds to Crystal's warptobspawn / WarpToBackup semantics.
// Semantic equivalent of Sem_WarpToBackup — use the backup warp point from GameState.
int warp_to_spawn(lua_State* L) {
    auto& stubs = get_stubs(L);
    stubs.movement_calls.push_back({"warp_to_spawn", ""});
    return 0;
}

// ctx.world:show_npc(npc_id)
int show_npc(lua_State* L) {
    int npc_id = luaL_checkinteger(L, 2);
    (void)npc_id;
    return 0;
}

// ctx.world:hide_npc(npc_id)
int hide_npc(lua_State* L) {
    int npc_id = luaL_checkinteger(L, 2);
    (void)npc_id;
    return 0;
}

// ctx.world:npc_visible(npc_id) -> bool
int npc_visible(lua_State* L) {
    int npc_id = luaL_checkinteger(L, 2);
    lua_pushboolean(L, true);
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
// species_id: 1-251, or 0 to clear the slot
// Stores in GameState::daycare_slot[slot-1].
int set_daycare_species(lua_State* L) {
    int slot       = static_cast<int>(luaL_checkinteger(L, 2));
    int species_id = static_cast<int>(luaL_checkinteger(L, 3));
    LuaRuntime* runtime = get_runtime(L);

    if (slot < 1 || slot > 2) {
        return luaL_error(L, "set_daycare_species: slot must be 1 or 2, got %d", slot);
    }
    if (species_id < 0 || species_id > 251) {
        return luaL_error(L,
            "set_daycare_species: species_id must be 0-251, got %d", species_id);
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
    lua_pushboolean(L, false);
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
    
    lua_pushboolean(L, true);
    return 1;
}

// ctx.party:has_move(move_id) -> bool
int has_move(lua_State* L) {
    int move_id = luaL_checkinteger(L, 2);
    lua_pushboolean(L, false);
    return 1;
}

// ctx.party:can_use_hm(hm_name) -> bool
int can_use_hm(lua_State* L) {
    const char* hm_name = luaL_checkstring(L, 2);
    lua_pushboolean(L, false);
    return 1;
}

} // namespace party_api

// =============================================================================
// Inventory API - ctx.inventory
// =============================================================================

namespace inventory_api {

// ctx.inventory:give(item_id, count)
int give(lua_State* L) {
    int item_id = luaL_checkinteger(L, 2);
    int count = luaL_optinteger(L, 3, 1);
    (void)item_id; (void)count;
    return 0;
}

// ctx.inventory:take(item_id, count) -> bool
int take(lua_State* L) {
    int item_id = luaL_checkinteger(L, 2);
    int count = luaL_optinteger(L, 3, 1);
    (void)item_id; (void)count;
    lua_pushboolean(L, true);
    return 1;
}

// ctx.inventory:has(item_id, count?) -> bool
int has(lua_State* L) {
    int item_id = luaL_checkinteger(L, 2);
    int count = luaL_optinteger(L, 3, 1);
    lua_pushboolean(L, false);
    return 1;
}

// ctx.inventory:count(item_id) -> number
int count(lua_State* L) {
    int item_id = luaL_checkinteger(L, 2);
    lua_pushinteger(L, 0);
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
    lua_pushboolean(L, success);
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
    lua_pushboolean(L, result);
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

// ctx.flags:set(flag_id)
int set(lua_State* L) {
    LuaRuntime* runtime = get_runtime(L);
    int flag_id = luaL_checkinteger(L, 2);
    // Production path: write through GameState when bound
    if (GameState* gs = runtime->get_game_state()) {
        gs->flags.insert("flag_" + std::to_string(flag_id));
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
        gs->flags.erase("flag_" + std::to_string(flag_id));
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
        value = gs->flags.count("flag_" + std::to_string(flag_id)) > 0;
    } else {
        value = get_test_flag(runtime, flag_id);
    }
    runtime->get_stub_services().flag_calls.push_back({"check", flag_id});
    lua_pushboolean(L, value);
    return 1;
}

// ctx.flags:set_var(var_id, value)
int set_var(lua_State* L) {
    LuaRuntime* runtime = get_runtime(L);
    int var_id = luaL_checkinteger(L, 2);
    int value  = luaL_checkinteger(L, 3);
    // Production path: write through GameState when bound
    if (GameState* gs = runtime->get_game_state()) {
        gs->variables["var_" + std::to_string(var_id)] = value;
    } else {
        runtime->get_stub_services().vars[var_id] = value;
    }
    return 0;
}

// ctx.flags:get_var(var_id) -> number
int get_var(lua_State* L) {
    LuaRuntime* runtime = get_runtime(L);
    int var_id = luaL_checkinteger(L, 2);
    int value  = 0;
    // Production path: read from GameState when bound
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
// Time API - ctx.time
// =============================================================================

namespace time_api {

// ctx.time:hour() -> 0-23
int hour(lua_State* L) {
    lua_pushinteger(L, 12);  // Stub: noon
    return 1;
}

// ctx.time:minute() -> 0-59
int minute(lua_State* L) {
    lua_pushinteger(L, 0);
    return 1;
}

// ctx.time:day_of_week() -> 0-6
int day_of_week(lua_State* L) {
    lua_pushinteger(L, 1);  // Monday
    return 1;
}

// ctx.time:time_of_day() -> "morning", "day", "night"
int time_of_day(lua_State* L) {
    lua_pushstring(L, "day");
    return 1;
}

// ctx.time:is_morning() -> bool
int is_morning(lua_State* L) {
    lua_pushboolean(L, false);
    return 1;
}

// ctx.time:is_day() -> bool
int is_day(lua_State* L) {
    lua_pushboolean(L, true);
    return 1;
}

// ctx.time:is_night() -> bool
int is_night(lua_State* L) {
    lua_pushboolean(L, false);
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

// ctx.game:behavior(name) - dispatch a named game-specific behavior
int behavior(lua_State* L) {
    const char* name = luaL_checkstring(L, 2);
    LuaRuntime* runtime = get_runtime(L);
    auto& stubs = runtime->get_stub_services();
    stubs.last_behavior_name = name;

    // Handle Sem_Sdefer deferred-script scheduling.
    // The emitter encodes "Sdefer_<script_id>" as the behavior name.
    static constexpr std::string_view SDEFER_PREFIX = "Sdefer_";
    std::string_view name_sv(name);
    if (name_sv.starts_with(SDEFER_PREFIX)) {
        std::string script_id(name_sv.substr(SDEFER_PREFIX.size()));
        if (stubs.deferred_script_fn) {
            stubs.deferred_script_fn(script_id);
        }
        // If no callback is wired (test/stub mode), the deferred script is
        // silently dropped — not a fabricated result, just no-op in isolation.
    }
    return 0;
}

// ctx.game:set_scene(scene)
int set_scene(lua_State* L) {
    int scene = luaL_checkinteger(L, 2);
    LuaRuntime* runtime = get_runtime(L);
    auto& stubs = runtime->get_stub_services();
    stubs.current_scene = scene;
    return 0;
}

// ctx.game:check_scene() -> scene_id
int check_scene(lua_State* L) {
    LuaRuntime* runtime = get_runtime(L);
    lua_pushinteger(L, runtime->get_stub_services().current_scene);
    return 1;
}

// ctx.game:set_map_scene(map_id, scene)
int set_map_scene(lua_State* L) {
    int map_id = luaL_checkinteger(L, 2);
    int scene   = luaL_checkinteger(L, 3);
    (void)map_id; (void)scene;
    return 0;
}

// ctx.game:check_map_scene(map_id) -> scene_id
int check_map_scene(lua_State* L) {
    int map_id = luaL_checkinteger(L, 2);
    (void)map_id;
    lua_pushinteger(L, 0); // stub
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
    lua_pushboolean(L, false); // stub
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
int set_daylight_saving(lua_State* L) {
    int enabled = luaL_checkinteger(L, 2);
    (void)enabled;
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
int read_state_var(lua_State* L) {
    int id = luaL_checkinteger(L, 2);
    (void)id;
    lua_pushinteger(L, 0); // stub
    return 1;
}

// ctx.game:write_state_var(id)
int write_state_var(lua_State* L) {
    int id = luaL_checkinteger(L, 2);
    (void)id;
    return 0;
}

// ctx.game:set_state_var(id, value)
int set_state_var(lua_State* L) {
    int id    = luaL_checkinteger(L, 2);
    int value = luaL_checkinteger(L, 3);
    (void)id; (void)value;
    return 0;
}

} // namespace game_api
} // namespace enginemon
