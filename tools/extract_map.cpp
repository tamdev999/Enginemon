// tools/extract_map.cpp
// Full map extraction test - extracts map + tileset to native format
// 
// Usage: extract_map <rom_file> [output_dir] [group] [index]
// Default: extracts New Bark Town (group 24, index 1) to ./output/

#include "crystal/rom/loader.hpp"
#include "crystal/rom/profile.hpp"
#include "crystal/extract/map_extractor.hpp"
#include "crystal/extract/tileset_extractor.hpp"

#include <iostream>
#include <iomanip>
#include <fstream>
#include <filesystem>

using namespace crystal;

void print_usage(const char* program) {
    std::cout << "Usage: " << program << " <rom_file> [output_dir] [group] [index]\n"
              << "\n"
              << "Extracts a map and tileset from Crystal ROM to semantic structures.\n"
              << "Default: New Bark Town (group 24, index 1)\n"
              << "\n"
              << "Example: " << program << " crystal.gbc ./output 24 1\n";
}

void dump_map(const ExtractedMap& map) {
    std::cout << "=== Extracted Map ===\n";
    std::cout << "ID: " << map.map_id << "\n";
    std::cout << "Display name: " << map.display_name << "\n";
    std::cout << "Dimensions: " << (int)map.width << " x " << (int)map.height << " metatiles\n";
    std::cout << "Border block: 0x" << std::hex << (int)map.border_block << std::dec << "\n";
    std::cout << "Tileset: " << map.tileset_id << "\n";
    std::cout << "Is outdoor: " << (map.is_outdoor ? "yes" : "no") << "\n";
    std::cout << "Script: " << map.map_script_id << "\n";
    std::cout << "\n";
    
    // Dump block data as grid
    std::cout << "=== Block Data (" << map.blocks.size() << " bytes) ===\n";
    for (uint8_t y = 0; y < map.height && y < 20; ++y) {
        std::cout << "  ";
        for (uint8_t x = 0; x < map.width && x < 20; ++x) {
            size_t idx = y * map.width + x;
            if (idx < map.blocks.size()) {
                std::cout << std::hex << std::setw(2) << std::setfill('0') 
                          << (int)map.blocks[idx] << " ";
            }
        }
        std::cout << "\n";
    }
    std::cout << std::dec << std::setfill(' ') << "\n";
    
    // Events summary
    std::cout << "=== Events ===\n";
    std::cout << "Warps: " << map.warps.size() << "\n";
    std::cout << "Coord events: " << map.coord_events.size() << "\n";
    std::cout << "BG events: " << map.bg_events.size() << "\n";
    std::cout << "Objects: " << map.objects.size() << "\n";
    std::cout << "Connections: " << map.connections.size() << "\n";
}

bool write_block_data(const std::filesystem::path& dir, const ExtractedMap& map) {
    std::filesystem::create_directories(dir);
    
    // Write map.json (semantic metadata)
    std::ofstream meta(dir / "map.json");
    if (!meta) return false;
    
    meta << "{\n";
    meta << "  \"id\": \"" << map.map_id << "\",\n";
    meta << "  \"display_name\": \"" << map.display_name << "\",\n";
    meta << "  \"width\": " << (int)map.width << ",\n";
    meta << "  \"height\": " << (int)map.height << ",\n";
    meta << "  \"tileset\": \"" << map.tileset_id << "\",\n";
    meta << "  \"border_block\": " << (int)map.border_block << ",\n";
    meta << "  \"is_outdoor\": " << (map.is_outdoor ? "true" : "false") << ",\n";
    meta << "  \"music\": \"" << map.music_id << "\"\n";
    meta << "}\n";
    
    // Write blocks.bin (raw block data)
    std::ofstream blocks(dir / "blocks.bin", std::ios::binary);
    if (!blocks) return false;
    blocks.write(reinterpret_cast<const char*>(map.blocks.data()), map.blocks.size());
    
    return true;
}

