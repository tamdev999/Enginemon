// tools/palette_audit.cpp
// Read-only diagnostic to verify Elm's Lab palette data
// Does NOT modify any runtime code

#include "crystal/output/native_package.hpp"
#include "engine/package/package_reader.hpp"
#include "engine/world/runtime_tileset.hpp"
#include "engine/world/runtime_map.hpp"
#include <iostream>
#include <iomanip>

using namespace enginemon;
// NOT using namespace crystal - conflicts with enginemon::PackageReader

// Inline resolve_palette_row (same logic as TileRenderer::resolve_palette_row)
PaletteRow resolve_palette_row(Environment env, PalettePolicy policy, PaletteRow rtc_time) {
    PaletteRow effective_time;
    switch (policy) {
        case PalettePolicy::Auto: effective_time = rtc_time; break;
        case PalettePolicy::Day: effective_time = PaletteRow::Day; break;
        case PalettePolicy::Nite: effective_time = PaletteRow::Nite; break;
        case PalettePolicy::Morn: effective_time = PaletteRow::Morn; break;
        case PalettePolicy::Dark: return PaletteRow::Dark;
    }
    
    switch (env) {
        case Environment::Outdoor:
            return effective_time;
        case Environment::Indoor:
        case Environment::Dungeon:
            switch (effective_time) {
                case PaletteRow::Morn:
                case PaletteRow::Day:
                    return PaletteRow::Indoor;
                case PaletteRow::Nite:
                    return PaletteRow::Nite;
                case PaletteRow::Dark:
                    return PaletteRow::Dark;
                default:
                    return PaletteRow::Indoor;
            }
    }
    return PaletteRow::Day;
}

