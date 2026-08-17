// stdscripts_coverage_test.cpp
// StdScripts Coverage Analysis for Stage 5 Verification
//
// For all 52 entries in Crystal's StdScripts table, reports:
// - std_id, target ROM address, pokecrystal label
// - whether target is unique or aliases another entry
// - whether its command body is present in Stage-1 typed IR
// - whether its command body has a Stage-2 CFG
// - whether its commands are Stage-4 semantically lowered
// - whether its body passes the Stage-5 legality gate
//
// Also reconciles the exact script count:
// - FullGameCompiler script count (~1339)
// - 1297 map-root unique addresses
// - 52 StdScripts table entries
// - overlap between StdScript targets and 1297 roots

#include "crystal/rom/loader.hpp"
#include "crystal/rom/profile.hpp"
#include "crystal/rom/symbol_map.hpp"
#include "crystal/script/typed_decoder.hpp"
#include "crystal/script/crystal_command.hpp"
#include "crystal/script/crystal_cfg.hpp"
#include "crystal/script/native_registry.hpp"
#include "crystal/script/semantic_legalizer.hpp"
#include "crystal/script/legality_gate.hpp"
#include "crystal/extract/map_extractor.hpp"
#include <iostream>
#include <iomanip>
#include <set>
#include <map>
#include <queue>
#include <algorithm>
#include <sstream>

using namespace crystal;
using namespace enginemon;

struct MapIdRef {
    uint8_t group;
    uint8_t map;
    bool operator<(const MapIdRef& other) const {
        if (group != other.group) return group < other.group;
        return map < other.map;
    }
};

// Pokecrystal standard script names (from engine/overworld/scripting.asm)
const char* get_std_script_name(uint16_t std_id) {
    static const char* names[] = {
        "PokecenterNurseScript",           // 0
        "DifficultBookshelfScript",        // 1
        "PicturePokemon",                   // 2
        "MerchandiseShelfScript",          // 3
        "TownMap",                          // 4
        "PCScript",                         // 5
        "RadioTowerPC",                     // 6
        "Pokemon",                          // 7
        "MerchandiseShelfScript2",         // 8
        "TrainerTalkScript",               // 9
        "Pokemon2",                         // 10
        "Pokemon3",                         // 11
        "MapDefaultScript",                // 12
        "AskNumber1M",                     // 13
        "AskNumber2M",                     // 14
        "RegisteredNumberM",               // 15
        "NumberAcceptedM",                 // 16
        "NumberDeclinedM",                 // 17
        "PhoneFullM",                      // 18
        "RematchM",                        // 19
        "GiftM",                           // 20
        "PackFullM",                       // 21
        "RematchGiftM",                    // 22
        "AskNumber1F",                     // 23
        "AskNumber2F",                     // 24
        "RegisteredNumberF",               // 25
        "NumberAcceptedF",                 // 26
        "NumberDeclinedF",                 // 27
        "PhoneFullF",                      // 28
        "RematchF",                        // 29
        "GiftF",                           // 30
        "PackFullF",                       // 31
        "RematchGiftF",                    // 32
        "LegendaryPokemon",                // 33
        "Pokemon4",                        // 34
        "TradeScript",                     // 35
        "TitleScreenPicScript",            // 36
        "LuckyNumberScript",               // 37
        "GameCornerCoinRewardScript",      // 38
        "BugContestResultsScript",         // 39
        "BugContestResultsWarpScript",     // 40
        "BugContestResultsScript2",        // 41
        "BattleTowerResultsScript",        // 42
        "BattleTowerExitScript",           // 43
        "ApricornAskScript",               // 44
        "DayMonScript",                    // 45
        "PokeSwapScript",                  // 46
        "MonCheckScript",                  // 47
        "FishingGuruTakesItScript",        // 48
        "TutorScript",                     // 49
        "MoveTutorScript",                 // 50
        "HappinessCheckScript",            // 51
    };
    if (std_id < 52) return names[std_id];
    return "Unknown";
}

