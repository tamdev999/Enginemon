// crystal/compile/crystal_compiler.cpp
// High-level Crystal ROM → EMON package compiler implementation

#include "crystal/compile/crystal_compiler.hpp"
#include <iostream>
#include <queue>

namespace crystal {

CrystalCompiler::CrystalCompiler(const RomData& rom, const ExtractionProfile& profile)
    : rom_(rom)
    , profile_(profile)
    , map_extractor_(rom, profile)
    , tileset_extractor_(rom, profile)
    , sprite_extractor_(rom, profile)
    , font_extractor_(rom, profile)
    , script_decoder_(rom, symbols_)
    , lua_emitter_()
{
}

std::string CrystalCompiler::make_global_script_id(const std::string& map_id,
                                                    const std::string& local_script_id) {
    // Format: "map_id::local_id" ensures global uniqueness
    return map_id + "::" + local_script_id;
}

std::vector<std::string> CrystalCompiler::discover_reachable_maps(
    const std::string& start_map,
    const CompilerConfig& config)
{
    std::vector<std::string> result;
    std::unordered_set<std::string> visited;
    std::queue<std::string> to_visit;
    
    to_visit.push(start_map);
    visited.insert(start_map);
    
    while (!to_visit.empty()) {
        std::string map_id = to_visit.front();
        to_visit.pop();
        result.push_back(map_id);
        
        // Extract map to find connections and warps
        auto map_result = map_extractor_.extract_map(map_id);
        if (!map_result.success) {
            stats_.warnings.push_back("Could not extract map for reachability: " + map_id);
            continue;
        }
        
        // Follow warps
        if (config.follow_warps) {
            for (const auto& warp : map_result.map.warps) {
                if (!warp.target_map_id.empty() && !visited.contains(warp.target_map_id)) {
                    visited.insert(warp.target_map_id);
                    to_visit.push(warp.target_map_id);
                }
            }
        }
        
        // Follow connections
        if (config.follow_connections) {
            for (const auto& conn : map_result.map.connections) {
                if (!conn.target_map_id.empty() && !visited.contains(conn.target_map_id)) {
                    visited.insert(conn.target_map_id);
                    to_visit.push(conn.target_map_id);
                }
            }
        }
    }
    
    return result;
}

std::string CrystalCompiler::compile_script(uint32_t rom_address, 
                                             const std::string& base_script_id,
                                             PackageWriter& writer)
{
    // Check for deduplication (same ROM address = same script)
    auto dedup_it = script_address_to_id_.find(rom_address);
    if (dedup_it != script_address_to_id_.end()) {
        stats_.scripts_deduplicated++;
        return dedup_it->second;
    }
    
    try {
        // Decode script from ROM
        ScriptIR script_ir = script_decoder_.decode_script(rom_address, base_script_id);
        
        // Emit as Lua
        std::string lua_code = lua_emitter_.emit(script_ir);
        
        // Wrap in IIFE to create global "script" table (same pattern as proven tests)
        lua_code = "script = (function()\n" + lua_code + "\nend)()";
        
        // Register for deduplication
        script_address_to_id_[rom_address] = base_script_id;
        
        // Add to package
        writer.add_script(base_script_id, lua_code);
        
        stats_.scripts_compiled++;
        stats_.total_lua_bytes += static_cast<uint32_t>(lua_code.size());
        
        return base_script_id;
    } catch (const std::exception& e) {
        stats_.warnings.push_back("Script compilation failed for " + base_script_id + 
                                   " at 0x" + std::format("{:x}", rom_address) + ": " + e.what());
        return base_script_id;  // Return the ID anyway, script will be missing from package
    }
}

void CrystalCompiler::compile_map_scripts(ExtractedMap& map, PackageWriter& writer)
{
    // Process BG events - use pre-extracted script_rom_address
    for (auto& bg : map.bg_events) {
        // Only Read/FacingUp types have scripts (not HiddenItem)
        if (bg.script_rom_address != 0) {
            // Make global script ID
            std::string global_id = make_global_script_id(map.map_id, bg.script_id);
            
            // Compile script and update event's script_id to global form
            std::string actual_id = compile_script(bg.script_rom_address, global_id, writer);
            bg.script_id = actual_id;
        }
    }
    
    // Process object events - use pre-extracted script_rom_address
    for (auto& obj : map.objects) {
        if (obj.script_rom_address != 0) {
            // Make global script ID
            std::string global_id = make_global_script_id(map.map_id, obj.script_id);
            
            // Compile script and update event's script_id to global form
            std::string actual_id = compile_script(obj.script_rom_address, global_id, writer);
            obj.script_id = actual_id;
        }
    }
}

bool CrystalCompiler::compile_map(const std::string& map_id, PackageWriter& writer)
{
    // Skip if already compiled
    if (compiled_maps_.contains(map_id)) {
        return true;
    }
    
    // Extract map
    auto result = map_extractor_.extract_map(map_id);
    if (!result.success) {
        stats_.errors.push_back("Failed to extract map " + map_id + ": " + result.error);
        return false;
    }
    
    ExtractedMap& map = result.map;
    
    std::cout << "[COMPILE] Map: " << map_id << " (" << (int)map.width << "x" << (int)map.height 
              << " blocks, " << map.objects.size() << " objects, " << map.bg_events.size() << " bg_events)\n";
    
    // Compile scripts for this map's events
    compile_map_scripts(map, writer);
    
    // Compile tileset if not already done
    if (!compiled_tilesets_.contains(map.tileset_id)) {
        auto tileset_result = tileset_extractor_.extract_tileset(map.tileset_id);
        if (tileset_result.success) {
            // Use native 8×8 tile format (preserves tile semantics for future 3D/RT)
            writer.add_tileset(tileset_result.tileset, TimeOfDay::Day);
            
            // Collision data is part of the tileset now
            // (No separate add_collision needed - it's included in add_tileset)
            
            compiled_tilesets_.insert(map.tileset_id);
            stats_.tilesets_compiled++;
            
            std::cout << "[COMPILE] Tileset: " << map.tileset_id 
                      << " (" << tileset_result.tileset.metatiles.size() << " metatiles, "
                      << tileset_result.tileset.tiles.size() << " tiles)\n";
        } else {
            stats_.warnings.push_back("Failed to extract tileset " + map.tileset_id);
        }
    }
    
    // Collect sprite IDs from map objects for later compilation
    for (const auto& obj : map.objects) {
        if (!obj.sprite_id.empty()) {
            required_sprites_.insert(obj.sprite_id);
        }
    }
    
    // Add map to package
    writer.add_map(map);
    compiled_maps_.insert(map_id);
    stats_.maps_compiled++;
    
    return true;
}

bool CrystalCompiler::compile(const std::filesystem::path& output_path, 
                               const CompilerConfig& config)
{
    std::cout << "=== Crystal Compiler ===\n";
    std::cout << "Source ROM: " << profile_.version_string << "\n";
    std::cout << "Output: " << output_path << "\n\n";
    
    // Reset state
    stats_ = CompilerStats{};
    compiled_maps_.clear();
    compiled_tilesets_.clear();
    script_address_to_id_.clear();
    required_sprites_.clear();
    compiled_sprites_.clear();
    
    // Configure emitter
    lua_emitter_ = LuaEmitter(config.emitter_config);
    
    PackageWriter writer;
    writer.set_source_rom(profile_.sha1, profile_.version_string);
    
    // Determine maps to compile
    std::vector<std::string> maps_to_compile;
    if (!config.maps_to_compile.empty()) {
        maps_to_compile = config.maps_to_compile;
    } else {
        // Discover reachable maps from starting point
        maps_to_compile = discover_reachable_maps(config.starting_map, config);
        
        // Limit to reasonable set for now (prevents runaway discovery)
        if (maps_to_compile.size() > 50) {
            std::cout << "Warning: Limiting to first 50 maps (found " << maps_to_compile.size() << ")\n";
            maps_to_compile.resize(50);
        }
    }
    
    std::cout << "Maps to compile: " << maps_to_compile.size() << "\n\n";
    
    // Compile each map (also collects required sprite IDs)
    for (const auto& map_id : maps_to_compile) {
        if (!compile_map(map_id, writer)) {
            // Continue with other maps even if one fails
        }
    }
    
    // Compile sprites - player sprite first, then all required NPC sprites
    std::cout << "\n[COMPILE] Sprites...\n";
    
    // Always include player sprite (chris = male, kris = female)
    auto player_result = sprite_extractor_.extract_player_sprite(false);
    if (player_result.success) {
        writer.add_sprite(player_result.sprite);
        compiled_sprites_.insert(player_result.sprite.sprite_id);
        stats_.sprites_compiled++;
        std::cout << "[COMPILE] Sprite: " << player_result.sprite.sprite_id 
                  << " (player, " << player_result.sprite.frame_count() << " frames)\n";
    } else {
        stats_.warnings.push_back("Failed to extract player sprite: " + player_result.error);
    }
    
    // Compile all required NPC sprites collected from maps
    for (const auto& sprite_id : required_sprites_) {
        if (compiled_sprites_.contains(sprite_id)) continue;
        
        auto sprite_result = sprite_extractor_.extract_sprite(sprite_id);
        if (sprite_result.success) {
            writer.add_sprite(sprite_result.sprite);
            compiled_sprites_.insert(sprite_id);
            stats_.sprites_compiled++;
            std::cout << "[COMPILE] Sprite: " << sprite_id 
                      << " (" << sprite_result.sprite.frame_count() << " frames)\n";
        } else {
            stats_.warnings.push_back("Failed to extract sprite " + sprite_id + ": " + sprite_result.error);
        }
    }
    
    // Extract and compile OBJ palettes (shared across all sprites)
    auto pal_result = sprite_extractor_.extract_obj_palettes();
    if (pal_result.success) {
        writer.add_obj_palettes(pal_result.palettes);
        std::cout << "[COMPILE] OBJ palettes (4 ToD × 8 palettes)\n";
    } else {
        stats_.warnings.push_back("Failed to extract OBJ palettes: " + pal_result.error);
    }
    
    // Compile font
    auto font_result = font_extractor_.extract_font();
    if (font_result.success) {
        auto font_atlas = render_font_atlas(font_result.font, default_text_palette());
        writer.add_font_atlas(font_atlas);
        std::cout << "[COMPILE] Font: " << font_atlas.font_id << "\n";
    } else {
        stats_.warnings.push_back("Failed to extract font: " + font_result.error);
    }
    
    // Write package
    std::cout << "\nWriting package...\n";
    if (!writer.write(output_path)) {
        stats_.errors.push_back("Failed to write package to " + output_path.string());
        return false;
    }
    
    // Report stats
    std::cout << "\n=== Compilation Complete ===\n";
    std::cout << "Maps: " << stats_.maps_compiled << "\n";
    std::cout << "Tilesets: " << stats_.tilesets_compiled << "\n";
    std::cout << "Sprites: " << stats_.sprites_compiled << "\n";
    std::cout << "Scripts: " << stats_.scripts_compiled << " (" << stats_.scripts_deduplicated << " deduplicated)\n";
    std::cout << "Total Lua: " << stats_.total_lua_bytes << " bytes\n";
    
    if (!stats_.warnings.empty()) {
        std::cout << "\nWarnings:\n";
        for (const auto& w : stats_.warnings) {
            std::cout << "  - " << w << "\n";
        }
    }
    
    if (!stats_.errors.empty()) {
        std::cout << "\nErrors:\n";
        for (const auto& e : stats_.errors) {
            std::cout << "  - " << e << "\n";
        }
        return false;
    }
    
    return true;
}

} // namespace crystal
