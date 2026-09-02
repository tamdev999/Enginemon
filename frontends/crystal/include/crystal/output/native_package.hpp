#pragma once
// crystal/output/native_package.hpp
// Native package format for extracted Crystal data
// 
// The package is the boundary between ROM extraction and runtime.
// After writing a package, the ROM can be closed.
// The engine loads only from packages, never from ROMs.
//
// Package format:
// - Header (version, checksums, table of contents)
// - Map data (semantic structures, no ROM addresses)
// - Tileset atlases (pre-rendered RGBA)
// - Collision data
// - Sprite atlases
// - Audio data (converted from GB format)
// - Script bytecode (compiled Lua)

#include "crystal/extract/tileset_extractor.hpp"
#include "engine/battle/battle_rules.hpp"
#include "engine/package/package_format.hpp"   // canonical PackageHeader/ChunkType/TocEntry
#include "engine/world/runtime_map.hpp"
#include "engine/world/sprite_atlas.hpp"
#include "engine/core/types.hpp"
#include <cstdint>
#include <string>
#include <vector>
#include <filesystem>
#include <unordered_map>
#include <unordered_set>

namespace crystal {

// Forward declarations
struct ExtractedMap;
struct ExtractedTileset;
struct TilesetAtlas;  // Defined in tileset_extractor.hpp
struct FontAtlas;     // Defined in font_extractor.hpp

// Use RuntimeSprite and SpriteObjPalettes from engine
using enginemon::RuntimeSprite;
using enginemon::SpriteObjPalettes;
using enginemon::SpeciesId;

// Package format types — single source of truth in engine/package/package_format.hpp.
// These aliases make the types available unqualified inside namespace crystal so that
// PackageWriter/PackageReader implementations can use them without qualification.
// No separate crystal:: definitions — drift is impossible.
using enginemon::PackageHeader;
using enginemon::ChunkType;
using enginemon::TocEntry;
// Note: calculate_crc32 is defined separately in native_package.cpp (identical algorithm).
// It remains a crystal:: local until a future consolidation pass removes the duplication.

//=============================================================================
// SERIALIZED MAP DATA
//=============================================================================

// Compact serialized map (no pointers, fixed-size where possible)
struct SerializedMap {
    char map_id[64];
    char display_name[64];
    char tileset_id[32];
    char music_id[32];
    
    uint8_t width;
    uint8_t height;
    uint8_t border_block;
    uint8_t environment_type;
    uint8_t flags;              // is_outdoor, phone_disabled, etc.
    uint8_t lighting;
    uint16_t _padding;
    
    // Variable-length data follows:
    // - blocks (width * height bytes)
    // - warps (count + data)
    // - coord_events (count + data)
    // - bg_events (count + data)
    // - objects (count + data)
    // - connections (count + data)
};

//=============================================================================
// PACKAGE WRITER
//=============================================================================

class PackageWriter {
public:
    PackageWriter();
    
    // Add extracted data
    void add_map(const ExtractedMap& map);
    void add_tileset_atlas(const TilesetAtlas& atlas);  // Legacy baked 32×32 metatiles
    void add_tileset(const ExtractedTileset& tileset, TimeOfDay tod);  // Native 8×8 tiles + blocks
    void add_font_atlas(const FontAtlas& atlas);
    
    // Add compiled scripts (ScriptId → Lua code)
    // ScriptId must be globally unique (e.g., "new_bark_town::bg_event_0")
    void add_script(const std::string& script_id, const std::string& lua_code);
    
    // Add sprite data (RuntimeSprite) - sprite_id becomes package key
    void add_sprite(const RuntimeSprite& sprite);
    
    // Add OBJ palettes (shared across all sprites)
    void add_obj_palettes(const SpriteObjPalettes& palettes);

    // Add species→icon mapping (SpeciesId → pokemon_icon asset ID string).
    // Compiled from Crystal MonMenuIcons table by the Crystal frontend.
    // Maps all valid species (1-251) to their "pokemon_icon:<icon_type_name>" package key.
    // Duplicate species entries → throws.
    // Empty icon_id → throws.
    using SpeciesIconEntry = std::pair<enginemon::SpeciesId, std::string>;
    void add_species_icon_map(const std::vector<SpeciesIconEntry>& entries);

    // Add species base stats registry.
    // One entry per species (SpeciesId → SpeciesDefinition).
    // Duplicate SpeciesId → throws.  Empty entries → throws.
    // Serialised as a flat array keyed by SpeciesId; runtime populates
    // Registries::species from this chunk via PackageReader::load_base_stats_registry().
    struct SpeciesBaseStatsEntry {
        enginemon::SpeciesId id;
        uint8_t hp, attack, defense, speed, sp_atk, sp_def;
        uint8_t type1, type2;
        uint8_t catch_rate, base_exp, gender_ratio;
    };
    void add_base_stats(const std::vector<SpeciesBaseStatsEntry>& entries);

