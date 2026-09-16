// stab_modifier_pilot.cpp
// Reruns the BattleCommand_Stab modifier pilot with correct TypeChart initialization.
// Usage: stab_modifier_pilot <rom_path> <sym_path>
#include "crystal_differential/oracle_runner.hpp"
#include <cstdio>
int main(int argc, char* argv[])
{
    if(argc < 3){
        std::fprintf(stderr,
            "Usage: %s <rom_path> <sym_path>\n",
            argc > 0 ? argv[0] : "stab_modifier_pilot");
        return 2;
    }
    return crystal::oracle::run_stab_modifier_pilot(argv[1], argv[2]);
}
