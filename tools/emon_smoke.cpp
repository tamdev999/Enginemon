// tools/emon_smoke.cpp
// Headless production smoke test — verifies the real orchestration path without Vulkan.
//
// Exercises:
//   PackageReader::open() -> validate()
//   PackageReader::load_map()
//   RuntimeTileset::from_package_data()
//   WorldManager::load_map() / map_loader_ callback
//   HeadlessGameLoop::load_map() / set_collision_data() / spawn_player()
//   GameState init + game_loop.set_game_state()
//   LuaRuntime init + game_loop.set_lua_runtime()
//   script_loader_ bound to PackageReader::load_script()
//   game_loop.tick() x 10 (collision, NPC update, deferred-script drain)
//
// This proves the PackageReader -> GameState -> HeadlessGameLoop -> LuaRuntime
// integration stack is live without requiring Vulkan or a display.
//
// Usage:
//   emon_smoke <package.emon>
//
// Exit codes:
//   0 = smoke passed (all integration steps succeeded, 10 ticks completed)
//   1 = failure (package missing, map not found, tick error, etc.)

#include "engine/package/package_reader.hpp"
#include "engine/world/runtime_tileset.hpp"
#include "engine/world/runtime_map.hpp"
#include "engine/world/world_manager.hpp"
#include "engine/world/collision_types.hpp"
#include "engine/world/johto_collision.hpp"
#include "engine/core/game_loop.hpp"
#include "engine/core/game_state.hpp"
#include "engine/scripting/lua_runtime.hpp"

#include <iostream>
#include <string>
#include <optional>

