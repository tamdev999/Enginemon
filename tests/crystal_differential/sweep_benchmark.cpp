// sweep_benchmark.cpp -- real Part B cost breakdown benchmark.
// Uses the certified oracle infrastructure directly.
// Measures per-component timing for one full ACC row.
//
// Usage: sweep_benchmark <rom_path> <sym_path> [acc_raw]
// Default acc_raw=7 (neutral).
// Exit 0 = benchmark passed validation; Exit 1 = failure.

#include "crystal_differential/oracle_runner.hpp"
#include <cstdio>
#include <cstdlib>

int main(int argc, char* argv[])
{
    if(argc < 3){
        std::fprintf(stderr,
            "Usage: %s <rom_path> <sym_path> [acc_raw]\n"
            "  acc_raw: Crystal raw ACC stage 1..13 (default 7=neutral)\n",
            argc > 0 ? argv[0] : "sweep_benchmark");
        return 1;
    }

    int acc_raw = 7;
    if(argc >= 4) acc_raw = std::atoi(argv[3]);

    return crystal::oracle::run_accuracy_sweep_benchmark(argv[1], argv[2], acc_raw);
}
