// engine/world/collision.cpp
// Native collision system implementation
//
// Reference: pokecrystal/data/collision/collision_permissions.asm
// Reference: pokecrystal/home/map.asm GetMovementPermissions
// Reference: Gen2Recomped/src/world/Collision.lua

#include "engine/world/collision.hpp"

namespace enginemon {

//=============================================================================
// COLLISION PERMISSION TABLE
// Exact reproduction of pokecrystal CollisionPermissionTable (256 entries)
//=============================================================================

CollisionPermissionTable::CollisionPermissionTable() {
    // Initialize from pokecrystal/data/collision/collision_permissions.asm
    // LAND_TILE = 0x00, WATER_TILE = 0x01, WALL_TILE = 0x0F, TALK = 0x10
    
    constexpr uint8_t L = 0x00;  // LAND_TILE
    constexpr uint8_t W = 0x01;  // WATER_TILE
    constexpr uint8_t X = 0x0F;  // WALL_TILE
    constexpr uint8_t T = 0x10;  // TALK flag (OR'd with base)
    
    // Full 256-entry table from collision_permissions.asm
    table_ = {{
        // 0x00-0x0F
        L,      // 00: COLL_FLOOR
        L,      // 01: COLL_01
        L,      // 02
        L,      // 03: COLL_03
        L,      // 04: COLL_04
        L,      // 05
        L,      // 06
        X,      // 07: COLL_WALL
        L,      // 08: COLL_CUT_08
        L,      // 09
        L,      // 0A
        L,      // 0B
        L,      // 0C
        L,      // 0D
        L,      // 0E
        X,      // 0F
        
        // 0x10-0x1F
        L,      // 10: COLL_TALL_GRASS_10
        L,      // 11
        X|T,    // 12: COLL_CUT_TREE (wall + talkable)
        L,      // 13
        L,      // 14: COLL_LONG_GRASS
        X|T,    // 15: COLL_HEADBUTT_TREE
        L,      // 16
        L,      // 17
        L,      // 18: COLL_TALL_GRASS
        L,      // 19
        X|T,    // 1A: COLL_CUT_TREE_1A
        L,      // 1B
        L,      // 1C: COLL_LONG_GRASS_1C
        X|T,    // 1D: COLL_HEADBUTT_TREE_1D
        L,      // 1E
        L,      // 1F
        
        // 0x20-0x2F (Water tiles)
        W,      // 20
        W,      // 21: COLL_WATER_21
        W|T,    // 22
        L,      // 23: COLL_ICE (special - land, but slippery)
        W|T,    // 24: COLL_WHIRLPOOL
        W,      // 25
        W,      // 26
        X,      // 27: COLL_BUOY (wall in water)
        W,      // 28: COLL_CUT_28
        W,      // 29: COLL_WATER
        W|T,    // 2A
        L,      // 2B: COLL_ICE_2B
        W|T,    // 2C: COLL_WHIRLPOOL_2C
        W,      // 2D
        W,      // 2E
        X,      // 2F
        
        // 0x30-0x3F (Waterfall/current tiles)
        W,      // 30: COLL_WATERFALL_RIGHT
        W,      // 31: COLL_WATERFALL_LEFT
        W,      // 32: COLL_WATERFALL_UP
        W,      // 33: COLL_WATERFALL
        W,      // 34
        W,      // 35
        W,      // 36
        W,      // 37
        W,      // 38: COLL_CURRENT_RIGHT
        W,      // 39: COLL_CURRENT_LEFT
        W,      // 3A: COLL_CURRENT_UP
        W,      // 3B: COLL_CURRENT_DOWN
        W,      // 3C
        W,      // 3D
        W,      // 3E
        W,      // 3F
        
        // 0x40-0x4F (Walk/brake tiles)
        L,      // 40: COLL_BRAKE
        L,      // 41: COLL_WALK_RIGHT
        L,      // 42: COLL_WALK_LEFT
        L,      // 43: COLL_WALK_UP
        L,      // 44: COLL_WALK_DOWN
        L,      // 45: COLL_BRAKE_45
        L,      // 46: COLL_BRAKE_46
        L,      // 47: COLL_BRAKE_47
        L,      // 48: COLL_GRASS_48
        L,      // 49: COLL_GRASS_49
        L,      // 4A: COLL_GRASS_4A
        L,      // 4B: COLL_GRASS_4B
        L,      // 4C: COLL_GRASS_4C
        L,      // 4D
        L,      // 4E
        L,      // 4F
        
        // 0x50-0x5F
        L,      // 50: COLL_WALK_RIGHT_ALT
        L,      // 51: COLL_WALK_LEFT_ALT
        L,      // 52: COLL_WALK_UP_ALT
        L,      // 53: COLL_WALK_DOWN_ALT
        L,      // 54: COLL_BRAKE_ALT
        L,      // 55: COLL_BRAKE_55
        L,      // 56: COLL_BRAKE_56
        L,      // 57: COLL_BRAKE_57
        L,      // 58
        L,      // 59
        L,      // 5A
        L,      // 5B: COLL_5B
        L,      // 5C
        L,      // 5D
        L,      // 5E
        L,      // 5F
        
        // 0x60-0x6F (Pit tiles)
        L,      // 60: COLL_PIT
        L,      // 61: COLL_VIRTUAL_BOY
        X,      // 62
        L,      // 63
        L,      // 64: COLL_64
        L,      // 65: COLL_65
        L,      // 66
        L,      // 67
        L,      // 68: COLL_PIT_68
        L,      // 69
        X,      // 6A
        L,      // 6B
        L,      // 6C
        L,      // 6D
        L,      // 6E
        L,      // 6F
        
        // 0x70-0x7F (Warp tiles)
        L,      // 70: COLL_WARP_CARPET_DOWN
        L,      // 71: COLL_DOOR
        L,      // 72: COLL_LADDER
        L,      // 73: COLL_STAIRCASE_73
        L,      // 74: COLL_CAVE_74
        L,      // 75: COLL_DOOR_75
        L,      // 76: COLL_WARP_CARPET_LEFT
        L,      // 77: COLL_WARP_77
        L,      // 78: COLL_WARP_CARPET_UP
        L,      // 79: COLL_DOOR_79
        L,      // 7A: COLL_STAIRCASE
        L,      // 7B: COLL_CAVE
        L,      // 7C: COLL_WARP_PANEL
        L,      // 7D: COLL_DOOR_7D
        L,      // 7E: COLL_WARP_CARPET_RIGHT
        L,      // 7F: COLL_WARP_7F
        
        // 0x80-0x8F
        X,      // 80
        X,      // 81
        X,      // 82
        X,      // 83
        X,      // 84
        L,      // 85
        L,      // 86
        L,      // 87
        X,      // 88
        X,      // 89
        X,      // 8A
        X,      // 8B
        X,      // 8C
        L,      // 8D
        L,      // 8E
        L,      // 8F
        
        // 0x90-0x9F (Counter/furniture tiles)
        X,      // 90: COLL_COUNTER
        X,      // 91: COLL_BOOKSHELF
        X,      // 92
        X,      // 93: COLL_PC
        X,      // 94: COLL_RADIO
        X,      // 95: COLL_TOWN_MAP
        X,      // 96: COLL_MART_SHELF
        X,      // 97: COLL_TV
        X,      // 98: COLL_COUNTER_98
        X,      // 99
        X,      // 9A
        X,      // 9B
        X,      // 9C: COLL_9C
        X,      // 9D: COLL_WINDOW
        X,      // 9E
        X,      // 9F: COLL_INCENSE_BURNER
        
        // 0xA0-0xAF (Ledge tiles)
        L,      // A0: COLL_HOP_RIGHT
        L,      // A1: COLL_HOP_LEFT
        L,      // A2: COLL_HOP_UP
        L,      // A3: COLL_HOP_DOWN
        L,      // A4: COLL_HOP_DOWN_RIGHT
        L,      // A5: COLL_HOP_DOWN_LEFT
        L,      // A6: COLL_HOP_UP_RIGHT
        L,      // A7: COLL_HOP_UP_LEFT
        L,      // A8
        L,      // A9
        L,      // AA
        L,      // AB
        L,      // AC
        L,      // AD
        L,      // AE
        L,      // AF
        
        // 0xB0-0xBF (Side walls - land tiles with directional blocking)
        L,      // B0: COLL_RIGHT_WALL
        L,      // B1: COLL_LEFT_WALL
        L,      // B2: COLL_UP_WALL
        L,      // B3: COLL_DOWN_WALL
        L,      // B4: COLL_DOWN_RIGHT_WALL
        L,      // B5: COLL_DOWN_LEFT_WALL
        L,      // B6: COLL_UP_RIGHT_WALL
        L,      // B7: COLL_UP_LEFT_WALL
        L,      // B8
        L,      // B9
        L,      // BA
        L,      // BB
        L,      // BC
        L,      // BD
        L,      // BE
        L,      // BF
        
        // 0xC0-0xCF (Side buoys - water tiles with directional blocking)
        W,      // C0: COLL_RIGHT_BUOY
        W,      // C1: COLL_LEFT_BUOY
        W,      // C2: COLL_UP_BUOY
        W,      // C3: COLL_DOWN_BUOY
        W,      // C4: COLL_DOWN_RIGHT_BUOY
        W,      // C5: COLL_DOWN_LEFT_BUOY
        W,      // C6: COLL_UP_RIGHT_BUOY
        W,      // C7: COLL_UP_LEFT_BUOY
        W,      // C8
        W,      // C9
        W,      // CA
        W,      // CB
        W,      // CC
        W,      // CD
        W,      // CE
        W,      // CF
        
        // 0xD0-0xDF
        L, L, L, L, L, L, L, L, L, L, L, L, L, L, L, L,
        
        // 0xE0-0xEF
        L, L, L, L, L, L, L, L, L, L, L, L, L, L, L, L,
        
        // 0xF0-0xFF
        L, L, L, L, L, L, L, L, L, L, L, L, L, L, L,
        X       // FF: COLL_FF (wall)
    }};
}

TilePermission CollisionPermissionTable::get_permission(uint8_t collision_byte) const {
    uint8_t perm = table_[collision_byte];
    // Strip TALK flag for base permission
    return static_cast<TilePermission>(perm & 0x0F);
}

bool CollisionPermissionTable::has_talk_flag(uint8_t collision_byte) const {
    return (table_[collision_byte] & 0x10) != 0;
}

bool CollisionPermissionTable::is_land(uint8_t collision_byte) const {
    return get_permission(collision_byte) == TilePermission::Land;
}

bool CollisionPermissionTable::is_water(uint8_t collision_byte) const {
    return get_permission(collision_byte) == TilePermission::Water;
}

bool CollisionPermissionTable::is_wall(uint8_t collision_byte) const {
    return get_permission(collision_byte) == TilePermission::Wall;
}

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

uint16_t Collision::get_occupant(
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
    
    return 0;  // No occupant
}

bool Collision::is_tile_walkable(uint8_t collision_byte, bool surfing) const {
    TilePermission perm = permissions_.get_permission(collision_byte);
    
    if (surfing) {
        // Can move on water tiles when surfing
        return perm == TilePermission::Land || perm == TilePermission::Water;
    } else {
        // On foot, only land tiles are walkable
        return perm == TilePermission::Land;
    }
}

bool Collision::is_side_wall_blocking(uint8_t collision_byte, Direction dir) const {
    // Reference: pokecrystal GetMovementPermissions
    // Side walls (0xB0-0xB7) and side buoys (0xC0-0xC7) block specific directions
    
    uint8_t hi = collision_byte & 0xF0;
    if (hi != CollisionNybble::HI_SIDE_WALLS && hi != CollisionNybble::HI_SIDE_BUOYS) {
        return false;
    }
    
    uint8_t lo = collision_byte & 0x07;
    
    // Wall types from collision_constants.asm:
    // 0: RIGHT_WALL (blocks left movement INTO the tile)
    // 1: LEFT_WALL (blocks right movement INTO the tile)
    // 2: UP_WALL (blocks down movement INTO the tile)
    // 3: DOWN_WALL (blocks up movement INTO the tile)
    // 4: DOWN_RIGHT_WALL
    // 5: DOWN_LEFT_WALL
    // 6: UP_RIGHT_WALL
    // 7: UP_LEFT_WALL
    
    // This checks: "does this tile block movement INTO it from direction dir?"
    // The direction is the direction we're ENTERING from, so we check opposite
    Direction enter_dir = opposite_direction(dir);
    
    switch (lo) {
        case 0: return enter_dir == Direction::Left;   // RIGHT_WALL blocks entering from left
        case 1: return enter_dir == Direction::Right;  // LEFT_WALL blocks entering from right
        case 2: return enter_dir == Direction::Down;   // UP_WALL blocks entering from down
        case 3: return enter_dir == Direction::Up;     // DOWN_WALL blocks entering from up
        case 4: return enter_dir == Direction::Down || enter_dir == Direction::Left;
        case 5: return enter_dir == Direction::Down || enter_dir == Direction::Right;
        case 6: return enter_dir == Direction::Up || enter_dir == Direction::Left;
        case 7: return enter_dir == Direction::Up || enter_dir == Direction::Right;
    }
    
    return false;
}

bool Collision::is_ledge(uint8_t collision_byte) const {
    uint8_t hi = collision_byte & 0xF0;
    return hi == CollisionNybble::HI_LEDGES;
}

Direction Collision::get_ledge_direction(uint8_t collision_byte) const {
    // Ledge tiles (0xA0-0xA7) specify which direction you can hop
    if (!is_ledge(collision_byte)) {
        return Direction::Down;  // Default
    }
    
    uint8_t lo = collision_byte & 0x07;
    
    // From collision_constants.asm:
    // A0: HOP_RIGHT
    // A1: HOP_LEFT
    // A2: HOP_UP
    // A3: HOP_DOWN
    // A4: HOP_DOWN_RIGHT
    // A5: HOP_DOWN_LEFT
    // A6: HOP_UP_RIGHT
    // A7: HOP_UP_LEFT
    
    switch (lo) {
        case 0: return Direction::Right;
        case 1: return Direction::Left;
        case 2: return Direction::Up;
        case 3: return Direction::Down;
        case 4: return Direction::Down;  // DOWN_RIGHT - primary is down
        case 5: return Direction::Down;  // DOWN_LEFT - primary is down
        case 6: return Direction::Up;    // UP_RIGHT - primary is up
        case 7: return Direction::Up;    // UP_LEFT - primary is up
        default: return Direction::Down;
    }
}

CollisionResult Collision::check_tile(const CollisionMap& map, int32_t x, int32_t y,
                                       bool surfing) const {
    uint8_t coll = map.get_collision(x, y);
    
    if (!is_tile_walkable(coll, surfing)) {
        CollisionResult result;
        result.allowed = false;
        result.reason = MoveBlockReason::Tile;
        result.collision_byte = coll;
        return result;
    }
    
    return CollisionResult::success();
}

CollisionResult Collision::check_side_walls(const CollisionMap& map,
                                            int32_t from_x, int32_t from_y,
                                            int32_t to_x, int32_t to_y,
                                            Direction dir) const {
    // Reference: pokecrystal GetMovementPermissions
    // Two checks:
    // 1. Standing tile blocks stepping OUT over its walled side
    // 2. Destination tile blocks stepping IN through its wall
    
    // Check source tile - does it block stepping OUT in direction dir?
    uint8_t from_coll = map.get_collision(from_x, from_y);
    uint8_t from_hi = from_coll & 0xF0;
    
    if (from_hi == CollisionNybble::HI_SIDE_WALLS || from_hi == CollisionNybble::HI_SIDE_BUOYS) {
        uint8_t lo = from_coll & 0x07;
        
        // Check if this wall blocks movement OUT in direction dir
        // E.g., RIGHT_WALL (0) blocks stepping RIGHT out of this tile
        bool blocks_out = false;
        switch (lo) {
            case 0: blocks_out = (dir == Direction::Right); break;
            case 1: blocks_out = (dir == Direction::Left); break;
            case 2: blocks_out = (dir == Direction::Up); break;
            case 3: blocks_out = (dir == Direction::Down); break;
            case 4: blocks_out = (dir == Direction::Down || dir == Direction::Right); break;
            case 5: blocks_out = (dir == Direction::Down || dir == Direction::Left); break;
            case 6: blocks_out = (dir == Direction::Up || dir == Direction::Right); break;
            case 7: blocks_out = (dir == Direction::Up || dir == Direction::Left); break;
        }
        
        if (blocks_out) {
            CollisionResult result;
            result.allowed = false;
            result.reason = MoveBlockReason::SideWall;
            result.collision_byte = from_coll;
            return result;
        }
    }
    
    // Check destination tile - does it block stepping IN from direction dir?
    uint8_t to_coll = map.get_collision(to_x, to_y);
    if (is_side_wall_blocking(to_coll, dir)) {
        CollisionResult result;
        result.allowed = false;
        result.reason = MoveBlockReason::SideWall;
        result.collision_byte = to_coll;
        return result;
    }
    
    return CollisionResult::success();
}

CollisionResult Collision::check_occupancy(const std::vector<CollisionEntity>& entities,
                                           int32_t x, int32_t y,
                                           uint16_t mover_id) const {
    uint16_t occupant = get_occupant(entities, x, y, mover_id);
    
    if (occupant != 0) {
        CollisionResult result;
        result.allowed = false;
        result.reason = MoveBlockReason::Entity;
        result.blocking_entity = occupant;
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
    
    // 3. Tile walkability
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
    return CollisionResult::success();
}

} // namespace enginemon
