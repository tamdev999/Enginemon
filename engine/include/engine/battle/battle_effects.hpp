#pragma once
// engine/include/engine/battle/battle_effects.hpp
//
// Semantic move effect identifiers used by the generic engine AI layer.
//
// These are the minimal set of effect IDs required by VanillaCrystalAI::ai_smart()
// and related engine scoring passes.  They happen to match Crystal's Gen 2
// EFFECT_* constants because the engine currently targets Crystal-derived packages,
// but they are considered EMON semantic IDs, not Crystal ABI constants.
//
// The Crystal source layout (stat-up ranges, stat-down ranges, etc.) is NOT
// expressed here.  Any Crystal-specific range assumptions belong exclusively in
// frontends/crystal/include/crystal/battle/crystal_effects.hpp.
//
// Extension: if a future frontend uses different numeric effect IDs, it must
// map them to these semantic IDs at the package boundary (SemanticScriptIR /
// FrozenGameData), just as MoveId is semantically mapped at extraction time.

#include <cstdint>

namespace enginemon {

// Semantic move effect IDs for engine AI dispatch.
// Numeric values match Crystal Gen 2 for vanilla packages.
// These are used ONLY by trainer_ai.cpp — not by battle formulas.
namespace SemEffect {
    // Status-inducing (also covered by BattleRules::ai_status_only_effects list)
    static constexpr uint8_t Sleep          =   1;
    static constexpr uint8_t Poison         =   2;
    static constexpr uint8_t Burn           =   5;
    static constexpr uint8_t Freeze         =   6;
    static constexpr uint8_t Paralyze       =   7;
    static constexpr uint8_t Toxic          =  34;
    static constexpr uint8_t Confuse        =  48;
    static constexpr uint8_t LeechSeed      =  73;
    static constexpr uint8_t Nightmare      =  92;

    // Damage/misc with specific AI dispatch logic
    static constexpr uint8_t DreamEater     =   9;
    static constexpr uint8_t Selfdestruct   =   8;
    static constexpr uint8_t HyperBeam      =  69;

    // Healing (used by ai_smart heal handler)
    static constexpr uint8_t Heal           =  33;
    static constexpr uint8_t MorningSun     = 112;
    static constexpr uint8_t Synthesis      = 113;
    static constexpr uint8_t Moonlight      = 114;

    // Utility effects with specific AI logic
    static constexpr uint8_t BatonPass      = 133;
    static constexpr uint8_t BellyDrum      =  97;
    static constexpr uint8_t Protect        = 122;
    static constexpr uint8_t Endure         = 135;
    static constexpr uint8_t Reflect        =  66;
    static constexpr uint8_t LightScreen    =  36;

    // Weather (synergy checked via BattleRules::is_ai_rain/sunny_day_move())
    static constexpr uint8_t RainDance      = 120;
    static constexpr uint8_t SunnyDay       = 121;
}

} // namespace enginemon
