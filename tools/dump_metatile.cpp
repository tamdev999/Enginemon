#include <iostream>
#include <iomanip>
#include "crystal/rom/loader.hpp"
#include "crystal/rom/profile.hpp"
#include "crystal/extract/tileset_extractor.hpp"

using namespace crystal;

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: dump_metatile <rom.gbc>\n";
        return 1;
    }

    auto rom = RomData::load(argv[1]);
    if (!rom) {
        std::cerr << "Failed to load ROM\n";
        return 1;
    }
    
    auto& registry = ProfileRegistry::instance();
    const auto* profile = registry.get_profile_by_hash(rom->hash());
    if (!profile) {
        std::cerr << "Unknown ROM\n";
        return 1;
    }

    TilesetExtractor extractor(*rom, *profile);
    auto result = extractor.extract_tileset("lab");
    
    if (!result.success) {
        std::cerr << "Failed to extract lab tileset: " << result.error << "\n";
        return 1;
    }

    std::cout << "Lab tileset: " << result.tileset.metatiles.size() << " metatiles, " 
              << result.tileset.tiles.size() << " tiles\n";
    std::cout << "Palette map size: " << result.tileset.palette_map.size() << " entries\n\n";

    // Dump tile 0x81 (129) - this is in block 0x21
    int tile_idx = 0x81;
    std::cout << "=== Tile 0x" << std::hex << tile_idx << " (" << std::dec << tile_idx << ") ===\n";
    
    if (tile_idx >= (int)result.tileset.tiles.size()) {
        std::cout << "ERROR: Tile index out of bounds (tileset has " 
                  << result.tileset.tiles.size() << " tiles)\n";
        return 1;
    }
    
    const auto& tile = result.tileset.tiles[tile_idx];
    
    std::cout << "Decoded 2-bit pixel indices (Enginemon, Game Boy native convention):\n";
    std::cout << "(0=lightest/white, 3=darkest/black)\n";
    for (int row = 0; row < 8; ++row) {
        std::cout << "  ";
        for (int col = 0; col < 8; ++col) {
            std::cout << (int)tile.pixels[row * 8 + col];
        }
        std::cout << "\n";
    }
    
    std::cout << "\nSame indices inverted (for comparison with pokecrystal PNG):\n";
    std::cout << "(0=darkest/black, 3=lightest/white)\n";
    for (int row = 0; row < 8; ++row) {
        std::cout << "  ";
        for (int col = 0; col < 8; ++col) {
            std::cout << (3 - (int)tile.pixels[row * 8 + col]);
        }
        std::cout << "\n";
    }
    
    // Get palette ID
    uint8_t pal_id = 0;
    if (tile_idx < (int)result.tileset.palette_map.size()) {
        pal_id = result.tileset.palette_map[tile_idx];
    }
    std::cout << "\nPalette ID for tile 0x" << std::hex << tile_idx << ": " 
              << std::dec << (int)pal_id;
    
    // Palette names
    const char* pal_names[] = {"GRAY", "RED", "GREEN", "WATER", "YELLOW", "BROWN", "ROOF"};
    if (pal_id < 7) {
        std::cout << " (" << pal_names[pal_id] << ")";
    }
    std::cout << "\n";
    
    // Get the actual palette colors (using Day time)
    const auto& day_pals = result.tileset.time_palettes[static_cast<int>(TimeOfDay::Day)];
    if (pal_id < 7) {
        const auto& pal = day_pals[pal_id];
        std::cout << "\nPalette colors (5-bit RGB):\n";
        for (int c = 0; c < 4; ++c) {
            std::cout << "  Color " << c << ": R=" << (int)pal.colors[c].r 
                      << " G=" << (int)pal.colors[c].g 
                      << " B=" << (int)pal.colors[c].b << "\n";
        }
        
        std::cout << "\nNote: Game Boy palette convention:\n";
        std::cout << "  Color 0 = lightest, Color 3 = darkest\n";
        std::cout << "  GRAY palette: Color 0=near-white, Color 3=dark\n";
    }
    
    // Now render the tile with its palette and show final RGBA
    std::cout << "\nFinal rendered pixels (8-bit RGB from to_rgba32()):\n";
    if (pal_id < 7) {
        const auto& pal = day_pals[pal_id];
        for (int row = 0; row < 8; ++row) {
            std::cout << "  ";
            for (int col = 0; col < 8; ++col) {
                uint8_t pixel = tile.pixels[row * 8 + col];
                uint32_t rgba = pal.colors[pixel].to_rgba32();
                uint8_t r = rgba & 0xFF;
                uint8_t g = (rgba >> 8) & 0xFF;
                uint8_t b = (rgba >> 16) & 0xFF;
                std::cout << "(" << (int)r << "," << (int)g << "," << (int)b << ") ";
            }
            std::cout << "\n";
        }
    }
    
    std::cout << std::dec << "\n";
    
    // Also dump block 0x21 tile indices
    std::cout << "=== Block 0x21 tile indices ===\n";
    if (0x21 < result.tileset.metatiles.size()) {
        const auto& mt = result.tileset.metatiles[0x21];
        for (int row = 0; row < 4; ++row) {
            std::cout << "  ";
            for (int col = 0; col < 4; ++col) {
                int idx = mt.tile_indices[row * 4 + col];
                std::cout << std::hex << std::setfill('0') << std::setw(2) << idx;
                if (idx < (int)result.tileset.palette_map.size()) {
                    std::cout << "(pal" << (int)result.tileset.palette_map[idx] << ") ";
                } else {
                    std::cout << "(pal?) ";
                }
            }
            std::cout << "\n";
        }
    }

    // Render the atlas and verify block 0x21
    std::cout << std::dec;  // Reset to decimal
    std::cout << "\n=== Rendering Atlas ===\n";
    std::cout << "Tileset metatile count (before render): " << result.tileset.metatiles.size() << "\n";
    std::cout << "Tileset tiles count (before render): " << result.tileset.tiles.size() << "\n";
    auto atlas = render_tileset_atlas(result.tileset, TimeOfDay::Day);
    std::cout << "Atlas dimensions: " << atlas.atlas_width << "x" << atlas.atlas_height << "\n";
    std::cout << "Atlas pixel count: " << atlas.pixels.size() << " (expected: " << (atlas.atlas_width * atlas.atlas_height) << ")\n";
    std::cout << "Metatile UVs: " << atlas.metatile_uvs.size() << "\n";
    
    // Block 0x21 (33) is at atlas position:
    // Row = 33/8 = 4, Col = 33%8 = 1
    // Pixel position: x = 1*32 = 32, y = 4*32 = 128
    int block_id = 0x21;
    int atlas_row = block_id / 8;
    int atlas_col = block_id % 8;
    int base_x = atlas_col * 32;
    int base_y = atlas_row * 32;
    
    std::cout << "\nBlock 0x21 (decimal " << std::dec << block_id << "):\n";
    std::cout << "  Atlas row: " << atlas_row << ", col: " << atlas_col << "\n";
    std::cout << "  Atlas position: (" << base_x << ", " << base_y << ")\n";
    
    // Verify block is in range
    if (block_id >= (int)atlas.metatile_uvs.size()) {
        std::cout << "  ERROR: Block 0x21 out of range! Only " << atlas.metatile_uvs.size() << " metatiles in atlas.\n";
        return 1;
    }
    
    // Tile 0x81 is at position (0, 8) within the block (row 1, col 0)
    // So its absolute atlas position is (32 + 0*8, 128 + 1*8) = (32, 136)
    int tile_x = base_x + 0 * 8;  // Column 0 of block
    int tile_y = base_y + 1 * 8;  // Row 1 of block
    
    std::cout << "Tile 0x81 position in atlas: (" << tile_x << ", " << tile_y << ")\n";
    
    // Bounds check
    if (tile_x + 8 > (int)atlas.atlas_width || tile_y + 8 > (int)atlas.atlas_height) {
        std::cout << "ERROR: Tile position out of bounds!\n";
        return 1;
    }
    
    std::cout << "\nAtlas pixels for tile 0x81 (8x8):\n";
    
    for (int py = 0; py < 8; ++py) {
        std::cout << "  ";
        for (int px = 0; px < 8; ++px) {
            int ax = tile_x + px;
            int ay = tile_y + py;
            uint32_t rgba = atlas.pixels[ay * atlas.atlas_width + ax];
            uint8_t r = rgba & 0xFF;
            uint8_t g = (rgba >> 8) & 0xFF;
            uint8_t b = (rgba >> 16) & 0xFF;
            std::cout << std::dec << "(" << (int)r << "," << (int)g << "," << (int)b << ") ";
        }
        std::cout << "\n";
    }

    return 0;
}
