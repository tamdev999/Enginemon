#pragma once
// crystal/extract/tileset_extractor.hpp
// Tileset and block/metatile extraction from Crystal ROM
// 
// Extracts:
// - Tile graphics (8x8 pixel tiles, 2bpp)
// - Metatile definitions (4x4 tile blocks with collision)
// - Palettes (CGB color palettes)
// - Collision data
//
// Output is semantic - no ROM addresses in release builds.

#include "crystal/rom/loader.hpp"
#include "crystal/rom/profile.hpp"
#include <cstdint>
#include <string>
#include <vector>
#include <array>

namespace crystal {

//=============================================================================
// SEMANTIC OUTPUT STRUCTURES
//=============================================================================

// Time of day for palette selection
enum class TimeOfDay : uint8_t {
    Morning = 0,
    Day = 1,
    Night = 2,
    Dark = 3,      // Flash/cave darkness
    Indoor = 4     // Indoor maps
};

// A single 8x8 tile (64 pixels, 2bpp = 16 bytes)
struct Tile {
    std::array<uint8_t, 64> pixels;  // 0-3 per pixel (2bpp palette index)
};

// CGB color (15-bit RGB)
struct Color {
    uint8_t r, g, b;  // 0-31 each
    
    static Color from_gbc(uint16_t gbc) {
        return {
            static_cast<uint8_t>(gbc & 0x1F),
            static_cast<uint8_t>((gbc >> 5) & 0x1F),
            static_cast<uint8_t>((gbc >> 10) & 0x1F)
        };
    }
    
    uint32_t to_rgba32() const {
        // Scale 5-bit to 8-bit
        // Memory layout for VK_FORMAT_R8G8B8A8_UNORM on little-endian:
        // byte 0 = R, byte 1 = G, byte 2 = B, byte 3 = A
        return static_cast<uint32_t>(r * 255 / 31) |
               (static_cast<uint32_t>(g * 255 / 31) << 8) |
               (static_cast<uint32_t>(b * 255 / 31) << 16) |
               (0xFFu << 24);
    }
};

// 4-color palette
struct Palette {
    std::array<Color, 4> colors;
};

// Collision type for metatile
enum class CollisionType : uint8_t {
    Walkable = 0x00,
    Solid = 0x01,
    Water = 0x02,
    WaterfallTop = 0x03,
    Grass = 0x04,
    TallGrass = 0x05,
    Ice = 0x06,
    Whirlpool = 0x07,
    Ledge = 0x08,
    // ... more types
    Counter = 0x60,
    Door = 0x70,
    Warp = 0x80,
};

// Metatile attributes
struct MetatileAttrs {
    CollisionType collision;
    uint8_t palette_index;    // Which palette to use
    bool flip_x;
    bool flip_y;
    bool priority;            // Above or below sprites
};

// A metatile (4x4 tiles = 32x32 pixels)
// Crystal metatiles are 4×4 8x8 tiles (32×32 pixels), NOT 2×2.
// See pokecrystal/constants/gfx_constants.asm: DEF METATILE_WIDTH EQU 4
// and Gen2Recomped/src/world/Map.lua: block[(ty % 4) * 4 + (tx % 4) + 1]
struct Metatile {
    // 16 tile indices in row-major order (4×4 grid)
    // Layout: [row0: 0,1,2,3] [row1: 4,5,6,7] [row2: 8,9,10,11] [row3: 12,13,14,15]
    std::array<uint8_t, 16> tile_indices;
    // Attributes for each tile (if needed - Crystal metatiles are just tile indices)
    std::array<MetatileAttrs, 16> attrs;
    // Overall collision for this metatile
    CollisionType collision;
};

// Animation frame for animated tiles
struct TileAnimation {
    std::string anim_id;
    uint8_t frame_count;
    uint8_t frame_delay;       // Frames between changes
    std::vector<uint8_t> tile_indices;  // Tile index for each frame
};

// Complete tileset (semantic output)
struct ExtractedTileset {
    std::string tileset_id;
    
    // Tile graphics (up to 256 tiles, though usually less)
    std::vector<Tile> tiles;
    
    // Metatiles (block definitions)
    std::vector<Metatile> metatiles;
    
    // Collision data: 4 bytes per metatile (TL, TR, BL, BR quadrants)
    // Index formula: collision[metatile_index * 4 + quadrant]
    // where quadrant = (cell_x % 2) + (cell_y % 2) * 2
    // See pokecrystal/data/tilesets/*_collision.asm for format
    std::vector<uint8_t> collision;
    
    // Palette map: maps tile index to palette ID (0-6)
    // Crystal uses two VRAM banks:
    //   - Bank 0: tiles 0-95
    //   - Bank 1: tiles 128-223
    // Tiles 96-127 and 224-255 don't exist in tileset graphics.
    // See pokecrystal gfx/tilesets/*_palette_map.asm and tilepal macro.
    std::vector<uint8_t> palette_map;  // palette ID per tile (256 entries)
    
