#pragma once
// crystal/extract/sprite_ids.hpp
// Authoritative sprite ID mapping for Crystal ROM
//
// Maps Crystal sprite indices (1-102) to semantic sprite IDs.
// Source: pokecrystal constants/sprite_constants.asm
//
// USAGE:
//   MapExtractor and SpriteExtractor both use these functions.
//   This ensures consistent sprite ID resolution across the compiler.
//
// INVARIANTS:
//   - Same raw sprite index → same semantic SpriteId everywhere
//   - Valid IDs 1..102 → always resolvable
//   - Invalid IDs (0, 103+) → explicit empty string / index 0

#include <cstdint>
#include <string>

namespace crystal {

//=============================================================================
// CRYSTAL SPRITE DOMAIN
//=============================================================================

// Valid Crystal sprite indices are 1..102 (SPRITE_NONE = 0 is invalid)
constexpr uint8_t CRYSTAL_SPRITE_MIN = 1;
constexpr uint8_t CRYSTAL_SPRITE_MAX = 102;

//=============================================================================
// SPRITE ID MAPPING
// Authoritative semantic names from pokecrystal sprite_constants.asm
//=============================================================================

// Convert sprite index to semantic ID
// Returns empty string for invalid indices (0, 103+)
inline std::string crystal_sprite_index_to_id(uint8_t index) {
    // Maps sprite index to semantic ID
    // Matches SPRITE_* constants from sprite_constants.asm
    static const char* SPRITE_NAMES[] = {
        nullptr,            // 0 = SPRITE_NONE (invalid)
        "chris",            // 1
        "chris_bike",       // 2
        "gameboy_kid",      // 3
        "rival",            // 4
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
        "teacher",          // 41
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
        "fisher",           // 58
        "fishing_guru",     // 59
        "scientist",        // 60
        "kimono_girl",      // 61
        "sage",             // 62
        "unused_guy",       // 63
        "gentleman",        // 64
        "black_belt",       // 65
        "receptionist",     // 66
        "officer",          // 67
        "cal",              // 68
        "slowpoke",         // 69
        "captain",          // 70
        "big_lapras",       // 71
        "gym_guide",        // 72
        "sailor",           // 73
        "biker",            // 74
        "pharmacist",       // 75
        "monster",          // 76
        "fairy",            // 77
        "bird",             // 78
        "dragon",           // 79
        "big_onix",         // 80
        "n64",              // 81
        "sudowoodo",        // 82
        "surf",             // 83
        "poke_ball",        // 84
        "pokedex",          // 85
        "paper",            // 86
        "virtual_boy",      // 87
        "old_link_receptionist", // 88
        "rock",             // 89
        "boulder",          // 90
        "snes",             // 91
        "famicom",          // 92
        "fruit_tree",       // 93
        "gold_trophy",      // 94
        "silver_trophy",    // 95
        "kris",             // 96
        "kris_bike",        // 97
        "kurt_outside",     // 98
        "suicune",          // 99
        "entei",            // 100
        "raikou",           // 101
        "standing_youngster", // 102
    };
    constexpr size_t NUM_SPRITE_NAMES = sizeof(SPRITE_NAMES) / sizeof(SPRITE_NAMES[0]);
    
    if (index < NUM_SPRITE_NAMES && SPRITE_NAMES[index] != nullptr) {
        return SPRITE_NAMES[index];
    }
    return "";  // Invalid index
}

// Convert semantic ID to sprite index
// Returns 0 for unknown IDs (SPRITE_NONE = invalid)
inline uint8_t crystal_sprite_id_to_index(const std::string& id) {
    if (id.empty()) return 0;
    
    // Search through valid range
    for (uint8_t i = CRYSTAL_SPRITE_MIN; i <= CRYSTAL_SPRITE_MAX; ++i) {
        if (crystal_sprite_index_to_id(i) == id) {
            return i;
        }
    }
    return 0;  // Unknown ID
}

// Check if sprite index is valid
inline bool crystal_sprite_index_valid(uint8_t index) {
    return index >= CRYSTAL_SPRITE_MIN && index <= CRYSTAL_SPRITE_MAX;
}

// Check if sprite ID is valid
inline bool crystal_sprite_id_valid(const std::string& id) {
    return crystal_sprite_id_to_index(id) != 0;
}

} // namespace crystal
