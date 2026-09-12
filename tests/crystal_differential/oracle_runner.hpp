// tests/crystal_differential/oracle_runner.hpp
//
// Crystal battle differential oracle — public API.
//
// Runs selected move(s) through the full semantic Crystal machine code path
// via SameBoy, normalizes outputs, and diffs against Enginemon live execution.
//
// ROM IMMUTABILITY CONTRACT:
//   The exact SHA-verified Crystal ROM bytes are passed to GB_load_rom_from_buffer
//   unmodified. No ROM bytes are read back or patched at any point. Entry into
//   each routine is established via CPU register/bank state only.
//
// PRESENTATION INTERCEPT:
//   Each move case registers an allowlisted presentation sink (e.g. AnimateCurrentMove).
//   When the SM83 execution callback fires at that PC, the GB_run loop exits.
//   The semantic Crystal WRAM outputs are read at that boundary.
//   No Crystal ROM bytes are modified to achieve this.
//
// PARALLELISM:
//   Each worker owns a dedicated SameBoy GB_gameboy_t + all associated state.
//   Workers share only: the read-only ROM byte vector, the sym cache, and the
//   Enginemon move/rules data loaded once from the profile.
//   Result ordering is deterministic regardless of --jobs value.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace crystal::oracle {

// ============================================================================
// RunnerConfig — build the set of cases to execute.
// ============================================================================
struct RunnerConfig {
    std::vector<uint16_t> move_ids;  // empty = all registered moves
    int jobs = 1;

    RunnerConfig& with_moves(std::vector<uint16_t> ids) {
        move_ids = std::move(ids);
        return *this;
    }
    RunnerConfig& with_jobs(int n) {
        jobs = n;
        return *this;
    }
};

// ============================================================================
// runner_main — parse argv and run.
//
// argv format (after exe name):
//   <rom_path> <sym_path> [--jobs N] [--all] [--move <id> ...]
//
// If a RunnerConfig is passed in, its move_ids are used as defaults
// (--all or --move on the command line overrides).
//
// Exit codes:
//   0  — all cases MATCH
//   1  — at least one ENGINEMON_MISMATCH or HARNESS_ERROR
// ============================================================================
int runner_main(int argc, char* argv[], RunnerConfig defaults = {});

} // namespace crystal::oracle
