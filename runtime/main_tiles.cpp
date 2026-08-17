// runtime/main_tiles.cpp
// Overworld rendering with collision, interaction, scripting, and world transitions
//
// Loads EMON native package, renders via Vulkan 1.3.
// Uses HeadlessGameLoop for collision/interaction/scripting - gameplay logic stays in engine layer.
// Uses WorldManager for warp/connection transitions - map loading is data-driven.
//
// Architecture (compiler/runtime boundary):
//   Crystal ROM → CrystalCompiler → EMON package (offline)
//   EMON package → PackageReader → RuntimeMap/RuntimeTileset/Scripts → LuaRuntime (runtime)
//
// Tileset rendering uses native 8×8 tiles expanded from blocks on map load.
// The runtime contains ZERO Crystal/ROM extraction code.
// All data is loaded from the EMON native package.

#include "platform/sdl3_platform.hpp"
#include "render/vulkan_bootstrap.hpp"
#include "render/tile_renderer.hpp"
#include "render/sprite_renderer.hpp"
#include "render/textbox_renderer.hpp"
#include "engine/package/package_reader.hpp"
#include "engine/world/runtime_tileset.hpp"
#include "engine/world/runtime_map.hpp"
#include "engine/world/sprite_atlas.hpp"
#include "engine/world/johto_collision.hpp"
#include "engine/world/world_manager.hpp"
#include "engine/core/game_loop.hpp"
#include "engine/core/game_state.hpp"
#include "engine/core/timing.hpp"
#include "engine/scripting/lua_runtime.hpp"
#include "engine/scripting/api_bindings.hpp"

#include <iostream>
#include <cmath>
#include <filesystem>
#include <set>
#include <unordered_map>
#include <chrono>

using namespace enginemon;

//=============================================================================
// WORLD STATE
// Holds all map-dependent state for rendering and gameplay
// Reference: Gen2Recomped OverworldState pattern
//=============================================================================

struct WorldState {
    // Map data
    RuntimeMap map;
    std::string map_id;
    
    // Tileset data (native 8×8 tiles + block definitions)
    RuntimeTileset tileset;
    std::string tileset_id;
    
    // Sprite data for this map
    std::vector<RuntimeSprite> sprites;
    RuntimeSpriteAtlas sprite_atlas;
    std::set<std::string> extracted_sprite_ids;
    
    // State flags
    bool valid = false;
};

//=============================================================================
// WARP ARRIVAL STATE
// Reference: Gen2Recomped OverworldController.lua warpEntryCell + standingOnWarp
//
// Gen2 warp triggering has two key state flags:
//
// 1. warpEntryCell - POSITIONAL ARRIVAL SUPPRESSION
//    When arriving via warp, store the landing cell. That exact cell is INERT
//    for warp triggering until player physically moves to a DIFFERENT cell.
//    This prevents bounce-back when warp destination is itself a warp tile.
//    Clear condition: player.cellX != entry.x OR player.cellY != entry.y
//    Checked in: update() every frame AND onStepComplete()
//
// 2. standingOnWarp - BIT_STANDING_ON_WARP
//    Gates collision warps (blocked step, edge exit). Does NOT gate onArrive.
//    Formula from Gen2Recomped refreshStandingOnWarp():
//      standingOnWarp = false
//      if warpAtCell(x,y) AND NOT (isWarpTileCell(x,y) AND NOT isDoorTileCell(x,y)):
//          standingOnWarp = true
//    Translation: Set if on warp square AND (NOT a warp-entrance tile OR is a door tile)
//
// Reference: pokecrystal home/overworld.asm CheckWarpsNoCollisionLoop,
//            engine/overworld/player_state.asm IsPlayerStandingOnDoorTileOrWarpTile
//=============================================================================

struct WarpArrivalState {
    // warpEntryCell: the exact cell where player landed after a warp
    bool entry_active = false;
    int32_t entry_x = 0;
    int32_t entry_y = 0;
    
    // standingOnWarp: BIT_STANDING_ON_WARP flag
    // Gates onCollision warps (blocked step, edge exit), NOT onArrive
    bool standing_on_warp = false;
    
    // is_door_auto_step: Tracks if current movement is a scripted door auto-step
    // Reference: Gen2Recomped OverworldController.lua lines 1293-1295
    // "if stepped and not scripted then self:onStepComplete() end"
    // Scripted movements (including door auto-step via scriptMove) do NOT call onStepComplete
    bool is_door_auto_step = false;
    
    // Set entry cell after warp arrival
    void set_entry(int32_t x, int32_t y) {
        entry_active = true;
        entry_x = x;
        entry_y = y;
    }
    
    // Clear entry suppression
    void clear_entry() {
        entry_active = false;
    }
    
    // Check if still on entry cell
    bool is_on_entry_cell(int32_t x, int32_t y) const {
        return entry_active && x == entry_x && y == entry_y;
    }
    
    // Check and clear entry if player moved off - call every frame AND onStepComplete
    // Reference: Gen2Recomped OverworldController.lua lines 1281-1283, 4137-4139
    void check_and_clear_entry(int32_t player_x, int32_t player_y) {
        if (entry_active && (player_x != entry_x || player_y != entry_y)) {
            clear_entry();
        }
    }
    
    // Refresh standingOnWarp flag based on current tile
    // Reference: Gen2Recomped OverworldController.lua refreshStandingOnWarp (lines 1348-1355)
    //
    // self.standingOnWarp = false
    // if self.map:warpAtCell(p.cellX, p.cellY)
    //    and not (self.map:isWarpTileCell(p.cellX, p.cellY)
    //             and not self.map:isDoorTileCell(p.cellX, p.cellY)) then
    //   self.standingOnWarp = true
    // end
    //
    // Logic: Set if warp exists AND NOT (warp-entrance-tile AND NOT door-tile)
    // Meaning: Clear if on a warp-entrance tile that's NOT a door (stairs, carpets, pits)
    void refresh_standing_on_warp(CollisionClass collision, bool has_warp) {
        standing_on_warp = false;
        if (has_warp) {
            bool is_warp_tile = collision_is_warp(collision);
            bool is_door = collision_is_door_warp(collision);
            // NOT (warp_tile AND NOT door) = NOT warp_tile OR door
            if (!is_warp_tile || is_door) {
                standing_on_warp = true;
            }
        }
    }
    
    // Can a collision warp (blocked step, edge exit) fire?
    // Reference: Gen2Recomped canCollisionWarp()
    bool can_collision_warp() const {
        return standing_on_warp;
    }
};

//=============================================================================
// PACKAGE CONTEXT
// Holds package reader and sprite resources for a runtime instance
// All data is loaded from the EMON native package - no ROM required
//
// OWNERSHIP: Per-runtime instance. Must NOT be process-global.
// Runtime A(package A) and Runtime B(package B) must be fully isolated.
//=============================================================================

struct PackageContext {
    PackageReader* package = nullptr;
    
    // Sprite data loaded from package
    SpriteObjPalettes obj_palettes;
    RuntimeSprite player_sprite;
    
    // Sprite cache - loaded on demand from package (per-instance)
    std::unordered_map<std::string, RuntimeSprite> sprite_cache;
    
