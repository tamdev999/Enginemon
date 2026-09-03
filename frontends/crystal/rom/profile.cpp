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
    // Graphics extended — inline addresses migrated from extractor .cpp files
    // These were previously hardcoded as constexpr in sprites.cpp, fonts.cpp,
    // and tilesets.cpp. They are now profile-driven for hack compatibility.
    //-------------------------------------------------------------------------
    o.overworld_sprites     = flat_offset(0x05, 0x4736);  // 05:4736 OverworldSprites
    o.mon_menu_icons        = flat_offset(0x23, 0x6ac4);  // 23:6ac4 MonMenuIcons (RGBDS hex bank)
    o.icon_pointers         = flat_offset(0x23, 0x6bbf);  // 23:6bbf IconPointers
    o.obj_palettes          = flat_offset(0x02, 0x7469);  // 02:7469 MapObjectPals
    o.tileset_bg_palette    = flat_offset(0x02, 0x7319);  // 02:7319 TilesetBGPalette
    o.font_tiles            = flat_offset(0x3e, 0x4200);  // 3e:4200 Font (128 1bpp tiles)
    o.font_extra_tiles      = flat_offset(0x3e, 0x4000);  // 3e:4000 FontExtra (32 2bpp tiles)

    // Special per-tileset palette overrides (bank 0x12 for all)
    // Source: pokecrystal/gfx/tilesets/*.pal  (pokecrystal11.sym bank 12 addresses)
    {
        constexpr uint8_t SPAL_BANK = 0x12;
        const struct { uint8_t ti; uint16_t addr; } entries[] = {
            { 5,  0x55EE },  // TILESET_HOUSE        → HousePalette
            { 13, 0x567D },  // TILESET_MANSION       → MansionPalette1
            { 21, 0x5501 },  // TILESET_POKECOM_CENTER→ PokeComPalette
            { 22, 0x5550 },  // TILESET_BATTLETOWER_I → BattleTowerInsidePalette
            { 27, 0x563D },  // TILESET_RADIO_TOWER   → RadioTowerPalette
            { 29, 0x559F },  // TILESET_ICE_PATH      → IcePathPalette
        };
        for (const auto& e : entries) {
            auto& slot = o.special_tileset_palettes[o.special_tileset_palette_count++];
            slot.tileset_index = e.ti;
            slot.rom_address   = flat_offset(SPAL_BANK, e.addr);
        }
    }
    
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
    // Native call specs — moved from NativeCallRegistry::initialize()
    // These flat addresses are Crystal v1.1 specific.
    //-------------------------------------------------------------------------
    {
        auto& calls = o.native_calls;
        uint8_t& n  = o.native_call_count;
        using C = NativeCallClass;
        using F = NativeCallFlow;

        auto add = [&](uint32_t addr, const char* sym, const char* sem,
                       C cls, F flow, const char* src, const char* notes = "") {
            if (n < ProfileOffsets::MAX_NATIVE_CALLS) {
                calls[n++] = { addr, sym, sem, cls, flow, src, notes };
            }
        };

        add(0x2f8c,  "Random",                        "random",
            C::PureSemantic, F::Returns,
            "pokecrystal/home/random.asm", "Returns random byte in a");
        add(0xC658,  "HealParty",                     "heal_party",
            C::PureSemantic, F::Returns,
            "pokecrystal/engine/pokemon/party.asm", "Heals all party pokemon");
        add(0xC07a,  "HealPartySpecial",               "heal_party",
            C::PureSemantic, F::Returns,
            "pokecrystal/engine/events/specials.asm", "Heals party special wrapper");
        add(0xC706,  "GetPartyNickname",               "get_party_nickname",
            C::Trivial, F::Returns,
            "pokecrystal/engine/pokemon/party.asm", "Gets nickname of party member");
        add(0xCD78,  "TryStrengthOW",                  "try_strength_overworld",
            C::PureSemantic, F::Returns,
            "pokecrystal/engine/events/overworld.asm",
            "Checks if Strength can be used overworld");
        add(0xCD12,  "SetStrengthFlag",                "set_strength_flag",
            C::PureSemantic, F::Returns,
            "pokecrystal/engine/events/overworld.asm", "Sets the Strength active flag");
        add(0xCF7C,  "HasRockSmash",                   "has_rock_smash",
            C::PureSemantic, F::Returns,
            "pokecrystal/engine/events/overworld.asm",
            "Checks if party has Rock Smash");
        add(0xB8219, "RockMonEncounter",               "rock_mon_encounter",
            C::PureSemantic, F::Returns,
            "pokecrystal/engine/events/treemons.asm",
            "Triggers rock smash wild encounter");
        add(0x9f5cb, "BattleTowerHallway.asm_load_battle_room",
            "load_battle_tower_level_group",
            C::PureSemantic, F::Returns,
            "pokecrystal/maps/BattleTowerHallway.asm",
            "Reads Battle Tower level group selection into wScriptVar");
        add(0x966d0, "EnableEvents",                   "enable_events",
            C::HostCapability, F::Returns,
            "pokecrystal/engine/overworld/events.asm",
            "Re-enables event processing");
        add(0x966ee, "DisableWildEncounters",          "disable_wild_encounters",
            C::HostCapability, F::Returns,
            "pokecrystal/engine/overworld/events.asm", "Disables wild encounters");
        add(0x96706, "EnableWildEncounters",           "enable_wild_encounters",
            C::HostCapability, F::Returns,
            "pokecrystal/engine/overworld/events.asm", "Enables wild encounters");
        add(0x8571,  "HealPartyPredef",                "heal_party",
            C::PureSemantic, F::Returns,
            "pokecrystal/engine/predefs.asm", "Heal party predef wrapper");
    }

    //-------------------------------------------------------------------------
    // RAM address specs — moved from RamAddressRegistry::initialize()
    // Crystal v1.1 wram addresses.
    //-------------------------------------------------------------------------
    {
        auto& rams = o.ram_addresses;
        uint8_t& n  = o.ram_address_count;

        // RamClassification values (0=KnownSemanticState, 1=KnownCapabilitySlot,
        //                           2=ControlFlowPointer, 3=OpaqueRam)
        constexpr uint8_t STATE  = 0;
        constexpr uint8_t CAP    = 1;
        constexpr uint8_t CTLPTR = 2;

        auto add = [&](uint16_t addr, const char* sym, const char* sem,
                       uint8_t cls, const char* src, const char* notes = "") {
            if (n < ProfileOffsets::MAX_RAM_ADDRESSES) {
                rams[n++] = { addr, sym, sem, cls, src, notes };
            }
        };

        add(0xc2dd, "wScriptVar",              "script_var",
            STATE, "pokecrystal/ram/wram.asm",
            "Primary script variable for return values");
        add(0xd437, "wScriptMode",             "script_mode",
            CAP,   "pokecrystal/ram/wram.asm",
            "Current script processing mode");
        add(0xd109, "wCurPartyMon",            "current_party_mon",
            STATE, "pokecrystal/ram/wram.asm",
            "Index of current party pokemon");
        add(0xd03f, "wCurFruit",               "current_fruit",
            STATE, "pokecrystal/ram/wram.asm",
            "ID of current fruit tree item");
        add(0xd03e, "wCurFruitTree",           "current_fruit_tree",
            STATE, "pokecrystal/ram/wram.asm",
            "ID of current fruit tree");
        add(0xc2de, "wPlayerNextMovement",     "player_next_movement",
            STATE, "pokecrystal/ram/wram.asm",
            "Player's next movement byte");
        add(0xc2df, "wPlayerMovement",         "player_movement",
            STATE, "pokecrystal/ram/wram.asm",
            "Player's current movement byte");
        add(0xc2e2, "wMovementObject",         "movement_object",
            STATE, "pokecrystal/ram/wram.asm",
            "Object ID for movement commands");
        add(0xc2e3, "wMovementDataBank",       "movement_data_bank",
            CAP,   "pokecrystal/ram/wram.asm",
            "Bank of movement data");
        add(0xc2e4, "wMovementDataAddress",    "movement_data_address",
            CTLPTR,"pokecrystal/ram/wram.asm",
            "Pointer to movement data");
        add(0xd962, "wMooMooBerries",          "moo_moo_berries",
            STATE, "pokecrystal11.sym",
            "MooMoo Farm berry feeding count");
        add(0xd963, "wUndergroundSwitchPositions","underground_switch_positions",
            STATE, "pokecrystal11.sym",
            "Goldenrod Underground switch states");
        add(0xd964, "wFarfetchdPosition",      "farfetchd_position",
            STATE, "pokecrystal11.sym",
            "Farfetch'd herding mini-game position");
        add(0xcf51, "wOtherPlayerLinkMode",    "other_player_link_mode",
            CAP,   "pokecrystal11.sym",
            "Link mode state (multi-use UNION address)");
        add(0xd1ef, "wStrengthSpecies",        "strength_species",
            STATE, "pokecrystal11.sym",
            "Species that used Strength (field move context)");
        add(0xd22e, "wTempWildMonSpecies",     "temp_wild_mon_species",
            STATE, "pokecrystal11.sym",
            "Pending wild encounter species (field move context)");
        add(0xcf64, "wNrOfBeatenBattleTowerTrainers","battle_tower_beaten_trainers",
            STATE, "pokecrystal11.sym",
            "Number of beaten Battle Tower trainers in current streak");
    }

    //-------------------------------------------------------------------------
    // Battle rule table addresses — moved from hardcoded tables in
    // calculator.cpp / trainer_ai.cpp.  These are now profile-driven so ROM
    // hacks that relocate any of these tables remain compatible.
    // All addresses from pokecrystal.sym (Crystal v1.1).
    //-------------------------------------------------------------------------
    o.stat_level_multipliers     = flat_offset(0x0f, 0x6d2b); // 0f:6d2b StatLevelMultipliers_Applied
    o.accuracy_level_multipliers = flat_offset(0x0d, 0x4eb2); // 0d:4eb2 AccuracyLevelMultipliers
    o.critical_hit_chances       = flat_offset(0x0d, 0x46ab); // 0d:46ab CriticalHitChances
    o.wobble_probabilities       = flat_offset(0x03, 0x79ba); // 03:79ba WobbleProbabilities
    o.weather_type_modifiers     = flat_offset(0x3e, 0x7e13); // 3e:7e13 WeatherTypeModifiers
    o.weather_move_modifiers     = flat_offset(0x3e, 0x7e20); // 3e:7e20 WeatherMoveModifiers
    o.critical_hit_moves         = flat_offset(0x0d, 0x46a3); // 0d:46a3 CriticalHitMoves
    o.move_effect_priorities     = flat_offset(0x0f, 0x45df); // 0f:45df MoveEffectPriorities
    o.ai_status_only_effects     = flat_offset(0x0e, 0x45db); // 0e:45db StatusOnlyEffects
    o.ai_risky_effects           = flat_offset(0x0e, 0x54ff); // 0e:54ff RiskyEffects
    o.ai_stall_moves             = flat_offset(0x0e, 0x5348); // 0e:5348 StallMoves
    o.ai_useful_moves            = flat_offset(0x0e, 0x5301); // 0e:5301 UsefulMoves
    o.ai_residual_moves          = flat_offset(0x0e, 0x5446); // 0e:5446 ResidualMoves
    o.ai_encore_moves            = flat_offset(0x0e, 0x4c85); // 0e:4c85 EncoreMoves
    o.ai_rain_dance_moves        = flat_offset(0x0e, 0x50e7); // 0e:50e7 RainDanceMoves
    o.ai_sunny_day_moves         = flat_offset(0x0e, 0x5134); // 0e:5134 SunnyDayMoves
    o.trainer_class_attributes   = flat_offset(0x0e, 0x559c); // 0e:559c TrainerClassAttributes
    o.trainer_class_dvs          = flat_offset(0x09, 0x70d6); // 09:70d6 TrainerClassDVs
    o.num_wobble_entries         = 24; // 24 entries in Crystal v1.1 WobbleProbabilities

    // SM83 routine addresses for static parameter lifting.
    // Verified against Crystal v1.1 (SHA-1: f2f52230b536214ef7c9924f483392993e226cfb).
    o.sm83_ai_discourage_move    = flat_offset(0x0e, 0x5503); // 0e:5503 AIDiscourageMove
    o.sm83_ai_choose_move        = flat_offset(0x11, 0x40ce); // 11:40ce AIChooseMove
    o.sm83_give_exp_points       = flat_offset(0x0f, 0x6e3b); // 0f:6e3b GiveExperiencePoints
    o.sm83_damage_variation      = flat_offset(0x0d, 0x4cfd); // 0d:4cfd BattleCommand_DamageVariation
    o.sm83_poke_ball_effect      = flat_offset(0x03, 0x68a2); // 03:68a2 PokeBallEffect
    o.sm83_try_to_run_away       = flat_offset(0x0f, 0x58b3); // 0f:58b3 TryToRunAwayFromBattle
    o.sm83_calc_mon_stat_c       = flat_offset(0x03, 0x617b); // 03:617b CalcMonStatC
    o.sm83_damage_calc           = flat_offset(0x0d, 0x5612); // 0d:5612 BattleCommand_DamageCalc
    o.sm83_get_eighth_max_hp     = flat_offset(0x0f, 0x4c83); // 0f:4c83 GetEighthMaxHP
    o.sm83_get_sixteenth_max_hp  = flat_offset(0x0f, 0x4c76); // 0f:4c76 GetSixteenthMaxHP
    o.sm83_critical              = flat_offset(0x0d, 0x4631); // 0d:4631 BattleCommand_Critical

    //-------------------------------------------------------------------------
    // Register
    //-------------------------------------------------------------------------
    hash_to_version_[profile.sha1] = profile.version;
    profiles_[profile.version] = profile;
    supported_list_.emplace_back(profile.sha1, profile.version_string);
}  // end register_crystal_v11()

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

