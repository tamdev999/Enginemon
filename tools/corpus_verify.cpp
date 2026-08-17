// tools/corpus_verify.cpp
// Expanded corpus verification tool for scene script discovery

#include "crystal/rom/loader.hpp"
#include "crystal/rom/profile.hpp"
#include "crystal/script/typed_decoder.hpp"
#include "crystal/script/crystal_cfg.hpp"
#include "crystal/script/semantic_legalizer.hpp"
#include "crystal/script/native_registry.hpp"
#include "crystal/script/legality_gate.hpp"
#include "crystal/script/elevator_registry.hpp"
#include "crystal/script/pokemail_registry.hpp"
#include "crystal/script/text_registry.hpp"
#include "crystal/script/trainer_registry.hpp"
#include "crystal/extract/map_extractor.hpp"
#include "crystal/compile/full_compiler.hpp"
#include "engine/scripting/semantic_ir.hpp"
#include <iostream>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <map>
#include <set>
#include <algorithm>

using namespace crystal;
using namespace enginemon;

// Track Special occurrences
struct SpecialOccurrence {
    uint8_t special_id;
    std::string script_id;
    uint32_t rom_address;
    std::string root_type;
    std::string map_name;
};

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <rom.gbc>\n";
        return 1;
    }

    std::cout << "=== Expanded Corpus Verification Tool ===\n\n";

    // Load ROM
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

    // Create pipeline components
    TypedScriptDecoder decoder(*rom, *profile);
    CFGBuilder cfg_builder;
    NativeCallRegistry native_registry;
    ElevatorRegistry elevator_registry;
    PokeMailRegistry pokemail_registry;
    TextRegistry text_registry;
    SemanticLegalizer legalizer;
    legalizer.set_native_registry(&native_registry);
    legalizer.set_elevator_registry(&elevator_registry);
    legalizer.set_pokemail_registry(&pokemail_registry);
    legalizer.set_text_registry(&text_registry);
    legalizer.set_num_pokemon(profile->counts.num_pokemon);

    // Discover all maps
    MapExtractor map_extractor(*rom, *profile);
    std::vector<ExtractedMap> all_maps;

    for (int group = 1; group <= 26; ++group) {
        for (int index = 1; index <= 100; ++index) {
            auto map = map_extractor.extract_map(group, index);
            if (map) {
                all_maps.push_back(*map);
            }
        }
    }

    std::cout << "Maps discovered: " << all_maps.size() << "\n\n";

    // Collect all script addresses with metadata
    struct ScriptRoot {
        uint32_t address;
        std::string root_type;
        std::string map_name;
        std::string source_label;
    };

    std::map<uint32_t, ScriptRoot> script_roots;
    size_t object_count = 0, bg_count = 0, scene_count = 0, callback_count = 0;

    for (const auto& map : all_maps) {
        // Object scripts
        for (size_t i = 0; i < map.objects.size(); ++i) {
            uint32_t addr = map.objects[i].script_rom_address;
            if (addr != 0 && addr != 0xFFFFFFFF && addr < rom->data.size()) {
                std::ostringstream label;
                label << map.name << "::object_" << i;
                if (!script_roots.count(addr)) {
                    script_roots[addr] = {addr, "object", map.name, label.str()};
                    object_count++;
                }
            }
        }

        // BG event scripts
        for (size_t i = 0; i < map.bg_events.size(); ++i) {
            uint32_t addr = map.bg_events[i].script_rom_address;
            if (addr != 0 && addr != 0xFFFFFFFF && addr < rom->data.size()) {
                std::ostringstream label;
                label << map.name << "::bg_event_" << i;
                if (!script_roots.count(addr)) {
                    script_roots[addr] = {addr, "bg_event", map.name, label.str()};
                    bg_count++;
                }
            }
        }

        // Scene scripts - read from MapScripts header
        uint32_t map_scripts_ptr = map.header_rom_address + 9;
        if (map_scripts_ptr + 2 <= rom->data.size()) {
            uint16_t scripts_ptr = rom->data[map_scripts_ptr] | (rom->data[map_scripts_ptr + 1] << 8);
            uint8_t script_bank = map.script_bank;
            uint32_t scripts_addr = (script_bank * 0x4000) + (scripts_ptr - 0x4000);

            if (scripts_addr < rom->data.size()) {
                uint8_t scene_script_count = rom->data[scripts_addr];
                uint32_t pos = scripts_addr + 1;

                for (uint8_t s = 0; s < scene_script_count && pos + 4 <= rom->data.size(); ++s) {
                    uint16_t scene_ptr = rom->data[pos] | (rom->data[pos + 1] << 8);
                    uint32_t scene_addr = (script_bank * 0x4000) + (scene_ptr - 0x4000);
                    
                    if (scene_addr < rom->data.size()) {
                        std::ostringstream label;
                        label << map.name << "::scene_" << (int)s;
                        if (!script_roots.count(scene_addr)) {
                            script_roots[scene_addr] = {scene_addr, "scene", map.name, label.str()};
                            scene_count++;
                        }
                    }
                    pos += 4;  // SCENE_SCRIPT_SIZE = 4
                }

                // Callbacks
                if (pos < rom->data.size()) {
                    uint8_t callback_cnt = rom->data[pos];
                    pos++;

                    for (uint8_t c = 0; c < callback_cnt && pos + 3 <= rom->data.size(); ++c) {
                        uint8_t cb_type = rom->data[pos];
                        uint16_t cb_ptr = rom->data[pos + 1] | (rom->data[pos + 2] << 8);
                        uint32_t cb_addr = (script_bank * 0x4000) + (cb_ptr - 0x4000);

                        if (cb_addr < rom->data.size()) {
                            std::ostringstream label;
                            label << map.name << "::callback_" << (int)c << "_type" << (int)cb_type;
                            if (!script_roots.count(cb_addr)) {
                                script_roots[cb_addr] = {cb_addr, "callback", map.name, label.str()};
                                callback_count++;
                            }
                        }
                        pos += 3;  // CALLBACK_SIZE = 3
                    }
                }
            }
        }
    }

    // StdScripts
    uint32_t std_table = profile->std_scripts_table;
    size_t std_count = 0;
    for (int i = 0; i < 52; ++i) {
        uint32_t entry = std_table + i * 3;
        if (entry + 3 > rom->data.size()) break;
        uint8_t bank = rom->data[entry];
        uint16_t ptr = rom->data[entry + 1] | (rom->data[entry + 2] << 8);
        uint32_t addr = (bank * 0x4000) + (ptr - 0x4000);

        std::ostringstream label;
        label << "StdScript_" << i;
        if (!script_roots.count(addr)) {
            script_roots[addr] = {addr, "stdscript", "N/A", label.str()};
            std_count++;
        }
    }

    std::cout << "=== Script Root Summary ===\n";
    std::cout << "  Object scripts:   " << object_count << "\n";
    std::cout << "  BG event scripts: " << bg_count << "\n";
    std::cout << "  Scene scripts:    " << scene_count << "\n";
    std::cout << "  Callback scripts: " << callback_count << "\n";
    std::cout << "  StdScripts:       " << std_count << "\n";
    std::cout << "  Total unique:     " << script_roots.size() << "\n\n";

    // Process all scripts and collect data
    std::vector<SpecialOccurrence> all_specials;
    std::map<uint8_t, int> special_counts;
    
    // CFG integrity tracking
    size_t duplicate_addresses = 0;
    size_t invalid_branch_targets = 0;
    size_t orphan_commands = 0;
    size_t overlapping_blocks = 0;
    size_t total_commands = 0;
    size_t total_blocks = 0;
    size_t processed_bodies = 0;
    size_t failed_decode = 0;
    size_t failed_cfg = 0;

    // Command queue and sdefer tracking
    int writecmdqueue_count = 0, delcmdqueue_count = 0, sdefer_count = 0;
    std::vector<std::string> writecmdqueue_sites;
    std::vector<std::string> delcmdqueue_sites;
    std::vector<std::string> sdefer_sites;

    std::cout << "Processing " << script_roots.size() << " script bodies...\n";

    for (const auto& [addr, root] : script_roots) {
        // Decode
        auto decode_result = decoder.decode(addr, root.source_label);
        if (!decode_result.success) {
            failed_decode++;
            continue;
        }

        // Build CFG
        auto cfg = cfg_builder.build(decode_result.ir, root.source_label);
        if (!cfg.valid) {
            failed_cfg++;
            continue;
        }

        processed_bodies++;
        total_commands += decode_result.ir.commands.size();
        total_blocks += cfg.blocks.size();

        // Check CFG integrity - duplicate addresses
        std::set<uint32_t> seen_addrs;
        for (const auto& cmd : decode_result.ir.commands) {
            if (seen_addrs.count(cmd.span.rom_address)) {
                duplicate_addresses++;
            }
            seen_addrs.insert(cmd.span.rom_address);
        }

        // Verify branch targets are block leaders
        std::set<uint32_t> block_starts;
        for (const auto& block : cfg.blocks) {
            block_starts.insert(block.start_address);
        }
        for (const auto& block : cfg.blocks) {
            for (const auto& cmd : decode_result.ir.commands) {
                if (cmd.span.rom_address >= block.start_address && 
                    cmd.span.rom_address < block.start_address + block.command_count) {
                    // Check jumps
                    if (auto* sjump = std::get_if<Cmd_Sjump>(&cmd.data)) {
                        if (!block_starts.count(sjump->target.rom_address) &&
                            sjump->target.rom_address != 0) {
                            invalid_branch_targets++;
                        }
                    }
                }
            }
        }

        // Check all commands belong to exactly one block
        std::set<size_t> covered_cmds;
        for (const auto& block : cfg.blocks) {
            for (size_t i = block.command_start; i < block.command_start + block.command_count; ++i) {
                if (covered_cmds.count(i)) {
                    overlapping_blocks++;
                }
                covered_cmds.insert(i);
            }
        }
        if (covered_cmds.size() != decode_result.ir.commands.size()) {
            orphan_commands += (decode_result.ir.commands.size() - covered_cmds.size());
        }

        // Lower and collect Specials and command queue ops
        auto lower_result = legalizer.lower(decode_result.ir, cfg);

        for (const auto& block : lower_result.ir.blocks) {
            for (const auto& inst : block.instructions) {
                if (auto* special = std::get_if<Sem_Special>(&inst.op)) {
                    special_counts[special->special_id]++;

                    SpecialOccurrence occ;
                    occ.special_id = special->special_id;
                    occ.script_id = root.source_label;
                    occ.rom_address = addr;
                    occ.root_type = root.root_type;
                    occ.map_name = root.map_name;
                    all_specials.push_back(occ);
                }
                if (std::holds_alternative<Sem_WriteCmdQueue>(inst.op)) {
                    writecmdqueue_count++;
                    auto& op = std::get<Sem_WriteCmdQueue>(inst.op);
                    std::ostringstream ss;
                    ss << root.source_label << " @ 0x" << std::hex << addr 
                       << " queue_ptr=0x" << op.queue_pointer << " [" << root.root_type << "]";
                    writecmdqueue_sites.push_back(ss.str());
                }
                if (std::holds_alternative<Sem_DeleteCmdQueue>(inst.op)) {
                    delcmdqueue_count++;
                    auto& op = std::get<Sem_DeleteCmdQueue>(inst.op);
                    std::ostringstream ss;
                    ss << root.source_label << " @ 0x" << std::hex << addr 
                       << " type=" << std::dec << (int)op.queue_type << " [" << root.root_type << "]";
                    delcmdqueue_sites.push_back(ss.str());
                }
                if (auto* sdef = std::get_if<Sem_Sdefer>(&inst.op)) {
                    sdefer_count++;
                    std::ostringstream ss;
                    ss << root.source_label << " @ 0x" << std::hex << addr 
                       << " -> " << sdef->target_script_id << " [" << root.root_type << "]";
                    sdefer_sites.push_back(ss.str());
                }
            }
        }
    }

    std::cout << "Processed: " << processed_bodies << " / " << script_roots.size() << " bodies\n";
    std::cout << "Failed decode: " << failed_decode << ", Failed CFG: " << failed_cfg << "\n\n";

    // === CFG INTEGRITY REPORT ===
    std::cout << "=== CFG/Decoder Integrity ===\n";
    std::cout << "  Total commands:          " << total_commands << "\n";
    std::cout << "  Total blocks:            " << total_blocks << "\n";
    std::cout << "  Duplicate addresses:     " << duplicate_addresses << "\n";
    std::cout << "  Invalid branch targets:  " << invalid_branch_targets << "\n";
    std::cout << "  Orphan commands:         " << orphan_commands << "\n";
    std::cout << "  Overlapping blocks:      " << overlapping_blocks << "\n";
    if (duplicate_addresses == 0 && orphan_commands == 0 && overlapping_blocks == 0) {
        std::cout << "  *** CFG INTEGRITY: PASS ***\n";
    } else {
        std::cout << "  *** CFG INTEGRITY: FAIL ***\n";
    }
    std::cout << "\n";

    // === SPECIAL 152 DETAILED REPORT ===
    std::cout << "=== Special 152 (SetPlayerPalette) Analysis ===\n";
    int special_152_count = 0;
    for (const auto& occ : all_specials) {
        if (occ.special_id == 152) {
            special_152_count++;
            std::cout << "  Occurrence " << special_152_count << ":\n";
            std::cout << "    Script ID:   " << occ.script_id << "\n";
            std::cout << "    ROM Address: 0x" << std::hex << occ.rom_address << std::dec << "\n";
            std::cout << "    Root Type:   " << occ.root_type << "\n";
            std::cout << "    Map:         " << occ.map_name << "\n\n";
        }
    }
    std::cout << "  Total Special 152 occurrences: " << special_152_count << "\n\n";

    // === SEM_SPECIAL INVENTORY ===
    std::cout << "=== Sem_Special Inventory (Post-Batch-8 Baseline) ===\n";
    std::cout << "  Unique Special IDs: " << special_counts.size() << "\n";
    int total_occurrences = 0;
    for (const auto& [id, count] : special_counts) {
        total_occurrences += count;
    }
    std::cout << "  Total occurrences:  " << total_occurrences << "\n\n";

    std::cout << "  ID  | Count\n";
    std::cout << "  ----|------\n";
    for (const auto& [id, count] : special_counts) {
        std::cout << "  " << std::setw(3) << (int)id << " | " << std::setw(5) << count << "\n";
    }

    // === COMMAND QUEUE OPS ===
    std::cout << "\n=== Sem_WriteCmdQueue Occurrences ===\n";
    for (const auto& site : writecmdqueue_sites) {
        std::cout << "  " << site << "\n";
    }
    std::cout << "  Total: " << writecmdqueue_count << "\n";

    std::cout << "\n=== Sem_DeleteCmdQueue Occurrences ===\n";
    for (const auto& site : delcmdqueue_sites) {
        std::cout << "  " << site << "\n";
    }
    std::cout << "  Total: " << delcmdqueue_count << "\n";

    // === SDEFER ANALYSIS ===
    std::cout << "\n=== Sem_Sdefer Occurrences ===\n";
    for (const auto& site : sdefer_sites) {
        std::cout << "  " << site << "\n";
    }
    std::cout << "  Total: " << sdefer_count << "\n";

    std::cout << "\n=== VERIFICATION SUMMARY ===\n";
    std::cout << "Authoritative corpus size: " << processed_bodies << " bodies\n";
    std::cout << "Sem_Special inventory: " << special_counts.size() << " unique IDs, " 
              << total_occurrences << " total occurrences\n";
    std::cout << "Special 152 (SetPlayerPalette): " << special_152_count << " occurrences\n";
    std::cout << "WriteCmdQueue: " << writecmdqueue_count << " occurrences\n";
    std::cout << "DeleteCmdQueue: " << delcmdqueue_count << " occurrences\n";
    std::cout << "Sdefer: " << sdefer_count << " occurrences\n";

    return 0;
}