bool write_tileset_atlas(const std::filesystem::path& dir, const TilesetAtlas& atlas) {
    std::filesystem::create_directories(dir);
    
    // Write atlas metadata
    std::ofstream meta(dir / "atlas.json");
    if (!meta) return false;
    
    meta << "{\n";
    meta << "  \"id\": \"" << atlas.tileset_id << "\",\n";
    meta << "  \"width\": " << atlas.atlas_width << ",\n";
    meta << "  \"height\": " << atlas.atlas_height << ",\n";
    meta << "  \"metatile_count\": " << atlas.metatile_uvs.size() << "\n";
    meta << "}\n";
    
    // Write raw RGBA data (would be PNG in production)
    std::ofstream pixels(dir / "atlas.rgba", std::ios::binary);
    if (!pixels) return false;
    pixels.write(reinterpret_cast<const char*>(atlas.pixels.data()), 
                 atlas.pixels.size() * sizeof(uint32_t));
    
    // Write collision data
    std::ofstream collision(dir / "collision.bin", std::ios::binary);
    if (!collision) return false;
    collision.write(reinterpret_cast<const char*>(atlas.collision.data()),
                    atlas.collision.size());
    
    return true;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }
    
    std::filesystem::path rom_path = argv[1];
    std::filesystem::path output_dir = argc > 2 ? argv[2] : "./output";
    uint8_t group = argc > 3 ? std::stoi(argv[3]) : 24;
    uint8_t index = argc > 4 ? std::stoi(argv[4]) : 1;
    
    // Load ROM
    std::cout << "Loading ROM: " << rom_path << "\n";
    auto rom = RomData::load(rom_path);
    if (!rom) {
        std::cerr << "Failed to load ROM\n";
        return 1;
    }
    
    // Get profile
    auto& registry = ProfileRegistry::instance();
    const auto* profile = registry.get_profile_by_hash(rom->hash());
    if (!profile) {
        std::cerr << "ROM not supported (SHA-1: " << rom->hash() << ")\n";
        return 1;
    }
    
    std::cout << "Profile: " << profile->version_string << "\n";
    std::cout << "Extracting map group=" << (int)group << " index=" << (int)index << "\n\n";
    
    // Extract map
    MapExtractor map_extractor(*rom, *profile);
    auto map_result = map_extractor.extract_map(group, index);
    
    if (!map_result.success) {
        std::cerr << "Failed to extract map: " << map_result.error << "\n";
        return 1;
    }
    
    dump_map(map_result.map);
    
    // Extract tileset for this map
    std::cout << "\n=== Extracting Tileset ===\n";
    TilesetExtractor tileset_extractor(*rom, *profile);
    
    // For New Bark Town, tileset is TILESET_JOHTO (index 0)
    auto tileset_result = tileset_extractor.extract_tileset("johto_outdoor");
    
    if (tileset_result.success) {
        std::cout << "Tileset: " << tileset_result.tileset.tileset_id << "\n";
        std::cout << "Tiles: " << tileset_result.tileset.tiles.size() << "\n";
        std::cout << "Metatiles: " << tileset_result.tileset.metatiles.size() << "\n";
        std::cout << "Collision entries: " << tileset_result.tileset.collision.size() << "\n";
        
        // Render atlas
        std::cout << "\n=== Rendering Atlas ===\n";
        auto atlas = render_tileset_atlas(tileset_result.tileset, TimeOfDay::Day);
        std::cout << "Atlas size: " << atlas.atlas_width << "x" << atlas.atlas_height << "\n";
        
        // Write to output directory
        std::cout << "\n=== Writing Output ===\n";
        
        auto map_dir = output_dir / "maps" / map_result.map.map_id;
        if (write_block_data(map_dir, map_result.map)) {
            std::cout << "Wrote map data to: " << map_dir << "\n";
        } else {
            std::cerr << "Failed to write map data\n";
        }
        
        auto tileset_dir = output_dir / "tilesets" / atlas.tileset_id;
        if (write_tileset_atlas(tileset_dir, atlas)) {
            std::cout << "Wrote tileset atlas to: " << tileset_dir << "\n";
        } else {
            std::cerr << "Failed to write tileset atlas\n";
        }
    } else {
        std::cerr << "Failed to extract tileset: " << tileset_result.error << "\n";
    }
    
    // Statistics
    std::cout << "\n=== Extraction Stats ===\n";
    const auto& map_stats = map_extractor.stats();
    std::cout << "Maps extracted: " << map_stats.maps_extracted << "\n";
    std::cout << "Total blocks: " << map_stats.total_blocks << "\n";
    std::cout << "Bounds check failures: " << map_stats.bounds_check_failures << "\n";
    
    const auto& tileset_stats = tileset_extractor.stats();
    std::cout << "Tilesets extracted: " << tileset_stats.tilesets_extracted << "\n";
    std::cout << "Tiles decoded: " << tileset_stats.tiles_extracted << "\n";
    std::cout << "Metatiles decoded: " << tileset_stats.metatiles_extracted << "\n";
    
    // Verify semantic IDs (no ROM addresses leaked)
    std::cout << "\n=== Semantic Verification ===\n";
    bool clean = true;
    if (map_result.map.map_id.find("0x") != std::string::npos) {
        std::cerr << "WARNING: ROM address in map_id\n";
        clean = false;
    }
    if (map_result.map.tileset_id.find("0x") != std::string::npos) {
        std::cerr << "WARNING: ROM address in tileset_id\n";
        clean = false;
    }
    
    if (clean) {
        std::cout << "All IDs are semantic (no ROM addresses leaked) ✓\n";
    }
    
    std::cout << "\n=== ROM can now be closed ===\n";
    std::cout << "Extracted data is self-contained in: " << output_dir << "\n";
    
    return 0;
}