// Minimal collision lookup used during smoke — returns Floor for all cells
// so the tick loop can run without real tileset collision data.
static enginemon::CollisionClass floor_collision(int32_t, int32_t) {
    return enginemon::CollisionClass::Floor;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: emon_smoke <package.emon>\n";
        return 1;
    }

    const std::string pkg_path = argv[1];
    std::cerr << "[smoke] Opening package: " << pkg_path << "\n";

    // ---------------------------------------------------------------------------
    // Step 1: Open and validate package
    // ---------------------------------------------------------------------------
    auto pkg = enginemon::PackageReader::open(pkg_path);
    if (!pkg) {
        std::cerr << "[smoke] FAIL: PackageReader::open() returned null\n";
        return 1;
    }
    if (!pkg->validate()) {
        std::cerr << "[smoke] FAIL: PackageReader::validate() failed (CRC mismatch or corrupt)\n";
        return 1;
    }
    std::cerr << "[smoke] Package opened and validated OK\n";

    // ---------------------------------------------------------------------------
    // Step 2: Find first available map in the package
    // ---------------------------------------------------------------------------
    auto map_ids = pkg->list_maps();
    if (map_ids.empty()) {
        std::cerr << "[smoke] FAIL: No maps in package\n";
        return 1;
    }
    const std::string& first_map_id = map_ids.front();
    std::cerr << "[smoke] First map ID: " << first_map_id << "\n";

    auto rmap_opt = pkg->load_map(first_map_id);
    if (!rmap_opt.has_value()) {
        std::cerr << "[smoke] FAIL: PackageReader::load_map('" << first_map_id << "') returned nullopt\n";
        return 1;
    }
    std::cerr << "[smoke] Map loaded: " << first_map_id
              << " (" << rmap_opt->width << "x" << rmap_opt->height << " blocks)\n";

    // ---------------------------------------------------------------------------
    // Step 3: Load tileset for collision
    // ---------------------------------------------------------------------------
    const std::string& tileset_id = rmap_opt->tileset_id;
    auto tileset_opt = pkg->load_tileset_data(tileset_id);
    // Tileset load is best-effort — smoke continues with floor collision if absent
    bool has_tileset = tileset_opt.has_value();
    std::optional<enginemon::RuntimeTileset> rtileset;
    if (has_tileset) {
        rtileset = enginemon::RuntimeTileset::from_package_data(tileset_id, *tileset_opt);
        if (!rtileset.has_value()) {
            std::cerr << "[smoke] WARNING: RuntimeTileset::from_package_data failed for '"
                      << tileset_id << "'; using floor collision\n";
        } else {
            std::cerr << "[smoke] Tileset loaded: " << tileset_id << "\n";
        }
    } else {
        std::cerr << "[smoke] WARNING: tileset '" << tileset_id
                  << "' not in package; using floor collision\n";
    }

    // ---------------------------------------------------------------------------
    // Step 4: Set up WorldManager
    // ---------------------------------------------------------------------------
    enginemon::WorldManager world_manager;
    world_manager.set_map_loader([&pkg](const std::string& mid)
        -> std::optional<enginemon::RuntimeMap> {
        return pkg->load_map(mid);
    });
    if (!world_manager.load_map(first_map_id)) {
        std::cerr << "[smoke] FAIL: WorldManager::load_map('" << first_map_id << "') failed\n";
        return 1;
    }
    std::cerr << "[smoke] WorldManager loaded map OK\n";

    // ---------------------------------------------------------------------------
    // Step 5: Set up GameState, LuaRuntime, HeadlessGameLoop
    // ---------------------------------------------------------------------------
    enginemon::GameState game_state;
    game_state.rng.seed(0xDEADBEEF);
    game_state.player.current_map_id = first_map_id;
    game_state.player.x = 1;
    game_state.player.y = 1;
    game_state.player.facing = enginemon::Direction::Down;

    enginemon::LuaRuntime lua_runtime;
    lua_runtime.set_game_state(&game_state);

    enginemon::HeadlessGameLoop game_loop;

    // Load map into game loop
    game_loop.load_map(*rmap_opt);

    // Set collision — use real tileset data if available, else floor everywhere
    if (rtileset.has_value()) {
        const auto& tileset_ref = *rtileset;
        game_loop.set_collision_data(
            [&rmap_opt, &tileset_ref](int32_t x, int32_t y) -> enginemon::CollisionClass {
                return enginemon::get_collision_from_blocks(
                    rmap_opt->blocks, tileset_ref.collision,
                    rmap_opt->width, x, y);
            });
    } else {
        game_loop.set_collision_data(floor_collision);
    }

    game_loop.set_game_state(&game_state);
    game_loop.set_lua_runtime(&lua_runtime);

    // Wire script loader so any scripted interaction during ticks can resolve scripts
    lua_runtime.set_error_handler([](const std::string& err, const std::string&) {
        // Script errors during smoke are warnings, not fatals — we are not
        // running scripts intentionally, just ticking the loop.
        std::cerr << "[smoke] script error (non-fatal): " << err << "\n";
    });
    game_loop.set_script_loader([&pkg](const std::string& sid) -> std::string {
        auto lua = pkg->load_script(sid);
        return lua.value_or("");
    });

    // Spawn player
    game_loop.spawn_player(1, 1, enginemon::Direction::Down);

    // Add NPCs from map objects
    for (const auto& obj : rmap_opt->objects) {
        enginemon::NpcState npc;
        npc.id       = obj.local_id;
        npc.x        = obj.x;
        npc.y        = obj.y;
        npc.facing   = enginemon::Direction::Down;
        npc.visible  = true;
        npc.script_id = obj.script_id;
        game_loop.add_npc(npc);
    }

    std::cerr << "[smoke] HeadlessGameLoop initialized ("
              << rmap_opt->objects.size() << " NPCs)\n";

    // ---------------------------------------------------------------------------
    // Step 6: Run 10 simulation ticks
    // ---------------------------------------------------------------------------
    bool had_error = false;
    for (int i = 0; i < 10; ++i) {
        auto tr = game_loop.tick();
        if (tr.script_error) {
            // Script errors during idle ticks are deferred-script failures.
            // Not fatal for smoke — connection/package integrity is what we test.
            std::cerr << "[smoke] tick " << i << ": script_error=true (deferred script failure)\n";
            had_error = true;
        }
    }
    std::cerr << "[smoke] 10 ticks completed\n";

    // ---------------------------------------------------------------------------
    // Result
    // ---------------------------------------------------------------------------
    if (had_error) {
        std::cerr << "[smoke] FAIL: script errors during tick loop\n";
        return 1;
    }

    std::cerr << "[smoke] PASS\n";
    return 0;
}
