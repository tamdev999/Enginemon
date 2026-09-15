// damagecalc_matrix.cpp
// Crystal BattleCommand_DamageCalc vs Enginemon calculate_damage conformance matrix.
// Usage: damagecalc_matrix <rom_path> <sym_path>
// Exit 0 = all cases match. Exit 1 = any mismatch or harness error.

#include "crystal_differential/oracle_runner.hpp"
#include <cstdio>

int main(int argc, char* argv[])
{
    if(argc < 3){
        std::fprintf(stderr,
            "Usage: %s <rom_path> <sym_path>\n",
            argc > 0 ? argv[0] : "damagecalc_matrix");
        return 1;
    }
    return crystal::oracle::run_damagecalc_matrix(argv[1], argv[2]);
}