    // Tileset cache - loaded on demand from package (per-instance)
    // Key is tileset_id, value is parsed RuntimeTileset
    // This cache is package-dependent and MUST be per-instance
    std::unordered_map<std::string, RuntimeTileset> tileset_cache;
    
    bool initialized = false;
};

// Load world state for a map from package
// Returns true on success, fills out the WorldState
// pkg_ctx: Package context (per-runtime instance, NOT global)
static bool load_world_state(
    PackageContext& pkg_ctx,
    const std::string& map_id,
    WorldState& state,
    std::string& error
) {
    if (!pkg_ctx.initialized) {
        error = "Package not initialized";
        return false;
    }
    
    // Load map from package
    auto map_opt = pkg_ctx.package->load_map(map_id);
    if (!map_opt) {
        error = "Failed to load map from package: " + map_id;
        return false;
    }
    
    state.map = std::move(*map_opt);
    state.map_id = map_id;
    
    // Load or retrieve tileset from package (per-instance cache)
    std::string tileset_id = state.map.tileset_id;
    state.tileset_id = tileset_id;
    
    if (pkg_ctx.tileset_cache.find(tileset_id) == pkg_ctx.tileset_cache.end()) {
        // Load tileset from package
        auto tileset_data = pkg_ctx.package->load_tileset_data(tileset_id);
        if (!tileset_data) {
            error = "Failed to load tileset from package: " + tileset_id;
            return false;
        }
        
        // Parse tileset data into RuntimeTileset (native 8×8 tiles + blocks)
        auto tileset = RuntimeTileset::from_package_data(tileset_id, *tileset_data);
        pkg_ctx.tileset_cache[tileset_id] = std::move(tileset);
    }
    
    state.tileset = pkg_ctx.tileset_cache[tileset_id];
    
    // Load NPC sprites for this map from package
    state.sprites.clear();
    state.extracted_sprite_ids.clear();
    
    // Always include player sprite
    state.sprites.push_back(pkg_ctx.player_sprite);
    state.extracted_sprite_ids.insert(pkg_ctx.player_sprite.sprite_id);
    
    // Load NPC sprites from package
    for (const auto& obj : state.map.objects) {
        if (state.extracted_sprite_ids.contains(obj.sprite_id)) continue;
        
        // Check cache first (per-instance)
        auto cache_it = pkg_ctx.sprite_cache.find(obj.sprite_id);
        if (cache_it != pkg_ctx.sprite_cache.end()) {
            state.sprites.push_back(cache_it->second);
            state.extracted_sprite_ids.insert(obj.sprite_id);
            continue;
        }
        
        // Load from package
        auto sprite_opt = pkg_ctx.package->load_sprite(obj.sprite_id);
        if (sprite_opt) {
            pkg_ctx.sprite_cache[obj.sprite_id] = *sprite_opt;  // Cache it
            state.sprites.push_back(std::move(*sprite_opt));
            state.extracted_sprite_ids.insert(obj.sprite_id);
        }
        // Missing sprites silently skipped - non-fatal
    }
    
    // Render sprite atlas using the render_sprite_atlas function
    // Now in enginemon:: namespace as part of engine layer
    state.sprite_atlas = render_sprite_atlas(
        state.sprites, pkg_ctx.obj_palettes, 1);
    
    state.valid = true;
    return true;
}

//=============================================================================
// WORLD TRANSITION
// Reference: Gen2Recomped Warp.lua takeWarp, OverworldState.loadMap
// Handles all state that needs resetting when changing maps
//=============================================================================

struct TransitionContext {
    // Renderers
    TileRenderer* tile_renderer = nullptr;
    SpriteRenderer* sprite_renderer = nullptr;
    VulkanBootstrap* vulkan = nullptr;
    
    // Game systems
    HeadlessGameLoop* game_loop = nullptr;
    WorldManager* world_manager = nullptr;
    
    // Package context (per-runtime instance, NOT global)
    PackageContext* pkg_ctx = nullptr;
    
    // Presentation state (reset on transition)
    float* player_start_x = nullptr;
    float* player_start_y = nullptr;
    float* player_target_x = nullptr;
    float* player_target_y = nullptr;
    bool* player_moving = nullptr;
    int* step_frame = nullptr;
    int* anim_clock = nullptr;
    
    // Warp arrival state
    WarpArrivalState* warp_state = nullptr;
    
    // Transition type flag - connections do NOT set warpEntryCell
    bool is_warp_arrival = true;
};

