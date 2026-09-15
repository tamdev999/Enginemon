// damagecalc_pilot.cpp -- Direct BattleCommand_DamageCalc entry pilot.
// Usage: damagecalc_pilot <rom_path> <sym_path>
// Exit 0 = all cases pass. Exit 1 = any failure.

#include "crystal_differential/oracle_runner.hpp"
#include <cstdio>

int main(int argc, char* argv[])
{
    if(argc < 3){
        std::fprintf(stderr,
            "Usage: %s <rom_path> <sym_path>\n",
            argc > 0 ? argv[0] : "damagecalc_pilot");
        return 1;
    }
    return crystal::oracle::run_damagecalc_pilot(argv[1], argv[2]);
}