void print_rgba(uint32_t rgba) {
    uint8_t r = (rgba >> 0) & 0xFF;
    uint8_t g = (rgba >> 8) & 0xFF;
    uint8_t b = (rgba >> 16) & 0xFF;
    uint8_t a = (rgba >> 24) & 0xFF;
    std::cout << "RGBA(" << (int)r << "," << (int)g << "," << (int)b << "," << (int)a << ")";
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: palette_audit <package.emon>\n";
        return 1;
    }
    
    auto package = enginemon::PackageReader::open(argv[1]);
    if (!package) {
        std::cerr << "Failed to open package\n";
        return 1;
    }
    
    std::cout << "=== Elm's Lab Palette Audit ===\n\n";
    
    // 1. Load Elm's Lab map
    auto map_opt = package->load_map("elms_lab");
    if (!map_opt) {
        std::cerr << "Failed to load elms_lab map\n";
        return 1;
    }
    RuntimeMap& map = *map_opt;
    
    std::cout << "1. RuntimeMap.environment = " << static_cast<int>(map.environment) 
              << " (" << (map.environment == Environment::Outdoor ? "Outdoor" :
                         map.environment == Environment::Indoor ? "Indoor" : "Dungeon") << ")\n";
    
    std::cout << "2. RuntimeMap.time_policy = " << static_cast<int>(map.time_policy)
              << " (" << (map.time_policy == PalettePolicy::Auto ? "Auto" :
                         map.time_policy == PalettePolicy::Day ? "Day" :
                         map.time_policy == PalettePolicy::Nite ? "Nite" :
                         map.time_policy == PalettePolicy::Morn ? "Morn" : "Dark") << ")\n";
    
    // 3. resolve_palette_row result
    PaletteRow resolved = resolve_palette_row(
        map.environment, map.time_policy, PaletteRow::Day);
    std::cout << "3. resolve_palette_row(env=" << static_cast<int>(map.environment)
              << ", policy=" << static_cast<int>(map.time_policy) 
              << ", RTC=Day) = " << static_cast<int>(resolved)
              << " (" << (resolved == PaletteRow::Morn ? "Morn" :
                         resolved == PaletteRow::Day ? "Day" :
                         resolved == PaletteRow::Nite ? "Nite" :
                         resolved == PaletteRow::Dark ? "Dark" : "Indoor") << ")\n\n";
    
    // Load tileset for Elm's Lab
    auto tileset_data = package->load_tileset_data(map.tileset_id);
    if (!tileset_data) {
        std::cerr << "Failed to load tileset data for " << map.tileset_id << "\n";
        return 1;
    }
    
    RuntimeTileset tileset = RuntimeTileset::from_package_data(map.tileset_id, *tileset_data);
    
    // 4. fixed_special_palette
    std::cout << "4. Tileset '" << map.tileset_id << "' fixed_special_palette = "
              << (tileset.fixed_special_palette.has_value() ? "PRESENT" : "NONE") << "\n\n";
    
    // 5. Pick a visible tile - let's use the first non-zero tile referenced by block 0
    // Block 0 is typically floor/background
    uint16_t sample_tile_id = 0;
    if (!tileset.blocks.empty()) {
        for (int i = 0; i < 16; ++i) {
            uint16_t tid = tileset.blocks[0].tile_ids[i];
            if (tid > 0 && tid < tileset.tiles.size()) {
                sample_tile_id = tid;
                break;
            }
        }
    }
    // If block 0 is all zeros, try block 1
    if (sample_tile_id == 0 && tileset.blocks.size() > 1) {
        for (int i = 0; i < 16; ++i) {
            uint16_t tid = tileset.blocks[1].tile_ids[i];
            if (tid > 0 && tid < tileset.tiles.size()) {
                sample_tile_id = tid;
                break;
            }
        }
    }
    std::cout << "5. Sample NativeTileId from block[0 or 1] = " << sample_tile_id << "\n";
    
    // 6. palette_map[tile_id]
    uint8_t pal_id = 0;
    if (sample_tile_id < tileset.palette_map.size()) {
        pal_id = tileset.palette_map[sample_tile_id];
    }
    std::cout << "6. palette_map[" << sample_tile_id << "] = " << (int)pal_id << "\n\n";
    
    // 7. Colors for that palette in Day vs Indoor rows
    std::cout << "7. Palette " << (int)pal_id << " colors:\n";
    
    const RuntimePaletteSet& day_set = tileset.standard_palette_rows[static_cast<size_t>(PaletteRow::Day)];
    const RuntimePaletteSet& indoor_set = tileset.standard_palette_rows[static_cast<size_t>(PaletteRow::Indoor)];
    
    std::cout << "   standard_palette_rows[Day][" << (int)pal_id << "]:\n";
    for (int c = 0; c < 4; ++c) {
        std::cout << "     color[" << c << "] = ";
        print_rgba(day_set.palettes[pal_id].colors[c]);
        std::cout << "\n";
    }
    
    std::cout << "   standard_palette_rows[Indoor][" << (int)pal_id << "]:\n";
    for (int c = 0; c < 4; ++c) {
        std::cout << "     color[" << c << "] = ";
        print_rgba(indoor_set.palettes[pal_id].colors[c]);
        std::cout << "\n";
    }
    
    // Check if they differ
    bool differ = false;
    for (int c = 0; c < 4; ++c) {
        if (day_set.palettes[pal_id].colors[c] != indoor_set.palettes[pal_id].colors[c]) {
            differ = true;
            break;
        }
    }
    std::cout << "\n   Day vs Indoor: " << (differ ? "DIFFERENT" : "IDENTICAL") << "\n\n";
    
    // 8. Actual RGBA from atlas generation
    std::cout << "8. TileAtlas::from_tileset_with_palette() output for tile " << sample_tile_id << ":\n";
    
    // Get the resolved palette set (what set_tileset would use)
    const RuntimePaletteSet* active_set;
    if (tileset.fixed_special_palette) {
        active_set = &(*tileset.fixed_special_palette);
        std::cout << "   Using: fixed_special_palette\n";
    } else {
        active_set = &tileset.standard_palette_rows[static_cast<size_t>(resolved)];
        std::cout << "   Using: standard_palette_rows[" << static_cast<int>(resolved) << "]\n";
    }
    
    // Show what colors would be used for this tile
    const RuntimePalette& tile_pal = active_set->palettes[pal_id];
    std::cout << "   Resolved palette " << (int)pal_id << " colors:\n";
    for (int c = 0; c < 4; ++c) {
        std::cout << "     color[" << c << "] = ";
        print_rgba(tile_pal.colors[c]);
        std::cout << "\n";
    }
    
    // Show actual pixel values from the tile
    if (sample_tile_id < tileset.tiles.size()) {
        const RuntimeTile& tile = tileset.tiles[sample_tile_id];
        std::cout << "\n   Tile " << sample_tile_id << " first 8 pixel indices: ";
        for (int i = 0; i < 8; ++i) {
            std::cout << (int)tile.indices[i] << " ";
        }
        std::cout << "\n   -> First 8 RGBA values:\n";
        for (int i = 0; i < 8; ++i) {
            uint8_t idx = tile.indices[i];
            std::cout << "      pixel[" << i << "] idx=" << (int)idx << " -> ";
            print_rgba(tile_pal.colors[idx]);
            std::cout << "\n";
        }
    }
    
    std::cout << "\n=== Package timestamp check ===\n";
    std::cout << "Source version: " << package->source_version() << "\n";
    
    return 0;
}
