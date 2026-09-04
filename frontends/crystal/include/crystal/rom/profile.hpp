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
#include <array>

namespace crystal {

// Supported ROM versions with known profiles
enum class RomVersion {
    Unknown,
    Crystal_USA_v1_0,    // USA Rev 0
    Crystal_USA_v1_1,    // USA Rev 1 (most common)
    Polished_Crystal_3_2_3, // Polished Crystal 3.2.3 by Rangi42
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
    uint8_t events_ptr_offset = 9;      // 2 bytes little-endian; 0xFF = not present in header
    uint8_t connections_offset = 11;    // connections byte (bitfield)
    
    // Map scripts header
    // scene_script_size: bytes per scene script entry in the MapScriptHeader.
    // Vanilla Crystal: 4 bytes (dw script_ptr, dw 0 filler) — SCENE_SCRIPT_SIZE = 4
    // Polished Crystal: 2 bytes (dw script_ptr only)       — SCENE_SCRIPT_SIZE = 2
    // This value is ROM-derivable via the xref pattern in the map-loading routine:
    //   ld a, [hli] / ld bc, SCENE_SCRIPT_SIZE / call|rst AddNTimes
    // resolve_crystal_layout() fills this from the ROM; the default below is the
    // vanilla correct value so vanilla-profile compilations work without the resolver.
    uint8_t map_script_header_size = 4; // SCENE_SCRIPT_SIZE: vanilla=4, Polished=2
    
    // Connection format (from data/maps/attributes.asm connection macro)
    // group, map, blocks_ptr(2), map_ptr(2), length, width, y, x, window_ptr(2)
    uint8_t connection_size = 12;       // bytes per connection entry
    
    // Event format (from macros/scripts/maps.asm)
    uint8_t warp_size = 5;              // db y, x, warp_dest, group, map
    uint8_t coord_event_size = 8;       // db scene, y, x, 0; dw script; dw 0
    uint8_t bg_event_size = 5;          // db y, x, type; dw script
    uint8_t object_event_size = 13;     // complex structure

    // MapGroup entry size (MAP_LENGTH from map_data_constants.asm)
    // Crystal: 9 bytes per entry (attr_bank, tileset, environment, attr_ptr(2),
    //          location, music, phone_palette, fishgroup)
    // Polished Crystal: 7 bytes per entry (tileset, dn(sign,env), attr_ptr(2),
    //          location, music, dn(phone_flag,palette)) — attr_bank removed, fishgroup removed
    // GEN2-STABLE: same layout in Gold/Silver — field meanings identical.
    uint8_t map_entry_size = 9;

    // Map entry field layout selectors:
    //
    // attr_bank_in_entry: vanilla Crystal stores the MapAttributes bank as byte[0]
    //   of the 9-byte entry. Polished Crystal removes this field entirely.
    //   When false, the bank must be resolved by ROM scan (see resolve_attr_bank_by_scan).
    bool attr_bank_in_entry = true;

    // sign_env_nibble: when true, byte[1] of the map entry packs
    //   high-nibble=SIGN_* and low-nibble=ENV_* (Polished Crystal).
    //   When false, byte[2] is environment directly (vanilla layout).
    bool sign_env_nibble = false;

    // resolve_attr_bank_by_scan: when attr_bank_in_entry is false, the extractor
    //   must discover the correct ROM bank for each MapAttributes pointer by scanning
    //   all ROM banks and applying a structural validity test.
    //   This is required for Polished Crystal where MapAttributes are spread across
    //   many banks with no explicit bank byte in the map entry.
    bool resolve_attr_bank_by_scan = false;

    // events_ptr_in_header: vanilla Crystal has events_ptr(2) at bytes 9-10 of the
    //   12-byte MapAttributes header. Polished removes the separate events pointer;
    //   events are reached via the MapScriptHeader. Set false for Polished.
    bool events_ptr_in_header = true;

