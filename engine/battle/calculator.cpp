// engine/battle/calculator.cpp
// Gen 2 battle calculation formulas
//
// All algorithms source-proven from:
//   pokecrystal/engine/battle/
//   suiCune/engine/battle/effect_commands.c       (DamageCalc, BattleCommand_Stab, crit)
//   suiCune/engine/battle/core.c                  (stat stages, exp, run)
//   suiCune/engine/items/item_effects.c            (capture formula)
//   suiCune/engine/battle_anims/pokeball_wobble.c  (wobble table)
//   suiCune/data/battle/stat_multipliers_2.c       (stage tables)
//   suiCune/data/battle/critical_hit_chances.c     (crit chances)
//   suiCune/data/battle/accuracy_multipliers.c     (acc stages)
//   suiCune/data/battle/wobble_probabilities.c     (catch wobble table)
//   suiCune/data/battle/weather_modifiers.c        (weather modifiers)

#include "engine/battle/calculator.hpp"
#include "engine/battle/battle.hpp"
#include "engine/battle/battle_rules.hpp"
#include "engine/core/registry.hpp"
#include <algorithm>
#include <cassert>

namespace enginemon {

// ============================================================================
// Internal tables (source-proven from suiCune data files)
// ============================================================================

// Stat stage multipliers for all stats except accuracy/evasion.
// suiCune: data/battle/stat_multipliers_2.c
// Table is indexed as [stage + 6] where stage is -6..+6 → index 0..12.
// Each entry is {numerator, denominator}.
static constexpr uint8_t kStatMult[13][2] = {
    {25, 100}, // -6 =  25%
    {28, 100}, // -5 =  28%
    {33, 100}, // -4 =  33%
    {40, 100}, // -3 =  40%
    {50, 100}, // -2 =  50%
    {66, 100}, // -1 =  66%
    { 1,   1}, //  0 = 100%
    {15,  10}, // +1 = 150%
    { 2,   1}, // +2 = 200%
    {25,  10}, // +3 = 250%
    { 3,   1}, // +4 = 300%
    {35,  10}, // +5 = 350%
    { 4,   1}, // +6 = 400%
};

// Accuracy/evasion stage multipliers.
// suiCune: data/battle/accuracy_multipliers.c
// Indexed as [stage + 6], stage -6..+6 → index 0..12.
static constexpr uint8_t kAccMult[13][2] = {
    { 33, 100}, // -6 =  33%
    { 36, 100}, // -5 =  36%
    { 43, 100}, // -4 =  43%
    { 50, 100}, // -3 =  50%
    { 60, 100}, // -2 =  60%
    { 75, 100}, // -1 =  75%
    {  1,   1}, //  0 = 100%
    {133, 100}, // +1 = 133%
    {166, 100}, // +2 = 166%
    {  2,   1}, // +3 = 200%
    {233, 100}, // +4 = 233%
    {133,  50}, // +5 = 266%
    {  3,   1}, // +6 = 300%
};

// Critical hit chances (threshold values, out of 255).
// suiCune: data/battle/critical_hit_chances.c
// A crit fires when random(0-255) < kCritChance[stage].
// Stages cap at 6 but table has 7 entries; stages >=4 all use 128.
static constexpr uint8_t kCritChance[7] = {
    17,   // stage 0 = 1/15  (≈6.25%)
    32,   // stage 1 = 1/8   (12.5%)
    64,   // stage 2 = 1/4   (25%)
    85,   // stage 3 = 1/3   (33.3%)
    128,  // stage 4 = 1/2   (50%)
    128,  // stage 5 = 1/2
    128,  // stage 6 = 1/2
};

// Wobble probability lookup table for capture animation.
// suiCune: data/battle/wobble_probabilities.c
// nLeft/255 = (nRight/255)^4  — each entry is {catch_rate_threshold, wobble_chance/255}.
// Walked linearly; first entry where [0] >= final_catch_rate is used.
static constexpr uint8_t kWobble[24][2] = {
    {  1,  63},
    {  2,  75},
    {  3,  84},
    {  4,  90},
    {  5,  95},
    {  7, 103},
    { 10, 113},
    { 15, 126},
    { 20, 134},
    { 30, 149},
    { 40, 160},
    { 50, 169},
    { 60, 177},
    { 80, 191},
    {100, 201},
    {120, 211},
    {140, 220},
    {160, 227},
    {180, 234},
    {200, 240},
    {220, 246},
    {240, 251},
    {254, 253},
    {255, 255},
};

// ============================================================================
// Stat stage multipliers
// ============================================================================

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#pragma warning(push)
#pragma warning(disable: 4996)

int32_t apply_stat_stage(int32_t base_stat, int8_t stage) {
    // Fallback: uses internal static table (test-only)
    const int8_t clamped = std::clamp(stage, static_cast<int8_t>(-6), static_cast<int8_t>(6));
    const int idx = clamped + 6;
    const auto [num, den] = kStatMult[idx];
    int32_t result = (base_stat * num) / den;
    if (result < 1) result = 1;
    if (result > 999) result = 999;
    return result;
}

#pragma warning(pop)
#pragma clang diagnostic pop

int32_t apply_stat_stage(int32_t base_stat, int8_t stage, const BattleRules& rules) {
    // ROM-derived overload: uses rules.stat_stage_mult (from BattleRules package chunk).
    const auto e = rules.get_stat_mult(stage);
    int32_t result = (base_stat * e.numerator) / e.denominator;
    if (result < 1) result = 1;
    if (result > 999) result = 999;
    return result;
}

int32_t apply_accuracy_stage(int32_t base_accuracy, int8_t acc_stage, int8_t eva_stage) {
    // Fallback: single net-stage (test-only).
    const int8_t net = std::clamp(
        static_cast<int8_t>(acc_stage - eva_stage),
        static_cast<int8_t>(-6),
        static_cast<int8_t>(6));
    const int idx = net + 6;
    const auto [num, den] = kAccMult[idx];
    int32_t result = (base_accuracy * num) / den;
    if (result < 1) result = 1;
    return result;
}

int32_t apply_accuracy_stage(int32_t base_accuracy, int8_t acc_stage, int8_t eva_stage,
                              const BattleRules& rules) {
    // ROM-derived overload: uses rules.acc_stage_mult.
    const int8_t net = std::clamp(
        static_cast<int8_t>(acc_stage - eva_stage),
        static_cast<int8_t>(-6),
        static_cast<int8_t>(6));
    const auto e = rules.get_acc_mult(net);
    int32_t result = (base_accuracy * e.numerator) / e.denominator;
    if (result < 1) result = 1;
    return result;
}

// ============================================================================
// Stat calculation (source: pokemon.cpp recalculate_stats, ceil-sqrt)
// ============================================================================

// Ceiling sqrt matching Crystal's GetSquareRoot (already in pokemon.cpp).
// Repeated here so calculator is self-contained for testing.
static uint8_t ceil_sqrt_u8(uint16_t v) {
    if (v == 0) return 0;
    uint16_t b = 1;
    while (b * b < v) ++b;
    return static_cast<uint8_t>(std::min<uint16_t>(b, 255));
}

int32_t calc_hp(uint8_t base, uint8_t iv, uint16_t ev, uint8_t level) {
    // suiCune/engine/pokemon/stats.c CalcMonStatC:
    // sqrt_term = floor(ceil_sqrt(ev) / 4)  [cap ceil_sqrt at 255]
    // HP: floor(((base + iv) * 2 + sqrt_term) * level / 100) + level + 10
    const uint8_t sq = ceil_sqrt_u8(ev);
    const uint32_t sqrt_term = sq / 4;
    return static_cast<int32_t>(((base + iv) * 2 + sqrt_term) * level / 100) + level + 10;
}

int32_t calc_stat(uint8_t base, uint8_t iv, uint16_t ev, uint8_t level) {
    // suiCune/engine/pokemon/stats.c CalcMonStatC:
    // non-HP: floor(((base + iv) * 2 + sqrt_term) * level / 100) + 5
    const uint8_t sq = ceil_sqrt_u8(ev);
    const uint32_t sqrt_term = sq / 4;
    return static_cast<int32_t>(((base + iv) * 2 + sqrt_term) * level / 100) + 5;
}

// ============================================================================
// Damage calculation
// ============================================================================

// Truncate stat to 8-bit, matching Crystal's TruncateHL_BC algorithm.
// suiCune: effect_commands.c TruncateHL_BC
// Repeatedly right-shifts both atk and def by 2 until both fit in 8 bits.
// Minimum value after truncation is 1.
static void truncate_stats(int32_t& atk, int32_t& def) {
    while (atk > 255 || def > 255) {
        def >>= 2;
        if (def == 0) def = 1;
        atk >>= 2;
        if (atk == 0) atk = 1;
    }
}

int32_t calculate_damage(const DamageParams& params) {
    // suiCune: effect_commands.c DamageCalc()
    //
    // Formula:
    //   n = (level*2/5 + 2) * power * atk / def / 50
    //   If critical: n *= 2  (before +2 minimum floor)
    //   Apply type effectiveness: n = n * effectiveness / 10
    //   Apply STAB: n += n/2  (if stab)
    //   Apply weather: n = n * modifier / 10
    //   Apply screen: def *= 2 already folded into params.defense_stat
    //   Apply random variation: n = n * random(85-100) / 100  (not in this function — caller)
    //   Final: +2, clamp 2..999
    //
    // Note: damage variation (85-100%) is NOT applied here — the caller supplies
    // a pre-rolled random factor via params.type_effectiveness composition.
    // This function returns the deterministic base damage (pre-random).

    if (params.move_power == 0) return 0;

    int32_t atk = params.attack_stat;
    int32_t def = params.defense_stat;

    // Truncate both stats to 8-bit using Crystal's shift algorithm
    truncate_stats(atk, def);
    if (def <= 0) def = 1;

    // Core formula: (level*2/5 + 2) * power * atk / def / 50
    int32_t n = static_cast<int32_t>(params.attacker_level) * 2 / 5 + 2;
    n = n * params.move_power;
    n = n * atk;
    n = n / def;
    n = n / 50;

    // Critical hit: ×2 to the raw formula result (before any other modifiers)
    if (params.critical) {
        n *= 2;
        if (n > 0xFFFF) n = 0xFFFF;
    }

    // Burn penalty: physical attacker with burn does half damage
    // suiCune: the burn check halves the attacker's Attack *before* damage calc
    // in practice; here we halve the result equivalently.
    if (params.burned) {
        n >>= 1;
        if (n == 0) n = 1;
    }

    // Type effectiveness: multiplier uses {0, 5, 10, 20} (×0, ×0.5, ×1, ×2)
    // Combined dual-type uses {0, 25, 50, 100, 200, 400} (per-100 notation).
    // suiCune effect_commands.c: applies as (n * eff) / 10 for single matchup,
    // iteratively for dual types (so combined 200 = two ×20/10 passes).
    // We accept the caller-computed combined effectiveness in per-100 notation.
    if (params.type_effectiveness == 0) {
        return 0;  // Immune — full short-circuit, no +2 floor
    }
    n = n * static_cast<int32_t>(params.type_effectiveness) / 100;
    // NOTE: n may be 0 here from integer division (not immunity). Do NOT
    // return 0 — Crystal always adds the +2 floor after the formula.

    // STAB: + n/2 (integer arithmetic, so n + n>>1)
    if (params.stab) {
        n += n / 2;
    }

    // Weather modifier — intentionally NOT applied here.
    // Weather depends on TypeId values that are registry-defined per frontend and
    // cannot be hard-coded in the engine layer (TypeId for WATER/FIRE vary by game).
    // Callers must check params.weather + params.move_type and call
    // apply_weather_modifier() on the result of calculate_damage() themselves.
    // The weather/move_type fields in DamageParams are informational only.

    // Add minimum floor of +2, cap at 999
    // suiCune: MIN_DAMAGE=2 is added after the /50 result, before random variation.
    n += 2;
    if (n > 999) n = 999;
    if (n < 2) n = 2;

    return n;
}

// Apply Gen 2 weather damage modifier to a computed damage value.
// suiCune: misc.c DoWeatherModifiers + weather_modifiers.c
// Returns modified damage (caller must pass the pre-weather damage).
// weather_type_matches_move indicates if move type matches the boosted/penalized category.
// is_boosted: true = 1.5× (MORE_EFFECTIVE), false = 0.5× (NOT_VERY_EFFECTIVE)
int32_t apply_weather_modifier(int32_t damage, bool apply, bool boosted) {
    if (!apply) return damage;
    // MORE_EFFECTIVE = 15/10 = 1.5×; NOT_VERY_EFFECTIVE = 5/10 = 0.5×
    int32_t result = damage * (boosted ? 15 : 5) / 10;
    if (result == 0) result = 1;
    return result;
}

int32_t apply_weather_modifier(int32_t damage, uint8_t weather_id, uint8_t type_id,
                                uint8_t effect_id, const BattleRules& rules) {
    // suiCune misc.c DoWeatherModifiers — check WeatherTypeModifiers then
    // WeatherMoveModifiers.  First matching entry wins.
    // Multiplier is in Crystal's per-10 notation: 15 = ×1.5, 5 = ×0.5.
    // Result = max(1, floor(damage × multiplier / 10)).
    if (weather_id == 0) return damage;  // No weather active

    // Check type modifiers (weather × move type)
    for (const auto& e : rules.weather_type_modifiers) {
        if (e.weather_id == weather_id && e.type_id == type_id) {
            int32_t result = damage * static_cast<int32_t>(e.multiplier) / 10;
            return result < 1 ? 1 : result;
        }
    }
    // Check move-effect modifiers (weather × move effect)
    for (const auto& e : rules.weather_move_modifiers) {
        if (e.weather_id == weather_id && e.type_id == effect_id) {
            int32_t result = damage * static_cast<int32_t>(e.multiplier) / 10;
            return result < 1 ? 1 : result;
        }
    }
    return damage;  // No modifier found
}

// ============================================================================
// Critical hit check
// ============================================================================

bool roll_critical(uint8_t crit_stage, uint32_t random) {
    // suiCune: effect_commands.c BattleCommand_Critical
    // random is 0-255; a crit fires when random < threshold.
    const uint8_t stage_idx = std::min<uint8_t>(crit_stage, 6);
    return (random & 0xFF) < kCritChance[stage_idx];
}

bool roll_critical(uint8_t crit_stage, uint32_t random, const BattleRules& rules) {
    // ROM-derived overload: uses rules.crit_chances (from BattleRules package chunk).
    return (random & 0xFF) < rules.get_crit_chance(crit_stage);
}

uint8_t build_crit_stage(const BattlePokemon& user, const MoveData& move,
                          const BattleRules& rules) {
    // Source: effect_commands.asm BattleCommand_Critical
    //   c = 0 (base)
    //   +2 if move ID is in CriticalHitMoves list
    //   +1 if user has Focus Energy volatile (SUBSTATUS_FOCUS_ENERGY)
    //   Scope Lens (+1) and Lucky Punch/Stick (+2) require held-item checks
    //   which are not yet representable in BattlePokemon; leave those at 0
    //   and add explicit gating comments so future held-item work can wire in.
    uint8_t stage = 0;

    // High-crit move: +2 (source: data/moves/critical_hit_moves.asm)
    // rules.high_crit_moves stores Crystal move animation IDs.
    // MoveData::animation_id matches Crystal's MOVE_ANIM field.
    if (rules.is_high_crit_move(move.animation_id)) {
        stage += 2;
    }

    // Focus Energy: +1 (source: BattleCommand_Critical .FocusEnergy check)
    if (user.has_volatile(VolatileStatus::FocusEnergy)) {
        stage += 1;
    }

    // Scope Lens (HELD_CRITICAL_UP): +1 — not yet: held items not in BattlePokemon
    // Lucky Punch (Chansey) / Stick (Farfetch'd): +2 — not yet: same reason

    return std::min<uint8_t>(stage, 6);
}

// ============================================================================
// Accuracy check
// ============================================================================

// Crystal source: effect_commands.asm BattleCommand_CheckHit .StatModifiers
//
// Two-pass algorithm matching Crystal exactly:
//   Pass 1: apply acc_stage multiplier to move_accuracy (floor, min 1)
//   Pass 2: apply evasion-derived stage (inverted: MAX_STAT_LEVEL+1 - eva_stage_value,
//           but we use the matching index in acc_mult) (floor, min 1, max 255)
//   Hit if random < result  (miss if random >= result)
//   move_accuracy == 0xFF or 0 → always hit
//
// Crystal's evasion application: instead of net stage, Crystal applies the accuracy
// table again using (MAX_STAT_LEVEL+1 - eva_level) as the stage index.
// MAX_STAT_LEVEL = 13 (see constants/battle_constants.asm), so the loop uses
// stage index = 14 - eva_level_value.  The acc_stage_mult table is 13 entries
// indexed 0..12 (stage -6..+6). The crystal "level" values run 1..13 mapping
// to indices 0..12 (stage -6..+6).  evasion level 7 = stage 0 (neutral).
// So the evasion stage (-6..+6) maps to neutral index 6.  For the second pass,
// Crystal uses the *inverted* table entry: at eva_stage +1 (level 8), Crystal
// uses level 14-8=6 → stage index 5 (acc stage -1, so 0.75×). This exactly
// matches applying accuracy stage = -(eva_stage) in the same table.
// Therefore: pass2 uses acc_mult[-eva_stage] which equals acc_mult[0 - eva_stage].
static uint32_t apply_acc_mult_entry(uint32_t val, const StageMultiplierEntry& e) {
    uint32_t r = val * e.numerator / e.denominator;
    return r < 1 ? 1 : r;
}

bool roll_accuracy(uint8_t move_accuracy, int8_t acc_stage,
                   int8_t eva_stage, uint32_t random,
                   const BattleRules& rules) {
    // 0xFF = always hit (Crystal encoding); 0 = always hit (Enginemon normalisation)
    if (move_accuracy == 0xFF || move_accuracy == 0) return true;

    // Pass 1: apply attacker accuracy stage
    const auto e1 = rules.get_acc_mult(acc_stage);
    uint32_t acc = apply_acc_mult_entry(static_cast<uint32_t>(move_accuracy), e1);

    // Pass 2: apply defender evasion stage (inverted)
    // Crystal applies stage (14 - eva_level), which equals -eva_stage in our notation.
    const auto e2 = rules.get_acc_mult(static_cast<int8_t>(-eva_stage));
    acc = apply_acc_mult_entry(acc, e2);
    if (acc > 255) acc = 255;

    // Hit if random < acc (miss if random >= acc)
    // Crystal: BattleRandom() cp [hl]; jr nc, .Miss  — miss if random >= acc
    return (random & 0xFF) < acc;
}

// Fallback (test-only, uses internal static table)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#pragma warning(push)
#pragma warning(disable: 4996)
bool roll_accuracy(uint8_t move_accuracy, int8_t acc_stage,
                   int8_t eva_stage, uint32_t random) {
    if (move_accuracy == 0xFF || move_accuracy == 0) return true;

    // Two-pass floor using internal static table
    auto apply_entry = [](uint32_t v, int8_t stage) -> uint32_t {
        const int8_t clamped = std::clamp(stage, static_cast<int8_t>(-6), static_cast<int8_t>(6));
        const int idx = clamped + 6;
        const auto [num, den] = kAccMult[idx];
        uint32_t r = v * num / den;
        return r < 1 ? 1 : r;
    };

    uint32_t acc = apply_entry(static_cast<uint32_t>(move_accuracy), acc_stage);
    acc = apply_entry(acc, static_cast<int8_t>(-eva_stage));
    if (acc > 255) acc = 255;

    return (random & 0xFF) < acc;
}
#pragma warning(pop)
#pragma clang diagnostic pop

// ============================================================================
// Type effectiveness
// ============================================================================

uint8_t get_type_effectiveness(TypeId attack_type, TypeId defend_type,
                                const TypeChart& chart) {
    return chart.get_effectiveness(attack_type, defend_type);
}

uint16_t get_combined_effectiveness(TypeId attack_type,
                                    TypeId def_type1, TypeId def_type2,
                                    const TypeChart& chart) {
    // suiCune: effect_commands.c CheckTypeMatchup iterates matchup table and
    // multiplies wTypeMatchup by each applicable entry then divides by 10.
    // Starting value = EFFECTIVE = 10.
    // For dual types this is applied per-defender-type, so:
    //   final = start * eff1/10 * eff2/10 * 100 (to get per-100 notation)
    uint32_t result = 10u;  // Start at EFFECTIVE
    result = result * chart.get_effectiveness(attack_type, def_type1) / 10u;
    if (def_type2 != def_type1 && def_type2 != TYPE_NONE) {
        result = result * chart.get_effectiveness(attack_type, def_type2) / 10u;
    }
    // Convert from per-10 notation to per-100 notation used by DamageParams
    // 0→0, 10→100, 20→200, 5→50, 4→40, etc.
    return static_cast<uint16_t>(result * 10u);
}

// ============================================================================
// Experience gain
// ============================================================================

uint32_t calculate_exp_gain(uint8_t base_exp, uint8_t defeated_level,
                           bool is_trainer_battle, uint8_t participants) {
    // suiCune: core.c GiveExperiencePoints
    // Base formula: exp = base_exp * defeated_level / 7
    // Then BoostExp (×1.5, floor) for trainer battle and/or traded mon.
    // participants: Crystal divides among participants AFTER boosting in the
    // loop, so each mon receives base/7 independently; callers must handle
    // the per-participant split externally. Here we compute the per-mon share.
    (void)participants;  // Each mon gets full base; caller divides if needed

    uint32_t exp = (static_cast<uint32_t>(base_exp) * defeated_level) / 7u;
    if (exp == 0) exp = 1;  // Minimum 1 exp

    // Trainer battle boost: ×1.5 (floor)
    // suiCune: BoostExp returns exp + exp/2
    if (is_trainer_battle) {
        exp = exp + exp / 2u;
    }

    return exp;
}

// ============================================================================
// Capture formula
// ============================================================================

uint16_t calculate_catch_value(const CaptureParams& params) {
    // suiCune: item_effects.c PokeBallEffect
    //
    // Crystal's algorithm (source-proven):
    //   hp_term = 2 * current_hp  (with the hp*2 and maxhp*3 reduced if >255)
    //   maxhp_term = 3 * max_hp
    //   num = catch_rate * (maxhp_term - hp_term) / maxhp_term  [floor, min 1]
    //   status_add = 10 if SLP/FRZ, 0 otherwise (vanilla bug: BRN/PSN/PAR=0)
    //   num = min(num + status_add, 255)
    //   ball_modifier: multiply catch_rate by ball_modifier before the HP calc
    //     (the actual Crystal code stores a pre-multiplied base; we accept it directly)
    //   final = num (0-255); catch succeeds if random > final (i.e., random >= final+1)
    //   wobble check uses WobbleProbabilities table

    if (params.max_hp <= 0) return 0;

    int32_t hp     = params.current_hp;
    int32_t max_hp = params.max_hp;

    // Apply ball modifier (base catch rate already scaled by caller via ball_modifier)
    // Protect against overflow: use same shift-right logic as Crystal.
    int32_t base = static_cast<int32_t>(params.catch_rate)
                 * static_cast<int32_t>(params.ball_modifier) / 10;
    if (base > 255) base = 255;
    if (base < 1) base = 1;

    // Compute HP factor: (3*max - 2*hp) / max  [with overflow protection]
    int32_t hp2    = hp * 2;
    int32_t max3   = max_hp * 3;

    // If max3 > 255, both terms are halved together (Crystal's overflow protection)
    if (max3 > 255) {
        max3 >>= 2;
        hp2  >>= 2;
        if (hp2 == 0) hp2 = 1;
    }

    int32_t numerator = base * (max3 - hp2);
    int32_t num = (max3 > 0) ? (numerator / max3) : 1;
    if (num <= 0) num = 1;

    // Status bonus
    // Crystal bug: only SLP and FRZ get +10; BRN/PSN/PAR get +0 (not +5 as intended)
    int32_t status_add = 0;
    if (params.status == Status::Sleep || params.status == Status::Freeze) {
        status_add = 10;
    }
    // Note: NOT adding for Burn/Poison/Paralysis — vanilla bug preserved.

    int32_t final_rate = num + status_add;
    if (final_rate > 255) final_rate = 255;

    return static_cast<uint16_t>(final_rate);
}

bool roll_capture(const CaptureParams& params, uint32_t random1, uint32_t random2) {
    // suiCune: item_effects.c — catch succeeds if random > final_rate (i.e. > catch_value)
    // Wait: the check is "random > b" where b = final_rate, so:
    //   wWildMon = 0 (not caught) if random > final_catch_rate
    //   wWildMon = species (caught) if random <= final_catch_rate
    // suiCune line 726: if(r > b) wram->wWildMon = 0; else wram->wWildMon = species;
    (void)random2;  // random2 is used only for Ball wobble animation; catch is one roll
    const uint16_t catch_val = calculate_catch_value(params);
    return (random1 & 0xFF) <= catch_val;
}

// ============================================================================
// Run/escape formula
// ============================================================================

bool roll_escape(int32_t player_speed, int32_t wild_speed,
                 uint8_t attempts, uint32_t random) {
    // suiCune: core.c TryToRunAwayFromBattle
    //
    // If player_speed >= wild_speed: always escape.
    if (player_speed >= wild_speed) return true;

    // odds = (player_speed * 32) / (wild_speed / 4)  [integer division]
    // If wild_speed/4 == 0: always escape (avoid divide-by-zero).
    int32_t divisor = wild_speed / 4;
    if (divisor <= 0) return true;

    // Each run attempt adds 30 to odds; Crystal increments wNumFleeAttempts
    // BEFORE computing odds, so attempts here is the current attempt count (1-based).
    int32_t odds = (player_speed * 32) / divisor + (attempts - 1) * 30;

    // If odds > 255: always escape
    if (odds > 255) return true;

    // Succeed if random(0-255) < odds
    return (random & 0xFF) < static_cast<uint32_t>(odds);
}

// ============================================================================
// Wobble probability lookup (for capture animation; separate from catch success)
// ============================================================================

uint8_t capture_wobble_chance(uint16_t final_catch_rate) {
    // suiCune: pokeball_wobble.c GetPokeBallWobble
    // Walk table until [0] >= final_catch_rate, use [1] as wobble chance/255.
    for (const auto& entry : kWobble) {
        if (entry[0] >= static_cast<uint8_t>(final_catch_rate)) {
            return entry[1];
        }
    }
    return 255;  // Should not reach; last entry covers 255
}

uint8_t capture_wobble_chance(uint16_t final_catch_rate, const BattleRules& rules) {
    // ROM-derived overload: uses rules.wobble_probabilities.
    return rules.get_wobble_chance(static_cast<uint8_t>(final_catch_rate));
}

} // namespace enginemon
