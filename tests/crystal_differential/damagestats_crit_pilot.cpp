// damagestats_crit_pilot.cpp
// Live-proves the Crystal DamageStats crit-stat divergence vs Enginemon.
// Usage: damagestats_crit_pilot <rom_path> <sym_path>
// Exit 0 if all Crystal runs completed (match/mismatch reported but does not
// affect exit code — the divergence is expected and is the result being proved).

#include "crystal_differential/oracle_runner.hpp"
#include <cstdio>

int main(int argc, char* argv[])
{
    if(argc < 3){
        std::fprintf(stderr,
            "Usage: %s <rom_path> <sym_path>\n",
            argc > 0 ? argv[0] : "damagestats_crit_pilot");
        return 1;
    }
    return crystal::oracle::run_damagestats_crit_pilot(argv[1], argv[2]);
}
