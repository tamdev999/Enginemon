// corpus_dupe_scan.cpp - Scan corpus for any duplicate command identities
// This tool checks that every decoded command has a unique ROM address within its script body
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

using namespace crystal;

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
    
    // Collect unique script addresses
    std::set<uint32_t> all_addresses;
    
    for (const auto& ref : discovered_maps) {
        auto result = extractor.extract_map(ref.group, ref.map);
        if (!result.success) continue;
        
        for (const auto& obj : result.map.objects) {
            if (obj.script_rom_address != 0) {
                all_addresses.insert(obj.script_rom_address);
            }
        }
        for (const auto& bg : result.map.bg_events) {
            if (bg.script_rom_address != 0) {
                all_addresses.insert(bg.script_rom_address);
            }
        }
    }
    
    // Add StdScript addresses
    for (size_t i = 0; i < std_scripts.size(); ++i) {
        const auto* entry = std_scripts.get(static_cast<uint16_t>(i));
        if (entry && entry->flat_address != 0) {
            all_addresses.insert(entry->flat_address);
        }
    }
    
    std::cout << "Scanning " << all_addresses.size() << " script bodies for duplicate command addresses...\n\n";
    
    // Setup decoder
    SymbolMap symbols;
    TypedScriptDecoder decoder(*rom, symbols);
    
    size_t affected_bodies = 0;
    size_t total_duplicates = 0;
    
    std::vector<std::pair<uint32_t, size_t>> affected_list;  // (script_addr, dupe_count)
    
    for (uint32_t addr : all_addresses) {
        CrystalScriptIR ir = decoder.decode_script(addr);
        
        // Check for duplicate ROM addresses within this script
        std::map<uint32_t, std::vector<size_t>> addr_to_indices;
        for (size_t i = 0; i < ir.commands.size(); ++i) {
            addr_to_indices[ir.commands[i].span.rom_address].push_back(i);
        }
        
        size_t dupes_in_body = 0;
        for (const auto& [cmd_addr, indices] : addr_to_indices) {
            if (indices.size() > 1) {
                dupes_in_body += indices.size() - 1;  // Count extras
            }
        }
        
        if (dupes_in_body > 0) {
            affected_bodies++;
            total_duplicates += dupes_in_body;
            affected_list.push_back({addr, dupes_in_body});
        }
    }
    
    std::cout << "=== Duplicate Command Identity Scan Results ===\n";
    std::cout << "Total script bodies:    " << all_addresses.size() << "\n";
    std::cout << "Affected bodies:        " << affected_bodies << "\n";
    std::cout << "Total duplicate cmds:   " << total_duplicates << "\n\n";
    
    if (affected_bodies == 0) {
        std::cout << "✓ NO DUPLICATES FOUND - unique command identity invariant holds.\n";
    } else {
        std::cout << "✗ DUPLICATES DETECTED:\n";
        for (const auto& [script_addr, dupe_count] : affected_list) {
            std::cout << "  Script 0x" << std::hex << script_addr << std::dec 
                      << ": " << dupe_count << " duplicate command(s)\n";
        }
    }
    
    return affected_bodies > 0 ? 1 : 0;
}
