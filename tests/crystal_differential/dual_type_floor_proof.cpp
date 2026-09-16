// dual_type_floor_proof.cpp
// Live-proves the dual-type sequential-flooring divergence using real Ice Beam (id=58).
// Usage: dual_type_floor_proof <rom_path> <sym_path>
#include "crystal_differential/oracle_runner.hpp"
#include <cstdio>
int main(int argc, char* argv[])
{
    if(argc < 3){
        std::fprintf(stderr,
            "Usage: %s <rom_path> <sym_path>\n",
            argc > 0 ? argv[0] : "dual_type_floor_proof");
        return 2;
    }
    return crystal::oracle::run_dual_type_floor_proof(argv[1], argv[2]);
}
