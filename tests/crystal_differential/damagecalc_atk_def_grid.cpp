// damagecalc_atk_def_grid.cpp
// Exhaustive Crystal BattleCommand_DamageCalc vs Enginemon calculate_damage
// attack×defense sweep: attack 1..255 × defense 1..255 × crit {0,1}.
//
// Usage: damagecalc_atk_def_grid <rom_path> <sym_path> [--jobs N]
// Exit 0 = all 130,050 cases match. Exit 1 = any mismatch or harness error.

#include "crystal_differential/oracle_runner.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>

int main(int argc, char* argv[])
{
    if(argc < 3){
        std::fprintf(stderr,
            "Usage: %s <rom_path> <sym_path> [--jobs N]\n",
            argc > 0 ? argv[0] : "damagecalc_atk_def_grid");
        return 1;
    }

    int jobs = 8; // default: 8 parallel workers
    for(int i = 3; i < argc; ++i){
        if((std::strcmp(argv[i], "--jobs") == 0 || std::strcmp(argv[i], "-j") == 0)
            && i + 1 < argc){
            jobs = std::atoi(argv[++i]);
            if(jobs < 1)  jobs = 1;
            if(jobs > 64) jobs = 64;
        }
    }

    return crystal::oracle::run_damagecalc_atk_def_grid(argv[1], argv[2], jobs);
}
