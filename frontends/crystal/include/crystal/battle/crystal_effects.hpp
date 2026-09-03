#pragma once
// frontends/crystal/include/crystal/battle/crystal_effects.hpp
//
// Crystal Gen 2 move effect ID constants.
//
// Source: pokecrystal constants/battle_constants.asm — EFFECT_* enum values.
// These are Crystal-specific numeric identifiers that appear in the MoveData::effect_id
// field extracted from the ROM.  They are used by the Crystal frontend extractor
// and by the Crystal-facing AI categorization logic.
//
// SCOPE RULE: This header belongs exclusively to the Crystal frontend.
//   - frontends/crystal/**   — MAY include this
//   - engine/**              — MUST NOT include this
//   - runtime/**             — MUST NOT include this
//
// The engine AI layer (trainer_ai.cpp) uses only BattleRules lookup helpers
// (is_ai_status_only, is_ai_rain_dance_move, etc.) and a small set of
// semantic effect IDs defined in engine/include/engine/battle/battle_effects.hpp.

#include <cstdint>

namespace crystal {

// Crystal Gen 2 EFFECT_* constants (battle_constants.asm).
// These are the raw ROM byte values stored in MoveData::effect_id.
namespace EffectId {
    static constexpr uint8_t NORMAL_HIT         =   0;
    static constexpr uint8_t SLEEP              =   1;
    static constexpr uint8_t POISON             =   2;
    static constexpr uint8_t LEECH_HIT          =   3;
    static constexpr uint8_t BURN               =   5;
    static constexpr uint8_t FREEZE             =   6;
    static constexpr uint8_t PARALYZE           =   7;
    static constexpr uint8_t SELFDESTRUCT       =   8;
    static constexpr uint8_t DREAM_EATER        =   9;
    static constexpr uint8_t MIRROR_MOVE        =  10;
    static constexpr uint8_t ATTACK_UP         =  11;
    static constexpr uint8_t DEFENSE_UP        =  12;
    static constexpr uint8_t SPEED_UP          =  13;
    static constexpr uint8_t SPECIAL_UP        =  14;
    static constexpr uint8_t ACCURACY_UP       =  15;
    static constexpr uint8_t EVASION_UP        =  16;
    static constexpr uint8_t ALWAYS_HIT        =  17;
    static constexpr uint8_t ATTACK_DOWN       =  18;
    static constexpr uint8_t DEFENSE_DOWN      =  19;
    static constexpr uint8_t SPEED_DOWN        =  20;
    static constexpr uint8_t SPECIAL_DOWN      =  21;
    static constexpr uint8_t ACCURACY_DOWN     =  22;
    static constexpr uint8_t EVASION_DOWN      =  23;
    static constexpr uint8_t BIDE              =  27;
    static constexpr uint8_t FORCE_SWITCH      =  29;
    static constexpr uint8_t HEAL              =  33;
    static constexpr uint8_t TOXIC             =  34;
    static constexpr uint8_t LIGHT_SCREEN      =  36;
    static constexpr uint8_t OHKO              =  38;
    static constexpr uint8_t CONFUSE           =  48;
    static constexpr uint8_t SP_DEF_UP_2       =  53;
    static constexpr uint8_t REFLECT           =  66;
    static constexpr uint8_t SUBSTITUTE        =  68;
    static constexpr uint8_t HYPER_BEAM        =  69;
    static constexpr uint8_t LEECH_SEED        =  73;
    static constexpr uint8_t ATTACK_UP_2       =  74;
    static constexpr uint8_t DEFENSE_UP_2      =  75;
    static constexpr uint8_t SPEED_UP_2        =  76;
    static constexpr uint8_t SPECIAL_UP_2      =  77;
    static constexpr uint8_t ACCURACY_UP_2     =  78;
    static constexpr uint8_t EVASION_UP_2      =  79;
    static constexpr uint8_t ATTACK_DOWN_2     =  80;
    static constexpr uint8_t DEFENSE_DOWN_2    =  81;
    static constexpr uint8_t SPEED_DOWN_2      =  82;
    static constexpr uint8_t SPECIAL_DOWN_2    =  83;
    static constexpr uint8_t ACCURACY_DOWN_2   =  84;
    static constexpr uint8_t EVASION_DOWN_2    =  85;
    static constexpr uint8_t NIGHTMARE         =  92;
    static constexpr uint8_t BELLY_DRUM        =  97;
    static constexpr uint8_t SAFEGUARD         = 108;
    static constexpr uint8_t MORNING_SUN       = 112;
    static constexpr uint8_t SYNTHESIS         = 113;
    static constexpr uint8_t MOONLIGHT         = 114;
    static constexpr uint8_t RAIN_DANCE        = 120;
    static constexpr uint8_t SUNNY_DAY         = 121;
    static constexpr uint8_t PROTECT           = 122;
    static constexpr uint8_t BATON_PASS        = 133;
    static constexpr uint8_t ENDURE            = 135;

