// tools/profile_verify.cpp
// Verifies ROM profile system against actual ROM and symbol file
// 
// Usage: profile_verify <rom_file> [symbol_file]
// 
// Tests:
// 1. Load ROM and compute SHA-1 hash
// 2. Strict identification via profile registry (no guessing)
// 3. If symbols provided, verify profile offsets match
// 4. Read known ROM locations to validate offsets

#include "crystal/rom/loader.hpp"
#include "crystal/rom/profile.hpp"
#include "crystal/rom/symbol_map.hpp"

#include <iostream>
#include <iomanip>
#include <filesystem>

using namespace crystal;

void print_usage(const char* program) {
    std::cout << "Usage: " << program << " <rom_file> [symbol_file]\n"
              << "\n"
              << "Verifies the ROM profile system:\n"
              << "  1. Loads ROM and computes SHA-1 hash\n"
              << "  2. Strictly identifies ROM version (no fallback guessing)\n"
              << "  3. If symbols provided, validates profile offsets match\n"
              << "  4. Reads known ROM locations to verify data integrity\n";
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }
    
    std::filesystem::path rom_path = argv[1];
    std::filesystem::path sym_path;
    if (argc > 2) {
        sym_path = argv[2];
    }
    
    std::cout << "=== ROM Profile Verification ===\n\n";
    
    // Load ROM
    std::cout << "Loading ROM: " << rom_path << "\n";
    auto rom = RomData::load(rom_path);
    if (!rom) {
        std::cerr << "FAIL: Could not load ROM file\n";
        return 1;
    }
    
    std::cout << "ROM size: " << rom->size() << " bytes\n";
    std::cout << "Header title: \"" << rom->header().title << "\"\n";
    std::cout << "SHA-1 hash: " << rom->hash() << "\n\n";
    
    // Check against expected hash
    constexpr const char* EXPECTED_V11_HASH = "f2f52230b536214ef7c9924f483392993e226cfb";
    
    std::cout << "=== Hash Verification ===\n";
    std::cout << "Expected (Crystal v1.1): " << EXPECTED_V11_HASH << "\n";
    std::cout << "Actual:                  " << rom->hash() << "\n";
    if (rom->hash() == EXPECTED_V11_HASH) {
        std::cout << "Result: MATCH ✓\n\n";
    } else {
        std::cout << "Result: MISMATCH ✗\n";
        std::cout << "This ROM is not the expected Crystal v1.1 version.\n\n";
    }
    
    // Strict identification via profile registry
    std::cout << "=== Profile Registry Lookup ===\n";
    auto& registry = ProfileRegistry::instance();
    
    std::cout << "Supported ROMs:\n";
    for (const auto& [sha1, name] : registry.supported_roms()) {
        std::cout << "  " << name << "\n";
        std::cout << "    " << sha1 << "\n";
    }
    std::cout << "\n";
    
    const auto* profile = registry.get_profile_by_hash(rom->hash());
    if (!profile) {
        std::cout << "Result: ROM NOT RECOGNIZED ✗\n";
        std::cout << "This ROM hash is not in the profile registry.\n";
        std::cout << "Extraction requires a supported ROM version.\n";
        return 1;
    }
    
    std::cout << "Result: IDENTIFIED ✓\n";
    std::cout << "Profile: " << profile->version_string << "\n";
    std::cout << "Provenance:\n";
    std::cout << "  Generator: " << profile->provenance.generator_version << "\n";
    std::cout << "  Symbol file: " << profile->provenance.symbol_file << "\n";
    std::cout << "  pokecrystal: " << profile->provenance.pokecrystal_commit << "\n";
    std::cout << "  Generated: " << profile->provenance.generated_date << "\n\n";
    
    // Display key offsets
    std::cout << "=== Profile Offsets ===\n";
    const auto& o = profile->offsets;
    std::cout << std::hex << std::setfill('0');
    std::cout << "  MapGroupPointers:    0x" << std::setw(5) << o.map_group_pointers << "\n";
    std::cout << "  BaseData:            0x" << std::setw(5) << o.base_data << "\n";
    std::cout << "  Moves:               0x" << std::setw(5) << o.moves << "\n";
    std::cout << "  ItemAttributes:      0x" << std::setw(5) << o.item_attributes << "\n";
    std::cout << "  Tilesets:            0x" << std::setw(5) << o.tilesets << "\n";
    std::cout << "  ScriptCommandTable:  0x" << std::setw(5) << o.script_command_table << "\n";
    std::cout << std::dec << "\n";
    
    // Display format rules
    std::cout << "=== Format Rules ===\n";
    const auto& fmt = profile->format;
    std::cout << "  Map header size: " << (int)fmt.map.header_size << " bytes\n";
    std::cout << "  Pokemon base data size: " << (int)fmt.pokemon.base_data_size << " bytes\n";
    std::cout << "  Move data size: " << (int)fmt.move.move_data_size << " bytes\n";
    std::cout << "  Tileset entry size: " << (int)fmt.tileset.tileset_size << " bytes\n";
    std::cout << "\n";
    
    // Verify by reading known data
    std::cout << "=== Data Verification ===\n";
    
    // Read first Pokemon (Bulbasaur) base stats
    // Known values: HP=45, ATK=49, DEF=49, SPD=45, SATK=65, SDEF=65
    auto base_data = rom->read_bytes(o.base_data, fmt.pokemon.base_data_size);
    
    std::cout << "Bulbasaur base stats (from BaseData):\n";
    uint8_t hp   = base_data[fmt.pokemon.hp_offset];
    uint8_t atk  = base_data[fmt.pokemon.atk_offset];
    uint8_t def  = base_data[fmt.pokemon.def_offset];
    uint8_t spd  = base_data[fmt.pokemon.spd_offset];
    uint8_t satk = base_data[fmt.pokemon.satk_offset];
    uint8_t sdef = base_data[fmt.pokemon.sdef_offset];
    
    auto verify_stat = [](const char* name, uint8_t actual, uint8_t expected) {
        std::cout << "  " << std::setw(4) << name << ": " << std::setw(3) << (int)actual;
        if (actual == expected) {
            std::cout << " (expected " << (int)expected << ") ✓\n";
        } else {
            std::cout << " (expected " << (int)expected << ") ✗ MISMATCH\n";
        }
        return actual == expected;
    };
    
    bool stats_ok = true;
    stats_ok &= verify_stat("HP", hp, 45);
    stats_ok &= verify_stat("ATK", atk, 49);
    stats_ok &= verify_stat("DEF", def, 49);
    stats_ok &= verify_stat("SPD", spd, 45);
    stats_ok &= verify_stat("SATK", satk, 65);
    stats_ok &= verify_stat("SDEF", sdef, 65);
    
    std::cout << "\n";
    
    // Read first move (Pound) data
    // Known values: Power=40, Type=NORMAL(0), Accuracy=100, PP=35
    auto move_data = rom->read_bytes(o.moves, fmt.move.move_data_size);
    
    std::cout << "Pound move data (from Moves):\n";
    std::cout << "  Power:    " << (int)move_data[fmt.move.power_offset] << " (expected 40)\n";
    std::cout << "  Type:     " << (int)move_data[fmt.move.type_offset] << " (expected 0 = NORMAL)\n";
    std::cout << "  Accuracy: " << (int)move_data[fmt.move.accuracy_offset] << " (expected 100)\n";
    std::cout << "  PP:       " << (int)move_data[fmt.move.pp_offset] << " (expected 35)\n";
    std::cout << "\n";
    
    // If symbols provided, cross-validate
    if (!sym_path.empty()) {
        std::cout << "=== Symbol Cross-Validation ===\n";
        auto symbols = SymbolMap::load(sym_path);
        if (!symbols) {
            std::cerr << "Warning: Could not load symbol file\n";
        } else {
            std::cout << "Loaded " << symbols->count() << " symbols from " << sym_path.filename() << "\n\n";
            
            auto check = [&](const char* name, uint32_t profile_val) {
                auto sym_val = symbols->address(name);
                std::cout << "  " << std::setw(25) << std::left << name << " ";
                std::cout << std::hex;
                std::cout << "profile=0x" << std::setw(5) << std::right << std::setfill('0') << profile_val;
                std::cout << std::setfill(' ');
                if (sym_val) {
                    std::cout << " symbol=0x" << std::setw(5) << std::setfill('0') << *sym_val;
                    std::cout << std::setfill(' ');
                    if (profile_val == *sym_val) {
                        std::cout << " ✓";
                    } else {
                        std::cout << " ✗ MISMATCH";
                    }
                } else {
                    std::cout << " (not found in symbols)";
                }
                std::cout << std::dec << "\n";
            };
            
            check("MapGroupPointers", o.map_group_pointers);
            check("BaseData", o.base_data);
            check("Moves", o.moves);
            check("ItemAttributes", o.item_attributes);
            check("Tilesets", o.tilesets);
            check("ScriptCommandTable", o.script_command_table);
            check("TypeMatchups", o.type_matchups);
            check("TrainerGroups", o.trainer_groups);
            check("JohtoGrassWildMons", o.johto_grass_wild);
            check("PokemonNames", o.pokemon_names);
            check("MoveNames", o.move_names);
            std::cout << "\n";
        }
    }
    
    std::cout << "=== Summary ===\n";
    if (stats_ok) {
        std::cout << "Profile verification: PASSED ✓\n";
        std::cout << "ROM is ready for extraction.\n";
        return 0;
    } else {
        std::cout << "Profile verification: FAILED ✗\n";
        std::cout << "Profile offsets may be incorrect.\n";
        return 1;
    }
}
