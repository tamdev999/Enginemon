#pragma once
// engine/package/package_reader.hpp
// Runtime package reader - reads EMON native packages directly into runtime types
//
// This is the runtime side of the compiler/runtime boundary.
// All deserialization produces native runtime types directly - no frontend types.
//
// The reader knows nothing about Crystal ROM format, only the semantic
// EMON package format.

#include "engine/package/package_format.hpp"
#include "engine/world/runtime_map.hpp"
#include "engine/world/runtime_tileset.hpp"
#include "engine/world/sprite_atlas.hpp"
#include "engine/core/types.hpp"
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace enginemon {

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
    
    // Load map data - returns runtime-native type directly
    std::vector<std::string> list_maps() const;
    std::optional<RuntimeMap> load_map(const std::string& map_id) const;
    
    // Load tileset - returns raw bytes for RuntimeTileset::from_package_data()
    std::vector<std::string> list_tilesets() const;
    std::optional<std::vector<uint8_t>> load_tileset_data(const std::string& tileset_id) const;
    
    // Legacy - kept for compatibility during transition
    std::optional<std::vector<uint8_t>> load_tileset_atlas(const std::string& tileset_id) const {
        return load_tileset_data(tileset_id);
    }
    
    // Load font atlas (raw package data)
    std::optional<std::vector<uint8_t>> load_font_atlas(const std::string& font_id) const;
    
    // Load script by ScriptId (returns Lua code string)
    std::optional<std::string> load_script(const std::string& script_id) const;
    std::vector<std::string> list_scripts() const;
    
    // Load sprite - returns runtime-native type directly
    std::optional<RuntimeSprite> load_sprite(const std::string& sprite_id) const;
    std::vector<std::string> list_sprites() const;
    
    // Load OBJ palettes (shared across all sprites)
    std::optional<SpriteObjPalettes> load_obj_palettes() const;

    // Load species→icon mapping.
    // Returns a map from SpeciesId (1-251) to "pokemon_icon:<icon_type_name>" string.
    // Empty map if chunk absent (old packages without the section).
    std::unordered_map<SpeciesId, std::string> load_species_icon_map() const;

private:
    PackageReader() = default;
    
    PackageHeader header_;
    std::vector<TocEntry> toc_;
    std::filesystem::path path_;
    size_t file_size_ = 0;  // For bounds validation
    
    // Index maps for fast lookup
    std::unordered_map<std::string, size_t> map_index_;
    std::unordered_map<std::string, size_t> tileset_index_;
    std::unordered_map<std::string, size_t> font_index_;
    std::unordered_map<std::string, size_t> script_index_;
    std::unordered_map<std::string, size_t> sprite_index_;
    
    // Internal helpers for reading indexed chunks
    std::optional<std::vector<uint8_t>> read_indexed_chunk(
        ChunkType type,
        const std::string& id,
        const std::unordered_map<std::string, size_t>& index) const;
};

} // namespace enginemon
