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

} // namespace crystal::oracle