// =============================================================================
// COUNT VALIDATION
// ROM-structural probing to detect profile count mismatches.
// =============================================================================

std::vector<ProfileRegistry::CountMismatch> ProfileRegistry::probe_profile_counts(
    const ExtractionProfile& profile,
    const uint8_t* rom,
    size_t rom_size)
{
    std::vector<CountMismatch> mismatches;
    const auto& o   = profile.offsets;
    const auto& c   = profile.counts;
    const auto& fmt = profile.format;

    auto read_byte = [&](uint32_t flat) -> uint8_t {
        return (flat < rom_size) ? rom[flat] : 0xFF;
    };

    // ── Probe 1: species count from BaseData type-byte boundary ──────────────
    // Scans BaseData records until a type byte is clearly outside the valid
    // Crystal/hack type range. Uses 0x3F as the generous upper bound —
    // the same value used by TypeMatchups sanity-checking — so hacks that
    // add new types (e.g. Fairy = 0x1C) are still counted correctly.
    // Values >= 0x40 reliably indicate non-species data (text, code, etc.)
    // At the end of the table in all known Crystal-family ROMs.
    if (o.base_data != 0 && fmt.pokemon.base_data_size > 0) {
        // MAX_VALID_TYPE must be generous enough for expansion hacks (Fairy, etc.)
        // 0x3F = 63 allows up to 64 types, well beyond any current hack.
        constexpr uint8_t MAX_VALID_TYPE = 0x3F;
        uint32_t structural_count = 0;
        for (uint32_t i = 0; i < 512u; ++i) {
            uint32_t entry_addr = o.base_data + i * fmt.pokemon.base_data_size;
            if (entry_addr + fmt.pokemon.base_data_size > rom_size) break;
            uint8_t t1 = read_byte(entry_addr + fmt.pokemon.type1_offset);
            uint8_t t2 = read_byte(entry_addr + fmt.pokemon.type2_offset);
            // Also require non-zero HP to exclude padding/all-zero entries
            uint8_t hp = read_byte(entry_addr + fmt.pokemon.hp_offset);
            if (t1 > MAX_VALID_TYPE || t2 > MAX_VALID_TYPE || hp == 0) break;
            ++structural_count;
        }
        if (structural_count != c.num_pokemon) {
            mismatches.push_back({
                "num_pokemon",
                c.num_pokemon,
                static_cast<uint16_t>(structural_count),
                std::format("BaseData type-byte boundary at record {} (profile says {}); "
                            "hack may have {} species — update profile.counts.num_pokemon",
                            structural_count + 1, c.num_pokemon, structural_count)
            });
        }
    }

    // ── Probe 2: move count from Moves table type-byte boundary ──────────────
    // Uses the same generous type bound (0x3F) as Probe 1 so hacks adding
    // new types (Fairy=0x1C, etc.) are counted correctly rather than truncated.
    if (o.moves != 0 && fmt.move.move_data_size > 0) {
        constexpr uint8_t MAX_VALID_TYPE = 0x3F;  // generous; same as TypeMatchups guard
        uint32_t structural_count = 0;
        for (uint32_t i = 0; i < 512u; ++i) {
            uint32_t entry_addr = o.moves + i * fmt.move.move_data_size;
            if (entry_addr + fmt.move.move_data_size > rom_size) break;
            uint8_t type_byte = read_byte(entry_addr + fmt.move.type_offset);
            if (type_byte > MAX_VALID_TYPE) break;
            ++structural_count;
        }
        if (structural_count != c.num_moves) {
            mismatches.push_back({
                "num_moves",
                c.num_moves,
                static_cast<uint16_t>(structural_count),
                std::format("Moves table type-byte boundary at record {} (profile says {}); "
                            "update profile.counts.num_moves",
                            structural_count + 1, c.num_moves, structural_count)
            });
        }
    }

    // ── Probe 3b: Map group count from ptr-table sentinel ────────────────────
    // Count consecutive valid bank-local 2-byte ptrs at map_group_pointers.
    // A ptr in [0x4000, 0x7FFF] is valid; anything else terminates the table.
    if (o.map_group_pointers != 0) {
        uint16_t structural_count = 0;
        for (uint32_t i = 0; i < 256u; ++i) {
            uint32_t entry_addr = o.map_group_pointers + i * 2u;
            if (entry_addr + 2u > rom_size) break;
            uint16_t ptr = static_cast<uint16_t>(read_byte(entry_addr))
                         | (static_cast<uint16_t>(read_byte(entry_addr + 1)) << 8);
            if (ptr < 0x4000u || ptr > 0x7FFFu) break;
            ++structural_count;
        }
        if (structural_count != c.num_map_groups) {
            mismatches.push_back({
                "num_map_groups",
                c.num_map_groups,
                structural_count,
                std::format("MapGroupPointers sentinel at entry {} (profile says {}); "
                            "update profile.counts.num_map_groups",
                            structural_count, c.num_map_groups)
            });
        }
    }

    // ── Probe 3: StdScripts count from entry-validity sentinel ───────────────
    // A valid StdScript 3-byte entry has: bank in [0x00, 0x7F], ptr in [0x4000, 0x7FFF].
    // The first invalid entry is the end of the table.
    if (o.std_scripts != 0) {
        uint16_t structural_count = 0;
        for (uint32_t i = 0; i < 256u; ++i) {
            uint32_t entry_addr = o.std_scripts + i * 3u;
            if (entry_addr + 3u > rom_size) break;
            uint8_t  bank = read_byte(entry_addr);
            uint16_t ptr  = static_cast<uint16_t>(read_byte(entry_addr + 1))
                          | (static_cast<uint16_t>(read_byte(entry_addr + 2)) << 8);
            // Valid: bank < 0x80 (ROM bank) and ptr in switchable-bank window [0x4000, 0x7FFF]
            if (bank >= 0x80 || ptr < 0x4000u || ptr > 0x7FFFu) break;
            ++structural_count;
        }
        if (structural_count != o.std_scripts_count) {
            mismatches.push_back({
                "std_scripts_count",
                o.std_scripts_count,
                structural_count,
                std::format("StdScripts sentinel at entry {} (profile says {}); "
                            "update profile.offsets.std_scripts_count",
                            structural_count, o.std_scripts_count)
            });
        }
    }

    // ── Probe 4: Structural address candidates — search when profile address
    //    is wrong. These probes scan the ROM for the table using its structural
    //    signature and emit a diagnostic if the found address differs from the
    //    profile's configured address.  They do NOT auto-update the profile —
    //    that requires a new per-hack profile registration.
    // ─────────────────────────────────────────────────────────────────────────

    // ── Probe 4a: TypeMatchups table — pure sentinel + {0,5,20} multiplier ──
    // The TypeMatchups table is identified by:
    //   • 3-byte entries: {atk_type, def_type, multiplier}
    //   • multiplier ∈ {0, 5, 20} exactly
    //   • separated by optional 0xFE bytes (Gen2 boundary marker)
    //   • terminated by 0xFF
    // We require at least 30 valid entries before the 0xFF sentinel to
    // distinguish it from random data that happens to have valid multipliers.
    if (o.type_matchups != 0) {
        // Scan forward from configured address, counting valid entries.
        // The existing extractor already validates this; here we check whether
        // the configured address actually reaches enough entries.
        constexpr uint32_t MIN_EXPECTED_ENTRIES = 30u;
        uint32_t valid_entries = 0;
        uint32_t ptr = o.type_matchups;
        bool found_sentinel = false;
        for (uint32_t i = 0; i < 2048u && ptr < rom_size; ++i) {
            uint8_t b = read_byte(ptr);
            if (b == 0xFF) { found_sentinel = true; break; }
            if (b == 0xFE) { ptr += 1; continue; }  // Gen2 separator
            if (ptr + 3u > rom_size) break;
            uint8_t mult = read_byte(ptr + 2);
            if (mult == 0 || mult == 5 || mult == 20) {
                ++valid_entries;
                ptr += 3;
            } else {
                break;  // invalid multiplier: not a type matchup table
            }
        }
        if (!found_sentinel || valid_entries < MIN_EXPECTED_ENTRIES) {
            // Configured address does not look like a valid TypeMatchups table.
            // Search for a better candidate with generous type-ID limit.
            constexpr uint8_t MAX_TYPE_ID_SEARCH = 0x3F;
            uint32_t best_candidate = 0;
            uint32_t best_count = 0;
            for (uint32_t search = 0;
                 search + 12 <= rom_size && best_count < MIN_EXPECTED_ENTRIES;
                 search += 3)
            {
                if (read_byte(search) == 0xFE || read_byte(search) == 0xFF) {
                    search -= 2; continue;  // align on entry boundary
                }
                uint32_t p2 = search;
                uint32_t cnt = 0;
                bool ok = false;
                for (uint32_t j = 0; j < 2048u && p2 < rom_size; ++j) {
                    uint8_t a2 = read_byte(p2);
                    if (a2 == 0xFF) { ok = true; break; }
                    if (a2 == 0xFE) { p2 += 1; continue; }
                    if (p2 + 3u > rom_size) break;
                    uint8_t d2 = read_byte(p2 + 1);
                    uint8_t m2 = read_byte(p2 + 2);
                    if (a2 > MAX_TYPE_ID_SEARCH || d2 > MAX_TYPE_ID_SEARCH) break;
                    if (m2 != 0 && m2 != 5 && m2 != 20) break;
                    ++cnt; p2 += 3;
                }
                if (ok && cnt > best_count) { best_count = cnt; best_candidate = search; }
            }
            if (best_count >= MIN_EXPECTED_ENTRIES && best_candidate != o.type_matchups) {
                mismatches.push_back({
                    "type_matchups_address",
                    0,  // not a count field; reuse profile_count=0 as sentinel
                    0,  // structural_count unused here
                    std::format("profile.offsets.type_matchups=0x{:05X} does not point to a "
                                "valid TypeMatchups table ({} valid entries, sentinel={}).\n"
                                "  Structural search found candidate at 0x{:05X} "
                                "({} entries).\n"
                                "  Update profile.offsets.type_matchups=0x{:05X} for this ROM.",
                                o.type_matchups, valid_entries, found_sentinel,
                                best_candidate, best_count, best_candidate)
                });
            } else if (!found_sentinel || valid_entries < MIN_EXPECTED_ENTRIES) {
                mismatches.push_back({
                    "type_matchups_address",
                    0, 0,
                    std::format("profile.offsets.type_matchups=0x{:05X} does not look like a "
                                "valid TypeMatchups table ({} valid entries, sentinel={}).\n"
                                "  No structural candidate found in ROM scan.\n"
                                "  TypeMatchups table may have been relocated or reformatted.",
                                o.type_matchups, valid_entries, found_sentinel)
                });
            }
        }
    }

    // ── Probe 4b: Moves table — Pound (MoveId 1) signature ───────────────────
    // Move record 0 (MoveId 1 = Pound) has a known signature independent of
    // animation byte: effect=0, power=40(0x28), type=0(Normal), acc=255(0xFF),
    // pp=35(0x23), effect_chance=0.  This is unique enough to locate the table.
    if (o.moves != 0 && fmt.move.move_data_size > 0) {
        // Quick sanity check: does the profile address look like it starts with Pound?
        // Pound bytes at: [effect_off]=0, [power_off]=0x28, [type_off]=0x00,
        //                 [accuracy_off]=0xFF, [pp_off]=0x23, [ec_off]=0x00
        auto check_pound_at = [&](uint32_t base) -> bool {
            if (base + fmt.move.move_data_size > rom_size) return false;
            return read_byte(base + fmt.move.effect_offset)        == 0x00 &&
                   read_byte(base + fmt.move.power_offset)         == 0x28 &&
                   read_byte(base + fmt.move.type_offset)          == 0x00 &&
                   read_byte(base + fmt.move.accuracy_offset)      == 0xFF &&
                   read_byte(base + fmt.move.pp_offset)            == 0x23 &&
                   read_byte(base + fmt.move.effect_chance_offset) == 0x00;
        };
        if (!check_pound_at(o.moves)) {
            // Profile address doesn't start with Pound — search for it.
            // Pound: effect=0x00, power=0x28, type=0x00, acc=0xFF, pp=0x23, ec=0x00
            uint32_t found_at = 0;
            // Only search at 7-byte and 8-byte strides (known move record sizes)
            for (uint32_t stride : {7u, 8u}) {
                if (stride != fmt.move.move_data_size && found_at == 0) {
                    // Only scan with the configured stride unless nothing found
                }
                if (fmt.move.move_data_size == stride || found_at == 0) {
                    for (uint32_t i = 0; i + stride * 3 <= rom_size; ++i) {
                        // Fast pre-check: power=0x28, type=0x00, acc=0xFF at their offsets
                        if (read_byte(i + fmt.move.power_offset)    != 0x28) continue;
                        if (read_byte(i + fmt.move.type_offset)     != 0x00) continue;
                        if (read_byte(i + fmt.move.accuracy_offset) != 0xFF) continue;
                        if (check_pound_at(i)) { found_at = i; break; }
                    }
                }
                if (found_at != 0) break;
            }
            if (found_at != 0 && found_at != o.moves) {
                mismatches.push_back({
                    "moves_address",
                    0, 0,
                    std::format("profile.offsets.moves=0x{:05X} does not start with "
                                "Pound (MoveId 1) signature.\n"
                                "  Structural search found Pound at 0x{:05X}.\n"
                                "  Update profile.offsets.moves=0x{:05X} for this ROM.",
                                o.moves, found_at, found_at)
                });
            } else if (found_at == 0) {
                mismatches.push_back({
                    "moves_address",
                    0, 0,
                    std::format("profile.offsets.moves=0x{:05X} does not start with "
                                "Pound signature, and no candidate found in ROM scan.\n"
                                "  Moves table may be relocated or use different record format.",
                                o.moves)
                });
            }
        }
    }

    return mismatches;
}

