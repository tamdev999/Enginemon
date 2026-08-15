// special_inventory.cpp - Dump all Special IDs encountered in corpus
// After lowering, counts how many remain as Sem_Special vs lowered to semantic ops
#include "crystal/rom/loader.hpp"
#include "crystal/rom/profile.hpp"
#include "crystal/script/typed_decoder.hpp"
#include "crystal/script/crystal_cfg.hpp"
#include "crystal/script/semantic_legalizer.hpp"
#include "crystal/script/native_registry.hpp"
#include "crystal/script/legality_gate.hpp"
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
    
    std::cout << "=== Special ID Inventory (Post-Lowering) ===\n\n";
    
    // Load StdScripts table
    StdScriptsTable std_scripts;
    std_scripts.load(*rom, profile->offsets.std_scripts, profile->offsets.std_scripts_count);
    
    // Discover all maps
    MapExtractor extractor(*rom, *profile);
    auto discovered_maps = discover_reachable_maps(*rom, *profile, extractor);
    
    // Collect unique script addresses
    std::set<uint32_t> all_addresses;
    
    for (const auto& ref : discovered_maps) {
        auto result = extractor.extract_map(ref.group, ref.map);
        if (!result.success) continue;
        
        for (const auto& obj : result.map.objects) {
            if (obj.script_rom_address != 0) {
                all_addresses.insert(obj.script_rom_address);
            }
        }
        for (const auto& bg : result.map.bg_events) {
            if (bg.script_rom_address != 0) {
                all_addresses.insert(bg.script_rom_address);
            }
        }
    }
    
    // Add StdScript addresses
    for (size_t i = 0; i < std_scripts.size(); ++i) {
        const auto* entry = std_scripts.get(static_cast<uint16_t>(i));
        if (entry && entry->flat_address != 0) {
            all_addresses.insert(entry->flat_address);
        }
    }
    
    std::cout << "Total unique script addresses: " << all_addresses.size() << "\n\n";
    
    // Setup decoder and legalizer
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
    
    // Track pre-lowering Special IDs
    std::map<uint16_t, size_t> pre_lowering_counts;
    
    // Track post-lowering Sem_Special occurrences
    std::map<uint16_t, size_t> post_lowering_counts;
    
    // Track lowered ops by type
    size_t screen_fade_count = 0;
    size_t sync_palettes_count = 0;
    size_t refresh_player_sprite_count = 0;
    size_t sync_sprites_count = 0;
    size_t rebuild_sprites_count = 0;
    size_t restart_map_music_count = 0;
    size_t fade_to_silence_count = 0;
    size_t wait_sound_count = 0;
    size_t play_map_music_count = 0;
    size_t heal_party_count = 0;
    size_t show_balance_overlay_count = 0;
    size_t check_party_pokerus_count = 0;
    size_t gameboy_check_absorbed_count = 0;  // Absorbed as Sem_SetVar
    
    // Process each script
    for (uint32_t addr : all_addresses) {
        CrystalScriptIR ir = decoder.decode_script(addr);
        
        // Count pre-lowering specials
        for (const auto& cmd : ir.commands) {
            if (const auto* special = std::get_if<Cmd_Special>(&cmd.data)) {
                pre_lowering_counts[special->special_id]++;
            }
        }
        
        // Build CFG and lower
        auto cfg = cfg_builder.build(ir);
        auto lowered = legalizer.lower(ir, cfg);
        
        // Count post-lowering ops
        for (const auto& block : lowered.ir.blocks) {
            for (const auto& inst : block.instructions) {
                std::visit([&](const auto& sem_op) {
                    using T = std::decay_t<decltype(sem_op)>;
                    if constexpr (std::is_same_v<T, enginemon::Sem_Special>) {
                        post_lowering_counts[sem_op.special_id]++;
                    }
                    else if constexpr (std::is_same_v<T, enginemon::Sem_ScreenFade>) {
                        screen_fade_count++;
                    }
                    else if constexpr (std::is_same_v<T, enginemon::Sem_SyncPalettes>) {
                        sync_palettes_count++;
                    }
                    else if constexpr (std::is_same_v<T, enginemon::Sem_RefreshPlayerSprite>) {
                        refresh_player_sprite_count++;
                    }
                    else if constexpr (std::is_same_v<T, enginemon::Sem_SyncSprites>) {
                        sync_sprites_count++;
                    }
                    else if constexpr (std::is_same_v<T, enginemon::Sem_RebuildSprites>) {
                        rebuild_sprites_count++;
                    }
                    else if constexpr (std::is_same_v<T, enginemon::Sem_RestartMapMusic>) {
                        restart_map_music_count++;
                    }
                    else if constexpr (std::is_same_v<T, enginemon::Sem_FadeToSilence>) {
                        fade_to_silence_count++;
                    }
                    else if constexpr (std::is_same_v<T, enginemon::Sem_WaitSound>) {
                        wait_sound_count++;
                    }
                    else if constexpr (std::is_same_v<T, enginemon::Sem_PlayMapMusic>) {
                        play_map_music_count++;
                    }
                    else if constexpr (std::is_same_v<T, enginemon::Sem_HealParty>) {
                        heal_party_count++;
                    }
                    else if constexpr (std::is_same_v<T, enginemon::Sem_ShowBalanceOverlay>) {
                        show_balance_overlay_count++;
                    }
                    else if constexpr (std::is_same_v<T, enginemon::Sem_CheckPartyPokerus>) {
                        check_party_pokerus_count++;
                    }
                    else if constexpr (std::is_same_v<T, enginemon::Sem_SetVar>) {
                        // Note: GameboyCheck (102) is absorbed as Sem_SetVar with literal GBCHECK_CGB
                        // We can identify it by checking if var=0 and source is literal(2)
                        // For now, just track this doesn't add to Sem_Special
                    }
                }, inst.op);
            }
        }
    }
    
    // Calculate totals
    size_t pre_total = 0;
    for (const auto& [id, count] : pre_lowering_counts) {
        pre_total += count;
    }
    
    size_t post_total = 0;
    for (const auto& [id, count] : post_lowering_counts) {
        post_total += count;
    }
    
    size_t lowered_total = screen_fade_count + sync_palettes_count + 
                           refresh_player_sprite_count + sync_sprites_count +
                           rebuild_sprites_count + restart_map_music_count + 
                           fade_to_silence_count + wait_sound_count +
                           play_map_music_count + heal_party_count +
                           show_balance_overlay_count + check_party_pokerus_count;
    
    std::cout << "=== Pre-Lowering Summary ===\n";
    std::cout << "Unique Special IDs: " << pre_lowering_counts.size() << "\n";
    std::cout << "Total Special occurrences: " << pre_total << "\n\n";
    
    std::cout << "=== Post-Lowering Summary ===\n";
    std::cout << "Remaining Sem_Special unique IDs: " << post_lowering_counts.size() << "\n";
    std::cout << "Remaining Sem_Special occurrences: " << post_total << "\n\n";
    
    std::cout << "=== Lowered to Semantic Ops ===\n";
    std::cout << "Sem_ScreenFade:           " << screen_fade_count << "\n";
    std::cout << "Sem_SyncPalettes:         " << sync_palettes_count << "\n";
    std::cout << "Sem_RefreshPlayerSprite:  " << refresh_player_sprite_count << "\n";
    std::cout << "Sem_SyncSprites:          " << sync_sprites_count << "\n";
    std::cout << "Sem_RebuildSprites:       " << rebuild_sprites_count << "\n";
    std::cout << "Sem_RestartMapMusic:      " << restart_map_music_count << "\n";
    std::cout << "Sem_FadeToSilence:        " << fade_to_silence_count << "\n";
    std::cout << "Sem_WaitSound:            " << wait_sound_count << "\n";
    std::cout << "Sem_PlayMapMusic:         " << play_map_music_count << "\n";
    std::cout << "Sem_HealParty:            " << heal_party_count << "\n";
    std::cout << "Sem_ShowBalanceOverlay:   " << show_balance_overlay_count << "\n";
    std::cout << "Sem_CheckPartyPokerus:    " << check_party_pokerus_count << "\n";
    std::cout << "GameboyCheck absorbed:    (counted in Sem_SetVar, -1 from Sem_Special)\n";
    std::cout << "Total lowered:            " << lowered_total << "\n\n";
    
    std::cout << "=== Verification ===\n";
    std::cout << "Pre-lowering total:  " << pre_total << "\n";
    std::cout << "Post-lowering total: " << post_total << " (Sem_Special) + " 
              << lowered_total << " (semantic ops) = " << (post_total + lowered_total) << "\n";
    
    if (pre_total == post_total + lowered_total) {
        std::cout << "✓ Counts match\n\n";
    } else {
        std::cout << "✗ COUNT MISMATCH!\n\n";
    }
    
    std::cout << "=== Remaining Sem_Special IDs ===\n";
    std::cout << std::setw(4) << "ID" << " | "
              << std::setw(6) << "Count" << "\n";
    std::cout << std::string(15, '-') << "\n";
    
    for (const auto& [id, count] : post_lowering_counts) {
        std::cout << std::setw(4) << id << " | "
                  << std::setw(6) << count << "\n";
    }
    
    std::cout << "\n=== Reduction ===\n";
    std::cout << "Unique IDs reduced: " << pre_lowering_counts.size() << " -> " 
              << post_lowering_counts.size() << " (-" 
              << (pre_lowering_counts.size() - post_lowering_counts.size()) << ")\n";
    std::cout << "Occurrences reduced: " << pre_total << " -> " << post_total 
              << " (-" << (pre_total - post_total) << ")\n";
    
    return 0;
}
