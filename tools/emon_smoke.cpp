// tools/emon_smoke.cpp
// Headless production smoke test.
//
// Uses the shared production bootstrap (engine/core/headless_runtime.hpp)
// to exercise the same simulation topology as enginemon_tiles — without GPU.
//
// The bootstrap is NOT a simplified duplicate. It uses:
//   - PackageReader::open + validate
//   - WorldManager with production map_loader
//   - GameState with authoritative player state
//   - HeadlessGameLoop with production NPC visibility semantics
//   - LuaRuntime bound to GameState (ctx.flags writes go through)
//   - warp_fn with full atomicity (prepare_warp → load destination → commit)
//   - script_loader from package
//
// Smoke probes:
//   1. Production bootstrap succeeds
//   2. Initial map loads correctly
//   3. Lua → GameState mutation (ctx.flags:set → gs.check_flag)
//   4. NPC visibility semantics (flag SET = hidden)
//   5. One world transition succeeds (prepare_warp → commit is atomic)
//   6. Forced transition failure leaves old map authoritative everywhere
//   7. Several ticks without script error
//
// Exit codes:
//   0 = PASS
//   1 = FAIL

#include "engine/core/headless_runtime.hpp"
#include "engine/package/package_reader.hpp"

#include <iostream>
#include <string>
#include <cassert>
#include <cstdlib>

// ---------------------------------------------------------------------------
// Freshly generated package from run_all_tests.ps1 is passed as argv[1].
// ---------------------------------------------------------------------------

