// engine/party/pokemon.cpp
// Pokemon instance implementation

#include "engine/party/pokemon.hpp"
#include "engine/core/registry.hpp"
#include "engine/core/game_state.hpp"  // For RngState

namespace enginemon {

// =============================================================================
// PP HELPERS - Source-proven from pokecrystal/Gen2Recomped
// =============================================================================
// PP byte format in Gen 2 stores current PP (0-63) and PP Ups (0-3) separately
// in Enginemon's Move struct (pp and pp_ups fields).
//
// PP restoration formula (Gen2Recomped Pokemon.lua):
//   max_pp = base_pp + (base_pp / 5) * pp_ups
//
// This matches pokecrystal RestoreBonusPP which adds maxPP/5 per PP UP.
// =============================================================================

uint8_t Pokemon::max_pp(size_t move_slot, const MoveData& move_data) const {
    if (move_slot >= moves.size() || moves[move_slot].id == MOVE_NONE) {
        return 0;
    }
    
    uint8_t base_pp = move_data.pp;
    uint8_t pp_ups = moves[move_slot].pp_ups;
    
    // max_pp = base_pp + (base_pp / 5) * pp_ups
    // Each PP Up adds 20% (base_pp/5) to the max
    return base_pp + (base_pp / 5) * pp_ups;
}

void Pokemon::restore_pp(size_t move_slot, const MoveData& move_data) {
    if (move_slot >= moves.size() || moves[move_slot].id == MOVE_NONE) {
        return;
    }
    
    // Restore to computed max, preserving PP Ups
    moves[move_slot].pp = max_pp(move_slot, move_data);
}

void Pokemon::restore_all_pp(const Registries& reg) {
    for (size_t i = 0; i < moves.size(); ++i) {
        if (moves[i].id == MOVE_NONE) continue;
        
        const MoveData* move_data = reg.moves.get(moves[i].id);
        if (move_data) {
            restore_pp(i, *move_data);
        }
    }
}

// =============================================================================
// HEAL HELPERS
// =============================================================================

void Pokemon::heal_full() {
    current_hp = max_hp;
    cure_status();
}

void Pokemon::heal_hp(uint16_t amount) {
    current_hp = std::min(static_cast<uint16_t>(current_hp + amount), max_hp);
}

void Pokemon::cure_status() {
    status = Status::None;
    status_data = 0;
}

// =============================================================================
// COMPUTED PROPERTIES
// =============================================================================

bool Pokemon::is_shiny() const {
    // Gen 2 shiny check: Attack DV = 2, 3, 6, 7, 10, 11, 14, 15 (has bit 1 set)
    // AND Defense, Speed, Special all = 10
    bool attack_valid = (dvs.attack & 0x02) != 0; // bit 1 set
    bool others_10 = (dvs.defense == 10) && (dvs.speed == 10) && (dvs.special == 10);
    return attack_valid && others_10;
}

uint8_t Pokemon::gender() const {
    // Gen 2 gender is determined by Attack DV vs species gender ratio
    // For now, return genderless (2) - full implementation needs species data
    return 2;
}

uint8_t Pokemon::hp_dv() const {
    // HP DV is derived from the low bits of other DVs
    // HP DV = (Attack & 1) << 3 | (Defense & 1) << 2 | (Speed & 1) << 1 | (Special & 1)
    return ((dvs.attack & 1) << 3) | 
           ((dvs.defense & 1) << 2) | 
           ((dvs.speed & 1) << 1) | 
           (dvs.special & 1);
}

void Pokemon::recalculate_stats(const SpeciesData& species_data) {
    // Gen 2 stat formula:
    // HP = ((Base + DV) * 2 + sqrt(StatExp)/4) * Level / 100 + Level + 10
    // Other = ((Base + DV) * 2 + sqrt(StatExp)/4) * Level / 100 + 5
    
    auto calc_stat = [this](uint8_t base, uint8_t dv, uint16_t sexp, bool is_hp) -> uint16_t {
        // sqrt(StatExp)/4, capped at 63 contribution
        uint32_t sqrt_sexp = 0;
        if (sexp > 0) {
            // Integer square root approximation
            uint32_t x = sexp;
            uint32_t y = (x + 1) / 2;
            while (y < x) {
                x = y;
                y = (x + sexp / x) / 2;
            }
            sqrt_sexp = x / 4;
            if (sqrt_sexp > 63) sqrt_sexp = 63;
        }
        
        uint32_t stat = (base + dv) * 2 + sqrt_sexp;
        stat = stat * level / 100;
        stat += is_hp ? (level + 10) : 5;
        
        return static_cast<uint16_t>(std::min(stat, 999u));
    };
    
    uint16_t old_max_hp = max_hp;
    
    max_hp = calc_stat(species_data.base_stats.hp, hp_dv(), stat_exp.hp, true);
    attack = calc_stat(species_data.base_stats.attack, dvs.attack, stat_exp.attack, false);
    defense = calc_stat(species_data.base_stats.defense, dvs.defense, stat_exp.defense, false);
    speed = calc_stat(species_data.base_stats.speed, dvs.speed, stat_exp.speed, false);
    special_attack = calc_stat(species_data.base_stats.special_attack, dvs.special, stat_exp.special, false);
    special_defense = calc_stat(species_data.base_stats.special_defense, dvs.special, stat_exp.special, false);
    
    // Adjust current HP proportionally if max changed
    if (old_max_hp > 0 && max_hp != old_max_hp) {
        current_hp = static_cast<uint16_t>(
            static_cast<uint32_t>(current_hp) * max_hp / old_max_hp
        );
    }
}

// =============================================================================
// POKEMON CREATION
// =============================================================================

Pokemon create_pokemon(SpeciesId species, uint8_t level, RngState& rng, const Registries& reg) {
    // Draw DVs directly from the canonical GameState RNG.
    // Each call to rng.next() advances the authoritative RNG state.
    // This ensures:
    //   - Deterministic simulation (same sequence → same Pokemon)
    //   - Proper save/load (RNG state is part of GameState)
    //   - Multiplayer compatibility (synchronized RNG)
    //
    // NO private RNG streams. NO std::mt19937. All randomness from GameState.
    
    Pokemon::DVs dvs;
    dvs.attack = static_cast<uint8_t>(rng.next() & 0x0F);   // 0-15
    dvs.defense = static_cast<uint8_t>(rng.next() & 0x0F);
    dvs.speed = static_cast<uint8_t>(rng.next() & 0x0F);
    dvs.special = static_cast<uint8_t>(rng.next() & 0x0F);
    
    return create_pokemon(species, level, dvs, reg);
}

Pokemon create_pokemon(SpeciesId species, uint8_t level, Pokemon::DVs dvs, const Registries& reg) {
    Pokemon mon;
    mon.species = species;
    mon.level = level;
    mon.dvs = dvs;
    
    // Zero stat exp for new Pokemon
    mon.stat_exp = {0, 0, 0, 0, 0};
    
    // Get species data for stats and moves
    const SpeciesData* sp_data = reg.species.get(species);
    if (sp_data) {
        mon.recalculate_stats(*sp_data);
        mon.current_hp = mon.max_hp;
        
        // TODO: Initial moves from learnset
    }
    
    mon.status = Status::None;
    mon.status_data = 0;
    mon.held_item = ITEM_NONE;
    mon.friendship = 70; // Base friendship
    mon.pokerus = 0;
    mon.is_egg = false;
    mon.egg_cycles = 0;
    mon.personality = 0; // Would be random in real implementation
    mon.ot_id = 0;
    mon.exp = 0; // Would calculate from level
    
    return mon;
}

Pokemon create_wild_pokemon(SpeciesId species, uint8_t level, const Registries& reg, RngState& rng) {
    // Draw DVs directly from canonical RNG - no private substreams
    Pokemon::DVs dvs;
    dvs.attack = static_cast<uint8_t>(rng.next() & 0x0F);
    dvs.defense = static_cast<uint8_t>(rng.next() & 0x0F);
    dvs.speed = static_cast<uint8_t>(rng.next() & 0x0F);
    dvs.special = static_cast<uint8_t>(rng.next() & 0x0F);
    
    return create_pokemon(species, level, dvs, reg);
}

} // namespace enginemon
