// tests/crystal_differential/crystal_battle_diff.cpp
//
// Crystal battle differential oracle — main entry point.
//
// Executes selected Crystal battle routines through real SM83 machine code via
// SameBoy and diffs the semantic outputs against Enginemon's live execution.
//
// Usage:
//   crystal_battle_diff <rom_path> <sym_path> [options]
//
//   --all             run all registered moves
//   --move <id>...    run specific move ID(s)
//   --jobs N          parallel workers (default: 1)
//   --help            show usage and exit 0
//
// Quick start:
//   crystal_battle_diff crystal.gbc pokecrystal11.sym --all --jobs 16
//   crystal_battle_diff crystal.gbc pokecrystal11.sym --move 114
//
// Exit codes:
//   0  all MATCH
//   1  ENGINEMON_MISMATCH
//   2  HARNESS_ERROR  (takes precedence over 1)
//   3  invalid CLI / unregistered move

#include "crystal_differential/oracle_runner.hpp"

int main(int argc, char* argv[]) {
    return crystal::oracle::runner_main(argc, argv);
}
