// special152_trace.cpp - Trace all occurrences of Special 152 (SetPlayerPalette) in the expanded corpus
// Answers: which compiled bodies contain Special 152, and at what source addresses?
#include "crystal/rom/loader.hpp"
#include "crystal/rom/profile.hpp"
#include "crystal/script/typed_decoder.hpp"
#include "crystal/script/crystal_cfg.hpp"
#include "crystal/script/semantic_legalizer.hpp"
#include "crystal/script/native_registry.hpp"
#include "crystal/script/elevator_registry.hpp"
#include "crystal/extract/map_extractor.hpp"
#include "crystal/compile/full_compiler.hpp"
#include "engine/scripting/semantic_ir.hpp"
#include <iostream>
#include <iomanip>
#include <map>
#include <set>

using namespace crystal;
using namespace enginemon;

// Crystal MapScripts structure sizes
constexpr uint8_t SCENE_SCRIPT_SIZE = 4;  // dw script_ptr, dw 0 (filler)
constexpr uint8_t CALLBACK_SIZE = 3;      // db type, dw script_ptr

struct ScriptRoot {
    uint32_t address;
    std::string root_type;  // "object", "bg_event", "scene", "callback", "std"
    std::string map_name;
    uint8_t local_index;
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
    
    std::cout << "=== Special 152 (SetPlayerPalette) Trace ===\n\n";
    
    const auto& o = profile->offsets;
    const auto& fmt = profile->format.map;
    
    // Load StdScripts table
    StdScriptsTable std_scripts;
    std_scripts.load(*rom, profile->offsets.std_scripts, profile->offsets.std_scripts_count);
    
    // Discover maps
    MapExtractor extractor(*rom, *profile);
    auto discovered_maps = discover_reachable_maps(*rom, *profile, extractor);
    
    // Collect all script roots with metadata
    std::map<uint32_t, ScriptRoot> root_metadata;
    
    for (const auto& ref : discovered_maps) {
        auto result = extractor.extract_map(ref.group, ref.map);
        if (!result.success) continue;
        
        std::string map_name = result.map.map_id;
        
        // Object scripts
        uint8_t obj_idx = 0;
        for (const auto& obj : result.map.objects) {
            if (obj.script_rom_address != 0) {
                root_metadata[obj.script_rom_address] = {
                    obj.script_rom_address, "object", map_name, obj_idx
                };
            }
            ++obj_idx;
        }
        
        // BG event scripts
        uint8_t bg_idx = 0;
        for (const auto& bg : result.map.bg_events) {
            if (bg.script_rom_address != 0) {
                root_metadata[bg.script_rom_address] = {
                    bg.script_rom_address, "bg_event", map_name, bg_idx
                };
            }
            ++bg_idx;
        }
        
        // Scene scripts and callbacks from MapScripts header
        uint32_t group_ptr_addr = o.map_group_pointers + ((ref.group - 1) * 2);
        if (group_ptr_addr + 2 > rom->size()) continue;
        
        uint16_t group_addr = rom->read_word(group_ptr_addr);
        uint32_t group_flat = rom->bank_to_flat(o.map_groups_bank, group_addr);
        uint32_t map_entry_addr = group_flat + ((ref.map - 1) * 9);
        
        if (map_entry_addr + 9 > rom->size()) continue;
        
        auto entry = rom->read_bytes(map_entry_addr, 9);
        uint8_t attr_bank = entry[0];
        uint16_t attr_ptr = entry[3] | (entry[4] << 8);
        uint32_t header_addr = rom->bank_to_flat(attr_bank, attr_ptr);
        
        if (header_addr + fmt.header_size > rom->size()) continue;
        
        auto header = rom->read_bytes(header_addr, fmt.header_size);
        uint8_t script_bank = header[fmt.script_bank_offset];
        uint16_t script_ptr = header[fmt.script_ptr_offset] | (header[fmt.script_ptr_offset + 1] << 8);
        
        uint32_t map_scripts_addr = rom->bank_to_flat(script_bank, script_ptr);
        if (map_scripts_addr + 1 > rom->size()) continue;
        
        uint32_t ptr = map_scripts_addr;
        
        // Scene scripts
        uint8_t scene_count = rom->read_byte(ptr++);
        if (scene_count > 20) continue;
        
        for (uint8_t i = 0; i < scene_count; ++i) {
            if (ptr + SCENE_SCRIPT_SIZE > rom->size()) break;
            
            uint16_t scene_script_ptr = rom->read_word(ptr);
            ptr += 4;  // script_ptr + filler
            
            if (scene_script_ptr != 0) {
                uint32_t scene_script_addr = rom->bank_to_flat(script_bank, scene_script_ptr);
                if (scene_script_addr > 0 && scene_script_addr < rom->size()) {
                    root_metadata[scene_script_addr] = {
                        scene_script_addr, "scene", map_name, i
                    };
                }
            }
        }
        
        // Callbacks
        if (ptr + 1 > rom->size()) continue;
        
        uint8_t callback_count = rom->read_byte(ptr++);
        if (callback_count > 20) continue;
        
        for (uint8_t i = 0; i < callback_count; ++i) {
            if (ptr + CALLBACK_SIZE > rom->size()) break;
            
            ptr++;  // Skip callback type
            uint16_t callback_ptr = rom->read_word(ptr);
            ptr += 2;
            
            if (callback_ptr != 0) {
                uint32_t callback_addr = rom->bank_to_flat(script_bank, callback_ptr);
                if (callback_addr > 0 && callback_addr < rom->size()) {
                    root_metadata[callback_addr] = {
                        callback_addr, "callback", map_name, i
                    };
                }
            }
        }
    }
    