    // Time-of-day palettes (7 BG palettes × 4 colors each)
    // Crystal BG palette indices (from tileset_constants.asm):
    //   0 = GRAY, 1 = RED, 2 = GREEN, 3 = WATER, 4 = YELLOW, 5 = BROWN, 6 = ROOF
    //   (7 = TEXT is handled separately)
    // Each TimeOfDay has its own palette set
    // See pokecrystal/gfx/tilesets/bg_tiles.pal
    // 
    // The 5 rows are:
    //   0 = Morn ($00-$07 in TilesetBGPalette)
    //   1 = Day  ($08-$0F)
    //   2 = Nite ($10-$17)
    //   3 = Dark ($18-$1F)
    //   4 = Indoor ($20-$27) - used for morn AND day indoors
    std::array<std::array<Palette, 7>, 5> time_palettes;  // [TimeOfDay][palette_id]
    
    // Optional fixed special palette set for specific tilesets
    // Crystal tilesets that use special palettes (from tileset_palettes.asm):
    //   TILESET_HOUSE → HousePalette
    //   TILESET_ICE_PATH → IcePathPalette (unless Hall of Fame)
    //   TILESET_POKECOM_CENTER → PokecomPalette
    //   TILESET_BATTLE_TOWER_INSIDE → BattleTowerPalette
    //   TILESET_RADIO_TOWER → RadioTowerPalette
    //   TILESET_MANSION → MansionPalette
    // If present, this completely overrides environment+time palette selection
    std::optional<std::array<Palette, 7>> fixed_special_palette;
    
    // Legacy single palette set (for compatibility, uses Day palettes)
    std::array<Palette, 8> palettes;
    
    // Animations
    std::vector<TileAnimation> animations;
    
    // Debug-only ROM info
#ifndef NDEBUG
    struct Debug {
        uint8_t tileset_index;
        uint32_t gfx_rom_addr;
        uint32_t metatile_rom_addr;
        uint32_t collision_rom_addr;
        uint32_t palmap_rom_addr;
    } debug;
#endif
};

//=============================================================================
// EXTRACTION RESULT
//=============================================================================

struct TilesetExtractionResult {
    bool success = false;
    std::string error;
    ExtractedTileset tileset;
};

//=============================================================================
// EXTRACTOR
//=============================================================================

class TilesetExtractor {
public:
    TilesetExtractor(const RomData& rom, const ExtractionProfile& profile);
    
    // Extract single tileset
    TilesetExtractionResult extract_tileset(uint8_t tileset_index) const;
    TilesetExtractionResult extract_tileset(const std::string& tileset_id) const;
    
    // Extract all tilesets
    std::vector<ExtractedTileset> extract_all_tilesets() const;
    
    // Stats
    struct Stats {
        uint32_t tilesets_extracted = 0;
        uint32_t tiles_extracted = 0;
        uint32_t metatiles_extracted = 0;
        uint32_t bounds_check_failures = 0;
    };
    const Stats& stats() const { return stats_; }

private:
    const RomData& rom_;
    const ExtractionProfile& profile_;
    mutable Stats stats_;
    
    // Decode 2bpp tile data to pixels
    Tile decode_tile(const uint8_t* data) const;
    
    // Decompress LZ-compressed graphics
    bool decompress_lz(uint32_t addr, std::vector<uint8_t>& out) const;
    
    // ID generation
    std::string make_tileset_id(uint8_t index) const;
};

//=============================================================================
// RENDERED OUTPUT (for native package)
//=============================================================================

// Pre-rendered tileset atlas for the native package
// This is what actually gets saved - not the raw tile data
struct TilesetAtlas {
    std::string tileset_id;
    uint32_t atlas_width;       // In pixels
    uint32_t atlas_height;
    std::vector<uint32_t> pixels;  // RGBA32
    
    // UV coordinates for each metatile (32×32 pixels per metatile)
    struct MetatileUV {
        float u0, v0, u1, v1;
    };
    std::vector<MetatileUV> metatile_uvs;
    
    // Collision data: 4 bytes per metatile (TL, TR, BL, BR quadrants)
    // Index formula: collision[metatile_index * 4 + quadrant]
    // where quadrant = (cell_x % 2) + (cell_y % 2) * 2
    std::vector<uint8_t> collision;
};

// Render tileset to atlas with real Crystal palettes
// Uses the palette_map to assign each tile its correct palette
// time_of_day selects which palette variant to use (Morning/Day/Night/etc.)
TilesetAtlas render_tileset_atlas(const ExtractedTileset& tileset, TimeOfDay time_of_day = TimeOfDay::Day);

} // namespace crystal
