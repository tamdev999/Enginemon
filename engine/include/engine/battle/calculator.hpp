#pragma once
// engine/battle/calculator.hpp
// Gen 2 damage and stat calculation formulas
// Reference: pokecrystal + suiCune

#include "engine/core/types.hpp"
#include "engine/core/registry.hpp"

namespace enginemon {

struct BattlePokemon;

// Stat stage multipliers (Gen 2)
// Stage:     -6    -5    -4    -3    -2    -1     0    +1    +2    +3    +4    +5    +6
// Numerator:  2     2     2     2     2     2     2     3     4     5     6     7     8
// Denominator: 8     7     6     5     4     3     2     2     2     2     2     2     2
int32_t apply_stat_stage(int32_t base_stat, int8_t stage);

// Accuracy/evasion stage multipliers (different from stats)
int32_t apply_accuracy_stage(int32_t base_accuracy, int8_t acc_stage, int8_t eva_stage);

// Calculate actual stat from base stat, IV, EV, level, nature
// Gen 2 doesn't have natures, simplified formula
int32_t calc_hp(uint8_t base, uint8_t iv, uint16_t ev, uint8_t level);
int32_t calc_stat(uint8_t base, uint8_t iv, uint16_t ev, uint8_t level);

// Main damage formula
// Returns damage before random factor (multiply by 85-100 / 100)
struct DamageParams {
    uint8_t attacker_level;
    int32_t attack_stat;    // With stages applied
    int32_t defense_stat;   // With stages applied
    uint8_t move_power;
    
    // Type effectiveness (combined for dual types)
    // 0 = immune, 25 = 1/4x, 50 = 1/2x, 100 = 1x, 200 = 2x, 400 = 4x
    uint16_t type_effectiveness;
    
    // Modifiers
    bool stab;              // Same Type Attack Bonus (1.5x)
    bool critical;          // Critical hit (2x)
    bool burned;            // Physical attacker burned (0.5x)
    bool reflect_active;    // Reflect for physical (0.5x)
    bool light_screen_active; // Light Screen for special (0.5x)
    Weather weather;
    TypeId move_type;
};
int32_t calculate_damage(const DamageParams& params);

// Critical hit check
// Gen 2 crit stages: 0=6.25%, 1=12.5%, 2=25%, 3=33.3%, 4+=50%
bool roll_critical(uint8_t crit_stage, uint32_t random);

// Accuracy check
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
// Gen 2: (a * L * b) / (7 * s)
// a = base exp yield
// L = defeated pokemon level
// b = 1.5 for trainer, 1.0 for wild
// s = number of pokemon that participated (1 for simplicity now)
uint32_t calculate_exp_gain(uint8_t base_exp, uint8_t defeated_level,
                           bool is_trainer_battle, uint8_t participants);

// Capture formula
// Gen 2 formula is quite different from later gens
// Returns: 0-255 catch check value, >= 256 = guaranteed catch
struct CaptureParams {
    uint8_t catch_rate;     // Species base catch rate
    uint8_t ball_modifier;  // Ball effectiveness
    int32_t max_hp;
    int32_t current_hp;
    Status status;
};
uint16_t calculate_catch_value(const CaptureParams& params);
bool roll_capture(const CaptureParams& params, uint32_t random1, uint32_t random2);

// Run formula
// Gen 2: (speed_player * 128 / speed_wild) + 30 * attempts
// If >= random 0-255, escape succeeds
bool roll_escape(int32_t player_speed, int32_t wild_speed, 
                 uint8_t attempts, uint32_t random);

} // namespace enginemon
