// damagecalc_atk_def_grid.cpp
// Exhaustive Crystal BattleCommand_DamageCalc vs Enginemon calculate_damage
// attack x defense sweep: attack 1..255 x defense 1..255 x crit {0,1}.
//
// Usage: damagecalc_atk_def_grid <rom_path> <sym_path> [--jobs N] [--shard-max-atk N]
// Exit 0 = all cases match. Exit 1 = any mismatch or harness error.
// --shard-max-atk N: limit attack sweep to 1..N (for determinism shard tests).

#include "crystal_differential/oracle_runner.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>

int main(int argc, char* argv[])
{
    if(argc < 3){
        std::fprintf(stderr,
            "Usage: %s <rom_path> <sym_path> [--jobs N] [--shard-max-atk N]\n",
            argc > 0 ? argv[0] : "damagecalc_atk_def_grid");
        return 1;
    }

    int jobs      = 8;   // default: 8 parallel workers
    int shard_max = 255; // default: full sweep atk 1..255
    for(int i = 3; i < argc; ++i){
        if((std::strcmp(argv[i], "--jobs") == 0 || std::strcmp(argv[i], "-j") == 0)
            && i + 1 < argc){
            jobs = std::atoi(argv[++i]);
            if(jobs < 1)  jobs = 1;
            if(jobs > 64) jobs = 64;
        } else if(std::strcmp(argv[i], "--shard-max-atk") == 0 && i + 1 < argc){
            shard_max = std::atoi(argv[++i]);
            if(shard_max < 1)   shard_max = 1;
            if(shard_max > 255) shard_max = 255;
        }
    }

    return crystal::oracle::run_damagecalc_atk_def_grid(argv[1], argv[2], jobs, shard_max);
}
