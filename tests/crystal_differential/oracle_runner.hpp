// tests/crystal_differential/oracle_runner.hpp
//
// Crystal battle differential oracle â€” public API
//
// ROM IMMUTABILITY CONTRACT
//   The exact SHA-verified Crystal ROM bytes are passed to GB_load_rom_from_buffer
//   unmodified. No ROM bytes are read back or patched after that call. Entry into
//   each routine is established via CPU register and bank-state writes only.
//
// EXIT CODES
//   0  all selected cases MATCH
//   1  one or more ENGINEMON_MISMATCH (harness ran correctly, Crystal != Enginemon)
//   2  HARNESS_ERROR / startup / oracle failure  (takes precedence over code 1)
//   3  invalid CLI / unregistered move / bad arguments

#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace crystal::oracle {

// ============================================================================
// Process exit codes.
// ============================================================================
enum ExitCode : int {
    EXIT_ALL_MATCH     = 0,
    EXIT_MISMATCH      = 1,
    EXIT_HARNESS_ERROR = 2,
    EXIT_INVALID_ARGS  = 3,
};

// ============================================================================
// RunnerConfig â€” caller-supplied defaults; CLI flags override.
// ============================================================================
struct RunnerConfig {
    std::vector<uint16_t> move_ids; // empty = defer to CLI
    int  jobs    = 1;
    bool verbose = false;

    bool accuracy_sweep_a = false; // --accuracy-sweep-a: per-move 256-byte acc sweep
    bool accuracy_sweep_b = false; // --accuracy-sweep-b: Screech ACC/EVA stage matrix (coordinator)
    bool part_b_worker    = false; // --part-b-row N: single-row worker mode (spawned by coordinator)
    // Part B range limits (Crystal raw stages, valid domain 1..13, neutral=7).
    // Defaults cover the full domain. CLI args --acc-raw-min/max/--eva-raw-min/max override.
    int sweep_acc_min = 1;
    int sweep_acc_max = 13;
    int sweep_eva_min = 1;
    int sweep_eva_max = 13;
    RunnerConfig& with_moves(std::vector<uint16_t> ids){ move_ids=std::move(ids); return *this; }
    RunnerConfig& with_jobs(int n)      { jobs=n;       return *this; }
    RunnerConfig& with_verbose(bool v)  { verbose=v;    return *this; }
};

// ============================================================================
// runner_main
//
// Positional:  <rom_path>  <sym_path>
// Options:     --all | --move <id>... | --jobs N | --verbose | --list | --help
//
// Returns one of the ExitCode values above.
// ============================================================================
int runner_main(int argc, char* argv[], RunnerConfig defaults = {});

// ============================================================================
// run_harness_negative_tests
//
// Exercises the six fail-closed harness paths that cannot be reached via normal
// CLI usage. Intended for use by oracle_harness_negative_test only.
//
// rom_path:  path to the pinned Crystal ROM (SHA-verified internally)
// sym_path:  path to the pinned .sym file
// verbose:   if true, prints each test name and result to stdout
//
// Returns 0 if ALL six negative controls produce HARNESS_ERROR as expected.
// Returns non-zero on any unexpected outcome (test infrastructure failure).
// ============================================================================
int run_harness_negative_tests(const char* rom_path, const char* sym_path,
                                bool verbose = false);

// ============================================================================
// run_accuracy_sweep_benchmark
//
// Measures the real cost breakdown of the certified Part B accuracy sweep
// for ONE complete ACC row (13 EVA × 256 RNG × 4 poison = 3,328 Crystal runs).
//
// Instruments:
//   - GB_init + ROM load time
//   - Fixture/setup time
//   - GB_run (Crystal execution) time
//   - Enginemon execute_turn time
//   - Total per-row time (baseline: fresh init per run)
//   - Total per-row time (reuse: one init + snapshot restore per run)
//
// Validates that reuse-path outputs are identical to baseline (poison=0x00).
//
// acc_raw: Crystal raw stage value for the ACC row to benchmark (1..13, 7=neutral).
// Returns 0 on success, 1 on failure.
// ============================================================================
int run_accuracy_sweep_benchmark(const char* rom_path, const char* sym_path,
                                  int acc_raw = 7);

