// stab_arithmetic_sweep.cpp
// STAB/type-effectiveness arithmetic certification over input damage 1..255.
// 11 structural type configurations × 255 inputs = 2805 logical cases.
// Usage: stab_arithmetic_sweep <rom_path> <sym_path>
#include "crystal_differential/oracle_runner.hpp"
#include <cstdio>
int main(int argc, char* argv[])
{
    if(argc < 3){
        std::fprintf(stderr,
            "Usage: %s <rom_path> <sym_path>\n",
            argc > 0 ? argv[0] : "stab_arithmetic_sweep");
        return 2;
    }
    return crystal::oracle::run_stab_arithmetic_sweep(argv[1], argv[2]);
}