    // events_in_script_header: when true (Polished Crystal), map events (warps, coord,
    //   bg, objects) are packed inline after scene/callback data inside the MapScriptHeader
    //   blob pointed to by script_bank/script_ptr. The layout (from home/map.asm) is:
    //     [db scene_count] [scene_count × SCENE_SCRIPT_SIZE bytes] 
    //     [db callback_count] [callback_count × CALLBACK_SIZE bytes]
    //     [db warp_count] [warps]
    //     [db coord_count] [coord_events]
    //     [db bg_count] [bg_events]
    //     [db object_count] [objects]
    //   When false (vanilla), events are in a separate structure pointed to by events_ptr.
    bool events_in_script_header = false;

    // attr_ptr_field_offset: byte offset within the map entry where the 2-byte
    //   MapAttributes pointer starts.
    //   Vanilla Crystal: byte 3 (after attr_bank, tileset, environment = 3 bytes)
    //   Polished Crystal: byte 2 (after tileset, sign_env = 2 bytes)
    uint8_t attr_ptr_field_offset = 3;

    // location_field_offset, music_field_offset, phone_palette_field_offset:
    //   byte offsets within the map entry.
    //   Vanilla Crystal: location=5, music=6, phone_palette=7
    //   Polished Crystal: location=4, music=5, phone_palette=6
    uint8_t location_field_offset     = 5;
    uint8_t music_field_offset        = 6;
    uint8_t phone_palette_field_offset = 7;

    // allow_unknown_sprites: when true, sprite bytes outside the known vanilla
    //   Crystal namespaces (0x67-0x7F, 0xA3-0xDF, 0xFD-0xFF) are tolerated and
    //   mapped to "unknown:<hex>" rather than throwing. Required for Polished Crystal
    //   which extends the fixed sprite range beyond 0x66.
    bool allow_unknown_sprites = false;

    // max_environment_value: largest valid environment byte in a map group entry.
    // Vanilla Crystal: 7 (TOWN=1..DUNGEON=7, from map_data_constants.asm NUM_ENVIRONMENTS)
    // Polished Crystal: 8 (adds ISOLATED=3, shifting INDOOR/GATE/CAVE/DUNGEON up by 1)
    // This is a profile field rather than a ROM-derivable constant because the
    // EnvironmentColorsPointers table uses different encoding (dw vs dr) across versions,
    // making generic counting unreliable.
    uint8_t max_environment_value = 7;

    // max_map_dimension: largest valid single map dimension (width or height, in blocks).
    // Used as a structural plausibility guard when scanning ROM banks for MapAttributes.
    // Vanilla Crystal: actual maximum is 54×40 (large outdoor routes); 128 is conservative
    //   and covers all known vanilla maps with headroom.
    // Polished Crystal: outdoor maps can be ~50×50+; set to 200 in profile.
    // This is not a ROM-derivable constant — no single ROM value encodes "max map size".
    uint8_t max_map_dimension = 128;
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

    // Per-move Physical/Special/Status category offset.
    // 0xFF = not present: derive category from type byte using Gen 2 type-based split.
    // Non-0xFF: index into the move record where category is stored (Polished Crystal
    //   and Gen 4+ style ROMs that add a per-move P/S field beyond the vanilla 7 bytes).
    // Values at that offset: 0=Physical, 1=Special, 2=Status.
    uint8_t category_offset = 0xFF;
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