    // Stat-up range: ATTACK_UP(11)..EVASION_UP(16) and ATTACK_UP_2(74)..EVASION_UP_2(79)
    static constexpr uint8_t STAT_UP_MIN        =  11;  // ATTACK_UP
    static constexpr uint8_t STAT_UP_MAX        =  16;  // EVASION_UP
    static constexpr uint8_t STAT_UP2_MIN       =  74;  // ATTACK_UP_2
    static constexpr uint8_t STAT_UP2_MAX       =  79;  // EVASION_UP_2

    // Stat-down range: ATTACK_DOWN(18)..EVASION_DOWN(23) and ATTACK_DOWN_2(80)..EVASION_DOWN_2(85)
    static constexpr uint8_t STAT_DOWN_MIN      =  18;  // ATTACK_DOWN
    static constexpr uint8_t STAT_DOWN_MAX      =  23;  // EVASION_DOWN
    static constexpr uint8_t STAT_DOWN2_MIN     =  80;  // ATTACK_DOWN_2
    static constexpr uint8_t STAT_DOWN2_MAX     =  85;  // EVASION_DOWN_2

    // Heal effects used by AI_Smart
    // Source: scoring.asm AI_Smart_Heal / AI_Smart_MorningSun / AI_Smart_Synthesis / AI_Smart_Moonlight
    static constexpr uint8_t kHealEffects[]     = { HEAL, MORNING_SUN, SYNTHESIS, MOONLIGHT };

    // Sleep-synergy check: AI_Smart_Sleep checks if enemy has DreamEater or Nightmare
    static constexpr uint8_t kSleepSynergy[]    = { DREAM_EATER, NIGHTMARE };
}

// True if effect_id is a stat-up effect (AI_Setup uses this).
// Encodes Crystal's contiguous range layout — Crystal frontend only.
inline bool crystal_is_stat_up(uint8_t effect_id) {
    return (effect_id >= EffectId::STAT_UP_MIN   && effect_id <= EffectId::STAT_UP_MAX)
        || (effect_id >= EffectId::STAT_UP2_MIN  && effect_id <= EffectId::STAT_UP2_MAX);
}

// True if effect_id is a stat-down effect (AI_Setup uses this).
inline bool crystal_is_stat_down(uint8_t effect_id) {
    return (effect_id >= EffectId::STAT_DOWN_MIN  && effect_id <= EffectId::STAT_DOWN_MAX)
        || (effect_id >= EffectId::STAT_DOWN2_MIN && effect_id <= EffectId::STAT_DOWN2_MAX);
}

// True if effect_id is a heal effect (AI_Smart uses this).
inline bool crystal_is_heal_effect(uint8_t effect_id) {
    for (uint8_t e : EffectId::kHealEffects)
        if (e == effect_id) return true;
    return false;
}

// True if effect_id is a sleep-synergy effect (AI_Smart_Sleep: DreamEater or Nightmare).
inline bool crystal_is_sleep_synergy(uint8_t effect_id) {
    for (uint8_t e : EffectId::kSleepSynergy)
        if (e == effect_id) return true;
    return false;
}

} // namespace crystal
