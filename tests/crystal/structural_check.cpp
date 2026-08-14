// structural_check.cpp
// Structural compatibility analysis for field-move actor lifetime
// Reports CFG block boundaries and cross-block actor survival requirements

#include "crystal/rom/loader.hpp"
#include "crystal/rom/profile.hpp"
#include "crystal/rom/symbol_map.hpp"
#include "crystal/script/typed_decoder.hpp"
#include "crystal/script/crystal_cfg.hpp"
#include "crystal/script/native_registry.hpp"
#include "crystal/extract/map_extractor.hpp"
#include <iostream>
#include <iomanip>
#include <set>
#include <map>
#include <queue>

using namespace crystal;

// Native addresses we're tracking
constexpr uint32_t ADDR_TryStrengthOW = 0xCD78;
constexpr uint32_t ADDR_SetStrengthFlag = 0xCD12;
constexpr uint32_t ADDR_HasRockSmash = 0xCF7C;
constexpr uint32_t ADDR_GetPartyNickname = 0xC706;
constexpr uint32_t ADDR_RockMonEncounter = 0xB8219;

// RAM addresses we're tracking
constexpr uint16_t RAM_wCurPartyMon = 0xd109;
constexpr uint16_t RAM_wStrengthSpecies = 0xd1ef;
constexpr uint16_t RAM_wTempWildMonSpecies = 0xd22e;

struct MapIdRef {
    uint8_t group;
    uint8_t map;
    bool operator<(const MapIdRef& other) const {
        if (group != other.group) return group < other.group;
        return map < other.map;
    }
};

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
    
    enqueue(24, 4); enqueue(24, 7);
    
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

struct NativeOccurrence {
    uint32_t script_address;
    uint32_t native_address;
    size_t block_id;
    size_t cmd_index;
    std::string script_name;
};