// ============================================================================
// run_checkhit_pilot
//
// Direct BattleCommand_CheckHit entry pilot (Pilot A + Pilot B).
// Returns 0 on full pass, 1 on any failure or semantic disagreement.
// ============================================================================
int run_checkhit_pilot(const char* rom_path, const char* sym_path);

// ============================================================================
// run_damagecalc_pilot
//
// Direct BattleCommand_DamageCalc (0D:5612) entry pilot.
// Certifies that direct-called DamageCalc produces identical output to
// the full-script path for several ordinary damaging move cases.
// Returns 0 on full pass, 1 on any failure.
// ============================================================================
int run_damagecalc_pilot(const char* rom_path, const char* sym_path);

// ============================================================================
// run_damagecalc_matrix
//
// Crystal BattleCommand_DamageCalc (0D:5612) vs enginemon::calculate_damage.
//
// Sweep A — single-axis exhaustive (all crit=0 and crit=1):
//   power  1..255 (fixed level=50, atk=110, def=110)
//   level  1..100 (fixed power=80, atk=110, def=110)
//   attack 1..255 (fixed power=80, level=50, def=110)
//   defense 1..255 (fixed power=80, level=50, atk=110)
//
// Grid B — boundary interaction (atk × def, representative edge values):
//   {1,2,3,10,50,100,127,128,254,255} × {1,2,3,10,50,100,127,128,254,255}
//   fixed power=80, level=50, crit=0 and crit=1
//
// Per logical case: 4 Crystal poison executions must agree, then compared
// against enginemon::calculate_damage with stab=false, type_effectiveness=100.
//
// Returns 0 if all logical cases match, 1 on any mismatch/harness error.
// ============================================================================
int run_damagecalc_matrix(const char* rom_path, const char* sym_path);

// ============================================================================
// run_damagecalc_atk_def_grid
//
// Exhaustive attack×defense sweep: attack 1..255 × defense 1..255 × crit {0,1}
// = 130,050 logical cases, 520,200 Crystal executions.
// Fixed: power=80, level=50, no STAB, no type, no item, no variation.
// Entry: BattleCommand_DamageCalc (0D:5612). Sink: EndMoveEffect.
// Parallelized by attack row using std::async with `jobs` workers.
// Returns 0 if all cases match, 1 on any mismatch/harness error.
// ============================================================================
int run_damagecalc_atk_def_grid(const char* rom_path, const char* sym_path,
                                 int jobs = 1, int shard_max_atk = 255);

// ============================================================================
// run_damagecalc_level_power_grid
//
// Exhaustive level x power sweep: level 1..100 x power 1..255 x crit {0,1}
// = 51,000 logical cases, 204,000 Crystal executions.
// Fixed: attack=110, defense=110 (certified baseline from atk x def grid).
// Entry: BattleCommand_DamageCalc (0D:5612). Sink: EndMoveEffect.
// --shard-max-lv N: limit level sweep to 1..N (determinism shard tests).
// Returns 0 if all cases match, 1 on any mismatch/harness error.
// ============================================================================
int run_damagecalc_level_power_grid(const char* rom_path, const char* sym_path,
                                     int jobs = 1, int shard_max_lv = 100);

// ============================================================================
// run_damagecalc_edge_grid
//
// Dense edge grid across all four DamageCalc arithmetic inputs simultaneously.
// level={1,2,3,10,50,100} x power={1,2,3,10,50,100,127,128,254,255}
// x attack=same x defense=same x crit={0,1}
// = 12,000 logical cases, 48,000 Crystal executions.
// Purpose: catch combined intermediate-width / truncation / operation-order
// differences that separate 2-D sweeps cannot detect.
// Returns 0 if all cases match, 1 on any mismatch/harness error.
// ============================================================================
int run_damagecalc_edge_grid(const char* rom_path, const char* sym_path);

