// engine/world/collision.cpp
// Native collision system implementation using semantic CollisionClass
//
// ARCHITECTURE (Semantic Collision Boundary):
//   All collision checks operate on semantic CollisionClass values.
//   Raw Crystal byte interpretation exists ONLY in the frontend classifier:
//   frontends/crystal/include/crystal/world/collision_classifier.hpp

#include "engine/world/collision.hpp"

namespace enginemon {

//=============================================================================
// COLLISION CHECKER
//=============================================================================

Collision::Collision() = default;

void Collision::get_target(int32_t x, int32_t y, Direction dir, 
                           int32_t& out_x, int32_t& out_y) {
    int dx = 0, dy = 0;
    direction_to_delta(dir, dx, dy);
    out_x = x + dx;
    out_y = y + dy;
}

std::optional<uint16_t> Collision::get_occupant(
    const std::vector<CollisionEntity>& entities,
    int32_t x, int32_t y,
    uint16_t ignore_id
) {
    // Reference: Gen2Recomped Collision.occupied()
    // Check both current cell AND target cell (if entity is mid-step)
    for (const auto& e : entities) {
        if (e.id == ignore_id) continue;
        if (e.is_passable) continue;
        
        // Check current cell
        if (e.x == x && e.y == y) {
            return e.id;
        }
        
        // Check target cell if moving (destination is reserved)
        if (e.is_moving && e.target_x == x && e.target_y == y) {
            return e.id;
        }
    }
    
    return std::nullopt;  // No occupant
}

CollisionResult Collision::check_tile(const CollisionMap& map, int32_t x, int32_t y,
                                       bool surfing) const {
    CollisionClass coll = map.get_collision(x, y);
    
    if (!collision_is_passable(coll, surfing)) {
        CollisionResult result;
        result.allowed = false;
        result.reason = MoveBlockReason::Tile;
        result.collision_class = coll;
        return result;
    }
    
    CollisionResult result = CollisionResult::success();
    result.collision_class = coll;
    return result;
}

CollisionResult Collision::check_side_walls(const CollisionMap& map,
                                            int32_t from_x, int32_t from_y,
                                            int32_t to_x, int32_t to_y,
                                            Direction dir) const {
    // Check source tile - does it block stepping OUT in direction dir?
    CollisionClass from_coll = map.get_collision(from_x, from_y);
    
    if (collision_is_side_wall(from_coll)) {
        bool blocks_out = false;
        switch (from_coll) {
            case CollisionClass::SideWallN: blocks_out = (dir == Direction::Up); break;
            case CollisionClass::SideWallS: blocks_out = (dir == Direction::Down); break;
            case CollisionClass::SideWallE: blocks_out = (dir == Direction::Right); break;
            case CollisionClass::SideWallW: blocks_out = (dir == Direction::Left); break;
            default: break;
        }
        
        if (blocks_out) {
            CollisionResult result;
            result.allowed = false;
            result.reason = MoveBlockReason::SideWall;
            result.collision_class = from_coll;
            return result;
        }
    }
    
    // Check destination tile - does it block stepping IN from direction dir?
    CollisionClass to_coll = map.get_collision(to_x, to_y);
    
    if (collision_is_side_wall(to_coll)) {
        // The direction we're ENTERING from is opposite of movement direction
        Direction enter_dir = opposite_direction(dir);
        bool blocks_in = false;
        switch (to_coll) {
            case CollisionClass::SideWallN: blocks_in = (enter_dir == Direction::Down); break;
            case CollisionClass::SideWallS: blocks_in = (enter_dir == Direction::Up); break;
            case CollisionClass::SideWallE: blocks_in = (enter_dir == Direction::Left); break;
            case CollisionClass::SideWallW: blocks_in = (enter_dir == Direction::Right); break;
            default: break;
        }
        
        if (blocks_in) {
            CollisionResult result;
            result.allowed = false;
            result.reason = MoveBlockReason::SideWall;
            result.collision_class = to_coll;
            return result;
        }
    }
    
    return CollisionResult::success();
}

CollisionResult Collision::check_occupancy(const std::vector<CollisionEntity>& entities,
                                           int32_t x, int32_t y,
                                           uint16_t mover_id) const {
    auto occupant = get_occupant(entities, x, y, mover_id);
    
    if (occupant.has_value()) {
        CollisionResult result;
        result.allowed = false;
        result.reason = MoveBlockReason::Entity;
        result.blocking_entity = occupant.value();
        return result;
    }
    
    return CollisionResult::success();
}

CollisionResult Collision::can_move(
    const CollisionMap& map,
    const std::vector<CollisionEntity>& entities,
    const CollisionEntity& mover,
    Direction dir
) const {
    // Reference: Gen2Recomped Collision.canMove()
    // Order of checks: bounds -> tile walkability -> side walls -> entity occupancy
    
    // 1. Calculate target coordinates
    int32_t tx, ty;
    get_target(mover.x, mover.y, dir, tx, ty);
    
    // 2. Bounds check
    if (tx < 0 || tx >= map.width || ty < 0 || ty >= map.height) {
        return CollisionResult::blocked(MoveBlockReason::Bounds);
    }
    
    // 3. Tile walkability using semantic CollisionClass
    // TODO: Add surfing state to mover
    bool surfing = false;
    CollisionResult tile_result = check_tile(map, tx, ty, surfing);
    if (!tile_result.allowed) {
        return tile_result;
    }
    
    // 4. Side walls (check both source and destination)
    CollisionResult wall_result = check_side_walls(map, mover.x, mover.y, tx, ty, dir);
    if (!wall_result.allowed) {
        return wall_result;
    }
    
    // 5. Entity occupancy
    CollisionResult occupancy_result = check_occupancy(entities, tx, ty, mover.id);
    if (!occupancy_result.allowed) {
        return occupancy_result;
    }
    
    // All checks passed
    CollisionResult result = CollisionResult::success();
    result.collision_class = tile_result.collision_class;
    return result;
}

} // namespace enginemon
