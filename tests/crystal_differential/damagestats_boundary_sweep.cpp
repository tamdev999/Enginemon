// damagestats_boundary_sweep.cpp
// Exhaustive physical DamageStats stat-selection boundary sweep.
// Certifies Crystal pre-TruncateHL_BC attack/defense values against Enginemon
// DamageParams.attack_stat/defense_stat for every stage pair × crit × screen.
// Usage: damagestats_boundary_sweep <rom_path> <sym_path>
// Exit 0 = all match. Exit 1 = mismatch(es) found. Exit 2 = harness error.
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
    return crystal::oracle::run_damagestats_boundary_sweep(argv[1], argv[2]);
}
