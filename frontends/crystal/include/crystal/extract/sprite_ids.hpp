#pragma once
// crystal/extract/sprite_ids.hpp
// Authoritative sprite ID mapping for Crystal ROM
//
// Maps Crystal sprite indices to typed semantic SpriteRef strings.
// Source: pokecrystal/constants/sprite_constants.asm
//
// NAMESPACES:
//   0x01-0x66 (1-102):  Fixed overworld sprites → "fixed:<name>"
//   0x80-0xA2 (128-162): Pokémon icon sprites   → "pokemon_icon:<species_index>"
//   0xE0-0xE1 (224-225): Day Care Pokémon       → "daycare:<1|2>"
//   0xF0-0xFC (240-252): Variable sprite slots   → "variable:<slot_name>"
//
// The encoded string is the canonical sprite_id stored in the package.
// Runtime resolves each namespace appropriately:
//   fixed       → package sprite asset by name
//   pokemon_icon → dynamic species icon (species resolved from GameState)
//   daycare     → dynamic (species from wBreedMon1/2Species equivalent)
//   variable    → runtime wVariableSprites equivalent lookup
//
// INVARIANT: No valid stock Crystal sprite byte maps to "" (empty string).

#include <cstdint>
#include <string>
#include <string_view>
#include <stdexcept>
#include <cstdio>

