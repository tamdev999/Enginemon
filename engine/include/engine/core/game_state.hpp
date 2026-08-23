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
    
    // Initialize with seed
    void set_seed(uint64_t s) {
        seed = s;
        state = s;
    }
    
    // Generate next random value (LCG)
    // Uses same parameters as pokecrystal for compatibility
    uint32_t next() {
        // LCG parameters from Numerical Recipes (same as original HeadlessGameLoop)
        state = state * 1664525 + 1013904223;
        return static_cast<uint32_t>(state);
    }
};

//=============================================================================
// NPC SAVE STATE
// Per-NPC runtime state that affects gameplay determinism
// Must be saved/restored for deterministic simulation resume
//=============================================================================

struct NpcSaveState {
    uint16_t id = 0;            // Object local ID (1-indexed)
    int32_t x = 0;              // Current tile position
    int32_t y = 0;
    Direction facing = Direction::Down;
    bool is_moving = false;
    int32_t idle_timer = 0;     // Frames until next movement attempt - CRITICAL for determinism
    int32_t target_x = 0;       // Movement target X (during move)
    int32_t target_y = 0;       // Movement target Y
    int32_t move_progress = 0;  // Ticks into current movement
    bool frozen = false;        // Script is interacting with this NPC
    bool visible = true;        // Visibility state
};

//=============================================================================
// DESERIALIZATION RESULT TYPES (Audit 5)
// Explicit error handling for save loading - corrupt saves must not silently
// become default/new-game states
//=============================================================================

enum class DeserializeError {
    Success,
    TruncatedData,          // Data too short
    InvalidMagic,           // Wrong magic number
    UnsupportedVersion,     // Schema version mismatch
    CorruptedPayload,       // Parse error within payload
};

// Forward declaration - DeserializeResult is defined after GameState
struct DeserializeResult;

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
    
    // Variables (semantic ID -> integer value)
    std::unordered_map<std::string, int32_t> variables;
    
    // Variable sprite assignments (slot_name → assigned sprite_id string).
    // ...existing doc...
    std::unordered_map<std::string, std::string> variable_sprites;

    // Day Care species occupancy.
    // Source: Crystal wBreedMon1Species / wBreedMon2Species (WRAM bank 1).
    // SpeciesId 0 = slot is empty (no Pokémon deposited).
    // SpeciesId 1-251 = Pokémon species currently in the Day Care slot.
    // This state drives the overworld sprite for "daycare:1" / "daycare:2" objects
    // (Route 34, outdoor_sprites.asm).
    //
    // Visibility of the Day Care Pokémon objects is separately controlled by the
    // event flags EVENT_DAY_CARE_MON_1 / EVENT_DAY_CARE_MON_2 (in the flags set).
    // When the flag is SET the object is hidden; when CLEARED it is visible.
    // The Route34EggCheckCallback synchronizes flags with the actual occupancy.
    //
    // daycare_slot[0] = slot 1 (wBreedMon1Species equivalent)
    // daycare_slot[1] = slot 2 (wBreedMon2Species equivalent)
    std::array<SpeciesId, 2> daycare_slot = {0, 0};
    
    // RNG
    RngState rng;
    
    // NPC states per map (map_id -> NPC states)
    // This captures all gameplay-relevant NPC runtime state for deterministic resume
    std::unordered_map<std::string, std::vector<NpcSaveState>> npc_states;
    
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
    
    // Deserialize from bytes with explicit error handling (Audit 5)
    // Returns DeserializeResult with error code - corrupt saves never silently
    // become valid default states
    static DeserializeResult try_deserialize(const std::vector<uint8_t>& data);
    
    // Validate deserialized state
    bool is_valid() const;
};

//=============================================================================
// DESERIALIZATION RESULT (Audit 5)
// Must be defined after GameState since it contains a GameState member
//=============================================================================

struct DeserializeResult {
    DeserializeError error = DeserializeError::Success;
    GameState state;
    
    bool ok() const { return error == DeserializeError::Success; }
    explicit operator bool() const { return ok(); }
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
