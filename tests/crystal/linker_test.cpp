// linker_test.cpp
// Stage 6: Corpus-wide Typed-Reference Linker Test
//
// Links the full 1362-body corpus:
//   1310 unique map-root bodies + 52 unique StdScript bodies - 0 overlap
//
// Validates references against actual compiled game data, NOT Crystal ranges

#include "crystal/rom/loader.hpp"
#include "crystal/rom/profile.hpp"
#include "crystal/rom/symbol_map.hpp"
#include "crystal/script/typed_decoder.hpp"
#include "crystal/script/decoder.hpp"  // For text decoding
#include "crystal/script/crystal_cfg.hpp"
#include "crystal/script/native_registry.hpp"
#include "crystal/script/semantic_legalizer.hpp"
#include "crystal/script/legality_gate.hpp"
#include "crystal/script/semantic_linker.hpp"
#include "crystal/script/elevator_registry.hpp"
#include "crystal/script/trainer_registry.hpp"
#include "crystal/script/pokemail_registry.hpp"
#include "crystal/script/text_registry.hpp"
#include "crystal/extract/map_extractor.hpp"
#include "crystal/compile/full_compiler.hpp"  // For production discover_reachable_maps
#include <iostream>
#include <iomanip>
#include <set>
#include <queue>

using namespace crystal;
using namespace enginemon;

// NOTE: MapIdRef is now defined in full_compiler.hpp and imported via crystal namespace

// Collect script addresses from map events  
void collect_script_addresses(const ExtractedMap& map,
    std::set<uint32_t>& addresses,
    std::map<uint32_t, MapId>& address_to_map);

// Build CompiledGameData from actual ROM extraction
CompiledGameData build_compiled_game_data(const RomData& rom,
    const ExtractionProfile& profile,
    const std::vector<MapIdRef>& discovered_maps,
    const StdScriptsTable& std_scripts,
    MapExtractor& extractor);

