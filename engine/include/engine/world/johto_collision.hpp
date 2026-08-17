#pragma once
// engine/world/johto_collision.hpp
// Collision lookup using semantic CollisionClass
//
// The runtime receives semantic CollisionClass values from packages, NOT raw Crystal bytes.
// This file provides collision lookup and warp-related helpers using semantic types.
//
// ARCHITECTURE (Audit Item B - COMPLETE):
//   Crystal ROM → frontend classifier → semantic CollisionClass → package → runtime
//
// The semantic types are defined in collision_types.hpp. All runtime code uses:
//   - CollisionClass enum for semantic collision types
//   - collision_is_warp(), collision_is_walkable(), etc. for queries
//
// Raw Crystal collision byte interpretation exists ONLY in the frontend classifier:
//   frontends/crystal/include/crystal/world/collision_classifier.hpp
//
// Reference: pokecrystal/data/tilesets/*_collision.asm
// Each metatile (32×32 block) has 4 collision values (2×2 at 16×16 each)
// Collision values: TL, TR, BL, BR (top-left, top-right, bottom-left, bottom-right)
// Index formula: collision[metatile_index * 4 + (cell_x % 2) + (cell_y % 2) * 2]
//
// Per-tileset collision is extracted by the Crystal frontend, classified to
// semantic CollisionClass, and stored in the native package.

#include "engine/world/collision_types.hpp"
#include <cstdint>
#include <vector>

namespace enginemon {

//=============================================================================
// COLLISION LOOKUP
// Returns semantic CollisionClass for tile position
//=============================================================================

// Get collision class at tile position from metatile block array
// Uses per-tileset collision data (4 CollisionClass values per metatile: TL, TR, BL, BR)
// 
// Parameters:
// - blocks: metatile indices for the map (width*height bytes)
// - collision: per-tileset semantic collision data (metatile_count * 4 CollisionClass values)
// - map_width_blocks: map width in blocks (metatiles)
// - tile_x, tile_y: tile position (each block is 2×2 tiles)
//
// Reference: Gen2Recomped Map.lua cellTile(), pokecrystal data/tilesets/*_collision.asm
// Index formula: collision[blockId * 4 + (cx % 2) + (cy % 2) * 2]
//
// Returns: semantic CollisionClass (Floor, Wall, WarpDoor, etc.)
inline CollisionClass get_collision_from_blocks(
    const std::vector<uint8_t>& blocks,
    const std::vector<CollisionClass>& collision,
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
        return CollisionClass::Wall;  // Out of bounds = wall
    }
    
    int block_idx = block_y * map_width_blocks + block_x;
    uint8_t metatile = blocks[block_idx];
    
    // Block 0 always reads as wall (Gen2Recomped: "if blockId == 0 then return 0xFF")
    if (metatile == 0) {
        return CollisionClass::Wall;
    }
    
    // Collision index: metatile * 4 + quadrant
    size_t coll_idx = static_cast<size_t>(metatile) * 4 + quad_idx;
    
    // Bounds check for collision data
    if (coll_idx >= collision.size()) {
        return CollisionClass::Wall;  // Invalid = wall
    }
    
    return collision[coll_idx];
}

} // namespace enginemon
