// engine/world/world_manager.cpp
// World continuity manager implementation
//
// Reference: Gen2Recomped OverworldController.lua takeWarp/crossConnection
// Reference: pokecrystal home/map.asm EnterMap

#include "engine/world/world_manager.hpp"
#include <algorithm>

namespace enginemon {

WorldManager::WorldManager() = default;
WorldManager::~WorldManager() = default;

//=============================================================================
// MAP LOADING
//=============================================================================

bool WorldManager::load_map(const std::string& map_id) {
    if (!map_loader_) return false;
    
    auto loaded = map_loader_(map_id);
    if (!loaded.has_value()) return false;
    
    std::string old_map_id = current_map_id_;
    current_map_ = std::move(loaded.value());
    current_map_id_ = map_id;
    
    if (transition_cb_ && !old_map_id.empty()) {
        transition_cb_(old_map_id, map_id);
    }
    
    return true;
}

//=============================================================================
// WARPS
//=============================================================================

const RuntimeWarp* WorldManager::get_warp_at(int32_t x, int32_t y) const {
    if (!current_map_.has_value()) return nullptr;
    
    for (const auto& warp : current_map_->warps) {
        if (warp.x == static_cast<uint8_t>(x) && warp.y == static_cast<uint8_t>(y)) {
            return &warp;
        }
    }
    return nullptr;
}

WarpResult WorldManager::resolve_warp(const RuntimeWarp& warp, const GameState& state) const {
    WarpResult result;
    
    std::string target_map = warp.target_map_id;
    int32_t target_x = 0;
    int32_t target_y = 0;
    
    // Handle special map IDs
    if (target_map == SpecialMapId::LAST_MAP) {
        // Use remembered outdoor location
        if (state.warp_memory.map_id.empty()) {
            result.error = "LAST_MAP with no remembered outdoor";
            return result;
        }
        target_map = state.warp_memory.map_id;
        target_x = state.warp_memory.x;
        target_y = state.warp_memory.y;
        result.is_indoor_to_outdoor = true;
    } else if (target_map == SpecialMapId::LAST_WARP) {
        // Use backup warp
        if (state.warp_memory.backup_map_id.empty()) {
            result.error = "LAST_WARP with no backup warp";
            return result;
        }
        target_map = state.warp_memory.backup_map_id;
        target_x = state.warp_memory.backup_x;
        target_y = state.warp_memory.backup_y;
    } else {
        // Load target map to get warp destination position
        if (!map_loader_) {
            result.error = "No map loader";
            return result;
        }
        
        auto target = map_loader_(target_map);
        if (!target.has_value()) {
            result.error = "Target map not found: " + target_map;
            return result;
        }
        
        // Find the target warp by index
        // Crystal warp indices are 1-based: warp_index=1 → warps[0]
        // CRITICAL: No silent fallback - invalid index must fail explicitly
        uint8_t warp_index = warp.target_warp_index;
        if (warp_index == 0) {
            // warp_index=0 is invalid in Crystal (indices are 1-based)
            result.error = "Invalid warp index 0 (Crystal indices are 1-based)";
            return result;
        }
        if (target->warps.empty()) {
            result.error = "Target map has no warps: " + target_map;
            return result;
        }
        if (static_cast<size_t>(warp_index - 1) >= target->warps.size()) {
            result.error = "Warp index " + std::to_string(warp_index) + 
                           " out of range (map has " + std::to_string(target->warps.size()) + " warps)";
            return result;
        }
        
        // Valid warp index
        const auto& dest_warp = target->warps[warp_index - 1];
        target_x = dest_warp.x;
        target_y = dest_warp.y;
        
        // Check if this is outdoor→indoor transition
        if (current_map_.has_value() && current_map_->is_outdoor && !target->is_outdoor) {
            result.is_outdoor_to_indoor = true;
        }
        if (current_map_.has_value() && !current_map_->is_outdoor && target->is_outdoor) {
            result.is_indoor_to_outdoor = true;
        }
    }
    
    result.success = true;
    result.target_map_id = target_map;
    result.target_x = target_x;
    result.target_y = target_y;
    // Preserve pre-warp facing (Gen2Recomped takeWarp: local facing = self.player.facing)
    // Door auto-step changes facing to Down when scripted south step begins, not here
    result.target_facing = state.player.facing;
    
    return result;
}

WarpResult WorldManager::execute_warp(const RuntimeWarp& warp, GameState& state) {
    WarpResult result = resolve_warp(warp, state);
    if (!result.success) return result;
    
    // Remember outdoor position before entering indoor
    if (result.is_outdoor_to_indoor && current_map_.has_value()) {
        remember_outdoor(current_map_id_, 
                        state.player.x, state.player.y, state);
    }
    
    // Remember backup warp (GSC: wBackupWarp)
    remember_backup_warp(current_map_id_, 
                        state.player.x, state.player.y, state);
    
    // Load new map
    if (!load_map(result.target_map_id)) {
        result.success = false;
        result.error = "Failed to load map: " + result.target_map_id;
        return result;
    }
    
    // Update player state
    state.player.current_map_id = result.target_map_id;
    state.player.x = result.target_x;
    state.player.y = result.target_y;
    
    return result;
}

WarpResult WorldManager::execute_warp_at(int32_t x, int32_t y, GameState& state) {
    const RuntimeWarp* warp = get_warp_at(x, y);
    if (!warp) {
        WarpResult result;
        result.error = "No warp at position";
        return result;
    }
    return execute_warp(*warp, state);
}

//=============================================================================
// CONNECTIONS
//=============================================================================

ConnectionDirection WorldManager::direction_to_connection(Direction dir) {
    switch (dir) {
        case Direction::Up: return ConnectionDirection::North;
        case Direction::Down: return ConnectionDirection::South;
        case Direction::Left: return ConnectionDirection::West;
        case Direction::Right: return ConnectionDirection::East;
    }
    return ConnectionDirection::North;
}

const RuntimeConnection* WorldManager::get_connection(ConnectionDirection dir) const {
    if (!current_map_.has_value()) return nullptr;
    
    for (const auto& conn : current_map_->connections) {
        if (conn.direction == dir) {
            return &conn;
        }
    }
    return nullptr;
}

bool WorldManager::is_at_connection_edge(int32_t x, int32_t y, Direction facing) const {
    if (!current_map_.has_value()) return false;
    
    // Use collision dimensions (cells), not render tile dimensions
    // Player coordinates are in the 16×16 cell grid
    int cell_w = current_map_->collision_width();
    int cell_h = current_map_->collision_height();
    
    switch (facing) {
        case Direction::Up:
            return y == 0;
        case Direction::Down:
            return y == cell_h - 1;
        case Direction::Left:
            return x == 0;
        case Direction::Right:
            return x == cell_w - 1;
    }
    return false;
}

bool WorldManager::calculate_connection_landing(
    const RuntimeConnection& conn,
    int32_t player_x, int32_t player_y,
    Direction facing,
    int32_t& out_x, int32_t& out_y
) const {
    // Reference: Gen2Recomped connectionLanding
    // destX = curX - offset*2 (clamped to [0, destWidth-1])
    // destY depends on direction
    
    if (!map_loader_) return false;
    
    auto dest = map_loader_(conn.target_map_id);
    if (!dest.has_value()) return false;
    
    // Use collision dimensions (cells), not render tile dimensions
    // Reference: Gen2Recomped - local destW, destH = dest.width * 2, dest.height * 2
    int dest_w = dest->collision_width();
    int dest_h = dest->collision_height();
    
    // Strip offset is in blocks, convert to tiles (*2)
    int offset_tiles = conn.strip_offset * 2;
    
    switch (facing) {
        case Direction::Up:
            // Crossing north: land at bottom of destination
            out_x = player_x - offset_tiles;
            out_y = dest_h - 1;
            break;
        case Direction::Down:
            // Crossing south: land at top of destination
            out_x = player_x - offset_tiles;
            out_y = 0;
            break;
        case Direction::Left:
            // Crossing west: land at right edge of destination
            out_x = dest_w - 1;
            out_y = player_y - offset_tiles;
            break;
        case Direction::Right:
            // Crossing east: land at left edge of destination
            out_x = 0;
            out_y = player_y - offset_tiles;
            break;
    }
    
    // Clamp to map bounds
    out_x = std::max(0, std::min(dest_w - 1, out_x));
    out_y = std::max(0, std::min(dest_h - 1, out_y));
    
    return true;
}

ConnectionResult WorldManager::resolve_connection(
    int32_t player_x, int32_t player_y,
    Direction facing
) const {
    ConnectionResult result;
    
    if (!current_map_.has_value()) {
        result.error = "No current map";
        return result;
    }
    
    // Check if at edge
    if (!is_at_connection_edge(player_x, player_y, facing)) {
        result.error = "Not at map edge";
        return result;
    }
    
    // Get connection in this direction
    ConnectionDirection conn_dir = direction_to_connection(facing);
    const RuntimeConnection* conn = get_connection(conn_dir);
    if (!conn) {
        result.error = "No connection in this direction";
        return result;
    }
    
    // Calculate landing position
    int32_t land_x = 0, land_y = 0;
    if (!calculate_connection_landing(*conn, player_x, player_y, facing, land_x, land_y)) {
        result.error = "Failed to calculate landing";
        return result;
    }
    
    // Validate landing (collision check)
    if (collision_cb_ && !collision_cb_(conn->target_map_id, land_x, land_y, false)) {
        result.error = "Landing blocked by collision";
        return result;
    }
    
    result.success = true;
    result.target_map_id = conn->target_map_id;
    result.target_x = land_x;
    result.target_y = land_y;
    result.target_facing = facing;
    
    // Seam position (one step back from landing)
    int dx = 0, dy = 0;
    switch (facing) {
        case Direction::Up: dy = 1; break;
        case Direction::Down: dy = -1; break;
        case Direction::Left: dx = 1; break;
        case Direction::Right: dx = -1; break;
    }
    result.seam_x = land_x + dx;
    result.seam_y = land_y + dy;
    
    return result;
}

ConnectionResult WorldManager::execute_connection(
    int32_t player_x, int32_t player_y,
    Direction facing,
    GameState& state
) {
    ConnectionResult result = resolve_connection(player_x, player_y, facing);
    if (!result.success) return result;
    
    // Load new map
    if (!load_map(result.target_map_id)) {
        result.success = false;
        result.error = "Failed to load map: " + result.target_map_id;
        return result;
    }
    
    // Update player state
    state.player.current_map_id = result.target_map_id;
    state.player.x = result.target_x;
    state.player.y = result.target_y;
    state.player.facing = result.target_facing;
    
    return result;
}

//=============================================================================
// WARP MEMORY
//=============================================================================

void WorldManager::remember_outdoor(const std::string& map_id, int32_t x, int32_t y, GameState& state) {
    state.warp_memory.map_id = map_id;
    state.warp_memory.x = x;
    state.warp_memory.y = y;
}

void WorldManager::remember_backup_warp(const std::string& map_id, int32_t x, int32_t y, GameState& state) {
    state.warp_memory.backup_map_id = map_id;
    state.warp_memory.backup_x = x;
    state.warp_memory.backup_y = y;
}

} // namespace enginemon
