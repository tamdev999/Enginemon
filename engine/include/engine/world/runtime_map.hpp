#pragma once
// engine/world/runtime_map.hpp
// Runtime-native map structures for the engine
//
// These types are what the engine operates on at runtime.
// They are populated from packages (which are the boundary between
// frontend extraction and runtime execution).
//
// No ROM addresses, no Game Boy concepts - purely semantic data.

#include "engine/core/types.hpp"
#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>

namespace enginemon {

//=============================================================================
// MAP PALETTE POLICY (from Crystal's wMapTimeOfDay)
//=============================================================================

// Determines how a map selects its palette based on time
// From pokecrystal constants/map_constants.asm:
//   PALETTE_AUTO EQU 0  ; follow RTC
//   PALETTE_DAY  EQU 1  ; always Day
//   PALETTE_NITE EQU 2  ; always Night  
//   PALETTE_MORN EQU 3  ; always Morning
//   PALETTE_DARK EQU 4  ; dark cave (requires Flash)
enum class PalettePolicy : uint8_t {
    Auto = 0,   // Follow RTC time-of-day
    Day  = 1,   // Always use Day palette
    Nite = 2,   // Always use Night palette
    Morn = 3,   // Always use Morning palette
    Dark = 4    // Dark cave (requires Flash)
};

//=============================================================================
// MAP ENVIRONMENT TYPE (from Crystal's environment byte)
//=============================================================================

// Determines the color set used for a map
// From pokecrystal constants/map_constants.asm:
//   TOWN       EQU 1 → OutdoorColors
//   ROUTE      EQU 2 → OutdoorColors
//   INDOOR     EQU 3 → IndoorColors
//   CAVE       EQU 4 → DungeonColors
//   ENVIRONMENT_5 EQU 5 → DungeonColors (unused)
//   GATE       EQU 6 → IndoorColors
//   DUNGEON    EQU 7 → DungeonColors
//
// Simplified to 3 categories that affect palette row selection:
//   Outdoor: Morn/Day/Nite/Dark → corresponding row directly
//   Indoor:  Morn/Day → Indoor row, Nite → Nite row, Dark → Dark row
//   Dungeon: follows same rules as Indoor (uses DungeonColors)
enum class Environment : uint8_t {
    Outdoor = 0,  // TOWN, ROUTE (outdoor colors)
    Indoor  = 1,  // INDOOR, GATE (indoor colors)
    Dungeon = 2   // CAVE, DUNGEON, ENVIRONMENT_5 (dungeon colors)
};

// Convert Crystal's raw environment byte to our Environment enum
inline Environment environment_from_crystal(uint8_t raw) {
    switch (raw) {
        case 1:  // TOWN
        case 2:  // ROUTE
            return Environment::Outdoor;
        case 3:  // INDOOR
        case 6:  // GATE
            return Environment::Indoor;
        case 4:  // CAVE
        case 5:  // ENVIRONMENT_5
        case 7:  // DUNGEON
        default:
            return Environment::Dungeon;
    }
}

//=============================================================================
// RUNTIME MAP EVENTS
// These mirror the semantic structures from package loading but live
// in the engine namespace with no frontend dependencies.
//=============================================================================

// Warp destination
struct RuntimeWarp {
    uint8_t x, y;                   // Position in this map (tiles)
    std::string target_map_id;      // Destination map (semantic ID)
    uint8_t target_warp_index;      // Which warp in target map
};

// Coordinate-triggered event (step triggers)
struct RuntimeCoordEvent {
    uint8_t x, y;
    std::string script_id;          // Script to run (semantic ID)
    uint16_t scene_id;              // Scene script index (0 = always active)
};

// Background event types
enum class RuntimeBgEventType : uint8_t {
    Read = 0,           // Sign, bookshelf - any facing
    Up = 1,             // Requires facing up
    Down = 2,           // Requires facing down
    Right = 3,          // Requires facing right
    Left = 4,           // Requires facing left
    IfSet = 5,          // Flag check
    IfNotSet = 6,       // Inverse flag check
    HiddenItem = 7,     // Hidden item on ground
    Copy = 8,           // Copy tile
};

// Background event (signs, hidden items, etc.)
struct RuntimeBgEvent {
    uint8_t x, y;
    RuntimeBgEventType type;
    std::string script_id;          // For signs/readable
    std::string item_id;            // For hidden items (semantic ID)
    uint8_t quantity;
};

// NPC/Object event
struct RuntimeObject {
    uint8_t local_id;               // Object ID within map (1-indexed)
    uint8_t x, y;                   // Position
    std::string sprite_id;          // Sprite to use (semantic ID)
    uint8_t movement_type;          // Movement behavior
    uint8_t movement_radius_x;
    uint8_t movement_radius_y;
    uint8_t hour_start, hour_end;   // Active hours (0 = always)
    uint8_t time_of_day;            // When visible (0 = always)
    bool is_trainer;
    uint8_t trainer_sight_range;
    std::string script_id;          // Interaction script (semantic ID)
    std::string visibility_flag;    // Flag controlling visibility (semantic ID or empty)
};

// Map connection direction (for exterior connections)
enum class ConnectionDirection : uint8_t {
    North, South, East, West
};

// Map connection (how maps link together)
struct RuntimeConnection {
    ConnectionDirection direction;
    std::string target_map_id;      // Semantic ID
    int32_t strip_offset;           // Tile offset within connection strip
    uint8_t strip_length;           // Length of connection in tiles
};

//=============================================================================
// RUNTIME MAP
// Complete map loaded from package, ready for gameplay
//=============================================================================

struct RuntimeMap {
    // Identity
    std::string map_id;             // e.g., "new_bark_town"
    std::string display_name;       // e.g., "New Bark Town"
    
