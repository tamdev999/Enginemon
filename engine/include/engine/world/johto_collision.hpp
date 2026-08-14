#pragma once
// engine/world/johto_collision.hpp
// Collision lookup and warp entrance validation for Crystal-derived tilesets
//
// Reference: pokecrystal/data/tilesets/*_collision.asm
// Each metatile (32×32 block) has 4 collision bytes (2×2 at 16×16 each)
// Collision values: TL, TR, BL, BR (top-left, top-right, bottom-left, bottom-right)
// Index formula: collision[metatile_index * 4 + (cell_x % 2) + (cell_y % 2) * 2]
//
// Per-tileset collision is extracted by the Crystal frontend and stored
// in the native package. Each tileset has its own collision table.

#include <cstdint>
#include <vector>

namespace enginemon {

//=============================================================================
// WARP/ENTRANCE COLLISION CLASS CHECKS
// Reference: Gen2Recomped Map.lua gen2IsEntrance, gen2IsDoorway, gen2IsPit
//
// CheckWarpCollision (05:$4A18): $60, $68 and the whole $70-$7F carpet/door
// range are the collision classes that let a Gen2 warp_event fire.
//=============================================================================

// Is this collision class a valid warp entrance?
// Warps only trigger when standing on a tile with one of these collision classes.
inline bool is_warp_entrance(uint8_t coll) {
    // 0x60 = COLL_PIT, 0x68 = COLL_PIT_68, 0x70-0x7F = carpet/door/stair range
    return coll == 0x60 || coll == 0x68 || (coll >= 0x70 && coll <= 0x7F);
}

// Is this collision class a doorway (outdoor door or cave mouth)?
// These are the tiles that leave the player standing in the opening.
// 0x71 = COLL_DOOR (outdoor building door), 0x7B = COLL_CAVE (cave mouth)
inline bool is_doorway_entrance(uint8_t coll) {
    return coll == 0x71 || coll == 0x7B;
}

// Is this collision class a pit/hole?
// 0x60 = COLL_PIT, 0x68 = COLL_PIT_68
// Used for Strength boulder drops and fall-through holes.
inline bool is_pit_collision(uint8_t coll) {
    return coll == 0x60 || coll == 0x68;
}

// Is this collision class an exit carpet?
// Exit carpets (mats at interior exits) require facing outward + d-pad held.
// 0x70 = WARP_CARPET_DOWN, 0x76 = WARP_CARPET_LEFT
// 0x78 = WARP_CARPET_RIGHT, 0x7E = WARP_CARPET_UP
inline bool is_exit_carpet(uint8_t coll) {
    return coll == 0x70 || coll == 0x76 || coll == 0x78 || coll == 0x7E;
}

// Is this collision class a doorway (outdoor door or cave mouth)?
// Reference: Gen2Recomped Map.lua gen2IsDoorway
// ONLY 0x71 = COLL_DOOR (outdoor building door), 0x7B = COLL_CAVE (cave mouth)
// These are the tiles that trigger auto-step south on arrival (PlayerStepOutFromDoor).
// Note: This is the SAME as is_doorway_entrance() - both check gen2IsDoorway semantics.
inline bool is_door_tile(uint8_t coll) {
    return coll == 0x71 || coll == 0x7B;
}

// Is this collision class a staircase/ladder (clears standingOnWarp flag)?
// Reference: Gen2Recomped OverworldController.lua refreshStandingOnWarp
// Staircase tiles: 0x72 = COLL_STAIRCASE, 0x73 = COLL_STAIRCASE_73
// These clear BIT_STANDING_ON_WARP so you can't bounce between floors.
inline bool is_staircase_tile(uint8_t coll) {
    return coll == 0x72 || coll == 0x73;
}

//=============================================================================
// COLLISION LOOKUP
//=============================================================================

// Get collision byte at tile position from metatile block array
// Uses per-tileset collision data (4 bytes per metatile: TL, TR, BL, BR)
// 
// Parameters:
// - blocks: metatile indices for the map (width*height bytes)
// - collision: per-tileset collision data (metatile_count * 4 bytes)
// - map_width_blocks: map width in blocks (metatiles)
// - tile_x, tile_y: tile position (each block is 2×2 tiles)
//
// Reference: Gen2Recomped Map.lua cellTile(), pokecrystal data/tilesets/*_collision.asm
// Index formula: collision[blockId * 4 + (cx % 2) + (cy % 2) * 2]
//
// Returns: collision byte (0x00 = walkable, 0x07 = wall, etc.)
inline uint8_t get_collision_from_blocks(
    const std::vector<uint8_t>& blocks,
    const std::vector<uint8_t>& collision,
    int map_width_blocks,
    int tile_x, int tile_y
) {
    // Convert tile coords to block coords and quadrant
    int block_x = tile_x / 2;
    int block_y = tile_y / 2;
    int quad_x = tile_x % 2;
    int quad_y = tile_y % 2;
    
    // Quadrant index: (cx % 2) + (cy % 2) * 2
    // Gives: TL=0, TR=1, BL=2, BR=3
    int quad_idx = quad_x + quad_y * 2;
    
    // Bounds check for blocks
    int map_height_blocks = static_cast<int>(blocks.size()) / map_width_blocks;
    if (block_x < 0 || block_y < 0 || 
        block_x >= map_width_blocks || 
        block_y >= map_height_blocks) {
        return 0xFF;  // Out of bounds = wall
    }
    
    int block_idx = block_y * map_width_blocks + block_x;
    uint8_t metatile = blocks[block_idx];
    
    // Block 0 always reads as wall (Gen2Recomped: "if blockId == 0 then return 0xFF")
    if (metatile == 0) {
        return 0xFF;
    }
    
    // Collision index: metatile * 4 + quadrant
    size_t coll_idx = static_cast<size_t>(metatile) * 4 + quad_idx;
    
    // Bounds check for collision data
    if (coll_idx >= collision.size()) {
        return 0xFF;  // Invalid = wall
    }
    
    return collision[coll_idx];
}

} // namespace enginemon
