// crystal/compile/corpus_discovery.cpp
// Unified corpus discovery implementation with fixed-point deferred discovery

#include "crystal/compile/corpus_discovery.hpp"
#include "crystal/compile/full_compiler.hpp"  // For discover_reachable_maps, MapIdRef
#include "crystal/script/crystal_command.hpp"
#include <queue>
#include <iostream>

namespace crystal {

//=============================================================================
// STATS IMPLEMENTATION
//=============================================================================

uint32_t CorpusDiscoveryStats::total_unique_bodies() const {
    // Map roots (deduplicated) + StdScript roots
    // Note: deferred_new_roots is already counted in total_map_roots()
    // std_script_roots is separate
    return total_map_roots() + std_script_roots;
}

//=============================================================================
// HELPER: EXTRACT SDEFER TARGETS FROM DECODED BODY
//=============================================================================

std::set<uint32_t> extract_sdefer_targets(
    const CrystalScriptIR& ir,
    uint8_t script_bank,
    const RomData& rom) {
    
    std::set<uint32_t> targets;
    
    for (const auto& cmd : ir.commands) {
        if (const auto* sdef = std::get_if<Cmd_Sdefer>(&cmd.data)) {
            // sdefer pointer is bank-relative; resolve to flat via canonical helper.
            // Source-proven: Script_sdefer (pokecrystal scripting.asm:1389) uses
            // wScriptBank (the calling script's bank) as the target bank.
            targets.insert(rom.bank_to_flat(script_bank, sdef->pointer));
        }
    }
    
    return targets;
}

//=============================================================================
// COLLECT INITIAL ROOTS (WITHOUT DEFERRED DISCOVERY)
//=============================================================================

CorpusDiscoveryResult collect_initial_roots(
    const RomData& rom,
    const ExtractionProfile& profile,
    MapExtractor& extractor,
    const StdScriptsTable& std_scripts) {
    
    CorpusDiscoveryResult result;
    
    // Crystal MapScripts structure sizes (from pokecrystal/constants/script_constants.asm)
    constexpr uint8_t SCENE_SCRIPT_SIZE = 4;  // dw script_ptr, dw 0 (filler)
    constexpr uint8_t CALLBACK_SIZE = 3;      // db type, dw script_ptr
    
    const auto& o = profile.offsets;
    const auto& fmt = profile.format.map;
    
    // Discover reachable maps
    auto discovered_maps = discover_reachable_maps(rom, profile, extractor);
    
    // Collect from all discovered maps
    for (const auto& ref : discovered_maps) {
        auto map_result = extractor.extract_map(ref.group, ref.map);
        if (!map_result.success) continue;
        
        enginemon::MapId map_id = (static_cast<uint16_t>(ref.group) << 8) | ref.map;
        
        // === Object event scripts ===
        uint8_t obj_idx = 0;
        for (const auto& obj : map_result.map.objects) {
            if (obj.script_rom_address != 0) {
                if (!result.map_roots.contains(obj.script_rom_address)) {
                    result.map_roots[obj.script_rom_address] = {
                        obj.script_rom_address,
                        ScriptRootType::Object,
                        map_id,
                        obj_idx,
                        0  // Initial root, not discovered from another body
                    };
                    ++result.stats.object_roots;
                }
            }
            ++obj_idx;
        }
        
        // === BG event scripts ===
        uint8_t bg_idx = 0;
        for (const auto& bg : map_result.map.bg_events) {
            if (bg.script_rom_address != 0) {
                if (!result.map_roots.contains(bg.script_rom_address)) {
                    result.map_roots[bg.script_rom_address] = {
                        bg.script_rom_address,
                        ScriptRootType::BgEvent,
                        map_id,
                        bg_idx,
                        0
                    };
                    ++result.stats.bg_event_roots;
                }
            }
            ++bg_idx;
        }
        
        // === Coord event scripts ===
        uint8_t coord_idx = 0;
        for (const auto& coord : map_result.map.coord_events) {
            if (coord.script_rom_address != 0) {
                if (!result.map_roots.contains(coord.script_rom_address)) {
                    result.map_roots[coord.script_rom_address] = {
                        coord.script_rom_address,
                        ScriptRootType::CoordEvent,
                        map_id,
                        coord_idx,
                        0
                    };
                    ++result.stats.coord_event_roots;
                }
            }
            ++coord_idx;
        }
        
        // === Scene scripts and callbacks from MapScripts header ===
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
        uint8_t script_bank = header[fmt.script_bank_offset];
        uint16_t script_ptr = header[fmt.script_ptr_offset] | (header[fmt.script_ptr_offset + 1] << 8);
        
        uint32_t map_scripts_addr = rom.bank_to_flat(script_bank, script_ptr);
        if (map_scripts_addr + 1 > rom.size()) continue;
        
        uint32_t ptr = map_scripts_addr;
        
        // --- Scene scripts ---
        uint8_t scene_count = rom.read_byte(ptr++);
        if (scene_count > 20) continue;
        
        for (uint8_t i = 0; i < scene_count; ++i) {
            if (ptr + SCENE_SCRIPT_SIZE > rom.size()) break;
            
            uint16_t scene_script_ptr = rom.read_word(ptr);
            ptr += 4;  // script_ptr + filler
            
            if (scene_script_ptr != 0) {
                uint32_t scene_script_addr = rom.bank_to_flat(script_bank, scene_script_ptr);
                if (scene_script_addr > 0 && scene_script_addr < rom.size()) {
                    if (!result.map_roots.contains(scene_script_addr)) {
                        result.map_roots[scene_script_addr] = {
                            scene_script_addr,
                            ScriptRootType::Scene,
                            map_id,
                            i,
                            0
                        };
                        ++result.stats.scene_roots;
                    }
                }
            }
        }
        
        // --- Callbacks ---
        if (ptr + 1 > rom.size()) continue;
        
        uint8_t callback_count = rom.read_byte(ptr++);
        if (callback_count > 20) continue;
        
        for (uint8_t i = 0; i < callback_count; ++i) {
            if (ptr + CALLBACK_SIZE > rom.size()) break;
            
            ptr++;  // Skip callback type
            uint16_t callback_ptr = rom.read_word(ptr);
            ptr += 2;
            
            if (callback_ptr != 0) {
                uint32_t callback_addr = rom.bank_to_flat(script_bank, callback_ptr);
                if (callback_addr > 0 && callback_addr < rom.size()) {
                    if (!result.map_roots.contains(callback_addr)) {
                        result.map_roots[callback_addr] = {
                            callback_addr,
                            ScriptRootType::Callback,
                            map_id,
                            i,
                            0
                        };
                        ++result.stats.callback_roots;
                    }
                }
            }
        }
    }
    
    // === StdScript addresses ===
    for (size_t i = 0; i < std_scripts.size(); ++i) {
        const auto* entry = std_scripts.get(static_cast<uint16_t>(i));
        if (entry && entry->flat_address != 0) {
            // Only add if not already in map_roots (dedup)
            if (!result.map_roots.contains(entry->flat_address)) {
                result.std_script_addresses.insert(entry->flat_address);
                ++result.stats.std_script_roots;
            }
        }
    }
    
    return result;
}

//=============================================================================
// FULL CORPUS DISCOVERY WITH FIXED-POINT DEFERRED DISCOVERY
//=============================================================================

CorpusDiscoveryResult discover_corpus(
    const RomData& rom,
    const ExtractionProfile& profile,
    MapExtractor& extractor,
    TypedScriptDecoder& decoder,
    const StdScriptsTable& std_scripts) {
    
    // Step 1: Collect initial roots
    CorpusDiscoveryResult result = collect_initial_roots(rom, profile, extractor, std_scripts);
    
    // Step 2: Fixed-point deferred discovery
    //
    // We need to decode each root body and scan for sdefer targets.
    // New targets become new roots, which may contain more sdefer targets.
    // Continue until no new roots are discovered.
    
    // Build map of script_bank for each root (needed to resolve sdefer pointers)
    // For map roots, we need to look up the script_bank from the map header
    std::map<uint32_t, uint8_t> address_to_script_bank;
    
    const auto& o = profile.offsets;
    const auto& fmt = profile.format.map;
    
    auto discovered_maps = discover_reachable_maps(rom, profile, extractor);
    
    // Build script_bank map for all roots
    for (const auto& ref : discovered_maps) {
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
        uint8_t script_bank = header[fmt.script_bank_offset];
        
        // Associate script_bank with all roots from this map
        enginemon::MapId map_id = (static_cast<uint16_t>(ref.group) << 8) | ref.map;
        
        for (const auto& [addr, info] : result.map_roots) {
            if (info.owning_map == map_id) {
                address_to_script_bank[addr] = script_bank;
            }
        }
    }
    
    // For StdScripts, infer bank from address
    for (uint32_t addr : result.std_script_addresses) {
        uint8_t bank = rom.flat_to_bank(addr);
        address_to_script_bank[addr] = bank;
    }
    
    // Queue of roots to process for deferred discovery
    std::queue<uint32_t> pending;
    std::set<uint32_t> decoded;  // Roots whose bodies we've scanned
    
    // Add all initial roots to pending queue
    for (const auto& [addr, info] : result.map_roots) {
        pending.push(addr);
    }
    for (uint32_t addr : result.std_script_addresses) {
        pending.push(addr);
    }
    
    // Fixed-point loop
    while (!pending.empty()) {
        ++result.stats.deferred_iterations;
        
        // Process one batch: decode all pending, collect new targets
        // Track which root discovered each target (target -> discovering_root)
        std::map<uint32_t, uint32_t> new_deferred_targets;
        
        while (!pending.empty()) {
            uint32_t addr = pending.front();
            pending.pop();
            
            if (decoded.contains(addr)) continue;
            decoded.insert(addr);
            
            // Get script_bank for this address
            uint8_t script_bank = 0;
            auto bank_it = address_to_script_bank.find(addr);
            if (bank_it != address_to_script_bank.end()) {
                script_bank = bank_it->second;
            } else {
                // Infer bank from address
                script_bank = rom.flat_to_bank(addr);
                address_to_script_bank[addr] = script_bank;
            }
            
            // Decode the script body
            try {
                CrystalScriptIR ir = decoder.decode_script(addr);
                
                // Extract sdefer targets via canonical bank helper
                auto targets = extract_sdefer_targets(ir, script_bank, rom);
                
                for (uint32_t target : targets) {
                    ++result.stats.deferred_targets_encountered;
                    
                    // Check if already known
                    bool is_map_root = result.map_roots.contains(target);
                    bool is_std_root = result.std_script_addresses.contains(target);
                    
                    if (is_map_root || is_std_root) {
                        ++result.stats.deferred_already_known;
                    } else if (!decoded.contains(target) && !new_deferred_targets.contains(target)) {
                        // New deferred target - record which root discovered it
                        new_deferred_targets[target] = addr;
                    }
                }
            } catch (const std::exception& e) {
                // Decode failure - skip this body
                // This is non-fatal for discovery (body will fail at compile time)
            }
        }
        
        // Add new deferred targets as roots and queue them
        for (const auto& [target, discovering_root] : new_deferred_targets) {
            if (!result.map_roots.contains(target) && 
                !result.std_script_addresses.contains(target)) {
                
                // Inherit owning_map from the actual discovering root
                enginemon::MapId owning_map = enginemon::MAP_NONE;
                
                auto root_it = result.map_roots.find(discovering_root);
                if (root_it != result.map_roots.end()) {
                    owning_map = root_it->second.owning_map;
                }
                // StdScript roots have no owning_map, which is correct (MAP_NONE)
                
                result.map_roots[target] = {
                    target,
                    ScriptRootType::Deferred,
                    owning_map,
                    static_cast<uint8_t>(result.stats.deferred_new_roots),
                    discovering_root
                };
                
                // Set script_bank for the new root
                uint8_t target_bank = rom.flat_to_bank(target);
                address_to_script_bank[target] = target_bank;
                
                ++result.stats.deferred_new_roots;
                pending.push(target);
            }
        }
        
        // If no new targets were added, we've reached fixed point
        if (new_deferred_targets.empty()) {
            break;
        }
    }
    
    return result;
}

} // namespace crystal
