// tests/crystal_differential/sameboy_link_smoke.cpp
//
// Trivial link test for sameboy_core.
// Verifies that SameBoy Core compiles and links cleanly with the Enginemon
// clang-cl toolchain before the full differential harness is added.
//
// No production changes. No Pokémon logic. No test assertions.

#include "Core/gb.h"
#include <cstdio>

int main() {
    // Allocate and init a SameBoy instance (GBC model = Crystal requires CGB).
    GB_gameboy_t gb;
    GB_init(&gb, GB_MODEL_CGB_E);

    // Verify we can read the register pointer.
    GB_registers_t* regs = GB_get_registers(&gb);
    std::printf("SameBoy Core linked OK. GB_registers_t* = %p\n",
                static_cast<void*>(regs));

    GB_free(&gb);
    std::printf("GB_free OK\n");
    return 0;
}