struct RamOccurrence {
    uint32_t script_address;
    uint16_t ram_address;
    size_t block_id;
    size_t cmd_index;
    std::string op_type;  // readmem, writemem, loadmem
    std::string script_name;
};

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <rom_path>\n";
        return 1;
    }
    
    std::cout << "Loading ROM: " << argv[1] << "\n";
    auto rom = RomData::load(argv[1]);
    if (!rom) { std::cerr << "Failed to load ROM\n"; return 1; }
    
    auto& registry = ProfileRegistry::instance();
    auto profile = registry.get_profile_by_hash(rom->hash());
    if (!profile) { std::cerr << "ROM not supported\n"; return 1; }
    
    std::cout << "\n=== Structural Compatibility Check ===\n\n";

    // Load StdScripts table
    StdScriptsTable std_scripts;
    std_scripts.load(*rom, profile->offsets.std_scripts, profile->offsets.std_scripts_count);
    
    // Discover map roots
    MapExtractor extractor(*rom, *profile);
    auto discovered_maps = discover_reachable_maps(*rom, *profile, extractor);
    
    std::set<uint32_t> all_script_addresses;
    for (const auto& ref : discovered_maps) {
        auto result = extractor.extract_map(ref.group, ref.map);
        if (result.success) {
            for (const auto& obj : result.map.objects) {
                if (obj.script_rom_address != 0)
                    all_script_addresses.insert(obj.script_rom_address);
            }
            for (const auto& bg : result.map.bg_events) {
                if (bg.script_rom_address != 0)
                    all_script_addresses.insert(bg.script_rom_address);
            }
        }
    }
    for (size_t i = 0; i < std_scripts.size(); ++i) {
        if (auto* e = std_scripts.get(static_cast<uint16_t>(i)))
            all_script_addresses.insert(e->flat_address);
    }
    
    std::cout << "Total unique script addresses: " << all_script_addresses.size() << "\n\n";
    
    // Setup pipeline
    SymbolMap symbols;
    TypedScriptDecoder decoder(*rom, symbols);
    NativeCallRegistry native_registry;
    native_registry.initialize();
    CFGBuilder cfg_builder;
    cfg_builder.set_std_scripts(&std_scripts);
    cfg_builder.set_native_registry(&native_registry);
    
    // Track occurrences
    std::vector<NativeOccurrence> native_occs;
    std::vector<RamOccurrence> ram_occs;

    // Scan all 1349 scripts
    for (uint32_t script_addr : all_script_addresses) {
        try {
            CrystalScriptIR ir = decoder.decode_script(script_addr);
            CrystalCFG cfg = cfg_builder.build(ir);
            
            std::string script_name = "script_0x" + 
                ([&]{ std::ostringstream ss; ss << std::hex << script_addr; return ss.str(); })();
            
            for (const auto& block : cfg.blocks) {
                for (size_t i = 0; i < block.command_count; ++i) {
                    size_t cmd_idx = block.command_start + i;
                    if (cmd_idx >= ir.commands.size()) break;
                    const auto& cmd = ir.commands[cmd_idx];
                    
                    // Check for callasm
                    if (auto* p = std::get_if<Cmd_Callasm>(&cmd.data)) {
                        NativeOccurrence occ;
                        occ.script_address = script_addr;
                        occ.native_address = p->flat_address;
                        occ.block_id = block.id;
                        occ.cmd_index = i;
                        occ.script_name = script_name;
                        native_occs.push_back(occ);
                    }
                    
                    // Check for readmem
                    if (auto* p = std::get_if<Cmd_Readmem>(&cmd.data)) {
                        RamOccurrence occ;
                        occ.script_address = script_addr;
                        occ.ram_address = p->ram_address;
                        occ.block_id = block.id;
                        occ.cmd_index = i;
                        occ.op_type = "readmem";
                        occ.script_name = script_name;
                        ram_occs.push_back(occ);
                    }
                    if (auto* p = std::get_if<Cmd_Writemem>(&cmd.data)) {
                        RamOccurrence occ;
                        occ.script_address = script_addr;
                        occ.ram_address = p->ram_address;
                        occ.block_id = block.id;
                        occ.cmd_index = i;
                        occ.op_type = "writemem";
                        occ.script_name = script_name;
                        ram_occs.push_back(occ);
                    }
                    if (auto* p = std::get_if<Cmd_Loadmem>(&cmd.data)) {
                        RamOccurrence occ;
                        occ.script_address = script_addr;
                        occ.ram_address = p->ram_address;
                        occ.block_id = block.id;
                        occ.cmd_index = i;
                        occ.op_type = "loadmem";
                        occ.script_name = script_name;
                        ram_occs.push_back(occ);
                    }
                }
            }
        } catch (...) {
            // Skip failed decodes
        }
    }
    
    // === SECTION 1: Actor Lifetime vs CFG ===
    std::cout << "=== 1. Actor Lifetime vs CFG ===\n\n";
    
    // Find AskStrengthScript address (via farsjump from StdScript 14)
    uint32_t ask_strength_addr = 0;
    uint32_t ask_rocksmash_addr = 0;
    
    // Decode StdScript 14 to find its farsjump target
    if (auto* e = std_scripts.get(14)) {
        try {
            CrystalScriptIR ir = decoder.decode_script(e->flat_address);
            for (const auto& cmd : ir.commands) {
                if (auto* p = std::get_if<Cmd_Farsjump>(&cmd.data)) {
                    ask_strength_addr = p->target.rom_address;
                    break;
                }
            }
        } catch (...) {}
    }
    if (auto* e = std_scripts.get(15)) {
        try {
            CrystalScriptIR ir = decoder.decode_script(e->flat_address);
            for (const auto& cmd : ir.commands) {
                if (auto* p = std::get_if<Cmd_Farsjump>(&cmd.data)) {
                    ask_rocksmash_addr = p->target.rom_address;
                    break;
                }
            }
        } catch (...) {}
    }
    
    std::cout << "AskStrengthScript address: 0x" << std::hex << ask_strength_addr << std::dec << "\n";
    std::cout << "AskRockSmashScript address: 0x" << std::hex << ask_rocksmash_addr << std::dec << "\n\n";

    // Analyze AskStrengthScript CFG
    if (ask_strength_addr != 0) {
        std::cout << "--- AskStrengthScript CFG Analysis ---\n";
        try {
            CrystalScriptIR ir = decoder.decode_script(ask_strength_addr);
            CrystalCFG cfg = cfg_builder.build(ir);
            
            std::cout << "Blocks: " << cfg.blocks.size() << "\n\n";
            
            for (const auto& block : cfg.blocks) {
                std::cout << "Block " << block.id << " [" << block.label << "]";
                if (block.is_entry) std::cout << " (ENTRY)";
                std::cout << ":\n";
                
                for (size_t i = 0; i < block.command_count; ++i) {
                    size_t cmd_idx = block.command_start + i;
                    if (cmd_idx >= ir.commands.size()) break;
                    const auto& cmd = ir.commands[cmd_idx];
                    
                    std::cout << "  [" << i << "] opcode=0x" << std::hex 
                              << (int)cmd.opcode() << std::dec << " ";
                    
                    // Print relevant info
                    if (auto* p = std::get_if<Cmd_Callasm>(&cmd.data)) {
                        std::cout << "callasm 0x" << std::hex << p->flat_address << std::dec;
                        if (p->flat_address == ADDR_TryStrengthOW) std::cout << " <-- TryStrengthOW";
                        if (p->flat_address == ADDR_SetStrengthFlag) std::cout << " <-- SetStrengthFlag";
                        if (p->flat_address == ADDR_GetPartyNickname) std::cout << " <-- GetPartyNickname";
                    } else if (std::holds_alternative<Cmd_Iffalse>(cmd.data)) {
                        std::cout << "iffalse";
                    } else if (auto* p = std::get_if<Cmd_Ifequal>(&cmd.data)) {
                        std::cout << "ifequal " << (int)p->value;
                    } else if (std::holds_alternative<Cmd_Yesorno>(cmd.data)) {
                        std::cout << "yesorno <-- USER INTERACTION";
                    } else if (std::holds_alternative<Cmd_Iftrue>(cmd.data)) {
                        std::cout << "iftrue";
                    } else if (auto* p = std::get_if<Cmd_Readmem>(&cmd.data)) {
                        std::cout << "readmem 0x" << std::hex << p->ram_address << std::dec;
                        if (p->ram_address == RAM_wStrengthSpecies) std::cout << " <-- wStrengthSpecies";
                    } else if (std::holds_alternative<Cmd_End>(cmd.data)) {
                        std::cout << "end";
                    } else if (std::holds_alternative<Cmd_Sjump>(cmd.data)) {
                        std::cout << "sjump";
                    } else {
                        std::cout << "(other)";
                    }
                    std::cout << "\n";
                }
                std::cout << "\n";
            }
        } catch (const std::exception& e) {
            std::cout << "Error: " << e.what() << "\n";
        }
    }

    // Analyze AskRockSmashScript CFG
    if (ask_rocksmash_addr != 0) {
        std::cout << "\n--- AskRockSmashScript CFG Analysis ---\n";
        try {
            CrystalScriptIR ir = decoder.decode_script(ask_rocksmash_addr);
            CrystalCFG cfg = cfg_builder.build(ir);
            
            std::cout << "Blocks: " << cfg.blocks.size() << "\n\n";
            
            for (const auto& block : cfg.blocks) {
                std::cout << "Block " << block.id << " [" << block.label << "]";
                if (block.is_entry) std::cout << " (ENTRY)";
                std::cout << ":\n";
                
                for (size_t i = 0; i < block.command_count; ++i) {
                    size_t cmd_idx = block.command_start + i;
                    if (cmd_idx >= ir.commands.size()) break;
                    const auto& cmd = ir.commands[cmd_idx];
                    
                    std::cout << "  [" << i << "] opcode=0x" << std::hex 
                              << (int)cmd.opcode() << std::dec << " ";
                    
                    if (auto* p = std::get_if<Cmd_Callasm>(&cmd.data)) {
                        std::cout << "callasm 0x" << std::hex << p->flat_address << std::dec;
                        if (p->flat_address == ADDR_HasRockSmash) std::cout << " <-- HasRockSmash";
                        if (p->flat_address == ADDR_GetPartyNickname) std::cout << " <-- GetPartyNickname";
                        if (p->flat_address == ADDR_RockMonEncounter) std::cout << " <-- RockMonEncounter";
                    } else if (std::holds_alternative<Cmd_Iffalse>(cmd.data)) {
                        std::cout << "iffalse";
                    } else if (auto* p = std::get_if<Cmd_Ifequal>(&cmd.data)) {
                        std::cout << "ifequal " << (int)p->value;
                    } else if (std::holds_alternative<Cmd_Yesorno>(cmd.data)) {
                        std::cout << "yesorno <-- USER INTERACTION";
                    } else if (std::holds_alternative<Cmd_Iftrue>(cmd.data)) {
                        std::cout << "iftrue";
                    } else if (auto* p = std::get_if<Cmd_Readmem>(&cmd.data)) {
                        std::cout << "readmem 0x" << std::hex << p->ram_address << std::dec;
                        if (p->ram_address == RAM_wTempWildMonSpecies) std::cout << " <-- wTempWildMonSpecies";
                    } else if (std::holds_alternative<Cmd_End>(cmd.data)) {
                        std::cout << "end";
                    } else if (std::holds_alternative<Cmd_Sjump>(cmd.data)) {
                        std::cout << "sjump";
                    } else {
                        std::cout << "(other)";
                    }
                    std::cout << "\n";
                }
                std::cout << "\n";
            }
        } catch (const std::exception& e) {
            std::cout << "Error: " << e.what() << "\n";
        }
    }

    // === SECTION 2: Block-local fusion legality for RockSmash ===
    std::cout << "\n=== 2. Block-local Fusion Legality (RockSmash) ===\n\n";
    
    // Find RockSmashScript (the actual smash script, not AskRockSmash)
    // RockSmashScript is referenced by iftrue from AskRockSmashScript
    // We need to trace through to find where RockMonEncounter + readmem pattern occurs
    
    std::cout << "Looking for RockMonEncounter + readmem wTempWildMonSpecies pattern...\n\n";
    
    for (const auto& occ : native_occs) {
        if (occ.native_address == ADDR_RockMonEncounter) {
            std::cout << "Found RockMonEncounter in script 0x" << std::hex 
                      << occ.script_address << std::dec 
                      << " block " << occ.block_id << " cmd " << occ.cmd_index << "\n";
            
            // Decode and show the surrounding context
            try {
                CrystalScriptIR ir = decoder.decode_script(occ.script_address);
                CrystalCFG cfg = cfg_builder.build(ir);
                
                for (const auto& block : cfg.blocks) {
                    if (block.id == occ.block_id) {
                        std::cout << "  Block " << block.id << " commands:\n";
                        for (size_t i = 0; i < block.command_count; ++i) {
                            size_t cmd_idx = block.command_start + i;
                            if (cmd_idx >= ir.commands.size()) break;
                            const auto& cmd = ir.commands[cmd_idx];
                            
                            std::cout << "    [" << i << "] ";
                            if (auto* p = std::get_if<Cmd_Callasm>(&cmd.data)) {
                                std::cout << "callasm 0x" << std::hex << p->flat_address << std::dec;
                            } else if (auto* p = std::get_if<Cmd_Readmem>(&cmd.data)) {
                                std::cout << "readmem 0x" << std::hex << p->ram_address << std::dec;
                            } else if (std::holds_alternative<Cmd_Iffalse>(cmd.data)) {
                                std::cout << "iffalse -> BRANCH";
                            } else {
                                std::cout << "opcode 0x" << std::hex << (int)cmd.opcode() << std::dec;
                            }
                            std::cout << "\n";
                        }
                        break;
                    }
                }
            } catch (...) {}
            std::cout << "\n";
        }
    }

    // === SECTION 3: wCurPartyMon Stage-3 status ===
    std::cout << "\n=== 3. wCurPartyMon Stage-3 Status ===\n\n";
    
    RamAddressRegistry ram_registry;
    ram_registry.initialize();
    
    auto* entry = ram_registry.get(RAM_wCurPartyMon);
    if (entry) {
        std::cout << "wCurPartyMon (0x" << std::hex << RAM_wCurPartyMon << std::dec << "):\n";
        std::cout << "  Symbol: " << entry->symbol_name << "\n";
        std::cout << "  Semantic: " << entry->semantic_meaning << "\n";
        std::cout << "  Classification: " << static_cast<int>(entry->classification) << "\n";
    } else {
        std::cout << "wCurPartyMon NOT in registry\n";
    }
    
    // Count accesses to wCurPartyMon in corpus
    size_t curpartymmon_accesses = 0;
    for (const auto& occ : ram_occs) {
        if (occ.ram_address == RAM_wCurPartyMon) {
            curpartymmon_accesses++;
            std::cout << "  Access: " << occ.op_type << " in script 0x" 
                      << std::hex << occ.script_address << std::dec 
                      << " block " << occ.block_id << "\n";
        }
    }
    std::cout << "Total wCurPartyMon accesses in 1349 corpus: " << curpartymmon_accesses << "\n\n";
    
    // Check wStrengthSpecies and wTempWildMonSpecies
    std::cout << "wStrengthSpecies (0x" << std::hex << RAM_wStrengthSpecies << std::dec << "): ";
    if (ram_registry.get(RAM_wStrengthSpecies)) std::cout << "IN registry\n";
    else std::cout << "NOT in registry\n";
    
    std::cout << "wTempWildMonSpecies (0x" << std::hex << RAM_wTempWildMonSpecies << std::dec << "): ";
    if (ram_registry.get(RAM_wTempWildMonSpecies)) std::cout << "IN registry\n";
    else std::cout << "NOT in registry\n";
    
    // Find all unique RAM addresses in corpus
    std::set<uint16_t> all_ram_addrs;
    for (const auto& occ : ram_occs) {
        all_ram_addrs.insert(occ.ram_address);
    }
    
    std::cout << "\nAll RAM addresses accessed in 1349 corpus: " << all_ram_addrs.size() << "\n";
    std::cout << "Unclassified RAM addresses:\n";
    for (uint16_t addr : all_ram_addrs) {
        if (!ram_registry.is_classified(addr)) {
            std::cout << "  0x" << std::hex << addr << std::dec << "\n";
        }
    }

    // === SECTION 4: Corpus-wide native reuse ===
    std::cout << "\n=== 4. Corpus-wide Native Reuse ===\n\n";
    
    std::map<uint32_t, std::vector<NativeOccurrence>> by_native;
    for (const auto& occ : native_occs) {
        by_native[occ.native_address].push_back(occ);
    }
    
    auto print_native = [&](uint32_t addr, const char* name) {
        std::cout << name << " (0x" << std::hex << addr << std::dec << "): ";
        auto it = by_native.find(addr);
        if (it == by_native.end() || it->second.empty()) {
            std::cout << "0 occurrences\n";
        } else {
            std::cout << it->second.size() << " occurrences\n";
            for (const auto& occ : it->second) {
                std::cout << "  - script 0x" << std::hex << occ.script_address << std::dec
                          << " block " << occ.block_id << "\n";
            }
        }
    };
    
    print_native(ADDR_TryStrengthOW, "TryStrengthOW");
    print_native(ADDR_SetStrengthFlag, "SetStrengthFlag");
    print_native(ADDR_HasRockSmash, "HasRockSmash");
    print_native(ADDR_GetPartyNickname, "GetPartyNickname");
    print_native(ADDR_RockMonEncounter, "RockMonEncounter");
    
    // Look for other field-move related natives that share the actor/nickname pattern
    std::cout << "\n--- Searching for other GetPartyNickname-like patterns ---\n";
    
    // Find all unique native addresses
    std::set<uint32_t> all_native_addrs;
    for (const auto& occ : native_occs) {
        all_native_addrs.insert(occ.native_address);
    }
    
    std::cout << "Total unique callasm targets: " << all_native_addrs.size() << "\n";
    std::cout << "Known field-move natives in corpus:\n";
    
    for (uint32_t addr : all_native_addrs) {
        auto* e = native_registry.get(addr);
        if (e && e->is_classified()) {
            std::cout << "  0x" << std::hex << addr << std::dec 
                      << " " << e->symbol_name << " (" << by_native[addr].size() << " uses)\n";
        }
    }
    
    std::cout << "\n=== Structural Check Complete ===\n";
    return 0;
}