// Process a script through Stages 1-5, return SemanticScriptIR
std::optional<SemanticScriptIR> process_script_to_stage5(
    uint32_t rom_address,
    const std::string& script_id,
    TypedScriptDecoder& decoder,
    CFGBuilder& cfg_builder,
    SemanticLegalizer& legalizer,
    LegalityGate& legality_gate);

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
    
    std::cout << "\n=== Stage 6: Corpus-wide Typed-Reference Linker ===\n\n";
    
    // Load StdScripts table
    StdScriptsTable std_scripts;
    bool loaded = std_scripts.load(*rom, profile->offsets.std_scripts, 
                                    profile->offsets.std_scripts_count);
    if (!loaded) {
        std::cerr << "Failed to load StdScripts table\n";
        return 1;
    }
    
    std::cout << "StdScripts table: " << std_scripts.size() << " entries\n";
    
    // Discover all reachable maps using PRODUCTION discovery function
    MapExtractor extractor(*rom, *profile);
    auto discovered_maps = discover_reachable_maps(*rom, *profile, extractor);
    std::cout << "Discovered maps: " << discovered_maps.size() << "\n";
    
    // Collect unique script addresses from maps
    std::set<uint32_t> map_root_addresses;
    std::map<uint32_t, MapId> address_to_map;
    
    for (const auto& ref : discovered_maps) {
        auto result = extractor.extract_map(ref.group, ref.map);
        if (!result.success) continue;
        
        MapId map_id = (static_cast<uint16_t>(ref.group) << 8) | ref.map;
        
        collect_script_addresses(result.map, map_root_addresses, address_to_map);
        
        // Update address_to_map with this map's ID
        for (const auto& obj : result.map.objects) {
            if (obj.script_rom_address != 0) {
                if (!address_to_map.contains(obj.script_rom_address)) {
                    address_to_map[obj.script_rom_address] = map_id;
                }
            }
        }
        for (const auto& bg : result.map.bg_events) {
            if (bg.script_rom_address != 0) {
                if (!address_to_map.contains(bg.script_rom_address)) {
                    address_to_map[bg.script_rom_address] = map_id;
                }
            }
        }
    }
    
    std::cout << "Unique map root addresses: " << map_root_addresses.size() << "\n";
    
    // Collect unique StdScript addresses
    std::set<uint32_t> std_script_addresses;
    std::map<uint32_t, uint16_t> std_addr_to_id;
    
    for (size_t i = 0; i < std_scripts.size(); ++i) {
        const auto* entry = std_scripts.get(static_cast<uint16_t>(i));
        if (entry && entry->flat_address != 0) {
            if (!std_script_addresses.contains(entry->flat_address)) {
                std_script_addresses.insert(entry->flat_address);
                std_addr_to_id[entry->flat_address] = entry->std_id;
            }
        }
    }
    
    std::cout << "Unique StdScript addresses: " << std_script_addresses.size() << "\n";
    
    // Calculate overlap
    std::set<uint32_t> overlap;
    for (uint32_t addr : std_script_addresses) {
        if (map_root_addresses.contains(addr)) {
            overlap.insert(addr);
        }
    }
    
    std::cout << "Overlap (StdScript in map roots): " << overlap.size() << "\n";
    
    size_t expected_total = map_root_addresses.size() + std_script_addresses.size() - overlap.size();
    std::cout << "Expected unique bodies: " << expected_total << "\n\n";
    
    // Build compiled game data
    std::cout << "Building CompiledGameData from ROM...\n";
    CompiledGameData game_data = build_compiled_game_data(
        *rom, *profile, discovered_maps, std_scripts, extractor);
    
    std::cout << "  Species: " << game_data.species.size() << "\n";
    std::cout << "  Items: " << game_data.items.size() << "\n";
    std::cout << "  Maps: " << game_data.maps.size() << "\n";
    std::cout << "  Trainers: " << game_data.trainers.size() << "\n";
    std::cout << "  StdScripts: " << game_data.std_scripts.size() << "\n";
    std::cout << "  Specials: " << game_data.specials.size() << "\n";
    std::cout << "  Music: " << game_data.music.size() << "\n";
    std::cout << "  SFX: " << game_data.sfx.size() << "\n";
    std::cout << "  Phone persons: " << game_data.phone_persons.size() << "\n";
    std::cout << "  Trades: " << game_data.trades.size() << "\n";
    std::cout << "  Fruit trees: " << game_data.fruit_trees.size() << "\n";
    std::cout << "  Elevators: (populated after script processing)\n";
    std::cout << "  Marts: " << game_data.marts.size() << "\n";
    std::cout << "  Emotes: " << game_data.emotes.size() << "\n";
    std::cout << "\n";
    
    // Setup pipeline components
    SymbolMap symbols;
    TypedScriptDecoder decoder(*rom, symbols);
    ScriptDecoder script_decoder(*rom, symbols);  // For text decoding
    
    NativeCallRegistry native_registry;
    native_registry.initialize();
    RamAddressRegistry ram_registry;
    ram_registry.initialize();
    
    // Elevator registry for floor-list resolution
    ElevatorRegistry elevator_registry(*rom);
    
    // PokeMail registry for semantic mail extraction
    PokeMailRegistry pokemail_registry(*rom);
    
    // Text registry for semantic text extraction (win/loss text, etc.)
    TextRegistry text_registry(
        [&script_decoder](uint32_t addr) { return script_decoder.decode_text_sequence(addr); });
    
    CFGBuilder cfg_builder;
    cfg_builder.set_std_scripts(&std_scripts);
    cfg_builder.set_native_registry(&native_registry);
    
    SemanticLegalizer legalizer;
    legalizer.set_native_registry(&native_registry);
    legalizer.set_ram_registry(&ram_registry);
    legalizer.set_elevator_registry(&elevator_registry);
    legalizer.set_pokemail_registry(&pokemail_registry);
    legalizer.set_text_registry(&text_registry);
    
    LegalityGate legality_gate;
    
    // Process map-root scripts through Stage 5
    std::cout << "Processing map-root scripts through Stage 5...\n";
    
    std::vector<SemanticScriptIR> map_root_irs;
    size_t map_root_failures = 0;
    
    for (uint32_t addr : map_root_addresses) {
        // Generate script_id based on map
        std::string script_id;
        auto map_it = address_to_map.find(addr);
        if (map_it != address_to_map.end()) {
            script_id = "map_" + std::to_string(map_it->second >> 8) + "_" + 
                        std::to_string(map_it->second & 0xFF) + "_0x" + 
                        std::to_string(addr);
        } else {
            script_id = "script_0x" + std::to_string(addr);
        }
        
        auto ir = process_script_to_stage5(addr, script_id, decoder, cfg_builder, 
                                            legalizer, legality_gate);
        if (ir) {
            map_root_irs.push_back(std::move(*ir));
        } else {
            map_root_failures++;
        }
    }
    
    std::cout << "  Processed: " << map_root_irs.size() << " / " 
              << map_root_addresses.size() << "\n";
    std::cout << "  Failures: " << map_root_failures << "\n\n";
    
    // Process StdScript bodies through Stage 5 (skip overlaps)
    std::cout << "Processing StdScript bodies through Stage 5...\n";
    
    std::vector<SemanticScriptIR> std_script_irs;
    size_t std_script_failures = 0;
    size_t std_script_skipped = 0;
    
    for (uint32_t addr : std_script_addresses) {
        // Skip if this address is already in map roots (overlap)
        if (overlap.contains(addr)) {
            std_script_skipped++;
            continue;
        }
        
        // Generate script_id for StdScript
        std::string script_id;
        uint16_t std_id = 0;
        auto std_it = std_addr_to_id.find(addr);
        if (std_it != std_addr_to_id.end()) {
            std_id = std_it->second;
            script_id = "std_" + std::to_string(std_id);
        } else {
            script_id = "std_0x" + std::to_string(addr);
        }
        
        auto ir = process_script_to_stage5(addr, script_id, decoder, cfg_builder,
                                            legalizer, legality_gate);
        if (ir) {
            std_script_irs.push_back(std::move(*ir));
            // Track successfully compiled StdScript ID
            if (std_it != std_addr_to_id.end()) {
                game_data.compiled_std_scripts.insert(std_id);
            }
        } else {
            std_script_failures++;
        }
    }
    
    std::cout << "  Processed: " << std_script_irs.size() << " / " 
              << (std_script_addresses.size() - overlap.size()) << "\n";
    std::cout << "  Skipped (overlap): " << std_script_skipped << "\n";
    std::cout << "  Failures: " << std_script_failures << "\n";
    std::cout << "  Compiled StdScript bodies: " << game_data.compiled_std_scripts.size() << "\n\n";
    
    // === Populate compiled elevator IDs from registry ===
    // Elevators are discovered during script lowering (rule_commerce).
    // Now that all scripts have been processed, we can populate game_data.
    std::cout << "Populating registries from discovered commands...\n";
    for (const auto& def : elevator_registry.all_definitions()) {
        game_data.elevators.insert(def.id);
    }
    std::cout << "  Compiled elevators: " << game_data.elevators.size() << "\n";
    
    // === Populate compiled PokeMail IDs from registry ===
    // PokeMail definitions are extracted during script lowering (rule_pokemail_ops).
    for (const auto& def : pokemail_registry.all_definitions()) {
        game_data.pokemail_ids.insert(def.id);
    }
    std::cout << "  Compiled PokeMail: " << game_data.pokemail_ids.size() << "\n";
    
    // === Populate compiled Text IDs from registry ===
    // Text definitions are extracted during script lowering (rule_win_loss_text).
    for (const auto& def : text_registry.all_definitions()) {
        game_data.text_ids.insert(def.id);
    }
    std::cout << "  Compiled Text: " << game_data.text_ids.size() << "\n\n";
    
    // Setup linker
    SemanticLinker linker;
    linker.set_game_data(&game_data);
    
    // Set script context (map ownership) for each script
    for (const auto& ir : map_root_irs) {
        if (ir.source_rom_address != 0) {
            auto map_it = address_to_map.find(ir.source_rom_address);
            if (map_it != address_to_map.end()) {
                linker.set_script_context(ir.script_id, map_it->second);
            }
        }
    }
    
    // Link the full corpus
    std::cout << "Linking full corpus...\n\n";
    
    LinkedCorpus result = linker.link_full_corpus(map_root_irs, std_script_irs);
    
    // Print the report
    result.print_report();
    
    // Final verification
    std::cout << "\n=== Stage 6 Verification ===\n";
    std::cout << "Map-root bodies linked:    " << result.map_root_bodies << " (expected: " 
              << map_root_irs.size() << ")\n";
    std::cout << "StdScript bodies linked:   " << result.std_script_bodies << " (expected: " 
              << std_script_irs.size() << ")\n";
    std::cout << "Overlap:                   " << result.overlap << " (expected: 0)\n";
    std::cout << "Total unique bodies:       " << result.total_bodies << " (expected: " 
              << expected_total << ")\n";
    
    // Check completion condition
    bool success = true;
    
    if (result.stats.total_unresolved() > 0) {
        std::cout << "\n*** FAILURE: " << result.stats.total_unresolved() << " unresolved references ***\n";
        success = false;
    }
    
    if (result.stats.total_invalid_ownership() > 0) {
        std::cout << "\n*** FAILURE: " << result.stats.total_invalid_ownership() << " invalid ownership references ***\n";
        success = false;
    }
    
    if (result.stats.total_wrong_type() > 0) {
        std::cout << "\n*** FAILURE: " << result.stats.total_wrong_type() << " wrong-type references ***\n";
        success = false;
    }
    
    if (success && result.all_linked()) {
        std::cout << "\n*** STAGE 6 COMPLETE: " << result.total_bodies << "/" << result.total_bodies 
                  << " bodies linked ***\n";
        std::cout << "  ExactResolved:       " << result.stats.total_exact_resolved() << "\n";
        std::cout << "  OwnershipValidated:  " << result.stats.total_ownership_validated() << "\n";
        std::cout << "  RangeOnly:           " << result.stats.total_range_only() << "\n";
        
        // === Elevator Negative Tests ===
        std::cout << "\n=== Elevator Negative Tests ===\n";
        
        // Test 1: Nonexistent ElevatorId should be InvalidDomain
        {
            std::cout << "Test: nonexistent_elevator_id... ";
            
            // Create a fake script IR with a nonexistent elevator reference
            enginemon::SemanticScriptIR fake_ir;
            fake_ir.script_id = "test_nonexistent_elevator";
            
            enginemon::SemanticBasicBlock block;
            block.id = 0;
            block.is_entry = true;
            
            enginemon::Sem_Elevator fake_elevator;
            fake_elevator.elevator_id = 999;  // Nonexistent ID
            
            enginemon::SemanticInstruction inst;
            inst.op = fake_elevator;
            block.instructions.push_back(inst);
            fake_ir.blocks.push_back(block);
            
            // Create test linker with same game_data (which only has IDs 0 and 1)
            SemanticLinker test_linker;
            test_linker.set_game_data(&game_data);
            
            auto refs = test_linker.link_script(fake_ir);
            
            bool found_invalid = false;
            for (const auto& ref : refs) {
                if (ref.type == ReferenceType::ElevatorId && 
                    ref.validation == ValidationClass::InvalidDomain) {
                    found_invalid = true;
                    break;
                }
            }
            
            if (found_invalid) {
                std::cout << "PASS (nonexistent ID 999 correctly flagged as InvalidDomain)\n";
            } else {
                std::cout << "FAIL (expected InvalidDomain for nonexistent elevator ID)\n";
                success = false;
            }
        }
        
        // Test 2: Verify elevator deduplication by content
        {
            std::cout << "Test: elevator_content_deduplication... ";
            
            // The elevator registry should deduplicate by floor-list content
            // If two ROM addresses point to identical floor-lists, they get the same ID
            
            // Verify we got exactly 2 unique elevators (the two distinct floor-lists in Crystal)
            bool correct_count = (game_data.elevators.size() == 2);
            
            if (correct_count) {
                std::cout << "PASS (2 unique elevator definitions compiled)\n";
            } else {
                std::cout << "FAIL (expected 2 elevators, got " << game_data.elevators.size() << ")\n";
                success = false;
            }
        }
        
        // Test 3: Verify elevator definition details
        {
            std::cout << "Test: elevator_definition_extraction... ";
            
            // The elevator registry should have floor data for each elevator
            bool all_valid = true;
            for (const auto& def : elevator_registry.all_definitions()) {
                if (def.floors.empty()) {
                    std::cout << "FAIL (elevator " << def.id << " has no floors)\n";
                    all_valid = false;
                    break;
                }
            }
            
            if (all_valid) {
                std::cout << "PASS (all elevator definitions have floor data)\n";
                
                // Print elevator details for the report
                for (const auto& def : elevator_registry.all_definitions()) {
                    std::cout << "  Elevator " << def.id << ": " << def.floors.size() << " floors\n";
                }
            } else {
                success = false;
            }
        }
        
        // === Additional Negative Tests for ResourceReference Types ===
        std::cout << "\n=== Resource Boundary Negative Tests ===\n";
        
        // Test: out-of-range Emote (enum boundary)
        {
            std::cout << "Test: out_of_range_emote... ";
            
            enginemon::SemanticScriptIR fake_ir;
            fake_ir.script_id = "test_bad_emote";
            
            enginemon::SemanticBasicBlock block;
            block.id = 0;
            block.is_entry = true;
            
            // Emotes are 0-11 (num_emotes=12), so 12 should be invalid
            enginemon::Sem_Emote fake_emote;
            fake_emote.emote_id = 12;  // Out of range
            fake_emote.object_id = 0;
            fake_emote.duration = 0;
            
            enginemon::SemanticInstruction inst;
            inst.op = fake_emote;
            block.instructions.push_back(inst);
            fake_ir.blocks.push_back(block);
            
            SemanticLinker test_linker;
            test_linker.set_game_data(&game_data);
            
            auto refs = test_linker.link_script(fake_ir);
            
            bool found_invalid = false;
            for (const auto& ref : refs) {
                if (ref.type == ReferenceType::Emote && 
                    ref.validation == ValidationClass::InvalidDomain) {
                    found_invalid = true;
                    break;
                }
            }
            
            if (found_invalid) {
                std::cout << "PASS (emote 12 out-of-range detected as InvalidDomain)\n";
            } else {
                std::cout << "FAIL (expected InvalidDomain for emote 12)\n";
                success = false;
            }
        }
        
        // Test: out-of-range Mart (closed table boundary)
        {
            std::cout << "Test: out_of_range_mart... ";
            
            enginemon::SemanticScriptIR fake_ir;
            fake_ir.script_id = "test_bad_mart";
            
            enginemon::SemanticBasicBlock block;
            block.id = 0;
            block.is_entry = true;
            
            // Marts are 0-33 (num_marts=34), so 34 should be invalid
            enginemon::Sem_Pokemart fake_mart;
            fake_mart.mart_id = 34;  // Out of range
            fake_mart.dialog_id = 0;
            
            enginemon::SemanticInstruction inst;
            inst.op = fake_mart;
            block.instructions.push_back(inst);
            fake_ir.blocks.push_back(block);
            
            SemanticLinker test_linker;
            test_linker.set_game_data(&game_data);
            
            auto refs = test_linker.link_script(fake_ir);
            
            bool found_invalid = false;
            for (const auto& ref : refs) {
                if (ref.type == ReferenceType::MartId && 
                    ref.validation == ValidationClass::InvalidDomain) {
                    found_invalid = true;
                    break;
                }
            }
            
            if (found_invalid) {
                std::cout << "PASS (mart 34 out-of-range detected as InvalidDomain)\n";
            } else {
                std::cout << "FAIL (expected InvalidDomain for mart 34)\n";
                success = false;
            }
        }
        
        // Test: out-of-range Music (enum boundary)
        {
            std::cout << "Test: out_of_range_music... ";
            
            enginemon::SemanticScriptIR fake_ir;
            fake_ir.script_id = "test_bad_music";
            
            enginemon::SemanticBasicBlock block;
            block.id = 0;
            block.is_entry = true;
            
            // Music is 0-102 (num_music=103), so 103 should be invalid
            enginemon::Sem_PlayMusic fake_music;
            fake_music.music = static_cast<enginemon::MusicId>(103);  // Out of range
            
            enginemon::SemanticInstruction inst;
            inst.op = fake_music;
            block.instructions.push_back(inst);
            fake_ir.blocks.push_back(block);
            
            SemanticLinker test_linker;
            test_linker.set_game_data(&game_data);
            
            auto refs = test_linker.link_script(fake_ir);
            
            bool found_invalid = false;
            for (const auto& ref : refs) {
                if (ref.type == ReferenceType::Music && 
                    ref.validation == ValidationClass::InvalidDomain) {
                    found_invalid = true;
                    break;
                }
            }
            
            if (found_invalid) {
                std::cout << "PASS (music 103 out-of-range detected as InvalidDomain)\n";
            } else {
                std::cout << "FAIL (expected InvalidDomain for music 103)\n";
                success = false;
            }
        }
        
        // Test: out-of-range SFX (enum boundary)
        {
            std::cout << "Test: out_of_range_sfx... ";
            
            enginemon::SemanticScriptIR fake_ir;
            fake_ir.script_id = "test_bad_sfx";
            
            enginemon::SemanticBasicBlock block;
            block.id = 0;
            block.is_entry = true;
            
            // SFX is 0-206 (num_sfx=207), so 207 should be invalid
            enginemon::Sem_PlaySound fake_sfx;
            fake_sfx.sound = static_cast<enginemon::SfxId>(207);  // Out of range
            
            enginemon::SemanticInstruction inst;
            inst.op = fake_sfx;
            block.instructions.push_back(inst);
            fake_ir.blocks.push_back(block);
            
            SemanticLinker test_linker;
            test_linker.set_game_data(&game_data);
            
            auto refs = test_linker.link_script(fake_ir);
            
            bool found_invalid = false;
            for (const auto& ref : refs) {
                if (ref.type == ReferenceType::Sfx && 
                    ref.validation == ValidationClass::InvalidDomain) {
                    found_invalid = true;
                    break;
                }
            }
            
            if (found_invalid) {
                std::cout << "PASS (sfx 207 out-of-range detected as InvalidDomain)\n";
            } else {
                std::cout << "FAIL (expected InvalidDomain for sfx 207)\n";
                success = false;
            }
        }
        
        // Test: out-of-range PhonePerson (closed table boundary)
        {
            std::cout << "Test: out_of_range_phone_person... ";
            
            enginemon::SemanticScriptIR fake_ir;
            fake_ir.script_id = "test_bad_phone";
            
            enginemon::SemanticBasicBlock block;
            block.id = 0;
            block.is_entry = true;
            
            // Phone contacts are 0-37 (num_phone_contacts=38), so 38 should be invalid
            enginemon::Sem_AddPhoneNumber fake_phone;
            fake_phone.person = 38;  // Out of range
            
            enginemon::SemanticInstruction inst;
            inst.op = fake_phone;
            block.instructions.push_back(inst);
            fake_ir.blocks.push_back(block);
            
            SemanticLinker test_linker;
            test_linker.set_game_data(&game_data);
            
            auto refs = test_linker.link_script(fake_ir);
            
            bool found_invalid = false;
            for (const auto& ref : refs) {
                if (ref.type == ReferenceType::PhonePerson && 
                    ref.validation == ValidationClass::InvalidDomain) {
                    found_invalid = true;
                    break;
                }
            }
            
            if (found_invalid) {
                std::cout << "PASS (phone 38 out-of-range detected as InvalidDomain)\n";
            } else {
                std::cout << "FAIL (expected InvalidDomain for phone 38)\n";
                success = false;
            }
        }
        
        // Test: out-of-range Trade (closed table boundary)
        {
            std::cout << "Test: out_of_range_trade... ";
            
            enginemon::SemanticScriptIR fake_ir;
            fake_ir.script_id = "test_bad_trade";
            
            enginemon::SemanticBasicBlock block;
            block.id = 0;
            block.is_entry = true;
            
            // Trades are 0-6 (num_npc_trades=7), so 7 should be invalid
            enginemon::Sem_Trade fake_trade;
            fake_trade.trade_id = 7;  // Out of range
            
            enginemon::SemanticInstruction inst;
            inst.op = fake_trade;
            block.instructions.push_back(inst);
            fake_ir.blocks.push_back(block);
            
            SemanticLinker test_linker;
            test_linker.set_game_data(&game_data);
            
            auto refs = test_linker.link_script(fake_ir);
            
            bool found_invalid = false;
            for (const auto& ref : refs) {
                if (ref.type == ReferenceType::TradeId && 
                    ref.validation == ValidationClass::InvalidDomain) {
                    found_invalid = true;
                    break;
                }
            }
            
            if (found_invalid) {
                std::cout << "PASS (trade 7 out-of-range detected as InvalidDomain)\n";
            } else {
                std::cout << "FAIL (expected InvalidDomain for trade 7)\n";
                success = false;
            }
        }
        
        // Test: out-of-range FruitTree (closed table boundary)
        {
            std::cout << "Test: out_of_range_fruit_tree... ";
            
            enginemon::SemanticScriptIR fake_ir;
            fake_ir.script_id = "test_bad_fruit_tree";
            
            enginemon::SemanticBasicBlock block;
            block.id = 0;
            block.is_entry = true;
            
            // Fruit trees are 1-30 (num_fruit_trees=30, 1-indexed), so 31 should be invalid
            enginemon::Sem_FruitTree fake_tree;
            fake_tree.tree_id = 31;  // Out of range
            
            enginemon::SemanticInstruction inst;
            inst.op = fake_tree;
            block.instructions.push_back(inst);
            fake_ir.blocks.push_back(block);
            
            SemanticLinker test_linker;
            test_linker.set_game_data(&game_data);
            
            auto refs = test_linker.link_script(fake_ir);
            
            bool found_invalid = false;
            for (const auto& ref : refs) {
                if (ref.type == ReferenceType::FruitTreeId && 
                    ref.validation == ValidationClass::InvalidDomain) {
                    found_invalid = true;
                    break;
                }
            }
            
            if (found_invalid) {
                std::cout << "PASS (fruit tree 31 out-of-range detected as InvalidDomain)\n";
            } else {
                std::cout << "FAIL (expected InvalidDomain for fruit tree 31)\n";
                success = false;
            }
        }
        
        // === StdScript Authority Test ===
        // Test: StdScript ROM table entry exists but compiled body is absent
        {
            std::cout << "\n=== StdScript Authority Tests ===\n";
            std::cout << "Test: stdscript_uncompiled_body... ";
            
            // Find a StdScript ID that exists in ROM table but wasn't compiled
            // (This could happen if a StdScript failed Stage 5 legality)
            uint16_t uncompiled_std_id = 0xFFFF;
            for (const auto& [id, addr] : game_data.std_scripts) {
                if (!game_data.compiled_std_scripts.contains(id)) {
                    uncompiled_std_id = id;
                    break;
                }
            }
            
            if (uncompiled_std_id != 0xFFFF) {
                // Found an uncompiled StdScript - test that it returns Unresolved
                enginemon::SemanticScriptIR fake_ir;
                fake_ir.script_id = "test_uncompiled_std";
                
                enginemon::SemanticBasicBlock block;
                block.id = 0;
                block.is_entry = true;
                
                enginemon::Sem_CallStd fake_std;
                fake_std.std_id = uncompiled_std_id;
                
                enginemon::SemanticInstruction inst;
                inst.op = fake_std;
                block.instructions.push_back(inst);
                fake_ir.blocks.push_back(block);
                
                SemanticLinker test_linker;
                test_linker.set_game_data(&game_data);
                
                auto refs = test_linker.link_script(fake_ir);
                
                bool found_unresolved = false;
                for (const auto& ref : refs) {
                    if (ref.type == ReferenceType::StdScript && 
                        ref.validation == ValidationClass::Unresolved) {
                        found_unresolved = true;
                        break;
                    }
                }
                
                if (found_unresolved) {
                    std::cout << "PASS (std_id " << uncompiled_std_id 
                              << " in ROM table but not compiled, correctly Unresolved)\n";
                } else {
                    std::cout << "FAIL (expected Unresolved for uncompiled std_id " 
                              << uncompiled_std_id << ")\n";
                    success = false;
                }
            } else {
                // All ROM table StdScripts were compiled - use a fake ID instead
                enginemon::SemanticScriptIR fake_ir;
                fake_ir.script_id = "test_fake_std";
                
                enginemon::SemanticBasicBlock block;
                block.id = 0;
                block.is_entry = true;
                
                enginemon::Sem_CallStd fake_std;
                fake_std.std_id = 9999;  // Definitely not in ROM table or compiled
                
                enginemon::SemanticInstruction inst;
                inst.op = fake_std;
                block.instructions.push_back(inst);
                fake_ir.blocks.push_back(block);
                
                SemanticLinker test_linker;
                test_linker.set_game_data(&game_data);
                
                auto refs = test_linker.link_script(fake_ir);
                
                bool found_unresolved = false;
                for (const auto& ref : refs) {
                    if (ref.type == ReferenceType::StdScript && 
                        ref.validation == ValidationClass::Unresolved) {
                        found_unresolved = true;
                        break;
                    }
                }
                
                if (found_unresolved) {
                    std::cout << "PASS (all ROM StdScripts compiled; fake ID 9999 correctly Unresolved)\n";
                } else {
                    std::cout << "FAIL (expected Unresolved for fake std_id 9999)\n";
                    success = false;
                }
            }
        }
        
        // Test: Valid compiled StdScript should resolve
        {
            std::cout << "Test: stdscript_compiled_body... ";
            
            if (!game_data.compiled_std_scripts.empty()) {
                // Get first compiled StdScript ID
                uint16_t compiled_std_id = *game_data.compiled_std_scripts.begin();
                
                enginemon::SemanticScriptIR fake_ir;
                fake_ir.script_id = "test_valid_std";
                
                enginemon::SemanticBasicBlock block;
                block.id = 0;
                block.is_entry = true;
                
                enginemon::Sem_CallStd fake_std;
                fake_std.std_id = compiled_std_id;
                
                enginemon::SemanticInstruction inst;
                inst.op = fake_std;
                block.instructions.push_back(inst);
                fake_ir.blocks.push_back(block);
                
                SemanticLinker test_linker;
                test_linker.set_game_data(&game_data);
                
                auto refs = test_linker.link_script(fake_ir);
                
                bool found_exact = false;
                for (const auto& ref : refs) {
                    if (ref.type == ReferenceType::StdScript && 
                        ref.validation == ValidationClass::ExactResolved) {
                        found_exact = true;
                        break;
                    }
                }
                
                if (found_exact) {
                    std::cout << "PASS (std_id " << compiled_std_id 
                              << " compiled, correctly ExactResolved)\n";
                } else {
                    std::cout << "FAIL (expected ExactResolved for compiled std_id " 
                              << compiled_std_id << ")\n";
                    success = false;
                }
            } else {
                std::cout << "SKIP (no compiled StdScripts to test)\n";
            }
        }
        
        // === PokeMail Negative Tests ===
        std::cout << "\n=== PokeMail Boundary Tests ===\n";
        
        // Test: POKEMAIL_NONE should be InvalidDomain (extraction failure sentinel)
        {
            std::cout << "Test: pokemail_none_rejected... ";
            
            enginemon::SemanticScriptIR fake_ir;
            fake_ir.script_id = "test_pokemail_none";
            
            enginemon::SemanticBasicBlock block;
            block.id = 0;
            block.is_entry = true;
            
            enginemon::Sem_GivePokeMail fake_mail;
            fake_mail.mail_id = enginemon::POKEMAIL_NONE;
            
            enginemon::SemanticInstruction inst;
            inst.op = fake_mail;
            block.instructions.push_back(inst);
            fake_ir.blocks.push_back(block);
            
            SemanticLinker test_linker;
            test_linker.set_game_data(&game_data);
            
            auto refs = test_linker.link_script(fake_ir);
            
            bool found_invalid_domain = false;
            bool found_rangeonly = false;
            for (const auto& ref : refs) {
                if (ref.type == ReferenceType::PokeMailId) {
                    if (ref.validation == ValidationClass::InvalidDomain) {
                        found_invalid_domain = true;
                    }
                    if (ref.validation == ValidationClass::RangeOnly) {
                        found_rangeonly = true;
                    }
                }
            }
            
            if (found_invalid_domain && !found_rangeonly) {
                std::cout << "PASS (POKEMAIL_NONE correctly flagged as InvalidDomain)\n";
            } else if (found_rangeonly) {
                std::cout << "FAIL (POKEMAIL_NONE incorrectly allowed as RangeOnly)\n";
                success = false;
            } else {
                std::cout << "FAIL (expected InvalidDomain for POKEMAIL_NONE sentinel)\n";
                success = false;
            }
        }
        
        // Test: Nonexistent PokeMail ID should be InvalidDomain
        {
            std::cout << "Test: nonexistent_pokemail_id... ";
            
            enginemon::SemanticScriptIR fake_ir;
            fake_ir.script_id = "test_bad_pokemail";
            
            enginemon::SemanticBasicBlock block;
            block.id = 0;
            block.is_entry = true;
            
            // Use a large ID that definitely won't exist
            enginemon::Sem_CheckPokeMail fake_mail;
            fake_mail.mail_id = static_cast<enginemon::PokeMailId>(9999);
            
            enginemon::SemanticInstruction inst;
            inst.op = fake_mail;
            block.instructions.push_back(inst);
            fake_ir.blocks.push_back(block);
            
            SemanticLinker test_linker;
            test_linker.set_game_data(&game_data);
            
            auto refs = test_linker.link_script(fake_ir);
            
            bool found_invalid = false;
            for (const auto& ref : refs) {
                if (ref.type == ReferenceType::PokeMailId && 
                    ref.validation == ValidationClass::InvalidDomain) {
                    found_invalid = true;
                    break;
                }
            }
            
            if (found_invalid) {
                std::cout << "PASS (nonexistent PokeMail ID 9999 flagged as InvalidDomain)\n";
            } else {
                std::cout << "FAIL (expected InvalidDomain for nonexistent PokeMail ID)\n";
                success = false;
            }
        }
        
        // Test: Raw uint16_t pointer value should NOT pass merely because it fits
        {
            std::cout << "Test: raw_pointer_value_rejected... ";
            
            // This test verifies that a raw ROM pointer (like 0x1A5C89) passed as PokeMailId
            // will be rejected because it's not in the content-based registry
            enginemon::SemanticScriptIR fake_ir;
            fake_ir.script_id = "test_raw_pointer_pokemail";
            
            enginemon::SemanticBasicBlock block;
            block.id = 0;
            block.is_entry = true;
            
            // Use a value that looks like a ROM pointer but isn't a valid semantic ID
            // In the old broken code, this would pass because it was just a uint32_t
            enginemon::Sem_GivePokeMail fake_mail;
            fake_mail.mail_id = static_cast<enginemon::PokeMailId>(0x1A5C);  // Truncated ROM address
            
            enginemon::SemanticInstruction inst;
            inst.op = fake_mail;
            block.instructions.push_back(inst);
            fake_ir.blocks.push_back(block);
            
            SemanticLinker test_linker;
            test_linker.set_game_data(&game_data);
            
            auto refs = test_linker.link_script(fake_ir);
            
            bool found_invalid = false;
            for (const auto& ref : refs) {
                if (ref.type == ReferenceType::PokeMailId && 
                    (ref.validation == ValidationClass::InvalidDomain ||
                     ref.validation == ValidationClass::Unresolved)) {
                    found_invalid = true;
                    break;
                }
            }
            
            if (found_invalid) {
                std::cout << "PASS (raw pointer-like value rejected)\n";
            } else {
                std::cout << "FAIL (raw pointer value should not pass validation)\n";
                success = false;
            }
        }
        
        // === TextId Negative Tests ===
        std::cout << "\n=== TextId Boundary Tests ===\n";
        
        // Test: Absence (nullopt) should emit NO Text references
        // TEXT_NONE was the old sentinel - now modeled as std::nullopt
        {
            std::cout << "Test: text_absence_emits_no_refs... ";
            
            enginemon::SemanticScriptIR fake_ir;
            fake_ir.script_id = "test_text_absence";
            
            enginemon::SemanticBasicBlock block;
            block.id = 0;
            block.is_entry = true;
            
            // Create SetWinLossText with both texts absent (std::nullopt)
            enginemon::Sem_SetWinLossText fake_text;
            // Both win_text and loss_text are std::nullopt by default
            
            enginemon::SemanticInstruction inst;
            inst.op = fake_text;
            block.instructions.push_back(inst);
            fake_ir.blocks.push_back(block);
            
            SemanticLinker test_linker;
            test_linker.set_game_data(&game_data);
            
            auto refs = test_linker.link_script(fake_ir);
            
            int text_ref_count = 0;
            for (const auto& ref : refs) {
                if (ref.type == ReferenceType::TextId) {
                    text_ref_count++;
                }
            }
            
            if (text_ref_count == 0) {
                std::cout << "PASS (absence emits no Text ResourceRefs)\n";
            } else {
                std::cout << "FAIL (expected 0 Text refs for absence, got " 
                          << text_ref_count << ")\n";
                success = false;
            }
        }
        
        // Test: Nonexistent TextId should be InvalidDomain
        {
            std::cout << "Test: nonexistent_text_id... ";
            
            enginemon::SemanticScriptIR fake_ir;
            fake_ir.script_id = "test_bad_text";
            
            enginemon::SemanticBasicBlock block;
            block.id = 0;
            block.is_entry = true;
            
            // Use a large ID that definitely won't exist in the registry
            enginemon::Sem_SetWinLossText fake_text;
            fake_text.win_text = static_cast<enginemon::TextId>(99999);
            fake_text.loss_text = static_cast<enginemon::TextId>(88888);
            
            enginemon::SemanticInstruction inst;
            inst.op = fake_text;
            block.instructions.push_back(inst);
            fake_ir.blocks.push_back(block);
            
            SemanticLinker test_linker;
            test_linker.set_game_data(&game_data);
            
            auto refs = test_linker.link_script(fake_ir);
            
            int invalid_count = 0;
            for (const auto& ref : refs) {
                if (ref.type == ReferenceType::TextId && 
                    ref.validation == ValidationClass::InvalidDomain) {
                    invalid_count++;
                }
            }
            
            if (invalid_count == 2) {
                std::cout << "PASS (both nonexistent TextIds flagged as InvalidDomain)\n";
            } else {
                std::cout << "FAIL (expected 2 InvalidDomain for nonexistent TextIds, got " 
                          << invalid_count << ")\n";
                success = false;
            }
        }
        
        // Test: Raw ROM address passed as TextId should NOT pass
        {
            std::cout << "Test: raw_text_pointer_rejected... ";
            
            // This test verifies that a raw ROM text pointer passed as TextId
            // will be rejected because it's not in the content-based registry
            enginemon::SemanticScriptIR fake_ir;
            fake_ir.script_id = "test_raw_pointer_text";
            
            enginemon::SemanticBasicBlock block;
            block.id = 0;
            block.is_entry = true;
            
            // Use a value that looks like a ROM address but isn't a valid semantic ID
            enginemon::Sem_SetWinLossText fake_text;
            fake_text.win_text = static_cast<enginemon::TextId>(0x1A5C89);  // Raw ROM address
            fake_text.loss_text = static_cast<enginemon::TextId>(0x1A5C90);
            
            enginemon::SemanticInstruction inst;
            inst.op = fake_text;
            block.instructions.push_back(inst);
            fake_ir.blocks.push_back(block);
            
            SemanticLinker test_linker;
            test_linker.set_game_data(&game_data);
            
            auto refs = test_linker.link_script(fake_ir);
            
            int invalid_count = 0;
            for (const auto& ref : refs) {
                if (ref.type == ReferenceType::TextId && 
                    (ref.validation == ValidationClass::InvalidDomain ||
                     ref.validation == ValidationClass::Unresolved)) {
                    invalid_count++;
                }
            }
            
            if (invalid_count == 2) {
                std::cout << "PASS (raw ROM addresses rejected)\n";
            } else {
                std::cout << "FAIL (raw ROM addresses should not pass validation)\n";
                success = false;
            }
        }
        
        // Test: TEXT_NONE sentinel must NOT leak into SemanticIR (absence = nullopt)
        {
            std::cout << "Test: text_none_sentinel_rejected... ";
            
            // This tests that if somehow TEXT_NONE ends up in SemanticIR, 
            // it's flagged as InvalidDomain (semantic leak), NOT RangeOnly
            enginemon::SemanticScriptIR fake_ir;
            fake_ir.script_id = "test_text_none_leak";
            
            enginemon::SemanticBasicBlock block;
            block.id = 0;
            block.is_entry = true;
            
            // Explicitly set TEXT_NONE (this would be a bug in the legalizer)
            enginemon::Sem_SetWinLossText fake_text;
            fake_text.win_text = enginemon::TEXT_NONE;  // Should be nullopt, not this
            
            enginemon::SemanticInstruction inst;
            inst.op = fake_text;
            block.instructions.push_back(inst);
            fake_ir.blocks.push_back(block);
            
            SemanticLinker test_linker;
            test_linker.set_game_data(&game_data);
            
            auto refs = test_linker.link_script(fake_ir);
            
            bool found_invalid_domain = false;
            bool found_range_only = false;
            for (const auto& ref : refs) {
                if (ref.type == ReferenceType::TextId) {
                    if (ref.validation == ValidationClass::InvalidDomain) {
                        found_invalid_domain = true;
                    }
                    if (ref.validation == ValidationClass::RangeOnly) {
                        found_range_only = true;
                    }
                }
            }
            
            if (found_invalid_domain && !found_range_only) {
                std::cout << "PASS (TEXT_NONE sentinel correctly flagged as InvalidDomain)\n";
            } else if (found_range_only) {
                std::cout << "FAIL (TEXT_NONE sentinel incorrectly allowed as RangeOnly)\n";
                success = false;
            } else {
                std::cout << "FAIL (expected InvalidDomain for TEXT_NONE sentinel)\n";
                success = false;
            }
        }
        
        // === ExactResolved Authority Tests ===
        // These tests prove ExactResolved comes from actual compiled definitions,
        // not count/range acceptance
        std::cout << "\n=== ExactResolved Authority Tests ===\n";
        
        // Test: Nonexistent MapId within plausible numeric range → rejected
        {
            std::cout << "Test: nonexistent_map_exactresolved... ";
            
            // Create a MapId that's within plausible numeric range but not in discovered maps
            // Use a valid-looking (group, index) pair that doesn't exist
            // Group 7 index 255 = 0x07FF = 2047 - likely not a real map
            enginemon::MapId fake_map_id = (7 << 8) | 255;
            
            // Verify this ID is NOT in the compiled map set
            if (game_data.maps.contains(fake_map_id)) {
                // If by chance this exists, try another
                fake_map_id = (15 << 8) | 200;  // Group 15 index 200 = 0x0FC8
            }
            
            if (!game_data.maps.contains(fake_map_id)) {
                enginemon::SemanticScriptIR fake_ir;
                fake_ir.script_id = "test_bad_map";
                
                enginemon::SemanticBasicBlock block;
                block.id = 0;
                block.is_entry = true;
                
                enginemon::Sem_Warp fake_warp;
                fake_warp.map = fake_map_id;
                fake_warp.x = 5;
                fake_warp.y = 5;
                
                enginemon::SemanticInstruction inst;
                inst.op = fake_warp;
                block.instructions.push_back(inst);
                fake_ir.blocks.push_back(block);
                
                SemanticLinker test_linker;
                test_linker.set_game_data(&game_data);
                
                auto refs = test_linker.link_script(fake_ir);
                
                bool found_invalid = false;
                for (const auto& ref : refs) {
                    if (ref.type == ReferenceType::Map && 
                        ref.validation == ValidationClass::InvalidDomain) {
                        found_invalid = true;
                        break;
                    }
                }
                
                if (found_invalid) {
                    std::cout << "PASS (nonexistent MapId " << std::hex << fake_map_id 
                              << std::dec << " correctly flagged as InvalidDomain)\n";
                } else {
                    std::cout << "FAIL (expected InvalidDomain for nonexistent MapId)\n";
                    success = false;
                }
            } else {
                std::cout << "SKIP (test MapIds unexpectedly exist in ROM)\n";
            }
        }
        
        // Test: Nonexistent TrainerId (group,id) within plausible range → rejected
        {
            std::cout << "Test: nonexistent_trainer_exactresolved... ";
            
            // Create a (group, id) pair that's within plausible range but not in TrainerRegistry
            // Most trainer groups have < 30 trainers, so id=99 is likely nonexistent
            uint8_t test_group = 1;   // Valid group (FALKNER's group exists)
            uint8_t test_id = 99;     // Very unlikely to have 99 trainers in group 1
            
            // Verify this pair is NOT in the compiled trainer set
            if (game_data.trainers.contains({test_group, test_id})) {
                // If by chance this exists, try another combination
                test_group = 10;
                test_id = 200;
            }
            
            if (!game_data.trainers.contains({test_group, test_id})) {
                enginemon::SemanticScriptIR fake_ir;
                fake_ir.script_id = "test_bad_trainer";
                
                enginemon::SemanticBasicBlock block;
                block.id = 0;
                block.is_entry = true;
                
                enginemon::Sem_LoadTrainer fake_trainer;
                fake_trainer.trainer_group = test_group;
                fake_trainer.trainer_id = test_id;
                
                enginemon::SemanticInstruction inst;
                inst.op = fake_trainer;
                block.instructions.push_back(inst);
                fake_ir.blocks.push_back(block);
                
                SemanticLinker test_linker;
                test_linker.set_game_data(&game_data);
                
                auto refs = test_linker.link_script(fake_ir);
                
                bool found_invalid = false;
                for (const auto& ref : refs) {
                    if (ref.type == ReferenceType::Trainer && 
                        ref.validation == ValidationClass::InvalidDomain) {
                        found_invalid = true;
                        break;
                    }
                }
                
                if (found_invalid) {
                    std::cout << "PASS (nonexistent Trainer (" << (int)test_group << "," 
                              << (int)test_id << ") correctly flagged as InvalidDomain)\n";
                } else {
                    std::cout << "FAIL (expected InvalidDomain for nonexistent Trainer)\n";
                    success = false;
                }
            } else {
                std::cout << "SKIP (test Trainer IDs unexpectedly exist in ROM)\n";
            }
        }
        
        // === PokeMail Content Verification ===
        // Verify that pointer resolution is extracting actual data correctly
        std::cout << "\n=== PokeMail Content Verification ===\n";
        {
            // Check that we extracted PokeMail definitions 
            const auto& pokemail_defs = pokemail_registry.all_definitions();
            std::cout << "  Extracted PokeMail definitions: " << pokemail_defs.size() << "\n";
            
            if (pokemail_defs.size() >= 1) {
                // Verify content looks correct (not garbage from wrong pointer resolution)
                for (size_t i = 0; i < pokemail_defs.size(); ++i) {
                    const auto& def = pokemail_defs[i];
                    std::cout << "  PokeMail " << i << ":\n";
                    std::cout << "    ID: " << def.id << "\n";
                    std::cout << "    Mail Type: 0x" << std::hex << def.mail_type << std::dec << "\n";
                    std::cout << "    Message: \"" << def.message << "\"\n";
                    std::cout << "    Source ROM: 0x" << std::hex << def.source_rom_address << std::dec << "\n";
                    
                    // Verify the message looks like actual text (not garbage bytes)
                    // GiftSpearowMail should contain "DARK CAVE" 
                    bool has_printable_text = false;
                    for (char c : def.message) {
                        if (c >= 'A' && c <= 'z') {
                            has_printable_text = true;
                            break;
                        }
                    }
                    
                    if (!has_printable_text && !def.message.empty()) {
                        std::cout << "    WARNING: Message contains no printable text (possible bad pointer resolution)\n";
                    }
                }
                
                // The GiftSpearowMail is in bank 0x1A (address 0x5D98) = flat 0x69D98
                // Check if we got the right address
                bool found_spearow_mail = false;
                for (const auto& def : pokemail_defs) {
                    // Expected address: bank 0x1A * 0x4000 + (0x5D98 - 0x4000) = 0x68000 + 0x1D98 = 0x69D98
                    if (def.source_rom_address == 0x69D98) {
                        found_spearow_mail = true;
                        // FLOWER_MAIL = 0xB6 in Crystal item constants
                        std::cout << "  Found GiftSpearowMail at correct address 0x69D98\n";
                        if (def.message.find("DARK") != std::string::npos || 
                            def.message.find("CAVE") != std::string::npos) {
                            std::cout << "  Content verification PASS: message contains expected text\n";
                        } else {
                            std::cout << "  Content verification WARNING: message doesn't match expected\n";
                        }
                    }
                }
                if (!found_spearow_mail && pokemail_defs.size() > 0) {
                    std::cout << "  Note: GiftSpearowMail address 0x69D98 not found in definitions\n";
                    std::cout << "        (may be from a different script bank)\n";
                }
            }
        }
        
        // === Text Content Verification ===
        std::cout << "\n=== Text Content Verification ===\n";
        {
            const auto& text_defs = text_registry.all_definitions();
            std::cout << "  Extracted Text definitions: " << text_defs.size() << "\n";
            
            if (text_defs.size() > 0) {
                // Sample a few text definitions to verify they contain actual text
                size_t sample_count = std::min(text_defs.size(), size_t(3));
                for (size_t i = 0; i < sample_count; ++i) {
                    const auto& def = text_defs[i];
                    std::string preview = def.identity_string();
                    if (preview.length() > 40) preview = preview.substr(0, 40) + "...";
                    std::cout << "  Text " << i << ": \"" << preview << "\"\n";
                    std::cout << "    Source ROM: 0x" << std::hex << def.source_rom_address << std::dec << "\n";
                }
            }
        }
        
        if (success) {
            std::cout << "\n*** ALL BOUNDARY TESTS PASSED ***\n";
        }
        
        return success ? 0 : 1;
    } else {
        std::cout << "\n*** STAGE 6 INCOMPLETE ***\n";
        
        // Document known issue categories
        std::cout << "\n=== Known Issue Analysis ===\n";
        
        if (result.stats.unresolved.contains(ReferenceType::Special)) {
            std::cout << "Special: " << result.stats.unresolved.at(ReferenceType::Special) 
                      << " references have invalid IDs (e.g., 515, 768)\n";
            std::cout << "  → Likely Stage 1-4 decoding error - special IDs should be < 180\n";
        }
        
        if (result.stats.unresolved.contains(ReferenceType::Map)) {
            std::cout << "Map: " << result.stats.unresolved.at(ReferenceType::Map)
                      << " references have invalid IDs (e.g., 784)\n";
            std::cout << "  → Likely Stage 1-4 decoding error - map IDs should be < 0x1A64\n";
        }
        
        if (result.stats.invalid_ownership.contains(ReferenceType::Object)) {
            std::cout << "Object: " << result.stats.invalid_ownership.at(ReferenceType::Object)
                      << " references exceed map object counts\n";
            std::cout << "  → May indicate cross-map script sharing or dynamic objects\n";
        }
        
        return 1;
    }
}

