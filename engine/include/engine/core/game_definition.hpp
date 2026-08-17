#pragma once
// engine/core/game_definition.hpp
// Complete game content package
// Produced by frontends (Crystal compiler), consumed by engine
// No ROM knowledge - purely semantic data

#include "engine/core/types.hpp"
#include "engine/core/registry.hpp"
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace enginemon {

// Forward declarations
struct WorldData;
struct AssetManifest;

// Metadata about the game definition
struct GameMetadata {
    std::string name;           // e.g. "Pokemon Crystal"
    std::string version;        // e.g. "1.0" or "USA Rev 1"
    std::string source_hash;    // Hash of source ROM for identification
    uint64_t compile_timestamp;
};

// Map data (single map, not world coordinates yet)
struct MapData {
    MapId id;
    std::string name;           // e.g. "NewBarkTown"
    std::string display_name;   // e.g. "NEW BARK TOWN"
    
    uint8_t width;              // In tiles
    uint8_t height;
    TilesetId tileset;
    
    // Tile indices into tileset
    std::vector<uint8_t> tiles;
    
    // Border block for out-of-bounds
    uint8_t border_block;
    
    // Music
    MusicId music;
    
    // Environment properties
    bool is_indoor;
    bool is_cave;
    bool can_bike;
    bool can_dig;
    bool can_fly_to;
    uint8_t lighting;           // 0=normal, affects flash requirement
    
    // Location in continuous world coordinates (set by connection resolver)
    int32_t world_x = 0;
    int32_t world_y = 0;
    bool in_continuous_world = false;
};

// Connection between maps
struct MapConnection {
    MapId from_map;
    MapId to_map;
    Direction direction;
    int16_t offset;             // Tile offset along connection edge
};

// Warp point (doors, stairs, etc.)
struct WarpData {
    MapId map;
    uint8_t x;
    uint8_t y;
    MapId destination_map;
    uint8_t destination_warp_id;
};

// NPC/object on map
struct NpcData {
    MapId map;
    uint8_t x;
    uint8_t y;
    SpriteId sprite;
    Direction facing;
    uint8_t movement_type;      // Standing, walking, etc.
    uint8_t movement_radius;
    
    ScriptId interact_script;
    FlagId visibility_flag;     // Flag that controls visibility (0 = always visible)
    bool flag_inverted;         // If true, visible when flag is NOT set
};

// Sign/hidden item
struct SignData {
    MapId map;
    uint8_t x;
    uint8_t y;
    ScriptId script;
};

// Wild encounter slot
struct EncounterSlot {
    SpeciesId species;
    uint8_t min_level;
    uint8_t max_level;
};

// Wild encounters for a map
struct EncounterData {
    MapId map;
    uint8_t encounter_rate;
    
    std::vector<EncounterSlot> grass_morning;
    std::vector<EncounterSlot> grass_day;
    std::vector<EncounterSlot> grass_night;
    std::vector<EncounterSlot> water;
    // Rock smash, fishing, etc. as needed
};

// Complete world data
struct WorldData {
    std::vector<MapData> maps;
    std::vector<MapConnection> connections;
    std::vector<WarpData> warps;
    std::vector<NpcData> npcs;
    std::vector<SignData> signs;
    std::vector<EncounterData> encounters;
    
    // Lookup helpers
    const MapData* get_map(MapId id) const;
    std::vector<const MapConnection*> get_connections_from(MapId id) const;
    std::vector<const WarpData*> get_warps_on(MapId id) const;
    std::vector<const NpcData*> get_npcs_on(MapId id) const;
};

// Asset references (sprites, tiles, audio)
// Actual asset data loaded separately via AssetManager
struct AssetManifest {
    struct SpriteEntry {
        SpriteId id;
        std::string filename;
        uint8_t width;
        uint8_t height;
        uint8_t frame_count;
    };
    
    struct TilesetEntry {
        TilesetId id;
        std::string tiles_filename;
        std::string collision_filename;
        std::string palettes_filename;
    };
    
    struct MusicEntry {
        MusicId id;
        std::string filename;
    };
    
    struct SfxEntry {
        SfxId id;
        std::string filename;
    };
    
    std::vector<SpriteEntry> sprites;
    std::vector<TilesetEntry> tilesets;
    std::vector<MusicEntry> music;
    std::vector<SfxEntry> sfx;
};

// Script manifest (references to generated Lua files)
struct ScriptManifest {
    // Base game scripts (generated from ROM)
    std::filesystem::path scripts_directory;
    
    // Map script files
    std::vector<std::pair<MapId, std::string>> map_scripts;
    
    // Event scripts by ID
    std::vector<std::pair<ScriptId, std::string>> event_scripts;
};

// The complete game definition
// This is what the Crystal frontend produces and the engine consumes
class GameDefinition {
public:
    GameMetadata metadata;
    
    // Registries (populated during load, frozen before gameplay)
    Registries registries;
    
    // World data
    WorldData world;
    
    // Asset manifest
    AssetManifest assets;
    
    // Script manifest
    ScriptManifest scripts;
    
    // Load from compiled game directory
    static std::unique_ptr<GameDefinition> load(const std::filesystem::path& path);
    
    // Save to compiled game directory
    void save(const std::filesystem::path& path) const;
    
    // Validate integrity
    bool validate() const;
};

} // namespace enginemon
