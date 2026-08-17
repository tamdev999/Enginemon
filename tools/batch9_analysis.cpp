// batch9_analysis.cpp - Detailed Batch 9 candidate analysis
// Analyzes each occurrence of Special IDs 40, 57, 66, 67, 95, 152
// Reports context tracking for setval→special patterns
#include "crystal/rom/loader.hpp"
#include "crystal/rom/profile.hpp"
#include "crystal/compile/corpus_discovery.hpp"
#include "crystal/script/typed_decoder.hpp"
#include "crystal/script/crystal_cfg.hpp"
#include "crystal/script/native_registry.hpp"
#include "crystal/extract/map_extractor.hpp"
#include "crystal/script/crystal_command.hpp"
#include <iostream>
#include <iomanip>
#include <map>
#include <set>
#include <vector>
#include <sstream>

using namespace crystal;
using namespace enginemon;

// Batch 9 candidate IDs
constexpr uint16_t BATCH9_IDS[] = {40, 57, 66, 67, 95, 152};

// Special ID symbols (from pokecrystal)
const std::map<uint16_t, const char*> SPECIAL_SYMBOLS = {
    {40, "MapRadio"},
    {57, "GameCornerPrizeMonCheckDex"},
    {66, "FindPartyMonThatSpecies"},
    {67, "FindPartyMonThatSpeciesYourTrainerID"},
    {95, "PlaySlowCry"},
    {152, "SetPlayerPalette"},
};

struct OccurrenceInfo {
    uint32_t body_root;
    ScriptRootType root_type;
    uint32_t special_addr;
    uint16_t special_id;
    bool has_setval_same_block;
    uint16_t setval_literal;
    uint32_t setval_addr;
    size_t commands_between;
    bool all_intervening_preserve_script_var;
    std::string intervening_commands;
    MapId owning_map;
};

// Check if a command potentially modifies wScriptVar
bool command_potentially_modifies_script_var(const CrystalCommand& cmd) {
    return std::visit([](const auto& data) -> bool {
        using T = std::decay_t<decltype(data)>;
        // Commands known to modify wScriptVar
        if constexpr (std::is_same_v<T, Cmd_Setval>) return true;
        if constexpr (std::is_same_v<T, Cmd_Special>) return true;
        if constexpr (std::is_same_v<T, Cmd_Callasm>) return true;
        if constexpr (std::is_same_v<T, Cmd_Readmem>) return true;
        if constexpr (std::is_same_v<T, Cmd_Random>) return true;
        if constexpr (std::is_same_v<T, Cmd_Addval>) return true;
        if constexpr (std::is_same_v<T, Cmd_Readvar>) return true;
        if constexpr (std::is_same_v<T, Cmd_Checkscene>) return true;
        if constexpr (std::is_same_v<T, Cmd_Checkmapscene>) return true;
        // Control flow commands don't modify it themselves
        if constexpr (std::is_same_v<T, Cmd_Sjump>) return false;
        if constexpr (std::is_same_v<T, Cmd_Scall>) return false;
        if constexpr (std::is_same_v<T, Cmd_End>) return false;
        // Text/movement commands don't modify it
        if constexpr (std::is_same_v<T, Cmd_Jumptext>) return false;
        if constexpr (std::is_same_v<T, Cmd_Faceplayer>) return false;
        if constexpr (std::is_same_v<T, Cmd_Opentext>) return false;
        if constexpr (std::is_same_v<T, Cmd_Closetext>) return false;
        if constexpr (std::is_same_v<T, Cmd_Waitbutton>) return false;
        if constexpr (std::is_same_v<T, Cmd_Writetext>) return false;
        if constexpr (std::is_same_v<T, Cmd_Playsound>) return false;
        if constexpr (std::is_same_v<T, Cmd_Playmusic>) return false;
        if constexpr (std::is_same_v<T, Cmd_Cry>) return false;
        if constexpr (std::is_same_v<T, Cmd_Waitsfx>) return false;
        if constexpr (std::is_same_v<T, Cmd_Pause>) return false;
        if constexpr (std::is_same_v<T, Cmd_Loadvar>) return false; // writes to VAR, not ScriptVar
        if constexpr (std::is_same_v<T, Cmd_Showemote>) return false;
        // Default: assume it could modify (conservative)
        return true;
    }, cmd.data);
}

