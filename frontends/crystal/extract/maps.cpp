// crystal/extract/maps.cpp
// Generic map extraction from Crystal ROM
// 
// All maps go through the same extraction path - no special cases.
// Crystal group/index are frontend-only, converted to semantic IDs.
// ROM addresses are debug-only metadata, stripped in release.
// All ROM reads are bounds-checked.

#include "crystal/extract/map_extractor.hpp"
#include <format>
#include <algorithm>
#include <cctype>

namespace crystal {

//=============================================================================
// CONSTRUCTION
//=============================================================================

MapExtractor::MapExtractor(const RomData& rom, const ExtractionProfile& profile)
    : rom_(rom), profile_(profile) {}

//=============================================================================
// BOUNDS-CHECKED ROM ACCESS
//=============================================================================

bool MapExtractor::read_map_header(uint32_t addr, std::vector<uint8_t>& out) const {
    const size_t size = profile_.format.map.header_size;
    if (addr + size > rom_.size()) {
        stats_.bounds_check_failures++;
        return false;
    }
    auto span = rom_.read_bytes(addr, size);
    out.assign(span.begin(), span.end());
    return true;
}

bool MapExtractor::read_block_data(uint8_t bank, uint16_t addr, uint8_t w, uint8_t h,
                                   std::vector<uint8_t>& out) const {
    uint32_t flat = rom_.bank_to_flat(bank, addr);
    size_t size = static_cast<size_t>(w) * h;
    
    if (flat + size > rom_.size()) {
        stats_.bounds_check_failures++;
        return false;
    }
    
    auto span = rom_.read_bytes(flat, size);
    out.assign(span.begin(), span.end());
    return true;
}

//=============================================================================
// SEMANTIC ID GENERATION
// Crystal group/index -> stable lowercase semantic IDs
//=============================================================================

std::string MapExtractor::make_map_id(uint8_t group, uint8_t index) const {
    // Map known Crystal maps to semantic IDs
    // IDs are lowercase with underscores, stable across versions
    // From pokecrystal data/maps/maps.asm MapGroup_* definitions
    
    // Group 24: New Bark area (MapGroup_NewBark from maps.asm)
    if (group == 24) {
        static const char* ids[] = {
            nullptr,                    // index 0: unused
            "route_26",                 // index 1
            "route_27",                 // index 2
            "route_29",                 // index 3
            "new_bark_town",            // index 4 (10x9 blocks)
            "elms_lab",                 // index 5
            "players_house_1f",         // index 6
            "players_house_2f",         // index 7
            "players_neighbors_house",  // index 8
            "elms_house",               // index 9
            "route_26_heal_house",      // index 10
            "day_of_week_siblings_house", // index 11
            "route_27_sandstorm_house", // index 12
            "route_29_route_46_gate"    // index 13
        };
        if (index > 0 && index <= 13) return ids[index];
    }
    
    // Group 26: Cherrygrove area (MapGroup_Cherrygrove from maps.asm)
    if (group == 26) {
        static const char* ids[] = {
            nullptr,                        // index 0: unused
            "route_30",                     // index 1
            "route_31",                     // index 2
            "cherrygrove_city",             // index 3
            "cherrygrove_mart",             // index 4
            "cherrygrove_pokecenter_1f",    // index 5
            "cherrygrove_gym_speech_house", // index 6
            "guide_gents_house",            // index 7
            "cherrygrove_evolution_speech_house", // index 8
            "route_30_berry_house",         // index 9
            "mr_pokemons_house",            // index 10
            "route_31_violet_gate"          // index 11
        };
        if (index > 0 && index <= 11) return ids[index];
    }
    
    // Fallback: generate stable ID from group/index
    // This ensures we can still extract unknown maps
    return std::format("map_g{:02d}_i{:02d}", group, index);
}

std::string MapExtractor::make_tileset_id(uint8_t tileset_index) const {
    // Known tilesets - 1-indexed! (from constants/tileset_constants.asm)
    // const_def 1 means first constant is 1, not 0
    static const char* tilesets[] = {
        nullptr,                // 0: unused (indices are 1-based)
        "johto_outdoor",        // 1: TILESET_JOHTO
        "johto_modern",         // 2: TILESET_JOHTO_MODERN
        "kanto_outdoor",        // 3: TILESET_KANTO
        "battle_tower_outside", // 4: TILESET_BATTLE_TOWER_OUTSIDE
        "house",                // 5: TILESET_HOUSE
        "players_house",        // 6: TILESET_PLAYERS_HOUSE
        "pokecenter",           // 7: TILESET_POKECENTER
        "gate",                 // 8: TILESET_GATE
        "port",                 // 9: TILESET_PORT
        "lab",                  // 10 (0x0A): TILESET_LAB
        "facility",             // 11 (0x0B): TILESET_FACILITY
        "mart",                 // 12 (0x0C): TILESET_MART
        "mansion",              // 13 (0x0D): TILESET_MANSION
        "game_corner",          // 14 (0x0E): TILESET_GAME_CORNER
        "elite_four_room",      // 15 (0x0F): TILESET_ELITE_FOUR_ROOM
        "traditional_house",    // 16 (0x10): TILESET_TRADITIONAL_HOUSE
        "train_station",        // 17 (0x11): TILESET_TRAIN_STATION
        "champions_room",       // 18 (0x12): TILESET_CHAMPIONS_ROOM
        "lighthouse",           // 19 (0x13): TILESET_LIGHTHOUSE
        "players_room",         // 20 (0x14): TILESET_PLAYERS_ROOM
        "pokecom_center",       // 21 (0x15): TILESET_POKECOM_CENTER
        "battle_tower_inside",  // 22 (0x16): TILESET_BATTLE_TOWER_INSIDE
        "tower",                // 23 (0x17): TILESET_TOWER
        "cave",                 // 24 (0x18): TILESET_CAVE
        "park",                 // 25 (0x19): TILESET_PARK
        "ruins_of_alph",        // 26 (0x1A): TILESET_RUINS_OF_ALPH
        "radio_tower",          // 27 (0x1B): TILESET_RADIO_TOWER
        "underground",          // 28 (0x1C): TILESET_UNDERGROUND
        "ice_path",             // 29 (0x1D): TILESET_ICE_PATH
        "dark_cave",            // 30 (0x1E): TILESET_DARK_CAVE
        "forest",               // 31 (0x1F): TILESET_FOREST
        "beta_word_room",       // 32 (0x20): TILESET_BETA_WORD_ROOM
        "ho_oh_word_room",      // 33 (0x21): TILESET_HO_OH_WORD_ROOM
        "kabuto_word_room",     // 34 (0x22): TILESET_KABUTO_WORD_ROOM
        "omanyte_word_room",    // 35 (0x23): TILESET_OMANYTE_WORD_ROOM
        "aerodactyl_word_room", // 36 (0x24): TILESET_AERODACTYL_WORD_ROOM
    };
    
    constexpr size_t num_tilesets = sizeof(tilesets) / sizeof(tilesets[0]);
    if (tileset_index > 0 && tileset_index < num_tilesets && tilesets[tileset_index]) {
        return tilesets[tileset_index];
    }
    return std::format("tileset_{:02d}", tileset_index);
}

std::string MapExtractor::make_sprite_id(uint8_t sprite_index) const {
    // Sprite ID names from pokecrystal sprite_constants.asm
    // These must match the sprite extractor's SPRITE_NAMES table
    static const char* sprites[] = {
        nullptr,            // 0 = SPRITE_NONE
        "chris",            // 1
        "chris_bike",       // 2
        "gameboy_kid",      // 3
        "rival",            // 4 (silver)
        "oak",              // 5
        "red",              // 6
        "blue",             // 7
        "bill",             // 8
        "elder",            // 9
        "janine",           // 10
        "kurt",             // 11
        "mom",              // 12
        "blaine",           // 13
        "reds_mom",         // 14
        "daisy",            // 15
        "elm",              // 16
        "will",             // 17
        "falkner",          // 18
        "whitney",          // 19
        "bugsy",            // 20
        "morty",            // 21
        "chuck",            // 22
        "jasmine",          // 23
        "pryce",            // 24
        "clair",            // 25
        "brock",            // 26
        "karen",            // 27
        "bruno",            // 28
        "misty",            // 29
        "lance",            // 30
        "surge",            // 31
        "erika",            // 32
        "koga",             // 33
        "sabrina",          // 34
        "cooltrainer_m",    // 35
        "cooltrainer_f",    // 36
        "bug_catcher",      // 37
        "twin",             // 38
        "youngster",        // 39
        "lass",             // 40
        "teacher",          // 41 (0x29)
        "beauty",           // 42
        "super_nerd",       // 43
        "rocker",           // 44
        "pokefan_m",        // 45
        "pokefan_f",        // 46
        "gramps",           // 47
        "granny",           // 48
        "swimmer_guy",      // 49
        "swimmer_girl",     // 50
        "big_snorlax",      // 51
        "surfing_pikachu",  // 52
        "rocket",           // 53
        "rocket_girl",      // 54
        "nurse",            // 55
        "link_receptionist", // 56
        "clerk",            // 57
        "fisher",           // 58 (0x3a)
    };
    constexpr size_t NUM_SPRITES = sizeof(sprites) / sizeof(sprites[0]);
    
    if (sprite_index < NUM_SPRITES && sprites[sprite_index]) {
        return sprites[sprite_index];
    }
    return std::format("sprite_{:02d}", sprite_index);
}

std::string MapExtractor::make_music_id(uint8_t music_index) const {
    // Known music tracks from constants/music_constants.asm
    static const char* music[] = {
        "none",                     // 0
        "title",                    // 1
        "route_29",                 // 2 (MUSIC_ROUTE_29)
        "new_bark_town",            // 3 (MUSIC_NEW_BARK_TOWN) - same as MUSIC_PALLET_TOWN
        "cherrygrove_city",         // 4 (MUSIC_CHERRYGROVE_CITY)
        "violet_city",              // 5
        "azalea_town",              // 6
        "goldenrod_city",           // 7
        "ecruteak_city",            // 8
        "pokemon_center",           // 9
        "gym",                      // 10
        "route_30",                 // 11
        "route_36",                 // 12
        "route_37",                 // 13
    };
    
    if (music_index < 14) {
        return music[music_index];
    }
    return std::format("music_{:02x}", music_index);
}

std::string MapExtractor::make_landmark_id(uint8_t landmark_index) const {
    // Known landmarks from constants/landmark_constants.asm
    static const char* landmarks[] = {
        "special",              // 0: LANDMARK_SPECIAL
        "new_bark_town",        // 1: LANDMARK_NEW_BARK_TOWN
        "route_29",             // 2: LANDMARK_ROUTE_29
        "cherrygrove_city",     // 3: LANDMARK_CHERRYGROVE_CITY
        "route_30",             // 4: LANDMARK_ROUTE_30
        "route_31",             // 5: LANDMARK_ROUTE_31
        "violet_city",          // 6: LANDMARK_VIOLET_CITY
        "sprout_tower",         // 7: LANDMARK_SPROUT_TOWER
        "route_32",             // 8: LANDMARK_ROUTE_32
        "ruins_of_alph",        // 9: LANDMARK_RUINS_OF_ALPH
        "union_cave",           // 10: LANDMARK_UNION_CAVE
        "route_33",             // 11: LANDMARK_ROUTE_33
        "azalea_town",          // 12: LANDMARK_AZALEA_TOWN
        "slowpoke_well",        // 13: LANDMARK_SLOWPOKE_WELL
        "ilex_forest",          // 14: LANDMARK_ILEX_FOREST
        "route_34",             // 15: LANDMARK_ROUTE_34
        "goldenrod_city",       // 16: LANDMARK_GOLDENROD_CITY
        "radio_tower",          // 17: LANDMARK_RADIO_TOWER
        "route_35",             // 18: LANDMARK_ROUTE_35
        "national_park",        // 19: LANDMARK_NATIONAL_PARK
        "route_36",             // 20: LANDMARK_ROUTE_36
        "route_37",             // 21: LANDMARK_ROUTE_37
        "ecruteak_city",        // 22: LANDMARK_ECRUTEAK_CITY
        "tin_tower",            // 23: LANDMARK_TIN_TOWER
        "burned_tower",         // 24: LANDMARK_BURNED_TOWER
        "route_38",             // 25: LANDMARK_ROUTE_38
        "route_39",             // 26: LANDMARK_ROUTE_39
        "olivine_city",         // 27: LANDMARK_OLIVINE_CITY
        "lighthouse",           // 28: LANDMARK_LIGHTHOUSE
        "battle_tower",         // 29: LANDMARK_BATTLE_TOWER
        "route_40",             // 30: LANDMARK_ROUTE_40
        "whirl_islands",        // 31: LANDMARK_WHIRL_ISLANDS
        "route_41",             // 32: LANDMARK_ROUTE_41
        "cianwood_city",        // 33: LANDMARK_CIANWOOD_CITY
        "route_42",             // 34: LANDMARK_ROUTE_42
        "mt_mortar",            // 35: LANDMARK_MT_MORTAR
        "mahogany_town",        // 36: LANDMARK_MAHOGANY_TOWN
        "route_43",             // 37: LANDMARK_ROUTE_43
        "lake_of_rage",         // 38: LANDMARK_LAKE_OF_RAGE
        "route_44",             // 39: LANDMARK_ROUTE_44
        "ice_path",             // 40: LANDMARK_ICE_PATH
        "blackthorn_city",      // 41: LANDMARK_BLACKTHORN_CITY
        "dragons_den",          // 42: LANDMARK_DRAGONS_DEN
        "route_45",             // 43: LANDMARK_ROUTE_45
        "route_46",             // 44: LANDMARK_ROUTE_46
        "silver_cave",          // 45: LANDMARK_SILVER_CAVE
    };
    
    if (landmark_index < sizeof(landmarks) / sizeof(landmarks[0])) {
        return landmarks[landmark_index];
    }
    return std::format("landmark_{:02x}", landmark_index);
}

std::string MapExtractor::make_fishgroup_id(uint8_t fishgroup_index) const {
    // Known fish groups from constants/map_data_constants.asm
    static const char* fishgroups[] = {
        "none",             // 0: FISHGROUP_NONE
        "shore",            // 1: FISHGROUP_SHORE
        "ocean",            // 2: FISHGROUP_OCEAN
        "lake",             // 3: FISHGROUP_LAKE
        "pond",             // 4: FISHGROUP_POND
        "dratini",          // 5: FISHGROUP_DRATINI
        "qwilfish_swarm",   // 6: FISHGROUP_QWILFISH_SWARM
        "remoraid_swarm",   // 7: FISHGROUP_REMORAID_SWARM
        "gyarados",         // 8: FISHGROUP_GYARADOS
        "dratini_2",        // 9: FISHGROUP_DRATINI_2
        "whirl_islands",    // 10: FISHGROUP_WHIRL_ISLANDS
        "qwilfish",         // 11: FISHGROUP_QWILFISH
        "remoraid",         // 12: FISHGROUP_REMORAID
        "qwilfish_no_swarm", // 13: FISHGROUP_QWILFISH_NO_SWARM
    };
    
    if (fishgroup_index < sizeof(fishgroups) / sizeof(fishgroups[0])) {
        return fishgroups[fishgroup_index];
    }
    return std::format("fishgroup_{:02x}", fishgroup_index);
}

std::string MapExtractor::make_flag_id(uint16_t flag) const {
    // Convert numeric flag to semantic ID
    // High byte indicates flag type, low byte is index
    if (flag == 0xFFFF) return "";  // No flag
    
    // Known event flags could be mapped to semantic names
    // For now, use a stable numeric format
    return std::format("flag_{:04x}", flag);
}

std::string MapExtractor::make_item_id(uint8_t item_index) const {
    // Known items
    static const char* items[] = {
        "none",
        "master_ball",
        "ultra_ball",
        "great_ball",
        "poke_ball",
        "town_map",
        // ... etc
    };
    
    if (item_index < 6) {
        return items[item_index];
    }
    return std::format("item_{:02x}", item_index);
}

std::string MapExtractor::make_script_id(uint8_t group, uint8_t index, const char* suffix) const {
    return std::format("{}_{}", make_map_id(group, index), suffix);
}

//=============================================================================
// ADDRESS RESOLUTION
//=============================================================================

// MapGroup entry layout (MAP_LENGTH = 9 bytes from map_data_constants.asm)
// Format from maps.asm macro:
//   db BANK(MapAttributes), tileset, environment
//   dw MapAttributes
//   db location, music
//   dn phone_flag, palette   (high nibble = phone, low nibble = palette)
//   db fishgroup
//
// We read bytes directly to avoid alignment/packing issues

bool MapExtractor::read_map_group_entry(uint8_t group, uint8_t index, MapGroupEntry& out) const {
    const auto& o = profile_.offsets;
    
    // Bounds check group (groups are 1-indexed, 1..num_map_groups)
    if (group == 0 || group > profile_.counts.num_map_groups) {
        return false;
    }
    
    // MapGroupPointers[group-1] is a 2-byte pointer to the group's map list
    uint32_t group_ptr_addr = o.map_group_pointers + ((group - 1) * 2);
    if (group_ptr_addr + 2 > rom_.size()) {
        stats_.bounds_check_failures++;
        return false;
    }
    
    uint16_t group_addr = rom_.read_word(group_ptr_addr);
    
    // Use profile-driven bank for map data
    uint8_t map_bank = o.map_groups_bank;
    uint32_t group_flat = rom_.bank_to_flat(map_bank, group_addr);
    
    // MapGroup entries are 9 bytes each (MAP_LENGTH)
    constexpr uint8_t map_entry_size = 9;
    
    // index is 1-based in pokecrystal
    uint32_t map_entry_addr = group_flat + ((index - 1) * map_entry_size);
    
    if (map_entry_addr + map_entry_size > rom_.size()) {
        stats_.bounds_check_failures++;
        return false;
    }
    
    // Read the map entry
    auto data = rom_.read_bytes(map_entry_addr, map_entry_size);
    
    out.attr_bank = data[0];
    out.tileset = data[1];
    out.environment = data[2];
    out.attr_ptr = data[3] | (data[4] << 8);
    out.location = data[5];
    out.music = data[6];
    out.phone_palette = data[7];
    out.fishgroup = data[8];
    
    // Validate entry fields to detect garbage data (end of group)
    // Crystal has 36 tilesets (1-36) - value 0 or >36 indicates garbage
    if (out.tileset == 0 || out.tileset > profile_.counts.num_tilesets) {
        return false;
    }
    
    // Environment must be 1-7 (TOWN through DUNGEON)
    if (out.environment == 0 || out.environment > 7) {
        return false;
    }
    
    // attr_bank must be a valid ROM bank (0-127 for 2MB ROM)
    if (out.attr_bank >= 128) {
        return false;
    }
    
    // attr_ptr must be in banked ROM range (0x4000-0x7FFF) or home bank (0x0000-0x3FFF for bank 0)
    if (out.attr_bank == 0) {
        if (out.attr_ptr >= 0x4000) {
            return false;
        }
    } else {
        if (out.attr_ptr < 0x4000 || out.attr_ptr >= 0x8000) {
            return false;
        }
    }
    
    // Additional validation: probe the map header to verify it looks valid
    // Read the first few bytes of the map attributes
    uint32_t header_addr = rom_.bank_to_flat(out.attr_bank, out.attr_ptr);
    if (header_addr + 12 > rom_.size()) {
        return false;
    }
    
    auto header = rom_.read_bytes(header_addr, 12);
    // header[1] = height, header[2] = width
    // Valid maps have dimensions 1-100
    uint8_t height = header[1];
    uint8_t width = header[2];
    if (height == 0 || height > 100 || width == 0 || width > 100) {
        return false;
    }
    
    return true;
}

uint32_t MapExtractor::get_map_header_address(uint8_t group, uint8_t index) const {
    const auto& fmt = profile_.format.map;
    
    MapGroupEntry entry;
    if (!read_map_group_entry(group, index, entry)) {
        return 0;
    }
    
    uint32_t header_addr = rom_.bank_to_flat(entry.attr_bank, entry.attr_ptr);
    
    if (header_addr + fmt.header_size > rom_.size()) {
        stats_.bounds_check_failures++;
        return 0;
    }
    
    return header_addr;
}

//=============================================================================
// EVENT EXTRACTION
//=============================================================================

bool MapExtractor::extract_warps(uint32_t ptr, uint8_t count, 
                                  std::vector<WarpPoint>& out) const {
    const auto& fmt = profile_.format.map;
    out.reserve(count);
    
    for (uint8_t i = 0; i < count; ++i) {
        if (ptr + fmt.warp_size > rom_.size()) {
            stats_.bounds_check_failures++;
            return false;
        }
        
        auto data = rom_.read_bytes(ptr, fmt.warp_size);
        
        WarpPoint warp;
        warp.y = data[0];
        warp.x = data[1];
        warp.target_warp_index = data[2];
        uint8_t target_group = data[3];
        uint8_t target_index = data[4];
        warp.target_map_id = make_map_id(target_group, target_index);
        
        out.push_back(warp);
        stats_.total_warps++;
        ptr += fmt.warp_size;
    }
    
    return true;
}

bool MapExtractor::extract_coord_events(uint32_t ptr, uint8_t count,
                                         std::vector<CoordEvent>& out,
                                         uint8_t script_bank) const {
    const auto& fmt = profile_.format.map;
    out.reserve(count);
    
    // coord_event macro from pokecrystal macros/scripts/maps.asm:
    //   db \3, \2, \1    ; scene_id, y, x
    //   db 0             ; filler
    //   dw \4            ; script pointer
    //   dw 0             ; filler
    // Total: 8 bytes (COORD_EVENT_SIZE)
    
    for (uint8_t i = 0; i < count; ++i) {
        if (ptr + fmt.coord_event_size > rom_.size()) {
            stats_.bounds_check_failures++;
            return false;
        }
        
        auto data = rom_.read_bytes(ptr, fmt.coord_event_size);
        
        CoordEvent evt;
        evt.scene_id = data[0];     // Scene script index (-1 = always active)
        evt.y = data[1];
        evt.x = data[2];
        // byte 3: filler
        // bytes 4-5: script pointer (local to script_bank)
        uint16_t script_ptr = data[4] | (data[5] << 8);
        // bytes 6-7: filler
        
        // Resolve to flat ROM address using script_bank
        evt.script_rom_address = rom_.bank_to_flat(script_bank, script_ptr);
        evt.script_id = std::format("coord_event_{}", i);
        
        out.push_back(evt);
        stats_.total_coord_events++;
        ptr += fmt.coord_event_size;
    }
    
    return true;
}

bool MapExtractor::extract_bg_events(uint32_t ptr, uint8_t count,
                                      std::vector<BgEvent>& out,
                                      uint8_t script_bank) const {
    const auto& fmt = profile_.format.map;
    out.reserve(count);
    
    // bg_event macro from pokecrystal macros/scripts/maps.asm:
    //   db \2, \1, \3    ; y, x, type
    //   dw \4            ; script pointer OR pointer to secondary structure
    // Total: 5 bytes (BG_EVENT_SIZE)
    //
    // BGEVENT types from constants/script_constants.asm:
    //   0 = BGEVENT_READ     - script pointer
    //   1 = BGEVENT_UP       - script pointer (requires facing up)
    //   2 = BGEVENT_DOWN     - script pointer (requires facing down)
    //   3 = BGEVENT_RIGHT    - script pointer (requires facing right)
    //   4 = BGEVENT_LEFT     - script pointer (requires facing left)
    //   5 = BGEVENT_IFSET    - pointer to conditional_event (flag, script)
    //   6 = BGEVENT_IFNOTSET - pointer to conditional_event (flag, script)
    //   7 = BGEVENT_ITEM     - pointer to hiddenitem (flag, item)
    //   8 = BGEVENT_COPY     - unused in Crystal
    
    for (uint8_t i = 0; i < count; ++i) {
        if (ptr + fmt.bg_event_size > rom_.size()) {
            stats_.bounds_check_failures++;
            return false;
        }
        
        auto data = rom_.read_bytes(ptr, fmt.bg_event_size);
        
        BgEvent evt;
        evt.y = data[0];
        evt.x = data[1];
        uint8_t bg_type = data[2];
        
        // Pointer at bytes 3-4 (little-endian, local to script_bank)
        uint16_t event_ptr = data[3] | (data[4] << 8);
        uint32_t event_flat = rom_.bank_to_flat(script_bank, event_ptr);
        
        // Map Crystal bg event types to our enum and parse secondary structures
        switch (bg_type) {
            case 0:  // BGEVENT_READ
                evt.type = BgEventType::Read;
                evt.script_rom_address = event_flat;
                evt.script_id = std::format("bg_event_{}", i);
                break;
                
            case 1:  // BGEVENT_UP
                evt.type = BgEventType::FacingUp;
                evt.script_rom_address = event_flat;
                evt.script_id = std::format("bg_event_{}", i);
                break;
                
            case 2:  // BGEVENT_DOWN
                evt.type = BgEventType::FacingDown;
                evt.script_rom_address = event_flat;
                evt.script_id = std::format("bg_event_{}", i);
                break;
                
            case 3:  // BGEVENT_RIGHT
                evt.type = BgEventType::FacingRight;
                evt.script_rom_address = event_flat;
                evt.script_id = std::format("bg_event_{}", i);
                break;
                
            case 4:  // BGEVENT_LEFT
                evt.type = BgEventType::FacingLeft;
                evt.script_rom_address = event_flat;
                evt.script_id = std::format("bg_event_{}", i);
                break;
                
            case 5:  // BGEVENT_IFSET - conditional_event macro: dw flag, script
            case 6:  // BGEVENT_IFNOTSET
            {
                evt.type = (bg_type == 5) ? BgEventType::IfSet : BgEventType::IfNotSet;
                
                // Read conditional_event structure: dw flag, dw script
                if (event_flat + 4 <= rom_.size()) {
                    auto cond_data = rom_.read_bytes(event_flat, 4);
                    uint16_t flag = cond_data[0] | (cond_data[1] << 8);
                    uint16_t script_ptr = cond_data[2] | (cond_data[3] << 8);
                    
                    evt.condition_flag = make_flag_id(flag);
                    evt.script_rom_address = rom_.bank_to_flat(script_bank, script_ptr);
                    evt.script_id = std::format("bg_event_{}", i);
                }
                break;
            }
                
            case 7:  // BGEVENT_ITEM - hiddenitem macro: dwb flag, item
            {
                evt.type = BgEventType::HiddenItem;
                
                // Read hiddenitem structure: dw flag, db item (3 bytes)
                if (event_flat + 3 <= rom_.size()) {
                    auto item_data = rom_.read_bytes(event_flat, 3);
                    uint16_t flag = item_data[0] | (item_data[1] << 8);
                    uint8_t item = item_data[2];
                    
                    evt.condition_flag = make_flag_id(flag);
                    evt.item_id = make_item_id(item);
                    evt.quantity = 1;  // Hidden items are always quantity 1
                }
                evt.script_rom_address = 0;  // No script for hidden items
                break;
            }
                
            case 8:  // BGEVENT_COPY - unused in Crystal
            default:
                evt.type = BgEventType::Copy;
                evt.script_rom_address = 0;
                break;
        }
        
        out.push_back(evt);
        stats_.total_bg_events++;
        ptr += fmt.bg_event_size;
    }
    
    return true;
}

bool MapExtractor::extract_objects(uint32_t ptr, uint8_t count,
                                    std::vector<ObjectEvent>& out,
                                    uint8_t script_bank,
                                    uint8_t map_group, uint8_t map_index) const {
    const auto& fmt = profile_.format.map;
    out.reserve(count);
    
    // Object event format (from pokecrystal macros/scripts/maps.asm):
    // db sprite, y+4, x+4, movement    ; bytes 0-3
    // dn radius_y, radius_x             ; byte 4 (nibbles)
    // db hour_start, hour_end           ; bytes 5-6
    // dn palette, object_type           ; byte 7 (nibbles)
    // db sight_range                    ; byte 8
    // dw script_ptr, event_flag         ; bytes 9-10, 11-12
    
    //=========================================================================
    // OBJECT POINTER ROLE OVERRIDE TABLE
    //
    // Stock Crystal ROM has exactly 2 objects where OBJECTTYPE_SCRIPT (0)
    // has a pointer field that does NOT contain script bytecode. These are
    // proven assembly-time label reuses that cannot be determined from ROM
    // bytes alone - the assembler knew MovementData_* was movement data
    // and *_MapEvents was a map events header, but that type information
    // is lost in the assembled ROM.
    //
    // Why these objects are safe despite non-script pointers:
    // Both maps use scene scripts (sdefer) that take control immediately
    // on map entry, so the NPC is never in STANDING state when the player
    // could press A to trigger TryObjectEvent.
    //
    // ROM profile: Crystal EN v1.0/v1.1 (UE)
    // Source verification: pokecrystal/maps/BattleTower*.asm
    //=========================================================================
    struct PointerRoleOverride {
        uint8_t group;
        uint8_t map_index;
        uint8_t object_index;   // 0-based local index
        const char* reason;     // Documentation only
    };
    static constexpr PointerRoleOverride NON_SCRIPT_POINTERS[] = {
        // BattleTowerElevator (group 22, map 13)
        // Object 0: BATTLETOWERELEVATOR_RECEPTIONIST
        // Pointer: MovementData_BattleTowerElevatorReceptionistWalksIn
        // Actual data: movement steps (step RIGHT, turn_head DOWN, step_end)
        { 22, 13, 0, "MovementData - scene script controls this NPC" },
        
        // BattleTowerHallway (group 22, map 14)
        // Object 0: BATTLETOWERHALLWAY_RECEPTIONIST
        // Pointer: BattleTowerHallway_MapEvents
        // Actual data: map events header (db 0, 0, def_warp_events, ...)
        { 22, 14, 0, "MapEvents header - scene script controls this NPC" },
    };
    
    auto is_non_script_pointer = [&](uint8_t object_index) -> bool {
        for (const auto& override : NON_SCRIPT_POINTERS) {
            if (override.group == map_group && 
                override.map_index == map_index && 
                override.object_index == object_index) {
                return true;
            }
        }
        return false;
    };
    
    for (uint8_t i = 0; i < count; ++i) {
        if (ptr + fmt.object_event_size > rom_.size()) {
            stats_.bounds_check_failures++;
            return false;
        }
        
        auto data = rom_.read_bytes(ptr, fmt.object_event_size);
        
        // Object event format (from pokecrystal macros/scripts/maps.asm object_event):
        // byte 0:  sprite
        // byte 1:  y + 4
        // byte 2:  x + 4
        // byte 3:  movement function
        // byte 4:  dn radius_y, radius_x (nibbles)
        // byte 5:  hour_start
        // byte 6:  hour_end
        // byte 7:  dn palette, object_type (nibbles)
        // byte 8:  sight_range
        // bytes 9-10:  script_ptr (little-endian)
        // bytes 11-12: event_flag (little-endian)
        
        ObjectEvent obj;
        obj.local_id = i + 1;  // 1-indexed
        obj.sprite_id = make_sprite_id(data[0]);
        obj.y = data[1] - 4;  // Crystal adds 4 to stored Y
        obj.x = data[2] - 4;  // Crystal adds 4 to stored X
        obj.movement_type = data[3];
        obj.movement_radius_x = data[4] & 0x0F;
        obj.movement_radius_y = (data[4] >> 4) & 0x0F;
        obj.hour_start = data[5];
        obj.hour_end = data[6];
        
        // Byte 7: dn palette, object_type
        // High nibble = PAL_NPC_* palette (0 = sprite default)
        // Low nibble = OBJECTTYPE_* constant
        obj.palette = (data[7] >> 4) & 0x0F;
        uint8_t object_type = data[7] & 0x0F;
        
        // Object type determines what the pointer field contains:
        // OBJECTTYPE_SCRIPT   = 0  (normal script bytecode)
        // OBJECTTYPE_ITEMBALL = 1  (item + quantity data, not script)
        // OBJECTTYPE_TRAINER  = 2  (trainer data struct, not script)
        obj.is_trainer = (object_type == 2);  // OBJECTTYPE_TRAINER
        
        obj.trainer_sight_range = data[8];
        
        // Script pointer at bytes 9-10 (2 bytes, local to map's script_bank)
        // Event flag at bytes 11-12
        uint16_t script_ptr = data[9] | (data[10] << 8);
        
        // Only OBJECTTYPE_SCRIPT (0) has actual script bytecode at the pointer.
        // OBJECTTYPE_ITEMBALL (1) points to itemball data (item + qty)
        // OBJECTTYPE_TRAINER (2) points to trainer data struct
        //
        // Additionally, some OBJECTTYPE_SCRIPT objects have non-script pointers
        // due to assembly-time label reuse (see NON_SCRIPT_POINTERS table above).
        bool has_script_bytecode = (object_type == 0) && !is_non_script_pointer(i);
        
        if (!has_script_bytecode) {
            // Non-script object types don't have script bytecode at their pointer
            if (obj.is_trainer) {
                obj.script_id = std::format("trainer_{}", i);
            } else if (object_type == 1) {
                obj.script_id = std::format("itemball_{}", i);
            } else {
                // OBJECTTYPE_SCRIPT with non-script pointer (Battle Tower exceptions)
                obj.script_id = std::format("nonscript_{}", i);
            }
            obj.script_rom_address = 0;
        } else {
            // Normal NPCs - script pointer is local to script_bank
            obj.script_rom_address = rom_.bank_to_flat(script_bank, script_ptr);
            obj.script_id = std::format("object_script_{}", i);
        }
        
        // Visibility flag at bytes 11-12
        uint16_t flag = data[11] | (data[12] << 8);
        if (flag == 0xFFFF) {
            obj.visibility_flag = "";  // Always visible
        } else {
            obj.visibility_flag = make_flag_id(flag);
        }
        
        out.push_back(obj);
        stats_.total_objects++;
        ptr += fmt.object_event_size;
    }
    
    return true;
}

bool MapExtractor::extract_connections(uint32_t map_attr_addr, uint8_t conn_byte,
                                        std::vector<MapConnection>& out) const {
    // Connection byte encodes which directions have connections
    // From map_data_constants.asm: bit 3: North, bit 2: South, bit 1: West, bit 0: East
    
    const auto& fmt = profile_.format.map;
    
    // Connections follow immediately after the 12-byte MapAttributes header
    uint32_t conn_ptr = map_attr_addr + fmt.header_size;
    
    auto read_connection = [&](Direction dir) -> bool {
        if (conn_ptr + fmt.connection_size > rom_.size()) {
            stats_.bounds_check_failures++;
            return false;
        }
        
        auto data = rom_.read_bytes(conn_ptr, fmt.connection_size);
        
        MapConnection conn;
        conn.direction = dir;
        
        // Connection data format (from connection macro in attributes.asm):
        // 0-1: map_id (group, map)
        // 2-3: blocks pointer offset in source map
        // 4-5: blocks pointer offset in target map  
        // 6: strip length
        // 7: target width
        // 8: y offset
        // 9: x offset
        // 10-11: window pointer offset
        
        uint8_t target_group = data[0];
        uint8_t target_index = data[1];
        conn.target_map_id = make_map_id(target_group, target_index);
        conn.strip_offset = static_cast<int8_t>(data[8]);  // y or x offset
        conn.strip_length = data[6];
        
        out.push_back(conn);
        conn_ptr += fmt.connection_size;
        return true;
    };
    
    // Read connections in order: North, South, West, East
    // (must match the order in attributes.asm)
    if (conn_byte & 0x08) {  // NORTH
        if (!read_connection(Direction::North)) return false;
    }
    if (conn_byte & 0x04) {  // SOUTH
        if (!read_connection(Direction::South)) return false;
    }
    if (conn_byte & 0x02) {  // WEST
        if (!read_connection(Direction::West)) return false;
    }
    if (conn_byte & 0x01) {  // EAST
        if (!read_connection(Direction::East)) return false;
    }
    
    return true;
}

//=============================================================================
// MAIN EXTRACTION
//=============================================================================

MapExtractionResult MapExtractor::extract_map(uint8_t group, uint8_t index) const {
    MapExtractionResult result;
    
    // First read the MapGroup entry to get secondary metadata
    MapGroupEntry map_entry;
    if (!read_map_group_entry(group, index, map_entry)) {
        result.error = "Invalid map group/index or address out of bounds";
        stats_.maps_failed++;
        return result;
    }
    
    // Get map header address from the entry
    uint32_t header_addr = rom_.bank_to_flat(map_entry.attr_bank, map_entry.attr_ptr);
    const auto& fmt = profile_.format.map;
    
    if (header_addr + fmt.header_size > rom_.size()) {
        result.error = "Map header address out of bounds";
        stats_.maps_failed++;
        return result;
    }
    
    // Read map header (12 bytes for Crystal)
    std::vector<uint8_t> header;
    if (!read_map_header(header_addr, header)) {
        result.error = "Failed to read map header";
        stats_.maps_failed++;
        return result;
    }
    
    // Parse header fields (MapAttributes from data/maps/attributes.asm)
    ExtractedMap& map = result.map;
    map.border_block = header[fmt.border_block_offset];
    map.height = header[fmt.height_offset];
    map.width = header[fmt.width_offset];
    
    // Validate dimensions
    if (map.width == 0 || map.height == 0 || 
        map.width > 100 || map.height > 100) {
        result.error = std::format("Invalid dimensions: {}x{}", map.width, map.height);
        stats_.maps_failed++;
        return result;
    }
    
    // Block data pointer
    uint8_t block_bank = header[fmt.blockdata_bank_offset];
    uint16_t block_ptr = header[fmt.blockdata_ptr_offset] |
                         (header[fmt.blockdata_ptr_offset + 1] << 8);
    
    // Script pointer (also determines bank for events)
    uint8_t script_bank = header[fmt.script_bank_offset];
    uint16_t script_ptr = header[fmt.script_ptr_offset] |
                          (header[fmt.script_ptr_offset + 1] << 8);
    
    // Events pointer (in same bank as script)
    uint16_t events_ptr = header[fmt.events_ptr_offset] |
                          (header[fmt.events_ptr_offset + 1] << 8);
    
    // Connection byte (bitfield: bit 3=north, 2=south, 1=west, 0=east)
    uint8_t conn_byte = header[fmt.connections_offset];
    
    // Extract block data
    if (!read_block_data(block_bank, block_ptr, map.width, map.height, map.blocks)) {
        result.error = "Failed to read block data";
        stats_.maps_failed++;
        return result;
    }
    
    // Set semantic IDs
    map.map_id = make_map_id(group, index);
    map.display_name = map.map_id;
    std::transform(map.display_name.begin(), map.display_name.end(), 
                   map.display_name.begin(), [](char c) {
        return c == '_' ? ' ' : c;
    });
    
    // Script ID
    map.map_script_id = make_script_id(group, index, "scripts");
    
    // Extract secondary metadata from the 9-byte MapGroup entry
    map.tileset_id = make_tileset_id(map_entry.tileset);
    map.environment_type = map_entry.environment;
    map.landmark_id = make_landmark_id(map_entry.location);
    map.music_id = make_music_id(map_entry.music);
    map.phone_service_disabled = map_entry.phone_service_disabled();
    map.lighting = map_entry.palette();  // PALETTE_AUTO, PALETTE_DAY, etc.
    map.fish_group_id = make_fishgroup_id(map_entry.fishgroup);
    
    // Determine if outdoor based on environment type
    // TOWN=1, ROUTE=2 are outdoor; INDOOR=3, CAVE=4, ENVIRONMENT_5=5, GATE=6, DUNGEON=7 are not
    map.is_outdoor = (map_entry.environment == 1 || map_entry.environment == 2);
    
    // Extract events from events pointer
    uint32_t events_flat = rom_.bank_to_flat(script_bank, events_ptr);
    if (events_flat + 2 <= rom_.size()) {
        // Events header: 2 filler bytes, then counts for each event type
        auto events_header = rom_.read_bytes(events_flat, 2);
        // Skip filler
        uint32_t event_ptr = events_flat + 2;
        
        // Read warp count and extract warps
        if (event_ptr < rom_.size()) {
            uint8_t warp_count = rom_.read_byte(event_ptr++);
            if (warp_count > 0 && warp_count < 100) {
                extract_warps(event_ptr, warp_count, map.warps);
                event_ptr += warp_count * fmt.warp_size;
            }
        }
        
        // Read coord event count and extract
        if (event_ptr < rom_.size()) {
            uint8_t coord_count = rom_.read_byte(event_ptr++);
            if (coord_count > 0 && coord_count < 100) {
                extract_coord_events(event_ptr, coord_count, map.coord_events, script_bank);
                event_ptr += coord_count * fmt.coord_event_size;
            }
        }
        
        // Read bg event count and extract
        if (event_ptr < rom_.size()) {
            uint8_t bg_count = rom_.read_byte(event_ptr++);
            if (bg_count > 0 && bg_count < 100) {
                extract_bg_events(event_ptr, bg_count, map.bg_events, script_bank);
                event_ptr += bg_count * fmt.bg_event_size;
            }
        }
        
        // Read object event count and extract
        if (event_ptr < rom_.size()) {
            uint8_t obj_count = rom_.read_byte(event_ptr++);
            if (obj_count > 0 && obj_count < 100) {
                extract_objects(event_ptr, obj_count, map.objects, script_bank, group, index);
            }
        }
    }
    
    // Extract connections if any
    if (conn_byte != 0) {
        // Connections follow immediately after the header in attributes.asm
        extract_connections(header_addr, conn_byte, map.connections);
    }
    
#ifndef NDEBUG
    // Debug metadata
    map.debug.crystal_group = group;
    map.debug.crystal_index = index;
    map.debug.header_rom_addr = header_addr;
    map.debug.blocks_rom_addr = rom_.bank_to_flat(block_bank, block_ptr);
    map.debug.script_rom_addr = rom_.bank_to_flat(script_bank, script_ptr);
    map.debug.events_rom_addr = events_flat;
#endif
    
    stats_.maps_extracted++;
    stats_.total_blocks += map.blocks.size();
    result.success = true;
    return result;
}

MapExtractionResult MapExtractor::extract_map(const std::string& map_id) const {
    // Look up group/index from semantic ID
    // Based on pokecrystal data/maps/maps.asm MapGroup_* definitions
    
    // Group 24: New Bark area (MapGroup_NewBark)
    if (map_id == "route_26") return extract_map(24, 1);
    if (map_id == "route_27") return extract_map(24, 2);
    if (map_id == "route_29") return extract_map(24, 3);
    if (map_id == "new_bark_town") return extract_map(24, 4);
    if (map_id == "elms_lab") return extract_map(24, 5);
    if (map_id == "players_house_1f") return extract_map(24, 6);
    if (map_id == "players_house_2f") return extract_map(24, 7);
    if (map_id == "players_neighbors_house") return extract_map(24, 8);
    if (map_id == "elms_house") return extract_map(24, 9);
    if (map_id == "route_26_heal_house") return extract_map(24, 10);
    if (map_id == "day_of_week_siblings_house") return extract_map(24, 11);
    if (map_id == "route_27_sandstorm_house") return extract_map(24, 12);
    if (map_id == "route_29_route_46_gate") return extract_map(24, 13);
    
    // Group 26: Cherrygrove area (MapGroup_Cherrygrove)
    if (map_id == "route_30") return extract_map(26, 1);
    if (map_id == "route_31") return extract_map(26, 2);
    if (map_id == "cherrygrove_city") return extract_map(26, 3);
    if (map_id == "cherrygrove_mart") return extract_map(26, 4);
    if (map_id == "cherrygrove_pokecenter_1f") return extract_map(26, 5);
    if (map_id == "cherrygrove_gym_speech_house") return extract_map(26, 6);
    if (map_id == "guide_gents_house") return extract_map(26, 7);
    if (map_id == "cherrygrove_evolution_speech_house") return extract_map(26, 8);
    if (map_id == "route_30_berry_house") return extract_map(26, 9);
    if (map_id == "mr_pokemons_house") return extract_map(26, 10);
    if (map_id == "route_31_violet_gate") return extract_map(26, 11);
    
    // Try to parse fallback format
    if (map_id.starts_with("map_g") && map_id.find("_i") != std::string::npos) {
        int g = 0, i = 0;
        if (sscanf(map_id.c_str(), "map_g%d_i%d", &g, &i) == 2) {
            return extract_map(static_cast<uint8_t>(g), static_cast<uint8_t>(i));
        }
    }
    
    MapExtractionResult result;
    result.error = "Unknown map ID: " + map_id;
    return result;
}

std::vector<ExtractedMap> MapExtractor::extract_all_maps() const {
    std::vector<ExtractedMap> maps;
    
    // Extract all maps from all groups
    for (uint8_t group = 1; group <= profile_.counts.num_map_groups; ++group) {
        for (uint8_t index = 1; index < 100; ++index) {
            auto result = extract_map(group, index);
            if (!result.success) {
                break;  // End of group
            }
            if (result.map.width == 0 || result.map.height == 0) {
                break;  // Invalid data
            }
            maps.push_back(std::move(result.map));
        }
    }
    
    return maps;
}

std::vector<MapGroupInfo> MapExtractor::get_map_groups() const {
    // Return info about map groups
    std::vector<MapGroupInfo> groups;
    
    // Known groups
    groups.push_back({"new_bark_area", {"new_bark_town", "elms_lab", "players_house_1f", 
                                         "players_house_2f", "players_neighbors_house", "elms_house"}});
    groups.push_back({"cherrygrove_area", {"cherrygrove_city", "route_30", "route_31"}});
    // ... etc
    
    return groups;
}

} // namespace crystal