    // Add move data registry.
    // One entry per move (MoveId → MoveData fields).
    // Duplicate MoveId → throws.
    // Serialised as a flat array keyed by MoveId; runtime populates
    // Registries::moves from this chunk via PackageReader::load_move_registry().
    struct MoveDataEntry {
        enginemon::MoveId id;
        uint8_t type_id;
        uint8_t power;
        uint8_t accuracy;
        uint8_t pp;
        uint8_t effect_id;
        uint8_t effect_chance;
    };
    void add_move_data(const std::vector<MoveDataEntry>& entries);

    // Add battle rules (BRLS chunk).
    // Serialises all ROM-derived battle tables extracted by BattleRulesExtractor.
    // Called once. Called more than once → throws.
    // rules.is_valid() must be true; invalid rules → throws.
    void add_battle_rules(const enginemon::BattleRules& rules);
    
    // Set metadata
    void set_source_rom(const std::string& sha1, const std::string& version);
    
    // Write package to file
    bool write(const std::filesystem::path& path) const;
    
    // Get statistics
    struct Stats {
        uint32_t maps_written = 0;
        uint32_t tilesets_written = 0;
        uint32_t scripts_written = 0;
        uint64_t total_bytes = 0;
    };
    const Stats& stats() const { return stats_; }

private:
    PackageHeader header_;
    std::vector<TocEntry> toc_;
    
    // Collected data
    std::vector<SerializedMap> maps_;
    std::vector<std::pair<std::string, std::vector<uint8_t>>> map_data_;
    std::vector<std::pair<std::string, std::vector<uint8_t>>> tileset_data_;
    std::vector<std::pair<std::string, std::vector<uint8_t>>> font_data_;
    std::vector<std::pair<std::string, std::string>> script_data_;  // ScriptId → Lua code
    std::vector<std::pair<std::string, std::vector<uint8_t>>> sprite_data_;  // sprite_id → serialized sprite
    std::vector<uint8_t> obj_palettes_data_;  // Serialized OBJ palettes (single chunk)
    std::vector<uint8_t> species_icon_map_data_;  // Serialized species→icon map (single chunk)
    std::vector<uint8_t> base_stats_data_;        // Serialized species base stats (single chunk)
    std::vector<uint8_t> move_data_data_;         // Serialized move data (single chunk)
    std::vector<uint8_t> battle_rules_data_;      // Serialized battle rules (single chunk)
    
    Stats stats_;
    
    void write_chunk(std::ostream& out, ChunkType type, 
                     const void* data, size_t size) const;
};

//=============================================================================
// PACKAGE READER
//=============================================================================

class PackageReader {
public:
    // Load package from file
    static std::unique_ptr<PackageReader> open(const std::filesystem::path& path);
    
    // Validate integrity
    bool validate() const;
    
    // Get metadata
    const PackageHeader& header() const { return header_; }
    std::string source_sha1() const { return header_.source_sha1; }
    std::string source_version() const { return header_.source_version; }
    
    // Load specific data
    std::vector<std::string> list_maps() const;
    std::optional<SerializedMap> load_map(const std::string& map_id) const;
    
    // Load full map with all events (for runtime use)
    // Returns runtime-native type, not frontend type
    std::optional<enginemon::RuntimeMap> load_full_map(const std::string& map_id) const;
    
    std::vector<std::string> list_tilesets() const;
    std::optional<std::vector<uint8_t>> load_tileset_atlas(const std::string& tileset_id) const;
    
    // Load font atlas
    std::optional<std::vector<uint8_t>> load_font_atlas(const std::string& font_id) const;
    
    // Load script by ScriptId (returns Lua code string)
    std::optional<std::string> load_script(const std::string& script_id) const;
    
    // List all available scripts
    std::vector<std::string> list_scripts() const;
    
    // Load sprite by sprite_id (returns semantic RuntimeSprite)
    std::optional<enginemon::RuntimeSprite> load_sprite(const std::string& sprite_id) const;
    
    // List all available sprites
    std::vector<std::string> list_sprites() const;
    
    // Load OBJ palettes (shared across all sprites)
    std::optional<enginemon::SpriteObjPalettes> load_obj_palettes() const;

private:
    PackageReader() = default;
    
    PackageHeader header_;
    std::vector<TocEntry> toc_;
    std::filesystem::path path_;
    
    // Index maps for fast lookup
    std::unordered_map<std::string, size_t> map_index_;
    std::unordered_map<std::string, size_t> tileset_index_;
    std::unordered_map<std::string, size_t> font_index_;
    std::unordered_map<std::string, size_t> script_index_;
    std::unordered_map<std::string, size_t> sprite_index_;
};

// calculate_crc32 — local implementation in native_package.cpp.
// Uses identical algorithm to enginemon::calculate_crc32 in package_reader.cpp.
uint32_t calculate_crc32(const void* data, size_t size);

} // namespace crystal
