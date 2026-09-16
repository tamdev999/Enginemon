// stab_type_sweep.cpp
// Exhaustive STAB/type-effectiveness certification sweep.
// Usage: stab_type_sweep <rom_path> <sym_path>
#include "crystal_differential/oracle_runner.hpp"
#include <cstdio>
int main(int argc, char* argv[])
{
    if(argc < 3){
        std::fprintf(stderr,
            "Usage: %s <rom_path> <sym_path>\n",
            argc > 0 ? argv[0] : "stab_type_sweep");
        return 2;
    }
    return crystal::oracle::run_stab_type_sweep(argv[1], argv[2]);
}
