#pragma once
// engine/party/pokemon.hpp
// Individual Pokemon instance (in party or PC)

#include "engine/core/types.hpp"
#include <array>
#include <string>
#include <cstdint>

namespace enginemon {

// Individual Pokemon instance
struct Pokemon {
    // Identity
    SpeciesId species;
    std::string nickname;       // Empty = use species name
    uint32_t personality;       // Determines gender, shininess in Gen 2
    uint16_t ot_id;            // Original trainer ID
    std::string ot_name;       // Original trainer name
    
    // Level and experience
    uint8_t level;
    uint32_t exp;
    
    // Stats
    uint16_t current_hp;
    uint16_t max_hp;
    uint16_t attack;
    uint16_t defense;
    uint16_t speed;
    uint16_t special_attack;
    uint16_t special_defense;
    
    // IVs (0-15 in Gen 2)
    // Gen 2 uses DVs: Attack, Defense, Speed, Special
    // HP DV is derived from others
    struct DVs {
        uint8_t attack : 4;
        uint8_t defense : 4;
        uint8_t speed : 4;
        uint8_t special : 4;    // Used for both SpAtk and SpDef
    } dvs;
    
    // EVs (0-65535 total per stat, capped at 255 contribution)
    // Gen 2 calls these Stat Exp
    struct StatExp {
        uint16_t hp;
        uint16_t attack;
        uint16_t defense;
        uint16_t speed;
        uint16_t special;       // Shared for SpAtk/SpDef
    } stat_exp;
    
    // Moves
    struct Move {
        MoveId id;
        uint8_t pp;
        uint8_t pp_ups;         // 0-3, each adds 20% PP
    };
    std::array<Move, 4> moves;
    
    // Status
    Status status = Status::None;
    uint8_t status_data = 0;    // Sleep turns remaining, toxic counter
    
    // Held item
    ItemId held_item = ITEM_NONE;
    
    // Happiness/Friendship (0-255)
    uint8_t friendship = 0;
    
    // Pokerus
    uint8_t pokerus = 0;        // Strain and days
    
    // Met info
    MapId met_location = MAP_NONE;
    uint8_t met_level = 0;
    TimeOfDay met_time = TimeOfDay::Day;
    
    // Egg info
    bool is_egg = false;
    uint8_t egg_cycles = 0;     // Steps to hatch / 256
    
    // Computed properties
    bool is_shiny() const;
    uint8_t gender() const;     // 0=male, 1=female, 2=genderless
    uint8_t hp_dv() const;      // Derived from other DVs
    
    // Recalculate stats from base, DVs, stat exp, level
    void recalculate_stats(const struct SpeciesData& species_data);
    
    // PP helpers
    uint8_t max_pp(size_t move_slot, const struct MoveData& move_data) const;
    void restore_pp(size_t move_slot, const struct MoveData& move_data);
    void restore_all_pp(const struct Registries& reg);
    
    // Heal
    void heal_full();
    void heal_hp(uint16_t amount);
    void cure_status();
};

// Create a new Pokemon
Pokemon create_pokemon(SpeciesId species, uint8_t level, 
                       const struct Registries& reg);

// Create with specific DVs (for starters, gifts, etc.)
Pokemon create_pokemon(SpeciesId species, uint8_t level,
                       Pokemon::DVs dvs, const struct Registries& reg);

// Create wild Pokemon with random DVs
Pokemon create_wild_pokemon(SpeciesId species, uint8_t level,
                           const struct Registries& reg, uint32_t seed);

} // namespace enginemon