namespace crystal {

//=============================================================================
// CRYSTAL SPRITE NAMESPACE BOUNDARIES
// Source: pokecrystal/constants/sprite_constants.asm
//=============================================================================

constexpr uint8_t CRYSTAL_SPRITE_FIXED_MIN    = 0x01;  // SPRITE_CHRIS
constexpr uint8_t CRYSTAL_SPRITE_FIXED_MAX    = 0x66;  // SPRITE_STANDING_YOUNGSTER (102)
constexpr uint8_t CRYSTAL_SPRITE_POKEMON_MIN  = 0x80;  // SPRITE_POKEMON / SPRITE_UNOWN
constexpr uint8_t CRYSTAL_SPRITE_POKEMON_MAX  = 0xA2;  // SPRITE_HO_OH
constexpr uint8_t CRYSTAL_SPRITE_DAYCARE_1    = 0xE0;  // SPRITE_DAY_CARE_MON_1
constexpr uint8_t CRYSTAL_SPRITE_DAYCARE_2    = 0xE1;  // SPRITE_DAY_CARE_MON_2
constexpr uint8_t CRYSTAL_SPRITE_VARS_MIN     = 0xF0;  // SPRITE_VARS / SPRITE_CONSOLE
constexpr uint8_t CRYSTAL_SPRITE_VARS_MAX     = 0xFC;  // SPRITE_JANINE_IMPERSONATOR

// Legacy aliases (kept for backward compat with callers using old names)
constexpr uint8_t CRYSTAL_SPRITE_MIN = CRYSTAL_SPRITE_FIXED_MIN;
constexpr uint8_t CRYSTAL_SPRITE_MAX = CRYSTAL_SPRITE_FIXED_MAX;

//=============================================================================
// TAG PREFIXES
// Wire-format tags embedded in sprite_id strings.
//=============================================================================

constexpr std::string_view SPRITE_TAG_FIXED        = "fixed:";
constexpr std::string_view SPRITE_TAG_POKEMON_ICON  = "pokemon_icon:";
constexpr std::string_view SPRITE_TAG_DAYCARE       = "daycare:";
constexpr std::string_view SPRITE_TAG_VARIABLE      = "variable:";

//=============================================================================
// FIXED SPRITE NAMES (0x01–0x66)
// Source: pokecrystal/constants/sprite_constants.asm SPRITE_* constants
//=============================================================================

// Returns the bare name for a fixed sprite index (1-102), or "" if out of range.
inline const char* crystal_fixed_sprite_name(uint8_t index) {
    static const char* SPRITE_NAMES[] = {
        nullptr,                    // 0 = SPRITE_NONE (invalid)
        "chris",                    // 0x01
        "chris_bike",               // 0x02
        "gameboy_kid",              // 0x03
        "rival",                    // 0x04
        "oak",                      // 0x05
        "red",                      // 0x06
        "blue",                     // 0x07
        "bill",                     // 0x08
        "elder",                    // 0x09
        "janine",                   // 0x0A
        "kurt",                     // 0x0B
        "mom",                      // 0x0C
        "blaine",                   // 0x0D
        "reds_mom",                 // 0x0E
        "daisy",                    // 0x0F
        "elm",                      // 0x10
        "will",                     // 0x11
        "falkner",                  // 0x12
        "whitney",                  // 0x13
        "bugsy",                    // 0x14
        "morty",                    // 0x15
        "chuck",                    // 0x16
        "jasmine",                  // 0x17
        "pryce",                    // 0x18
        "clair",                    // 0x19
        "brock",                    // 0x1A
        "karen",                    // 0x1B
        "bruno",                    // 0x1C
        "misty",                    // 0x1D
        "lance",                    // 0x1E
        "surge",                    // 0x1F
        "erika",                    // 0x20
        "koga",                     // 0x21
        "sabrina",                  // 0x22
        "cooltrainer_m",            // 0x23
        "cooltrainer_f",            // 0x24
        "bug_catcher",              // 0x25
        "twin",                     // 0x26
        "youngster",                // 0x27
        "lass",                     // 0x28
        "teacher",                  // 0x29
        "beauty",                   // 0x2A
        "super_nerd",               // 0x2B
        "rocker",                   // 0x2C
        "pokefan_m",                // 0x2D
        "pokefan_f",                // 0x2E
        "gramps",                   // 0x2F
        "granny",                   // 0x30
        "swimmer_guy",              // 0x31
        "swimmer_girl",             // 0x32
        "big_snorlax",              // 0x33
        "surfing_pikachu",          // 0x34
        "rocket",                   // 0x35
        "rocket_girl",              // 0x36
        "nurse",                    // 0x37
        "link_receptionist",        // 0x38
        "clerk",                    // 0x39
        "fisher",                   // 0x3A
        "fishing_guru",             // 0x3B
        "scientist",                // 0x3C
        "kimono_girl",              // 0x3D
        "sage",                     // 0x3E
        "unused_guy",               // 0x3F
        "gentleman",                // 0x40
        "black_belt",               // 0x41
        "receptionist",             // 0x42
        "officer",                  // 0x43
        "cal",                      // 0x44
        "slowpoke",                 // 0x45
        "captain",                  // 0x46
        "big_lapras",               // 0x47
        "gym_guide",                // 0x48
        "sailor",                   // 0x49
        "biker",                    // 0x4A
        "pharmacist",               // 0x4B
        "monster",                  // 0x4C
        "fairy",                    // 0x4D
        "bird",                     // 0x4E
        "dragon",                   // 0x4F
        "big_onix",                 // 0x50
        "n64",                      // 0x51
        "sudowoodo",                // 0x52
        "surf",                     // 0x53
        "poke_ball",                // 0x54
        "pokedex",                  // 0x55
        "paper",                    // 0x56
        "virtual_boy",              // 0x57
        "old_link_receptionist",    // 0x58
        "rock",                     // 0x59
        "boulder",                  // 0x5A
        "snes",                     // 0x5B
        "famicom",                  // 0x5C
        "fruit_tree",               // 0x5D
        "gold_trophy",              // 0x5E
        "silver_trophy",            // 0x5F
        "kris",                     // 0x60
        "kris_bike",                // 0x61
        "kurt_outside",             // 0x62
        "suicune",                  // 0x63
        "entei",                    // 0x64
        "raikou",                   // 0x65
        "standing_youngster",       // 0x66
    };
    constexpr size_t N = sizeof(SPRITE_NAMES) / sizeof(SPRITE_NAMES[0]);
    if (index < N && SPRITE_NAMES[index] != nullptr)
        return SPRITE_NAMES[index];
    return nullptr;
}

//=============================================================================
// VARIABLE SPRITE SLOT NAMES (0xF0–0xFC)
// Source: pokecrystal/constants/sprite_constants.asm SPRITE_VARS section
//=============================================================================

// Returns the semantic slot name for a variable sprite index (0xF0-0xFC), or nullptr.
inline const char* crystal_variable_sprite_name(uint8_t index) {
    // index must be in [0xF0, 0xFC]
    static const char* VAR_NAMES[] = {
        "console",              // 0xF0  SPRITE_CONSOLE
        "doll_1",               // 0xF1  SPRITE_DOLL_1
        "doll_2",               // 0xF2  SPRITE_DOLL_2
        "big_doll",             // 0xF3  SPRITE_BIG_DOLL
        "weird_tree",           // 0xF4  SPRITE_WEIRD_TREE
        "olivine_rival",        // 0xF5  SPRITE_OLIVINE_RIVAL
        "azalea_rocket",        // 0xF6  SPRITE_AZALEA_ROCKET
        "fuchsia_gym_1",        // 0xF7  SPRITE_FUCHSIA_GYM_1
        "fuchsia_gym_2",        // 0xF8  SPRITE_FUCHSIA_GYM_2
        "fuchsia_gym_3",        // 0xF9  SPRITE_FUCHSIA_GYM_3
        "fuchsia_gym_4",        // 0xFA  SPRITE_FUCHSIA_GYM_4
        "copycat",              // 0xFB  SPRITE_COPYCAT
        "janine_impersonator",  // 0xFC  SPRITE_JANINE_IMPERSONATOR
    };
    if (index < CRYSTAL_SPRITE_VARS_MIN || index > CRYSTAL_SPRITE_VARS_MAX)
        return nullptr;
    return VAR_NAMES[index - CRYSTAL_SPRITE_VARS_MIN];
}

//=============================================================================
// PRIMARY API: crystal_sprite_byte_to_id()
//
// Convert a raw Crystal object-event sprite byte to a typed semantic sprite_id.
// NEVER returns "": every valid namespace returns a tagged string.
// Unknown bytes (e.g., 0, 0x67–0x7F, 0xA3–0xDF, 0xFD-0xFF) return an
// explicit "unknown:<hex>" tag so the problem is surfaced, not silenced.
//=============================================================================

inline std::string crystal_sprite_byte_to_id(uint8_t byte) {
    // Namespace 0: Fixed overworld sprites (0x01–0x66)
    if (byte >= CRYSTAL_SPRITE_FIXED_MIN && byte <= CRYSTAL_SPRITE_FIXED_MAX) {
        const char* name = crystal_fixed_sprite_name(byte);
        if (name) return std::string(SPRITE_TAG_FIXED) + name;
    }

    // Namespace 1: Pokémon icon sprites (0x80–0xA2)
    // Source: SpriteMons table; index = byte - SPRITE_POKEMON (0x80)
    if (byte >= CRYSTAL_SPRITE_POKEMON_MIN && byte <= CRYSTAL_SPRITE_POKEMON_MAX) {
        uint8_t icon_index = byte - CRYSTAL_SPRITE_POKEMON_MIN;
        return std::string(SPRITE_TAG_POKEMON_ICON) + std::to_string(icon_index);
    }

    // Namespace 2: Day Care Pokémon sentinels (0xE0, 0xE1)
    if (byte == CRYSTAL_SPRITE_DAYCARE_1) return std::string(SPRITE_TAG_DAYCARE) + "1";
    if (byte == CRYSTAL_SPRITE_DAYCARE_2) return std::string(SPRITE_TAG_DAYCARE) + "2";

    // Namespace 3: Variable sprite slots (0xF0–0xFC)
    if (byte >= CRYSTAL_SPRITE_VARS_MIN && byte <= CRYSTAL_SPRITE_VARS_MAX) {
        const char* name = crystal_variable_sprite_name(byte);
        if (name) return std::string(SPRITE_TAG_VARIABLE) + name;
    }

    // Everything else (0x00, 0x67-0x7F, 0xA3-0xDF, 0xFD-0xFF) is genuinely invalid.
    // There is no source-proven Crystal overworld behavior for these bytes.
    // Callers must never reach this path with a byte from a known-valid object event;
    // throw so the invalid byte surfaces at extraction time rather than silently
    // producing a runtime identity.
    throw std::runtime_error(
        std::string("crystal_sprite_byte_to_id: byte 0x") +
        [byte]{ char buf[3]; std::snprintf(buf, sizeof(buf), "%02x", byte); return std::string(buf); }() +
        " is outside all defined Crystal sprite namespaces "
        "(fixed 0x01-0x66, pokemon_icon 0x80-0xA2, daycare 0xE0-0xE1, variable 0xF0-0xFC)");
}

//=============================================================================
// BACKWARD-COMPAT WRAPPERS
// These let existing callers (sprite_ids_to_index, crystal_sprite_id_valid)
// continue to work for the fixed namespace only.
//=============================================================================

// Convert sprite index (1-102) to bare semantic name.
// Kept for backward compatibility; new code should use crystal_sprite_byte_to_id().
inline std::string crystal_sprite_index_to_id(uint8_t index) {
    const char* name = crystal_fixed_sprite_name(index);
    return name ? name : "";
}

// Reverse lookup: bare name → fixed sprite index. Returns 0 for unknown.
inline uint8_t crystal_sprite_id_to_index(const std::string& id) {
    if (id.empty()) return 0;
    for (uint8_t i = CRYSTAL_SPRITE_FIXED_MIN; i <= CRYSTAL_SPRITE_FIXED_MAX; ++i) {
        const char* name = crystal_fixed_sprite_name(i);
        if (name && id == name) return i;
    }
    return 0;
}

// Check if a raw index is in the fixed range
inline bool crystal_sprite_index_valid(uint8_t index) {
    return index >= CRYSTAL_SPRITE_FIXED_MIN && index <= CRYSTAL_SPRITE_FIXED_MAX;
}

// Check if a bare name is a valid fixed sprite
inline bool crystal_sprite_id_valid(const std::string& id) {
    return crystal_sprite_id_to_index(id) != 0;
}

//=============================================================================
// SPRITE_ID TAG QUERIES
// Used by runtime resolution to dispatch to the correct loader.
//=============================================================================

inline bool sprite_id_is_fixed(const std::string& id) {
    return id.starts_with(SPRITE_TAG_FIXED);
}
inline bool sprite_id_is_pokemon_icon(const std::string& id) {
    return id.starts_with(SPRITE_TAG_POKEMON_ICON);
}
inline bool sprite_id_is_daycare(const std::string& id) {
    return id.starts_with(SPRITE_TAG_DAYCARE);
}
inline bool sprite_id_is_variable(const std::string& id) {
    return id.starts_with(SPRITE_TAG_VARIABLE);
}

// Extract the bare name from a fixed sprite id ("fixed:teacher" → "teacher")
inline std::string sprite_id_fixed_name(const std::string& id) {
    if (!sprite_id_is_fixed(id)) return "";
    return id.substr(SPRITE_TAG_FIXED.size());
}

// Extract icon index from pokemon_icon id ("pokemon_icon:8" → 8)
inline int sprite_id_pokemon_icon_index(const std::string& id) {
    if (!sprite_id_is_pokemon_icon(id)) return -1;
    try { return std::stoi(id.substr(SPRITE_TAG_POKEMON_ICON.size())); }
    catch (...) { return -1; }
}

// Extract daycare slot (1 or 2) from daycare id ("daycare:1" → 1)
inline int sprite_id_daycare_slot(const std::string& id) {
    if (!sprite_id_is_daycare(id)) return -1;
    try { return std::stoi(id.substr(SPRITE_TAG_DAYCARE.size())); }
    catch (...) { return -1; }
}

// Extract variable slot name from variable id ("variable:copycat" → "copycat")
inline std::string sprite_id_variable_name(const std::string& id) {
    if (!sprite_id_is_variable(id)) return "";
    return id.substr(SPRITE_TAG_VARIABLE.size());
}

} // namespace crystal
