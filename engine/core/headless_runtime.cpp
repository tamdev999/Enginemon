// engine/core/headless_runtime.cpp
//
// Shared production bootstrap: simulation layer used by both enginemon_tiles
// and emon_smoke (and any future test harness that needs a real runtime).

#include "engine/core/headless_runtime.hpp"
#include "engine/world/johto_collision.hpp"
#include "engine/world/collision_types.hpp"

#include <iostream>

namespace enginemon {

//=============================================================================
// load_headless_world_state
//=============================================================================

bool load_headless_world_state(
    PackageReader&       pkg,
    const std::string&   map_id,
    HeadlessWorldState&  state,
    std::string&         error
) {
    auto map_opt = pkg.load_map(map_id);
    if (!map_opt) {
        error = "load_headless_world_state: failed to load map '" + map_id + "'";
        return false;
    }

    const std::string& tileset_id = map_opt->tileset_id;
    auto tileset_data = pkg.load_tileset_data(tileset_id);
    if (!tileset_data) {
        error = "load_headless_world_state: failed to load tileset '" + tileset_id + "'";
        return false;
    }

    auto tileset_opt = RuntimeTileset::from_package_data(tileset_id, *tileset_data);
    if (!tileset_opt) {
        error = "load_headless_world_state: failed to parse tileset '" + tileset_id + "'";
        return false;
    }

    state.map      = std::move(*map_opt);
    state.tileset  = std::move(*tileset_opt);
    state.map_id   = map_id;
    state.valid    = true;
    return true;
}

//=============================================================================
// init_npcs_from_map
//=============================================================================

void init_npcs_from_map(
    HeadlessGameLoop& game_loop,
    const RuntimeMap& map,
    const GameState&  game_state
) {
    game_loop.clear_npcs();

    for (const auto& obj : map.objects) {
        NpcState npc;
        npc.id              = obj.local_id;
        npc.x               = obj.x;
        npc.y               = obj.y;
        npc.facing          = movement_data_to_facing(obj.movement_type);
        npc.is_moving       = false;
        npc.is_trainer      = obj.is_trainer;
        npc.script_id       = obj.script_id;
        npc.visibility_flag = obj.visibility_flag;

        // Crystal visibility semantics (map_objects_2.asm CheckObjectFlag):
        //   0xFFFF (empty string) → always visible
        //   flag CLEAR            → visible
        //   flag SET              → hidden
        if (obj.visibility_flag.empty()) {
            npc.visible = true;
        } else {
            npc.visible = !game_state.check_flag(obj.visibility_flag);
        }

        npc.behavior      = movement_data_to_behavior(obj.movement_type);
        npc.radius_x      = obj.movement_radius_x;
        npc.radius_y      = obj.movement_radius_y;
        npc.init_x        = obj.x;
        npc.init_y        = obj.y;
        npc.idle_timer    = 30 + (obj.local_id * 17) % 98;
        npc.target_x      = obj.x;
        npc.target_y      = obj.y;
        npc.move_progress = 0;
        npc.frozen        = false;

        game_loop.add_npc(npc);
    }
}

//=============================================================================
// setup_headless_runtime
//=============================================================================

bool setup_headless_runtime(
    HeadlessRuntime&   rt,
    const std::string& map_id,
    int32_t            player_x,
    int32_t            player_y,
    Direction          player_facing,
    std::string&       error
) {
    if (!rt.package) {
        error = "setup_headless_runtime: rt.package must be set before calling";
        return false;
    }

    // -------------------------------------------------------------------------
    // Load initial world state (map + tileset)
    // -------------------------------------------------------------------------
    if (!load_headless_world_state(*rt.package, map_id, rt.world_state, error)) {
        return false;
    }

    // -------------------------------------------------------------------------
    // WorldManager wiring
    // -------------------------------------------------------------------------
    rt.world_manager.set_map_loader(
        [&rt](const std::string& mid) -> std::optional<RuntimeMap> {
            return rt.package->load_map(mid);
        });
    rt.world_manager.load_map(map_id);

    // -------------------------------------------------------------------------
    // GameState initial player position
    // -------------------------------------------------------------------------
    rt.game_state.player.current_map_id = map_id;
    rt.game_state.player.x              = player_x;
    rt.game_state.player.y              = player_y;
    rt.game_state.player.facing         = player_facing;

    // -------------------------------------------------------------------------
    // HeadlessGameLoop: map, collision, game_state
    // -------------------------------------------------------------------------
    rt.game_loop.load_map(rt.world_state.map);

    rt.game_loop.set_collision_data(
        [&rt](int32_t x, int32_t y) -> CollisionClass {
            return get_collision_from_blocks(
                rt.world_state.map.blocks,
                rt.world_state.tileset.collision,
                rt.world_state.map.width,
                x, y);
        });

    rt.game_loop.set_game_state(&rt.game_state);

    // -------------------------------------------------------------------------
    // NPC initialization with production Crystal visibility semantics
    // -------------------------------------------------------------------------
    init_npcs_from_map(rt.game_loop, rt.world_state, rt.game_state);

    // -------------------------------------------------------------------------
    // Spawn player
    // -------------------------------------------------------------------------
    rt.game_loop.spawn_player(player_x, player_y, player_facing);

    // -------------------------------------------------------------------------
    // LuaRuntime: bind GameState, set error handler
    // -------------------------------------------------------------------------
    rt.lua_runtime.set_game_state(&rt.game_state);

    rt.lua_runtime.set_error_handler(
        [&rt](const std::string& err, const std::string& traceback) {
            auto msg = "[SCRIPT ERROR] " + err +
                       (traceback.empty() ? "" : "\n" + traceback);
            if (rt.on_script_error) {
                rt.on_script_error(msg);
            } else {
                std::cerr << msg << "\n";
            }
        });

    // -------------------------------------------------------------------------
    // Wire LuaRuntime to HeadlessGameLoop (NPC callbacks, deferred scripts, etc.)
    // -------------------------------------------------------------------------
    rt.game_loop.set_lua_runtime(&rt.lua_runtime);

    // -------------------------------------------------------------------------
    // Script loader: reads compiled Lua from the package
    // -------------------------------------------------------------------------
    rt.game_loop.set_script_loader(
        [&rt](const std::string& script_id) -> std::string {
            auto lua_code = rt.package->load_script(script_id);
            return lua_code.value_or("");
        });

    // -------------------------------------------------------------------------
    // Headless warp_fn:
    //
    // Implements the full authoritative transaction for scripted warps
    // in a headless context (no GPU):
    //
    //   1. prepare_warp — resolves destination, no state mutation (fallible)
    //   2. load_headless_world_state — loads new map+tileset (fallible)
    //   3. commit_warp — updates WorldManager + GameState.player (non-failing)
    //   4. game_loop.load_map — swaps game_loop's current map (non-failing)
    //   5. set_collision_data — rewires collision to new tileset (non-failing)
    //   6. init_npcs_from_map — rebuilds NPC list with correct visibility (non-failing)
    //   7. spawn_player — updates player position (non-failing)
    //
    // Atomicity: if steps 1 or 2 fail, NOTHING in WorldManager/GameState/
    // game_loop/world_state has been mutated. The old map remains authoritative.
    //
    // The production renderer (enginemon_tiles) REPLACES this warp_fn after
    // GPU init with a version that also calls vkDeviceWaitIdle + prepare/commit
    // on TileRenderer/SpriteRenderer. This headless version is the shared core;
    // the GPU version layers on top.
    // -------------------------------------------------------------------------
    rt.lua_runtime.get_stub_services().warp_fn =
        [&rt](const std::string& map_id, int32_t x, int32_t y) -> bool {

        // Step 1: resolve destination — fallible, no state mutation.
        // explicit_coords = true: x/y are the intended landing position;
        // no warp-index table lookup required.
        RuntimeWarp synthetic;
        synthetic.target_map_id    = map_id;
        synthetic.target_warp_index = 0;
        synthetic.x                = static_cast<uint8_t>(x);
        synthetic.y                = static_cast<uint8_t>(y);
        synthetic.explicit_coords  = true;  // scripted coordinate warp — bypass warp-index
        WarpResult warp_result = rt.world_manager.prepare_warp(synthetic, rt.game_state);
        if (!warp_result.success) return false;

        // Step 2: load destination world state — fallible, no state mutation yet
        HeadlessWorldState new_world;
        std::string load_err;
        if (!load_headless_world_state(*rt.package, map_id, new_world, load_err)) {
            std::cerr << "[headless warp] world state load failed for '"
                      << map_id << "': " << load_err << "\n";
            return false;
        }

        // --- ALL FALLIBLE OPERATIONS SUCCEEDED ---
        // Steps 3–7 are non-failing; apply atomically.

        // Step 3: commit WorldManager + GameState player
        rt.world_manager.commit_warp(warp_result, rt.game_state);

        // Step 4: swap world state
        rt.world_state = std::move(new_world);

        // Step 5: reload game_loop map + rewire collision
        rt.game_loop.load_map(rt.world_state.map);
        rt.game_loop.set_collision_data(
            [&rt](int32_t cx, int32_t cy) -> CollisionClass {
                return get_collision_from_blocks(
                    rt.world_state.map.blocks,
                    rt.world_state.tileset.collision,
                    rt.world_state.map.width,
                    cx, cy);
            });

        // Step 6: rebuild NPCs with production visibility semantics
        init_npcs_from_map(rt.game_loop, rt.world_state.map, rt.game_state);

        // Step 7: spawn player at destination
        rt.game_loop.spawn_player(x, y, rt.game_state.player.facing);
        return true;
    };

    return true;
}

} // namespace enginemon
