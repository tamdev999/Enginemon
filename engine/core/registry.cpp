// engine/core/registry.cpp
// Registry implementation

#include "engine/core/registry.hpp"

namespace enginemon {

void TypeChart::set_effectiveness(TypeId attacking, TypeId defending, uint8_t multiplier) {
    if (frozen_) {
        throw RegistryError("Cannot modify frozen type chart");
    }
    if (attacking < MAX_TYPES && defending < MAX_TYPES) {
        chart_[attacking][defending] = multiplier;
    }
}

uint8_t TypeChart::get_effectiveness(TypeId attacking, TypeId defending) const {
    if (attacking >= MAX_TYPES || defending >= MAX_TYPES) {
        return 10; // Normal effectiveness as fallback
    }
    uint8_t eff = chart_[attacking][defending];
    return eff == 0 ? 10 : eff; // 0 in chart means not set, default to normal
}

uint8_t TypeChart::get_effectiveness(TypeId attacking, TypeId def1, TypeId def2) const {
    if (def2 == TYPE_NONE) {
        return get_effectiveness(attacking, def1);
    }
    
    uint8_t eff1 = get_effectiveness(attacking, def1);
    uint8_t eff2 = get_effectiveness(attacking, def2);
    
    // Multiply: 10*10=100 (normal), 20*10=200 (2x), 20*20=400 (4x)
    // 0 * anything = 0 (immune)
    // 5*10=50 (0.5x), 5*5=25 (0.25x)
    if (eff1 == 0 || eff2 == 0) return 0;
    return (eff1 * eff2) / 10;
}

} // namespace enginemon
