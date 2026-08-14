#pragma once
// engine/party/party.hpp
// Player's party (up to 6 Pokemon)

#include "engine/party/pokemon.hpp"
#include <array>
#include <optional>
#include <vector>

namespace enginemon {

// Maximum party size
constexpr size_t MAX_PARTY_SIZE = 6;

// Party management
class Party {
public:
    Party();
    
    // Access
    size_t size() const { return count_; }
    bool empty() const { return count_ == 0; }
    bool full() const { return count_ >= MAX_PARTY_SIZE; }
    
    Pokemon& operator[](size_t index);
    const Pokemon& operator[](size_t index) const;
    
    Pokemon* get(size_t index);
    const Pokemon* get(size_t index) const;
    
    // Iteration
    auto begin() { return pokemon_.begin(); }
    auto end() { return pokemon_.begin() + count_; }
    auto begin() const { return pokemon_.begin(); }
    auto end() const { return pokemon_.begin() + count_; }
    
    // Modification
    bool add(Pokemon pokemon);
    bool remove(size_t index);
    void swap(size_t a, size_t b);
    
    // Queries
    bool has_species(SpeciesId species) const;
    bool has_usable_pokemon() const;  // At least one not fainted
    size_t first_usable() const;      // First non-fainted
    bool all_fainted() const;
    
    // Find Pokemon with specific move (for HM checks)
    std::optional<size_t> find_with_move(MoveId move) const;
    
    // Healing
    void heal_all();
    void heal_at(size_t index);
    
    // Total level (for some calculations)
    uint32_t total_level() const;

private:
    std::array<Pokemon, MAX_PARTY_SIZE> pokemon_;
    size_t count_ = 0;
};

// PC storage box
class PCBox {
public:
    static constexpr size_t BOX_SIZE = 20;
    
    PCBox();
    explicit PCBox(const std::string& name);
    
    const std::string& name() const { return name_; }
    void set_name(const std::string& name) { name_ = name; }
    
    size_t count() const { return count_; }
    bool empty() const { return count_ == 0; }
    bool full() const { return count_ >= BOX_SIZE; }
    
    Pokemon* get(size_t slot);
    const Pokemon* get(size_t slot) const;
    
    bool deposit(size_t slot, Pokemon pokemon);
    std::optional<Pokemon> withdraw(size_t slot);
    
private:
    std::string name_;
    std::array<std::optional<Pokemon>, BOX_SIZE> pokemon_;
    size_t count_ = 0;
};

// Complete PC storage
class PCStorage {
public:
    static constexpr size_t NUM_BOXES = 14;  // Crystal has 14 boxes
    
    PCStorage();
    
    PCBox& box(size_t index);
    const PCBox& box(size_t index) const;
    
    size_t current_box() const { return current_box_; }
    void set_current_box(size_t index);
    
    // Convenience: deposit to current box
    bool deposit(Pokemon pokemon);
    
    // Find first empty slot across all boxes
    std::optional<std::pair<size_t, size_t>> find_empty_slot() const;
    
    // Total Pokemon stored
    size_t total_stored() const;

private:
    std::array<PCBox, NUM_BOXES> boxes_;
    size_t current_box_ = 0;
};

} // namespace enginemon
