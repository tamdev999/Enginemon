// engine/world/runtime_tileset.cpp
// Native runtime tileset implementation

#include "engine/world/runtime_tileset.hpp"
#include <cstring>
#include <iostream>

namespace enginemon {

//=============================================================================
// PACKAGE DESERIALIZATION
//=============================================================================

// Helper to read little-endian values
template<typename T>
static T read_le(const uint8_t*& ptr) {
    T value = 0;
    for (size_t i = 0; i < sizeof(T); ++i) {
        value |= static_cast<T>(*ptr++) << (i * 8);
    }
    return value;
}

std::optional<RuntimeTileset> RuntimeTileset::from_package_data(
    const std::string& tileset_id,
    const std::vector<uint8_t>& data
) {
    RuntimeTileset tileset;
    tileset.tileset_id = tileset_id;
    
    if (data.size() < 8) {
        std::cerr << "[TILESET] " << tileset_id << ": data too small (" << data.size() << " bytes)\n";
        return std::nullopt;
    }
    
    const uint8_t* ptr = data.data();
    const uint8_t* end = ptr + data.size();
    
    // Read tile count
    uint32_t tile_count = read_le<uint32_t>(ptr);
    
    // Read tiles (each tile is 64 indexed pixels = 64 bytes)
    tileset.tiles.resize(tile_count);
    for (uint32_t i = 0; i < tile_count; ++i) {
        if (ptr + 64 > end) {
            std::cerr << "[TILESET] " << tileset_id << ": truncated at tile " << i << "\n";
            return std::nullopt;
        }
        std::memcpy(tileset.tiles[i].indices.data(), ptr, 64);
        ptr += 64;
    }
    
    // Read block count
    if (ptr + 4 > end) {
        std::cerr << "[TILESET] " << tileset_id << ": truncated at block count\n";
        return std::nullopt;
    }
    uint32_t block_count = read_le<uint32_t>(ptr);
    
    // Read blocks (each block is 16 tile IDs = 32 bytes)
    tileset.blocks.resize(block_count);
    for (uint32_t i = 0; i < block_count; ++i) {
        if (ptr + 32 > end) {
            std::cerr << "[TILESET] " << tileset_id << ": truncated at block " << i << "\n";
            return std::nullopt;
        }
        for (int t = 0; t < 16; ++t) {
            tileset.blocks[i].tile_ids[t] = read_le<uint16_t>(ptr);
        }
    }
    
    // Read collision count
    if (ptr + 4 > end) {
        std::cerr << "[TILESET] " << tileset_id << ": truncated at collision count\n";
        return std::nullopt;
    }
    uint32_t coll_count = read_le<uint32_t>(ptr);
    
    // Read collision data (semantic CollisionClass values, 1 byte each)
    if (ptr + coll_count > end) {
        std::cerr << "[TILESET] " << tileset_id << ": truncated at collision data\n";
        return std::nullopt;
    }
    tileset.collision.resize(coll_count);
    for (uint32_t i = 0; i < coll_count; ++i) {
        tileset.collision[i] = static_cast<CollisionClass>(*ptr++);
    }
    
    // Read palette map
    if (ptr + 4 > end) {
        std::cerr << "[TILESET] " << tileset_id << ": truncated at palette map count\n";
        return std::nullopt;
    }
    uint32_t palmap_count = read_le<uint32_t>(ptr);
    
    if (ptr + palmap_count > end) {
        std::cerr << "[TILESET] " << tileset_id << ": truncated at palette map data\n";
        return std::nullopt;
    }
    tileset.palette_map.resize(palmap_count);
    std::memcpy(tileset.palette_map.data(), ptr, palmap_count);
    ptr += palmap_count;
    
    // Helper to read a palette set (7 palettes × 4 colors × RGBA32)
    auto read_palette_set = [&ptr, end](RuntimePaletteSet& palette_set) -> bool {
        constexpr size_t SET_SIZE = 7 * 4 * 4;  // 7 palettes × 4 colors × 4 bytes
        if (ptr + SET_SIZE > end) return false;
        
        for (int pal_id = 0; pal_id < 7; ++pal_id) {
            for (int c = 0; c < 4; ++c) {
                palette_set.palettes[pal_id].colors[c] = read_le<uint32_t>(ptr);
            }
        }
        return true;
    };
    
    // Read 5 standard palette rows
    for (int row = 0; row < 5; ++row) {
        if (!read_palette_set(tileset.standard_palette_rows[row])) {
            std::cerr << "[TILESET] " << tileset_id << ": truncated at palette row " << row << "\n";
            return std::nullopt;
        }
    }
    
    // Read fixed special palette (if present)
    if (ptr + 1 > end) {
        std::cerr << "[TILESET] " << tileset_id << ": truncated at has_fixed flag\n";
        return std::nullopt;
    }
    uint8_t has_fixed = *ptr++;
    
    if (has_fixed) {
        RuntimePaletteSet special_set;
        if (!read_palette_set(special_set)) {
            std::cerr << "[TILESET] " << tileset_id << ": truncated at special palette\n";
            return std::nullopt;
        }
        tileset.fixed_special_palette = special_set;
    }
    
    return tileset;
}

//=============================================================================
// TILE ATLAS GENERATION
//=============================================================================

TileAtlas TileAtlas::from_tileset(const RuntimeTileset& tileset) {
    TileAtlas atlas;
    
    if (tileset.tiles.empty()) {
        return atlas;
    }
    
    // Layout: 16 tiles per row, each tile is 8×8 pixels
    // This gives us 128 pixels wide
    // For 192 tiles: 192/16 = 12 rows = 96 pixels tall
    // For 256 tiles: 256/16 = 16 rows = 128 pixels tall
    const uint32_t tiles_per_row = 16;
    const uint32_t tile_size = 8;
    
    uint32_t tile_count = static_cast<uint32_t>(tileset.tiles.size());
    uint32_t rows = (tile_count + tiles_per_row - 1) / tiles_per_row;
    
    atlas.width = tiles_per_row * tile_size;
    atlas.height = rows * tile_size;
    
    // Store as indexed pixels (R8 format - 1 byte per pixel)
    atlas.indices.resize(atlas.width * atlas.height, 0);
    atlas.tile_uvs.reserve(tile_count);
    
    // Copy tiles into atlas and compute UVs
    for (uint32_t i = 0; i < tile_count; ++i) {
        const RuntimeTile& tile = tileset.tiles[i];
        
        // Position in atlas
        uint32_t tx = (i % tiles_per_row) * tile_size;
        uint32_t ty = (i / tiles_per_row) * tile_size;
        
        // Copy 8×8 indexed pixels
        for (uint32_t py = 0; py < tile_size; ++py) {
            for (uint32_t px = 0; px < tile_size; ++px) {
                uint32_t src_idx = py * tile_size + px;
                uint32_t dst_idx = (ty + py) * atlas.width + (tx + px);
                atlas.indices[dst_idx] = tile.indices[src_idx];
            }
        }
        
        // Compute UV coordinates
        TileUV uv;
        uv.u0 = static_cast<float>(tx) / atlas.width;
        uv.v0 = static_cast<float>(ty) / atlas.height;
        uv.u1 = static_cast<float>(tx + tile_size) / atlas.width;
        uv.v1 = static_cast<float>(ty + tile_size) / atlas.height;
        atlas.tile_uvs.push_back(uv);
    }
    
    return atlas;
}

TileAtlas TileAtlas::from_tileset_with_palette(
    const RuntimeTileset& tileset, 
    const RuntimePaletteSet& palette_set)
{
    TileAtlas atlas;
    
    if (tileset.tiles.empty()) {
        return atlas;
    }
    
    // Layout: 16 tiles per row, each tile is 8×8 pixels
    const uint32_t tiles_per_row = 16;
    const uint32_t tile_size = 8;
    
    uint32_t tile_count = static_cast<uint32_t>(tileset.tiles.size());
    uint32_t rows = (tile_count + tiles_per_row - 1) / tiles_per_row;
    
    atlas.width = tiles_per_row * tile_size;
    atlas.height = rows * tile_size;
    atlas.pixels.resize(atlas.width * atlas.height, 0xFF000000);  // Black/opaque
    atlas.tile_uvs.reserve(tile_count);
    
    // Copy tiles into atlas with palette resolution and compute UVs
    for (uint32_t i = 0; i < tile_count; ++i) {
        const RuntimeTile& tile = tileset.tiles[i];
        
        // Get palette for this tile
        uint8_t palette_id = tileset.get_tile_palette(static_cast<uint16_t>(i));
        if (palette_id > 6) palette_id = 0;
        const RuntimePalette& pal = palette_set[palette_id];
        
        // Position in atlas
        uint32_t tx = (i % tiles_per_row) * tile_size;
        uint32_t ty = (i / tiles_per_row) * tile_size;
        
        // Copy 8×8 pixels with palette resolution
        for (uint32_t py = 0; py < tile_size; ++py) {
            for (uint32_t px = 0; px < tile_size; ++px) {
                uint32_t src_idx = py * tile_size + px;
                uint32_t dst_idx = (ty + py) * atlas.width + (tx + px);
                uint8_t color_idx = tile.indices[src_idx];
                atlas.pixels[dst_idx] = pal.colors[color_idx];
            }
        }
        
        // Compute UV coordinates
        TileUV uv;
        uv.u0 = static_cast<float>(tx) / atlas.width;
        uv.v0 = static_cast<float>(ty) / atlas.height;
        uv.u1 = static_cast<float>(tx + tile_size) / atlas.width;
        uv.v1 = static_cast<float>(ty + tile_size) / atlas.height;
        atlas.tile_uvs.push_back(uv);
    }
    
    return atlas;
}

} // namespace enginemon