// Transition to a new map - loads data, updates renderers, resets presentation
// Returns true on success
static bool transition_to_map(
    const std::string& new_map_id,
    int32_t player_x,
    int32_t player_y,
    Direction player_facing,
    WorldState& world_state,
    TransitionContext& ctx,
    std::string& error
) {
    //=========================================================================
    // CRITICAL: Wait for all GPU work to complete before replacing resources
    //
    // Vulkan rule: resources referenced by pending/submitted command buffers
    // must remain alive until that GPU work has completed.
    //
    // The renderers' set_tileset/set_atlas/build_map methods destroy old 
    // textures, buffers, and descriptor-bound resources. If any frame is 
    // still in flight referencing these, we get undefined behavior.
    //
    // vkDeviceWaitIdle is acceptable here because map transitions are 
    // infrequent (typically <1 per minute of gameplay).
    //=========================================================================
    vkDeviceWaitIdle(ctx.vulkan->device());
    
    // Load new map world state (uses per-instance package context)
    if (!load_world_state(*ctx.pkg_ctx, new_map_id, world_state, error)) {
        return false;
    }
    
    // Resolve palette row based on map environment + time policy
    // For now, use Day as the RTC time (future: integrate with actual RTC)
    PaletteRow active_palette = TileRenderer::resolve_palette_row(
        world_state.map.environment,
        world_state.map.time_policy,
        PaletteRow::Day  // RTC time - hardcoded to Day for now
    );
    
    // Update tile renderer with new tileset and map
    if (!ctx.tile_renderer->set_tileset(*ctx.vulkan, world_state.tileset, active_palette)) {
        error = "Failed to upload tileset for " + new_map_id;
        return false;
    }
    if (!ctx.tile_renderer->build_map(*ctx.vulkan, world_state.map, world_state.tileset)) {
        error = "Failed to build map geometry for " + new_map_id;
        return false;
    }
    
    // Update sprite renderer with new sprites
    if (!ctx.sprite_renderer->set_atlas(*ctx.vulkan, world_state.sprite_atlas)) {
        error = "Failed to upload sprite atlas for " + new_map_id;
        return false;
    }
    ctx.sprite_renderer->set_sprite_data(world_state.sprites);
    
    // Update game loop with new map
    ctx.game_loop->load_map(world_state.map);
    
    // Set collision data for new map using per-tileset collision
    ctx.game_loop->set_collision_data([&world_state](int32_t x, int32_t y) -> uint8_t {
        return get_collision_from_blocks(world_state.map.blocks, 
            world_state.tileset.collision, world_state.map.width, x, y);
    });
    
    // Clear NPCs and add new ones from the new map
    ctx.game_loop->clear_npcs();
    
    // Set RNG seed for deterministic NPC movement
    uint32_t map_seed = 0;
    for (char c : world_state.map.map_id) {
        map_seed = map_seed * 31 + static_cast<uint32_t>(c);
    }
    ctx.game_loop->set_rng_seed(map_seed);
    
    // Add NPCs from new map
    for (const auto& obj : world_state.map.objects) {
        NpcState npc;
        npc.id = obj.local_id;
        npc.x = obj.x;
        npc.y = obj.y;
        npc.facing = movement_data_to_facing(obj.movement_type);
        npc.is_moving = false;
        npc.is_trainer = obj.is_trainer;
        npc.script_id = obj.script_id;
        npc.visibility_flag = obj.visibility_flag;
        npc.visible = true;
        
        npc.behavior = movement_data_to_behavior(obj.movement_type);
        npc.radius_x = obj.movement_radius_x;
        npc.radius_y = obj.movement_radius_y;
        npc.init_x = obj.x;
        npc.init_y = obj.y;
        npc.idle_timer = 30 + (obj.local_id * 17) % 98;
        npc.target_x = obj.x;
        npc.target_y = obj.y;
        npc.move_progress = 0;
        npc.frozen = false;
        
        ctx.game_loop->add_npc(npc);
    }
    
    // Spawn player at destination
    ctx.game_loop->spawn_player(player_x, player_y, player_facing);
    
    // Reset presentation/interpolation state
    *ctx.player_start_x = player_x * 16.0f;
    *ctx.player_start_y = player_y * 16.0f;
    *ctx.player_target_x = *ctx.player_start_x;
    *ctx.player_target_y = *ctx.player_start_y;
    *ctx.player_moving = false;
    *ctx.step_frame = 0;
    *ctx.anim_clock = 0;
    
    //=========================================================================
    // WARP ARRIVAL STATE
    // Reference: Gen2Recomped OverworldController.lua startWarpTo (lines 4769-4810)
    //
    // WARP arrivals:
    //   1. Set warpEntryCell = destination (suppresses immediate re-warp)
    //   2. If landing on door tile (0x71 COLL_DOOR, 0x7B COLL_CAVE) AND can step south:
    //      - Clear warpEntryCell BEFORE auto-step begins
    //      - Begin real one-cell scripted movement south
    //   3. If door tile but blocked south: face south, keep warpEntryCell
    //   4. Non-door tiles: keep warpEntryCell until player moves to different cell
    //
    // CONNECTION arrivals:
    //   - NEVER set warpEntryCell
    //   - Warps on destination map are immediately active
    //=========================================================================
    
    if (ctx.warp_state) {
        if (ctx.is_warp_arrival) {
            // WARP ARRIVAL: Set warpEntryCell
            ctx.warp_state->set_entry(player_x, player_y);
            
            // Get collision class at landing position (returns semantic CollisionClass)
            CollisionClass landing_coll = get_collision_from_blocks(
                world_state.map.blocks, world_state.tileset.collision,
                world_state.map.width, player_x, player_y);
            
            // Check if landing on a doorway tile
            // Reference: Gen2Recomped Map.gen2IsDoorway, startWarpTo lines 4791-4805
            if (collision_is_door_warp(landing_coll)) {
                // Check if movement south is legal
                // Reference: Gen2Recomped Collision.canMove check
                int32_t south_y = player_y + 1;
                
                // Check collision at south cell
                CollisionClass south_coll = get_collision_from_blocks(
                    world_state.map.blocks, world_state.tileset.collision,
                    world_state.map.width, player_x, south_y);
                
                // Check for NPC collision at south cell
                bool npc_blocking = false;
                for (const auto& npc : ctx.game_loop->npcs()) {
                    if (npc.x == player_x && npc.y == south_y && npc.visible) {
                        npc_blocking = true;
                        break;
                    }
                }
                
                // South cell is walkable if it's a walkable semantic collision class
                bool south_walkable = collision_is_walkable(south_coll);
                bool can_step_south = south_walkable && !npc_blocking;
                
                if (can_step_south) {
                    // Reference: Gen2Recomped startWarpTo lines 4800-4802
                    // "self.warpEntryCell = nil" BEFORE scriptMove
                    // "self:scriptMove(self.player, "down", 1)"
                    //
                    // Clear warpEntryCell BEFORE auto-step begins
                    ctx.warp_state->clear_entry();
                    
                    // Mark this as a scripted door auto-step
                    // Reference: Gen2Recomped lines 1293-1295
                    // "if stepped and not scripted then self:onStepComplete() end"
                    // Scripted movements do NOT call onStepComplete - skip warp evaluation
                    ctx.warp_state->is_door_auto_step = true;
                    
                    // Execute door auto-step IMMEDIATELY (not deferred)
                    // Reference: Gen2Recomped calls scriptMove() inside the transition callback
                    // Player starts at door, movement target is one cell south
                    ctx.game_loop->spawn_player(player_x, player_y, Direction::Down);
                    ctx.game_loop->player().target_x = player_x;
                    ctx.game_loop->player().target_y = south_y;
                    ctx.game_loop->player().is_moving = true;
                    
                    // Set up presentation state for the auto-step
                    *ctx.player_start_x = player_x * 16.0f;
                    *ctx.player_start_y = player_y * 16.0f;
                    *ctx.player_target_x = player_x * 16.0f;
                    *ctx.player_target_y = south_y * 16.0f;
                    *ctx.player_moving = true;
                    *ctx.step_frame = 0;
                } else {
                    // Cannot step south (blocked) - face south, keep warpEntryCell
                    // Reference: Gen2Recomped startWarpTo line 4806
                    // "self.player.facing = "down""
                    ctx.game_loop->spawn_player(player_x, player_y, Direction::Down);
                    // warpEntryCell already set above
                }
            }
            // Non-door tiles: warpEntryCell already set, no auto-step
        } else {
            // CONNECTION ARRIVAL: Do NOT set warpEntryCell
            // Warps on destination map are immediately active after seam step
        }
    }
    
    return true;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <package_path>\n";
        return 1;
    }
    
    //=========================================================================
    // STEP 1: Load EMON package
    //=========================================================================
    
    auto package = PackageReader::open(argv[1]);
    if (!package) {
        std::cerr << "Failed to load package\n";
        return 1;
    }
    
    if (!package->validate()) {
        std::cerr << "Package validation failed\n";
        return 1;
    }

    //=========================================================================
    // STEP 2: Initialize package context (per-runtime instance, NOT global)
    //
    // OWNERSHIP: This PackageContext is owned by this runtime instance.
    // Runtime A(package A) and Runtime B(package B) must use separate contexts.
    //=========================================================================
    
    PackageContext pkg_ctx;
    
    // Load player sprite from package
    auto player_sprite_opt = package->load_sprite("chris");
    if (!player_sprite_opt) {
        std::cerr << "Failed to load player sprite from package\n";
        return 1;
    }
    pkg_ctx.player_sprite = std::move(*player_sprite_opt);
    
    // Load OBJ palettes from package
    auto obj_palettes_opt = package->load_obj_palettes();
    if (!obj_palettes_opt) {
        std::cerr << "Failed to load OBJ palettes from package\n";
        return 1;
    }
    pkg_ctx.obj_palettes = std::move(*obj_palettes_opt);
    
    // Set up package context
    pkg_ctx.package = package.get();
    pkg_ctx.initialized = true;
    
    //=========================================================================
    // STEP 3: Load initial map from package
    //=========================================================================
    
    WorldState world_state;
    std::string load_error;
    // TEST: Spawn inside Route 29/Route 46 Gate near south exit warp at (4,7)
    if (!load_world_state(pkg_ctx, "route_29_route_46_gate", world_state, load_error)) {
        std::cerr << "Failed to load initial map: " << load_error << "\n";
        return 1;
    }
    
    //=========================================================================
    // STEP 4: Set up WorldManager for warp/connection handling
    //=========================================================================
    
    WorldManager world_manager;
    GameState game_state;
    
    // Set up map loader callback - captures pkg_ctx by reference (per-instance)
    world_manager.set_map_loader([&pkg_ctx](const std::string& map_id) -> std::optional<RuntimeMap> {
        if (!pkg_ctx.initialized) return std::nullopt;
        return pkg_ctx.package->load_map(map_id);
    });
    
    // Load initial map into WorldManager
    // TEST: Gate interior, spawn at (4,6) facing down - walk down to test warp at (4,7)
    world_manager.load_map("route_29_route_46_gate");
    game_state.player.current_map_id = "route_29_route_46_gate";
    game_state.player.x = 4;
    game_state.player.y = 6;
    game_state.player.facing = Direction::Down;

    //=========================================================================
    // STEP 4b2: Load Crystal font from package
    //=========================================================================
    
    RuntimeFontAtlas runtime_font_atlas;
    bool has_crystal_font = false;
    
    auto font_data = package->load_font_atlas("crystal_main");
    if (font_data && RuntimeFontAtlas::from_package_data(*font_data, runtime_font_atlas)) {
        has_crystal_font = true;
    }

    //=========================================================================
    // STEP 4c: Set up HeadlessGameLoop with collision/interaction/scripting
    //=========================================================================
    
    HeadlessGameLoop game_loop;
    game_loop.load_map(world_state.map);
    
    // Set collision data using per-tileset collision
    // Lambda captures world_state by reference for the current map
    game_loop.set_collision_data([&world_state](int32_t x, int32_t y) -> uint8_t {
        return get_collision_from_blocks(world_state.map.blocks, 
            world_state.tileset.collision, world_state.map.width, x, y);
    });
    
    // Set RNG seed for deterministic NPC movement (hash map_id string for seed)
    uint32_t map_seed = 0;
    for (char c : world_state.map.map_id) {
        map_seed = map_seed * 31 + static_cast<uint32_t>(c);
    }
    game_loop.set_rng_seed(map_seed);
    
    // Add NPCs from map objects for collision, interaction, and autonomous movement
    // Reference: pokecrystal/maps/NewBarkTown.asm object_events
    for (const auto& obj : world_state.map.objects) {
        NpcState npc;
        npc.id = obj.local_id;
        npc.x = obj.x;
        npc.y = obj.y;
        npc.facing = movement_data_to_facing(obj.movement_type);
        npc.is_moving = false;
        npc.is_trainer = obj.is_trainer;
        npc.script_id = obj.script_id;
        npc.visibility_flag = obj.visibility_flag;
        npc.visible = true;  // TODO: check visibility flags
        
        // Initialize movement behavior from Crystal movement_type
        // Reference: pokecrystal SpriteMovementData table
        npc.behavior = movement_data_to_behavior(obj.movement_type);
        npc.radius_x = obj.movement_radius_x;
        npc.radius_y = obj.movement_radius_y;
        npc.init_x = obj.x;
        npc.init_y = obj.y;
        npc.idle_timer = 30 + (obj.local_id * 17) % 98;  // Stagger initial timers
        npc.target_x = obj.x;
        npc.target_y = obj.y;
        npc.move_progress = 0;
        npc.frozen = false;
        
        game_loop.add_npc(npc);
    }
    
    // TEST: Spawn player at (4,6) facing down in Gate - walk down to test warp at (4,7)
    game_loop.spawn_player(4, 6, Direction::Down);
    
    //=========================================================================
    // STEP 4d: Set up Lua scripting runtime
    //=========================================================================
    
    LuaRuntime lua_runtime;
    
    // Set error handler to log script errors
    lua_runtime.set_error_handler([](const std::string& error, const std::string& traceback) {
        std::cerr << "[SCRIPT ERROR] " << error << "\n";
        if (!traceback.empty()) {
            std::cerr << traceback << "\n";
        }
    });
    
    // Connect game loop to Lua runtime
    game_loop.set_lua_runtime(&lua_runtime);
    
    // Set up script loader that loads pre-compiled Lua from package
    // Scripts are already stored with global ScriptId: "map_id::local_script_id"
    // The map events contain the full global script_id after compilation
    game_loop.set_script_loader([&package](const std::string& script_id) -> std::string {
        auto lua_code = package->load_script(script_id);
        if (!lua_code) return "";
        return *lua_code;
    });
    
    //=========================================================================
    // DIALOG STATE
    // Shared between UI API and render loop for visible textbox
    //=========================================================================
    
    TextboxState dialog_state;
    uint64_t dialog_open_frame = 0;  // Frame when dialog opened (to ignore opening press)
    uint64_t frame_counter = 0;      // Current frame number
    
    // Pointer to textbox renderer - set after renderer is created
    TextboxRenderer* textbox_renderer_ptr = nullptr;
    
    // Wire up UI API callbacks to update dialog_state for visible rendering
    // Use per-runtime hooks (NOT process-global callbacks) for proper instance isolation
    auto& hooks = lua_runtime.get_presentation_hooks();
    
    hooks.text = [&dialog_state, &dialog_open_frame, &frame_counter, &textbox_renderer_ptr](const std::string& text) {
        // Legacy string callback - convert to semantic sequence
        // This creates a simple sequence: Text(content), Done
        RuntimeTextSequence seq;
        seq.elements.push_back(RuntimeTextElement::make_text(text));
        seq.elements.push_back(RuntimeTextElement::make_done());
        
        dialog_state.open_with_sequence(seq);
        dialog_open_frame = frame_counter;
        if (textbox_renderer_ptr && textbox_renderer_ptr->font_atlas()) {
            textbox_renderer_ptr->parse_text_pages(dialog_state);
        }
    };
    
    // Semantic text sequence callback - preserves LINE/CONT/PARA distinctions
    hooks.text_sequence = [&dialog_state, &dialog_open_frame, &frame_counter, &textbox_renderer_ptr](const RuntimeTextSequence& seq) {
        dialog_state.open_with_sequence(seq);
        dialog_open_frame = frame_counter;
        if (textbox_renderer_ptr && textbox_renderer_ptr->font_atlas()) {
            textbox_renderer_ptr->parse_text_pages(dialog_state);
        }
    };
    
    hooks.close_text = [&dialog_state]() {
        dialog_state.close();
    };
    
    hooks.open_text = [&dialog_state]() {
        // Prepare for text (open_text is called before text content arrives)
    };
    
    // Set up interaction callback (script execution handled by game_loop.handle_interact())
    game_loop.set_interaction_callback([](const InteractionResult& ir) {
        // Callback for any external interaction hooks (currently empty)
    });

    //=========================================================================
    // STEP 5: Initialize SDL3 + Vulkan
    //=========================================================================
    
    constexpr uint32_t LOGICAL_WIDTH = 320;
    constexpr uint32_t LOGICAL_HEIGHT = 288;
    constexpr uint32_t SCALE_FACTOR = 2;
    
    Sdl3Platform platform;
    WindowConfig window_config;
    window_config.title = "Enginemon";
    window_config.width = LOGICAL_WIDTH * SCALE_FACTOR;
    window_config.height = LOGICAL_HEIGHT * SCALE_FACTOR;
    window_config.resizable = true;
    
    if (!platform.initialize(window_config)) {
        std::cerr << "Failed to initialize SDL3\n";
        return 1;
    }
    
    VulkanBootstrap vulkan;
    VulkanConfig vulkan_config;
    vulkan_config.enable_validation = true;
    vulkan_config.vsync = true;
    
    if (!vulkan.initialize(platform, vulkan_config)) {
        std::cerr << "Failed to initialize Vulkan\n";
        return 1;
    }
    
    //=========================================================================
    // STEP 6: Initialize tile renderer
    //=========================================================================
    
    TileRenderer renderer;
    TileRendererConfig render_config;
    render_config.logical_width = LOGICAL_WIDTH;
    render_config.logical_height = LOGICAL_HEIGHT;
    render_config.scale_factor = SCALE_FACTOR;
    render_config.tile_size = 8;
    render_config.block_size = 32;
    
    if (!renderer.initialize(vulkan, render_config)) {
        std::cerr << "Failed to initialize tile renderer\n";
        return 1;
    }

    // Resolve palette row based on map environment + time policy
    PaletteRow initial_palette = TileRenderer::resolve_palette_row(
        world_state.map.environment,
        world_state.map.time_policy,
        PaletteRow::Day  // RTC time - hardcoded to Day for now
    );

    if (!renderer.set_tileset(vulkan, world_state.tileset, initial_palette)) {
        std::cerr << "Failed to upload tileset\n";
        return 1;
    }
    
    if (!renderer.build_map(vulkan, world_state.map, world_state.tileset)) {
        std::cerr << "Failed to build map geometry\n";
        return 1;
    }
    
    renderer.update_viewport(platform.width(), platform.height());
    renderer.set_view(0, 0);
    
    //=========================================================================
    // STEP 6b: Initialize sprite renderer
    //=========================================================================
    
    SpriteRenderer sprite_renderer;
    SpriteRendererConfig sprite_config;
    sprite_config.logical_width = LOGICAL_WIDTH;
    sprite_config.logical_height = LOGICAL_HEIGHT;
    
    if (!sprite_renderer.initialize(vulkan, sprite_config)) {
        std::cerr << "Failed to initialize sprite renderer\n";
        return 1;
    }
    
    if (!sprite_renderer.set_atlas(vulkan, world_state.sprite_atlas)) {
        std::cerr << "Failed to upload sprite atlas\n";
        return 1;
    }
    
    sprite_renderer.set_sprite_data(world_state.sprites);
    
    // Allocate buffers for player + NPCs
    if (!sprite_renderer.prepare_buffers(vulkan, 16)) {
        std::cerr << "Failed to allocate sprite buffers\n";
        return 1;
    }
    
    sprite_renderer.update_viewport(platform.width(), platform.height());
    sprite_renderer.set_view(0, 0);

    //=========================================================================
    // STEP 6c: Initialize textbox renderer
    //=========================================================================
    
    TextboxRenderer textbox_renderer;
    TextboxRendererConfig textbox_config;
    textbox_config.logical_width = LOGICAL_WIDTH;
    textbox_config.logical_height = LOGICAL_HEIGHT;
    
    if (!textbox_renderer.initialize(vulkan, textbox_config)) {
        std::cerr << "Failed to initialize textbox renderer\n";
        return 1;
    }
    
    // Load Crystal font if extracted
    if (has_crystal_font) {
        if (!textbox_renderer.load_font_atlas(vulkan, runtime_font_atlas)) {
            std::cerr << "Failed to load Crystal font atlas\n";
            return 1;
        }
    }
    
    textbox_renderer.update_viewport(platform.width(), platform.height());
    
    // Set the pointer for the dialog callbacks
    textbox_renderer_ptr = &textbox_renderer;

    //=========================================================================
    // Movement presentation state (separate from game logic)
    //=========================================================================
    
    // Reference: Gen2Recomped Player.lua - stepFlip, animClock, walkPhase
    constexpr int STEP_FRAMES = 16;
    int step_frame = 0;
    bool step_flip = false;
    int anim_clock = 0;
    
    // Interpolation state for smooth rendering
    float player_start_x = game_loop.player().x * 16.0f;
    float player_start_y = game_loop.player().y * 16.0f;
    float player_target_x = player_start_x;
    float player_target_y = player_start_y;
    bool player_moving = false;
    
    //=========================================================================
    // Warp arrival state
    // Reference: Gen2Recomped OverworldController.lua warpEntryCell + standingOnWarp
    //=========================================================================
    
    WarpArrivalState warp_state;
    
    //=========================================================================
    // Set up transition context for warp/connection handling
    //=========================================================================
    
    TransitionContext transition_ctx;
    transition_ctx.tile_renderer = &renderer;
    transition_ctx.sprite_renderer = &sprite_renderer;
    transition_ctx.vulkan = &vulkan;
    transition_ctx.game_loop = &game_loop;
    transition_ctx.world_manager = &world_manager;
    transition_ctx.pkg_ctx = &pkg_ctx;  // Per-instance package context
    transition_ctx.player_start_x = &player_start_x;
    transition_ctx.player_start_y = &player_start_y;
    transition_ctx.player_target_x = &player_target_x;
    transition_ctx.player_target_y = &player_target_y;
    transition_ctx.player_moving = &player_moving;
    transition_ctx.step_frame = &step_frame;
    transition_ctx.anim_clock = &anim_clock;
    transition_ctx.warp_state = &warp_state;
    
    //=========================================================================
    // STEP 7: Main render loop
    //
    // TIMING ARCHITECTURE (Audit 8 fix):
    //   Simulation runs at fixed 60 Hz regardless of render rate.
    //   VSync/VRR/uncapped rendering does not affect gameplay speed.
    //   Simulation is deterministic: same inputs + same ticks = same results.
    //=========================================================================
    
    InputSystem input;
    bool running = true;
    float camera_x = 0, camera_y = 0;
    
    // Fixed-timestep simulation scheduler (60 Hz)
    SimulationScheduler sim_scheduler(TICK_60HZ);
    
    // Helper to get monotonic time in nanoseconds
    auto get_time_ns = []() -> int64_t {
        using namespace std::chrono;
        return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
    };
    
    // Initialize scheduler with current time
    sim_scheduler.reset(get_time_ns());
    
    while (running) {
        PlatformEvent event = platform.poll_events(input);
        
        if (event == PlatformEvent::Quit) {
            running = false;
            continue;
        }
        
        // Handle resize
        if (event == PlatformEvent::Resized || platform.was_resized()) {
            vulkan.recreate_swapchain(platform.width(), platform.height());
            renderer.update_viewport(platform.width(), platform.height());
            sprite_renderer.update_viewport(platform.width(), platform.height());
            textbox_renderer.update_viewport(platform.width(), platform.height());
            platform.clear_resize_flag();
        }
        
        //=====================================================================
        // Fixed-timestep simulation
        // Run simulation ticks based on elapsed time, not render rate.
        // This ensures deterministic gameplay regardless of VSync/FPS.
        //=====================================================================
        
        int64_t current_time_ns = get_time_ns();
        SchedulerTickResult tick_schedule = sim_scheduler.update(current_time_ns);
        
        // Process simulation ticks (may be 0, 1, or more per render frame)
        for (int32_t tick_i = 0; tick_i < tick_schedule.ticks_to_run; ++tick_i) {
            //=================================================================
            // Input handling - route through HeadlessGameLoop
            // Input is sampled once per simulation tick
            //=================================================================
            
            frame_counter++;
            
            if (dialog_state.is_open && dialog_state.waiting_for_input) {
                // Dialog is open - A-button advances/closes based on text state
                bool can_advance = (frame_counter > dialog_open_frame);
                
                if (can_advance && input.snapshot().was_pressed(InputButton::A)) {
                    bool more_text = dialog_state.advance_page();
                    
                    if (!more_text) {
                        // No more pages - resume script which will call close_text
                        ScriptState script_state = lua_runtime.get_state(game_loop.active_coroutine());
                        if (script_state == ScriptState::Yielded) {
                            game_loop.resume_script();
                        }
                    }
                }
                // Block all other input while dialog is open
            }
            else if (!game_loop.is_input_locked()) {
                InputAction action = InputAction::None;
                
                // A-button (Space/Z key for interaction) - check FIRST with was_pressed
            if (input.snapshot().was_pressed(InputButton::A)) {
                action = InputAction::Interact;
            }
            // Movement keys (continuous hold)
            else if (input.snapshot().is_held(InputButton::Up)) {
                action = InputAction::MoveUp;
            } else if (input.snapshot().is_held(InputButton::Down)) {
                action = InputAction::MoveDown;
            } else if (input.snapshot().is_held(InputButton::Left)) {
                action = InputAction::MoveLeft;
            } else if (input.snapshot().is_held(InputButton::Right)) {
                action = InputAction::MoveRight;
            }
            
            if (action != InputAction::None) {
                InputResult result = game_loop.process_input(action);
                
                if (result.accepted && !result.blocked && action != InputAction::Interact) {
                    // Movement started - set up interpolation
                    player_moving = true;
                    step_frame = 0;
                    
                    // The previous position is current position minus the movement delta
                    int dx = 0, dy = 0;
                    direction_to_delta(game_loop.player().facing, dx, dy);
                    player_start_x = (game_loop.player().target_x - dx) * 16.0f;
                    player_start_y = (game_loop.player().target_y - dy) * 16.0f;
                    player_target_x = game_loop.player().target_x * 16.0f;
                    player_target_y = game_loop.player().target_y * 16.0f;
                }
                
                //=============================================================
                // BLOCKED STEP WARP CHECK
                // Reference: Gen2Recomped handleInput lines 1427-1433
                //
                // if result == "blocked" and self:canCollisionWarp() then
                //     w = Warp.onCollision(map, carpets, px, py, dir)
                //     if w then takeWarp(w.def) return end
                // end
                //
                // This fires when movement is blocked while standing on a warp
                // square with standingOnWarp set (route-gate doorways, docks).
                //=============================================================
                if (result.blocked && action != InputAction::Interact && warp_state.can_collision_warp()) {
                    int32_t px = game_loop.player().x;
                    int32_t py = game_loop.player().y;
                    Direction pfacing = game_loop.player().facing;
                    
                    const RuntimeWarp* warp_at_pos = world_manager.get_warp_at(px, py);
                    if (warp_at_pos) {
                        // Check extraCheck rules for collision warp
                        // Reference: Gen2Recomped Warp.lua extraCheck
                        bool facing_edge = false;
                        int map_width_cells = world_state.map.width * 2;
                        int map_height_cells = world_state.map.height * 2;
                        
                        if ((pfacing == Direction::Up && py == 0) ||
                            (pfacing == Direction::Down && py == map_height_cells - 1) ||
                            (pfacing == Direction::Left && px == 0) ||
                            (pfacing == Direction::Right && px == map_width_cells - 1)) {
                            facing_edge = true;
                        }
                        
                        if (facing_edge) {
                            // Take the warp
                            transition_ctx.is_warp_arrival = true;
                            auto warp_result = world_manager.execute_warp(*warp_at_pos, game_state);
                            if (warp_result.success) {
                                std::string trans_error;
                                transition_to_map(
                                    warp_result.target_map_id,
                                    warp_result.target_x,
                                    warp_result.target_y,
                                    warp_result.target_facing,
                                    world_state,
                                    transition_ctx,
                                    trans_error
                                );
                            }
                        }
                    }
                }
            }
        }

        //=====================================================================
        // EVERY-FRAME WARP ENTRY CELL CHECK
        // Reference: Gen2Recomped update() lines 1281-1283
        //
        // local entry = self.warpEntryCell
        // if entry and (self.player.cellX ~= entry.x or self.player.cellY ~= entry.y) then
        //     self.warpEntryCell = nil
        // end
        //
        // This runs EVERY FRAME, not just on step complete. It clears the entry
        // cell the instant the player's logical position differs, including
        // during a scripted auto-step.
        //=====================================================================
        warp_state.check_and_clear_entry(game_loop.player().x, game_loop.player().y);

        //=====================================================================
        // Simulation tick
        //=====================================================================
        
        TickResult tick_result{};
        if (dialog_state.is_open && dialog_state.waiting_for_input) {
            // Don't tick while waiting for dialog input - prevents auto-resume
        } else {
            tick_result = game_loop.tick();
        }
        
        // Update movement interpolation
        if (player_moving) {
            step_frame++;
            anim_clock++;
            
            if (tick_result.movement_complete || step_frame >= STEP_FRAMES) {
                // Movement complete
                player_moving = false;
                player_start_x = game_loop.player().x * 16.0f;
                player_start_y = game_loop.player().y * 16.0f;
                player_target_x = player_start_x;
                player_target_y = player_start_y;
                step_flip = !step_flip;
                
                //=============================================================
                // SCRIPTED MOVEMENT CHECK
                // Reference: Gen2Recomped OverworldController.lua lines 1293-1295
                //   "if stepped and not scripted then self:onStepComplete() end"
                //
                // Gen2 'scripted' = runner:isRunning() or #scriptMoves > 0 or ...
                // Door auto-step is a scriptMove, so onStepComplete is skipped.
                // Enginemon translation: is_door_auto_step flag tracks this.
                //=============================================================
                
                bool is_scripted_movement = warp_state.is_door_auto_step;
                
                // Clear scripted flag after the movement completes
                // (Gen2: scriptMoves queue entry is removed by updateScriptMoves)
                warp_state.is_door_auto_step = false;
                
                if (!is_scripted_movement) {
                    //=========================================================
                    // onStepComplete() - ONLY FOR PLAYER-INITIATED MOVEMENT
                    // Reference: Gen2Recomped OverworldController.lua onStepComplete
                    // Reference: Gen2Recomped Warp.lua onArrive, onCollision
                    //
                    // Crystal warp triggering model (in order):
                    //   1. Clear warpEntryCell if player moved to a different cell
                    //   2. Refresh standingOnWarp flag based on current tile
                    //   3. If still on warpEntryCell, skip warp checks entirely
                    //   4. Check Warp.onArrive - door/warp tiles fire immediately
                    //   5. If d-pad held, also check Warp.onCollision (exit carpets)
                    //   6. Check connection crossing (at map edge)
                    //
                    // IMPORTANT: connection checks run BEFORE warp checks to match
                    // the reference - but in practice they shouldn't conflict since
                    // connections are at map edges and warps are not usually there.
                    //=========================================================
                    
                    int32_t px = game_loop.player().x;
                    int32_t py = game_loop.player().y;
                    Direction pfacing = game_loop.player().facing;
                    
                    //=========================================================
                    // onStepComplete() WARP EVALUATION
                    // Reference: Gen2Recomped OverworldController.lua lines 4130-4170
                    //
                    // 1. Clear warpEntryCell if player moved to different cell
                    // 2. refreshStandingOnWarp()
                    // 3. If warpEntryCell still active: skip warp evaluation
                    // 4. Warp.onArrive(current cell)
                    // 5. If none AND (d-pad held OR forcedWarp):
                    //    Warp.onCollision(current cell, facing)
                    // 6. takeWarp if resolved
                    //=========================================================
                    
                    // Step 1: Clear warp entry suppression if player moved off entry cell
                    warp_state.check_and_clear_entry(px, py);
                    
                    // Get semantic collision class at player position
                    CollisionClass coll = get_collision_from_blocks(
                        world_state.map.blocks, world_state.tileset.collision,
                        world_state.map.width, px, py);
                    
                    // Check if there's a warp at current position
                    const RuntimeWarp* warp_at_pos = world_manager.get_warp_at(px, py);
                    bool has_warp = (warp_at_pos != nullptr);
                    
                    // Step 2: Refresh standingOnWarp flag based on current tile
                    warp_state.refresh_standing_on_warp(coll, has_warp);
                    
                    // Step 3: If still on entry cell, skip warp evaluation entirely
                    // Reference: Gen2Recomped lines 4153-4154: "if entry then" -> skip
                    bool on_entry_cell = warp_state.is_on_entry_cell(px, py);
                    
                    const RuntimeWarp* warp_to_take = nullptr;
                    
                    if (!on_entry_cell) {
                        // Step 4: Check Warp.onArrive - warp tiles fire immediately
                        // Reference: Gen2Recomped Warp.lua onArrive (lines 17-22)
                        // A warp fires if: warpAtCell exists AND isWarpTileCell (gen2IsEntrance)
                        if (has_warp && collision_is_warp(coll)) {
                            warp_to_take = warp_at_pos;
                        }
                        
                        // Step 5: Check Warp.onCollision if no onArrive warp
                        // Reference: Gen2Recomped lines 4159-4161
                        // if not w and (self:dirHeld() or self.forcedWarp) then
                        //     w = Warp.onCollision(...)
                        // end
                        if (!warp_to_take && has_warp) {
                            bool dir_held = input.snapshot().is_held(InputButton::Up) ||
                                           input.snapshot().is_held(InputButton::Down) ||
                                           input.snapshot().is_held(InputButton::Left) ||
                                           input.snapshot().is_held(InputButton::Right);
                            // Note: forcedWarp not implemented yet (Seafoam currents)
                            
                            if (dir_held) {
                                // Warp.onCollision requires extraCheck to pass
                                // Reference: Gen2Recomped Warp.lua extraCheck
                                bool facing_edge = false;
                                int map_width_cells = world_state.map.width * 2;
                                int map_height_cells = world_state.map.height * 2;
                                
                                if ((pfacing == Direction::Up && py == 0) ||
                                    (pfacing == Direction::Down && py == map_height_cells - 1) ||
                                    (pfacing == Direction::Left && px == 0) ||
                                    (pfacing == Direction::Right && px == map_width_cells - 1)) {
                                    facing_edge = true;
                                }
                                
                                if (facing_edge) {
                                    warp_to_take = warp_at_pos;
                                }
                            }
                        }
                    }
                    
                    if (warp_to_take) {
                        // Execute the warp through WorldManager
                        transition_ctx.is_warp_arrival = true;
                        auto result = world_manager.execute_warp(*warp_to_take, game_state);
                        
                        if (result.success) {
                            // Transition renderer and game state to new map
                            std::string trans_error;
                            if (!transition_to_map(
                                result.target_map_id,
                                result.target_x,
                                result.target_y,
                                result.target_facing,
                                world_state,
                                transition_ctx,
                                trans_error
                            )) {
                                std::cerr << "Warp transition failed: " << trans_error << "\n";
                            }
                        }
                    }
                    // Check for connection crossing (at map edge facing outward)
                    else if (world_manager.is_at_connection_edge(px, py, pfacing)) {
                        auto result = world_manager.resolve_connection(px, py, pfacing);
                        
                        if (result.success) {
                            // Execute the connection through WorldManager
                            auto exec_result = world_manager.execute_connection(px, py, pfacing, game_state);
                            
                            if (exec_result.success) {
                                // CONNECTION: Do NOT set warpEntryCell
                                // Reference: Gen2Recomped crossConnection never sets warpEntryCell
                                transition_ctx.is_warp_arrival = false;
                                
                                //=============================================================
                                // Gen2Recomped crossConnection seam step (lines ~1740-1760):
                                //   1. setMap(destMap, landing.x, landing.y, facing)
                                //      - Loads map with landing as "position" but...
                                //   2. Place player ONE CELL BEFORE landing:
                                //      p.cellX, p.cellY = x - d[1], y - d[2]
                                //   3. Start NORMAL movement to landing:
                                //      p.targetX, p.targetY = x, y
                                //      p.moving = true
                                //      p.progress = 0
                                //   4. Normal non-scripted movement completion runs
                                //
                                // Key insight: Gen2 modifies p.cellX/Y directly AFTER setMap,
                                // then starts a NORMAL movement. This is non-scripted movement
                                // so onStepComplete runs at the end.
                                //
                                // Enginemon translation:
                                //   1. transition_to_map with seam position (spawn_player sets x/y)
                                //   2. start_player_movement_to for authoritative movement
                                //   3. Presentation derives from game_loop state
                                //=============================================================
                                
                                // Transition to new map, placing player at SEAM position
                                std::string trans_error;
                                if (!transition_to_map(
                                    exec_result.target_map_id,
                                    exec_result.seam_x,   // Seam position (one cell before landing)
                                    exec_result.seam_y,
                                    exec_result.target_facing,
                                    world_state,
                                    transition_ctx,
                                    trans_error
                                )) {
                                    std::cerr << "Connection transition failed: " << trans_error << "\n";
                                } else {
                                    // Start seam step movement through AUTHORITATIVE path
                                    // Reference: Gen2Recomped crossConnection lines 1751-1756
                                    // This enqueues in movement_manager and sets state_=Moving
                                    game_loop.start_player_movement_to(
                                        exec_result.target_x,  // Landing
                                        exec_result.target_y,
                                        exec_result.target_facing
                                    );
                                    
                                    // Set up presentation state to match game_loop state
                                    // Presentation derives from the authoritative movement
                                    player_start_x = exec_result.seam_x * 16.0f;
                                    player_start_y = exec_result.seam_y * 16.0f;
                                    player_target_x = exec_result.target_x * 16.0f;
                                    player_target_y = exec_result.target_y * 16.0f;
                                    player_moving = true;
                                    step_frame = 0;
                                    anim_clock = 0;  // Fresh walk-cycle so seam step shows leg frames
                                }
                            }
                        }
                    }
                } // end if (!is_scripted_movement)
            } // end if (movement_complete)
        } // end if (player_moving)
        
        } // end for (tick_i) - simulation tick loop
        
        //=====================================================================
        // Calculate visual position for rendering
        // This runs once per render frame, after all simulation ticks.
        // Uses interpolation_alpha from scheduler for smooth rendering.
        //=====================================================================
        
        float player_walk_progress = player_moving ? 
            static_cast<float>(step_frame) / STEP_FRAMES : 0.0f;
        
        float visual_player_x = player_start_x;
        float visual_player_y = player_start_y;
        if (player_moving && player_walk_progress > 0) {
            visual_player_x = player_start_x + (player_target_x - player_start_x) * player_walk_progress;
            visual_player_y = player_start_y + (player_target_y - player_start_y) * player_walk_progress;
        }
        
        // Camera follows player (centered)
        camera_x = visual_player_x - (render_config.logical_width / 2.0f) + 8;
        camera_y = visual_player_y - (render_config.logical_height / 2.0f) + 8;
        
        // Clamp camera
        float map_pixel_width = world_state.map.width * 32.0f;
        float map_pixel_height = world_state.map.height * 32.0f;
        float max_x = std::max(0.0f, map_pixel_width - render_config.logical_width);
        float max_y = std::max(0.0f, map_pixel_height - render_config.logical_height);
        camera_x = std::max(0.0f, std::min(camera_x, max_x));
        camera_y = std::max(0.0f, std::min(camera_y, max_y));
        
        renderer.set_view(camera_x, camera_y);
        sprite_renderer.set_view(camera_x, camera_y);

        //=====================================================================
        // Build sprite instances for rendering
        //=====================================================================
        
        std::vector<SpriteInstance> sprites;
        
        // Walk animation phase (frames 4-11 show walk frame)
        bool show_walk_frame = false;
        if (player_moving) {
            int p = anim_clock % 16;
            show_walk_frame = (p >= 4 && p < 12);
        }
        bool current_step_flip = (anim_clock / 16) % 2 == 1;
        
        // Player sprite
        SpriteInstance player_inst;
        player_inst.sprite_id = "chris";
        player_inst.facing = static_cast<SpriteFacing>(game_loop.player().facing);
        player_inst.walking = show_walk_frame;
        player_inst.step_flip = current_step_flip;
        player_inst.is_moving = player_moving;
        player_inst.walk_progress = player_walk_progress;
        player_inst.start_x = player_start_x;
        player_inst.start_y = player_start_y - 4;  // Sprites render 4px above tile
        player_inst.target_x = player_target_x;
        player_inst.target_y = player_target_y - 4;
        sprites.push_back(player_inst);
        
        // NPC sprites - use game_loop NPC state for positions/facing, map objects for sprite_id
        // Reference: Gen2Recomped uses NPC.x, NPC.y with interpolation during movement
        for (size_t i = 0; i < world_state.map.objects.size() && i < game_loop.npcs().size(); ++i) {
            const auto& obj = world_state.map.objects[i];
            const auto& npc = game_loop.npcs()[i];
            
            if (!npc.visible) continue;
            
            SpriteInstance npc_inst;
            npc_inst.sprite_id = obj.sprite_id;
            npc_inst.facing = static_cast<SpriteFacing>(npc.facing);
            
            // Calculate walk animation state for moving NPCs
            bool npc_walk_frame = false;
            bool npc_step_flip = false;
            if (npc.is_moving) {
                // Walk animation: phase 4-11 of 16-frame step shows walk frame
                int phase = npc.move_progress % 16;
                npc_walk_frame = (phase >= 4 && phase < 12);
                // Alternate step flip every 16 frames
                npc_step_flip = ((npc.move_progress / 16) % 2) == 1;
            }
            
            npc_inst.walking = npc_walk_frame;
            npc_inst.step_flip = npc_step_flip;
            npc_inst.is_moving = npc.is_moving;
            
            // Calculate interpolated position during movement
            if (npc.is_moving) {
                float progress = static_cast<float>(npc.move_progress) / GameTiming::FRAMES_PER_STEP;
                
                // During movement: npc.x/y are START position, npc.target_x/y are END position
                float start_x = npc.x * 16.0f;
                float start_y = npc.y * 16.0f;
                float end_x = npc.target_x * 16.0f;
                float end_y = npc.target_y * 16.0f;
                
                npc_inst.start_x = start_x;
                npc_inst.start_y = start_y - 4;  // Sprites render 4px above tile
                npc_inst.target_x = end_x;
                npc_inst.target_y = end_y - 4;
                npc_inst.walk_progress = progress;
            } else {
                npc_inst.start_x = npc.x * 16.0f;
                npc_inst.start_y = npc.y * 16.0f - 4;
                npc_inst.target_x = npc_inst.start_x;
                npc_inst.target_y = npc_inst.start_y;
                npc_inst.walk_progress = 0.0f;
            }
            
            sprites.push_back(npc_inst);
        }
        
        sprite_renderer.set_sprites(sprites);

        //=====================================================================
        // Render frame
        //=====================================================================
        
        if (!vulkan.begin_frame()) {
            vulkan.recreate_swapchain(platform.width(), platform.height());
            continue;
        }
        
        VkCommandBuffer cmd = vulkan.get_command_buffer();
        uint32_t img_idx = vulkan.current_image_index();
        
        // Transition to color attachment
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = vulkan.swapchain_images()[img_idx];
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.baseMipLevel = 0;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount = 1;
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            0, 0, nullptr, 0, nullptr, 1, &barrier);

        // Begin dynamic rendering
        VkRenderingAttachmentInfo color_attachment{};
        color_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        color_attachment.imageView = vulkan.swapchain_image_views()[img_idx];
        color_attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color_attachment.clearValue.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
        
        VkRenderingInfo render_info{};
        render_info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        render_info.renderArea.offset = {0, 0};
        render_info.renderArea.extent = vulkan.swapchain_extent();
        render_info.layerCount = 1;
        render_info.colorAttachmentCount = 1;
        render_info.pColorAttachments = &color_attachment;
        
        vkCmdBeginRendering(cmd, &render_info);

        // Render tiles first
        renderer.render(cmd);
        
        // Render sprites on top
        sprite_renderer.render(cmd);
        
        // Render textbox overlay (if dialog is open)
        textbox_renderer.set_state(dialog_state);
        textbox_renderer.render(cmd);
        
        vkCmdEndRendering(cmd);
        
        // Transition to present
        barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        barrier.dstAccessMask = 0;
        
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
            0, 0, nullptr, 0, nullptr, 1, &barrier);
        
        // End frame
        if (!vulkan.end_frame(cmd)) {
            vulkan.recreate_swapchain(platform.width(), platform.height());
        }
    }
    
    textbox_renderer.destroy();
    sprite_renderer.destroy();
    renderer.destroy();
    vulkan.shutdown();
    platform.shutdown();
    
    return 0;
}
