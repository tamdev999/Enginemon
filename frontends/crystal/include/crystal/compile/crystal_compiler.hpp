#pragma once
// crystal/compile/crystal_compiler.hpp
// High-level Crystal ROM → EMON package compiler
//
// Owns the complete compilation pipeline:
//   ROM → MapExtractor → event pointer resolution → ScriptDecoder → LuaEmitter
//   → semantic maps + scripts → PackageWriter → EMON package
//
// After compilation, the ROM and all Crystal-specific types can be discarded.
// Runtime loads only from the package.

#include "crystal/rom/loader.hpp"
#include "crystal/rom/profile.hpp"
#include "crystal/extract/map_extractor.hpp"
#include "crystal/extract/tileset_extractor.hpp"
#include "crystal/extract/sprite_extractor.hpp"
#include "crystal/extract/font_extractor.hpp"
#include "crystal/script/decoder.hpp"
#include "crystal/script/lua_emitter.hpp"
#include "crystal/output/native_package.hpp"

#include <string>
#include <vector>
#include <unordered_set>
#include <unordered_map>
#include <filesystem>
#include <optional>

namespace crystal {

// Script compilation result (for deduplication tracking)
struct CompiledScript {
    std::string script_id;      // Globally unique: "map_id::event_type_index" or ROM address
    std::string lua_code;       // Generated Lua
    uint32_t rom_address;       // Source ROM address (for deduplication)
};

// Compiler configuration
struct CompilerConfig {
    // Maps to compile (empty = all reachable from starting map)
    std::vector<std::string> maps_to_compile;
    
    // Starting map for reachability analysis
    std::string starting_map = "new_bark_town";
    
    // Include connected maps recursively
    bool follow_connections = true;
    
    // Include warp targets recursively
    bool follow_warps = true;
    
    // Lua emitter config
    EmitterConfig emitter_config;
    
    // Time of day for tileset rendering (Day default)
    TimeOfDay tileset_time_of_day = TimeOfDay::Day;
};

// Compilation statistics
struct CompilerStats {
    uint32_t maps_compiled = 0;
    uint32_t tilesets_compiled = 0;
    uint32_t sprites_compiled = 0;
    uint32_t scripts_compiled = 0;
    uint32_t scripts_deduplicated = 0;  // Scripts that shared ROM address
    uint32_t total_lua_bytes = 0;
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
};

// High-level Crystal compiler
// Compiles ROM to complete EMON package with all scripts pre-decoded
class CrystalCompiler {
public:
    CrystalCompiler(const RomData& rom, const ExtractionProfile& profile);
    
    // Compile ROM to package file
    // Returns true on success
    bool compile(const std::filesystem::path& output_path, const CompilerConfig& config = {});
    
    // Get compilation statistics
    const CompilerStats& stats() const { return stats_; }
    
private:
    const RomData& rom_;
    const ExtractionProfile& profile_;
    
    // Owned subsystems
    MapExtractor map_extractor_;
    TilesetExtractor tileset_extractor_;
    SpriteExtractor sprite_extractor_;
    FontExtractor font_extractor_;
    ScriptDecoder script_decoder_;
    LuaEmitter lua_emitter_;
    SymbolMap symbols_;  // Empty - we use address-based decoding
    
    CompilerStats stats_;
    
    // Deduplication tracking
    std::unordered_set<std::string> compiled_maps_;
    std::unordered_set<std::string> compiled_tilesets_;
    std::unordered_set<std::string> compiled_sprites_;
    std::unordered_set<std::string> required_sprites_;   // Collected from map objects
    std::unordered_map<uint32_t, std::string> script_address_to_id_;  // ROM addr → ScriptId
    
    // Compilation helpers
    
    // Compile a single map and add to writer
    // Returns true on success
    bool compile_map(const std::string& map_id, PackageWriter& writer);
    
    // Compile all scripts for a map's events
    // Populates map.scripts with Lua code, updates event script_ids to global form
    void compile_map_scripts(ExtractedMap& map, PackageWriter& writer);
    
    // Compile a single script from ROM address
    // Returns globally unique ScriptId, adds to writer if not deduplicated
    std::string compile_script(uint32_t rom_address, const std::string& base_script_id, 
                               PackageWriter& writer);
    
    // Discover reachable maps from starting point
    std::vector<std::string> discover_reachable_maps(const std::string& start_map,
                                                      const CompilerConfig& config);
    
    // Make globally unique script ID from map and local event info
    // Format: "map_id::event_type_index" (e.g., "new_bark_town::bg_event_0")
    static std::string make_global_script_id(const std::string& map_id, 
                                              const std::string& local_script_id);
};

} // namespace crystal