    // StdScripts table entry size.
    // Vanilla Crystal: 3 bytes per entry — dba macro = bank(1) + addr(2).
    // Polished Crystal: 2 bytes per entry — dw macro = addr(2) only; all scripts
    //   in same bank as the table, so no explicit bank stored.
    // The bank for 2-byte entries is derived from the table address itself.
    uint8_t std_scripts_entry_size = 3;
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
// NATIVE CALL AND RAM ADDRESS SPECS
// Profile-driven knowledge of known callasm targets and readmem/writemem RAM
// addresses. Previously hardcoded in NativeCallRegistry::initialize() and
// RamAddressRegistry::initialize() with Crystal v1.1 flat addresses.
//
// Moving them here lets a non-vanilla profile override the addresses without
// changing any C++ code. Gold/Silver profiles provide their own tables.
//=============================================================================

// Classification enumerations (mirrored from native_registry.hpp for independence)
// These must stay in sync with NativeClassification / NativeControlFlow.
enum class NativeCallClass : uint8_t {
    PureSemantic  = 0,
    HostCapability = 1,
    Trivial       = 2,
    Opaque        = 3,
};

enum class NativeCallFlow : uint8_t {
    Returns          = 0,
    Terminal         = 1,
    ComputedTransfer = 2,
    Unknown          = 3,
};

struct NativeCallSpec {
    uint32_t flat_address = 0;           // Flat ROM address (bank * 0x4000 + offset)
    const char* symbol_name   = nullptr; // e.g. "HealParty"
    const char* semantic_name = nullptr; // e.g. "heal_party"
    NativeCallClass classification = NativeCallClass::Opaque;
    NativeCallFlow  control_flow   = NativeCallFlow::Unknown;
    const char* source_ref = nullptr;    // Source file reference
    const char* notes      = nullptr;    // Optional notes
};

struct RamAddressSpec {
    uint16_t    address       = 0;       // GB WRAM address (e.g. 0xc2dd)
    const char* symbol_name   = nullptr; // e.g. "wScriptVar"
    const char* semantic_name = nullptr; // e.g. "script_var"
    uint8_t     classification = 0;      // mirrors RamClassification
    const char* source_ref    = nullptr;
    const char* notes         = nullptr;
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

    // Per-group MapAttributes bank table.
    //
    // When a map entry does not carry an explicit attr_bank byte
    // (MapFormatRules::attr_bank_in_entry == false), the bank for each map
    // group's MapAttributes block is resolved once at compile time by
    // resolve_group_attr_banks() in crystal_layout_resolver and stored here.
    //
    // Index: group number (1-based).  group_attr_banks[0] is unused (sentinel).
    // Value 0xFF means "not yet resolved" (also the initial sentinel value).
    // Entries for groups beyond num_map_groups are 0xFF.
    //
    // Vanilla Crystal (attr_bank_in_entry == true): all entries stay 0xFF —
    // the bank is read directly from each map entry and this table is unused.
    //
    // Array sized to MAX_MAP_GROUPS (64) — enough headroom for any known hack.
    // The resolver fills only groups [1 .. num_map_groups].
    static constexpr uint8_t  ATTR_BANK_UNRESOLVED = 0xFF;
    static constexpr uint32_t MAX_MAP_GROUPS        = 64;
    std::array<uint8_t, 64> group_attr_banks = [](){
        std::array<uint8_t, 64> a{};
        a.fill(0xFF);
        return a;
    }();
    
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

    // Graphics — additional tables (moved from inline constexpr in extractors)
    // These were previously hardcoded in sprites.cpp / fonts.cpp / tilesets.cpp.
    // Moving them here makes them profile-driven and hackable.
    uint32_t overworld_sprites;         // OverworldSprites table (6 bytes/entry)
    uint32_t mon_menu_icons;            // MonMenuIcons table (1 byte/species, 0-indexed)
    uint32_t icon_pointers;             // IconPointers table (2 bytes/icon_type, dw)
    uint32_t obj_palettes;              // MapObjectPals (OBJ time-of-day palette sets)
    uint32_t tileset_bg_palette;        // TilesetBGPalette (BG time-of-day palette sets)
    uint32_t font_tiles;                // Font (main 1bpp font, 128 tiles)
    uint32_t font_extra_tiles;          // FontExtra (border/extra 2bpp font, 32 tiles)