// ============================================================================
// run_damagestats_crit_pilot
//
// Live-proves the DamageStats crit-stat divergence between Crystal and Enginemon.
// Tests 6 (atk_stage, def_stage) crit pairs × screen OFF/ON = 12 crit cases
// plus 2 non-crit neutral controls.
// Physical Return (move 216), P_ATK=110, E_DEF=110, P_LEVEL=50.
// Crystal entry: BattleCommand_DamageStats (0D:52DC).
// Reports Crystal B,C at DamageCalc entry for each case.
// Returns 0 if all cases ran (exit code independent of match/mismatch).
// ============================================================================
int run_damagestats_crit_pilot(const char* rom_path, const char* sym_path);

// ============================================================================
// run_damagestats_direct_pilot
//
// DamageStats direct semantic pilot. Crystal side:
//   1. Set base stats + stage raw bytes (7=neutral, 1..13 range).
//   2. Direct-call real CalcPlayerStats/CalcEnemyStats to populate wPlayerStats.
//   3. Direct-call BattleCommand_DamageStats (0D:52DC).
//   4. Capture B/C at DamageCalc entry (0D:5612) -- no harness stage math.
// Enginemon side: observe DamageParams.attack_stat/defense_stat via observer.
// Returns 0 if all runs succeed (harness error = 1).
// ============================================================================
int run_damagestats_direct_pilot(const char* rom_path, const char* sym_path);

// ============================================================================
// run_damagestats_boundary_sweep_with_bases
//
// Same sweep as run_damagestats_boundary_sweep but with caller-specified
// base attack and defense stats. Used for base-stat coincidence audit.
// ============================================================================
int run_damagestats_boundary_sweep_with_bases(
    const char* rom_path, const char* sym_path,
    uint16_t base_atk, uint16_t base_def);
// For every attacker ATK stage raw 1..13 × defender DEF stage raw 1..13
// × crit {0,1} × Reflect {OFF,ON} = 676 logical cases.
// 4 poison patterns per DamageStats execution = 2704 Crystal executions.
// Per logical case: runs real CalcPlayerStats, real CalcEnemyStats, then
// real DoMove (full script) for each of the 4 poison patterns.
// Captures pre-TruncateHL_BC values at 0D:533F (HL=attack, BC=defense)
// and compares against Enginemon DamageParams.attack_stat/defense_stat.
// No harness stage/screen/crit formula.
// Returns 0 all match, 1 mismatches found (harness healthy), 2 harness error.
// ============================================================================
int run_damagestats_boundary_sweep(const char* rom_path, const char* sym_path);
// run_damagestats_boundary_sweep_special
// run_damagestats_boundary_sweep_special_with_bases
//
// Same sweep structure as the physical variants but exercising the SPECIAL
// stat-selection path in PlayerAttackDamage:
//   wEnemyMonSpclDef → BC (with Light Screen bit 3 doubling)
//   wBattleMonSpclAtk → HL (via CheckDamageStatsCritical, wPlayerSAtkLevel/wEnemySDefLevel)
// Move: Surf (id=57), Water type → triggers special path (type >= SPECIAL=20).
// SpAtk stage index 3 (wPlayerSAtkLevel = wPlayerStatLevels+3 = 0xC6CF).
// SpDef stage index 4 (wEnemySDefLevel  = wEnemyStatLevels+4  = 0xC6D8).
// ============================================================================
int run_damagestats_boundary_sweep_special(const char* rom_path, const char* sym_path);
int run_damagestats_boundary_sweep_special_with_bases(
    const char* rom_path, const char* sym_path,
    uint16_t base_spatk, uint16_t base_spdef);
// ============================================================================
// run_stab_boundary_pilot
//
// Certifies BattleCommand_Stab (0D:46D2..0D:47C7) vs Enginemon
// execute_move_damaging for all owned modifiers.
// Crystal sink at 0D:47C7; Enginemon HP delta with 100% variation.
// Returns 0 on success (harness healthy), 2 on harness error.
// ============================================================================
int run_stab_boundary_pilot(const char* rom_path, const char* sym_path);
// run_type_effectiveness_diagnostic
//
// Diagnoses why type effectiveness was not applied in a77b0c3.
// Observes move type, attacker/defender types, TypeChart lookup results,
// pre/post-type damage for neutral, 2x, 0.5x, and immunity cases.
// No Crystal execution. No harness formulas.
// ============================================================================
int run_type_effectiveness_diagnostic(const char* rom_path, const char* sym_path);
// run_stab_type_sweep
//
// Exhaustive STAB/type-effectiveness certification over all 17 combat types,
// all non-neutral matchup pairs, dual-type combos, STAB on/off.
// ROM-derived TypeChart. Real supported moves per type. No harness formulas.
// ============================================================================
int run_stab_type_sweep(const char* rom_path, const char* sym_path);

