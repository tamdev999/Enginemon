// cfg_integrity_check.cpp - Verify CFG integrity across all 1362 bodies
// Checks:
// 1. Every branch target resolves to a CFG block leader
// 2. Every CrystalCommand belongs to exactly one CFG block
// 3. No CFG block overlaps another command range
// 4. All back-edges remain present (loop targets exist)

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

struct CFGIntegrityResult {
    bool all_targets_are_block_leaders = true;
    bool no_overlapping_blocks = true;
    bool all_commands_in_one_block = true;
    bool all_back_edges_preserved = true;
    
    std::vector<std::string> errors;
};

CFGIntegrityResult check_cfg_integrity(const CrystalScriptIR& ir, const CrystalCFG& cfg, 
                                        const std::string& body_name) {
    CFGIntegrityResult result;
    
    // Build set of block entry addresses
    std::set<uint32_t> block_entries;
    for (const auto& block : cfg.blocks) {
        block_entries.insert(block.start_address);
    }
    
    // 1. Check all branch targets are block leaders
    for (const auto& cmd : ir.commands) {
        std::vector<uint32_t> targets;
        
        // Extract targets from various branch commands
        if (auto* c = std::get_if<Cmd_Sjump>(&cmd.data)) {
            targets.push_back(c->target.rom_address);
        } else if (auto* c = std::get_if<Cmd_Ifequal>(&cmd.data)) {
            targets.push_back(c->target.rom_address);
        } else if (auto* c = std::get_if<Cmd_Ifnotequal>(&cmd.data)) {
            targets.push_back(c->target.rom_address);
        } else if (auto* c = std::get_if<Cmd_Iftrue>(&cmd.data)) {
            targets.push_back(c->target.rom_address);
        } else if (auto* c = std::get_if<Cmd_Iffalse>(&cmd.data)) {
            targets.push_back(c->target.rom_address);
        } else if (auto* c = std::get_if<Cmd_Ifgreater>(&cmd.data)) {
            targets.push_back(c->target.rom_address);
        } else if (auto* c = std::get_if<Cmd_Ifless>(&cmd.data)) {
            targets.push_back(c->target.rom_address);
        } else if (auto* c = std::get_if<Cmd_Scall>(&cmd.data)) {
            targets.push_back(c->target.rom_address);
        } else if (auto* c = std::get_if<Cmd_Farscall>(&cmd.data)) {
            targets.push_back(c->target.rom_address);
        } else if (auto* c = std::get_if<Cmd_Farsjump>(&cmd.data)) {
            targets.push_back(c->target.rom_address);
        }
        
        for (uint32_t target : targets) {
            if (!block_entries.contains(target)) {
                result.all_targets_are_block_leaders = false;
                std::ostringstream ss;
                ss << body_name << ": branch target 0x" << std::hex << target 
                   << " is not a block leader (from command at 0x" << cmd.span.rom_address << ")";
                result.errors.push_back(ss.str());
            }
        }
    }
    
    // 2. Check no overlapping blocks (build command->block mapping)
    std::map<uint32_t, uint32_t> cmd_to_block; // cmd address -> block entry
    for (const auto& block : cfg.blocks) {
        for (size_t i = block.command_start; i < block.command_start + block.command_count; ++i) {
            if (i >= ir.commands.size()) continue;
            uint32_t cmd_addr = ir.commands[i].span.rom_address;
            
            if (cmd_to_block.contains(cmd_addr)) {
                result.no_overlapping_blocks = false;
                result.all_commands_in_one_block = false;
                std::ostringstream ss;
                ss << body_name << ": command at 0x" << std::hex << cmd_addr 
                   << " belongs to multiple blocks (0x" << cmd_to_block[cmd_addr]
                   << " and 0x" << block.start_address << ")";
                result.errors.push_back(ss.str());
            } else {
                cmd_to_block[cmd_addr] = block.start_address;
            }
        }
    }
    
    // 3. Check all commands belong to at least one block
    for (const auto& cmd : ir.commands) {
        if (!cmd_to_block.contains(cmd.span.rom_address)) {
            result.all_commands_in_one_block = false;
            std::ostringstream ss;
            ss << body_name << ": command at 0x" << std::hex << cmd.span.rom_address 
               << " does not belong to any CFG block";
            result.errors.push_back(ss.str());
        }
    }
    
    // 4. Check back-edges (edges to already-seen addresses indicate loops)
    // A back-edge target must exist in block_entries
    for (const auto& block : cfg.blocks) {
        // Check primary target
        if (block.exit.primary_target && block.exit.primary_target->address != 0) {
            uint32_t target_addr = block.exit.primary_target->address;
            // If target < current block's start, it's a back-edge
            if (target_addr < block.start_address) {
                if (!block_entries.contains(target_addr)) {
                    result.all_back_edges_preserved = false;
                    std::ostringstream ss;
                    ss << body_name << ": back-edge target 0x" << std::hex << target_addr
                       << " from block 0x" << block.start_address << " is missing";
                    result.errors.push_back(ss.str());
                }
            }
        }
        // Check fallthrough target
        if (block.exit.fallthrough_target && block.exit.fallthrough_target->address != 0) {
            uint32_t target_addr = block.exit.fallthrough_target->address;
            if (target_addr < block.start_address) {
                if (!block_entries.contains(target_addr)) {
                    result.all_back_edges_preserved = false;
                    std::ostringstream ss;
                    ss << body_name << ": back-edge fallthrough target 0x" << std::hex << target_addr
                       << " from block 0x" << block.start_address << " is missing";
                    result.errors.push_back(ss.str());
                }
            }
        }
    }
    
    return result;
}

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
    
    StdScriptsTable std_scripts;
    std_scripts.load(*rom, profile->offsets.std_scripts, profile->offsets.std_scripts_count);
    
    MapExtractor extractor(*rom, *profile);
    auto discovered_maps = discover_reachable_maps(*rom, *profile, extractor);
    
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
    }
    
    for (size_t i = 0; i < std_scripts.size(); ++i) {
        const auto* entry = std_scripts.get(static_cast<uint16_t>(i));
        if (entry && entry->flat_address != 0) {
            std::string name = "StdScript_" + std::to_string(i);
            all_addresses[entry->flat_address] = name;
        }
    }
    
    std::cout << "=== CFG Integrity Check ===\n";
    std::cout << "Total executable bodies: " << all_addresses.size() << "\n\n";
    
    SymbolMap symbols;
    TypedScriptDecoder decoder(*rom, symbols);
    
    size_t bodies_checked = 0;
    size_t bodies_with_errors = 0;
    std::vector<std::string> all_errors;
    
    bool all_targets_ok = true;
    bool no_overlaps_ok = true;
    bool all_assigned_ok = true;
    bool back_edges_ok = true;
    
    for (const auto& [addr, name] : all_addresses) {
        CrystalScriptIR ir = decoder.decode_script(addr);
        CFGBuilder builder;
        CrystalCFG cfg = builder.build(ir);
        
        auto result = check_cfg_integrity(ir, cfg, name);
        bodies_checked++;
        
        if (!result.errors.empty()) {
            bodies_with_errors++;
            for (const auto& err : result.errors) {
                all_errors.push_back(err);
            }
        }
        
        if (!result.all_targets_are_block_leaders) all_targets_ok = false;
        if (!result.no_overlapping_blocks) no_overlaps_ok = false;
        if (!result.all_commands_in_one_block) all_assigned_ok = false;
        if (!result.all_back_edges_preserved) back_edges_ok = false;
    }
    
    std::cout << "=== RESULTS ===\n";
    std::cout << "Bodies checked: " << bodies_checked << "\n";
    std::cout << "Bodies with errors: " << bodies_with_errors << "\n\n";
    
    std::cout << "=== INVARIANTS ===\n";
    std::cout << "Every branch target is block leader: " << (all_targets_ok ? "PASS" : "FAIL") << "\n";
    std::cout << "No overlapping CFG blocks:           " << (no_overlaps_ok ? "PASS" : "FAIL") << "\n";
    std::cout << "Every command in exactly one block:  " << (all_assigned_ok ? "PASS" : "FAIL") << "\n";
    std::cout << "All back-edges preserved:            " << (back_edges_ok ? "PASS" : "FAIL") << "\n\n";
    
    if (!all_errors.empty()) {
        std::cout << "=== ERRORS ===\n";
        for (size_t i = 0; i < std::min(all_errors.size(), size_t(20)); ++i) {
            std::cout << all_errors[i] << "\n";
        }
        if (all_errors.size() > 20) {
            std::cout << "... and " << (all_errors.size() - 20) << " more errors\n";
        }
    }
    
    return bodies_with_errors > 0 ? 1 : 0;
}
