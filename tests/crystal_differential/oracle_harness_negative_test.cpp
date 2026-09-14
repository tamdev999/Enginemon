// tests/crystal_differential/oracle_harness_negative_test.cpp
//
// Standalone CTest executable that exercises all six fail-closed harness paths
// that cannot be triggered from the normal oracle CLI.
//
// Exit codes:
//   0  all six negative controls produced HARNESS_ERROR as expected
//   1  one or more controls produced an unexpected outcome
//
// Usage:
//   oracle_harness_negative_test <rom_path> <sym_path>
//
// Example:
//   oracle_harness_negative_test crystal.gbc pokecrystal11.sym

#include "crystal_differential/oracle_runner.hpp"
#include <cstdio>
#include <cstring>

int main(int argc, char* argv[])
{
    if(argc < 3){
        std::fprintf(stderr,
            "Usage: %s <rom_path> <sym_path>\n"
            "  Runs the six harness fail-closed negative controls.\n"
            "  Exit 0 = all pass.  Exit 1 = one or more failed.\n",
            argc > 0 ? argv[0] : "oracle_harness_negative_test");
        return 1;
    }

    const char* rom_path = argv[1];
    const char* sym_path = argv[2];

    // verbose=true so CTest captures the per-test output.
    int result = crystal::oracle::run_harness_negative_tests(rom_path, sym_path,
                                                              /*verbose=*/true);
    return result; // 0 = all pass, 1 = any failure
}