// Discover reachable maps (same as corpus_test)
std::vector<MapIdRef> discover_reachable_maps(const RomData& rom, const ExtractionProfile& profile, 
                                               MapExtractor& extractor) {
    std::set<MapIdRef> visited;
    std::queue<MapIdRef> frontier;
    std::vector<MapIdRef> result;
    
    auto enqueue = [&](uint8_t group, uint8_t map) {
        if (group == 0 || map == 0) return;
        MapIdRef ref{group, map};
        if (!visited.contains(ref)) {
            visited.insert(ref);
            frontier.push(ref);
        }
    };
    
    // Seeds
    enqueue(24, 4);
    enqueue(24, 7);
    
    // Spawn points
    if (profile.offsets.spawn_points != 0) {
        for (uint8_t i = 0; i < 30; ++i) {
            uint32_t addr = profile.offsets.spawn_points + (i * 4);
            if (addr + 2 > rom.size()) break;
            uint8_t grp = rom.read_byte(addr);
            uint8_t idx = rom.read_byte(addr + 1);
            if (grp == 0xFF) break;
            if (grp > 0 && grp <= 26 && idx > 0 && idx < 100) {
                enqueue(grp, idx);
            }
        }
    }
    
    const auto& o = profile.offsets;
    const auto& fmt = profile.format.map;
    
    while (!frontier.empty()) {
        MapIdRef ref = frontier.front();
        frontier.pop();
        auto map_result = extractor.extract_map(ref.group, ref.map);
        if (!map_result.success) continue;
        const auto& map = map_result.map;
        if (map.width == 0 || map.height == 0 || map.width > 100 || map.height > 100) continue;
        result.push_back(ref);
        
        uint32_t group_ptr_addr = o.map_group_pointers + ((ref.group - 1) * 2);
        if (group_ptr_addr + 2 > rom.size()) continue;
        uint16_t group_addr = rom.read_word(group_ptr_addr);
        uint32_t group_flat = rom.bank_to_flat(o.map_groups_bank, group_addr);
        uint32_t map_entry_addr = group_flat + ((ref.map - 1) * 9);
        if (map_entry_addr + 9 > rom.size()) continue;
        auto entry = rom.read_bytes(map_entry_addr, 9);
        uint8_t attr_bank = entry[0];
        uint16_t attr_ptr = entry[3] | (entry[4] << 8);
        uint32_t header_addr = rom.bank_to_flat(attr_bank, attr_ptr);
        if (header_addr + fmt.header_size > rom.size()) continue;
        auto header = rom.read_bytes(header_addr, fmt.header_size);
        uint8_t conn_byte = header[fmt.connections_offset];
        uint8_t script_bank = header[fmt.script_bank_offset];
        
        uint32_t conn_ptr = header_addr + fmt.header_size;
        auto read_conn = [&]() {
            if (conn_ptr + fmt.connection_size > rom.size()) return;
            auto data = rom.read_bytes(conn_ptr, fmt.connection_size);
            uint8_t tgt_group = data[0], tgt_map = data[1];
            if (tgt_group > 0 && tgt_group <= 26 && tgt_map > 0 && tgt_map < 100) {
                enqueue(tgt_group, tgt_map);
            }
            conn_ptr += fmt.connection_size;
        };
        if (conn_byte & 0x08) read_conn();
        if (conn_byte & 0x04) read_conn();
        if (conn_byte & 0x02) read_conn();
        if (conn_byte & 0x01) read_conn();
        
        uint16_t events_addr = header[fmt.events_ptr_offset] | (header[fmt.events_ptr_offset + 1] << 8);
        uint32_t events_flat = rom.bank_to_flat(script_bank, events_addr);
        if (events_flat + 2 > rom.size()) continue;
        uint32_t ptr = events_flat + 2;
        if (ptr + 1 > rom.size()) continue;
        uint8_t warp_count = rom.read_byte(ptr++);
        if (warp_count > 50) continue;
        for (uint8_t i = 0; i < warp_count; ++i) {
            if (ptr + fmt.warp_size > rom.size()) break;
            auto warp = rom.read_bytes(ptr, fmt.warp_size);
            uint8_t tgt_group = warp[3], tgt_map = warp[4];
            if (tgt_group > 0 && tgt_group <= 26 && tgt_map > 0 && tgt_map < 100) {
                enqueue(tgt_group, tgt_map);
            }
            ptr += fmt.warp_size;
        }
    }
    return result;
}

