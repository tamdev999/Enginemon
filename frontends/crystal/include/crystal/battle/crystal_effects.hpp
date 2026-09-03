#pragma once
// frontends/crystal/include/crystal/battle/crystal_effects.hpp
//
// Crystal Gen 2 move effect ID constants AND the mapping to EMON semantic EffectIds.
//
// Source authority: pokecrystal/constants/move_effect_constants.asm
//   const_def starts at 0; each `const` line increments by 1.
//   All values here have been verified against that enumeration.
//
// SCOPE RULE: This header belongs exclusively to the Crystal frontend.
//   - frontends/crystal/**   — MAY include this
//   - engine/**              — MUST NOT include this
//   - runtime/**             — MUST NOT include this
//
// The engine works only with EMON semantic EffectId values (engine/core/types.hpp).
// This file contains:
//   (1) crystal::EffectId:: — correct Crystal raw byte values for documentation/extraction
//   (2) crystal::to_semantic_effect() — maps Crystal raw bytes to EMON SemEffect:: values

#include "engine/core/types.hpp"  // for EffectId and SemEffect::

namespace crystal {

// Crystal Gen 2 EFFECT_* constants.
// Source: pokecrystal/constants/move_effect_constants.asm (const_def enumeration, 0-indexed)
// DO NOT use these values in engine code. Use engine/core/types.hpp SemEffect:: instead.
namespace EffectId {
    // Gen 1 core effects
    static constexpr uint8_t NORMAL_HIT         =   0;
    static constexpr uint8_t SLEEP              =   1;
    static constexpr uint8_t POISON_HIT         =   2;  // poison secondary on hit
    static constexpr uint8_t LEECH_HIT          =   3;
    static constexpr uint8_t BURN_HIT           =   4;  // burn secondary on hit
    static constexpr uint8_t FREEZE_HIT         =   5;  // freeze secondary on hit
    static constexpr uint8_t PARALYZE_HIT       =   6;  // paralyze secondary on hit
    static constexpr uint8_t SELFDESTRUCT       =   7;
    static constexpr uint8_t DREAM_EATER        =   8;
    static constexpr uint8_t MIRROR_MOVE        =   9;
    static constexpr uint8_t ATTACK_UP          =  10;
    static constexpr uint8_t DEFENSE_UP         =  11;
    static constexpr uint8_t SPEED_UP           =  12;
    static constexpr uint8_t SP_ATK_UP          =  13;
    static constexpr uint8_t SP_DEF_UP          =  14;
    static constexpr uint8_t ACCURACY_UP        =  15;
    static constexpr uint8_t EVASION_UP         =  16;
    static constexpr uint8_t ALWAYS_HIT         =  17;
    static constexpr uint8_t ATTACK_DOWN        =  18;
    static constexpr uint8_t DEFENSE_DOWN       =  19;
    static constexpr uint8_t SPEED_DOWN         =  20;
    static constexpr uint8_t SP_ATK_DOWN        =  21;
    static constexpr uint8_t SP_DEF_DOWN        =  22;
    static constexpr uint8_t ACCURACY_DOWN      =  23;
    static constexpr uint8_t EVASION_DOWN       =  24;
    static constexpr uint8_t RESET_STATS        =  25;
    static constexpr uint8_t BIDE               =  26;
    static constexpr uint8_t RAMPAGE            =  27;
    static constexpr uint8_t FORCE_SWITCH       =  28;
    static constexpr uint8_t MULTI_HIT          =  29;
    static constexpr uint8_t CONVERSION         =  30;
    static constexpr uint8_t FLINCH_HIT         =  31;
    static constexpr uint8_t HEAL               =  32;  // Recover / Softboiled / Rest
    static constexpr uint8_t TOXIC              =  33;
    static constexpr uint8_t PAY_DAY            =  34;
    static constexpr uint8_t LIGHT_SCREEN       =  35;
    static constexpr uint8_t TRI_ATTACK         =  36;
    static constexpr uint8_t UNUSED_25          =  37;
    static constexpr uint8_t OHKO               =  38;
    static constexpr uint8_t RAZOR_WIND         =  39;
    static constexpr uint8_t SUPER_FANG         =  40;
    static constexpr uint8_t STATIC_DAMAGE      =  41;
    static constexpr uint8_t TRAP_TARGET        =  42;
    static constexpr uint8_t UNUSED_2B          =  43;
    static constexpr uint8_t DOUBLE_HIT         =  44;
    static constexpr uint8_t JUMP_KICK          =  45;
    static constexpr uint8_t MIST               =  46;
    static constexpr uint8_t FOCUS_ENERGY       =  47;
    static constexpr uint8_t RECOIL_HIT         =  48;
    static constexpr uint8_t CONFUSE            =  49;
    // Gen 1 stage ×2 effects
    static constexpr uint8_t ATTACK_UP_2        =  50;
    static constexpr uint8_t DEFENSE_UP_2       =  51;
    static constexpr uint8_t SPEED_UP_2         =  52;
    static constexpr uint8_t SP_ATK_UP_2        =  53;
    static constexpr uint8_t SP_DEF_UP_2        =  54;
    static constexpr uint8_t ACCURACY_UP_2      =  55;
    static constexpr uint8_t EVASION_UP_2       =  56;
    static constexpr uint8_t TRANSFORM          =  57;
    static constexpr uint8_t ATTACK_DOWN_2      =  58;
    static constexpr uint8_t DEFENSE_DOWN_2     =  59;
    static constexpr uint8_t SPEED_DOWN_2       =  60;
    static constexpr uint8_t SP_ATK_DOWN_2      =  61;
    static constexpr uint8_t SP_DEF_DOWN_2      =  62;
    static constexpr uint8_t ACCURACY_DOWN_2    =  63;
    static constexpr uint8_t EVASION_DOWN_2     =  64;
    // Gen 1 continued
    static constexpr uint8_t REFLECT            =  65;
    static constexpr uint8_t POISON             =  66;  // standalone poison (Poison Powder)
    static constexpr uint8_t PARALYZE           =  67;  // standalone paralyze (Stun Spore)
    static constexpr uint8_t ATTACK_DOWN_HIT    =  68;
    static constexpr uint8_t DEFENSE_DOWN_HIT   =  69;
    static constexpr uint8_t SPEED_DOWN_HIT     =  70;
    static constexpr uint8_t SP_ATK_DOWN_HIT    =  71;
    static constexpr uint8_t SP_DEF_DOWN_HIT    =  72;
    static constexpr uint8_t ACCURACY_DOWN_HIT  =  73;
    static constexpr uint8_t EVASION_DOWN_HIT   =  74;
    static constexpr uint8_t SKY_ATTACK         =  75;
    static constexpr uint8_t CONFUSE_HIT        =  76;
    static constexpr uint8_t POISON_MULTI_HIT   =  77;
    static constexpr uint8_t UNUSED_4E          =  78;
    static constexpr uint8_t SUBSTITUTE        =  79;
    static constexpr uint8_t HYPER_BEAM         =  80;
    static constexpr uint8_t RAGE               =  81;
    static constexpr uint8_t MIMIC              =  82;
    static constexpr uint8_t METRONOME          =  83;
    static constexpr uint8_t LEECH_SEED         =  84;
    static constexpr uint8_t SPLASH             =  85;
    static constexpr uint8_t DISABLE            =  86;
    static constexpr uint8_t LEVEL_DAMAGE       =  87;
    static constexpr uint8_t PSYWAVE            =  88;
    static constexpr uint8_t COUNTER            =  89;
    static constexpr uint8_t ENCORE             =  90;
    static constexpr uint8_t PAIN_SPLIT         =  91;
    static constexpr uint8_t SNORE              =  92;
    static constexpr uint8_t CONVERSION2        =  93;
    static constexpr uint8_t LOCK_ON            =  94;
    static constexpr uint8_t SKETCH             =  95;
    static constexpr uint8_t DEFROST_OPPONENT   =  96;
    static constexpr uint8_t SLEEP_TALK         =  97;
    static constexpr uint8_t DESTINY_BOND       =  98;
    static constexpr uint8_t REVERSAL           =  99;
    static constexpr uint8_t SPITE              = 100;
    static constexpr uint8_t FALSE_SWIPE        = 101;
    static constexpr uint8_t HEAL_BELL          = 102;
    static constexpr uint8_t PRIORITY_HIT       = 103;
    static constexpr uint8_t TRIPLE_KICK        = 104;
    static constexpr uint8_t THIEF              = 105;
    static constexpr uint8_t MEAN_LOOK          = 106;
    static constexpr uint8_t NIGHTMARE          = 107;
    static constexpr uint8_t FLAME_WHEEL        = 108;
    static constexpr uint8_t CURSE              = 109;
    static constexpr uint8_t UNUSED_6E          = 110;
    static constexpr uint8_t PROTECT            = 111;
    static constexpr uint8_t SPIKES             = 112;
    static constexpr uint8_t FORESIGHT          = 113;
    static constexpr uint8_t PERISH_SONG        = 114;
    static constexpr uint8_t SANDSTORM          = 115;
    static constexpr uint8_t ENDURE             = 116;
    static constexpr uint8_t ROLLOUT            = 117;
    static constexpr uint8_t SWAGGER            = 118;
    static constexpr uint8_t FURY_CUTTER        = 119;
    static constexpr uint8_t ATTRACT            = 120;
    static constexpr uint8_t RETURN             = 121;
    static constexpr uint8_t PRESENT            = 122;
    static constexpr uint8_t FRUSTRATION        = 123;
    static constexpr uint8_t SAFEGUARD          = 124;
    static constexpr uint8_t SACRED_FIRE        = 125;
    static constexpr uint8_t MAGNITUDE          = 126;
    static constexpr uint8_t BATON_PASS         = 127;
    static constexpr uint8_t PURSUIT            = 128;
    static constexpr uint8_t RAPID_SPIN         = 129;
    static constexpr uint8_t UNUSED_82          = 130;
    static constexpr uint8_t UNUSED_83          = 131;
    static constexpr uint8_t MORNING_SUN        = 132;
    static constexpr uint8_t SYNTHESIS          = 133;
    static constexpr uint8_t MOONLIGHT          = 134;
    static constexpr uint8_t HIDDEN_POWER       = 135;
    static constexpr uint8_t RAIN_DANCE         = 136;
    static constexpr uint8_t SUNNY_DAY          = 137;
    static constexpr uint8_t DEFENSE_UP_HIT     = 138;
    static constexpr uint8_t ATTACK_UP_HIT      = 139;
    static constexpr uint8_t ALL_UP_HIT         = 140;
    static constexpr uint8_t FAKE_OUT           = 141;
    static constexpr uint8_t BELLY_DRUM         = 142;
    static constexpr uint8_t PSYCH_UP           = 143;
    static constexpr uint8_t MIRROR_COAT        = 144;
    static constexpr uint8_t SKULL_BASH         = 145;
    static constexpr uint8_t TWISTER            = 146;
    static constexpr uint8_t EARTHQUAKE         = 147;
    static constexpr uint8_t FUTURE_SIGHT       = 148;
    static constexpr uint8_t GUST               = 149;
    static constexpr uint8_t STOMP              = 150;
    static constexpr uint8_t SOLARBEAM          = 151;
    static constexpr uint8_t THUNDER            = 152;
    static constexpr uint8_t TELEPORT           = 153;
    static constexpr uint8_t BEAT_UP            = 154;
    static constexpr uint8_t FLY                = 155;
    static constexpr uint8_t DEFENSE_CURL       = 156;

