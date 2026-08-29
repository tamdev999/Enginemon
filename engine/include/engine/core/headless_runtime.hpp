// engine/include/engine/core/headless_runtime.hpp
//
// Shared production bootstrap for headless runtime instances.
//
// Both emon_smoke (headless tools) and enginemon_tiles (production renderer)
// use this to construct the authoritative simulation layer:
//
//   PackageReader → GameState → WorldManager → HeadlessGameLoop → LuaRuntime
//
// This covers everything up to the GPU/presentation boundary.
// GPU renderers (TileRenderer, SpriteRenderer, etc.) are layered on top by
// the production renderer after calling setup_headless_runtime().
//
// Invariant: any code path reachable in enginemon_tiles is also reachable here,
// so integration bugs cannot hide behind a simplified test bootstrap.

#pragma once

#include "engine/package/package_reader.hpp"
#include "engine/world/world_manager.hpp"
#include "engine/world/runtime_tileset.hpp"
#include "engine/world/johto_collision.hpp"
#include "engine/world/collision_types.hpp"
#include "engine/core/game_loop.hpp"
#include "engine/core/game_state.hpp"
#include "engine/scripting/lua_runtime.hpp"

#include <string>
#include <optional>
#include <functional>

namespace enginemon {

//=============================================================================
// HeadlessWorldState
//
// GPU-independent world snapshot: loaded map + tileset.
// Mirrors the simulation-relevant fields of main_tiles.cpp's WorldState
// without sprite atlas or palette data (those require GPU).
//=============================================================================
struct HeadlessWorldState {
    RuntimeMap      map;
    RuntimeTileset  tileset;
    std::string     map_id;
    bool            valid = false;
};

//=============================================================================
// HeadlessRuntime
//
// Owns the full simulation stack: PackageReader → GameState → WorldManager
// → HeadlessGameLoop → LuaRuntime, all wired together using production
// semantics identical to enginemon_tiles.
//
// Lifetime: all members are owned by this struct. The caller must keep
// the HeadlessRuntime alive for as long as any callback/lambda references it.
//=============================================================================
struct HeadlessRuntime {
    // Package access (the caller must supply a live PackageReader)
    PackageReader*  package = nullptr;  // non-owning; caller owns the reader

    // Core simulation objects (owned)
    GameState       game_state;
    WorldManager    world_manager;
    HeadlessGameLoop game_loop;
    LuaRuntime      lua_runtime;

    // Current loaded world state (updated on each transition)
    HeadlessWorldState world_state;

    // Error output (default: stderr)
    std::function<void(const std::string&)> on_script_error;

    HeadlessRuntime() = default;

    // Non-copyable, non-movable: lambdas capture 'this'.
    HeadlessRuntime(const HeadlessRuntime&)            = delete;
    HeadlessRuntime& operator=(const HeadlessRuntime&) = delete;
    HeadlessRuntime(HeadlessRuntime&&)                 = delete;
    HeadlessRuntime& operator=(HeadlessRuntime&&)      = delete;

    // Call this before game_state.serialize() to capture live NPC positions.
    // Delegates to HeadlessGameLoop::prepare_for_save() which snapshots the
    // current map's NPC states into game_state.npc_states.
    void prepare_for_save() {
        game_loop.prepare_for_save();
    }
};

//=============================================================================
// load_headless_world_state
//
// Loads map + tileset into HeadlessWorldState from the package.
// Pure CPU/package I/O — no GPU calls. Safe to call before Vulkan is init'd.
//
// Returns false and leaves state unchanged on any failure.
//=============================================================================
bool load_headless_world_state(
    PackageReader&       pkg,
    const std::string&   map_id,
    HeadlessWorldState&  state,
    std::string&         error
);

//=============================================================================
// init_npcs_from_map
//
// Populates the game_loop NPC list from map objects using the production
// Crystal visibility semantics:
//   0xFFFF (empty flag) → always visible
//   flag CLEAR          → visible
//   flag SET            → hidden  (Crystal CheckObjectFlag semantics)
//
// Clears existing NPCs before adding new ones.
//
// Source: pokecrystal/engine/overworld/map_objects_2.asm CheckObjectFlag
//=============================================================================
// Takes RuntimeMap directly — usable from both HeadlessWorldState
// and main_tiles.cpp's WorldState (which wraps RuntimeMap).
void init_npcs_from_map(
    HeadlessGameLoop&  game_loop,
    const RuntimeMap&  map,
    const GameState&   game_state
);

// Convenience overload for HeadlessWorldState
inline void init_npcs_from_map(
    HeadlessGameLoop&         game_loop,
    const HeadlessWorldState& world_state,
    const GameState&          game_state
) {
    init_npcs_from_map(game_loop, world_state.map, game_state);
}

//=============================================================================
// setup_headless_runtime
//
// Constructs the full production simulation stack.
//
// Wires:
//   - WorldManager map_loader → PackageReader::load_map
//   - GameState → LuaRuntime (ctx.flags, ctx.time, ctx.inventory)
//   - GameState → HeadlessGameLoop (NPC movement, RNG)
//   - LuaRuntime → HeadlessGameLoop (script execution)
//   - script_loader → PackageReader::load_script
//   - error_handler → rt.on_script_error (or stderr)
//   - warp_fn → authoritative WorldManager::prepare_warp → commit (headless path)
//     Headless warp: no GPU staging; updates WorldManager + GameState + game_loop
//     atomically; failure leaves all state unchanged.
//   - NPC init with correct visibility semantics
//   - collision data from loaded tileset
//
// Parameters:
//   rt       — HeadlessRuntime to initialize (must already have rt.package set)
//   map_id   — initial map to load
//   player_x, player_y, player_facing — spawn position
//   error    — filled on failure
//
// Returns true on success. The caller may then override warp_fn with a
// GPU-aware version after the renderer is initialized.
//=============================================================================
bool setup_headless_runtime(
    HeadlessRuntime&   rt,
    const std::string& map_id,
    int32_t            player_x,
    int32_t            player_y,
    Direction          player_facing,
    std::string&       error
);

} // namespace enginemon
