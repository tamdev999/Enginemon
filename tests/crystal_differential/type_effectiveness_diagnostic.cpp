// type_effectiveness_diagnostic.cpp
// Diagnoses why type effectiveness was not applied in a77b0c3 stab boundary pilot.
// Usage: type_effectiveness_diagnostic <rom_path> <sym_path>
#include "crystal_differential/oracle_runner.hpp"
#include <cstdio>
int main(int argc, char* argv[])
{
    if(argc < 3){
        std::fprintf(stderr,
            "Usage: %s <rom_path> <sym_path>\n",
            argc > 0 ? argv[0] : "type_effectiveness_diagnostic");
        return 2;
    }
    return crystal::oracle::run_type_effectiveness_diagnostic(argv[1], argv[2]);
}
