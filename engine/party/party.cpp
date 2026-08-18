// engine/party/party.cpp
// Party management implementation

#include "engine/party/party.hpp"
#include "engine/core/registry.hpp"
#include <algorithm>
#include <cassert>
#include <stdexcept>

namespace enginemon {

// =============================================================================
// Party Implementation
// =============================================================================

Party::Party() : count_(0) {
    // Initialize with default-constructed Pokemon
}

Pokemon& Party::operator[](size_t index) {
    return pokemon_[index];
}

const Pokemon& Party::operator[](size_t index) const {
    return pokemon_[index];
}

Pokemon* Party::get(size_t index) {
    if (index >= count_) return nullptr;
    return &pokemon_[index];
}

const Pokemon* Party::get(size_t index) const {
    if (index >= count_) return nullptr;
    return &pokemon_[index];
}

bool Party::add(Pokemon pokemon) {
    if (count_ >= MAX_PARTY_SIZE) return false;
    pokemon_[count_++] = std::move(pokemon);
    return true;
}

bool Party::remove(size_t index) {
    if (index >= count_) return false;
    
    // Shift remaining Pokemon down
    for (size_t i = index; i + 1 < count_; ++i) {
        pokemon_[i] = std::move(pokemon_[i + 1]);
    }
    --count_;
    return true;
}

void Party::swap(size_t a, size_t b) {
    if (a >= count_ || b >= count_) return;
    std::swap(pokemon_[a], pokemon_[b]);
}

bool Party::has_species(SpeciesId species) const {
    for (size_t i = 0; i < count_; ++i) {
        if (pokemon_[i].species == species) return true;
    }
    return false;
}

bool Party::has_usable_pokemon() const {
    for (size_t i = 0; i < count_; ++i) {
        if (!pokemon_[i].is_egg && pokemon_[i].current_hp > 0) {
            return true;
        }
    }
    return false;
}

size_t Party::first_usable() const {
    for (size_t i = 0; i < count_; ++i) {
        if (!pokemon_[i].is_egg && pokemon_[i].current_hp > 0) {
            return i;
        }
    }
    return MAX_PARTY_SIZE; // Invalid index
}

bool Party::all_fainted() const {
    for (size_t i = 0; i < count_; ++i) {
        // Eggs don't count for the fainted check
        if (!pokemon_[i].is_egg && pokemon_[i].current_hp > 0) {
            return false;
        }
    }
    return true;
}

std::optional<size_t> Party::find_with_move(MoveId move) const {
    for (size_t i = 0; i < count_; ++i) {
        for (const auto& m : pokemon_[i].moves) {
            if (m.id == move) return i;
        }
    }
    return std::nullopt;
}

// =============================================================================
// HEAL_ALL - Source-proven contract from pokecrystal/engine/pokemon/health.asm
// =============================================================================
// HealParty iterates all party slots:
//   - Eggs are SKIPPED (cp EGG / jr z, .next)
//   - Non-eggs call HealPartyMon which:
//     1. Clears both status bytes to 0
//     2. Sets current HP to max HP (revives fainted mons)
//     3. Calls RestoreAllPP to restore all move PP
//
// PP restoration formula (from Gen2Recomped):
//   max_pp = base_pp + (base_pp / 5) * pp_ups
//
// DOES NOT modify: DVs, statExp, friendship, level, held item, Pokérus, met info
// DOES NOT produce script result (wScriptVar unchanged)
// =============================================================================

void Party::heal_all(const Registry<MoveId, MoveData>& moves) {
    for (size_t i = 0; i < count_; ++i) {
        // Eggs are SKIPPED per source: cp EGG / jr z, .next
        if (pokemon_[i].is_egg) continue;
        
        heal_at(i, moves);
    }
}

void Party::heal_at(size_t index, const Registry<MoveId, MoveData>& moves) {
    if (index >= count_) return;
    
    Pokemon& mon = pokemon_[index];
    
    // Eggs are skipped - caller should check, but guard here too
    if (mon.is_egg) return;
    
    // 1. Restore HP to max (revives fainted mons)
    mon.current_hp = mon.max_hp;
    
    // 2. Clear status
    mon.status = Status::None;
    mon.status_data = 0;
    
    // 3. Restore PP for all moves using authoritative move definitions
    // PP formula: max_pp = base_pp + (base_pp / 5) * pp_ups
    for (size_t slot = 0; slot < mon.moves.size(); ++slot) {
        auto& move = mon.moves[slot];
        if (move.id == MOVE_NONE) continue;
        
        // Look up move definition for base PP
        const MoveData* move_data = moves.get(move.id);
        if (move_data) {
            // max_pp = base_pp + (base_pp / 5) * pp_ups
            uint8_t base_pp = move_data->pp;
            uint8_t max_pp = base_pp + (base_pp / 5) * move.pp_ups;
            move.pp = max_pp;
            // pp_ups is PRESERVED - we don't touch it
        }
    }
}

uint32_t Party::total_level() const {
    uint32_t total = 0;
    for (size_t i = 0; i < count_; ++i) {
        total += pokemon_[i].level;
    }
    return total;
}

// =============================================================================
// PCBox Implementation
// =============================================================================

PCBox::PCBox() : name_("BOX"), count_(0) {}
PCBox::PCBox(const std::string& name) : name_(name), count_(0) {}

Pokemon* PCBox::get(size_t slot) {
    if (slot >= BOX_SIZE || !pokemon_[slot].has_value()) return nullptr;
    return &pokemon_[slot].value();
}

const Pokemon* PCBox::get(size_t slot) const {
    if (slot >= BOX_SIZE || !pokemon_[slot].has_value()) return nullptr;
    return &pokemon_[slot].value();
}

bool PCBox::deposit(size_t slot, Pokemon pokemon) {
    if (slot >= BOX_SIZE || pokemon_[slot].has_value()) return false;
    pokemon_[slot] = std::move(pokemon);
    ++count_;
    return true;
}

std::optional<Pokemon> PCBox::withdraw(size_t slot) {
    if (slot >= BOX_SIZE || !pokemon_[slot].has_value()) return std::nullopt;
    auto mon = std::move(pokemon_[slot].value());
    pokemon_[slot] = std::nullopt;
    --count_;
    return mon;
}

// =============================================================================
// PCStorage Implementation
// =============================================================================

PCStorage::PCStorage() : current_box_(0) {
    // Initialize box names
    for (size_t i = 0; i < NUM_BOXES; ++i) {
        boxes_[i].set_name("BOX " + std::to_string(i + 1));
    }
}

PCBox& PCStorage::box(size_t index) {
    // AUDIT 12: Programmer error must not silently return wrong object
    // Invalid index is a programming error, not a user input issue
    assert(index < NUM_BOXES && "PCStorage::box() index out of bounds");
    if (index >= NUM_BOXES) {
        throw std::out_of_range("PCStorage::box() index out of bounds: " + std::to_string(index));
    }
    return boxes_[index];
}

const PCBox& PCStorage::box(size_t index) const {
    // AUDIT 12: Programmer error must not silently return wrong object
    assert(index < NUM_BOXES && "PCStorage::box() index out of bounds");
    if (index >= NUM_BOXES) {
        throw std::out_of_range("PCStorage::box() index out of bounds: " + std::to_string(index));
    }
    return boxes_[index];
}

void PCStorage::set_current_box(size_t index) {
    if (index < NUM_BOXES) {
        current_box_ = index;
    }
}

bool PCStorage::deposit(Pokemon pokemon) {
    // Find first empty slot in current box BEFORE moving the Pokemon
    // This prevents the repeated-move bug where we might move-from the Pokemon
    // multiple times while probing occupied slots
    for (size_t i = 0; i < PCBox::BOX_SIZE; ++i) {
        if (!boxes_[current_box_].get(i)) {
            // Found empty slot - deposit exactly once
            return boxes_[current_box_].deposit(i, std::move(pokemon));
        }
    }
    return false; // Box is full
}

std::optional<std::pair<size_t, size_t>> PCStorage::find_empty_slot() const {
    for (size_t b = 0; b < NUM_BOXES; ++b) {
        for (size_t s = 0; s < PCBox::BOX_SIZE; ++s) {
            if (!boxes_[b].get(s)) {
                return std::make_pair(b, s);
            }
        }
    }
    return std::nullopt;
}

size_t PCStorage::total_stored() const {
    size_t total = 0;
    for (const auto& box : boxes_) {
        total += box.count();
    }
    return total;
}

} // namespace enginemon