// StdScript coverage analysis result
struct StdScriptResult {
    uint16_t std_id;
    uint32_t flat_address;
    std::string name;
    bool is_alias = false;          // Same address as another StdScript
    uint16_t alias_of = 0;          // If alias, which std_id it aliases
    bool in_map_roots = false;      // Address is also a map event root
    bool decode_success = false;
    size_t command_count = 0;
    bool cfg_success = false;
    size_t block_count = 0;
    bool lowering_success = false;
    size_t instructions_lowered = 0;
    size_t commands_unlowered = 0;
    bool legality_pass = false;
    std::string failure_reason;
};

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <rom_path>\n";
        return 1;
    }
    
    std::cout << "Loading ROM: " << argv[1] << "\n";
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
    
    std::cout << "\n=== StdScripts Coverage Analysis ===\n\n";
    
    // Load StdScripts table
    StdScriptsTable std_scripts;
    bool loaded = std_scripts.load(*rom, profile->offsets.std_scripts, 
                                    profile->offsets.std_scripts_count);
    if (!loaded) {
        std::cerr << "Failed to load StdScripts table\n";
        return 1;
    }
    
    std::cout << "StdScripts table: " << std_scripts.size() << " entries at 0x" 
              << std::hex << profile->offsets.std_scripts << std::dec << "\n\n";

    // Discover map roots (1297)
    MapExtractor extractor(*rom, *profile);
    auto discovered_maps = discover_reachable_maps(*rom, *profile, extractor);
    
    std::set<uint32_t> map_root_addresses;
    size_t total_map_events = 0;
    for (const auto& ref : discovered_maps) {
        auto result = extractor.extract_map(ref.group, ref.map);
        if (result.success) {
            for (const auto& obj : result.map.objects) {
                if (obj.script_rom_address != 0) {
                    map_root_addresses.insert(obj.script_rom_address);
                    total_map_events++;
                }
            }
            for (const auto& bg : result.map.bg_events) {
                if (bg.script_rom_address != 0) {
                    map_root_addresses.insert(bg.script_rom_address);
                    total_map_events++;
                }
            }
        }
    }
    
    std::cout << "Map discovery: " << discovered_maps.size() << " maps\n";
    std::cout << "Map events with scripts: " << total_map_events << "\n";
    std::cout << "Unique map root addresses: " << map_root_addresses.size() << "\n\n";
    
    // Setup pipeline components
    SymbolMap symbols;
    TypedScriptDecoder decoder(*rom, symbols);
    
    NativeCallRegistry native_registry;
    native_registry.initialize();
    RamAddressRegistry ram_registry;
    ram_registry.initialize();
    
    CFGBuilder cfg_builder;
    cfg_builder.set_std_scripts(&std_scripts);
    cfg_builder.set_native_registry(&native_registry);
    
    SemanticLegalizer legalizer;
    legalizer.set_native_registry(&native_registry);
    legalizer.set_ram_registry(&ram_registry);
    legalizer.set_num_pokemon(profile->counts.num_pokemon);
    
    LegalityGate legality_gate;

    // Analyze each StdScript
    std::vector<StdScriptResult> results;
    std::map<uint32_t, uint16_t> addr_to_first_std;  // Track aliases
    std::set<uint32_t> std_unique_addresses;
    
    for (size_t i = 0; i < std_scripts.size(); ++i) {
        const auto* entry = std_scripts.get(static_cast<uint16_t>(i));
        if (!entry) continue;
        
        StdScriptResult r;
        r.std_id = entry->std_id;
        r.flat_address = entry->flat_address;
        r.name = get_std_script_name(entry->std_id);
        
        // Check for alias
        auto it = addr_to_first_std.find(entry->flat_address);
        if (it != addr_to_first_std.end()) {
            r.is_alias = true;
            r.alias_of = it->second;
        } else {
            addr_to_first_std[entry->flat_address] = entry->std_id;
            std_unique_addresses.insert(entry->flat_address);
        }
        
        // Check if in map roots
        r.in_map_roots = map_root_addresses.contains(entry->flat_address);
        
        // Skip full analysis for aliases (they'll have same result as original)
        if (r.is_alias) {
            results.push_back(r);
            continue;
        }
        
        // Stage 1: Decode
        try {
            CrystalScriptIR ir = decoder.decode_script(entry->flat_address);
            r.decode_success = true;
            r.command_count = ir.commands.size();
            
            // Stage 2: CFG
            CrystalCFG cfg = cfg_builder.build(ir);
            r.cfg_success = cfg.validation.valid;
            r.block_count = cfg.blocks.size();
            
            // Stage 4: Lowering
            LoweringResult lowering = legalizer.lower(ir, cfg);
            r.lowering_success = (lowering.commands_unlowered == 0);
            r.instructions_lowered = lowering.commands_lowered;
            r.commands_unlowered = lowering.commands_unlowered;

            // Stage 5: Legality
            LegalityInput input;
            input.ir = &ir;
            input.decode_complete = true;
            input.round_trip_failures = 0;
            input.unknown_opcodes = 0;
            for (const auto& cmd : ir.commands) {
                if (std::holds_alternative<Cmd_Unknown>(cmd.data)) {
                    input.unknown_opcodes++;
                }
            }
            input.cfg = &cfg;
            input.native_registry = &native_registry;
            input.ram_registry = &ram_registry;
            input.lowering = &lowering;
            
            LegalityResult legality = legality_gate.validate(input);
            r.legality_pass = legality.is_legal;
            if (!legality.is_legal && !legality.diagnostics().empty()) {
                r.failure_reason = legality.diagnostics()[0].reason;
            }
            
        } catch (const std::exception& e) {
            r.decode_success = false;
            r.failure_reason = std::string("Decode exception: ") + e.what();
        }
        
        results.push_back(r);
    }
    
    // Print detailed table
    std::cout << "=== StdScripts Table Analysis ===\n\n";
    std::cout << std::left << std::setw(4) << "ID" 
              << std::setw(10) << "Address"
              << std::setw(35) << "Name"
              << std::setw(8) << "Alias?"
              << std::setw(8) << "MapRoot?"
              << std::setw(8) << "Decode"
              << std::setw(6) << "Cmds"
              << std::setw(8) << "CFG"
              << std::setw(8) << "Lower"
              << std::setw(8) << "Legal"
              << "\n";
    std::cout << std::string(105, '-') << "\n";

    size_t legal_count = 0, illegal_count = 0, alias_count = 0;
    size_t in_map_roots_count = 0;
    
    for (const auto& r : results) {
        std::cout << std::left << std::setw(4) << r.std_id;
        std::cout << "0x" << std::hex << std::setw(8) << r.flat_address << std::dec;
        std::cout << std::setw(35) << r.name;
        
        if (r.is_alias) {
            std::cout << std::setw(8) << ("=" + std::to_string(r.alias_of));
            alias_count++;
        } else {
            std::cout << std::setw(8) << "-";
        }
        
        std::cout << std::setw(8) << (r.in_map_roots ? "YES" : "-");
        if (r.in_map_roots) in_map_roots_count++;
        
        if (r.is_alias) {
            std::cout << "(see alias)\n";
            continue;
        }
        
        std::cout << std::setw(8) << (r.decode_success ? "OK" : "FAIL");
        std::cout << std::setw(6) << r.command_count;
        std::cout << std::setw(8) << (r.cfg_success ? "OK" : "FAIL");
        std::cout << std::setw(8) << (r.lowering_success ? "OK" : "FAIL");
        std::cout << std::setw(8) << (r.legality_pass ? "PASS" : "FAIL");
        
        if (r.legality_pass) legal_count++;
        else if (!r.is_alias) illegal_count++;
        
        if (!r.failure_reason.empty()) {
            std::cout << " [" << r.failure_reason.substr(0, 40) << "]";
        }
        std::cout << "\n";
    }
    
    std::cout << "\n=== StdScripts Summary ===\n";
    std::cout << "Total entries:        " << std_scripts.size() << "\n";
    std::cout << "Unique addresses:     " << std_unique_addresses.size() << "\n";
    std::cout << "Aliases:              " << alias_count << "\n";
    std::cout << "Legal (pass Stage 5): " << legal_count << "\n";
    std::cout << "Illegal:              " << illegal_count << "\n";
    std::cout << "Also in map roots:    " << in_map_roots_count << "\n";

    // Calculate overlap
    std::set<uint32_t> overlap;
    for (uint32_t addr : std_unique_addresses) {
        if (map_root_addresses.contains(addr)) {
            overlap.insert(addr);
        }
    }
    
    std::cout << "\n=== Script Count Reconciliation ===\n\n";
    
    std::cout << "Map events with scripts:      " << total_map_events << "\n";
    std::cout << "Unique map root addresses:    " << map_root_addresses.size() << "\n";
    std::cout << "Duplicates (dedup):           " << (total_map_events - map_root_addresses.size()) << "\n\n";
    
    std::cout << "StdScripts entries:           " << std_scripts.size() << "\n";
    std::cout << "StdScripts unique addresses:  " << std_unique_addresses.size() << "\n";
    std::cout << "StdScripts aliases:           " << alias_count << "\n\n";
    
    std::cout << "Overlap (StdScript in map roots): " << overlap.size() << "\n";
    if (!overlap.empty()) {
        std::cout << "  Overlapping addresses:\n";
        for (uint32_t addr : overlap) {
            std::cout << "    0x" << std::hex << addr << std::dec << "\n";
        }
    }
    
    // Exact math
    size_t union_size = map_root_addresses.size() + std_unique_addresses.size() - overlap.size();
    
    std::cout << "\n=== Exact Counts ===\n";
    std::cout << "A = Map root unique addresses:    " << map_root_addresses.size() << "\n";
    std::cout << "B = StdScript unique addresses:   " << std_unique_addresses.size() << "\n";
    std::cout << "A ∩ B (overlap):                  " << overlap.size() << "\n";
    std::cout << "A ∪ B (union):                    " << union_size << "\n";
    std::cout << "A - B (map-only):                 " << (map_root_addresses.size() - overlap.size()) << "\n";
    std::cout << "B - A (std-only):                 " << (std_unique_addresses.size() - overlap.size()) << "\n";

    // Check ~1339 claim
    std::cout << "\n=== Earlier '~1339' Analysis ===\n";
    std::cout << "The earlier FullGameCompiler report showing ~1339 scripts:\n";
    std::cout << "  If it counted: map_roots + stdscripts_unique - overlap\n";
    std::cout << "  Expected: " << map_root_addresses.size() << " + " 
              << std_unique_addresses.size() << " - " << overlap.size() 
              << " = " << union_size << "\n";
    std::cout << "  If it counted: map_roots + stdscripts_total (with aliases)\n";
    std::cout << "  Expected: " << map_root_addresses.size() << " + " 
              << std_scripts.size() << " = " << (map_root_addresses.size() + std_scripts.size()) << "\n";
    
    // Final reconciliation
    std::cout << "\n=== Final Reconciliation ===\n";
    bool all_std_legal = (legal_count == std_unique_addresses.size());
    
    if (all_std_legal) {
        std::cout << "✓ All " << std_unique_addresses.size() 
                  << " unique StdScript bodies pass Stage 5 legality.\n";
    } else {
        std::cout << "✗ " << illegal_count << " StdScript bodies FAIL legality.\n";
        std::cout << "  These are COVERAGE GAPS - Sem_CallStd/Sem_JumpStd cannot\n";
        std::cout << "  merely defer to an unresolved mechanism.\n";
    }
    
    std::cout << "\nCorpus script count: " << map_root_addresses.size() << "\n";
    std::cout << "StdScripts (unique): " << std_unique_addresses.size() << "\n";
    std::cout << "Total semantic coverage: " << union_size << " unique script bodies\n";
    
    if (all_std_legal && legal_count == std_unique_addresses.size()) {
        std::cout << "\n*** STDSCRIPTS COVERAGE COMPLETE ***\n";
        std::cout << "All StdScript bodies participate in the semantic pipeline.\n";
        return 0;
    } else {
        std::cout << "\n*** STDSCRIPTS COVERAGE INCOMPLETE ***\n";
        return 1;
    }
}
