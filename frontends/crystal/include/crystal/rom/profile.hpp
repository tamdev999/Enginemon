#pragma once
// crystal/rom/profile.hpp
// ROM extraction profiles: identity + locations + format rules
// 
// A profile is versioned decoding metadata, not merely a symbol-address table.
// It encodes how to interpret ROM structures, not just where they are.
//
// Development: symbol_map.hpp parses .sym files for verification/generation
// Release: profiles provide complete extraction rules without .sym files
// 
// All ROM-specific addresses/formats stay here - never leak to engine or runtime

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace crystal {

// Supported ROM versions with known profiles
enum class RomVersion {
    Unknown,
    Crystal_USA_v1_0,    // USA Rev 0
    Crystal_USA_v1_1,    // USA Rev 1 (most common)
    Crystal_EUR,         // Europe
    Crystal_JPN,         // Japan
    Crystal_AUS          // Australia
};

//=============================================================================
// PROVENANCE - where this profile came from
//=============================================================================

struct ProfileProvenance {
    std::string generator_version;      // e.g., "1.0.0"
    std::string pokecrystal_commit;     // e.g., "abc123" or branch name
    std::string symbol_file;            // e.g., "pokecrystal11.sym"
    std::string generated_date;         // ISO date when profile was created
};

//=============================================================================
// FORMAT RULES - how to interpret data structures
//=============================================================================

// Pointer format used throughout Crystal ROM
enum class PointerFormat {
    // bank (1 byte) + addr (2 bytes little-endian)
    BankAddrLE,
    // addr (2 bytes) only, bank inferred from context
    AddrOnly,
    // 3-byte flat offset (used in some later games)
    Flat24
};

// Map system format rules
struct MapFormatRules {
    // MapGroupPointers structure
    PointerFormat group_pointer_format = PointerFormat::AddrOnly;
    uint8_t maps_per_group_offset = 0;  // offset to map count in group header
    
    // Map header structure (MapAttributes from data/maps/attributes.asm)
    // Crystal format: border, height, width, block_bank, block_ptr(2), 
    //                 script_bank, script_ptr(2), events_ptr(2), connections
    uint8_t header_size = 12;           // bytes per map header
    uint8_t border_block_offset = 0;
    uint8_t height_offset = 1;
    uint8_t width_offset = 2;
    uint8_t blockdata_bank_offset = 3;
    uint8_t blockdata_ptr_offset = 4;   // 2 bytes little-endian
    uint8_t script_bank_offset = 6;     // also events bank
    uint8_t script_ptr_offset = 7;      // 2 bytes little-endian
    uint8_t events_ptr_offset = 9;      // 2 bytes little-endian
    uint8_t connections_offset = 11;    // connections byte (bitfield)
    
    // Map scripts header
    uint8_t map_script_header_size = 3; // callbacks pointer + scene scripts count
    
    // Connection format (from data/maps/attributes.asm connection macro)
    // group, map, blocks_ptr(2), map_ptr(2), length, width, y, x, window_ptr(2)
    uint8_t connection_size = 12;       // bytes per connection entry
    
    // Event format (from macros/scripts/maps.asm)
    uint8_t warp_size = 5;              // db y, x, warp_dest, group, map
    uint8_t coord_event_size = 8;       // db scene, y, x, 0; dw script; dw 0
    uint8_t bg_event_size = 5;          // db y, x, type; dw script
    uint8_t object_event_size = 13;     // complex structure
};

// Pokemon data format rules
struct PokemonFormatRules {
    uint8_t base_data_size = 32;        // bytes per Pokemon in BaseData
    uint8_t name_length = 10;           // chars per Pokemon name (padded)
    
    // BaseData field offsets
    uint8_t dex_num_offset = 0;
    uint8_t hp_offset = 1;
    uint8_t atk_offset = 2;
    uint8_t def_offset = 3;
    uint8_t spd_offset = 4;
    uint8_t satk_offset = 5;
    uint8_t sdef_offset = 6;
    uint8_t type1_offset = 7;
    uint8_t type2_offset = 8;
    uint8_t catch_rate_offset = 9;
    uint8_t base_exp_offset = 10;
    uint8_t items_offset = 11;          // 2 bytes: item1, item2
    uint8_t gender_offset = 13;
    uint8_t egg_cycles_offset = 15;
    uint8_t growth_rate_offset = 22;
    uint8_t egg_groups_offset = 23;
    uint8_t tmhm_offset = 24;           // 8 bytes bitfield
};

// Move data format rules
struct MoveFormatRules {
    uint8_t move_data_size = 7;
    uint8_t name_length = 12;
    
    // Move data field offsets
    uint8_t anim_offset = 0;
    uint8_t effect_offset = 1;
    uint8_t power_offset = 2;
    uint8_t type_offset = 3;
    uint8_t accuracy_offset = 4;
    uint8_t pp_offset = 5;
    uint8_t effect_chance_offset = 6;
};

// Item data format rules
struct ItemFormatRules {
    uint8_t attr_size = 7;
    uint8_t name_length = 12;
    
    uint8_t price_offset = 0;           // 2 bytes
    uint8_t held_effect_offset = 2;
    uint8_t param_offset = 3;
    uint8_t property_offset = 4;
    uint8_t pocket_offset = 5;
    uint8_t field_menu_offset = 6;
};

// Tileset format rules
struct TilesetFormatRules {
    uint8_t tileset_size = 15;          // TILESET_LENGTH from tileset_constants.asm
    
    // Tileset entry (from data/tilesets.asm tileset macro):
    // dba GFX, Meta, Coll; dw Anim; dw NULL; dw PalMap
    uint8_t gfx_bank_offset = 0;
    uint8_t gfx_ptr_offset = 1;         // 2 bytes
    uint8_t metatile_bank_offset = 3;
    uint8_t metatile_ptr_offset = 4;    // 2 bytes
    uint8_t coll_bank_offset = 6;
    uint8_t coll_ptr_offset = 7;        // 2 bytes
    uint8_t anim_offset = 9;            // 2 bytes (pointer)
    uint8_t null_offset = 11;           // 2 bytes (unused)
    uint8_t palmap_offset = 13;         // 2 bytes (palette map pointer)
    
    // Metatile format: 16 bytes per metatile (4×4 tile indices)
    // Each metatile is a 4×4 arrangement of 8x8 tiles = 32×32 pixels
    // From pokecrystal/constants/gfx_constants.asm: DEF METATILE_WIDTH EQU 4
    // From pokecrystal/home/map.asm LoadMetatiles: block_index * 16 bytes
    uint8_t metatile_size = 16;         // bytes per metatile (16 tile indices)
    uint8_t metatile_count = 128;       // standard count per tileset
};

// Script format rules
struct ScriptFormatRules {
    uint8_t command_table_entry_size = 3;  // 3-byte pointers in command table
    PointerFormat script_pointer_format = PointerFormat::BankAddrLE;
    
    // Text encoding
    uint8_t text_terminator = 0x50;     // '@' in Crystal encoding
    uint8_t line_terminator = 0x4F;     // '<NEXT>' 
    uint8_t paragraph_terminator = 0x51; // '<PARA>'
};

// Text/character encoding
struct TextFormatRules {
    // Crystal uses a custom character map, not ASCII
    // We store the mapping table offset, or use a baked table
    bool uses_custom_charmap = true;
    uint8_t max_string_length = 80;     // safety limit
    uint8_t string_terminator = 0x50;
};

// All format rules combined
struct FormatRules {
    MapFormatRules map;
    PokemonFormatRules pokemon;
    MoveFormatRules move;
    ItemFormatRules item;
    TilesetFormatRules tileset;
    ScriptFormatRules script;
    TextFormatRules text;
};

//=============================================================================
// LOCATIONS - where data structures are in ROM
//=============================================================================

struct ProfileOffsets {
    // Map system
    uint32_t map_group_pointers;        // MapGroupPointers table
    uint8_t map_groups_bank;            // Bank containing MapGroupPointers (0x25 for Crystal)
    uint32_t map_names;                 // MapNames (for display)
    uint32_t spawn_points;              // SpawnPoints table (fly/respawn destinations)
    
    // Script system
    uint32_t script_command_table;      // ScriptCommandTable
    uint32_t special_pointers;          // SpecialsPointers
    
    // Pokemon data
    uint32_t base_data;                 // BaseData (all Pokemon base stats)
    uint32_t pokemon_names;             // PokemonNames
    uint32_t evos_attacks;              // EvosAttacksPointers
    uint32_t egg_move_pointers;         // EggMovePointers
    
    // Move data
    uint32_t moves;                     // Moves (move data table)
    uint32_t move_names;                // MoveNames
    
    // Item data
    uint32_t item_attributes;           // ItemAttributes
    uint32_t item_names;                // ItemNames
    
    // Type data
    uint32_t type_matchups;             // TypeMatchups effectiveness table
    uint32_t type_names;                // TypeNames
    
    // Trainer data
    uint32_t trainer_groups;            // TrainerGroups (party data)
    uint32_t trainer_class_names;       // TrainerClassNames
    
    // Encounter data
    uint32_t johto_grass_wild;          // JohtoGrassWildMons
    uint32_t kanto_grass_wild;          // KantoGrassWildMons
    uint32_t johto_water_wild;          // JohtoWaterWildMons
    uint32_t swarm_grass_wild;          // SwarmGrassWildMons
    
    // Graphics
    uint32_t tilesets;                  // Tilesets table
    uint32_t pokemon_pic_pointers;      // PokemonPicPointers
    uint32_t trainer_pic_pointers;      // TrainerPicPointers
    uint32_t unown_pic_pointers;        // UnownPicPointers
    
    // Audio
    uint32_t music_pointers;            // Music header table
    uint32_t sfx_pointers;              // SFX header table
    uint32_t cry_data;                  // Cry data
    
    // Text
    uint32_t text_commands;             // TextCommands jump table
    
    // Script system extended
    uint32_t std_scripts;               // StdScripts table (bank 0x2f)
    uint16_t std_scripts_count;         // Number of standard scripts (52)
};

//=============================================================================
// COUNTS - cardinalities that may differ between versions
//=============================================================================

struct ProfileCounts {
    uint16_t num_pokemon = 251;         // Includes Pokemon 0 (none)
    uint16_t num_moves = 251;           // Includes move 0 (none)
    uint16_t num_items = 256;           // 0-255
    uint16_t num_types = 18;            // 17 types + ??? type
    uint16_t num_tilesets = 36;
    uint16_t num_map_groups = 26;
    uint16_t num_trainer_classes = 67;
    uint16_t num_specials = 0x100;      // Special function count (SpecialsPointers)
    uint16_t num_script_commands = 0xA7;
    
    // Audio enumeration counts (from pokecrystal constants)
    uint16_t num_music = 103;           // NUM_MUSIC_SONGS (0-102)
    uint16_t num_sfx = 207;             // NUM_SFX (0-206)
    
    // Commerce/interaction resource counts (from pokecrystal constants)
    uint16_t num_emotes = 12;           // NUM_EMOTES (0-11)
    uint16_t num_phone_contacts = 38;   // NUM_PHONE_CONTACTS (0-37)
    uint16_t num_npc_trades = 7;        // NUM_NPC_TRADES (0-6)
    uint16_t num_fruit_trees = 30;      // NUM_FRUIT_TREES (1-30, 0 is invalid)
    uint16_t num_marts = 34;            // NUM_MARTS (0-33)
};

//=============================================================================
// COMPLETE PROFILE
//=============================================================================

// A complete extraction profile: identity + locations + format rules
struct ExtractionProfile {
    // Identity
    RomVersion version = RomVersion::Unknown;
    std::string version_string;
    std::string sha1;                   // Exact SHA-1 required for match
    
    // Provenance (how this profile was generated)
    ProfileProvenance provenance;
    
    // Format rules (how to interpret structures)
    FormatRules format;
    
    // Locations (where structures are)
    ProfileOffsets offsets;
    
    // Counts (cardinalities)
    ProfileCounts counts;
};

//=============================================================================
// PROFILE REGISTRY
//=============================================================================

// Profile registry - maps ROM hashes to profiles
// Strict matching only: no fallback to "looks like Crystal"
class ProfileRegistry {
public:
    static ProfileRegistry& instance();
    
    // Strict identification by exact SHA-1 hash
    // Returns nullopt if hash not recognized - no guessing
    std::optional<RomVersion> identify(std::string_view sha1) const;
    
    // Get profile for known version
    const ExtractionProfile* get_profile(RomVersion version) const;
    
    // Get profile by exact ROM hash (combines identify + get_profile)
    // Returns nullptr if ROM not supported - caller must handle
    const ExtractionProfile* get_profile_by_hash(std::string_view sha1) const;
    
    // Get all supported ROMs (for error messages)
    const std::vector<std::pair<std::string, std::string>>& supported_roms() const;
    
private:
    ProfileRegistry();
    
    std::unordered_map<std::string, RomVersion> hash_to_version_;
    std::unordered_map<RomVersion, ExtractionProfile> profiles_;
    std::vector<std::pair<std::string, std::string>> supported_list_; // sha1 -> name
    
    void register_crystal_v11();
};

//=============================================================================
// PROFILE GENERATOR (development tool)
//=============================================================================

// Generate profiles from symbol files (development only)
class ProfileGenerator {
public:
    struct GeneratorConfig {
        std::string generator_version = "1.0.0";
        std::string pokecrystal_commit;
        std::string symbol_file;
    };
    
    // Generate profile from symbol map
    static ExtractionProfile generate(
        const class SymbolMap& symbols,
        RomVersion version,
        std::string_view sha1,
        const GeneratorConfig& config);
    
    // Export profile as C++ source (for baking into build)
    static std::string export_cpp(const ExtractionProfile& profile);
    
    // Export profile as JSON (for external tools/versioning)
    static std::string export_json(const ExtractionProfile& profile);
};

} // namespace crystal
