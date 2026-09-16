// damagestats_boundary_sweep.cpp
// Exhaustive physical DamageStats stat-selection boundary sweep.
// Run 1: base P_ATK=110, E_DEF=110 (canonical baseline).
// Run 2: base P_ATK=111, E_DEF=113 (base-stat coincidence audit).
// Usage: damagestats_boundary_sweep <rom_path> <sym_path>
// Exit 0 = all match both runs. Exit 1 = mismatch(es). Exit 2 = harness error.
#include "crystal_differential/oracle_runner.hpp"
#include <cstdio>
int main(int argc, char* argv[])
{
    if(argc < 3){
        std::fprintf(stderr,
            "Usage: %s <rom_path> <sym_path>\n",
            argc > 0 ? argv[0] : "damagestats_boundary_sweep");
        return 2;
    }
    const char* rom = argv[1];
    const char* sym = argv[2];

    // Run 1: canonical 110/110 baseline
    int r1 = crystal::oracle::run_damagestats_boundary_sweep(rom, sym);
    if(r1 == 2) return 2;

    // Run 2: 111/113 coincidence audit
    int r2 = crystal::oracle::run_damagestats_boundary_sweep_with_bases(rom, sym, 111, 113);
    if(r2 == 2) return 2;

    // Return 1 if either run found mismatches (expected), 0 if all match
    return (r1 == 1 || r2 == 1) ? 1 : 0;
}
