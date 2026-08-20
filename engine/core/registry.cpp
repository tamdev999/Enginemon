// engine/core/registry.cpp
// Registry implementation

#include "engine/core/registry.hpp"

namespace enginemon {

void TypeChart::set_effectiveness(TypeId attacking, TypeId defending, uint8_t multiplier) {
    if (frozen_) {
        throw RegistryError("Cannot modify frozen type chart");
    }
    if (attacking >= MAX_TYPES || defending >= MAX_TYPES) {
        throw std::out_of_range(
            "TypeChart::set_effectiveness: TypeId out of range (max " +
            std::to_string(MAX_TYPES - 1) + ")");
    }
    chart_[attacking][defending] = multiplier;
}

uint8_t TypeChart::get_effectiveness(TypeId attacking, TypeId defending) const {
    if (attacking >= MAX_TYPES || defending >= MAX_TYPES) {
        throw std::out_of_range(
            "TypeChart::get_effectiveness: TypeId out of range (max " +
            std::to_string(MAX_TYPES - 1) + ")");
    }
    // Return exactly what was stored - chart is pre-filled with 10 (neutral)
    // 0 means immune (explicitly set), not "unset"
    return chart_[attacking][defending];
}

uint8_t TypeChart::get_effectiveness(TypeId attacking, TypeId def1, TypeId def2) const {
    if (def2 == TYPE_NONE) {
        return get_effectiveness(attacking, def1);
    }
    
    uint8_t eff1 = get_effectiveness(attacking, def1);
    uint8_t eff2 = get_effectiveness(attacking, def2);
    
    // Immunity takes priority: if either type is immune, result is immune
    // 0 * anything = 0
    if (eff1 == 0 || eff2 == 0) return 0;
    
    // Multiply: 10*10=100 (normal), 20*10=200 (2x), 20*20=400 (4x)
    // 5*10=50 (0.5x), 5*5=25 (0.25x)
    return (eff1 * eff2) / 10;
}

} // namespace enginemon