// run_stab_modifier_pilot
//
// Reruns the Stab modifier pilot with TypeChart populated from ROM rules.
// Cases: neutral, STAB, 2x, 0.5x, immunity, STAB+2x, STAB+0.5x, dual-type.
// Observes Eng pre-mod (calculate_damage inside observer) and post-mod (HP delta).
// Returns 0 on all matches, 1 on mismatches (harness healthy), 2 on error.
// ============================================================================
int run_stab_modifier_pilot(const char* rom_path, const char* sym_path);
// run_dual_type_floor_proof
//
// Proves dual-type sequential-flooring divergence using real Ice Beam (id=58).
// Crystal: per-type-pass snapshots at 0D:47AB (.ok label).
// Enginemon: calculate_damage(dp) in observer + HP delta with variation=100%.
// ============================================================================
int run_dual_type_floor_proof(const char* rom_path, const char* sym_path);
// run_stab_arithmetic_sweep
//
// Certifies STAB/type-effectiveness arithmetic over every input damage value
// 1..255 for 11 structural type configurations (neutral, ×0.5, ×2, immune,
// STAB+neutral, STAB+×0.5, STAB+×2, dual-cancel, dual-NVE×NVE, dual-SE×SE,
// dual-cancel-SE×NVE). ROM-derived TypeChart. Direct wCurDamage seeding.
// PostTypeObserver for Enginemon. 4 poison patterns.
// Returns 0 all match, 1 mismatches (harness healthy), 2 harness error.
// ============================================================================
int run_stab_arithmetic_sweep(const char* rom_path, const char* sym_path);

// run_weather_damage_sweep
//
// Certifies Crystal DoWeatherModifiers vs Enginemon apply_weather_modifier
// over input damage 1..255 for 7 structural weather configurations.
// Rain+Water, Rain+Fire, Sun+Fire, Sun+Water, no_weather, Rain+SolarBeam, Sun+SolarBeam.
// Crystal: direct BattleCommand_Stab entry (0D:46D2), wCurDamage seeded in fixture.
// Enginemon: set_field_weather seam + set_pre_type_damage_override + post_type_observer.
// Documents the Rain+SolarBeam WeatherMoveModifiers dead path on Enginemon side.
// Returns 0 all match, 1 expected dead-path mismatch (harness healthy), 2 harness error.
// ============================================================================
int run_weather_damage_sweep(const char* rom_path, const char* sym_path);

// run_badge_boost_sweep
//
// Certifies Crystal DoBadgeTypeBoosts against Enginemon's production path.
// Proves all 5 gating conditions (player turn, no link, no Battle Tower, matching badge, type match).
// Sweeps input damage 1..255 for badge inactive and badge active.
// Two badge/type pairs: Cascade(Water) and Volcano(Fire).
// Documents that Enginemon does not implement badge boost (all active inputs mismatch).
// Returns 0 if gating passes and active mismatches are expected, 1 on mismatch, 2 on harness error.
// ============================================================================
int run_badge_boost_sweep(const char* rom_path, const char* sym_path);

// run_type_item_sweep
//
// Certifies Crystal type-boost held-item behavior vs Enginemon production path.
// Documents that Crystal applies item inside BattleCommand_DamageCalc (BEFORE Stab),
// while Enginemon applies item after weather+STAB+type in execute_move_damaging.
// Runs 11 structural cases: 5 item-only + 6 ordering tests.
// Returns 0 all match, 1 on ordering mismatch (harness healthy), 2 on harness error.
// ============================================================================
int run_type_item_sweep(const char* rom_path, const char* sym_path);

} // namespace crystal::oracle