    // Add StdScript roots
    for (size_t i = 0; i < std_scripts.size(); ++i) {
        const auto* entry = std_scripts.get(static_cast<uint16_t>(i));
        if (entry && entry->flat_address != 0) {
            if (!root_metadata.contains(entry->flat_address)) {
                root_metadata[entry->flat_address] = {
                    entry->flat_address, "std", "StdScript", static_cast<uint8_t>(i)
                };
            }
        }
    }
    
    std::cout << "Total script roots: " << root_metadata.size() << "\n\n";
    
    // Setup decoder
    SymbolMap symbols;
    TypedScriptDecoder decoder(*rom, symbols);
    
    NativeCallRegistry native_registry;
    native_registry.initialize();
    RamAddressRegistry ram_registry;
    ram_registry.initialize();
    ElevatorRegistry elevator_registry(*rom);
    
    CFGBuilder cfg_builder;
    cfg_builder.set_std_scripts(&std_scripts);
    cfg_builder.set_native_registry(&native_registry);
    
    SemanticLegalizer legalizer;
    legalizer.set_native_registry(&native_registry);
    legalizer.set_ram_registry(&ram_registry);
    legalizer.set_elevator_registry(&elevator_registry);
    legalizer.set_num_pokemon(profile->counts.num_pokemon);
    
    // Track Special 152 occurrences
    struct Special152Occurrence {
        uint32_t body_root_address;
        std::string root_type;
        std::string map_name;
        uint32_t special_source_address;  // ROM address of the special opcode
    };
    
    std::vector<Special152Occurrence> occurrences;
    std::set<uint32_t> special152_source_addresses;  // Physical source addresses
    
    // Process each script root
    for (const auto& [root_addr, root] : root_metadata) {
        try {
            CrystalScriptIR ir = decoder.decode_script(root_addr);
            
            // Check for Special 152 in the decoded commands
            for (const auto& cmd : ir.commands) {
                if (const auto* special = std::get_if<Cmd_Special>(&cmd.data)) {
                    if (special->special_id == 152) {
                        occurrences.push_back({
                            root_addr,
                            root.root_type,
                            root.map_name,
                            cmd.span.rom_address
                        });
                        special152_source_addresses.insert(cmd.span.rom_address);
                    }
                }
            }
        } catch (...) {
            // Skip decode failures
        }
    }
    
    std::cout << "=== Special 152 Occurrences ===\n";
    std::cout << "Physical source addresses with Special 152: " << special152_source_addresses.size() << "\n";
    std::cout << "Compiled-body occurrences: " << occurrences.size() << "\n\n";
    
    std::cout << "Physical source addresses:\n";
    for (uint32_t addr : special152_source_addresses) {
        std::cout << "  0x" << std::hex << addr << std::dec << "\n";
    }
    
    std::cout << "\nCompiled-body occurrences:\n";
    std::cout << std::setw(10) << "Body Root" << " | "
              << std::setw(10) << "Root Type" << " | "
              << std::setw(30) << "Map" << " | "
              << std::setw(10) << "Src Addr" << "\n";
    std::cout << std::string(70, '-') << "\n";
    
    for (const auto& occ : occurrences) {
        std::cout << "0x" << std::hex << std::setw(8) << occ.body_root_address << std::dec << " | "
                  << std::setw(10) << occ.root_type << " | "
                  << std::setw(30) << occ.map_name << " | "
                  << "0x" << std::hex << occ.special_source_address << std::dec << "\n";
    }
    
    std::cout << "\n=== Analysis ===\n";
    
    // Map from source address to bodies that contain it
    std::map<uint32_t, std::vector<std::pair<uint32_t, std::string>>> source_to_bodies;
    for (const auto& occ : occurrences) {
        source_to_bodies[occ.special_source_address].push_back({occ.body_root_address, occ.root_type});
    }
    
    std::cout << "Source address → Body coverage:\n";
    for (const auto& [src_addr, bodies] : source_to_bodies) {
        std::cout << "  0x" << std::hex << src_addr << std::dec << " → " 
                  << bodies.size() << " bodies: ";
        for (const auto& [body_addr, root_type] : bodies) {
            std::cout << root_type << "@0x" << std::hex << body_addr << std::dec << " ";
        }
        std::cout << "\n";
    }
    
    return 0;
}