    // Stat-up contiguous ranges for batch registration
    // Source: const_def sequence in move_effect_constants.asm
    static constexpr uint8_t STAT_UP_MIN    = ATTACK_UP;    // 10
    static constexpr uint8_t STAT_UP_MAX    = EVASION_UP;   // 16
    static constexpr uint8_t STAT_UP2_MIN   = ATTACK_UP_2;  // 50
    static constexpr uint8_t STAT_UP2_MAX   = EVASION_UP_2; // 56

    // Stat-down contiguous ranges
    static constexpr uint8_t STAT_DOWN_MIN  = ATTACK_DOWN;   // 18
    static constexpr uint8_t STAT_DOWN_MAX  = EVASION_DOWN;  // 24
    static constexpr uint8_t STAT_DOWN2_MIN = ATTACK_DOWN_2; // 58
    static constexpr uint8_t STAT_DOWN2_MAX = EVASION_DOWN_2;// 64
}

// ============================================================================
// Crystal raw effect byte → EMON semantic EffectId mapping.
//
// Called by the Crystal extractor when writing MoveData into the package.
// Returns SemEffect::Unknown for effects not needed by the engine AI
// (they still get stored but the AI ignores them, which is correct behaviour).
//
// This is the only place Crystal EFFECT_* byte values map to SemEffect:: values.
// Engine code never sees Crystal raw bytes after this point.
// ============================================================================
inline enginemon::EffectId to_semantic_effect(uint8_t crystal_effect) {
    using namespace enginemon;
    switch (crystal_effect) {
        case EffectId::SLEEP:         return SemEffect::Sleep;
        case EffectId::HEAL:          return SemEffect::Heal;
        case EffectId::MORNING_SUN:   return SemEffect::Heal;
        case EffectId::SYNTHESIS:     return SemEffect::Heal;
        case EffectId::MOONLIGHT:     return SemEffect::Heal;
        case EffectId::SELFDESTRUCT:  return SemEffect::Selfdestruct;
        case EffectId::DREAM_EATER:   return SemEffect::DreamEater;
        case EffectId::HYPER_BEAM:    return SemEffect::HyperBeam;
        case EffectId::NIGHTMARE:     return SemEffect::Nightmare;
        case EffectId::TOXIC:         return SemEffect::Toxic;
        case EffectId::POISON:        return SemEffect::Poison;
        case EffectId::PARALYZE:      return SemEffect::Paralyze;
        case EffectId::BATON_PASS:    return SemEffect::BatonPass;
        case EffectId::BELLY_DRUM:    return SemEffect::BellyDrum;
        case EffectId::PROTECT:       return SemEffect::Protect;
        case EffectId::ENDURE:        return SemEffect::Endure;
        case EffectId::REFLECT:       return SemEffect::Reflect;
        case EffectId::LIGHT_SCREEN:  return SemEffect::LightScreen;
        case EffectId::RAIN_DANCE:    return SemEffect::RainDance;
        case EffectId::SUNNY_DAY:     return SemEffect::SunnyDay;
        default: break;
    }
    // Stat-up effects: ATTACK_UP(10)..EVASION_UP(16) and ATTACK_UP_2(50)..EVASION_UP_2(56)
    if ((crystal_effect >= EffectId::STAT_UP_MIN  && crystal_effect <= EffectId::STAT_UP_MAX)
     || (crystal_effect >= EffectId::STAT_UP2_MIN && crystal_effect <= EffectId::STAT_UP2_MAX))
        return SemEffect::StatUp;
    // Stat-down effects: ATTACK_DOWN(18)..EVASION_DOWN(24) and ATTACK_DOWN_2(58)..EVASION_DOWN_2(64)
    if ((crystal_effect >= EffectId::STAT_DOWN_MIN  && crystal_effect <= EffectId::STAT_DOWN_MAX)
     || (crystal_effect >= EffectId::STAT_DOWN2_MIN && crystal_effect <= EffectId::STAT_DOWN2_MAX))
        return SemEffect::StatDown;
    return SemEffect::Unknown;
}

// Crystal range helpers — Crystal frontend only.
// These encode Crystal's contiguous EFFECT_* layout and must NOT appear in engine code.

inline bool crystal_is_stat_up(uint8_t raw_effect) {
    return (raw_effect >= EffectId::STAT_UP_MIN   && raw_effect <= EffectId::STAT_UP_MAX)
        || (raw_effect >= EffectId::STAT_UP2_MIN  && raw_effect <= EffectId::STAT_UP2_MAX);
}

inline bool crystal_is_stat_down(uint8_t raw_effect) {
    return (raw_effect >= EffectId::STAT_DOWN_MIN  && raw_effect <= EffectId::STAT_DOWN_MAX)
        || (raw_effect >= EffectId::STAT_DOWN2_MIN && raw_effect <= EffectId::STAT_DOWN2_MAX);
}

} // namespace crystal
