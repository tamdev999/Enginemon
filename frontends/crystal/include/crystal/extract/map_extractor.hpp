#pragma once
// crystal/extract/map_extractor.hpp
// Generic map extraction from Crystal ROM
// 
// Extracts maps to fully semantic structures with NO ROM offsets/banks.
// The output is pure game data ready for the engine's GameDefinition.
//
// Crystal group/index are frontend-only - converted to stable semantic IDs.
// ROM addresses are debug-only metadata, stripped in release builds.

#include "crystal/rom/loader.hpp"
#include "crystal/rom/profile.hpp"
#include <cstdint>
#include <string>
#include <vector>
#include <unordered_set>
#include <optional>
#include <unordered_map>

namespace crystal {

//=============================================================================
// SEMANTIC OUTPUT STRUCTURES
// These are Crystal-agnostic - no ROM addresses, no banks, no GB concepts
// IDs are stable semantic strings, not Crystal group/index pairs
//=============================================================================

// Direction for connections
enum class Direction { North, South, East, West };

// Map connection (how maps link together)
// Stored raw first, then resolved to world coordinates in a separate stage
struct MapConnection {
    Direction direction;
    std::string target_map_id;      // Semantic ID, e.g., "route_29"
    int32_t strip_offset;           // Tile offset within connection strip
    uint8_t strip_length;           // Length of connection in tiles
};

// Warp destination
struct WarpPoint {
    uint8_t x, y;                   // Position in this map (tiles)
    std::string target_map_id;      // Destination map (semantic ID)
    uint8_t target_warp_index;      // Which warp in target map
};

// Coordinate-triggered event (step triggers)
struct CoordEvent {
    uint8_t x, y;
    std::string script_id;          // Script to run (semantic ID)
    uint16_t scene_id;              // Scene script index (0 = always active, -1/0xFF = always)
    
    // ROM address for script decoding (frontend use only, not serialized to package)
    uint32_t script_rom_address = 0;
};

// Background event (signs, hidden items, etc.)
// From pokecrystal constants/script_constants.asm BGEVENT_* constants
enum class BgEventType {
    Read = 0,           // BGEVENT_READ - any facing
    FacingUp = 1,       // BGEVENT_UP - requires facing up
    FacingDown = 2,     // BGEVENT_DOWN - requires facing down
    FacingRight = 3,    // BGEVENT_RIGHT - requires facing right
    FacingLeft = 4,     // BGEVENT_LEFT - requires facing left
    IfSet = 5,          // BGEVENT_IFSET - conditional script (flag set)
    IfNotSet = 6,       // BGEVENT_IFNOTSET - conditional script (flag not set)
    HiddenItem = 7,     // BGEVENT_ITEM - hidden item on ground
    Copy = 8,           // BGEVENT_COPY - copy tile (unused in Crystal)
};

struct BgEvent {
    uint8_t x, y;
    BgEventType type;
    std::string script_id;          // For signs/readable/conditional
    std::string item_id;            // For hidden items (semantic ID)
    uint8_t quantity;
    std::string condition_flag;     // For IFSET/IFNOTSET conditional scripts
    
    // ROM address for script decoding (frontend use only, not serialized to package)
    uint32_t script_rom_address = 0;
};

// NPC/Object event
struct ObjectEvent {
    uint8_t local_id;               // Object ID within map (1-indexed)
    uint8_t x, y;                   // Position
    std::string sprite_id;          // Sprite to use (semantic ID)
    uint8_t movement_type;          // Movement behavior
    uint8_t movement_radius_x;
    uint8_t movement_radius_y;
    uint8_t hour_start, hour_end;   // Active hours (0 = always, or h1<h2 for range, etc.)
    uint8_t palette;                // PAL_NPC_* palette (0 = sprite default)
    bool is_trainer;
    uint8_t trainer_sight_range;
    std::string script_id;          // Interaction script (semantic ID)
    std::string visibility_flag;    // Flag controlling visibility (semantic ID or empty)
    
    // ROM address for script decoding (frontend use only, not serialized to package)
    uint32_t script_rom_address = 0;
};

// Debug-only ROM metadata (stripped in release)
#ifndef NDEBUG
struct MapDebugInfo {
    uint8_t crystal_group;
    uint8_t crystal_index;
    uint32_t header_rom_addr;
    uint32_t blocks_rom_addr;
    uint32_t script_rom_addr;
    uint32_t events_rom_addr;
};
#endif

// Complete extracted map (fully semantic)
struct ExtractedMap {
    // Identity - stable semantic ID, not Crystal group/index
    std::string map_id;             // e.g., "new_bark_town" (lowercase, underscores)
    std::string display_name;       // e.g., "New Bark Town"
    
    // Dimensions (in metatiles, each 32x32 pixels / 4x4 tiles)
    uint8_t width;
    uint8_t height;
    
    // Tileset reference (semantic ID)
    std::string tileset_id;         // e.g., "johto_outdoor"
    
    // Block data (metatile indices, width*height bytes)
    std::vector<uint8_t> blocks;
    
    // Border block for map edges
    uint8_t border_block;
    
    // Connections to adjacent maps (raw, not yet world-resolved)
    std::vector<MapConnection> connections;
    
    // Events
    std::vector<WarpPoint> warps;
    std::vector<CoordEvent> coord_events;
    std::vector<BgEvent> bg_events;
    std::vector<ObjectEvent> objects;
    
    // Scripts - pre-decoded Lua code keyed by semantic script_id
    // Maps script_id (e.g., "bg_event_0", "object_script_1") to generated Lua
    // This is the package boundary: runtime loads Lua from here, no ROM access
    std::unordered_map<std::string, std::string> scripts;
    
    // Scripts
    std::string map_script_id;      // Main map script
    