    // Special per-tileset palette overrides.
    // Each entry: (tileset_1indexed, flat_rom_address) of a 7-palette (56-byte) block.
    // The table is profile-driven so ROM hacks that relocate palette data still work.
    // Crystal v1.1 has 6 overrides (HOUSE, MANSION, POKECOM, BATTLETOWER, RADIOTOWER, ICEPATH).
    static constexpr size_t MAX_SPECIAL_TILESET_PALETTES = 16;
    struct SpecialTilesetPalette {
        uint8_t  tileset_index = 0;   // 1-indexed tileset number
        uint32_t rom_address   = 0;   // Flat ROM address of 7-palette block (56 bytes)
    };
    std::array<SpecialTilesetPalette, MAX_SPECIAL_TILESET_PALETTES> special_tileset_palettes{};
    uint8_t special_tileset_palette_count = 0;

    // Native call and RAM address specs — moved from NativeCallRegistry::initialize()
    // and RamAddressRegistry::initialize(). These are now profile data so non-vanilla
    // profiles (ROM hacks, Gold/Silver) can provide their own address tables.
    //
    // NativeCallRegistry::initialize(profile) iterates native_calls and registers them.
    // RamAddressRegistry::initialize(profile) iterates ram_addresses and registers them.
    //
    // Crystal v1.1 entries are populated in register_crystal_v11().
    static constexpr size_t MAX_NATIVE_CALLS   = 32;
    static constexpr size_t MAX_RAM_ADDRESSES  = 32;

    std::array<NativeCallSpec,  MAX_NATIVE_CALLS>  native_calls{};
    uint8_t native_call_count = 0;

    std::array<RamAddressSpec, MAX_RAM_ADDRESSES> ram_addresses{};
    uint8_t ram_address_count = 0;

    // Battle rule tables — ROM addresses for all tables extracted into the BRLS chunk.
    // These were previously hardcoded as static constexpr in calculator.cpp / trainer_ai.cpp.
    // Moving them here makes them profile-driven and ROM-hack compatible.
    //
    // Sentinel-terminated tables (extractor scans until 0xFF): all weather/AI/move lists.
    // Fixed-length tables (extractor reads exact count): stat_mult, acc_mult, crit_chances.
    // Count-driven tables: wobble_probabilities (num_wobble_entries), trainer_class_ai
    //   uses profile.counts.num_trainer_classes.
    //
    // Crystal v1.1 addresses from pokecrystal.sym:
    uint32_t stat_level_multipliers;    // 0f:6d2b  StatLevelMultipliers_Applied — 13×2 bytes
    uint32_t accuracy_level_multipliers;// 0d:4eb2  AccuracyLevelMultipliers    — 13×2 bytes
    uint32_t critical_hit_chances;      // 0d:46ab  CriticalHitChances          — 7 bytes
    uint32_t wobble_probabilities;      // 03:79ba  WobbleProbabilities         — num_wobble_entries×2
    uint32_t weather_type_modifiers;    // 3e:7e13  WeatherTypeModifiers        — 3 bytes/entry, 0xFF sentinel
    uint32_t weather_move_modifiers;    // 3e:7e20  WeatherMoveModifiers        — 3 bytes/entry, 0xFF sentinel
    uint32_t critical_hit_moves;        // 0d:46a3  CriticalHitMoves            — 1 byte/entry, 0xFF sentinel
    uint32_t move_effect_priorities;    // 0f:45df  MoveEffectPriorities        — 2 bytes/entry, 0xFF sentinel
    uint32_t ai_status_only_effects;    // 0e:45db  StatusOnlyEffects           — 1 byte/entry, 0xFF sentinel
    uint32_t ai_risky_effects;          // 0e:54ff  RiskyEffects                — 1 byte/entry, 0xFF sentinel
    uint32_t ai_stall_moves;            // 0e:5348  StallMoves                  — 1 byte/entry, 0xFF sentinel
    uint32_t ai_useful_moves;           // 0e:5301  UsefulMoves                 — 1 byte/entry, 0xFF sentinel
    uint32_t ai_residual_moves;         // 0e:5446  ResidualMoves               — 1 byte/entry, 0xFF sentinel
    uint32_t ai_encore_moves;           // 0e:4c85  EncoreMoves                 — 1 byte/entry, 0xFF sentinel
    uint32_t ai_rain_dance_moves;       // 0e:50e7  RainDanceMoves              — 1 byte/entry, 0xFF sentinel
    uint32_t ai_sunny_day_moves;        // 0e:5134  SunnyDayMoves               — 1 byte/entry, 0xFF sentinel
    uint32_t trainer_class_attributes;  // 0e:559c  TrainerClassAttributes      — num_trainer_classes×7 bytes
    uint32_t trainer_class_dvs;         // 09:70d6  TrainerClassDVs             — num_trainer_classes×2 bytes

