#pragma once
// engine/world/collision.hpp
// Native collision system for Pokémon-style tile movement
//
// ARCHITECTURE (Semantic Collision Boundary):
//   Crystal ROM → frontend classifier → semantic CollisionClass → package → runtime
//
// The collision system operates ONLY on semantic CollisionClass values.
// All raw Crystal byte interpretation happens in the frontend:
//   frontends/crystal/include/crystal/world/collision_classifier.hpp
//
// The collision system determines whether an entity can move from one tile
// to another based on:
// 1. Tile walkability (from semantic CollisionClass)
// 2. Side walls (directional blocking, also semantic)
// 3. Entity occupancy (both current and target cells during movement)
// 4. Map bounds

#include "engine/core/types.hpp"
#include "engine/world/collision_types.hpp"
#include <cstdint>
#include <optional>
#include <vector>
#include <functional>

namespace enginemon {

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
    CollisionClass collision_class = CollisionClass::Floor;  // The collision at target tile
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
    
    // Get semantic collision class at tile (x, y)
    // Returns CollisionClass::Wall for out-of-bounds
    std::function<CollisionClass(int32_t x, int32_t y)> get_collision;
};

//=============================================================================
// COLLISION CHECKER
// Main collision checking logic using semantic CollisionClass
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
    // Returns the blocking entity's ID, or std::nullopt if no occupant
    // NOTE: ID 0 is a valid entity (player), so we use optional instead of sentinel
    static std::optional<uint16_t> get_occupant(
        const std::vector<CollisionEntity>& entities,
        int32_t x, int32_t y,
        uint16_t ignore_id
    );

private:
    // Check tile passability using semantic CollisionClass
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

} // namespace enginemon
