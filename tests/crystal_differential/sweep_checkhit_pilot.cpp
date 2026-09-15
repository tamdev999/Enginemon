// sweep_checkhit_pilot.cpp
//
// Direct BattleCommand_CheckHit entry pilot.
// Proves that entering at BattleCommand_CheckHit (0D:4D32) instead of DoMove
// (0D:402C) produces bit-identical hit/miss, RNG trace, and stop-reason
// results for the same fixture state.
//
// Usage: sweep_checkhit_pilot <rom_path> <sym_path>
// Exit 0 = both pilots pass. Exit 1 = any failure.

#include "crystal_differential/oracle_runner.hpp"
#include <cstdio>

int main(int argc, char* argv[])
{
    if(argc < 3){
        std::fprintf(stderr,
            "Usage: %s <rom_path> <sym_path>\n",
            argc > 0 ? argv[0] : "sweep_checkhit_pilot");
        return 1;
    }
    return crystal::oracle::run_checkhit_pilot(argv[1], argv[2]);
}