// =============================================================================
// LAYOUT VALIDATION
// Checks that a profile's key structural assumptions hold for a ROM.
// This is a lightweight set of spot-checks — not a full extraction attempt.
// The intent is to catch "completely wrong profile" situations (e.g., Gold ROM
// vs Crystal profile) without running the full compiler.
// =============================================================================

bool ProfileRegistry::validate_profile_layout(
    const ExtractionProfile& profile,
    const uint8_t* rom_bytes,
    size_t rom_size,
    std::string* out_reason)
{
    auto fail = [&](const char* msg) -> bool {
        if (out_reason) *out_reason = msg;
        return false;
    };

    // Minimum ROM size sanity check (2 MB for all Crystal-family ROMs)
    if (rom_size < 0x200000) {
        return fail("ROM too small (< 2 MB); not a Crystal-family ROM");
    }

    const auto& o = profile.offsets;
    const auto& c = profile.counts;
    const auto& fmt = profile.format;

    // Helper: read one byte from a flat offset
    auto read_byte = [&](uint32_t flat) -> uint8_t {
        if (flat >= rom_size) return 0xFF;
        return rom_bytes[flat];
    };

    auto in_range = [&](uint32_t flat, uint32_t size) -> bool {
        return flat + size <= rom_size;
    };

    // ── Check 1: BaseData table is reachable ──────────────────────────────────
    // The BaseData table must fit: base_data + num_pokemon * base_data_size bytes.
    uint32_t base_data_end = o.base_data +
        static_cast<uint32_t>(c.num_pokemon) * fmt.pokemon.base_data_size;
    if (!in_range(o.base_data, base_data_end - o.base_data)) {
        return fail("profile.offsets.base_data + num_pokemon*base_data_size exceeds ROM");
    }

    // ── Check 2: First BaseData record dex number is non-zero ────────────────
    // Record 0 is an unused slot; record 1 (entry at index 0 = base_data + 0*32)
    // must have a non-zero dex number at offset 0.
    // For Crystal/Gold/Silver the first species (Bulbasaur=1) has dex num = 1.
    uint8_t first_dex = read_byte(o.base_data + fmt.pokemon.dex_num_offset);
    if (first_dex == 0 || first_dex > c.num_pokemon) {
        return fail("first BaseData record has implausible dex_num; profile/ROM mismatch");
    }

    // ── Check 3: MapGroupPointers table is reachable ─────────────────────────
    if (!in_range(o.map_group_pointers, 2)) {
        return fail("profile.offsets.map_group_pointers exceeds ROM");
    }

    // ── Check 4: StdScripts table is reachable ───────────────────────────────
    if (!in_range(o.std_scripts, o.std_scripts_count * 3u)) {
        return fail("profile.offsets.std_scripts + count*3 exceeds ROM");
    }

    // ── Check 5: MonMenuIcons table is reachable ─────────────────────────────
    if (o.mon_menu_icons != 0 && !in_range(o.mon_menu_icons, c.num_pokemon)) {
        return fail("profile.offsets.mon_menu_icons + num_pokemon exceeds ROM");
    }

    // ── Check 6: Tilesets table is reachable ─────────────────────────────────
    // The tilesets table must hold at least (num_tilesets + 1) entries at
    // tileset_size bytes each (tilesets are 1-indexed, entry 0 is unused).
    // This catches ROMs that relocate the tileset table to a different bank.
    {
        uint32_t tilesets_end = o.tilesets +
            static_cast<uint32_t>(c.num_tilesets + 1u) *
            static_cast<uint32_t>(fmt.tileset.tileset_size);
        if (!in_range(o.tilesets, tilesets_end - o.tilesets)) {
            return fail("profile.offsets.tilesets table exceeds ROM bounds; "
                        "tileset table may have been relocated");
        }
    }

    // ── Check 7: First map group pointer is a valid ROM-bank address ──────────
    // Every map group pointer must be in the switchable ROM-bank window
    // (0x4000..0x7FFF).  A value outside that window means the profile's
    // map_group_pointers offset points at something other than the map table.
    if (in_range(o.map_group_pointers, 2)) {
        uint16_t first_grp_ptr = static_cast<uint16_t>(
            rom_bytes[o.map_group_pointers] |
            (static_cast<uint16_t>(rom_bytes[o.map_group_pointers + 1]) << 8));
        if (first_grp_ptr < 0x4000u || first_grp_ptr > 0x7FFFu) {
            return fail("first map group pointer is outside ROM-bank range 0x4000-0x7FFF; "
                        "map group table may have been relocated");
        }
    }

    return true;  // All checks passed
}

