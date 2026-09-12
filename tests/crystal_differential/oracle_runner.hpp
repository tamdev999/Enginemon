// tests/crystal_differential/oracle_runner.hpp
//
// Crystal battle differential oracle — public API
//
// ROM IMMUTABILITY CONTRACT
//   The exact SHA-verified Crystal ROM bytes are passed to GB_load_rom_from_buffer
//   unmodified. No ROM bytes are read back or patched after that call. Entry into
//   each routine is established via CPU register and bank-state writes only.
//
// EXIT CODES
//   0  all selected cases MATCH
//   1  one or more ENGINEMON_MISMATCH (harness ran correctly, Crystal != Enginemon)
//   2  HARNESS_ERROR / startup / oracle failure (takes precedence over code 1)
//   3  invalid CLI / unregistered move / bad arguments

#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace crystal::oracle {

// ============================================================================
// Exit codes — returned by runner_main, used as process exit code.
// ============================================================================
enum ExitCode : int {
    EXIT_ALL_MATCH        = 0,
    EXIT_MISMATCH         = 1,
    EXIT_HARNESS_ERROR    = 2,
    EXIT_INVALID_ARGS     = 3,
};

// ============================================================================
// RunnerConfig — specifies which moves to run and how many parallel workers.
// move_ids: empty means the caller controls selection via CLI flags.
// ============================================================================
struct RunnerConfig {
    std::vector<uint16_t> move_ids;
    int jobs = 1;

    RunnerConfig& with_moves(std::vector<uint16_t> ids) {
        move_ids = std::move(ids); return *this;
    }
    RunnerConfig& with_jobs(int n) { jobs = n; return *this; }
};

// ============================================================================
// runner_main
//
// Positional: <rom_path> <sym_path>  (both required)
// Options:    --jobs N | --all | --move <id>... | --help
//
// If a RunnerConfig is passed in, its move_ids are used as the default selection
// when no --all / --move flags are present on the command line.
//
// Returns one of the ExitCode values above.
// ============================================================================
int runner_main(int argc, char* argv[], RunnerConfig defaults = {});

} // namespace crystal::oracle