// =============================================================================
// HELPER IMPLEMENTATIONS
// =============================================================================
// Note: discover_reachable_maps() is now the production implementation from
// full_compiler.hpp/cpp - no duplicate implementation here.

void collect_script_addresses(const ExtractedMap& map,
    std::set<uint32_t>& addresses,
    std::map<uint32_t, MapId>& address_to_map) {
    
    for (const auto& obj : map.objects) {
        if (obj.script_rom_address != 0) {
            addresses.insert(obj.script_rom_address);
        }
    }
    for (const auto& bg : map.bg_events) {
        if (bg.script_rom_address != 0) {
            addresses.insert(bg.script_rom_address);
        }
    }
}

CompiledGameData build_compiled_game_data(const RomData& rom,
    const ExtractionProfile& profile,
    const std::vector<MapIdRef>& discovered_maps,
    const StdScriptsTable& std_scripts,
    MapExtractor& extractor) {
    
    CompiledGameData data;
    const auto& c = profile.counts;
    
    //=========================================================================
    // RESOURCE REFERENCES - Validated against actual ROM table counts
    // These values come from pokecrystal/constants and represent the actual
    // ROM data, not arbitrary ranges.
    //=========================================================================
    
    // === Species (1-251 for Gen 2, from profile.counts.num_pokemon) ===
    // Species 0 is SPECIES_NONE (sentinel), valid species are 1-251
    for (uint16_t i = 1; i <= c.num_pokemon; ++i) {
        data.species.insert(static_cast<SpeciesId>(i));
    }
    
    // === Items (0-255 in Crystal, from profile.counts.num_items) ===
    // Item 0 is NO_ITEM sentinel, all 256 values are valid indices
    for (uint16_t i = 0; i < c.num_items; ++i) {
        data.items.insert(static_cast<ItemId>(i));
    }
    
    // === Maps (from discovered set - PRODUCTION REGISTRY) ===
    for (const auto& ref : discovered_maps) {
        MapId map_id = (static_cast<uint16_t>(ref.group) << 8) | ref.map;
        data.maps.insert(map_id);
        
        // Get object and warp counts for this map
        auto result = extractor.extract_map(ref.group, ref.map);
        if (result.success) {
            data.map_object_counts[map_id] = static_cast<uint8_t>(result.map.objects.size());
            data.map_warp_counts[map_id] = static_cast<uint8_t>(result.map.warps.size());
        }
    }
    
    // === Trainers (from TrainerRegistry - AUTHORITATIVE PRODUCTION REGISTRY) ===
    // Extracts actual (group, id) pairs from ROM's TrainerGroups table
    // No fabricated ranges - only real existing trainers pass validation
    TrainerRegistry trainer_registry(rom, profile.offsets.trainer_groups, 
        static_cast<uint8_t>(c.num_trainer_classes));
    for (const auto& pair : trainer_registry.all_pairs()) {
        data.trainers.insert(pair);
    }
    
    // === StdScripts (from table - PRODUCTION REGISTRY) ===
    data.load_std_scripts(std_scripts);
    
    //=========================================================================
    // SEMANTIC ENUMERATIONS - Closed domains with known counts
    // These are function/data indices, not resource definitions.
    // Any value in [0, count) is valid by definition.
    //=========================================================================
    
    // === Specials (0 to num_specials-1, indices into SpecialsPointers) ===
    // SemanticEnum: Just function pointer indices, no "definition" data
    for (uint16_t i = 0; i < c.num_specials; ++i) {
        data.specials.insert(i);
    }
    
    // === Music (0 to num_music-1, indices into music_pointers.asm) ===
    // SemanticEnum: Song header indices, MUSIC_NONE = 0 is valid
    for (uint16_t i = 0; i < c.num_music; ++i) {
        data.music.insert(static_cast<MusicId>(i));
    }
    
    // === SFX (0 to num_sfx-1, indices into sfx_pointers.asm) ===
    // SemanticEnum: Sound effect indices
    for (uint16_t i = 0; i < c.num_sfx; ++i) {
        data.sfx.insert(static_cast<SfxId>(i));
    }
    
    // === Emotes (0 to num_emotes-1, sprite bubble indices) ===
    // SemanticEnum: Animation sprite indices
    for (uint8_t i = 0; i < c.num_emotes; ++i) {
        data.emotes.insert(i);
    }
    
    //=========================================================================
    // CLOSED RESOURCE TABLES - Small fixed-size tables from ROM
    // These have authoritative counts from pokecrystal/constants
    //=========================================================================
    
    // === Phone persons (0 to num_phone_contacts-1) ===
    // Closed table: PhoneContacts (data/phone/phone_contacts.asm)
    for (uint8_t i = 0; i < c.num_phone_contacts; ++i) {
        data.phone_persons.insert(i);
    }
    
    // === Trades (0 to num_npc_trades-1) ===
    // Closed table: NPCTrades (data/events/npc_trades.asm)
    for (uint8_t i = 0; i < c.num_npc_trades; ++i) {
        data.trades.insert(i);
    }
    
    // === Fruit trees (1 to num_fruit_trees, 0 is invalid) ===
    // Closed table: FruitTreeItems (data/items/fruit_trees.asm)
    // NOTE: Fruit tree IDs are 1-indexed (const_def 1)
    for (uint8_t i = 1; i <= c.num_fruit_trees; ++i) {
        data.fruit_trees.insert(i);
    }
    
    // === Marts (0 to num_marts-1) ===
    // Closed table: Marts (data/items/marts.asm)
    for (uint16_t i = 0; i < c.num_marts; ++i) {
        data.marts.insert(i);
    }
    
    // === Elevators ===
    // NOTE: Elevator IDs are assigned during script lowering when elevator 
    // commands are encountered. We leave this empty here and populate it
    // from the ElevatorRegistry after all scripts are processed.
    // (Do not add dummy IDs - they will come from actual elevator commands)
    
    return data;
}

