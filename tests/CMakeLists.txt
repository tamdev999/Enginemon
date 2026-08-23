# Test suite

# Golden tests (Crystal ROM extraction validation)
# These run without GTest - they just need the ROM
add_executable(golden_test
    crystal/golden_test.cpp
)
target_link_libraries(golden_test PRIVATE
    enginemon_crystal
)
# Not added to ctest - requires ROM path argument

# Lua Runtime tests (end-to-end script execution)
# Requires engine library (with Lua) + crystal frontend
if(ENGINEMON_ENABLE_ENGINE)
    add_executable(runtime_test
        scripting/runtime_test.cpp
    )
    target_link_libraries(runtime_test PRIVATE
        enginemon_crystal
        enginemon_engine
    )
    target_include_directories(runtime_test PRIVATE ${CMAKE_SOURCE_DIR}/tests)
    if(MSVC)
        target_compile_options(runtime_test PRIVATE /bigobj /Zm1000)
    endif()
    # Not added to ctest - requires ROM path argument

    # Focused tests for variable sprite identity model + money text state.
    # Does NOT require a ROM path argument — runs standalone.
    add_executable(sprite_money_test
        scripting/sprite_money_test.cpp
    )
    target_link_libraries(sprite_money_test PRIVATE
        enginemon_crystal
        enginemon_engine
    )
    target_include_directories(sprite_money_test PRIVATE ${CMAKE_SOURCE_DIR}/tests)
    if(MSVC)
        target_compile_options(sprite_money_test PRIVATE /bigobj)
    endif()
endif()

# Lua demo (show generated Lua output for a script)
add_executable(lua_demo
    crystal/lua_demo.cpp
)
target_link_libraries(lua_demo PRIVATE
    enginemon_crystal
)

# Text dump (debug text encoding)
add_executable(text_dump
    crystal/text_dump.cpp
)
target_link_libraries(text_dump PRIVATE
    enginemon_crystal
)

# Corpus validation test (typed decoder round-trip for all scripts)
add_executable(corpus_test
    crystal/corpus_test.cpp
)
target_link_libraries(corpus_test PRIVATE
    enginemon_crystal
    enginemon_engine
)

# Legality gate negative tests (Stage 5 validation)
add_executable(legality_gate_test
    crystal/legality_gate_test.cpp
)
target_link_libraries(legality_gate_test PRIVATE
    enginemon_crystal
    enginemon_engine
)
target_include_directories(legality_gate_test PRIVATE ${CMAKE_SOURCE_DIR}/tests)
if(MSVC)
    target_compile_options(legality_gate_test PRIVATE /bigobj)
endif()

# Structural compatibility check (field-move actor lifetime analysis)
add_executable(structural_check
    crystal/structural_check.cpp
)
target_link_libraries(structural_check PRIVATE
    enginemon_crystal
    enginemon_engine
)
if(MSVC)
    target_compile_options(structural_check PRIVATE /bigobj)
endif()

# Stage 6: Semantic Linker test (corpus-wide typed-reference validation)
add_executable(linker_test
    crystal/linker_test.cpp
)
target_link_libraries(linker_test PRIVATE
    enginemon_crystal
    enginemon_engine
)
if(MSVC)
    target_compile_options(linker_test PRIVATE /bigobj)
endif()

# Stage 6: Linker diagnostic (deep failure investigation)
add_executable(linker_diagnostic
    crystal/linker_diagnostic.cpp
)
target_link_libraries(linker_diagnostic PRIVATE
    enginemon_crystal
    enginemon_engine
)

# Compiler integrity tests (fail-open adversarial tests for asset and map discovery)
add_executable(compiler_integrity_test
    crystal/compiler_integrity_test.cpp
)
target_link_libraries(compiler_integrity_test PRIVATE
    enginemon_crystal
    enginemon_engine
)
if(MSVC)
    target_compile_options(compiler_integrity_test PRIVATE /bigobj)
endif()

# Crystal Frontend Oracle — Phase 1
# Independent source-fidelity tests using RGBDS-derived (or hand-authored) binary fixtures.
# Expected values come from pokecrystal semantics, NOT from Enginemon encoder output.
add_executable(oracle_test
    oracle/oracle_test.cpp
)
target_link_libraries(oracle_test PRIVATE
    enginemon_crystal
    enginemon_engine
)
if(MSVC)
    target_compile_options(oracle_test PRIVATE /bigobj)
endif()

# TODO: Enable these when GTest is available and dependencies are built
if(FALSE)

find_package(GTest REQUIRED)

# Engine unit tests
add_executable(engine_tests
    core/registry_test.cpp
    core/types_test.cpp
    battle/calculator_test.cpp
    battle/status_test.cpp
    battle/weather_test.cpp
    party/inventory_test.cpp
    scripting/lua_runtime_test.cpp
)
target_link_libraries(engine_tests PRIVATE
    enginemon_engine
    GTest::gtest_main
)
add_test(NAME EngineTests COMMAND engine_tests)

# Crystal frontend tests
add_executable(crystal_tests
    crystal/rom_loader_test.cpp
    crystal/script_decoder_test.cpp
    crystal/lua_emitter_test.cpp
    crystal/connection_resolver_test.cpp
)
target_link_libraries(crystal_tests PRIVATE
    enginemon_crystal
    GTest::gtest_main
)
add_test(NAME CrystalTests COMMAND crystal_tests)

# Parity tests (require reference data)
add_executable(parity_tests
    parity/battle_parity_test.cpp
    parity/movement_parity_test.cpp
    parity/script_parity_test.cpp
    parity/rng_parity_test.cpp
)
target_link_libraries(parity_tests PRIVATE
    enginemon_crystal
    enginemon_engine
    GTest::gtest_main
)
add_test(NAME ParityTests COMMAND parity_tests)

endif()
