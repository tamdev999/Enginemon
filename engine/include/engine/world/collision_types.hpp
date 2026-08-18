#pragma once
// engine/world/collision_types.hpp
// Semantic collision types - runtime knows these, NOT Crystal byte values
//
// The frontend (Crystal compiler) translates raw collision bytes into these
// semantic types before packaging. The runtime never sees raw Crystal IDs.

#include <cstdint>
#include "engine/core/types.hpp"  // For Direction enum

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
    
    // Directional ledges (one-way hop)
    // Player can hop DOWN (in the ledge direction) but not back up
    // Source: pokecrystal COLL_HOP_RIGHT/LEFT/UP/DOWN = 0xA0-0xA3
    LedgeRight = 3,     // Hop right (COLL_HOP_RIGHT = 0xA0)
    LedgeLeft = 4,      // Hop left (COLL_HOP_LEFT = 0xA1)
    LedgeUp = 5,        // Hop up (COLL_HOP_UP = 0xA2, unused in vanilla)
    LedgeDown = 6,      // Hop down (COLL_HOP_DOWN = 0xA3)
    
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

// Is this a side wall blocking from a specific direction?
inline bool collision_is_side_wall(CollisionClass c) {
    return c >= CollisionClass::SideWallN && c <= CollisionClass::SideWallW;
}

// Is this a directional ledge?
inline bool collision_is_ledge(CollisionClass c) {
    return c >= CollisionClass::LedgeRight && c <= CollisionClass::LedgeDown;
}

// Get the direction a ledge allows hopping (e.g., LedgeDown allows hop when facing Down)
// Returns the direction the player must be facing to hop over this ledge
// For invalid/non-ledge input, returns Down as a safe default
inline Direction collision_ledge_direction(CollisionClass c) {
    switch (c) {
        case CollisionClass::LedgeRight: return Direction::Right;
        case CollisionClass::LedgeLeft:  return Direction::Left;
        case CollisionClass::LedgeUp:    return Direction::Up;
        case CollisionClass::LedgeDown:  return Direction::Down;
        default: return Direction::Down;
    }
}

// Can the player hop over this ledge when facing the given direction?
// Ledges are passable ONLY when the player faces the ledge direction
// Note: Full hop execution is NOT implemented - this only checks if hop is allowed
inline bool collision_can_hop_ledge(CollisionClass c, Direction facing) {
    if (!collision_is_ledge(c)) return false;
    return collision_ledge_direction(c) == facing;
}

// Is this passable on foot (no special moves)?
inline bool collision_is_walkable(CollisionClass c) {
    return c == CollisionClass::Floor || 
           c == CollisionClass::Grass ||
           c == CollisionClass::Ice ||
           collision_is_warp(c) ||  // Warps are walkable (they trigger after)
           collision_is_side_wall(c);  // Side walls are walkable (they have directional restrictions checked separately)
}

// Is this passable while surfing?
inline bool collision_is_swimmable(CollisionClass c) {
    return c == CollisionClass::Water ||
           c == CollisionClass::Whirlpool;
}

// Is this passable with current mode (foot or surfing)?
// Note: On foot, only collision_is_walkable() tiles are passable.
//       While surfing, water tiles are also passable.
inline bool collision_is_passable(CollisionClass c, bool surfing) {
    if (collision_is_walkable(c)) return true;
    if (surfing && collision_is_swimmable(c)) return true;
    return false;
}

// Does this tile extend interaction reach?
inline bool collision_is_counter(CollisionClass c) {
    return c == CollisionClass::Counter;
}

} // namespace enginemon
