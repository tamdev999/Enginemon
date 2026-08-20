#pragma once
// engine/world/world_manager.hpp
// World continuity manager - warps, connections, map transitions
//
// Reference: Gen2Recomped OverworldController.lua takeWarp/crossConnection
// Reference: pokecrystal home/map.asm EnterMap, engine/overworld/events.asm
//
// Handles:
// - Warp transitions (door → interior, interior → exterior)
// - Map connections (exterior → exterior seamless crossing)
// - LAST_MAP/LAST_WARP resolution
// - Player position/facing preservation

#include "engine/core/types.hpp"
#include "engine/core/game_state.hpp"
#include "engine/world/runtime_map.hpp"
#include <cstdint>
#include <string>
#include <functional>
#include <optional>
#include <unordered_map>

namespace enginemon {

//=============================================================================
// WARP RESULT
//=============================================================================

struct WarpResult {
    bool success = false;
    std::string error;
    
    std::string target_map_id;      // Resolved destination
    int32_t target_x = 0;
    int32_t target_y = 0;
    Direction target_facing = Direction::Down;
    
    bool is_indoor_to_outdoor = false;  // For LAST_MAP tracking
    bool is_outdoor_to_indoor = false;
};

//=============================================================================
// CONNECTION RESULT
//=============================================================================

struct ConnectionResult {
    bool success = false;
    std::string error;
    
    std::string target_map_id;
    int32_t target_x = 0;
    int32_t target_y = 0;
    Direction target_facing = Direction::Down;
    
    // For seamless crossing, the "pre-seam" position
    int32_t seam_x = 0;
    int32_t seam_y = 0;
};

//=============================================================================
// WORLD MANAGER
//=============================================================================

class WorldManager {
public:
    WorldManager();
    ~WorldManager();
    
    //=========================================================================
    // MAP LOADING
    //=========================================================================
    
    // Map loader callback - returns RuntimeMap for a map_id
    using MapLoader = std::function<std::optional<RuntimeMap>(const std::string& map_id)>;
    void set_map_loader(MapLoader loader) { map_loader_ = std::move(loader); }
    
    // Load initial map
    bool load_map(const std::string& map_id);
    
    // Get current map
    const RuntimeMap* current_map() const { return current_map_.has_value() ? &current_map_.value() : nullptr; }
    const std::string& current_map_id() const { return current_map_id_; }
    
    //=========================================================================
    // WARPS
    // Reference: Gen2Recomped takeWarp, Warp.destination
    //=========================================================================
    
    // Check if position has a warp
    const RuntimeWarp* get_warp_at(int32_t x, int32_t y) const;
    
    // Resolve and execute a warp
    // Returns destination info without actually transitioning
    WarpResult resolve_warp(const RuntimeWarp& warp, const GameState& state) const;
    
    // Prepare warp: resolve destination + write warp_memory from current position.
    // Does NOT load the new map, does NOT update state.player to destination.
    // Safe to call before staging — only touches warp_memory (wBackupWarp/LAST_MAP).
    WarpResult prepare_warp(const RuntimeWarp& warp, GameState& state);
    
    // Commit warp: load the new map + write state.player to destination.
    // Call only after transition preparation (GPU/tileset staging) succeeds.
    void commit_warp(const WarpResult& result, GameState& state);
    
    // Execute warp transition (loads new map) — resolve + prepare + commit atomically
    // Legacy: use only when prepare/commit split is not needed.
    WarpResult execute_warp(const RuntimeWarp& warp, GameState& state);
    
    // Execute warp by position (convenience)
    WarpResult execute_warp_at(int32_t x, int32_t y, GameState& state);
    
    //=========================================================================
    // CONNECTIONS
    // Reference: Gen2Recomped crossConnection, connectionLanding
    //=========================================================================
    
    // Get connection in direction (if any)
    const RuntimeConnection* get_connection(ConnectionDirection dir) const;
    
    // Check if stepping in direction would cross a connection
    bool is_at_connection_edge(int32_t x, int32_t y, Direction facing) const;
    
    // Resolve connection landing position
    // Returns landing info without actually transitioning
    ConnectionResult resolve_connection(
        int32_t player_x, int32_t player_y, 
        Direction facing
    ) const;
    
    // Execute connection crossing (loads new map)
    ConnectionResult execute_connection(
        int32_t player_x, int32_t player_y,
        Direction facing,
        GameState& state
    );
    
    // Commit connection: load new map + write state.player to destination.
    // Call only after transition preparation succeeds.
    void commit_connection(const ConnectionResult& result, GameState& state);
    
    //=========================================================================
    // WARP MEMORY
    // For LAST_MAP exits
    //=========================================================================
    
    // Remember outdoor position for LAST_MAP
    void remember_outdoor(const std::string& map_id, int32_t x, int32_t y, GameState& state);
    
    // Remember backup warp for LAST_WARP (GSC)
    void remember_backup_warp(const std::string& map_id, int32_t x, int32_t y, GameState& state);
    
    //=========================================================================
    // COLLISION CALLBACK
    // For connection landing validation
    //=========================================================================
    
    using CollisionCallback = std::function<bool(const std::string& map_id, int32_t x, int32_t y, bool surfing)>;
    void set_collision_callback(CollisionCallback cb) { collision_cb_ = std::move(cb); }
    
    //=========================================================================
    // TRANSITION CALLBACK
    // Called when map changes
    //=========================================================================
    
    using TransitionCallback = std::function<void(const std::string& from_map, const std::string& to_map)>;
    void set_transition_callback(TransitionCallback cb) { transition_cb_ = std::move(cb); }

private:
    // Current map
    std::optional<RuntimeMap> current_map_;
    std::string current_map_id_;
    
    // Callbacks
    MapLoader map_loader_;
    CollisionCallback collision_cb_;
    TransitionCallback transition_cb_;
    
    // Direction to connection direction mapping
    static ConnectionDirection direction_to_connection(Direction dir);
    
    // Calculate connection landing position
    // Reference: Gen2Recomped connectionLanding math
    bool calculate_connection_landing(
        const RuntimeConnection& conn,
        int32_t player_x, int32_t player_y,
        Direction facing,
        int32_t& out_x, int32_t& out_y
    ) const;
};

//=============================================================================
// SPECIAL MAP IDS
// From pokecrystal constants
//=============================================================================

namespace SpecialMapId {
    constexpr const char* LAST_MAP = "LAST_MAP";
    constexpr const char* LAST_WARP = "LAST_WARP";
}

} // namespace enginemon
