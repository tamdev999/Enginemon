#pragma once
// engine/core/registry.hpp
// Type-safe registries for game data
// Populated at startup from GameDefinition, frozen before gameplay

#include "engine/core/types.hpp"
#include <unordered_map>
#include <memory>
#include <stdexcept>
#include <functional>

namespace enginemon {

// Registry lookup failure
class RegistryError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Generic registry template
template<typename Id, typename Data>
class Registry {
public:
    // Registration (mutable phase only)
    void register_entry(Id id, Data data) {
        if (frozen_) {
            throw RegistryError("Cannot modify frozen registry");
        }
        entries_[id] = std::move(data);
    }
    
    // Lookup
    const Data* get(Id id) const {
        auto it = entries_.find(id);
        return it != entries_.end() ? &it->second : nullptr;
    }
    
    const Data& require(Id id) const {
        auto* data = get(id);
        if (!data) {
            throw RegistryError("Registry entry not found");
        }
        return *data;
    }
    
    // Iteration
    auto begin() const { return entries_.begin(); }
    auto end() const { return entries_.end(); }
    size_t size() const { return entries_.size(); }
    
    // Freeze (call after all mods applied, before gameplay)
    void freeze() { frozen_ = true; }
    bool is_frozen() const { return frozen_; }
    
    // For mod composition: allows mods to patch/replace entries
    void patch(Id id, std::function<void(Data&)> patcher) {
        if (frozen_) {
            throw RegistryError("Cannot modify frozen registry");
        }
        auto it = entries_.find(id);
        if (it != entries_.end()) {
            patcher(it->second);
        }
    }
    
    // For mod composition: remove entry (rare, but supported)
    void remove(Id id) {
        if (frozen_) {
            throw RegistryError("Cannot modify frozen registry");
        }
        entries_.erase(id);
    }

private:
    std::unordered_map<Id, Data> entries_;
    bool frozen_ = false;
};

// Type effectiveness lookup (separate from registry pattern)
class TypeChart {
public:
    // Initialize all type matchups to neutral (10) by default
    TypeChart() {
        // Fill entire chart with neutral effectiveness (10)
        // This ensures unset pairs return neutral, not immune
        for (auto& row : chart_) {
            row.fill(10);
        }
    }
    
    void set_effectiveness(TypeId attacking, TypeId defending, uint8_t multiplier);
    
    // Returns multiplier: 0=immune, 5=resist, 10=normal, 20=super
    // NO translation: returns exactly what was stored (or neutral 10 if never set)
    uint8_t get_effectiveness(TypeId attacking, TypeId defending) const;
    
    // Combined effectiveness for dual types
    uint8_t get_effectiveness(TypeId attacking, TypeId def1, TypeId def2) const;
    
    void freeze() { frozen_ = true; }
    bool is_frozen() const { return frozen_; }

private:
    // 18x18 is plenty for Gen 2's 17 types plus room for mods
    static constexpr size_t MAX_TYPES = 32;
    std::array<std::array<uint8_t, MAX_TYPES>, MAX_TYPES> chart_{};
    bool frozen_ = false;
};

// All game registries
struct Registries {
    Registry<SpeciesId, SpeciesData> species;
    Registry<MoveId, MoveData> moves;
    Registry<ItemId, ItemData> items;
    Registry<TypeId, TypeData> types;
    Registry<TrainerId, TrainerData> trainers;
    TypeChart type_chart;
    
    // Extensible behavior registries (for AI, effects, etc.)
    // These allow mods to register new implementations
    // See: battle/trainer_ai.hpp for AIRegistry
    
    // Freeze all registries (call after mod composition, before gameplay)
    void freeze_all() {
        species.freeze();
        moves.freeze();
        items.freeze();
        types.freeze();
        trainers.freeze();
        type_chart.freeze();
    }
};

} // namespace enginemon