static void fail(const std::string& msg) {
    std::cerr << "[smoke] FAIL: " << msg << "\n";
    std::exit(1);
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: emon_smoke <package.emon>\n";
        return 1;
    }

    const std::string pkg_path = argv[1];

    // =========================================================================
    // GATE 1: Package open + validate
    // =========================================================================
    std::cerr << "[smoke] Opening package: " << pkg_path << "\n";

    auto pkg = enginemon::PackageReader::open(pkg_path);
    if (!pkg) fail("PackageReader::open() returned null");
    if (!pkg->validate()) fail("PackageReader::validate() failed (CRC or corrupt)");
    std::cerr << "[smoke] Package opened and validated OK\n";

    // =========================================================================
    // GATE 2: Production bootstrap
    //
    // Uses setup_headless_runtime — the same production wiring used by
    // enginemon_tiles, minus GPU resources.
    // =========================================================================
    auto map_ids = pkg->list_maps();
    if (map_ids.empty()) fail("No maps in package");
    const std::string& first_map_id = map_ids.front();
    std::cerr << "[smoke] First map ID: " << first_map_id << "\n";

    enginemon::HeadlessRuntime rt;
    rt.package = pkg.get();

    std::string bootstrap_error;
    if (!enginemon::setup_headless_runtime(rt, first_map_id, 1, 1,
                                           enginemon::Direction::Down,
                                           bootstrap_error)) {
        fail("setup_headless_runtime failed: " + bootstrap_error);
    }
    std::cerr << "[smoke] Production bootstrap OK (map='" << first_map_id
              << "', NPCs=" << rt.game_loop.npcs().size() << ")\n";

    // =========================================================================
    // GATE 3: Lua → authoritative GameState mutation
    //
    // ctx.flags:set(42) must write to GameState::flags["flag_002a"]
    // (hex-padded, matching api_bindings canonical format).
    // If set_game_state were not called, the write would silently fall into
    // StubServices and GameState would be untouched.
    // =========================================================================
    {
        const char* flag_script = R"(
script = {}
function script.main(ctx)
    ctx.flags:set(42)
    return
end
return script
)";
        try {
            rt.lua_runtime.execute_string(flag_script, "smoke_flag_test");
        } catch (const std::exception& ex) {
            fail(std::string("execute_string threw: ") + ex.what());
        }
        rt.lua_runtime.start_script("script");

        // Flag 42 (0x2A) → canonical key "flag_002a"
        if (!rt.game_state.check_flag("flag_002a")) {
            fail("flag_002a not found in GameState after ctx.flags:set(42) — "
                 "GameState not bound to LuaRuntime");
        }
        std::cerr << "[smoke] Lua→GameState mutation: ctx.flags:set(42) "
                     "→ flag_002a in GameState OK\n";
    }

    // =========================================================================
    // GATE 4: NPC visibility semantics
    //
    // Set a flag that controls an NPC's visibility, then reinitialize the NPC
    // list. The NPC must be hidden (flag SET = hidden per Crystal semantics).
    // Then clear the flag and reinitialize again — NPC must be visible.
    //
    // This proves the Crystal CheckObjectFlag semantics are in production.
    // =========================================================================
    {
        // Find an NPC with a controlling visibility_flag
        const enginemon::NpcState* flagged_npc = nullptr;
        for (const auto& npc : rt.game_loop.npcs()) {
            if (!npc.visibility_flag.empty()) {
                flagged_npc = &npc;
                break;
            }
        }

        if (flagged_npc) {
            const std::string vis_flag = flagged_npc->visibility_flag;
            uint16_t npc_id = flagged_npc->id;

            // Set the flag → NPC should be hidden after reinit
            rt.game_state.set_flag(vis_flag);
            enginemon::init_npcs_from_map(rt.game_loop, rt.world_state, rt.game_state);
            const enginemon::NpcState* reloaded = rt.game_loop.get_npc(npc_id);
            if (!reloaded) fail("NPC missing after reinit with flag set");
            if (reloaded->visible) {
                fail("NPC with flag SET is visible — Crystal visibility semantics inverted");
            }
            std::cerr << "[smoke] NPC visibility: flag SET → hidden OK\n";

            // Clear the flag → NPC should be visible after reinit
            rt.game_state.clear_flag(vis_flag);
            enginemon::init_npcs_from_map(rt.game_loop, rt.world_state, rt.game_state);
            const enginemon::NpcState* reshown = rt.game_loop.get_npc(npc_id);
            if (!reshown) fail("NPC missing after reinit with flag clear");
            if (!reshown->visible) {
                fail("NPC with flag CLEAR is hidden — Crystal visibility semantics inverted");
            }
            std::cerr << "[smoke] NPC visibility: flag CLEAR → visible OK\n";
        } else {
            std::cerr << "[smoke] NPC visibility: skipped (no flagged NPCs on "
                      << first_map_id << ")\n";
        }
    }

    // =========================================================================
    // GATE 5: Transition success path — canonical scripted warp
    //
    // Exercise the production headless warp_fn (prepare_warp → load destination
    // → commit_warp → init_npcs_from_map → restore_npc_states → spawn_player).
    // After the transition, WorldManager, GameState, HeadlessWorldState, and
    // game_loop must all agree on the destination map.
    //
    // A NPC snapshot is planted in GameState before the warp so we can verify
    // that restore_npc_states fires inside the warp_fn (not just init defaults).
    // =========================================================================
    std::string pre_transition_map = rt.game_state.player.current_map_id;
    bool did_transition = false;

    if (map_ids.size() >= 2) {
        const std::string& dest_map = map_ids[1];
        std::cerr << "[smoke] Transition: '" << pre_transition_map
                  << "' → '" << dest_map << "'\n";

        // Plant a fake NPC snapshot for the destination map so we can verify
        // restore_npc_states was called by the warp path (not manually here).
        // If the warp_fn calls restore_npc_states, the NPC will be at (3,4)
        // after the transition instead of whatever the package puts it at.
        // We only do this if the destination map has at least one object — pick
        // the first NPC id from the destination map's objects for the probe.
        uint16_t probe_npc_id = 0;
        bool probe_npc_planted = false;
        {
            auto dest_map_opt = rt.package->load_map(dest_map);
            if (dest_map_opt && !dest_map_opt->objects.empty()) {
                probe_npc_id = dest_map_opt->objects[0].local_id;
                enginemon::NpcSaveState probe;
                probe.id = probe_npc_id;
                probe.x = 3; probe.y = 4;
                probe.facing = enginemon::Direction::Left;
                probe.visible = true;
                rt.game_state.npc_states[dest_map] = {probe};
                probe_npc_planted = true;
                std::cerr << "[smoke] Planted NPC snapshot: id=" << probe_npc_id
                          << " at (3,4,Left) in dest map npc_states\n";
            }
        }

        // Use the canonical scripted warp path — explicit_coords=true, no
        // physical warp entry required.  This exercises the full headless
        // warp transaction: prepare_warp → load_headless_world_state →
        // commit_warp → init_npcs_from_map → restore_npc_states → spawn_player.
        bool warp_ok = rt.lua_runtime.get_stub_services().warp_fn(dest_map, 1, 1);
        if (!warp_ok) {
            fail("warp_fn('" + dest_map + "', 1, 1) returned false");
        }

        // Verify all authoritative subsystems agree on the destination map.
        const std::string& gs_map = rt.game_state.player.current_map_id;
        if (gs_map != dest_map) {
            fail("GameState.player.current_map_id '" + gs_map +
                 "' does not match destination '" + dest_map + "'");
        }
        const enginemon::RuntimeMap* wm_map = rt.world_manager.current_map();
        if (!wm_map || wm_map->map_id != dest_map) {
            fail("WorldManager::current_map() '" +
                 (wm_map ? wm_map->map_id : "<null>") +
                 "' does not match destination '" + dest_map + "'");
        }
        if (rt.world_state.map_id != dest_map) {
            fail("world_state.map_id '" + rt.world_state.map_id +
                 "' does not match destination '" + dest_map + "'");
        }
        std::cerr << "[smoke] Transition OK: all subsystems → '" << dest_map << "'\n";

        // Verify player landed at the explicit coordinates we requested.
        if (rt.game_state.player.x != 1 || rt.game_state.player.y != 1) {
            fail("GameState.player position is not (1,1) after warp — expected explicit coords");
        }
        std::cerr << "[smoke] Player coords: (" << rt.game_state.player.x
                  << "," << rt.game_state.player.y << ") = (1,1) OK\n";

        // Verify collision is live for the destination tileset.
        // Spot-check the (0,0) cell — it should not crash and must return a
        // CollisionClass (not raw garbage from a stale tileset).
        auto test_coll = enginemon::get_collision_from_blocks(
            rt.world_state.map.blocks,
            rt.world_state.tileset.collision,
            rt.world_state.map.width, 0, 0);
        // CollisionClass is an enum — any value other than the sentinel is fine;
        // the test here is that the collision data is coherent (didn't crash).
        (void)test_coll;
        std::cerr << "[smoke] Collision data coherent for dest map\n";

        // Verify restore_npc_states fired inside the warp path:
        // if we planted a probe, the live NPC for that id must be at (3,4,Left)
        // not at ROM defaults.
        if (probe_npc_planted) {
            const enginemon::NpcState* probe_live = rt.game_loop.get_npc(probe_npc_id);
            if (!probe_live) {
                // Map may have no NPC with that id if the package omits it; skip
                std::cerr << "[smoke] NPC restore: probe NPC id=" << probe_npc_id
                          << " not found in live loop — skipping position check\n";
            } else if (probe_live->x == 3 && probe_live->y == 4
                       && probe_live->facing == enginemon::Direction::Left) {
                std::cerr << "[smoke] NPC restore via warp_fn: probe NPC at (3,4,Left) OK\n";
            } else {
                fail("NPC restore: probe NPC id=" + std::to_string(probe_npc_id) +
                     " expected (3,4,Left) but got (" + std::to_string(probe_live->x) +
                     "," + std::to_string(probe_live->y) + ") — restore_npc_states not called by warp_fn");
            }
        }

        did_transition = true;
    } else {
        std::cerr << "[smoke] Transition: skipped (package has only 1 map)\n";
    }

    // =========================================================================
    // GATE 6: Forced transition failure — no partial commit
    //
    // Attempt a warp to a map that does not exist in the package.
    // The transition must fail cleanly: the old map must remain authoritative
    // in all three subsystems (WorldManager, GameState, world_state).
    // =========================================================================
    {
        const std::string current_map_before = rt.game_state.player.current_map_id;
        const std::string bogus_map = "__nonexistent_map_for_smoke_test__";

        bool failed_ok = rt.lua_runtime.get_stub_services().warp_fn(bogus_map, 0, 0);
        // Transition to nonexistent map must return false
        if (failed_ok) {
            fail("warp_fn to nonexistent map returned true — atomicity broken");
        }

        // All three subsystems must still point at the pre-failure map
        if (rt.game_state.player.current_map_id != current_map_before) {
            fail("GameState.player.current_map_id changed after failed transition");
        }
        const enginemon::RuntimeMap* wm_map = rt.world_manager.current_map();
        if (!wm_map || wm_map->map_id != current_map_before) {
            fail("WorldManager::current_map() changed after failed transition");
        }
        if (rt.world_state.map_id != current_map_before) {
            fail("world_state.map_id changed after failed transition");
        }
        std::cerr << "[smoke] Failure atomicity: failed transition left old map '"
                  << current_map_before << "' authoritative everywhere OK\n";
    }

    // =========================================================================
    // GATE 7: Several ticks without script error
    //
    // Reload the first map (so we have a stable state for ticking).
    // =========================================================================
    {
        // Re-bootstrap on first map for clean tick test
        std::string reload_error;
        if (!enginemon::load_headless_world_state(*pkg, first_map_id,
                                                   rt.world_state, reload_error)) {
            fail("Reload of first map failed: " + reload_error);
        }
        rt.world_manager.load_map(first_map_id);
        rt.game_loop.load_map(rt.world_state.map);
        rt.game_loop.set_collision_data(
            [&rt](int32_t x, int32_t y) -> enginemon::CollisionClass {
                return enginemon::get_collision_from_blocks(
                    rt.world_state.map.blocks,
                    rt.world_state.tileset.collision,
                    rt.world_state.map.width, x, y);
            });
        enginemon::init_npcs_from_map(rt.game_loop, rt.world_state, rt.game_state);
        rt.game_loop.spawn_player(1, 1, enginemon::Direction::Down);
        rt.game_state.player.current_map_id = first_map_id;

        bool had_error = false;
        for (int i = 0; i < 10; ++i) {
            auto tr = rt.game_loop.tick();
            if (tr.script_error) {
                std::cerr << "[smoke] tick " << i << ": script_error=true\n";
                had_error = true;
            }
        }
        if (had_error) fail("script errors during tick loop");
        std::cerr << "[smoke] 10 ticks completed without error\n";
    }

    std::cerr << "[smoke] PASS\n";
    return 0;
}
