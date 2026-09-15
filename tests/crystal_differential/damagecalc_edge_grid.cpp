// damagecalc_edge_grid.cpp
// Dense 4-D edge grid for Crystal BattleCommand_DamageCalc vs Enginemon calculate_damage.
// level={1,2,3,10,50,100} x power/atk/def={1,2,3,10,50,100,127,128,254,255} x crit={0,1}
// = 12,000 logical cases, 48,000 Crystal executions.
//
// Usage: damagecalc_edge_grid <rom_path> <sym_path>
// Exit 0 = all 12,000 cases match. Exit 1 = any mismatch or harness error.

#include "crystal_differential/oracle_runner.hpp"
#include <cstdio>

int main(int argc, char* argv[])
{
    if(argc < 3){
        std::fprintf(stderr,
            "Usage: %s <rom_path> <sym_path>\n",
            argc > 0 ? argv[0] : "damagecalc_edge_grid");
        return 1;
    }
    return crystal::oracle::run_damagecalc_edge_grid(argv[1], argv[2]);
}
