#pragma once
// engine/world/runtime_tileset.hpp
// Native runtime tileset types preserving 8×8 tile semantics
//
// This is the canonical runtime representation. The package stores this
// semantic data, and renderers expand it as needed.
//
// Key design: Tiles and Blocks are separate, enabling:
// - Future 3D: tile semantics → materials → meshes → BLAS/TLAS
// - Animation: tile-level animation without block rebaking
// - Modding: swap/patch individual tiles
// - RT: per-tile material properties
//
// The 2D renderer batches tile instances efficiently on map load.
//
// Palette Resolution:
// - Tiles store indexed color values (0-3)
// - Tilesets store palette_map (tile_id → palette_id 0-6)
// - Tilesets store standard palette rows (Morn, Day, Nite, Dark, Indoor)
// - Maps store environment + time_policy for palette selection
// - Renderer resolves: indexed tile + palette_id + active palette set → final color

#include "engine/world/collision_types.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace enginemon {

//=============================================================================
// RUNTIME PALETTE (4-color palette)
//=============================================================================

// A single 4-color palette
struct RuntimePalette {
    // 4 colors as RGBA32 values
    std::array<uint32_t, 4> colors;
};

// A complete palette set (7 BG palettes, indexed 0-6)
// Crystal BG palette indices:
//   0 = GRAY, 1 = RED, 2 = GREEN, 3 = WATER, 4 = YELLOW, 5 = BROWN, 6 = ROOF
struct RuntimePaletteSet {
    std::array<RuntimePalette, 7> palettes;
    
    // Access palette by ID (0-6), returns colors array
    const RuntimePalette& operator[](size_t idx) const { return palettes[idx]; }
    RuntimePalette& operator[](size_t idx) { return palettes[idx]; }
};

// Standard palette row indices (from Crystal's environment_colors.asm)
// These index into RuntimeTileset::standard_palette_rows
enum class PaletteRow : uint8_t {
    Morn   = 0,  // Morning outdoor
    Day    = 1,  // Day outdoor  
    Nite   = 2,  // Night outdoor/indoor
    Dark   = 3,  // Dark (cave, flash needed)
    Indoor = 4,  // Indoor (morn/day indoors use this)
    Count  = 5
};

//=============================================================================
// RUNTIME TILE (8×8 indexed pixels)
//=============================================================================

struct RuntimeTile {
    // 8×8 = 64 pixels, indexed color values 0-3 (2bpp)
    std::array<uint8_t, 64> indices;
    
    // Future: material/semantic metadata
    // uint8_t material_id;
    // bool is_animated;
    // etc.
};

//=============================================================================
// RUNTIME BLOCK (4×4 tiles = 32×32 pixels)
// Also called "metatile" in Crystal terminology
//=============================================================================

struct RuntimeBlock {
    // 16 tile indices in row-major 4×4 order
    // Layout: [row0: 0,1,2,3] [row1: 4,5,6,7] [row2: 8,9,10,11] [row3: 12,13,14,15]
    std::array<uint16_t, 16> tile_ids;
};

//=============================================================================
// RUNTIME TILESET
// Complete tileset with tiles, blocks, and collision
//=============================================================================

struct RuntimeTileset {
    std::string tileset_id;
    
    // All 8×8 tiles (typically 192-256 tiles per tileset)
    // Each tile stores indexed pixels (0-3), not RGBA
    std::vector<RuntimeTile> tiles;
    
    // All blocks/metatiles (typically 64-128 blocks per tileset)
    std::vector<RuntimeBlock> blocks;
    
    // Collision data: 4 entries per block (TL, TR, BL, BR quadrants)
    // Index formula: collision[block_index * 4 + quadrant]
    // where quadrant = (cell_x % 2) + (cell_y % 2) * 2
    // 
    // SEMANTIC: This stores CollisionClass values, NOT raw Crystal bytes.
    // The Crystal frontend classifies raw bytes at packaging time.
    std::vector<CollisionClass> collision;
    
    // Palette map: tile_id → palette_id (0-6)
    // Used at render time to look up which palette each tile uses
    std::vector<uint8_t> palette_map;
    
    // Standard palette rows extracted from Crystal ROM
    // 5 rows: Morn, Day, Nite, Dark, Indoor
    // Selected at runtime based on map.environment + time_policy + RTC
    std::array<RuntimePaletteSet, static_cast<size_t>(PaletteRow::Count)> standard_palette_rows;
    
    // Optional fixed special palette (e.g., TILESET_HOUSE, TILESET_ICE_PATH)
    // If present, this overrides environment/time-based palette selection
    std::optional<RuntimePaletteSet> fixed_special_palette;
    
    // Parse from package data.
    // Returns nullopt on any structural failure (truncated, malformed counts, etc.).
    // A partial RuntimeTileset is never returned as success.
    static std::optional<RuntimeTileset> from_package_data(const std::string& tileset_id,
                                                             const std::vector<uint8_t>& data);
    
    // Helpers
    size_t tile_count() const { return tiles.size(); }
    size_t block_count() const { return blocks.size(); }
    
    // Get the palette_id for a given tile (returns 0 if tile_id out of range)
    uint8_t get_tile_palette(uint16_t tile_id) const {
        if (tile_id < palette_map.size()) {
            return palette_map[tile_id];
        }
        return 0;
    }
};

//=============================================================================
// TILE ATLAS (GPU upload format)
// Generated from RuntimeTileset for efficient rendering
//=============================================================================

struct TileUV {
    float u0, v0;  // Top-left
    float u1, v1;  // Bottom-right
};

struct TileAtlas {
    uint32_t width = 0;   // Atlas width in pixels
    uint32_t height = 0;  // Atlas height in pixels
    
    // For indexed rendering: store indices as R8 texture
    // Each pixel is a 2-bit index (0-3) stored in an 8-bit value
    std::vector<uint8_t> indices;  // Indexed pixels (1 byte per pixel)
    
    // Legacy RGBA pixels for compatibility (can be removed later)
    std::vector<uint32_t> pixels;  // RGBA32, row-major
    
    std::vector<TileUV> tile_uvs;  // UV for each tile
    
    // Build atlas from tileset tiles (indexed mode - stores indices)
    static TileAtlas from_tileset(const RuntimeTileset& tileset);
    
    // Build atlas with pre-resolved colors (legacy mode - for testing)
    // This bakes palette resolution into RGBA - NOT for final use
    static TileAtlas from_tileset_with_palette(
        const RuntimeTileset& tileset, 
        const RuntimePaletteSet& palette_set);
};

} // namespace enginemon
