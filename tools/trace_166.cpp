// trace_166.cpp - Trace Special 166 duplication through pipeline layers
#include "crystal/rom/loader.hpp"
#include "crystal/rom/profile.hpp"
#include "crystal/script/typed_decoder.hpp"
#include "crystal/script/crystal_cfg.hpp"
#include "crystal/script/semantic_legalizer.hpp"
#include "crystal/script/native_registry.hpp"
#include "crystal/script/legality_gate.hpp"
#include "crystal/script/elevator_registry.hpp"
#include "engine/scripting/semantic_ir.hpp"
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
    
    // Setup decoder
    SymbolMap symbols;
    TypedScriptDecoder decoder(*rom, symbols);
    
    std::cout << "=== Tracing Special 166 at script 0x7a582 ===\n\n";
    
    // Decode the script containing Special 166
    CrystalScriptIR ir = decoder.decode_script(0x7a582);
    
    std::cout << "=== Layer A: Decoded CrystalCommand Storage ===\n";
    std::cout << "Total commands in ir.commands: " << ir.commands.size() << "\n\n";
    
    // Track all Special commands by ROM address
    std::map<uint32_t, std::vector<size_t>> special_by_rom_addr;
    
    for (size_t i = 0; i < ir.commands.size(); ++i) {
        const auto& cmd = ir.commands[i];
        if (const auto* special = std::get_if<Cmd_Special>(&cmd.data)) {
            special_by_rom_addr[cmd.span.rom_address].push_back(i);
        }
    }
    
    std::cout << "Special commands by ROM address:\n";
    for (const auto& [addr, indices] : special_by_rom_addr) {
        // Find the special ID for this address
        uint16_t special_id = 0;
        if (!indices.empty()) {
            if (const auto* sp = std::get_if<Cmd_Special>(&ir.commands[indices[0]].data)) {
                special_id = sp->special_id;
            }
        }
        
        std::cout << "  ROM 0x" << std::hex << addr << " (Special " << std::dec << special_id << "): ";
        std::cout << indices.size() << " occurrence(s) at command indices: ";
        for (size_t idx : indices) {
            std::cout << idx << " ";
        }
        std::cout << "\n";
    }
    
    // Check for Special 166 specifically
    std::cout << "\n=== Special 166 Analysis ===\n";
    size_t count_166 = 0;
    for (size_t i = 0; i < ir.commands.size(); ++i) {
        const auto& cmd = ir.commands[i];
        if (const auto* special = std::get_if<Cmd_Special>(&cmd.data)) {
            if (special->special_id == 166) {
                count_166++;
                std::cout << "  Found at command index " << i 
                          << ", ROM address 0x" << std::hex << cmd.span.rom_address << std::dec << "\n";
            }
        }
    }
    std::cout << "Total Special 166 in decoded IR: " << count_166 << "\n";
    
    // Check if these are unique ROM addresses
    std::set<uint32_t> unique_166_addrs;
    for (size_t i = 0; i < ir.commands.size(); ++i) {
        const auto& cmd = ir.commands[i];
        if (const auto* special = std::get_if<Cmd_Special>(&cmd.data)) {
            if (special->special_id == 166) {
                unique_166_addrs.insert(cmd.span.rom_address);
            }
        }
    }
    std::cout << "Unique ROM addresses for Special 166: " << unique_166_addrs.size() << "\n";
    
    if (count_166 > unique_166_addrs.size()) {
        std::cout << "\n*** DECODER DUPLICATION DETECTED ***\n";
        std::cout << "The same ROM instruction is being decoded multiple times!\n";
    } else if (count_166 == unique_166_addrs.size() && count_166 > 1) {
        std::cout << "\nSpecial 166 appears at multiple different ROM addresses.\n";
    }
    
    // Now check CFG layer
    std::cout << "\n=== Layer B: CFG Block Construction ===\n";
    
    StdScriptsTable std_scripts;
    std_scripts.load(*rom, profile->offsets.std_scripts, profile->offsets.std_scripts_count);
    
    NativeCallRegistry native_registry;
    native_registry.initialize();
    
    CFGBuilder cfg_builder;
    cfg_builder.set_std_scripts(&std_scripts);
    cfg_builder.set_native_registry(&native_registry);
    
    auto cfg = cfg_builder.build(ir);
    
    std::cout << "Total CFG blocks: " << cfg.blocks.size() << "\n";
    
    size_t cfg_166_count = 0;
    for (size_t bi = 0; bi < cfg.blocks.size(); ++bi) {
        const auto& block = cfg.blocks[bi];
        for (size_t ci = 0; ci < block.command_count; ++ci) {
            size_t cmd_idx = block.command_start + ci;
            const auto& cmd = ir.commands[cmd_idx];
            if (const auto* special = std::get_if<Cmd_Special>(&cmd.data)) {
                if (special->special_id == 166) {
                    cfg_166_count++;
                    std::cout << "  Block " << bi << ", local " << ci 
                              << " -> ir.commands[" << cmd_idx << "] @ ROM 0x" 
                              << std::hex << cmd.span.rom_address << std::dec << "\n";
                }
            }
        }
    }
    std::cout << "Total Special 166 references in CFG: " << cfg_166_count << "\n";
    
    // Check for duplicate command indices in CFG
    std::map<size_t, std::vector<std::pair<size_t, size_t>>> cmd_to_blocks; // cmd_idx -> [(block_idx, local_idx)]
    for (size_t bi = 0; bi < cfg.blocks.size(); ++bi) {
        const auto& block = cfg.blocks[bi];
        for (size_t ci = 0; ci < block.command_count; ++ci) {
            size_t cmd_idx = block.command_start + ci;
            cmd_to_blocks[cmd_idx].push_back({bi, ci});
        }
    }
    
    std::cout << "\nCommand indices appearing in multiple blocks:\n";
    bool found_dupe = false;
    for (const auto& [cmd_idx, locations] : cmd_to_blocks) {
        if (locations.size() > 1) {
            found_dupe = true;
            const auto& cmd = ir.commands[cmd_idx];
            std::cout << "  ir.commands[" << cmd_idx << "] @ ROM 0x" << std::hex << cmd.span.rom_address << std::dec;
            if (const auto* special = std::get_if<Cmd_Special>(&cmd.data)) {
                std::cout << " (Special " << special->special_id << ")";
            }
            std::cout << " appears in " << locations.size() << " blocks: ";
            for (auto [bi, ci] : locations) {
                std::cout << "block[" << bi << "][" << ci << "] ";
            }
            std::cout << "\n";
        }
    }
    if (!found_dupe) {
        std::cout << "  (none - each command index appears in exactly one block)\n";
    }
    
    // Now check SemanticIR layer
    std::cout << "\n=== Layer C: SemanticScriptIR Instruction Storage ===\n";
    
    RamAddressRegistry ram_registry;
    ram_registry.initialize();
    ElevatorRegistry elevator_registry(*rom);
    
    SemanticLegalizer legalizer;
    legalizer.set_native_registry(&native_registry);
    legalizer.set_ram_registry(&ram_registry);
    legalizer.set_elevator_registry(&elevator_registry);
    
    auto lowered = legalizer.lower(ir, cfg);
    
    std::cout << "Total SemanticIR blocks: " << lowered.ir.blocks.size() << "\n";
    
    size_t sem_special_166_count = 0;
    size_t sem_dst_true_count = 0;
    
    for (size_t bi = 0; bi < lowered.ir.blocks.size(); ++bi) {
        const auto& block = lowered.ir.blocks[bi];
        for (size_t ii = 0; ii < block.instructions.size(); ++ii) {
            const auto& inst = block.instructions[ii];
            
            if (const auto* sp = std::get_if<Sem_Special>(&inst.op)) {
                if (sp->special_id == 166) {
                    sem_special_166_count++;
                    std::cout << "  Sem_Special(166) at block[" << bi << "].inst[" << ii << "]\n";
                }
            }
            
            if (const auto* dst = std::get_if<Sem_SetDaylightSaving>(&inst.op)) {
                if (dst->enabled) {
                    sem_dst_true_count++;
                    std::cout << "  Sem_SetDaylightSaving(true) at block[" << bi << "].inst[" << ii << "]\n";
                }
            }
        }
    }
    
    std::cout << "\nSem_Special(166) count: " << sem_special_166_count << "\n";
    std::cout << "Sem_SetDaylightSaving(true) count: " << sem_dst_true_count << "\n";
    
    // Final diagnosis
    std::cout << "\n=== DIAGNOSIS ===\n";
    if (count_166 > 1 && unique_166_addrs.size() == 1) {
        std::cout << "COMPILER/DECODER DUPLICATION: The decoder is materializing the same ROM instruction multiple times.\n";
        std::cout << "This is a BUG - one source instruction should produce one decoded command.\n";
    } else if (cfg_166_count > count_166) {
        std::cout << "CFG DUPLICATION: CFG references the same decoded command multiple times.\n";
        std::cout << "This may be intentional for CFG traversal but inventory should count unique commands.\n";
    } else if (count_166 == 1 && unique_166_addrs.size() == 1) {
        std::cout << "NO DUPLICATION in decoder or CFG - issue is in inventory traversal only.\n";
    } else {
        std::cout << "Unexpected state - manual investigation needed.\n";
    }
    
    return 0;
}
