// crystal/rom/profile.cpp
// ROM extraction profile implementation
// Profile = identity + locations + format rules

#include "crystal/rom/profile.hpp"
#include "crystal/rom/symbol_map.hpp"
#include <format>
#include <sstream>

namespace crystal {

// Helper: convert bank:address to flat ROM offset
static constexpr uint32_t flat_offset(uint8_t bank, uint16_t addr) {
    if (addr < 0x4000) {
        return addr;  // Bank 0 always mapped to 0x0000-0x3FFF
    }
    return (bank * 0x4000) + (addr - 0x4000);
}

//=============================================================================
// ProfileRegistry
//=============================================================================

ProfileRegistry& ProfileRegistry::instance() {
    static ProfileRegistry registry;
    return registry;
}

ProfileRegistry::ProfileRegistry() {
    register_crystal_v11();
    // Additional versions added here as verified
}

void ProfileRegistry::register_crystal_v11() {
    // Pokemon Crystal (USA/Europe) v1.1
    // This is the canonical hash from pret/pokecrystal
    constexpr const char* SHA1_CRYSTAL_V11 = "f2f52230b536214ef7c9924f483392993e226cfb";
    
    ExtractionProfile profile;
    
    //-------------------------------------------------------------------------
    // Identity
    //-------------------------------------------------------------------------
    profile.version = RomVersion::Crystal_USA_v1_1;
    profile.version_string = "Pokemon Crystal (USA/Europe) v1.1";
    profile.sha1 = SHA1_CRYSTAL_V11;
    
    //-------------------------------------------------------------------------
    // Provenance
    //-------------------------------------------------------------------------
    profile.provenance.generator_version = "1.0.0";
    profile.provenance.pokecrystal_commit = "symbols-branch";  // pret/pokecrystal symbols branch
    profile.provenance.symbol_file = "pokecrystal11.sym";
    profile.provenance.generated_date = "2024-01-15";  // Profile creation date
    
    //-------------------------------------------------------------------------
    // Format Rules - Crystal v1.1 specific interpretation
    //-------------------------------------------------------------------------
    auto& fmt = profile.format;
    
    // Map format (verified from pokecrystal data/maps/attributes.asm)
    fmt.map.group_pointer_format = PointerFormat::AddrOnly;  // MapGroupPointers uses 2-byte ptrs
    fmt.map.header_size = 12;           // 12-byte MapAttributes header
    fmt.map.border_block_offset = 0;
    fmt.map.height_offset = 1;
    fmt.map.width_offset = 2;
    fmt.map.blockdata_bank_offset = 3;
    fmt.map.blockdata_ptr_offset = 4;   // 2 bytes
    fmt.map.script_bank_offset = 6;     // also events bank
    fmt.map.script_ptr_offset = 7;      // 2 bytes
    fmt.map.events_ptr_offset = 9;      // 2 bytes
    fmt.map.connections_offset = 11;    // connection bitfield
    fmt.map.connection_size = 12;
    fmt.map.warp_size = 5;              // db y, x, warp_dest, group, map
    fmt.map.coord_event_size = 8;       // db scene, y, x, 0; dw script; dw 0
    fmt.map.bg_event_size = 5;          // db y, x, type; dw script
    fmt.map.object_event_size = 13;
    
    // Pokemon format
    fmt.pokemon.base_data_size = 32;
    fmt.pokemon.name_length = 10;
    fmt.pokemon.hp_offset = 1;
    fmt.pokemon.atk_offset = 2;
    fmt.pokemon.def_offset = 3;
    fmt.pokemon.spd_offset = 4;
    fmt.pokemon.satk_offset = 5;
    fmt.pokemon.sdef_offset = 6;
    fmt.pokemon.type1_offset = 7;
    fmt.pokemon.type2_offset = 8;
    fmt.pokemon.catch_rate_offset = 9;
    fmt.pokemon.base_exp_offset = 10;
    fmt.pokemon.items_offset = 11;
    fmt.pokemon.gender_offset = 13;
    fmt.pokemon.egg_cycles_offset = 15;
    fmt.pokemon.growth_rate_offset = 22;
    fmt.pokemon.egg_groups_offset = 23;
    fmt.pokemon.tmhm_offset = 24;
    
    // Move format
    fmt.move.move_data_size = 7;
    fmt.move.name_length = 12;
    
    // Item format
    fmt.item.attr_size = 7;
    fmt.item.name_length = 12;
    
    // Tileset format (verified from pokecrystal data/tilesets.asm)
    fmt.tileset.tileset_size = 15;      // TILESET_LENGTH
    fmt.tileset.metatile_size = 16;     // 16 bytes per metatile (2048/128)
    fmt.tileset.metatile_count = 128;   // standard metatiles per tileset
    
    // Script format
    fmt.script.command_table_entry_size = 3;
    fmt.script.script_pointer_format = PointerFormat::BankAddrLE;
    fmt.script.text_terminator = 0x50;
    
    // Text format
    fmt.text.uses_custom_charmap = true;
    fmt.text.string_terminator = 0x50;
    
    //-------------------------------------------------------------------------
    // Offsets - extracted from pokecrystal11.sym
    // Format: symbol = BB:AAAA -> flat_offset(0xBB, 0xAAAA)
    //-------------------------------------------------------------------------
    auto& o = profile.offsets;
    
    // Map system
    o.map_group_pointers    = flat_offset(0x25, 0x4000);  // 25:4000 MapGroupPointers
    o.map_groups_bank       = 0x25;                       // Bank containing map data
    o.map_names             = 0;  // TODO: find MapNames symbol
    o.spawn_points          = flat_offset(0x05, 0x52ab);  // 05:52ab SpawnPoints
    
    // Script system
    o.script_command_table  = flat_offset(0x25, 0x6cb1);  // 25:6cb1 ScriptCommandTable
    o.special_pointers      = flat_offset(0x03, 0x4029);  // 03:4029 SpecialsPointers
    
    // Pokemon data
    o.base_data             = flat_offset(0x14, 0x5424);  // 14:5424 BaseData
    o.pokemon_names         = flat_offset(0x14, 0x7384);  // 14:7384 PokemonNames
    o.evos_attacks          = flat_offset(0x10, 0x65b1);  // 10:65b1 EvosAttacksPointers
    o.egg_move_pointers     = flat_offset(0x08, 0x7b11);  // 08:7b11 EggMovePointers
    
    // Move data
    o.moves                 = flat_offset(0x10, 0x5afb);  // 10:5afb Moves
    o.move_names            = flat_offset(0x72, 0x5f29);  // 72:5f29 MoveNames
    
    // Item data
    o.item_attributes       = flat_offset(0x01, 0x67c1);  // 01:67c1 ItemAttributes
    o.item_names            = flat_offset(0x72, 0x4000);  // 72:4000 ItemNames
    
    // Type data
    o.type_matchups         = flat_offset(0x0d, 0x4bb1);  // 0d:4bb1 TypeMatchups
    o.type_names            = flat_offset(0x14, 0x497b);  // 14:497b TypeNames
    
    // Trainer data
    o.trainer_groups        = flat_offset(0x0e, 0x5999);  // 0e:5999 TrainerGroups
    o.trainer_class_names   = flat_offset(0x0b, 0x41ef);  // 0b:41ef TrainerClassNames
    
    // Encounter data
    o.johto_grass_wild      = flat_offset(0x0a, 0x65e9);  // 0a:65e9 JohtoGrassWildMons
    o.kanto_grass_wild      = flat_offset(0x0a, 0x7274);  // 0a:7274 KantoGrassWildMons
    o.johto_water_wild      = flat_offset(0x0a, 0x711d);  // 0a:711d JohtoWaterWildMons
    o.swarm_grass_wild      = flat_offset(0x0a, 0x78d0);  // 0a:78d0 SwarmGrassWildMons
    
    // Graphics
    o.tilesets              = flat_offset(0x13, 0x5596);  // 13:5596 Tilesets
    o.pokemon_pic_pointers  = flat_offset(0x48, 0x4000);  // 48:4000 PokemonPicPointers
    o.trainer_pic_pointers  = flat_offset(0x4a, 0x4000);  // 4a:4000 TrainerPicPointers
    o.unown_pic_pointers    = 0;  // TODO
    
    // Audio
    o.music_pointers        = flat_offset(0x3a, 0x506e);  // 3a:506e Music
    o.sfx_pointers          = flat_offset(0x3a, 0x527c);  // 3a:527c SFX
    o.cry_data              = 0;  // TODO
    
    // Text
    o.text_commands         = flat_offset(0x00, 0x1410);  // 00:1410 TextCommands
    
    // Script system extended
    o.std_scripts           = flat_offset(0x2f, 0x4000);  // 2f:4000 StdScripts
    o.std_scripts_count     = 52;                         // 52 standard scripts
    
    //-------------------------------------------------------------------------
    // Counts
    //-------------------------------------------------------------------------
    // These counts establish the authoritative semantic domains for validation.
    // The species domain [1, num_pokemon] is used by SemanticLegalizer to validate
    // SpeciesId references. For vanilla Crystal, this is contiguous 1-251.
    //
    // GUARANTEE: num_pokemon establishes a closed, contiguous species domain.
    // There are no holes - all 251 species have definitions in base_stats/*.asm.
    //-------------------------------------------------------------------------
    auto& c = profile.counts;
    c.num_pokemon           = 251;  // Species domain [1-251] - contiguous, no holes
    c.num_moves             = 251;  // 1-251 (0 = none)
    c.num_items             = 256;  // 0-255
    c.num_types             = 18;   // 0-17 (includes ???)
    c.num_tilesets          = 36;
    c.num_map_groups        = 26;   // Groups 1-26 (0 unused in some contexts)
    c.num_trainer_classes   = 67;
    c.num_specials          = 0x100;
    c.num_script_commands   = 0xA7;
    
    // Audio enumeration counts (from pokecrystal/constants)
    c.num_music             = 103;  // NUM_MUSIC_SONGS (0-102)
    c.num_sfx               = 207;  // NUM_SFX (0-206)
    
    // Commerce/interaction resource counts (from pokecrystal/constants)
    c.num_emotes            = 12;   // NUM_EMOTES (0-11)
    c.num_phone_contacts    = 38;   // NUM_PHONE_CONTACTS (0-37)
    c.num_npc_trades        = 7;    // NUM_NPC_TRADES (0-6)
    c.num_fruit_trees       = 30;   // NUM_FRUIT_TREES (1-30, 0 is invalid)
    c.num_marts             = 34;   // NUM_MARTS (0-33)
    
    //-------------------------------------------------------------------------
    // Register
    //-------------------------------------------------------------------------
    hash_to_version_[profile.sha1] = profile.version;
    profiles_[profile.version] = profile;
    supported_list_.emplace_back(profile.sha1, profile.version_string);
}

std::optional<RomVersion> ProfileRegistry::identify(std::string_view sha1) const {
    auto it = hash_to_version_.find(std::string(sha1));
    if (it != hash_to_version_.end()) {
        return it->second;
    }
    return std::nullopt;  // Unknown ROM - no fallback guessing
}

const ExtractionProfile* ProfileRegistry::get_profile(RomVersion version) const {
    auto it = profiles_.find(version);
    if (it != profiles_.end()) {
        return &it->second;
    }
    return nullptr;
}

const ExtractionProfile* ProfileRegistry::get_profile_by_hash(std::string_view sha1) const {
    auto version = identify(sha1);
    if (version) {
        return get_profile(*version);
    }
    return nullptr;  // ROM not supported
}

const std::vector<std::pair<std::string, std::string>>& ProfileRegistry::supported_roms() const {
    return supported_list_;
}

//=============================================================================
// ProfileGenerator
//=============================================================================

ExtractionProfile ProfileGenerator::generate(
    const SymbolMap& symbols,
    RomVersion version,
    std::string_view sha1,
    const GeneratorConfig& config)
{
    ExtractionProfile profile;
    profile.version = version;
    profile.sha1 = std::string(sha1);
    
    // Provenance
    profile.provenance.generator_version = config.generator_version;
    profile.provenance.pokecrystal_commit = config.pokecrystal_commit;
    profile.provenance.symbol_file = config.symbol_file;
    // generated_date set by caller
    
    // Helper to get flat address or 0 if not found
    auto addr = [&symbols](const char* name) -> uint32_t {
        auto result = symbols.address(name);
        return result.value_or(0);
    };
    
    // Helper to get bank from symbol or default
    auto bank = [&symbols](const char* name, uint8_t default_bank) -> uint8_t {
        auto* sym = symbols.find(name);
        return sym ? sym->bank : default_bank;
    };
    
    // Offsets from symbols
    auto& o = profile.offsets;
    o.map_group_pointers    = addr("MapGroupPointers");
    o.map_groups_bank       = bank("MapGroupPointers", 0x25);  // Default 0x25 for Crystal
    o.spawn_points          = addr("SpawnPoints");
    o.script_command_table  = addr("ScriptCommandTable");
    o.special_pointers      = addr("SpecialsPointers");
    o.base_data             = addr("BaseData");
    o.pokemon_names         = addr("PokemonNames");
    o.evos_attacks          = addr("EvosAttacksPointers");
    o.egg_move_pointers     = addr("EggMovePointers");
    o.moves                 = addr("Moves");
    o.move_names            = addr("MoveNames");
    o.item_attributes       = addr("ItemAttributes");
    o.item_names            = addr("ItemNames");
    o.type_matchups         = addr("TypeMatchups");
    o.type_names            = addr("TypeNames");
    o.trainer_groups        = addr("TrainerGroups");
    o.trainer_class_names   = addr("TrainerClassNames");
    o.johto_grass_wild      = addr("JohtoGrassWildMons");
    o.kanto_grass_wild      = addr("KantoGrassWildMons");
    o.johto_water_wild      = addr("JohtoWaterWildMons");
    o.swarm_grass_wild      = addr("SwarmGrassWildMons");
    o.tilesets              = addr("Tilesets");
    o.pokemon_pic_pointers  = addr("PokemonPicPointers");
    o.trainer_pic_pointers  = addr("TrainerPicPointers");
    o.music_pointers        = addr("Music");
    o.sfx_pointers          = addr("SFX");
    o.text_commands         = addr("TextCommands");
    
    // Format rules and counts use defaults (Crystal-standard)
    // Could be customized per version if needed
    
    return profile;
}

std::string ProfileGenerator::export_cpp(const ExtractionProfile& profile) {
    std::ostringstream out;
    
    out << "// Auto-generated Crystal extraction profile\n";
    out << "// Version: " << profile.version_string << "\n";
    out << "// SHA-1: " << profile.sha1 << "\n";
    out << "// Generator: " << profile.provenance.generator_version << "\n";
    out << "// Source: " << profile.provenance.symbol_file << "\n";
    out << "// pokecrystal: " << profile.provenance.pokecrystal_commit << "\n";
    out << "// Generated: " << profile.provenance.generated_date << "\n\n";
    
    out << "void ProfileRegistry::register_crystal_v11() {\n";
    out << "    constexpr const char* SHA1 = \"" << profile.sha1 << "\";\n\n";
    
    out << "    ExtractionProfile profile;\n";
    out << "    profile.version = RomVersion::Crystal_USA_v1_1;\n";
    out << "    profile.version_string = \"" << profile.version_string << "\";\n";
    out << "    profile.sha1 = SHA1;\n\n";
    
    out << "    // Provenance\n";
    out << "    profile.provenance.generator_version = \"" << profile.provenance.generator_version << "\";\n";
    out << "    profile.provenance.pokecrystal_commit = \"" << profile.provenance.pokecrystal_commit << "\";\n";
    out << "    profile.provenance.symbol_file = \"" << profile.provenance.symbol_file << "\";\n";
    out << "    profile.provenance.generated_date = \"" << profile.provenance.generated_date << "\";\n\n";
    
    out << "    // Offsets\n";
    out << "    auto& o = profile.offsets;\n";
    const auto& o = profile.offsets;
    out << std::format("    o.map_group_pointers    = 0x{:05x};\n", o.map_group_pointers);
    out << std::format("    o.script_command_table  = 0x{:05x};\n", o.script_command_table);
    out << std::format("    o.special_pointers      = 0x{:05x};\n", o.special_pointers);
    out << std::format("    o.base_data             = 0x{:05x};\n", o.base_data);
    out << std::format("    o.pokemon_names         = 0x{:05x};\n", o.pokemon_names);
    out << std::format("    o.evos_attacks          = 0x{:05x};\n", o.evos_attacks);
    out << std::format("    o.egg_move_pointers     = 0x{:05x};\n", o.egg_move_pointers);
    out << std::format("    o.moves                 = 0x{:05x};\n", o.moves);
    out << std::format("    o.move_names            = 0x{:05x};\n", o.move_names);
    out << std::format("    o.item_attributes       = 0x{:05x};\n", o.item_attributes);
    out << std::format("    o.item_names            = 0x{:05x};\n", o.item_names);
    out << std::format("    o.type_matchups         = 0x{:05x};\n", o.type_matchups);
    out << std::format("    o.type_names            = 0x{:05x};\n", o.type_names);
    out << std::format("    o.trainer_groups        = 0x{:05x};\n", o.trainer_groups);
    out << std::format("    o.trainer_class_names   = 0x{:05x};\n", o.trainer_class_names);
    out << std::format("    o.johto_grass_wild      = 0x{:05x};\n", o.johto_grass_wild);
    out << std::format("    o.kanto_grass_wild      = 0x{:05x};\n", o.kanto_grass_wild);
    out << std::format("    o.johto_water_wild      = 0x{:05x};\n", o.johto_water_wild);
    out << std::format("    o.swarm_grass_wild      = 0x{:05x};\n", o.swarm_grass_wild);
    out << std::format("    o.tilesets              = 0x{:05x};\n", o.tilesets);
    out << std::format("    o.pokemon_pic_pointers  = 0x{:05x};\n", o.pokemon_pic_pointers);
    out << std::format("    o.trainer_pic_pointers  = 0x{:05x};\n", o.trainer_pic_pointers);
    out << std::format("    o.music_pointers        = 0x{:05x};\n", o.music_pointers);
    out << std::format("    o.sfx_pointers          = 0x{:05x};\n", o.sfx_pointers);
    out << std::format("    o.text_commands         = 0x{:05x};\n", o.text_commands);
    
    out << "\n    // Counts\n";
    out << "    auto& c = profile.counts;\n";
    const auto& c = profile.counts;
    out << std::format("    c.num_pokemon         = {};\n", c.num_pokemon);
    out << std::format("    c.num_moves           = {};\n", c.num_moves);
    out << std::format("    c.num_items           = {};\n", c.num_items);
    out << std::format("    c.num_types           = {};\n", c.num_types);
    out << std::format("    c.num_tilesets        = {};\n", c.num_tilesets);
    out << std::format("    c.num_map_groups      = {};\n", c.num_map_groups);
    
    out << "\n    // Register\n";
    out << "    hash_to_version_[profile.sha1] = profile.version;\n";
    out << "    profiles_[profile.version] = profile;\n";
    out << "    supported_list_.emplace_back(profile.sha1, profile.version_string);\n";
    out << "}\n";
    
    return out.str();
}

std::string ProfileGenerator::export_json(const ExtractionProfile& profile) {
    std::ostringstream out;
    
    out << "{\n";
    out << "  \"version\": \"" << profile.version_string << "\",\n";
    out << "  \"sha1\": \"" << profile.sha1 << "\",\n";
    out << "  \"provenance\": {\n";
    out << "    \"generator_version\": \"" << profile.provenance.generator_version << "\",\n";
    out << "    \"pokecrystal_commit\": \"" << profile.provenance.pokecrystal_commit << "\",\n";
    out << "    \"symbol_file\": \"" << profile.provenance.symbol_file << "\",\n";
    out << "    \"generated_date\": \"" << profile.provenance.generated_date << "\"\n";
    out << "  },\n";
    out << "  \"offsets\": {\n";
    
    const auto& o = profile.offsets;
    out << std::format("    \"map_group_pointers\": {},\n", o.map_group_pointers);
    out << std::format("    \"script_command_table\": {},\n", o.script_command_table);
    out << std::format("    \"base_data\": {},\n", o.base_data);
    out << std::format("    \"moves\": {},\n", o.moves);
    out << std::format("    \"tilesets\": {}\n", o.tilesets);
    out << "  },\n";
    
    out << "  \"counts\": {\n";
    const auto& c = profile.counts;
    out << std::format("    \"num_pokemon\": {},\n", c.num_pokemon);
    out << std::format("    \"num_moves\": {},\n", c.num_moves);
    out << std::format("    \"num_map_groups\": {}\n", c.num_map_groups);
    out << "  }\n";
    out << "}\n";
    
    return out.str();
}

} // namespace crystal
