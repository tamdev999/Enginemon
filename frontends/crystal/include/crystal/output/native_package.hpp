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

//=============================================================================
// PACKAGE HEADER
//=============================================================================

struct PackageHeader {
    static constexpr uint32_t MAGIC = 0x454D4F4E;  // "EMON"
    static constexpr uint32_t VERSION = 3;  // v3: connection fields src_skip_blocks/strip_length_blocks/coord_adjust_tiles replace strip_offset/strip_length
    
    uint32_t magic;
    uint32_t version;
    uint32_t flags;
    
    // Source ROM info (for verification, not extraction)
    char source_sha1[41];       // Null-terminated hex string
    char source_version[32];    // e.g., "Crystal USA v1.1"
    
    // Table of contents offsets
    uint32_t toc_offset;
    uint32_t toc_size;
    
    // Checksums for integrity
    uint32_t data_crc32;
};

//=============================================================================
// TABLE OF CONTENTS
//=============================================================================

enum class ChunkType : uint32_t {
    Maps = 0x4D415053,          // "MAPS"
    TilesetAtlases = 0x54494C53, // "TILS"
    Sprites = 0x53505254,        // "SPRT"
    ObjPalettes = 0x4F424A50,    // "OBJP"
    Scripts = 0x53435250,        // "SCRP"
    Audio = 0x41554449,          // "AUDI"
    Strings = 0x53545247,        // "STRG"
    Fonts = 0x464F4E54,          // "FONT"
    SpeciesIconMap = 0x53494D50, // "SIMP" — SpeciesId → pokemon_icon asset ID mapping
                                 // Compiled from Crystal MonMenuIcons (bank 23) by the
                                 // Crystal frontend. Runtime uses this for Day Care sprite
                                 // resolution without any hardcoded Crystal tables.
};

struct TocEntry {
    ChunkType type;
    uint32_t offset;
    uint32_t size;
    uint32_t count;             // Number of items in chunk
    uint32_t crc32;             // Chunk checksum
};

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

//=============================================================================
// UTILITY
//=============================================================================

// Calculate CRC32 for data integrity
uint32_t calculate_crc32(const void* data, size_t size);

} // namespace crystal