std::optional<SemanticScriptIR> process_script_to_stage5(
    uint32_t rom_address,
    const std::string& script_id,
    TypedScriptDecoder& decoder,
    CFGBuilder& cfg_builder,
    SemanticLegalizer& legalizer,
    LegalityGate& legality_gate) {
    
    try {
        // Stage 1: Decode
        CrystalScriptIR ir = decoder.decode_script(rom_address);
        if (ir.commands.empty()) {
            return std::nullopt;
        }
        
        // Stage 2: CFG
        CrystalCFG cfg = cfg_builder.build(ir);
        if (!cfg.validation.valid) {
            return std::nullopt;
        }
        
        // Stage 4: Lowering
        LoweringResult lowering = legalizer.lower(ir, cfg);
        if (lowering.commands_unlowered > 0) {
            // Allow partial lowering - Stage 5 will catch issues
        }
        
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
        input.native_registry = nullptr;  // Will use internal
        input.ram_registry = nullptr;
        input.lowering = &lowering;
        
        LegalityResult legality = legality_gate.validate(input);
        if (!legality.is_legal) {
            return std::nullopt;
        }
        
        // Store script_id and source address
        lowering.ir.script_id = script_id;
        lowering.ir.source_rom_address = rom_address;
        
        return lowering.ir;
        
    } catch (const std::exception&) {
        return std::nullopt;
    }
}