    // SM83 routine addresses for static-lifting parameter extraction.
    // Used by frontends/crystal/extract/sm83_lifter.cpp.
    // Set to 0 to skip extraction for a routine (struct defaults are used).
    uint32_t sm83_ai_discourage_move     = 0;  // 0e:5503 AIDiscourageMove
    uint32_t sm83_ai_choose_move         = 0;  // 11:40ce AIChooseMove
    uint32_t sm83_give_exp_points        = 0;  // 0f:6e3b GiveExperiencePoints
    uint32_t sm83_damage_variation       = 0;  // 0d:4cfd BattleCommand_DamageVariation
    uint32_t sm83_poke_ball_effect       = 0;  // 03:68a2 PokeBallEffect
    uint32_t sm83_try_to_run_away        = 0;  // 0f:58b3 TryToRunAwayFromBattle
    uint32_t sm83_calc_mon_stat_c        = 0;  // 03:617b CalcMonStatC
    uint32_t sm83_damage_calc            = 0;  // 0d:5612 BattleCommand_DamageCalc
    uint32_t sm83_get_eighth_max_hp      = 0;  // 0f:4c83 GetEighthMaxHP
    uint32_t sm83_get_sixteenth_max_hp   = 0;  // 0f:4c76 GetSixteenthMaxHP
    uint32_t sm83_critical               = 0;  // 0d:4631 BattleCommand_Critical
    // Span sizes (bytes to read from each routine address for pattern matching).
    // Conservative defaults — must cover all expected patterns.
    static constexpr uint32_t SM83_SPAN_AI_DISCOURAGE     = 8;
    static constexpr uint32_t SM83_SPAN_AI_CHOOSE_MOVE    = 30;
    static constexpr uint32_t SM83_SPAN_GIVE_EXP          = 170;
    static constexpr uint32_t SM83_SPAN_DAMAGE_VARIATION  = 40;
    static constexpr uint32_t SM83_SPAN_POKE_BALL         = 250;
    static constexpr uint32_t SM83_SPAN_TRY_TO_RUN        = 200;
    static constexpr uint32_t SM83_SPAN_CALC_MON_STAT_C   = 250;
    static constexpr uint32_t SM83_SPAN_DAMAGE_CALC       = 250;
    static constexpr uint32_t SM83_SPAN_GET_EIGHTH_HP     = 30;
    static constexpr uint32_t SM83_SPAN_GET_SIXTEENTH_HP  = 25;
    static constexpr uint32_t SM83_SPAN_CRITICAL          = 90;

    // Fixed count for WobbleProbabilities (24 in vanilla; may differ in hacks that
    // rewrite the table but keep the same format).
    uint8_t  num_wobble_entries = 24;
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

