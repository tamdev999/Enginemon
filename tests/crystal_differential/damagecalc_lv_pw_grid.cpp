// damagecalc_lv_pw_grid.cpp
// Crystal BattleCommand_DamageCalc vs Enginemon calculate_damage
// level x power sweep: level 1..100 x power 1..255 x crit {0,1}.
//
// Usage: damagecalc_lv_pw_grid <rom_path> <sym_path> [--jobs N] [--shard-max-lv N]
// Exit 0 = all 51,000 cases match. Exit 1 = any mismatch or harness error.

#include "crystal_differential/oracle_runner.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>

int main(int argc, char* argv[])
{
    if(argc < 3){
        std::fprintf(stderr,
            "Usage: %s <rom_path> <sym_path> [--jobs N] [--shard-max-lv N]\n",
            argc > 0 ? argv[0] : "damagecalc_lv_pw_grid");
        return 1;
    }

    int jobs      = 8;   // default: 8 parallel workers
    int shard_max = 100; // default: full sweep level 1..100
    for(int i = 3; i < argc; ++i){
        if((std::strcmp(argv[i], "--jobs") == 0 || std::strcmp(argv[i], "-j") == 0)
            && i + 1 < argc){
            jobs = std::atoi(argv[++i]);
            if(jobs < 1)  jobs = 1;
            if(jobs > 64) jobs = 64;
        } else if(std::strcmp(argv[i], "--shard-max-lv") == 0 && i + 1 < argc){
            shard_max = std::atoi(argv[++i]);
            if(shard_max < 1)   shard_max = 1;
            if(shard_max > 100) shard_max = 100;
        }
    }

    return crystal::oracle::run_damagecalc_level_power_grid(argv[1], argv[2],
                                                             jobs, shard_max);
}
