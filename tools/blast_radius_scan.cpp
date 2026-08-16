// blast_radius_scan.cpp - Exact pre-fix blast radius measurement
// Scans all 1362 bodies for duplicate command addresses
#include "crystal/rom/loader.hpp"
#include "crystal/rom/profile.hpp"
#include "crystal/script/typed_decoder.hpp"
#include "crystal/script/crystal_cfg.hpp"
#include "crystal/extract/map_extractor.hpp"
#include "crystal/compile/full_compiler.hpp"
#include <iostream>
#include <iomanip>
#include <map>
#include <set>
#include <vector>

using namespace crystal;

struct BodyDuplicateInfo {
    uint32_t body_address;
    std::string body_name;
    size_t duplicate_count;
    std::vector<uint32_t> duplicated_addresses;
};

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <rom_path>\n";
        return 1;
    }
    
    auto rom = RomData::load(argv[1]);
    if (!rom) {
        std::cerr << "Failed to load ROM\n";
        return 1;
    }
    
    auto& registry = ProfileRegistry::instance();
    auto profile = registry.get_profile_by_hash(rom->hash());
    if (!profile) {
        std::cerr << "ROM not supported\n";
        return 1;
    }
    
    // Load StdScripts table
    StdScriptsTable std_scripts;
    std_scripts.load(*rom, profile->offsets.std_scripts, profile->offsets.std_scripts_count);
    
    // Discover all maps
    MapExtractor extractor(*rom, *profile);
    auto discovered_maps = discover_reachable_maps(*rom, *profile, extractor);
    
    // Collect unique script addresses with names
    std::map<uint32_t, std::string> all_addresses;
    
    for (const auto& ref : discovered_maps) {
        auto result = extractor.extract_map(ref.group, ref.map);
        if (!result.success) continue;
        
        std::string map_name = result.map.map_id;
        
        for (size_t i = 0; i < result.map.objects.size(); ++i) {
            const auto& obj = result.map.objects[i];
            if (obj.script_rom_address != 0) {
                std::string name = map_name + "::obj_" + std::to_string(i);
                all_addresses[obj.script_rom_address] = name;
            }
        }
        for (size_t i = 0; i < result.map.bg_events.size(); ++i) {
            const auto& bg = result.map.bg_events[i];
            if (bg.script_rom_address != 0) {
                std::string name = map_name + "::bg_" + std::to_string(i);
                all_addresses[bg.script_rom_address] = name;
            }
        }
        // CoordEvents don't have script_rom_address - they use scene_id
        // Skip coord_events for script address collection
    }
    
    // Add StdScript addresses
    for (size_t i = 0; i < std_scripts.size(); ++i) {
        const auto* entry = std_scripts.get(static_cast<uint16_t>(i));
        if (entry && entry->flat_address != 0) {
            std::string name = "StdScript_" + std::to_string(i);
            all_addresses[entry->flat_address] = name;
        }
    }
    
    std::cout << "=== Pre-Fix Blast Radius Scan ===\n";
    std::cout << "Total executable bodies: " << all_addresses.size() << "\n\n";
    
    // Setup decoder
    SymbolMap symbols;
    TypedScriptDecoder decoder(*rom, symbols);
    
    std::vector<BodyDuplicateInfo> affected_bodies;
    size_t total_duplicate_instances = 0;
    std::set<uint32_t> all_duplicated_addresses;
    
    for (const auto& [addr, name] : all_addresses) {
        CrystalScriptIR ir = decoder.decode_script(addr);
        
        // Check for duplicate ROM addresses within this script
        std::map<uint32_t, size_t> addr_counts;
        for (const auto& cmd : ir.commands) {
            addr_counts[cmd.span.rom_address]++;
        }
        
        BodyDuplicateInfo info;
        info.body_address = addr;
        info.body_name = name;
        info.duplicate_count = 0;
        
        for (const auto& [cmd_addr, count] : addr_counts) {
            if (count > 1) {
                info.duplicate_count += count - 1;
                info.duplicated_addresses.push_back(cmd_addr);
                all_duplicated_addresses.insert(cmd_addr);
            }
        }
        
        if (info.duplicate_count > 0) {
            affected_bodies.push_back(info);
            total_duplicate_instances += info.duplicate_count;
        }
    }
    
    std::cout << "=== EXACT BLAST RADIUS ===\n";
    std::cout << "Bodies with duplicate source addresses: " << affected_bodies.size() << "\n";
    std::cout << "Total duplicated command instances:     " << total_duplicate_instances << "\n";
    std::cout << "Unique source addresses duplicated:     " << all_duplicated_addresses.size() << "\n\n";
    
    std::cout << "=== AFFECTED BODIES ===\n";
    for (const auto& info : affected_bodies) {
        std::cout << "Body 0x" << std::hex << info.body_address << std::dec 
                  << " (" << info.body_name << ")\n";
        std::cout << "  Duplicate count: " << info.duplicate_count << "\n";
        std::cout << "  Duplicated ROM addresses: ";
        for (uint32_t a : info.duplicated_addresses) {
            std::cout << "0x" << std::hex << a << std::dec << " ";
        }
        std::cout << "\n\n";
    }
    
    std::cout << "=== ALL DUPLICATED ADDRESSES ===\n";
    for (uint32_t a : all_duplicated_addresses) {
        std::cout << "0x" << std::hex << a << std::dec << "\n";
    }
    
    return 0;
}
