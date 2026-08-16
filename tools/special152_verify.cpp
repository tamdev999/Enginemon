// special152_verify.cpp - Verify all 5 Special 152 addresses are in discovered corpus
#include "crystal/rom/loader.hpp"
#include "crystal/rom/profile.hpp"
#include "crystal/compile/corpus_discovery.hpp"
#include "crystal/script/typed_decoder.hpp"
#include "crystal/script/crystal_cfg.hpp"
#include "crystal/extract/map_extractor.hpp"
#include <iostream>
#include <iomanip>
#include <map>
#include <set>

using namespace crystal;
using namespace enginemon;

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
    
    std::cout << "=== Special 152 Verification ===\n\n";
    
    // The 5 known physical source addresses for Special 152 in Pokecenter2F.asm
    std::set<uint32_t> expected_addresses = {
        0x192b34,  // Script_WalkOutOfLinkTradeRoom (male path)
        0x192b77,  // Script_WalkOutOfLinkTradeRoom.Female (sdefer target)
        0x192bb1,  // Script_WalkOutOfLinkBattleRoom.Female (sdefer target)
        0x192c2f,  // Script_LeftTimeCapsule
        0x192c7a   // Script_LeftTimeCapsule.Female (sdefer target)
    };
    
    // Load StdScripts table
    StdScriptsTable std_scripts;
    std_scripts.load(*rom, profile->offsets.std_scripts, profile->offsets.std_scripts_count);
    
    // Setup decoder for corpus discovery
    SymbolMap symbols;
    TypedScriptDecoder decoder(*rom, symbols);
    
    // Discover production corpus using UNIFIED fixed-point discovery
    MapExtractor extractor(*rom, *profile);
    auto corpus = discover_corpus(*rom, *profile, extractor, decoder, std_scripts);
    
    std::cout << "Corpus discovery stats:\n";
    std::cout << "  Map-root bodies:     " << corpus.stats.total_map_roots() << "\n";
    std::cout << "  StdScript bodies:    " << corpus.stats.std_script_roots << "\n";
    std::cout << "  Total unique bodies: " << corpus.stats.total_unique_bodies() << "\n";
    std::cout << "  Deferred targets:    " << corpus.stats.deferred_targets_encountered << "\n";
    std::cout << "  New deferred roots:  " << corpus.stats.deferred_new_roots << "\n\n";
    
    // Get all addresses
    auto all_addresses = corpus.all_addresses();
    
    // Scan all bodies for Special 152
    std::map<uint32_t, std::vector<uint32_t>> special152_sources;  // source_addr -> [body roots that contain it]
    
    for (uint32_t addr : all_addresses) {
        try {
            CrystalScriptIR ir = decoder.decode_script(addr);
            
            for (const auto& cmd : ir.commands) {
                if (const auto* special = std::get_if<Cmd_Special>(&cmd.data)) {
                    if (special->special_id == 152) {
                        special152_sources[cmd.span.rom_address].push_back(addr);
                    }
                }
            }
        } catch (...) {
            // Skip decode failures
        }
    }
    
    std::cout << "=== Special 152 Physical Addresses Found ===\n";
    std::cout << "Total: " << special152_sources.size() << " unique source addresses\n\n";
    
    for (const auto& [src_addr, body_roots] : special152_sources) {
        bool is_expected = expected_addresses.count(src_addr) > 0;
        std::cout << "0x" << std::hex << src_addr << std::dec;
        std::cout << " [" << (is_expected ? "EXPECTED" : "unexpected") << "]";
        std::cout << " in " << body_roots.size() << " bodies: ";
        for (uint32_t root : body_roots) {
            std::cout << "0x" << std::hex << root << std::dec << " ";
            
            // Show root type
            auto it = corpus.map_roots.find(root);
            if (it != corpus.map_roots.end()) {
                std::cout << "(" << script_root_type_name(it->second.root_type) << ") ";
            }
        }
        std::cout << "\n";
    }
    
    std::cout << "\n=== Verification ===\n";
    size_t found_expected = 0;
    for (uint32_t expected : expected_addresses) {
        bool found = special152_sources.count(expected) > 0;
        std::cout << "0x" << std::hex << expected << std::dec << ": ";
        std::cout << (found ? "FOUND" : "MISSING") << "\n";
        if (found) ++found_expected;
    }
    
    std::cout << "\nFound " << found_expected << " / " << expected_addresses.size() 
              << " expected Special 152 addresses\n";
    
    if (found_expected == expected_addresses.size()) {
        std::cout << "\n✓ ALL 5 SPECIAL 152 ADDRESSES VERIFIED IN CORPUS\n";
        return 0;
    } else {
        std::cout << "\n✗ MISSING SPECIAL 152 ADDRESSES!\n";
        return 1;
    }
}
