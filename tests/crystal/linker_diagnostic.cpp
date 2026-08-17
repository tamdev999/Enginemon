// linker_diagnostic.cpp
// Deep diagnostic tool for Stage 6 linker failures
// Traces each failure back to its source

#include "crystal/rom/loader.hpp"
#include "crystal/rom/profile.hpp"
#include "crystal/rom/symbol_map.hpp"
#include "crystal/script/typed_decoder.hpp"
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

// Discover reachable maps
std::vector<MapIdRef> discover_reachable_maps(const RomData& rom, 
    const ExtractionProfile& profile, MapExtractor& extractor);

// Format hex bytes
std::string hex_bytes(const std::vector<uint8_t>& bytes) {
    std::ostringstream ss;
    for (size_t i = 0; i < bytes.size(); ++i) {
        if (i > 0) ss << " ";
        ss << std::hex << std::setfill('0') << std::setw(2) << (int)bytes[i];
    }
    return ss.str();
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
    
    // Setup components
    MapExtractor extractor(*rom, *profile);
    SymbolMap symbols;
    TypedScriptDecoder decoder(*rom, symbols);
    
    NativeCallRegistry native_registry;
    native_registry.initialize();
    RamAddressRegistry ram_registry;
    ram_registry.initialize();
    
    StdScriptsTable std_scripts;
    std_scripts.load(*rom, profile->offsets.std_scripts, profile->offsets.std_scripts_count);
    
    CFGBuilder cfg_builder;
    cfg_builder.set_std_scripts(&std_scripts);
    cfg_builder.set_native_registry(&native_registry);
    
    SemanticLegalizer legalizer;
    legalizer.set_native_registry(&native_registry);
    legalizer.set_ram_registry(&ram_registry);
    legalizer.set_num_pokemon(profile->counts.num_pokemon);
    
    // Discover maps and collect script addresses
    auto discovered_maps = discover_reachable_maps(*rom, *profile, extractor);
    
    // Build address -> map(s) mapping (track ALL maps, not just first)
    std::map<uint32_t, std::vector<MapId>> address_to_maps;
    std::map<MapId, uint8_t> map_object_counts;
    std::map<MapId, std::string> map_names;
    
    for (const auto& ref : discovered_maps) {
        auto result = extractor.extract_map(ref.group, ref.map);
        if (!result.success) continue;
        
        MapId map_id = (static_cast<uint16_t>(ref.group) << 8) | ref.map;
        map_object_counts[map_id] = static_cast<uint8_t>(result.map.objects.size());
        map_names[map_id] = result.map.name;
        
        for (const auto& obj : result.map.objects) {
            if (obj.script_rom_address != 0) {
                address_to_maps[obj.script_rom_address].push_back(map_id);
            }
        }
        for (const auto& bg : result.map.bg_events) {
            if (bg.script_rom_address != 0) {
                address_to_maps[bg.script_rom_address].push_back(map_id);
            }
        }
    }
    
    std::cout << "=== STAGE 6 LINKER FAILURE DIAGNOSTIC ===\n\n";
    
    // Known failing script addresses from the linker output
    // These are the scripts we need to investigate
    struct FailingScript {
        uint32_t address;
        std::string issue_type;
        std::string issue_detail;
    };
    
    std::vector<FailingScript> failing_scripts = {
        // Special failures
        {0x426946, "Special", "515"},
        {0x433463, "Special", "768"},
        
        // Map failures  
        {0x434692, "Map", "784"},
        {0x437216, "Map", "784"},
        
        // Object failures (sample - need to trace all)
        {0x351991, "Object", "5"},
        {0x355354, "Object", "13"},
        {0x360574, "Object", "6"},
        {0x362408, "Object", "3"},
        {0x369439, "Object", "7"},
        {0x431308, "Object", "6"},
    };
    
    std::cout << "======================================\n";
    std::cout << "SECTION A: UNRESOLVED SPECIAL REFERENCES\n";
    std::cout << "======================================\n\n";
    
    // Investigate Special 515 at 0x426946
    {
        uint32_t addr = 0x426946;
        std::cout << "--- Script at 0x" << std::hex << addr << std::dec << " ---\n";
        
        // Find which maps reference this script
        auto it = address_to_maps.find(addr);
        if (it != address_to_maps.end()) {
            std::cout << "Referenced by maps:\n";
            for (MapId m : it->second) {
                uint8_t g = m >> 8;
                uint8_t i = m & 0xFF;
                std::cout << "  Map " << (int)g << ":" << (int)i;
                auto name_it = map_names.find(m);
                if (name_it != map_names.end()) {
                    std::cout << " (" << name_it->second << ")";
                }
                std::cout << ", objects: " << (int)map_object_counts[m] << "\n";
            }
        }
        
        // Decode the script
        std::cout << "\nDecoding script...\n";
        try {
            CrystalScriptIR ir = decoder.decode_script(addr);
            
            // Find commands with special opcode (0x89)
            for (size_t i = 0; i < ir.commands.size(); ++i) {
                const auto& cmd = ir.commands[i];
                
                if (std::holds_alternative<Cmd_Special>(cmd.data)) {
                    const auto& special = std::get<Cmd_Special>(cmd.data);
                    std::cout << "\nCommand " << i << " at 0x" << std::hex << cmd.rom_address << std::dec << ":\n";
                    std::cout << "  Opcode: 0x89 (special)\n";
                    std::cout << "  Decoded special_id: " << special.special_id << "\n";
                    
                    // Read raw bytes from ROM
                    auto raw = rom->read_bytes(cmd.rom_address, 3);
                    std::cout << "  Raw bytes: " << hex_bytes(raw) << "\n";
                    std::cout << "  Byte[0] = opcode: 0x" << std::hex << (int)raw[0] << std::dec << "\n";
                    std::cout << "  Byte[1] = low: 0x" << std::hex << (int)raw[1] << std::dec << " (" << (int)raw[1] << ")\n";
                    std::cout << "  Byte[2] = high: 0x" << std::hex << (int)raw[2] << std::dec << " (" << (int)raw[2] << ")\n";
                    std::cout << "  Little-endian 16-bit: " << (raw[1] | (raw[2] << 8)) << "\n";
                    
                    if (special.special_id > 200) {
                        std::cout << "  *** SUSPICIOUS: special_id > 200 ***\n";
                    }
                }
            }
            
            // Build CFG and lower
            CrystalCFG cfg = cfg_builder.build(ir);
            LoweringResult lowering = legalizer.lower(ir, cfg);
            
            std::cout << "\nLowered semantic ops with Special:\n";
            for (size_t bi = 0; bi < lowering.ir.blocks.size(); ++bi) {
                const auto& block = lowering.ir.blocks[bi];
                for (size_t ii = 0; ii < block.instructions.size(); ++ii) {
                    const auto& inst = block.instructions[ii];
                    if (std::holds_alternative<Sem_Special>(inst.op)) {
                        const auto& sem = std::get<Sem_Special>(inst.op);
                        std::cout << "  Block " << bi << " Inst " << ii << ": Sem_Special id=" 
                                  << sem.special_id << " name=\"" << sem.name << "\"\n";
                    }
                }
            }
            
        } catch (const std::exception& e) {
            std::cout << "  Decode error: " << e.what() << "\n";
        }
    }
    
    std::cout << "\n";
    
    // Investigate Special 768 at 0x433463
    {
        uint32_t addr = 0x433463;
        std::cout << "--- Script at 0x" << std::hex << addr << std::dec << " ---\n";
        
        auto it = address_to_maps.find(addr);
        if (it != address_to_maps.end()) {
            std::cout << "Referenced by maps:\n";
            for (MapId m : it->second) {
                uint8_t g = m >> 8;
                uint8_t i = m & 0xFF;
                std::cout << "  Map " << (int)g << ":" << (int)i;
                auto name_it = map_names.find(m);
                if (name_it != map_names.end()) {
                    std::cout << " (" << name_it->second << ")";
                }
                std::cout << "\n";
            }
        }
        
        std::cout << "\nDecoding script...\n";
        try {
            CrystalScriptIR ir = decoder.decode_script(addr);
            
            for (size_t i = 0; i < ir.commands.size(); ++i) {
                const auto& cmd = ir.commands[i];
                
                if (std::holds_alternative<Cmd_Special>(cmd.data)) {
                    const auto& special = std::get<Cmd_Special>(cmd.data);
                    std::cout << "\nCommand " << i << " at 0x" << std::hex << cmd.rom_address << std::dec << ":\n";
                    std::cout << "  Decoded special_id: " << special.special_id << "\n";
                    
                    auto raw = rom->read_bytes(cmd.rom_address, 3);
                    std::cout << "  Raw bytes: " << hex_bytes(raw) << "\n";
                    std::cout << "  Little-endian 16-bit: " << (raw[1] | (raw[2] << 8)) << "\n";
                }
            }
        } catch (const std::exception& e) {
            std::cout << "  Decode error: " << e.what() << "\n";
        }
    }
    
    std::cout << "\n======================================\n";
    std::cout << "SECTION B: UNRESOLVED MAP REFERENCES\n";
    std::cout << "======================================\n\n";
    
    // Investigate Map 784 at 0x434692 and 0x437216
    for (uint32_t addr : {0x434692u, 0x437216u}) {
        std::cout << "--- Script at 0x" << std::hex << addr << std::dec << " ---\n";
        
        auto it = address_to_maps.find(addr);
        if (it != address_to_maps.end()) {
            std::cout << "Referenced by maps:\n";
            for (MapId m : it->second) {
                uint8_t g = m >> 8;
                uint8_t i = m & 0xFF;
                std::cout << "  Map " << (int)g << ":" << (int)i;
                auto name_it = map_names.find(m);
                if (name_it != map_names.end()) {
                    std::cout << " (" << name_it->second << ")";
                }
                std::cout << "\n";
            }
        }
        
        std::cout << "\nDecoding script...\n";
        try {
            CrystalScriptIR ir = decoder.decode_script(addr);
            
            for (size_t i = 0; i < ir.commands.size(); ++i) {
                const auto& cmd = ir.commands[i];
                
                // Check warp commands
                if (std::holds_alternative<Cmd_Warp>(cmd.data)) {
                    const auto& warp = std::get<Cmd_Warp>(cmd.data);
                    MapId packed = (static_cast<uint16_t>(warp.map_group) << 8) | warp.map_num;
                    if (packed == 784) {
                        std::cout << "\nCommand " << i << " at 0x" << std::hex << cmd.rom_address << std::dec << ":\n";
                        std::cout << "  Opcode: warp\n";
                        std::cout << "  map_group: " << (int)warp.map_group << "\n";
                        std::cout << "  map_num: " << (int)warp.map_num << "\n";
                        std::cout << "  Packed MapId: " << packed << " (0x" << std::hex << packed << std::dec << ")\n";
                        
                        auto raw = rom->read_bytes(cmd.rom_address, 5);
                        std::cout << "  Raw bytes: " << hex_bytes(raw) << "\n";
                    }
                }
                
                if (std::holds_alternative<Cmd_WarpFacing>(cmd.data)) {
                    const auto& warp = std::get<Cmd_WarpFacing>(cmd.data);
                    MapId packed = (static_cast<uint16_t>(warp.map_group) << 8) | warp.map_num;
                    if (packed == 784) {
                        std::cout << "\nCommand " << i << " at 0x" << std::hex << cmd.rom_address << std::dec << ":\n";
                        std::cout << "  Opcode: warpfacing\n";
                        std::cout << "  facing: " << (int)warp.facing << "\n";
                        std::cout << "  map_group: " << (int)warp.map_group << "\n";
                        std::cout << "  map_num: " << (int)warp.map_num << "\n";
                        std::cout << "  Packed MapId: " << packed << " (0x" << std::hex << packed << std::dec << ")\n";
                        
                        // 784 = 0x310 = group 3, map 16
                        std::cout << "  Unpacked: group=" << (784 >> 8) << " map=" << (784 & 0xFF) << "\n";
                        
                        auto raw = rom->read_bytes(cmd.rom_address, 6);
                        std::cout << "  Raw bytes: " << hex_bytes(raw) << "\n";
                    }
                }
            }
        } catch (const std::exception& e) {
            std::cout << "  Decode error: " << e.what() << "\n";
        }
    }
    
    std::cout << "\n======================================\n";
    std::cout << "SECTION C: INVALID OBJECT OWNERSHIP\n";
    std::cout << "======================================\n\n";
    
    // For each failing object reference, trace the script and its callers
    struct ObjectFailure {
        uint32_t script_addr;
        uint8_t object_id;
    };
    
    std::vector<ObjectFailure> object_failures = {
        {0x351991, 5},
        {0x355354, 13},
        {0x360574, 6},
        {0x362408, 3},
        {0x369439, 7},
        {0x431308, 6},
    };
    
    for (const auto& fail : object_failures) {
        std::cout << "--- Script 0x" << std::hex << fail.script_addr << std::dec 
                  << " referencing Object " << (int)fail.object_id << " ---\n";
        
        auto it = address_to_maps.find(fail.script_addr);
        if (it != address_to_maps.end()) {
            std::cout << "Maps that execute this script:\n";
            bool shared = it->second.size() > 1;
            
            for (MapId m : it->second) {
                uint8_t g = m >> 8;
                uint8_t i = m & 0xFF;
                uint8_t obj_count = map_object_counts[m];
                bool valid = (fail.object_id >= 1 && fail.object_id <= obj_count);
                
                std::cout << "  Map " << (int)g << ":" << (int)i;
                auto name_it = map_names.find(m);
                if (name_it != map_names.end()) {
                    std::cout << " (" << name_it->second << ")";
                }
                std::cout << " - objects: " << (int)obj_count;
                std::cout << " - Object " << (int)fail.object_id << " valid: " << (valid ? "YES" : "NO");
                std::cout << "\n";
            }
            
            if (shared) {
                std::cout << "  *** SHARED SCRIPT: Referenced by " << it->second.size() << " maps ***\n";
                
                // Check if object is valid in ANY of the referencing maps
                bool valid_in_any = false;
                bool valid_in_all = true;
                for (MapId m : it->second) {
                    uint8_t obj_count = map_object_counts[m];
                    bool valid = (fail.object_id >= 1 && fail.object_id <= obj_count);
                    if (valid) valid_in_any = true;
                    else valid_in_all = false;
                }
                
                if (valid_in_any && !valid_in_all) {
                    std::cout << "  *** CROSS-MAP REFERENCE: Object valid in some maps but not others ***\n";
                }
            }
        } else {
            std::cout << "  No map references found for this script!\n";
        }
        
        std::cout << "\n";
    }
    
    std::cout << "======================================\n";
    std::cout << "SECTION D: SUMMARY\n";
    std::cout << "======================================\n\n";
    
    std::cout << "Investigation complete. See above for detailed provenance.\n";
    
    return 0;
}

// Same map discovery as linker_test
std::vector<MapIdRef> discover_reachable_maps(const RomData& rom, 
    const ExtractionProfile& profile, MapExtractor& extractor) {
    
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
    
    enqueue(24, 4);
    enqueue(24, 7);
    
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