    // Map properties
    bool is_outdoor;                // Part of continuous world?
    uint8_t environment_type;       // TOWN, ROUTE, INDOOR, CAVE, etc.
    std::string landmark_id;        // Location name for pokegear
    std::string music_id;           // Background music (semantic ID)
    bool phone_service_disabled;
    uint8_t lighting;               // Time-of-day palette mode
    std::string fish_group_id;      // Fishing encounter group
    
    // Debug-only ROM metadata
#ifndef NDEBUG
    MapDebugInfo debug;
#endif
};

//=============================================================================
// MAP GROUP INFO (for bulk extraction)
//=============================================================================

struct MapGroupInfo {
    std::string group_id;           // Semantic ID, e.g., "new_bark_area"
    std::vector<std::string> map_ids;
};

//=============================================================================
// EXTRACTION RESULT
//=============================================================================

struct MapExtractionResult {
    bool success = false;
    std::string error;
    ExtractedMap map;
};

//=============================================================================
// EXTRACTOR
//=============================================================================

class MapExtractor {
public:
    // Initialize with ROM and profile
    MapExtractor(const RomData& rom, const ExtractionProfile& profile);
    
    // Extract a single map by Crystal group/index (frontend coordinates)
    // Outputs semantic map with stable ID
    MapExtractionResult extract_map(uint8_t group, uint8_t index) const;
    
    // Extract by semantic ID (looks up group/index internally)
    MapExtractionResult extract_map(const std::string& map_id) const;
    
    // Extract all maps
    std::vector<ExtractedMap> extract_all_maps() const;
    
    // Get map groups info
    std::vector<MapGroupInfo> get_map_groups() const;
    
    // Statistics
    struct Stats {
        uint32_t maps_extracted = 0;
        uint32_t maps_failed = 0;
        uint32_t total_blocks = 0;
        uint32_t total_warps = 0;
        uint32_t total_objects = 0;
        uint32_t total_coord_events = 0;
        uint32_t total_bg_events = 0;
        uint32_t bounds_check_failures = 0;
    };
    const Stats& stats() const { return stats_; }
    
    // Test-only: make extract_map(group, index) return failure for this pair.
    // Used to prove discovery/compilation fails closed when a reachable map
    // cannot be extracted.  Harmless no-op in production (never called).
    void for_test_fail_extraction(uint8_t group, uint8_t index) {
        test_fail_pairs_.emplace((static_cast<uint16_t>(group) << 8) | index);
    }

private:
    const RomData& rom_;
    const ExtractionProfile& profile_;
    mutable Stats stats_;
    mutable std::unordered_set<uint16_t> test_fail_pairs_;  // test seam only
    
    // Bounds-checked ROM access
    bool read_map_header(uint32_t addr, std::vector<uint8_t>& out) const;
    bool read_block_data(uint8_t bank, uint16_t addr, uint8_t w, uint8_t h,
                         std::vector<uint8_t>& out) const;
    
    // Event extraction
    struct EventCounts {
        uint8_t warps = 0;
        uint8_t coord_events = 0;
        uint8_t bg_events = 0;
        uint8_t objects = 0;
    };
    bool read_events_header(uint32_t addr, EventCounts& counts, uint32_t& events_start) const;
    bool extract_warps(uint32_t ptr, uint8_t count, std::vector<WarpPoint>& out) const;
    bool extract_coord_events(uint32_t ptr, uint8_t count, std::vector<CoordEvent>& out,
                              uint8_t script_bank) const;
    bool extract_bg_events(uint32_t ptr, uint8_t count, std::vector<BgEvent>& out,
                           uint8_t script_bank) const;
    bool extract_objects(uint32_t ptr, uint8_t count, std::vector<ObjectEvent>& out,
                         uint8_t script_bank, uint8_t map_group, uint8_t map_index) const;
    bool extract_connections(uint32_t map_attr_addr, uint8_t conn_byte,
                            std::vector<MapConnection>& out) const;
    
    // Address resolution
    // MapGroup entry (MAP_LENGTH = 9 bytes) - parsed fields from ROM bytes
    struct MapGroupEntry {
        uint8_t attr_bank;          // 0: Bank for MapAttributes
        uint8_t tileset;            // 1: TILESET_* constant
        uint8_t environment;        // 2: TOWN, ROUTE, INDOOR, CAVE, etc.
        uint16_t attr_ptr;          // 3-4: Pointer to MapAttributes (little-endian)
        uint8_t location;           // 5: LANDMARK_* constant
        uint8_t music;              // 6: MUSIC_* constant
        uint8_t phone_palette;      // 7: (phone_flag << 4) | palette
        uint8_t fishgroup;          // 8: FISHGROUP_* constant
        
        bool phone_service_disabled() const { return (phone_palette >> 4) != 0; }
        uint8_t palette() const { return phone_palette & 0x0F; }
    };
    bool read_map_group_entry(uint8_t group, uint8_t index, MapGroupEntry& out) const;
    uint32_t get_map_header_address(uint8_t group, uint8_t index) const;
    
    // ID resolution - Crystal group/index to semantic IDs
    std::string make_map_id(uint8_t group, uint8_t index) const;
    std::string make_tileset_id(uint8_t tileset_index) const;
    std::string make_sprite_id(uint8_t sprite_index) const;
    std::string make_music_id(uint8_t music_index) const;
    std::string make_landmark_id(uint8_t landmark_index) const;
    std::string make_fishgroup_id(uint8_t fishgroup_index) const;
    std::string make_flag_id(uint16_t flag) const;
    std::string make_item_id(uint8_t item_index) const;
    std::string make_script_id(uint8_t group, uint8_t index, const char* suffix) const;
};

} // namespace crystal