std::string command_name(const CrystalCommand& cmd) {
    return std::visit([](const auto& data) -> std::string {
        using T = std::decay_t<decltype(data)>;
        if constexpr (std::is_same_v<T, Cmd_Setval>) return "setval";
        if constexpr (std::is_same_v<T, Cmd_Special>) return "special";
        if constexpr (std::is_same_v<T, Cmd_Sjump>) return "sjump";
        if constexpr (std::is_same_v<T, Cmd_Scall>) return "scall";
        if constexpr (std::is_same_v<T, Cmd_End>) return "end";
        if constexpr (std::is_same_v<T, Cmd_Jumptext>) return "jumptext";
        if constexpr (std::is_same_v<T, Cmd_Faceplayer>) return "faceplayer";
        if constexpr (std::is_same_v<T, Cmd_Opentext>) return "opentext";
        if constexpr (std::is_same_v<T, Cmd_Closetext>) return "closetext";
        if constexpr (std::is_same_v<T, Cmd_Loadvar>) return "loadvar";
        if constexpr (std::is_same_v<T, Cmd_Writetext>) return "writetext";
        if constexpr (std::is_same_v<T, Cmd_Waitbutton>) return "waitbutton";
        if constexpr (std::is_same_v<T, Cmd_Callasm>) return "callasm";
        if constexpr (std::is_same_v<T, Cmd_Readmem>) return "readmem";
        if constexpr (std::is_same_v<T, Cmd_Setevent>) return "setevent";
        if constexpr (std::is_same_v<T, Cmd_Checkevent>) return "checkevent";
        if constexpr (std::is_same_v<T, Cmd_Playsound>) return "playsound";
        if constexpr (std::is_same_v<T, Cmd_Cry>) return "cry";
        if constexpr (std::is_same_v<T, Cmd_Playmusic>) return "playmusic";
        if constexpr (std::is_same_v<T, Cmd_Giveitem>) return "giveitem";
        if constexpr (std::is_same_v<T, Cmd_Verbosegiveitem>) return "verbosegiveitem";
        if constexpr (std::is_same_v<T, Cmd_Loadwildmon>) return "loadwildmon";
        if constexpr (std::is_same_v<T, Cmd_Random>) return "random";
        if constexpr (std::is_same_v<T, Cmd_Waitsfx>) return "waitsfx";
        if constexpr (std::is_same_v<T, Cmd_Pause>) return "pause";
        if constexpr (std::is_same_v<T, Cmd_Showemote>) return "showemote";
        return "other";
    }, cmd.data);
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
    
    std::cout << "=== Batch 9 Detailed Analysis ===\n";
    std::cout << "Corpus: 1679 bodies (frozen)\n\n";
    
    // Load StdScripts
    StdScriptsTable std_scripts;
    std_scripts.load(*rom, profile->offsets.std_scripts, profile->offsets.std_scripts_count);
    
    SymbolMap symbols;
    TypedScriptDecoder decoder(*rom, symbols);
    MapExtractor extractor(*rom, *profile);
    
    // Discover corpus
    auto corpus = discover_corpus(*rom, *profile, extractor, decoder, std_scripts);
    auto all_addresses = corpus.all_addresses();
    
    std::cout << "Total unique bodies: " << all_addresses.size() << "\n\n";
    
    // Initialize registries for CFG
    NativeCallRegistry native_registry;
    native_registry.initialize();
    CFGBuilder cfg_builder;
    cfg_builder.set_std_scripts(&std_scripts);
    cfg_builder.set_native_registry(&native_registry);

    // Collect occurrences per candidate ID
    std::map<uint16_t, std::vector<OccurrenceInfo>> occurrences;
    
    for (uint32_t addr : all_addresses) {
        CrystalScriptIR ir = decoder.decode_script(addr);
        auto cfg = cfg_builder.build(ir);
        
        // Get root info
        ScriptRootType root_type = corpus.get_root_type(addr);
        MapId owning_map = MAP_NONE;
        auto root_it = corpus.map_roots.find(addr);
        if (root_it != corpus.map_roots.end()) {
            owning_map = root_it->second.owning_map;
        }
        
        // Find all Batch 9 specials in this body
        for (size_t i = 0; i < ir.commands.size(); ++i) {
            const auto& cmd = ir.commands[i];
            const auto* special = std::get_if<Cmd_Special>(&cmd.data);
            if (!special) continue;
            
            bool is_batch9 = false;
            for (uint16_t id : BATCH9_IDS) {
                if (special->special_id == id) {
                    is_batch9 = true;
                    break;
                }
            }
            if (!is_batch9) continue;
            
            // Found a Batch 9 special - analyze context
            OccurrenceInfo info;
            info.body_root = addr;
            info.root_type = root_type;
            info.special_addr = cmd.span.rom_address;
            info.special_id = special->special_id;
            info.has_setval_same_block = false;
            info.setval_literal = 0;
            info.setval_addr = 0;
            info.commands_between = 0;
            info.all_intervening_preserve_script_var = true;
            info.owning_map = owning_map;

            // Find which CFG block contains this Special
            size_t special_block_idx = SIZE_MAX;
            size_t special_idx_in_block = SIZE_MAX;
            for (size_t b = 0; b < cfg.blocks.size(); ++b) {
                const auto& block = cfg.blocks[b];
                if (i >= block.command_start && i < block.command_start + block.command_count) {
                    special_block_idx = b;
                    special_idx_in_block = i - block.command_start;
                    break;
                }
            }
            
            // Search backward in same block for setval
            if (special_block_idx != SIZE_MAX && special_idx_in_block > 0) {
                const auto& block = cfg.blocks[special_block_idx];
                std::stringstream intervening;
                
                for (size_t offset = 1; offset <= special_idx_in_block; ++offset) {
                    size_t cmd_idx = block.command_start + special_idx_in_block - offset;
                    const auto& prev_cmd = ir.commands[cmd_idx];
                    
                    if (const auto* sv = std::get_if<Cmd_Setval>(&prev_cmd.data)) {
                        info.has_setval_same_block = true;
                        info.setval_literal = sv->value;
                        info.setval_addr = prev_cmd.span.rom_address;
                        info.commands_between = offset - 1;
                        break;
                    }
                    
                    // Record intervening command
                    intervening << command_name(prev_cmd) << " ";
                    
                    // Check if it preserves ScriptVar
                    if (command_potentially_modifies_script_var(prev_cmd)) {
                        info.all_intervening_preserve_script_var = false;
                    }
                }
                info.intervening_commands = intervening.str();
            }
            
            occurrences[special->special_id].push_back(info);
        }
    }

    // Report per candidate
    for (uint16_t id : BATCH9_IDS) {
        const auto& occ_list = occurrences[id];
        auto symbol_it = SPECIAL_SYMBOLS.find(id);
        const char* symbol = symbol_it != SPECIAL_SYMBOLS.end() ? symbol_it->second : "???";
        
        std::cout << "\n=== Special " << id << " (" << symbol << ") ===\n";
        std::cout << "Total occurrences: " << occ_list.size() << "\n\n";
        
        size_t context_valid = 0;
        for (size_t i = 0; i < occ_list.size(); ++i) {
            const auto& occ = occ_list[i];
            
            std::cout << "Occurrence " << (i+1) << ":\n";
            std::cout << "  Body root:    0x" << std::hex << occ.body_root << std::dec << "\n";
            std::cout << "  Root type:    " << script_root_type_name(occ.root_type) << "\n";
            std::cout << "  Owning map:   0x" << std::hex << occ.owning_map << std::dec << "\n";
            std::cout << "  Special addr: 0x" << std::hex << occ.special_addr << std::dec << "\n";
            
            if (occ.has_setval_same_block) {
                std::cout << "  setval found: YES\n";
                std::cout << "    Literal:    " << occ.setval_literal 
                          << " (0x" << std::hex << occ.setval_literal << std::dec << ")\n";
                std::cout << "    setval addr: 0x" << std::hex << occ.setval_addr << std::dec << "\n";
                std::cout << "    Commands between: " << occ.commands_between << "\n";
                std::cout << "    All preserve ScriptVar: " 
                          << (occ.all_intervening_preserve_script_var ? "YES" : "NO") << "\n";
                if (!occ.intervening_commands.empty() && occ.commands_between > 0) {
                    std::cout << "    Intervening: " << occ.intervening_commands << "\n";
                }
                
                if (occ.all_intervening_preserve_script_var) {
                    std::cout << "  CONTEXT VALID: YES\n";
                    context_valid++;
                } else {
                    std::cout << "  CONTEXT VALID: NO (intervening commands may clobber)\n";
                }
            } else {
                std::cout << "  setval found: NO (not in same basic block)\n";
                std::cout << "  CONTEXT VALID: NO\n";
            }
            std::cout << "\n";
        }
        
        std::cout << "Summary for Special " << id << ":\n";
        std::cout << "  Total occurrences: " << occ_list.size() << "\n";
        std::cout << "  Context-valid:     " << context_valid << "\n";
        std::cout << "  Fully removable:   " << (context_valid == occ_list.size() && !occ_list.empty() ? "YES" : "NO") << "\n";
    }

    // Final summary
    std::cout << "\n=== BATCH 9 FINAL SUMMARY ===\n";
    std::cout << "\n| ID  | Symbol | Total | Context-Valid | Removable |\n";
    std::cout << "|-----|--------|-------|---------------|----------|\n";
    
    size_t total_occ = 0;
    size_t total_valid = 0;
    size_t removable_ids = 0;
    size_t removable_occ = 0;
    
    for (uint16_t id : BATCH9_IDS) {
        const auto& occ_list = occurrences[id];
        auto symbol_it = SPECIAL_SYMBOLS.find(id);
        const char* symbol = symbol_it != SPECIAL_SYMBOLS.end() ? symbol_it->second : "???";
        
        size_t context_valid = 0;
        for (const auto& occ : occ_list) {
            if (occ.has_setval_same_block && occ.all_intervening_preserve_script_var) {
                context_valid++;
            }
        }
        
        bool fully_removable = (context_valid == occ_list.size()) && !occ_list.empty();
        
        std::cout << "| " << std::setw(3) << id << " | " 
                  << std::setw(6) << symbol << " | "
                  << std::setw(5) << occ_list.size() << " | "
                  << std::setw(13) << context_valid << " | "
                  << (fully_removable ? "YES" : "NO") << " |\n";
        
        total_occ += occ_list.size();
        total_valid += context_valid;
        if (fully_removable) {
            removable_ids++;
            removable_occ += occ_list.size();
        }
    }
    
    std::cout << "\nRemovable IDs: " << removable_ids << " / 6\n";
    std::cout << "Removable occurrences: " << removable_occ << " / " << total_occ << "\n";
    std::cout << "Partially valid (context established): " << total_valid << " / " << total_occ << "\n";
    
    return 0;
}
