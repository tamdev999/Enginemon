#pragma once
// engine/world/collision.hpp
// Native collision system for Pokémon-style tile movement
//
// Reference: pokecrystal/constants/collision_constants.asm
// Reference: pokecrystal/data/collision/collision_permissions.asm
// Reference: pokecrystal/home/map.asm GetMovementPermissions
// Reference: Gen2Recomped/src/world/Collision.lua
//
// The collision system determines whether an entity can move from one tile
// to another based on:
// 1. Tile walkability (from collision permission table)
// 2. Side walls (directional blocking)
// 3. Entity occupancy (both current and target cells during movement)
// 4. Map bounds

#include "engine/core/types.hpp"
#include <cstdint>
#include <array>
#include <optional>
#include <vector>
#include <functional>

namespace enginemon {

//=============================================================================
// COLLISION PERMISSIONS
// From pokecrystal/constants/collision_constants.asm
//=============================================================================

// Base permission types (what GetTilePermission returns)
enum class TilePermission : uint8_t {
    Land = 0x00,    // LAND_TILE - walkable on foot
    Water = 0x01,   // WATER_TILE - requires surf
    Wall = 0x0F,    // WALL_TILE - impassable
    Talk = 0x10,    // TALK flag - can interact (counters, trees)
};

// Common collision byte values from pokecrystal
// These map to TilePermission via CollisionPermissionTable
namespace CollisionByte {
    constexpr uint8_t FLOOR = 0x00;
    constexpr uint8_t WALL = 0x07;
    constexpr uint8_t CUT_TREE = 0x12;
    constexpr uint8_t LONG_GRASS = 0x14;
    constexpr uint8_t HEADBUTT_TREE = 0x15;
    constexpr uint8_t TALL_GRASS = 0x18;
    constexpr uint8_t ICE = 0x23;
    constexpr uint8_t WHIRLPOOL = 0x24;
    constexpr uint8_t BUOY = 0x27;
    constexpr uint8_t WATER = 0x29;
    constexpr uint8_t WATERFALL = 0x33;
    constexpr uint8_t PIT = 0x60;
    constexpr uint8_t WARP_CARPET_DOWN = 0x70;
    constexpr uint8_t DOOR = 0x71;
    constexpr uint8_t LADDER = 0x72;
    constexpr uint8_t STAIRCASE = 0x7A;
    constexpr uint8_t CAVE = 0x7B;
    constexpr uint8_t WARP_PANEL = 0x7C;
    constexpr uint8_t COUNTER = 0x90;
    constexpr uint8_t BOOKSHELF = 0x91;
    constexpr uint8_t PC = 0x93;
    constexpr uint8_t TV = 0x97;
    constexpr uint8_t HOP_RIGHT = 0xA0;
    constexpr uint8_t HOP_LEFT = 0xA1;
    constexpr uint8_t HOP_DOWN = 0xA3;
    constexpr uint8_t RIGHT_WALL = 0xB0;
    constexpr uint8_t LEFT_WALL = 0xB1;
    constexpr uint8_t UP_WALL = 0xB2;
    constexpr uint8_t DOWN_WALL = 0xB3;
}

// Nybble patterns for collision type detection
namespace CollisionNybble {
    constexpr uint8_t HI_TALL_GRASS = 0x10;
    constexpr uint8_t HI_WATER = 0x20;
    constexpr uint8_t HI_CURRENT = 0x30;
    constexpr uint8_t HI_WALK = 0x40;
    constexpr uint8_t HI_WARPS = 0x70;
    constexpr uint8_t HI_LEDGES = 0xA0;
    constexpr uint8_t HI_SIDE_WALLS = 0xB0;
    constexpr uint8_t HI_SIDE_BUOYS = 0xC0;
}

//=============================================================================
// DIRECTION MASKS
// From pokecrystal for movement permission checks
//=============================================================================

namespace DirectionMask {
    constexpr uint8_t DOWN = 0x01;
    constexpr uint8_t UP = 0x02;
    constexpr uint8_t LEFT = 0x04;
    constexpr uint8_t RIGHT = 0x08;
}

//=============================================================================
// COLLISION RESULT
//=============================================================================

enum class MoveBlockReason {
    None,           // Movement allowed
    Bounds,         // Out of map bounds
    Tile,           // Tile not walkable (wall, water without surf)
    SideWall,       // Directional wall blocking this direction
    Entity,         // Another entity occupies the target
    Ledge,          // Ledge can only be jumped from specific direction
};

struct CollisionResult {
    bool allowed = false;
    MoveBlockReason reason = MoveBlockReason::None;
    
    // Additional context
    uint8_t collision_byte = 0;     // The collision byte at target tile
    uint16_t blocking_entity = 0;   // ID of entity blocking (if Entity reason)
    
