// type_item_order_sweep.cpp
// Exhaustive type-item ordering differences: 15 configs × Q=1..255.
// Usage: type_item_order_sweep <rom_path> <sym_path>
#include "crystal_differential/oracle_runner.hpp"
#include <cstdio>
int main(int argc, char* argv[])
{
    if(argc < 3){
        std::fprintf(stderr,
            "Usage: %s <rom_path> <sym_path>\n",
            argc > 0 ? argv[0] : "type_item_order_sweep");
        return 2;
    }
    return crystal::oracle::run_type_item_order_sweep(argv[1], argv[2]);
}
