#pragma once
// engine/world/collision_types.hpp
// Semantic collision types - runtime knows these, NOT Crystal byte values
//
// The frontend (Crystal compiler) translates raw collision bytes into these
// semantic types before packaging. The runtime never sees raw Crystal IDs.

#include <cstdint>

namespace enginemon {

//=============================================================================
// SEMANTIC COLLISION TYPES
// These represent navigation/collision semantics, not source-game byte values.
//=============================================================================

enum class CollisionClass : uint8_t {
    // Basic navigation
    Floor = 0,          // Walkable floor tile
    Wall = 1,           // Solid wall, cannot pass
    Water = 2,          // Requires Surf to cross
    Ledge = 3,          // One-way hop (directional)
    
    // Warp triggers
    WarpFloor = 10,     // Triggers warp when stepped on
    WarpDoor = 11,      // Outdoor door, triggers warp + auto-step animation
    WarpCave = 12,      // Cave entrance, triggers warp + auto-step animation
    WarpStair = 13,     // Stair/ladder, triggers warp + clears standing-on-warp
    WarpCarpet = 14,    // Exit carpet, requires directional input + warp
    WarpPit = 15,       // Fall-through hole, triggers fall + warp
    
    // Interactive tiles
    Counter = 20,       // Counter tile, extends interaction reach
    CuttableTree = 21,  // Can be cut with Cut
    SmashableRock = 22, // Can be smashed with Rock Smash
    StrengthBoulder = 23, // Can be pushed with Strength
    Whirlpool = 24,     // Water obstacle, requires Whirlpool
    Waterfall = 25,     // Climbable with Waterfall
    
    // Side walls (directional blocking)
    SideWallN = 30,     // Blocks movement from south
    SideWallS = 31,     // Blocks movement from north
    SideWallE = 32,     // Blocks movement from west
    SideWallW = 33,     // Blocks movement from east
    
    // Special
    Grass = 40,         // Tall grass, triggers encounters
    Ice = 41,           // Sliding ice
    Spinner = 42,       // Spin tile (directional)
    
    Unknown = 255       // Unclassified, treat as wall
};

//=============================================================================
// SEMANTIC QUERIES
// Runtime uses these instead of raw byte comparisons
//=============================================================================

// Is this collision class a valid warp entrance?
inline bool collision_is_warp(CollisionClass c) {
    return c >= CollisionClass::WarpFloor && c <= CollisionClass::WarpPit;
}

// Does this warp type trigger auto-step animation?
inline bool collision_is_door_warp(CollisionClass c) {
    return c == CollisionClass::WarpDoor || c == CollisionClass::WarpCave;
}

// Does this warp type require directional input?
inline bool collision_is_carpet_warp(CollisionClass c) {
    return c == CollisionClass::WarpCarpet;
}

// Is this a pit/fall-through?
inline bool collision_is_pit(CollisionClass c) {
    return c == CollisionClass::WarpPit;
}

// Does this clear standing-on-warp flag?
inline bool collision_clears_warp_flag(CollisionClass c) {
    return c == CollisionClass::WarpStair;
}

// Is this passable on foot (no special moves)?
inline bool collision_is_walkable(CollisionClass c) {
    return c == CollisionClass::Floor || 
           c == CollisionClass::Grass ||
           collision_is_warp(c);  // Warps are walkable (they trigger after)
}

// Is this passable while surfing?
inline bool collision_is_swimmable(CollisionClass c) {
    return c == CollisionClass::Water ||
           c == CollisionClass::Whirlpool;
}

// Does this tile extend interaction reach?
inline bool collision_is_counter(CollisionClass c) {
    return c == CollisionClass::Counter;
}

// Is this a side wall blocking from a specific direction?
inline bool collision_is_side_wall(CollisionClass c) {
    return c >= CollisionClass::SideWallN && c <= CollisionClass::SideWallW;
}

} // namespace enginemon