    static CollisionResult success() { return {true, MoveBlockReason::None}; }
    static CollisionResult blocked(MoveBlockReason r) { return {false, r}; }
};

//=============================================================================
// ENTITY INTERFACE
// Minimal interface for collision checking - doesn't depend on Actor class
//=============================================================================

struct CollisionEntity {
    uint16_t id;
    int32_t x;              // Current cell X
    int32_t y;              // Current cell Y
    int32_t target_x;       // Target cell X (if moving)
    int32_t target_y;       // Target cell Y (if moving)
    bool is_moving;         // True if mid-step (target cell should also block)
    bool is_passable;       // True if other entities can walk through (e.g., Pikachu follower)
};

//=============================================================================
// MAP INTERFACE
// Minimal interface for collision checking - doesn't depend on MapData class
//=============================================================================

struct CollisionMap {
    int32_t width;          // Map width in tiles
    int32_t height;         // Map height in tiles
    
    // Get collision byte at tile (x, y)
    // Returns 0xFF for out-of-bounds
    std::function<uint8_t(int32_t x, int32_t y)> get_collision;
    
    // Get side wall directions blocked at tile (x, y)
    // Returns bitmask of DirectionMask values
    std::function<uint8_t(int32_t x, int32_t y)> get_side_walls;
};

//=============================================================================
// COLLISION PERMISSION TABLE
// Maps 256 collision bytes to permissions (from pokecrystal)
//=============================================================================

class CollisionPermissionTable {
public:
    CollisionPermissionTable();
    
    // Get base permission for a collision byte
    TilePermission get_permission(uint8_t collision_byte) const;
    
    // Check if tile has TALK flag (counters, cuttable trees, etc.)
    bool has_talk_flag(uint8_t collision_byte) const;
    
    // Check if tile is walkable on foot (LAND_TILE)
    bool is_land(uint8_t collision_byte) const;
    
    // Check if tile is water (requires surf)
    bool is_water(uint8_t collision_byte) const;
    
    // Check if tile is wall (impassable)
    bool is_wall(uint8_t collision_byte) const;

private:
    // 256-entry permission table (from collision_permissions.asm)
    std::array<uint8_t, 256> table_;
};

//=============================================================================
// COLLISION CHECKER
// Main collision checking logic
//=============================================================================

class Collision {
public:
    Collision();
    
    // Check if mover can step from current position toward direction
    // This is the main entry point matching Gen2Recomped Collision.canMove()
    CollisionResult can_move(
        const CollisionMap& map,
        const std::vector<CollisionEntity>& entities,
        const CollisionEntity& mover,
        Direction dir
    ) const;
    
    // Get target coordinates for a direction
    static void get_target(int32_t x, int32_t y, Direction dir, 
                           int32_t& out_x, int32_t& out_y);
    
    // Check if an entity occupies a cell (current or target during movement)
    // Returns the blocking entity's ID, or 0 if none
    static uint16_t get_occupant(
        const std::vector<CollisionEntity>& entities,
        int32_t x, int32_t y,
        uint16_t ignore_id = 0
    );
    
    // Direct tile checks
    bool is_tile_walkable(uint8_t collision_byte, bool surfing = false) const;
    bool is_side_wall_blocking(uint8_t collision_byte, Direction dir) const;
    bool is_ledge(uint8_t collision_byte) const;
    Direction get_ledge_direction(uint8_t collision_byte) const;

private:
    CollisionPermissionTable permissions_;
    
    // Check tile passability
    CollisionResult check_tile(const CollisionMap& map, int32_t x, int32_t y, 
                               bool surfing) const;
    
    // Check side wall in both source and target cells
    CollisionResult check_side_walls(const CollisionMap& map,
                                     int32_t from_x, int32_t from_y,
                                     int32_t to_x, int32_t to_y,
                                     Direction dir) const;
    
    // Check entity occupancy
    CollisionResult check_occupancy(const std::vector<CollisionEntity>& entities,
                                    int32_t x, int32_t y,
                                    uint16_t mover_id) const;
};

//=============================================================================
// UTILITY
//=============================================================================

// Convert Direction enum to delta
inline void direction_to_delta(Direction dir, int& dx, int& dy) {
    switch (dir) {
        case Direction::Down:  dx = 0;  dy = 1;  break;
        case Direction::Up:    dx = 0;  dy = -1; break;
        case Direction::Left:  dx = -1; dy = 0;  break;
        case Direction::Right: dx = 1;  dy = 0;  break;
    }
}

// Get opposite direction
inline Direction opposite_direction(Direction dir) {
    switch (dir) {
        case Direction::Down:  return Direction::Up;
        case Direction::Up:    return Direction::Down;
        case Direction::Left:  return Direction::Right;
        case Direction::Right: return Direction::Left;
    }
    return dir;
}

// Convert direction to mask
inline uint8_t direction_to_mask(Direction dir) {
    switch (dir) {
        case Direction::Down:  return DirectionMask::DOWN;
        case Direction::Up:    return DirectionMask::UP;
        case Direction::Left:  return DirectionMask::LEFT;
        case Direction::Right: return DirectionMask::RIGHT;
    }
    return 0;
}

} // namespace enginemon
