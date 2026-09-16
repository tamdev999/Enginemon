// damagestats_special_sweep.cpp
// Exhaustive special DamageStats stat-selection boundary sweep.
// Run 1: base SpAtk=110, SpDef=110 (canonical baseline).
// Run 2: base SpAtk=111, SpDef=113 (base-stat coincidence audit).
// Move: Surf (id=57, Water type) — triggers the special stat path in PlayerAttackDamage.
// Usage: damagestats_special_sweep <rom_path> <sym_path>
// Exit 0 = all match both runs. Exit 1 = mismatch(es). Exit 2 = harness error.
#include "crystal_differential/oracle_runner.hpp"
#include <cstdio>
int main(int argc, char* argv[])
{
    if(argc < 3){
        std::fprintf(stderr,
            "Usage: %s <rom_path> <sym_path>\n",
            argc > 0 ? argv[0] : "damagestats_special_sweep");
        return 2;
    }
    const char* rom = argv[1];
    const char* sym = argv[2];

    int r1 = crystal::oracle::run_damagestats_boundary_sweep_special(rom, sym);
    if(r1 == 2) return 2;

    int r2 = crystal::oracle::run_damagestats_boundary_sweep_special_with_bases(
        rom, sym, 111, 113);
    if(r2 == 2) return 2;

    return (r1 == 1 || r2 == 1) ? 1 : 0;
}