    // Dimensions (in blocks/metatiles)
    uint8_t width;
    uint8_t height;
    
    // Tileset reference
    std::string tileset_id;
    
    // Block data (metatile indices, width*height bytes)
    std::vector<uint8_t> blocks;
    
    // Border block for map edges
    uint8_t border_block;
    
    // Palette resolution fields
    Environment environment = Environment::Outdoor;   // Color set selection
    PalettePolicy time_policy = PalettePolicy::Auto;  // Time-based palette selection
    
    // Map properties (legacy - environment_type preserved for compatibility)
    uint8_t environment_type;       // Raw Crystal byte (TOWN, ROUTE, INDOOR, etc.)
    bool is_outdoor;
    bool phone_service_disabled;
    uint8_t lighting;
    
    // Music/audio
    std::string music_id;
    std::string fish_group_id;
    
    // Location info
    std::string landmark_id;
    std::string map_script_id;
    
    // Events
    std::vector<RuntimeWarp> warps;
    std::vector<RuntimeCoordEvent> coord_events;
    std::vector<RuntimeBgEvent> bg_events;
    std::vector<RuntimeObject> objects;
    std::vector<RuntimeConnection> connections;
    
    // Scripts - pre-decoded Lua code keyed by semantic script_id
    // Maps script_id (e.g., "bg_event_0", "object_script_1") to generated Lua
    // Runtime loads scripts from here - no ROM/frontend access needed
    std::unordered_map<std::string, std::string> scripts;
    
    // Helper: get tile dimensions (blocks × 4, since each block is 4×4 8px tiles)
    // Used for rendering (8×8 pixel grid)
    int tile_width() const { return width * 4; }
    int tile_height() const { return height * 4; }
    
    // Helper: get collision/cell dimensions (blocks × 2, since each block is 2×2 16px cells)
    // Used for player coordinates, collision, warps, connections (16×16 pixel grid)
    // Reference: Gen2Recomped Map.lua widthCells = def.width * 2
    int collision_width() const { return width * 2; }
    int collision_height() const { return height * 2; }
};

} // namespace enginemon
