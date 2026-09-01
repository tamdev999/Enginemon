// battle_test.cpp
// Battle calculator adversarial tests.
// Links only against enginemon_engine (NOT enginemon_crystal).
//
// Formula citations per test — all source-proven from suiCune:
//   DamageCalc:         engine/battle/effect_commands.c
//   StatMult:           data/battle/stat_multipliers_2.c
//   AccMult:            data/battle/accuracy_multipliers.c
//   CritChances:        data/battle/critical_hit_chances.c
//   ExpFormula:         engine/battle/core.c GiveExperiencePoints
//   CaptureFormula:     engine/items/item_effects.c PokeBallEffect
//   RunFormula:         engine/battle/core.c TryToRunAwayFromBattle
//   WobbleProbabilities:data/battle/wobble_probabilities.c

#include "engine/battle/calculator.hpp"
#include "engine/core/registry.hpp"
#include <iostream>
#include <cassert>
#include <cmath>

// =============================================================================
// Minimal test framework
// =============================================================================
static int g_passed = 0;
static int g_failed = 0;
static bool g_test_failed = false;

#define ASSERT_TRUE(cond) \
    do { if (!(cond)) { std::cerr << "  FAIL: " #cond " at line " << __LINE__ << "\n"; g_test_failed = true; return; } } while (0)
#define ASSERT_EQ(a, b) \
    do { auto _a = (a); auto _b = (b); \
         if (_a != _b) { std::cerr << "  FAIL: " #a " == " #b " (" << _a << " != " << _b << ") at line " << __LINE__ << "\n"; g_test_failed = true; return; } } while (0)
#define ASSERT_GE(a, b) \
    do { auto _a = (a); auto _b = (b); \
         if (!(_a >= _b)) { std::cerr << "  FAIL: " #a " >= " #b " (" << _a << " < " << _b << ") at line " << __LINE__ << "\n"; g_test_failed = true; return; } } while (0)
#define ASSERT_LE(a, b) \
    do { auto _a = (a); auto _b = (b); \
         if (!(_a <= _b)) { std::cerr << "  FAIL: " #a " <= " #b " (" << _a << " > " << _b << ") at line " << __LINE__ << "\n"; g_test_failed = true; return; } } while (0)

static void run_test(const char* name, void (*fn)()) {
    std::cout << "Running " << name << "... ";
    g_test_failed = false;
    try {
        fn();
        if (g_test_failed) { std::cout << "FAIL\n"; g_failed++; }
        else               { std::cout << "PASS\n"; g_passed++; }
    } catch (const std::exception& e) {
        std::cout << "EXCEPTION: " << e.what() << "\n"; g_failed++;
    }
}

#define RUN(name) run_test(#name, test_##name)
#define TEST(name) static void test_##name()

using namespace enginemon;

// =============================================================================
// Smoke test
// =============================================================================
TEST(battle_test_binary_links) {
    ASSERT_TRUE(true);
    std::cout << "  [battle_test binary: engine-only link confirmed]\n";
}

// =============================================================================
// Stat stage multiplier tests
// Source: suiCune data/battle/stat_multipliers_2.c
// =============================================================================

TEST(stat_stage_neutral) {
    // Stage 0: stat unchanged (100/100 = 1.0)
    ASSERT_EQ(apply_stat_stage(100, 0), 100);
    ASSERT_EQ(apply_stat_stage(50, 0),  50);
}

TEST(stat_stage_plus1) {
    // +1: 15/10 = 1.5 — floor(100 * 15 / 10) = 150
    ASSERT_EQ(apply_stat_stage(100, 1), 150);
}

TEST(stat_stage_plus2) {
    // +2: 2/1 = 2.0 — 100 * 2 = 200
    ASSERT_EQ(apply_stat_stage(100, 2), 200);
}

TEST(stat_stage_plus6) {
    // +6: 4/1 = 4.0 — 100 * 4 = 400
    ASSERT_EQ(apply_stat_stage(100, 6), 400);
}

TEST(stat_stage_minus1) {
    // -1: 66/100 = 0.66 — floor(100 * 66 / 100) = 66
    ASSERT_EQ(apply_stat_stage(100, -1), 66);
}

TEST(stat_stage_minus6) {
    // -6: 25/100 = 0.25 — floor(100 * 25 / 100) = 25
    ASSERT_EQ(apply_stat_stage(100, -6), 25);
}

TEST(stat_stage_clamped_beyond_6) {
    // Stages beyond ±6 are clamped to ±6
    ASSERT_EQ(apply_stat_stage(100, 7),  apply_stat_stage(100, 6));
    ASSERT_EQ(apply_stat_stage(100, -7), apply_stat_stage(100, -6));
}

TEST(stat_stage_minimum_1) {
    // Very low base stat * negative stage must never go below 1
    ASSERT_GE(apply_stat_stage(1, -6), 1);
}

TEST(stat_stage_capped_999) {
    // Large stat at +6 must not exceed 999
    // 999 * 4 = 3996 → capped to 999
    ASSERT_EQ(apply_stat_stage(999, 6), 999);
    // 300 * 4 = 1200 → capped to 999
    ASSERT_EQ(apply_stat_stage(300, 6), 999);
}

// =============================================================================
// Accuracy stage tests
// Source: suiCune data/battle/accuracy_multipliers.c
// =============================================================================

TEST(accuracy_stage_neutral) {
    // Net stage 0: 1/1 = 100 → floor(100 * 1 / 1) = 100
    ASSERT_EQ(apply_accuracy_stage(100, 0, 0), 100);
}

TEST(accuracy_stage_plus1) {
    // acc+1, eva 0 → net +1: 133/100 → floor(100 * 133 / 100) = 133
    ASSERT_EQ(apply_accuracy_stage(100, 1, 0), 133);
}

TEST(accuracy_stage_minus1) {
    // acc 0, eva+1 → net -1: 75/100 → floor(100 * 75 / 100) = 75
    ASSERT_EQ(apply_accuracy_stage(100, 0, 1), 75);
}

TEST(accuracy_stage_plus6) {
    // Net +6: 3/1 = 300 → floor(100 * 3) = 300
    ASSERT_EQ(apply_accuracy_stage(100, 6, 0), 300);
}

TEST(accuracy_stage_minus6) {
    // Net -6: 33/100 → floor(100 * 33 / 100) = 33
    ASSERT_EQ(apply_accuracy_stage(100, 0, 6), 33);
}

TEST(accuracy_stage_net_clamped) {
    // Net stage clamped: acc+6, eva+3 = net+3; acc+6, eva+7 = net-1 (clamp)
    // net+3 = 200/100 = 200
    ASSERT_EQ(apply_accuracy_stage(100, 6, 3), 200);
    // net-1 = 75/100 = 75
    ASSERT_EQ(apply_accuracy_stage(100, 6, 7), 75);
}

// =============================================================================
// Damage formula tests
// Source: suiCune engine/battle/effect_commands.c DamageCalc
// Formula: n = (level*2/5 + 2) * power * atk8 / def8 / 50 + 2  (clamp 2..999)
// =============================================================================

TEST(damage_basic_known_value) {
    // Level 50, power 80, atk=100, def=100, neutral type (100), no stab, no crit, no burn
    // n = (50*2/5 + 2) * 80 * 100 / 100 / 50
    // = (20 + 2) * 80 * 100 / 100 / 50
    // = 22 * 80 / 50 = 1760 / 50 = 35
    // + 2 = 37
    DamageParams p{};
    p.attacker_level = 50;
    p.attack_stat    = 100;
    p.defense_stat   = 100;
    p.move_power     = 80;
    p.type_effectiveness = 100;
    p.stab = false; p.critical = false; p.burned = false;
    ASSERT_EQ(calculate_damage(p), 37);
}

TEST(damage_critical_doubles_pre_floor) {
    // Same as above but critical=true: n=35*2=70, +2=72
    DamageParams p{};
    p.attacker_level = 50;
    p.attack_stat    = 100;
    p.defense_stat   = 100;
    p.move_power     = 80;
    p.type_effectiveness = 100;
    p.critical = true;
    ASSERT_EQ(calculate_damage(p), 72);
}

TEST(damage_stab_adds_half) {
    // Without crit: base=35, +2=37. With STAB: 35 + 35/2 = 35+17=52, +2=54
    DamageParams p{};
    p.attacker_level = 50;
    p.attack_stat    = 100;
    p.defense_stat   = 100;
    p.move_power     = 80;
    p.type_effectiveness = 100;
    p.stab = true;
    ASSERT_EQ(calculate_damage(p), 54);
}

TEST(damage_super_effective_2x) {
    // type_effectiveness=200 (2x): base=35*200/100=70, +2=72
    DamageParams p{};
    p.attacker_level = 50;
    p.attack_stat    = 100;
    p.defense_stat   = 100;
    p.move_power     = 80;
    p.type_effectiveness = 200;
    ASSERT_EQ(calculate_damage(p), 72);
}

TEST(damage_immune_zero) {
    // type_effectiveness=0 (immune): always 0 regardless of everything else
    DamageParams p{};
    p.attacker_level = 100;
    p.attack_stat    = 255;
    p.defense_stat   = 1;
    p.move_power     = 150;
    p.type_effectiveness = 0;
    ASSERT_EQ(calculate_damage(p), 0);
}

TEST(damage_not_very_effective_half) {
    // type_effectiveness=50: base=35*50/100=17, +2=19
    DamageParams p{};
    p.attacker_level = 50;
    p.attack_stat    = 100;
    p.defense_stat   = 100;
    p.move_power     = 80;
    p.type_effectiveness = 50;
    ASSERT_EQ(calculate_damage(p), 19);
}

TEST(damage_burn_halves_result) {
    // Burn: base=35, >>1=17, +2=19
    DamageParams p{};
    p.attacker_level = 50;
    p.attack_stat    = 100;
    p.defense_stat   = 100;
    p.move_power     = 80;
    p.type_effectiveness = 100;
    p.burned = true;
    ASSERT_EQ(calculate_damage(p), 19);
}

TEST(damage_capped_at_999) {
    // Artificially high values should cap at 999
    DamageParams p{};
    p.attacker_level = 100;
    p.attack_stat    = 255;
    p.defense_stat   = 1;
    p.move_power     = 150;
    p.type_effectiveness = 400;  // 4x
    p.stab   = true;
    p.critical = true;
    ASSERT_EQ(calculate_damage(p), 999);
}

TEST(damage_minimum_is_2) {
    // The +2 floor applies after the formula result, but only if the formula
    // produces a non-zero value. A power-1 move against max defense at level 1
    // may produce 0 before +2 because integer division floors to 0.
    // Crystal behavior: if formula result is 0 (from very high defense), damage is 0.
    // Test instead that a non-trivial case respects the minimum.
    // Level 50, power 1, atk=1, def=1: n=(22*1*1/1/50)+2 = 0+2=2
    DamageParams p{};
    p.attacker_level = 50;
    p.attack_stat    = 1;
    p.defense_stat   = 1;
    p.move_power     = 1;
    p.type_effectiveness = 100;
    ASSERT_GE(calculate_damage(p), 2);
}

TEST(damage_zero_power_returns_zero) {
    // Power 0 = status move, no damage
    DamageParams p{};
    p.attacker_level = 100;
    p.attack_stat    = 255;
    p.defense_stat   = 1;
    p.move_power     = 0;
    p.type_effectiveness = 100;
    ASSERT_EQ(calculate_damage(p), 0);
}

TEST(damage_stat_truncation_over_255) {
    // Stats > 255 are truncated by >>2 in pairs. Verify result is still
    // deterministic and sane (not zero, not insane).
    DamageParams p{};
    p.attacker_level = 50;
    p.attack_stat    = 512;  // Will be truncated
    p.defense_stat   = 512;  // Both truncated — ratio preserved
    p.move_power     = 80;
    p.type_effectiveness = 100;
    // After truncation both become 512>>2=128 (same as atk=128,def=128),
    // so damage should equal the neutral case with atk=def=128.
    // n = (50*2/5+2) * 80 * 128 / 128 / 50 = 22*80/50 = 35, +2 = 37
    ASSERT_EQ(calculate_damage(p), 37);
}

// =============================================================================
// Weather modifier tests
// Source: suiCune misc.c DoWeatherModifiers + data/battle/weather_modifiers.c
// MORE_EFFECTIVE = 15/10; NOT_VERY_EFFECTIVE = 5/10
// =============================================================================

TEST(weather_modifier_boost) {
    // 100 * 15 / 10 = 150
    ASSERT_EQ(apply_weather_modifier(100, true, true),  150);
}

TEST(weather_modifier_penalty) {
    // 100 * 5 / 10 = 50
    ASSERT_EQ(apply_weather_modifier(100, true, false), 50);
}

TEST(weather_modifier_not_applied) {
    // apply=false: unchanged
    ASSERT_EQ(apply_weather_modifier(100, false, true),  100);
    ASSERT_EQ(apply_weather_modifier(100, false, false), 100);
}

TEST(weather_modifier_minimum_1) {
    // 1 * 5 / 10 = 0 → clamp to 1
    ASSERT_EQ(apply_weather_modifier(1, true, false), 1);
}

// =============================================================================
// Critical hit tests
// Source: suiCune data/battle/critical_hit_chances.c
// Stage 0: threshold=17 (≈6.25%), random(0-255)
// =============================================================================

TEST(crit_roll_below_threshold_hits) {
    // Stage 0 threshold = 17; random=0 → crit
    ASSERT_TRUE(roll_critical(0, 0));
    ASSERT_TRUE(roll_critical(0, 16));
}

TEST(crit_roll_at_threshold_misses) {
    // random = 17 → NOT a crit (must be strictly < threshold)
    ASSERT_TRUE(!roll_critical(0, 17));
}

TEST(crit_stage1_threshold_32) {
    ASSERT_TRUE(roll_critical(1, 31));
    ASSERT_TRUE(!roll_critical(1, 32));
}

TEST(crit_stage4_half_chance) {
    // Stage >=4: threshold=128 — exactly half of 0-255
    ASSERT_TRUE(roll_critical(4, 127));
    ASSERT_TRUE(!roll_critical(4, 128));
    // Stages 5 and 6 use the same threshold
    ASSERT_TRUE(roll_critical(5, 127));
    ASSERT_TRUE(!roll_critical(5, 128));
    ASSERT_TRUE(roll_critical(6, 127));
    ASSERT_TRUE(!roll_critical(6, 128));
}

TEST(crit_stage_beyond_6_uses_half) {
    // Stages > 6 clamped to 6 → threshold=128
    ASSERT_TRUE(roll_critical(99, 127));
    ASSERT_TRUE(!roll_critical(99, 128));
}

// =============================================================================
// Accuracy check tests
// =============================================================================

TEST(accuracy_always_hit_zero_accuracy) {
    // move_accuracy == 0 → always hits regardless of stages or random
    ASSERT_TRUE(roll_accuracy(0, -6, 6, 255));
}

TEST(accuracy_high_roll_misses) {
    // 60% accuracy, no stages: threshold = 60*255/100 = 153.
    // random=200 → miss
    ASSERT_TRUE(!roll_accuracy(60, 0, 0, 200));
}

TEST(accuracy_low_roll_hits) {
    // 60% accuracy: threshold=153, random=10 → hit
    ASSERT_TRUE(roll_accuracy(60, 0, 0, 10));
}

TEST(accuracy_stage_increases_hit_rate) {
    // acc+6 vs eva-6 → net+6 = 300%: even random=254 hits for 100% moves
    ASSERT_TRUE(roll_accuracy(100, 6, -6, 254));
}

TEST(accuracy_stage_decreases_hit_rate) {
    // acc-6: effective = 100*33/100 = 33; threshold = 33*255/100 = 84 out of 255
    // random=90 > 84 → miss (threshold is strictly <)
    ASSERT_TRUE(!roll_accuracy(100, -6, 0, 90));
    // random=10 < 84 → hit
    ASSERT_TRUE(roll_accuracy(100, -6, 0, 10));
}

// =============================================================================
// Stat calculation tests (HP and non-HP)
// Source: suiCune engine/pokemon/stats.c CalcMonStatC
// Already tested in runtime_test.cpp; minimal coverage here for linkage.
// =============================================================================

TEST(calc_hp_zero_ev_level5) {
    // base=50, iv=0, ev=0, level=5: sqrt(0)=0, sqrt_term=0
    // floor((50+0)*2*5/100) + 5 + 10 = floor(5) + 15 = 20
    ASSERT_EQ(calc_hp(50, 0, 0, 5), 20);
}

TEST(calc_stat_zero_ev_level5) {
    // non-HP: floor((50+0)*2*5/100) + 5 = floor(5) + 5 = 10
    ASSERT_EQ(calc_stat(50, 0, 0, 5), 10);
}

// =============================================================================
// Experience gain tests
// Source: suiCune core.c GiveExperiencePoints
// =============================================================================

TEST(exp_wild_battle) {
    // base_exp=64, level=30: floor(64*30/7) = floor(1920/7) = 274
    ASSERT_EQ(calculate_exp_gain(64, 30, false, 1), 274u);
}

TEST(exp_trainer_battle_boost) {
    // Trainer: exp*3/2 = 274 + 274/2 = 274 + 137 = 411
    ASSERT_EQ(calculate_exp_gain(64, 30, true, 1), 411u);
}

TEST(exp_minimum_one) {
    // base_exp=1, level=1: floor(1/7)=0 → minimum 1
    ASSERT_GE(calculate_exp_gain(1, 1, false, 1), 1u);
}

TEST(exp_level1_wild_pidgey) {
    // Pidgey base_exp=55, level 2: floor(55*2/7)=floor(110/7)=15
    ASSERT_EQ(calculate_exp_gain(55, 2, false, 1), 15u);
}

// =============================================================================
// Capture formula tests
// Source: suiCune engine/items/item_effects.c PokeBallEffect
// =============================================================================

TEST(capture_full_hp_reduces_rate) {
    // Species catch_rate=45, ball_modifier=10 (PokéBall), full HP, no status
    // max_hp=100, current_hp=100:
    //   base = 45*10/10 = 45
    //   max3=300>255 → max3>>=2=75, hp2=200>>=2=50
    //   num = 45*(75-50)/75 = 45*25/75 = 15
    //   status_add=0
    //   final=15
    CaptureParams p;
    p.catch_rate = 45; p.ball_modifier = 10;
    p.max_hp = 100; p.current_hp = 100; p.status = Status::None;
    ASSERT_EQ(calculate_catch_value(p), 15u);
}

TEST(capture_low_hp_increases_rate) {
    // Same params but current_hp=1 (nearly fainted)
    // hp2=2, max3=300>255 → max3>>=2=75, hp2=2>>=2=0→min1
    // num = 45*(75-1)/75 = 45*74/75 = 44 (floor)
    CaptureParams p;
    p.catch_rate = 45; p.ball_modifier = 10;
    p.max_hp = 100; p.current_hp = 1; p.status = Status::None;
    const uint16_t v = calculate_catch_value(p);
    // Low HP should be strictly higher than full HP
    CaptureParams p2 = p; p2.current_hp = 100;
    ASSERT_TRUE(v > calculate_catch_value(p2));
}

TEST(capture_sleep_adds_bonus) {
    // SLP adds +10 to final rate
    CaptureParams p;
    p.catch_rate = 45; p.ball_modifier = 10;
    p.max_hp = 100; p.current_hp = 50; p.status = Status::None;
    const uint16_t no_status = calculate_catch_value(p);
    p.status = Status::Sleep;
    const uint16_t slp = calculate_catch_value(p);
    ASSERT_EQ(slp, static_cast<uint16_t>(std::min(no_status + 10, 255)));
}

TEST(capture_freeze_adds_bonus) {
    CaptureParams p;
    p.catch_rate = 45; p.ball_modifier = 10;
    p.max_hp = 100; p.current_hp = 50; p.status = Status::None;
    const uint16_t no_status = calculate_catch_value(p);
    p.status = Status::Freeze;
    const uint16_t frz = calculate_catch_value(p);
    ASSERT_EQ(frz, static_cast<uint16_t>(std::min(no_status + 10, 255)));
}

TEST(capture_burn_no_bonus) {
    // Crystal vanilla bug: BRN/PSN/PAR get +0, not +5
    CaptureParams p;
    p.catch_rate = 45; p.ball_modifier = 10;
    p.max_hp = 100; p.current_hp = 50; p.status = Status::None;
    const uint16_t no_status = calculate_catch_value(p);
    p.status = Status::Burn;
    ASSERT_EQ(calculate_catch_value(p), no_status);
}

TEST(capture_paralysis_no_bonus) {
    CaptureParams p;
    p.catch_rate = 45; p.ball_modifier = 10;
    p.max_hp = 100; p.current_hp = 50; p.status = Status::None;
    const uint16_t no_status = calculate_catch_value(p);
    p.status = Status::Paralysis;
    ASSERT_EQ(calculate_catch_value(p), no_status);
}

TEST(capture_ball_modifier_scales_rate) {
    // GreatBall modifier=15 vs PokéBall=10
    CaptureParams pb, gb;
    pb.catch_rate = gb.catch_rate = 45;
    pb.ball_modifier = 10; gb.ball_modifier = 15;
    pb.max_hp = gb.max_hp = 100;
    pb.current_hp = gb.current_hp = 50;
    pb.status = gb.status = Status::None;
    ASSERT_TRUE(calculate_catch_value(gb) > calculate_catch_value(pb));
}

TEST(capture_capped_at_255) {
    // Catch rate 255 with large ball modifier — must not exceed 255
    CaptureParams p;
    p.catch_rate = 255; p.ball_modifier = 20;  // UltraBall
    p.max_hp = 100; p.current_hp = 1; p.status = Status::Sleep;
    ASSERT_LE(calculate_catch_value(p), 255u);
}

TEST(roll_capture_succeeds_when_rate_high) {
    // catch_value = 200, random = 150 → 150 <= 200 → caught
    CaptureParams p;
    p.catch_rate = 255; p.ball_modifier = 10;
    p.max_hp = 10; p.current_hp = 1; p.status = Status::Sleep;
    // Ensure rate is high enough
    ASSERT_TRUE(roll_capture(p, 0, 0));
    ASSERT_TRUE(roll_capture(p, 150, 0));
}

TEST(roll_capture_fails_when_rate_low) {
    // catch_value near 0, random = 200 → miss
    CaptureParams p;
    p.catch_rate = 1; p.ball_modifier = 10;
    p.max_hp = 100; p.current_hp = 100; p.status = Status::None;
    // With very low catch rate and high random, should fail
    ASSERT_TRUE(!roll_capture(p, 200, 0));
}

// =============================================================================
// Run/escape tests
// Source: suiCune core.c TryToRunAwayFromBattle
// =============================================================================

TEST(escape_player_faster_always_escapes) {
    // player_speed > wild_speed → always escape, any random
    ASSERT_TRUE(roll_escape(100, 50, 1, 255));
    ASSERT_TRUE(roll_escape(50, 50, 1, 255));  // Equal → escape
}

TEST(escape_formula_attempt1) {
    // player=80, wild=100, attempt=1
    // divisor = 100/4 = 25
    // odds = 80*32/25 + 0*30 = 2560/25 + 0 = 102
    // random=50 < 102 → escape
    ASSERT_TRUE(roll_escape(80, 100, 1, 50));
    // random=200 >= 102 → no escape
    ASSERT_TRUE(!roll_escape(80, 100, 1, 200));
}

TEST(escape_formula_attempt2_adds_30) {
    // attempt=2: odds = 102 + 1*30 = 132
    // random=120 < 132 → escape
    ASSERT_TRUE(roll_escape(80, 100, 2, 120));
    // random=200 >= 132 → no escape
    ASSERT_TRUE(!roll_escape(80, 100, 2, 200));
}

TEST(escape_formula_attempt_overflow_escapes) {
    // After enough attempts, odds exceeds 255 → guaranteed escape
    // player=80, wild=100: base_odds=102; attempt=6 → 102+5*30=252; attempt=7 → 282>255
    ASSERT_TRUE(roll_escape(80, 100, 7, 255));
}

TEST(escape_zero_wild_speed_div4_escapes) {
    // wild_speed=1 → wild_speed/4=0 → guaranteed escape
    ASSERT_TRUE(roll_escape(1, 1, 1, 255));
}

// =============================================================================
// Type effectiveness helpers
// =============================================================================

TEST(type_effectiveness_neutral) {
    TypeChart chart;
    // Default is 10 (neutral)
    ASSERT_EQ(get_type_effectiveness(1, 2, chart), 10u);
}

TEST(type_effectiveness_set_immune) {
    TypeChart chart;
    chart.set_effectiveness(5, 3, 0);  // Type 5 vs Type 3: immune
    ASSERT_EQ(get_type_effectiveness(5, 3, chart), 0u);
}

TEST(combined_effectiveness_dual_type) {
    TypeChart chart;
    chart.set_effectiveness(1, 2, 20);  // super effective vs type 2
    chart.set_effectiveness(1, 3, 20);  // super effective vs type 3
    // Combined: 10*20/10 * 20/10 = 40, ×10 = 400 (4x)
    ASSERT_EQ(get_combined_effectiveness(1, 2, 3, chart), 400u);
}

TEST(combined_effectiveness_single_type) {
    TypeChart chart;
    chart.set_effectiveness(1, 2, 20);  // 2x
    // Single type (type2 == type1): should only apply once → 200
    ASSERT_EQ(get_combined_effectiveness(1, 2, 2, chart), 200u);
}

TEST(combined_effectiveness_immune) {
    TypeChart chart;
    chart.set_effectiveness(1, 2, 0);   // immune vs type 2
    chart.set_effectiveness(1, 3, 20);  // super vs type 3
    // 10*0/10 = 0, then 0*20/10 = 0 → immune wins → 0
    ASSERT_EQ(get_combined_effectiveness(1, 2, 3, chart), 0u);
}

// =============================================================================
// Main
// =============================================================================

// =============================================================================
// Trainer AI tests — helpers and fixtures
// =============================================================================

#include "engine/battle/battle.hpp"
#include "engine/battle/trainer_ai.hpp"
#include "engine/party/party.hpp"

namespace {

Party make_test_party() {
    Party p;
    Pokemon mon{};
    mon.species    = 1;
    mon.level      = 50;
    mon.current_hp = 100;
    mon.max_hp     = 100;
    mon.attack = mon.defense = mon.speed = mon.special_attack = mon.special_defense = 50;
    p.add(mon);
    return p;
}

Registries make_test_registries() {
    Registries reg;
    TypeData t1; t1.id = 1; t1.name = "Normal"; reg.types.register_entry(1, t1);
    TypeData t2; t2.id = 2; t2.name = "Fire";   reg.types.register_entry(2, t2);
    TypeData t3; t3.id = 3; t3.name = "Water";  reg.types.register_entry(3, t3);
    TypeData t4; t4.id = 4; t4.name = "Grass";  reg.types.register_entry(4, t4);
    reg.type_chart.set_effectiveness(2, 4, 20);
    reg.type_chart.set_effectiveness(3, 2, 20);
    reg.type_chart.set_effectiveness(2, 3,  5);
    reg.type_chart.set_effectiveness(1, 1, 10);

    MoveData tackle{}; tackle.id = 1; tackle.name = "Tackle"; tackle.type = 1;
    tackle.power = 40; tackle.accuracy = 100; tackle.pp = 35;
    tackle.category = MoveCategory::Physical; tackle.effect_id = 0; tackle.priority = 0;
    reg.moves.register_entry(1, tackle);

    MoveData ember{}; ember.id = 2; ember.name = "Ember"; ember.type = 2;
    ember.power = 40; ember.accuracy = 100; ember.pp = 25;
    ember.category = MoveCategory::Special; ember.effect_id = 0; ember.priority = 0;
    reg.moves.register_entry(2, ember);

    MoveData growl{}; growl.id = 4; growl.name = "Growl"; growl.type = 1;
    growl.power = 0; growl.accuracy = 100; growl.pp = 40;
    growl.category = MoveCategory::Status; growl.effect_id = 18; growl.priority = 0;
    reg.moves.register_entry(4, growl);

    MoveData toxic{}; toxic.id = 5; toxic.name = "Toxic"; toxic.type = 1;
    toxic.power = 0; toxic.accuracy = 90; toxic.pp = 10;
    toxic.category = MoveCategory::Status; toxic.effect_id = 34; toxic.priority = 0;
    reg.moves.register_entry(5, toxic);

    MoveData recover{}; recover.id = 6; recover.name = "Recover"; recover.type = 1;
    recover.power = 0; recover.accuracy = 0; recover.pp = 10;
    recover.category = MoveCategory::Status; recover.effect_id = 33; recover.priority = 0;
    reg.moves.register_entry(6, recover);

    SpeciesData charman{}; charman.id = 4; charman.name = "Charmander";
    charman.type1 = 2; charman.type2 = 2;
    charman.base_stats = {39, 52, 43, 65, 60, 50};
    charman.catch_rate = 45; charman.base_exp = 64;
    reg.species.register_entry(4, charman);

    SpeciesData bulba{}; bulba.id = 1; bulba.name = "Bulbasaur";
    bulba.type1 = 4; bulba.type2 = 4;
    bulba.base_stats = {45, 49, 49, 65, 65, 45};
    bulba.catch_rate = 45; bulba.base_exp = 64;
    reg.species.register_entry(1, bulba);

    reg.freeze_all();
    return reg;
}

BattlePokemon make_test_bp(SpeciesId species, TypeId t1, TypeId t2,
                           const std::array<MoveId, 4>& moves,
                           int16_t hp = 100, int16_t max_hp = 100) {
    BattlePokemon bp{};
    bp.species = species; bp.type1 = t1; bp.type2 = t2;
    bp.level = 50;
    bp.stats.hp = hp; bp.stats.max_hp = max_hp;
    bp.stats.attack = bp.stats.defense = bp.stats.speed = 50;
    bp.stats.special_attack = bp.stats.special_defense = 50;
    bp.base_stats = bp.stats;
    for (size_t i = 0; i < 4; ++i) {
        bp.moves[i].move = moves[i];
        bp.moves[i].pp = bp.moves[i].max_pp = 10;
    }
    return bp;
}

} // anonymous namespace

TEST(ai_types_prefers_super_effective) {
    auto party = make_test_party();
    auto reg   = make_test_registries();
    Battle battle(BattleType::Wild, party, reg);
    BattlePokemon self = make_test_bp(4, 2, 2, {1, 2, MOVE_NONE, MOVE_NONE});
    BattlePokemon opp  = make_test_bp(1, 4, 4, {1, MOVE_NONE, MOVE_NONE, MOVE_NONE});
    AIContext ctx{battle, self, opp, true, false, false, false, 0, {}, {}};
    VanillaCrystalAI ai(VanillaAI::SMART);
    const ActionFight& af = std::get<ActionFight>(ai.decide(ctx).action);
    ASSERT_EQ(af.move_slot, 1u);
}

TEST(ai_types_avoids_immune) {
    auto party = make_test_party();
    Registries reg2;
    TypeData tnorm; tnorm.id = 1; tnorm.name = "Normal"; reg2.types.register_entry(1, tnorm);
    TypeData tghost; tghost.id = 5; tghost.name = "Ghost"; reg2.types.register_entry(5, tghost);
    TypeData tfire;  tfire.id  = 2; tfire.name  = "Fire";  reg2.types.register_entry(2, tfire);
    reg2.type_chart.set_effectiveness(1, 5, 0);
    reg2.type_chart.set_effectiveness(2, 5, 10);
    MoveData tackle2{}; tackle2.id = 1; tackle2.name = "Tackle"; tackle2.type = 1;
    tackle2.power = 40; tackle2.accuracy = 100; tackle2.pp = 35;
    tackle2.category = MoveCategory::Physical; tackle2.priority = 0;
    reg2.moves.register_entry(1, tackle2);
    MoveData ember2{}; ember2.id = 2; ember2.name = "Ember"; ember2.type = 2;
    ember2.power = 40; ember2.accuracy = 100; ember2.pp = 25;
    ember2.category = MoveCategory::Special; ember2.priority = 0;
    reg2.moves.register_entry(2, ember2);
    reg2.freeze_all();
    Battle battle(BattleType::Wild, party, reg2);
    BattlePokemon self = make_test_bp(4, 2, 2, {1, 2, MOVE_NONE, MOVE_NONE});
    BattlePokemon opp  = make_test_bp(99, 5, 5, {MOVE_NONE, MOVE_NONE, MOVE_NONE, MOVE_NONE});
    AIContext ctx{battle, self, opp, true, false, false, false, 0, {}, {}};
    VanillaCrystalAI ai(VanillaAI::SMART);
    const ActionFight& af = std::get<ActionFight>(ai.decide(ctx).action);
    ASSERT_EQ(af.move_slot, 1u);
}

TEST(ai_basic_discourages_toxic_when_already_poisoned) {
    auto party = make_test_party();
    auto reg   = make_test_registries();
    Battle battle(BattleType::Wild, party, reg);
    BattlePokemon self = make_test_bp(4, 2, 2, {5, 1, MOVE_NONE, MOVE_NONE});
    BattlePokemon opp  = make_test_bp(1, 4, 4, {MOVE_NONE, MOVE_NONE, MOVE_NONE, MOVE_NONE});
    opp.status = Status::Poison;
    AIContext ctx{battle, self, opp, true, false, false, false, 0, {}, {}};
    VanillaCrystalAI ai(VanillaAI::BASIC);
    const ActionFight& af = std::get<ActionFight>(ai.decide(ctx).action);
    ASSERT_EQ(af.move_slot, 1u);
}

TEST(ai_smart_encourages_recover_at_low_hp) {
    auto party = make_test_party();
    auto reg   = make_test_registries();
    Battle battle(BattleType::Wild, party, reg);
    BattlePokemon self = make_test_bp(4, 2, 2, {6, 1, MOVE_NONE, MOVE_NONE}, 20, 100);
    BattlePokemon opp  = make_test_bp(1, 4, 4, {MOVE_NONE, MOVE_NONE, MOVE_NONE, MOVE_NONE});
    AIContext ctx{battle, self, opp, true, false, false, false, 0, {}, {}};
    VanillaCrystalAI ai(VanillaAI::SMART);
    const ActionFight& af = std::get<ActionFight>(ai.decide(ctx).action);
    ASSERT_EQ(af.move_slot, 0u);
}

TEST(ai_smart_discourages_recover_at_full_hp) {
    auto party = make_test_party();
    auto reg   = make_test_registries();
    Battle battle(BattleType::Wild, party, reg);
    BattlePokemon self = make_test_bp(4, 2, 2, {6, 1, MOVE_NONE, MOVE_NONE}, 100, 100);
    BattlePokemon opp  = make_test_bp(1, 4, 4, {MOVE_NONE, MOVE_NONE, MOVE_NONE, MOVE_NONE});
    AIContext ctx{battle, self, opp, true, false, false, false, 0, {}, {}};
    VanillaCrystalAI ai(VanillaAI::SMART);
    const ActionFight& af = std::get<ActionFight>(ai.decide(ctx).action);
    ASSERT_EQ(af.move_slot, 1u);
}

TEST(ai_basic_picks_valid_slot_when_all_neutral) {
    auto party = make_test_party();
    auto reg   = make_test_registries();
    Battle battle(BattleType::Wild, party, reg);
    BattlePokemon self = make_test_bp(4, 1, 1, {1, 1, MOVE_NONE, MOVE_NONE});
    BattlePokemon opp  = make_test_bp(1, 1, 1, {MOVE_NONE, MOVE_NONE, MOVE_NONE, MOVE_NONE});
    AIContext ctx{battle, self, opp, true, false, false, false, 0, {}, {}};
    VanillaCrystalAI ai(VanillaAI::BASIC);
    AIDecision d = ai.decide(ctx);
    ASSERT_TRUE(std::holds_alternative<ActionFight>(d.action));
    ASSERT_TRUE(std::get<ActionFight>(d.action).move_slot < 2u);
}

TEST(ai_registry_creates_known_behaviors) {
    AIRegistry reg;
    auto basic = reg.create(VanillaAI::BASIC);
    ASSERT_TRUE(basic != nullptr);
    ASSERT_EQ(basic->behavior_id(), VanillaAI::BASIC);
    auto smart = reg.create(VanillaAI::SMART);
    ASSERT_TRUE(smart != nullptr);
    ASSERT_EQ(smart->behavior_id(), VanillaAI::SMART);
}

TEST(ai_registry_unknown_id_falls_back_to_basic) {
    AIRegistry reg;
    auto unknown = reg.create(9999);
    ASSERT_TRUE(unknown != nullptr);
    ASSERT_EQ(unknown->behavior_id(), VanillaAI::BASIC);
}

TEST(ai_registry_trainer_override) {
    AIRegistry reg;
    reg.set_trainer_override(42, VanillaAI::GYM_LEADER);
    ASSERT_EQ(reg.get_ai_for_trainer(42, 0), static_cast<AIBehaviorId>(VanillaAI::GYM_LEADER));
    ASSERT_EQ(reg.get_ai_for_trainer(99, 0), static_cast<AIBehaviorId>(VanillaAI::BASIC));
}

TEST(ai_registry_class_override) {
    AIRegistry reg;
    reg.set_class_override(5, VanillaAI::ELITE_FOUR);
    ASSERT_EQ(reg.get_ai_for_trainer(0, 5), static_cast<AIBehaviorId>(VanillaAI::ELITE_FOUR));
    reg.set_trainer_override(77, VanillaAI::CHAMPION);
    ASSERT_EQ(reg.get_ai_for_trainer(77, 5), static_cast<AIBehaviorId>(VanillaAI::CHAMPION));
}

TEST(ai_registry_freeze_prevents_mutation) {
    AIRegistry reg;
    reg.freeze();
    bool threw = false;
    try { reg.set_trainer_override(1, VanillaAI::BASIC); }
    catch (const std::exception&) { threw = true; }
    ASSERT_TRUE(threw);
}

TEST(ai_registry_lists_registered_behaviors) {
    AIRegistry reg;
    ASSERT_GE(static_cast<int>(reg.list_registered().size()), 9);
}

// =============================================================================
// Main
// =============================================================================
int main(int /*argc*/, char* /*argv*/[]) {
    std::cout << "=== Battle Calculator + AI Tests ===\n";

    RUN(battle_test_binary_links);

    RUN(stat_stage_neutral);
    RUN(stat_stage_plus1);
    RUN(stat_stage_plus2);
    RUN(stat_stage_plus6);
    RUN(stat_stage_minus1);
    RUN(stat_stage_minus6);
    RUN(stat_stage_clamped_beyond_6);
    RUN(stat_stage_minimum_1);
    RUN(stat_stage_capped_999);

    RUN(accuracy_stage_neutral);
    RUN(accuracy_stage_plus1);
    RUN(accuracy_stage_minus1);
    RUN(accuracy_stage_plus6);
    RUN(accuracy_stage_minus6);
    RUN(accuracy_stage_net_clamped);

    RUN(damage_basic_known_value);
    RUN(damage_critical_doubles_pre_floor);
    RUN(damage_stab_adds_half);
    RUN(damage_super_effective_2x);
    RUN(damage_immune_zero);
    RUN(damage_not_very_effective_half);
    RUN(damage_burn_halves_result);
    RUN(damage_capped_at_999);
    RUN(damage_minimum_is_2);
    RUN(damage_zero_power_returns_zero);
    RUN(damage_stat_truncation_over_255);

    RUN(weather_modifier_boost);
    RUN(weather_modifier_penalty);
    RUN(weather_modifier_not_applied);
    RUN(weather_modifier_minimum_1);

    RUN(crit_roll_below_threshold_hits);
    RUN(crit_roll_at_threshold_misses);
    RUN(crit_stage1_threshold_32);
    RUN(crit_stage4_half_chance);
    RUN(crit_stage_beyond_6_uses_half);

    RUN(accuracy_always_hit_zero_accuracy);
    RUN(accuracy_high_roll_misses);
    RUN(accuracy_low_roll_hits);
    RUN(accuracy_stage_increases_hit_rate);
    RUN(accuracy_stage_decreases_hit_rate);

    RUN(calc_hp_zero_ev_level5);
    RUN(calc_stat_zero_ev_level5);

    RUN(exp_wild_battle);
    RUN(exp_trainer_battle_boost);
    RUN(exp_minimum_one);
    RUN(exp_level1_wild_pidgey);

    RUN(capture_full_hp_reduces_rate);
    RUN(capture_low_hp_increases_rate);
    RUN(capture_sleep_adds_bonus);
    RUN(capture_freeze_adds_bonus);
    RUN(capture_burn_no_bonus);
    RUN(capture_paralysis_no_bonus);
    RUN(capture_ball_modifier_scales_rate);
    RUN(capture_capped_at_255);
    RUN(roll_capture_succeeds_when_rate_high);
    RUN(roll_capture_fails_when_rate_low);

    RUN(escape_player_faster_always_escapes);
    RUN(escape_formula_attempt1);
    RUN(escape_formula_attempt2_adds_30);
    RUN(escape_formula_attempt_overflow_escapes);
    RUN(escape_zero_wild_speed_div4_escapes);

    RUN(type_effectiveness_neutral);
    RUN(type_effectiveness_set_immune);
    RUN(combined_effectiveness_dual_type);
    RUN(combined_effectiveness_single_type);
    RUN(combined_effectiveness_immune);

    // AI tests
    RUN(ai_types_prefers_super_effective);
    RUN(ai_types_avoids_immune);
    RUN(ai_basic_discourages_toxic_when_already_poisoned);
    RUN(ai_smart_encourages_recover_at_low_hp);
    RUN(ai_smart_discourages_recover_at_full_hp);
    RUN(ai_basic_picks_valid_slot_when_all_neutral);
    RUN(ai_registry_creates_known_behaviors);
    RUN(ai_registry_unknown_id_falls_back_to_basic);
    RUN(ai_registry_trainer_override);
    RUN(ai_registry_class_override);
    RUN(ai_registry_freeze_prevents_mutation);
    RUN(ai_registry_lists_registered_behaviors);

    std::cout << "\n=== Results ===\n";
    std::cout << "Passed: " << g_passed << "\n";
    std::cout << "Failed: " << g_failed << "\n";
    return g_failed > 0 ? 1 : 0;
}
