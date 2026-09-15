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

} // namespace crystal::oracle
