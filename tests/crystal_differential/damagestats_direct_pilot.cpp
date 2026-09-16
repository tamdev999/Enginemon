// damagestats_direct_pilot.cpp
// Direct semantic DamageStats pilot: Crystal side uses real CalcPlayerStats/CalcEnemyStats
// to populate staged stats from real ROM routines, then calls BattleCommand_DamageStats
// to produce B/C. No harness stage math.
// Usage: damagestats_direct_pilot <rom_path> <sym_path>

#include "crystal_differential/oracle_runner.hpp"
#include <cstdio>

int main(int argc, char* argv[])
{
    if(argc < 3){
        std::fprintf(stderr, "Usage: %s <rom_path> <sym_path>\n",
            argc > 0 ? argv[0] : "damagestats_direct_pilot");
        return 1;
    }
    return crystal::oracle::run_damagestats_direct_pilot(argv[1], argv[2]);
}