// =============================================================================
// HASH POLICY — get_profile_for_rom
// =============================================================================

ProfileRegistry::CompatResult ProfileRegistry::get_profile_for_rom(
    std::string_view sha1,
    const uint8_t* rom_bytes,
    size_t rom_size,
    const ExtractionProfile* fallback) const
{
    // ── Path 1: Exact SHA-1 match ─────────────────────────────────────────────
    if (const ExtractionProfile* exact = get_profile_by_hash(sha1)) {
        return { exact, CompatMatchType::ExactHash, "" };
    }

    // ── Path 2: Fallback profile + layout validation ──────────────────────────
    if (!fallback) {
        std::string reason = "ROM SHA-1 not recognized and no fallback profile supplied. "
                             "Supported: ";
        for (const auto& [hash, name] : supported_roms()) {
            reason += name + " ";
        }
        return { nullptr, CompatMatchType::ExactHash, reason };
    }

    std::string layout_reason;
    if (!validate_profile_layout(*fallback, rom_bytes, rom_size, &layout_reason)) {
        std::string reason = "ROM SHA-1 not recognized and supplied profile failed layout "
                             "validation: " + layout_reason;
        return { nullptr, CompatMatchType::LayoutValidated, reason };
    }

    return { fallback, CompatMatchType::LayoutValidated, "" };
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
