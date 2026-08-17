// pokecenter2f_trace.cpp - Trace EVERY Pokecenter2F script body and its contents
// Focus on finding all 5 Special 152 occurrences
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
constexpr uint8_t SCENE_SCRIPT_SIZE = 4;
constexpr uint8_t CALLBACK_SIZE = 3;

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
    
    std::cout << "=== Pokecenter2F Script Trace ===\n\n";
    
    const auto& o = profile->offsets;
    const auto& fmt = profile->format.map;
    
    // Load StdScripts table
    StdScriptsTable std_scripts;
    std_scripts.load(*rom, profile->offsets.std_scripts, profile->offsets.std_scripts_count);
    
    // Setup decoder
    SymbolMap symbols;
    TypedScriptDecoder decoder(*rom, symbols);
    
    NativeCallRegistry native_registry;
    native_registry.initialize();
    RamAddressRegistry ram_registry;
    ram_registry.initialize();
    
    CFGBuilder cfg_builder;
    cfg_builder.set_std_scripts(&std_scripts);
    cfg_builder.set_native_registry(&native_registry);
    
    // Find Pokecenter2F (group 20, map 1 based on map_g20_i01 in earlier output)
    MapExtractor extractor(*rom, *profile);
    auto discovered_maps = discover_reachable_maps(*rom, *profile, extractor);
    
    // Find the exact group/map for Pokecenter2F
    uint8_t target_group = 0, target_map = 0;
    for (const auto& ref : discovered_maps) {
        auto result = extractor.extract_map(ref.group, ref.map);
        if (result.success && result.map.map_id.find("Pokecenter2F") != std::string::npos) {
            target_group = ref.group;
            target_map = ref.map;
            std::cout << "Found Pokecenter2F: group=" << (int)ref.group 
                      << " map=" << (int)ref.map << "\n\n";
            break;
        }
    }
    
    if (target_group == 0) {
        // Try group 20, map 1 based on map_g20_i01
        target_group = 20;
        target_map = 1;
        std::cout << "Using fallback: group=20, map=1\n\n";
    }
    
    auto result = extractor.extract_map(target_group, target_map);
    if (!result.success) {
        std::cerr << "Failed to extract Pokecenter2F\n";
        return 1;
    }
    
    std::cout << "Map ID: " << result.map.map_id << "\n";
    std::cout << "Objects: " << result.map.objects.size() << "\n";
    std::cout << "BG Events: " << result.map.bg_events.size() << "\n\n";
    
    // Get MapScripts header for scene scripts
    uint32_t group_ptr_addr = o.map_group_pointers + ((target_group - 1) * 2);
    uint16_t group_addr = rom->read_word(group_ptr_addr);
    uint32_t group_flat = rom->bank_to_flat(o.map_groups_bank, group_addr);
    uint32_t map_entry_addr = group_flat + ((target_map - 1) * 9);
    
    auto entry = rom->read_bytes(map_entry_addr, 9);
    uint8_t attr_bank = entry[0];
    uint16_t attr_ptr = entry[3] | (entry[4] << 8);
    uint32_t header_addr = rom->bank_to_flat(attr_bank, attr_ptr);
    
    auto header = rom->read_bytes(header_addr, fmt.header_size);
    uint8_t script_bank = header[fmt.script_bank_offset];
    uint16_t script_ptr = header[fmt.script_ptr_offset] | (header[fmt.script_ptr_offset + 1] << 8);
    
    uint32_t map_scripts_addr = rom->bank_to_flat(script_bank, script_ptr);
    
    std::cout << "Script bank: 0x" << std::hex << (int)script_bank << std::dec << "\n";
    std::cout << "MapScripts address: 0x" << std::hex << map_scripts_addr << std::dec << "\n\n";
    
    // Read MapScripts header
    uint32_t ptr = map_scripts_addr;
    uint8_t scene_count = rom->read_byte(ptr++);
    
    std::cout << "Scene script count: " << (int)scene_count << "\n";
    
    std::set<uint32_t> all_scripts;
    
    // Collect scene scripts
    for (uint8_t i = 0; i < scene_count; ++i) {
        uint16_t scene_script_ptr = rom->read_word(ptr);
        ptr += 4;  // script_ptr + filler
        
        if (scene_script_ptr != 0) {
            uint32_t scene_script_addr = rom->bank_to_flat(script_bank, scene_script_ptr);
            std::cout << "  Scene " << (int)i << ": 0x" << std::hex << scene_script_addr << std::dec << "\n";
            all_scripts.insert(scene_script_addr);
        }
    }
    
    uint8_t callback_count = rom->read_byte(ptr++);
    std::cout << "Callback count: " << (int)callback_count << "\n";
    
    // Collect callbacks
    for (uint8_t i = 0; i < callback_count; ++i) {
        ptr++;  // Skip type
        uint16_t callback_ptr = rom->read_word(ptr);
        ptr += 2;
        
        if (callback_ptr != 0) {
            uint32_t callback_addr = rom->bank_to_flat(script_bank, callback_ptr);
            std::cout << "  Callback " << (int)i << ": 0x" << std::hex << callback_addr << std::dec << "\n";
            all_scripts.insert(callback_addr);
        }
    }
    
    // Collect object scripts
    std::cout << "\nObject scripts:\n";
    for (size_t i = 0; i < result.map.objects.size(); ++i) {
        const auto& obj = result.map.objects[i];
        if (obj.script_rom_address != 0) {
            std::cout << "  Object " << i << ": 0x" << std::hex << obj.script_rom_address << std::dec << "\n";
            all_scripts.insert(obj.script_rom_address);
        }
    }
    
    // Collect BG event scripts
    std::cout << "\nBG event scripts:\n";
    for (size_t i = 0; i < result.map.bg_events.size(); ++i) {
        const auto& bg = result.map.bg_events[i];
        if (bg.script_rom_address != 0) {
            std::cout << "  BG " << i << ": 0x" << std::hex << bg.script_rom_address << std::dec << "\n";
            all_scripts.insert(bg.script_rom_address);
        }
    }
    
    std::cout << "\n=== Decoding all " << all_scripts.size() << " Pokecenter2F scripts ===\n\n";
    
    // Track sdefer targets that need to be decoded
    std::set<uint32_t> sdefer_target_addrs;
    
    // Decode each script and look for Special 152, sdefer, scall
    for (uint32_t addr : all_scripts) {
        try {
            CrystalScriptIR ir = decoder.decode_script(addr);
            
            bool has_special152 = false;
            bool has_sdefer = false;
            bool has_scall = false;
            std::vector<uint32_t> sdefer_targets;
            std::vector<uint32_t> scall_targets;
            std::vector<uint32_t> special152_addrs;
            
            for (const auto& cmd : ir.commands) {
                if (const auto* special = std::get_if<Cmd_Special>(&cmd.data)) {
                    if (special->special_id == 152) {
                        has_special152 = true;
                        special152_addrs.push_back(cmd.span.rom_address);
                    }
                }
                if (const auto* sdef = std::get_if<Cmd_Sdefer>(&cmd.data)) {
                    has_sdefer = true;
                    // sdefer pointer is bank-relative, resolve to flat address
                    uint32_t flat_target = rom->bank_to_flat(script_bank, sdef->pointer);
                    sdefer_targets.push_back(flat_target);
                    sdefer_target_addrs.insert(flat_target);
                }
                if (const auto* scal = std::get_if<Cmd_Scall>(&cmd.data)) {
                    has_scall = true;
                    scall_targets.push_back(scal->target.rom_address);
                }
            }
            
            if (has_special152 || has_sdefer || has_scall) {
                std::cout << "0x" << std::hex << addr << std::dec << " (" << ir.commands.size() << " cmds):";
                if (has_special152) {
                    std::cout << " SPECIAL_152 at";
                    for (uint32_t a : special152_addrs) {
                        std::cout << " 0x" << std::hex << a << std::dec;
                    }
                }
                if (has_sdefer) {
                    std::cout << " sdefer to";
                    for (uint32_t t : sdefer_targets) {
                        std::cout << " 0x" << std::hex << t << std::dec;
                    }
                }
                if (has_scall) {
                    std::cout << " scall to";
                    for (uint32_t t : scall_targets) {
                        std::cout << " 0x" << std::hex << t << std::dec;
                    }
                }
                std::cout << "\n";
            }
        } catch (const std::exception& e) {
            std::cerr << "0x" << std::hex << addr << std::dec << ": decode failed: " << e.what() << "\n";
        }
    }
    
    // Now decode sdefer targets to find more Special 152s
    std::cout << "\n=== Decoding " << sdefer_target_addrs.size() << " sdefer targets ===\n\n";
    
    for (uint32_t addr : sdefer_target_addrs) {
        try {
            CrystalScriptIR ir = decoder.decode_script(addr);
            
            bool has_special152 = false;
            bool has_scall = false;
            std::vector<uint32_t> scall_targets;
            std::vector<uint32_t> special152_addrs;
            
            for (const auto& cmd : ir.commands) {
                if (const auto* special = std::get_if<Cmd_Special>(&cmd.data)) {
                    if (special->special_id == 152) {
                        has_special152 = true;
                        special152_addrs.push_back(cmd.span.rom_address);
                    }
                }
                if (const auto* scal = std::get_if<Cmd_Scall>(&cmd.data)) {
                    has_scall = true;
                    scall_targets.push_back(scal->target.rom_address);
                }
            }
            
            std::cout << "sdefer target 0x" << std::hex << addr << std::dec << " (" << ir.commands.size() << " cmds):";
            if (has_special152) {
                std::cout << " SPECIAL_152 at";
                for (uint32_t a : special152_addrs) {
                    std::cout << " 0x" << std::hex << a << std::dec;
                }
            }
            if (has_scall) {
                std::cout << " scall to";
                for (uint32_t t : scall_targets) {
                    std::cout << " 0x" << std::hex << t << std::dec;
                }
            }
            if (!has_special152 && !has_scall) {
                std::cout << " (no special152 or scall)";
            }
            std::cout << "\n";
            
            // Now trace scall targets from sdefer targets
            for (uint32_t scall_target : scall_targets) {
                try {
                    CrystalScriptIR scall_ir = decoder.decode_script(scall_target);
                    
                    bool scall_has_152 = false;
                    std::vector<uint32_t> scall_152_addrs;
                    
                    for (const auto& cmd : scall_ir.commands) {
                        if (const auto* special = std::get_if<Cmd_Special>(&cmd.data)) {
                            if (special->special_id == 152) {
                                scall_has_152 = true;
                                scall_152_addrs.push_back(cmd.span.rom_address);
                            }
                        }
                    }
                    
                    if (scall_has_152) {
                        std::cout << "    scall target 0x" << std::hex << scall_target << std::dec 
                                  << " (" << scall_ir.commands.size() << " cmds): SPECIAL_152 at";
                        for (uint32_t a : scall_152_addrs) {
                            std::cout << " 0x" << std::hex << a << std::dec;
                        }
                        std::cout << "\n";
                    }
                } catch (...) {
                    // Ignore
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "sdefer target 0x" << std::hex << addr << std::dec << ": decode failed: " << e.what() << "\n";
        }
    }
    
    return 0;
}
