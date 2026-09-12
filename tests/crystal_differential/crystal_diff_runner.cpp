// tests/crystal_differential/crystal_diff_runner.cpp
//
// Standalone parallel Crystal battle differential runner.
//
// Usage:
//   crystal_diff_runner <rom_path> <sym_path> [--jobs N] [--all] [--move <id> ...]
//
// Examples:
//   crystal_diff_runner crystal.gbc pokecrystal11.sym --all
//   crystal_diff_runner crystal.gbc pokecrystal11.sym --move 114
//   crystal_diff_runner crystal.gbc pokecrystal11.sym --all --jobs 16
//
// Exit codes:
//   0  — all selected cases MATCH
//   1  — at least one ENGINEMON_MISMATCH or HARNESS_ERROR

#include "crystal_differential/oracle_runner.hpp"

int main(int argc, char* argv[]) {
    // No defaults — user must specify --all or --move explicitly.
    return crystal::oracle::runner_main(argc, argv, {});
}
