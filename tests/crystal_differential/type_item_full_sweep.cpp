// type_item_full_sweep.cpp
// Full certification of type-boost held-item: arithmetic 1..255 + ordering interactions.
// Usage: type_item_full_sweep <rom_path> <sym_path>
#include "crystal_differential/oracle_runner.hpp"
#include <cstdio>
int main(int argc, char* argv[])
{
    if(argc < 3){
        std::fprintf(stderr,
            "Usage: %s <rom_path> <sym_path>\n",
            argc > 0 ? argv[0] : "type_item_full_sweep");
        return 2;
    }
    return crystal::oracle::run_type_item_full_sweep(argv[1], argv[2]);
}