    // Get profile for a ROM, with explicit fallback to a supplied compatible profile.
    //
    // Policy:
    //   1. If the ROM hash exactly matches a registered profile → use that profile.
    //   2. Otherwise, if 'fallback' is non-null and passes layout validation
    //      against the ROM bytes → accept it with CompatMatchType::LayoutValidated.
    //   3. Otherwise → fail.
    //
    // 'rom_bytes' must be the full ROM contents for layout validation.
    // 'fallback'  may be nullptr (disables the fallback path entirely).
    //
    // This allows ordinary Crystal hacks (modified bytes, same table layout)
    // to compile with the stock Crystal profile as a fallback without
    // treating hash mismatch as "ROM is not Crystal."
    enum class CompatMatchType {
        ExactHash,        // ROM SHA-1 matched a registered profile exactly
        LayoutValidated,  // Hash unknown; fallback profile passed layout checks
    };

    struct CompatResult {
        const ExtractionProfile* profile = nullptr;
        CompatMatchType match_type = CompatMatchType::ExactHash;
        std::string reason;  // Non-empty on failure
    };

    CompatResult get_profile_for_rom(
        std::string_view sha1,
        const uint8_t* rom_bytes,
        size_t rom_size,
        const ExtractionProfile* fallback) const;

    // Validate that a profile's key layout assumptions hold for the given ROM bytes.
    // Returns true (valid) or false with a reason string.
    // Called internally by get_profile_for_rom; also usable by tests.
    static bool validate_profile_layout(
        const ExtractionProfile& profile,
        const uint8_t* rom_bytes,
        size_t rom_size,
        std::string* out_reason = nullptr);

    // ROM-structural count validation.
    // Probes actual ROM data to verify that profile.counts.num_pokemon,
    // num_moves, and std_scripts_count match what the ROM structure indicates.
    // This is NOT merely a bounds check — it reads actual table entries and
    // validates them structurally to detect silent truncation.
    //
    // Returns true if all probed counts agree with the profile.
    // Returns false with out_reason set if any count is wrong.
    // On mismatch, out_actual (if non-null) is set to the ROM-derived count.
    //
    // Species: scans BaseData records forward until a type byte is out of the
    //   valid Crystal type range [0x00-0x1B], then confirms the profile count
    //   matches that boundary (±0).
    // Moves: same approach — scans Moves records until an invalid type byte.
    // StdScripts: scans 3-byte entries until a ptr < 0x4000 (invalid bank-local
    //   pointer sentinel) is found, then confirms profile count matches.
    // TypeMatchups: validates that the configured address actually points at a
    //   valid TypeMatchups table (>= 30 entries, 0xFF sentinel). Searches for
    //   a better candidate and emits a diagnostic if the address is wrong.
    // Moves: also validates that the configured address starts with the Pound
    //   (MoveId 1) signature; searches for a candidate if not.
    //
    // NOTE: Type boundary for species/move scanning uses 0x3F (generous) so
    //   hacks adding new types (Fairy=0x1C, etc.) are counted correctly.
    struct CountMismatch {
        std::string field;    // "num_pokemon", "num_moves", "std_scripts_count",
                              // "num_map_groups", "type_matchups_address", "moves_address"
        uint16_t profile_count;      // value from profile (0 for address-mismatch fields)
        uint16_t rom_derived_count;  // value from ROM probe (0 for address-mismatch fields)
        std::string detail;  // human-readable explanation with suggested fix
    };
    static std::vector<CountMismatch> probe_profile_counts(
        const ExtractionProfile& profile,
        const uint8_t* rom_bytes,
        size_t rom_size);
    
    // Get all supported ROMs (for error messages)
    const std::vector<std::pair<std::string, std::string>>& supported_roms() const;
    
private:
    ProfileRegistry();
    
    std::unordered_map<std::string, RomVersion> hash_to_version_;
    std::unordered_map<RomVersion, ExtractionProfile> profiles_;
    std::vector<std::pair<std::string, std::string>> supported_list_; // sha1 -> name
    
    void register_crystal_v11();
    void register_polished_crystal_3_2_3();
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
