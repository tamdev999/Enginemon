#pragma once
// engine/core/game_state.hpp
// Minimal game state for save/load and continuity
//
// Serializes/restores enough to prove ownership boundaries:
// - Current map (semantic ID)
// - Player position/facing
// - Event flags
// - RNG state (if present)
//
// Reference: Gen2Recomped save system, pokecrystal SRAM layout

#include "engine/core/types.hpp"
#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>

namespace enginemon {

//=============================================================================
// PLAYER SAVE STATE
//=============================================================================

struct PlayerSaveState {
    std::string current_map_id;     // Semantic map ID
    int32_t x = 0;                  // Tile position
    int32_t y = 0;
    Direction facing = Direction::Down;
    bool surfing = false;
    bool on_bike = false;
};

//=============================================================================
// WARP MEMORY
// For LAST_MAP/LAST_WARP style exits (pokered wLastMap, GSC wBackupWarp)
//=============================================================================

struct WarpMemory {
    std::string map_id;             // Last outdoor map (for LAST_MAP exits)
    int32_t x = 0;
    int32_t y = 0;
    
    // Backup warp for LAST_WARP (GSC)
    std::string backup_map_id;
    int32_t backup_x = 0;
    int32_t backup_y = 0;
};

//=============================================================================
// RNG STATE
// Determinism requires capturing RNG state
//=============================================================================

struct RngState {
    uint64_t seed = 0;
    uint64_t state = 0;             // Current generator state
};

//=============================================================================
// GAME STATE
// Complete saveable state
//=============================================================================

struct GameState {
    // Player
    PlayerSaveState player;
    
    // Warp memory
    WarpMemory warp_memory;
    
    // Event flags (semantic IDs)
    std::unordered_set<std::string> flags;
    
    // Variables (semantic ID -> value)
    std::unordered_map<std::string, int32_t> variables;
    
    // RNG
    RngState rng;
    
    // Playtime (frames or seconds)
    uint64_t playtime_frames = 0;
    
    //=========================================================================
    // FLAG OPERATIONS
    //=========================================================================
    
    void set_flag(const std::string& flag_id) {
        flags.insert(flag_id);
    }
    
    void clear_flag(const std::string& flag_id) {
        flags.erase(flag_id);
    }
    
    bool check_flag(const std::string& flag_id) const {
        return flags.find(flag_id) != flags.end();
    }
    
    //=========================================================================
    // VARIABLE OPERATIONS
    //=========================================================================
    
    void set_var(const std::string& var_id, int32_t value) {
        variables[var_id] = value;
    }
    
    int32_t get_var(const std::string& var_id) const {
        auto it = variables.find(var_id);
        return it != variables.end() ? it->second : 0;
    }
    
    //=========================================================================
    // SERIALIZATION (minimal binary format)
    //=========================================================================
    
    // Serialize to bytes
    std::vector<uint8_t> serialize() const;
    
    // Deserialize from bytes
    static GameState deserialize(const std::vector<uint8_t>& data);
    
    // Validate deserialized state
    bool is_valid() const;
};

//=============================================================================
// SAVE SLOT
// Wrapper with metadata
//=============================================================================

struct SaveSlot {
    bool occupied = false;
    std::string player_name;        // For display
    std::string map_display_name;   // Current location for display
    uint64_t playtime_frames = 0;
    uint32_t save_timestamp = 0;    // Unix timestamp
    
    GameState state;
};

} // namespace enginemon
