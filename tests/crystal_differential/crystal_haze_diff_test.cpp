// tests/crystal_differential/crystal_haze_diff_test.cpp
//
// Single-case Haze differential oracle — entry point for the parallel runner.
// Delegates entirely to the runner with --move 114.
//
// Usage: crystal_haze_diff_test <rom_path> <sym_path> [--jobs N]
//
// SameBoy commit pinned: 213a12ce93d66b105a113debd9396306066a7cfc
// Crystal ROM SHA-1 enforced: F2F52230B536214EF7C9924F483392993E226CFB

#include "crystal_differential/oracle_runner.hpp"

int main(int argc, char* argv[]) {
    return crystal::oracle::runner_main(argc, argv,
        crystal::oracle::RunnerConfig{}.with_moves({114}));
}
