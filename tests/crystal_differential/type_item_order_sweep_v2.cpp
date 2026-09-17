// type_item_order_sweep_v2.cpp
// Repaired type-item ordering sweep with correct Q boundary.
// Usage: type_item_order_sweep_v2 <rom_path> <sym_path>
#include "crystal_differential/oracle_runner.hpp"
#include <cstdio>
int main(int argc, char* argv[])
{
    if(argc < 3){
        std::fprintf(stderr,
            "Usage: %s <rom_path> <sym_path>\n",
            argc > 0 ? argv[0] : "type_item_order_sweep_v2");
        return 2;
    }
    return crystal::oracle::run_type_item_order_sweep_v2(argv[1], argv[2]);
}
