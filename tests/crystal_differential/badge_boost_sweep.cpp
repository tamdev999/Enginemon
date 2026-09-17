// badge_boost_sweep.cpp
// Certifies Crystal DoBadgeTypeBoosts against Enginemon.
// Usage: badge_boost_sweep <rom_path> <sym_path>
#include "crystal_differential/oracle_runner.hpp"
#include <cstdio>
int main(int argc, char* argv[])
{
    if(argc < 3){
        std::fprintf(stderr,
            "Usage: %s <rom_path> <sym_path>\n",
            argc > 0 ? argv[0] : "badge_boost_sweep");
        return 2;
    }
    return crystal::oracle::run_badge_boost_sweep(argv[1], argv[2]);
}
