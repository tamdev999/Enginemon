#pragma once
// engine/battle/calculator.hpp
// Gen 2 battle calculation formulas
//
// All formulas source-proven from pokecrystal + suiCune.
// See engine/battle/calculator.cpp for per-function citations.
//
// Stage multiplier table (source: suiCune data/battle/stat_multipliers_2.c):
// Stage:  -6    -5    -4    -3    -2    -1     0    +1    +2    +3    +4    +5    +6
//         25%   28%   33%   40%   50%   66%  100%  150%  200%  250%  300%  350%  400%
// (applied as floor(stat * num / den), capped 1..999)
//
// ROM-derived overloads:
//   Functions that read from a table (apply_stat_stage, apply_accuracy_stage,
//   roll_critical, capture_wobble_chance) have overloads that accept a
//   const BattleRules& instead of using the internal static fallback tables.
//   Production code should use the BattleRules& overloads.
//   The no-rules overloads remain for tests that don't have a package.

#include "engine/battle/battle_rules.hpp"
#include "engine/core/types.hpp"
#include "engine/core/registry.hpp"

namespace enginemon {

struct BattlePokemon;

// Stat stage multipliers (Gen 2) — all stats except accuracy/evasion
// stage clamped to -6..+6; result floored, capped at 999, minimum 1
//
// Production code must use the BattleRules& overload.
// The fallback overload is for tests that construct a calculator without a package.
[[deprecated("use apply_stat_stage(base, stage, rules) in production")]]
int32_t apply_stat_stage(int32_t base_stat, int8_t stage);
int32_t apply_stat_stage(int32_t base_stat, int8_t stage, const BattleRules& rules);

// Accuracy/evasion stage multipliers — applies two separate floor passes matching
// Crystal's BattleCommand_CheckHit .StatModifiers loop.
// Production code must use the BattleRules& overload.
[[deprecated("use apply_accuracy_stage(base, acc, eva, rules) in production")]]
int32_t apply_accuracy_stage(int32_t base_accuracy, int8_t acc_stage, int8_t eva_stage);
int32_t apply_accuracy_stage(int32_t base_accuracy, int8_t acc_stage, int8_t eva_stage,
                              const BattleRules& rules);

// Calculate actual stat from base stat, IV, EV, level, nature
// Gen 2 doesn't have natures, simplified formula
int32_t calc_hp(uint8_t base, uint8_t iv, uint16_t ev, uint8_t level);
int32_t calc_stat(uint8_t base, uint8_t iv, uint16_t ev, uint8_t level);

// Main damage formula
// Returns base damage (before random 85-100% variation, which caller applies).
// Inputs: attack_stat and defense_stat are post-stage values; reflect/light_screen
// callers should double the defense stat before passing in.
//
// Formula (source: suiCune effect_commands.c DamageCalc):
//   base = ((level*2/5 + 2) * power * atk8 / def8 / 50)
//   if critical: base *= 2  (before +2 floor)
//   if burned:   base >>= 1
//   base = base * type_effectiveness / 100
//   if stab:     base += base/2
//   +2, clamp 2..999
//
// Weather and screen modifiers are responsibility of the caller:
//   - Caller passes doubled defense_stat if reflect/light_screen is active
//   - Caller calls apply_weather_modifier() before or after this call
struct DamageParams {
    uint8_t attacker_level;
    int32_t attack_stat;      // Post-stage; do NOT double for burn here
    int32_t defense_stat;     // Post-stage; double for reflect/light_screen before passing
    uint8_t move_power;

    // Combined type effectiveness in per-100 notation:
    //   0=immune, 50=0.5x, 100=1x, 200=2x, 400=4x
    //   Use get_combined_effectiveness() to compute this.
    uint16_t type_effectiveness;

    // Modifiers
    bool stab;              // Same Type Attack Bonus (+50%)
    bool critical;          // Critical hit (×2 the pre-floor result)
    bool burned;            // Physical attacker with burn (>>1 the result)
    bool reflect_active;    // Caller must double defense_stat if true
    bool light_screen_active; // Caller must double defense_stat if true
    Weather weather;        // Informational; caller applies weather via apply_weather_modifier()
    TypeId move_type;       // Informational; used by caller for weather lookup
};
int32_t calculate_damage(const DamageParams& params);

// Apply Gen 2 weather modifier to a damage value.
// Apply weather modifier to a damage value using ROM-derived tables.
// Looks up (weather_id, type_id) in rules.weather_type_modifiers and
// rules.weather_move_modifiers. Returns modified damage (unchanged if no entry matches).
// Source: misc.asm DoWeatherModifiers — multiplier × damage / 10, minimum 1.
int32_t apply_weather_modifier(int32_t damage, uint8_t weather_id, uint8_t type_id,
                                uint8_t effect_id, const BattleRules& rules);

// Fallback: apply a pre-known modifier (tests without a package)
[[deprecated("use apply_weather_modifier(damage, weather, type, effect, rules) in production")]]
int32_t apply_weather_modifier(int32_t damage, bool apply, bool boosted);

// Critical hit check
// Gen 2 crit stages: 0=6.25%, 1=12.5%, 2=25%, 3=33.3%, 4+=50%
//
// Production code must use the BattleRules& overload.
[[deprecated("use roll_critical(stage, random, rules) in production")]]
bool roll_critical(uint8_t crit_stage, uint32_t random);
bool roll_critical(uint8_t crit_stage, uint32_t random, const BattleRules& rules);

// Build the effective crit stage from battle state.
// Source: effect_commands.asm BattleCommand_Critical
//   base stage = 0
//   +2 if move ID is in BattleRules::high_crit_moves
//   +1 if user has Focus Energy volatile (VolatileStatus::FocusEnergy)
//   +2 if user holds Lucky Punch (Chansey) or Stick (Farfetch'd) — not yet representable
//   +1 if user holds Scope Lens (HELD_CRITICAL_UP) — not yet representable
// Returns 0..6 (clamped).
uint8_t build_crit_stage(const BattlePokemon& user, const MoveData& move,
                         const BattleRules& rules);

// Accuracy check
// Crystal source: effect_commands.asm BattleCommand_CheckHit .StatModifiers
//
// Algorithm (two-pass, each with separate floor):
//   Step 1: acc  = floor(move_accuracy  * acc_mult[acc_stage].num  / acc_mult[acc_stage].den)
//           acc  = max(1, acc)
//   Step 2: acc  = floor(acc * acc_mult[(MAX_STAT_LEVEL+1) - eva_stage].num
//                               / acc_mult[(MAX_STAT_LEVEL+1) - eva_stage].den)
//           acc  = max(1, min(acc, 255))
//   Hit if random < acc  (miss if random >= acc)
//   move_accuracy == 0xFF → always hit (Crystal encoding for "never misses")
//   move_accuracy == 0   → always hit (Enginemon normalisation for same)
//
// Production code must use the BattleRules& overload.
bool roll_accuracy(uint8_t move_accuracy, int8_t acc_stage,
                   int8_t eva_stage, uint32_t random,
                   const BattleRules& rules);

// Fallback overload (tests without a package — uses internal static acc table)
[[deprecated("use roll_accuracy(acc, acc_stage, eva_stage, random, rules) in production")]]
bool roll_accuracy(uint8_t move_accuracy, int8_t acc_stage,
                   int8_t eva_stage, uint32_t random);

// Type effectiveness lookup
// Returns: 0=immune, 5=not very, 10=normal, 20=super
uint8_t get_type_effectiveness(TypeId attack_type, TypeId defend_type,
                                const TypeChart& chart);

// Combined type effectiveness for dual types
// Returns: 0, 25, 50, 100, 200, 400 (percentage)
uint16_t get_combined_effectiveness(TypeId attack_type, 
                                    TypeId def_type1, TypeId def_type2,
                                    const TypeChart& chart);

// Experience formula
// Source: suiCune core.c GiveExperiencePoints
//   base = base_exp * defeated_level / 7  (floor, min 1)
//   trainer_battle boost: base = base + base/2  (×1.5 floor)
//   traded mon boost: same ×1.5 applied additionally (not in this function — caller handles)
// participants: each mon that participated earns the full base independently;
// the caller is responsible for the exp-share split.
uint32_t calculate_exp_gain(uint8_t base_exp, uint8_t defeated_level,
                           bool is_trainer_battle, uint8_t participants);

// Capture formula
// Source: suiCune engine/items/item_effects.c PokeBallEffect
//
// final_catch_rate = (catch_rate * ball_modifier/10 * (3*max - 2*hp) / max) + status_add
//   clamped to 0..255
// Catch succeeds if random(0-255) <= final_catch_rate
// Status bonus: SLP/FRZ = +10; BRN/PSN/PAR = +0 (vanilla bug preserved)
//
// ball_modifier: passed as ×10 integer (so PokéBall=10, GreatBall=15, UltraBall=20,
// MasterBall=255 mapped to guaranteed catch via catch_rate=255).
struct CaptureParams {
    uint8_t catch_rate;     // Species base catch rate (0-255)
    uint8_t ball_modifier;  // Ball effectiveness ×10 (PokéBall=10, GreatBall=15, UltraBall=20)
    int32_t max_hp;
    int32_t current_hp;
    Status status;
};
uint16_t calculate_catch_value(const CaptureParams& params);
bool roll_capture(const CaptureParams& params, uint32_t random1, uint32_t random2);

// Wobble probability for capture animation (separate from catch success).
// Source: suiCune engine/battle_anims/pokeball_wobble.c + data/battle/wobble_probabilities.c
// Returns the wobble chance/255 for the given final catch rate.
//
// ROM-derived overload: uses BattleRules::wobble_probabilities.
// Fallback overload:    uses internal static table.
uint8_t capture_wobble_chance(uint16_t final_catch_rate);
uint8_t capture_wobble_chance(uint16_t final_catch_rate, const BattleRules& rules);

// Run/escape formula
// Source: suiCune core.c TryToRunAwayFromBattle
//   Always escape if player_speed >= wild_speed.
//   odds = (player_speed * 32) / (wild_speed / 4) + (attempts-1) * 30
//   Always escape if odds > 255; otherwise escape if random(0-255) < odds.
//   attempts is 1-based (incremented before check in Crystal).
bool roll_escape(int32_t player_speed, int32_t wild_speed,
                 uint8_t attempts, uint32_t random);

} // namespace enginemon
