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
    # =========================================================================
    # runtime_test ÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬Ãƒâ€¦Ã‚Â¡ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â original monolithic target, superseded by split targets.
    # Coverage is complete: runtime_test_engine (194) + runtime_test_compiler (392).
    # EXCLUDE_FROM_ALL: never rebuilt during normal or canonical builds.
    # Still buildable explicitly: cmake --build build --target runtime_test
    # =========================================================================
    add_executable(runtime_test EXCLUDE_FROM_ALL
        scripting/runtime_test_core.cpp
        scripting/runtime_test_maps.cpp
        scripting/runtime_test_scripts.cpp
        scripting/runtime_test_batches.cpp
        scripting/runtime_test_vm_state.cpp
        scripting/runtime_test_rtc.cpp
        scripting/runtime_test_integration.cpp
        scripting/runtime_test_warp_semantics.cpp
        scripting/runtime_test_blast_radius.cpp
        scripting/runtime_test_text_rng.cpp
        scripting/runtime_test_pcg_vm.cpp
        scripting/runtime_test_main.cpp
    )
    target_link_libraries(runtime_test PRIVATE
        enginemon_crystal
        enginemon_engine
    )
    target_include_directories(runtime_test PRIVATE ${CMAKE_SOURCE_DIR}/tests)
    if(MSVC)
        # /O1 (minimize size) keeps OBJ files small; /Od produced 550MB of OBJ input
        # that made the linker take minutes even on fast hardware.
        # /bigobj + /Zm2000: required for the large std::variant<> instantiation TUs.
        # /wd4566: suppress C4566 unicode escape warnings that OOM MSBuild's regex parser.
        target_compile_options(runtime_test PRIVATE /bigobj /Zm2000 /O1 /wd4566)
    endif()
    # Not added to ctest - requires ROM path argument

    # =========================================================================
    # runtime_test_engine ÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬Ãƒâ€¦Ã‚Â¡ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â engine-only TUs, no Crystal compiler IR variants.
    # No CrystalCommandData (171 alts) or SemanticOp (154 alts) instantiation.
    # Safe to compile in full parallelism without OOM risk.
    # Covers: core, maps, blast_radius, integration, warp_semantics, rtc (191 tests)
    # =========================================================================
    add_executable(runtime_test_engine
        scripting/runtime_test_core.cpp
        scripting/runtime_test_maps.cpp
        scripting/runtime_test_blast_radius.cpp
        scripting/runtime_test_integration.cpp
        scripting/runtime_test_warp_semantics.cpp
        scripting/runtime_test_rtc.cpp
        scripting/runtime_test_engine_main.cpp
    )
    target_link_libraries(runtime_test_engine PRIVATE
        enginemon_crystal
        enginemon_engine
    )
    target_include_directories(runtime_test_engine PRIVATE ${CMAKE_SOURCE_DIR}/tests)
    if(MSVC)
        target_compile_options(runtime_test_engine PRIVATE /bigobj /wd4566)
    endif()
    # Not added to ctest - requires ROM path argument

    # =========================================================================
    # runtime_test_compiler ÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬Ãƒâ€¦Ã‚Â¡ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â Crystal compiler/IR TUs.
    # Includes semantic_ir.hpp (154-alt SemanticOp) + crystal_command.hpp
    # (171-alt CrystalCommandData) ÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬Ãƒâ€¦Ã‚Â¡ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â each TU peaks at ~8 GB MSVC WS.
    # /O1 /Zm2000 required; at most 5 TUs so peak RAM ~40 GB at full parallelism.
    # Covers: scripts, batches, vm_state, text_rng, pcg_vm (392 tests)
    # =========================================================================
    add_executable(runtime_test_compiler
        scripting/runtime_test_scripts.cpp
        scripting/runtime_test_batches.cpp
        scripting/runtime_test_vm_state.cpp
        scripting/runtime_test_text_rng.cpp
        scripting/runtime_test_pcg_vm.cpp
        scripting/runtime_test_compiler_main.cpp
    )
    target_link_libraries(runtime_test_compiler PRIVATE
        enginemon_crystal
        enginemon_engine
    )
    target_include_directories(runtime_test_compiler PRIVATE ${CMAKE_SOURCE_DIR}/tests)
    if(MSVC)
        target_compile_options(runtime_test_compiler PRIVATE /bigobj /Zm2000 /O1 /wd4566)
        # BuildInParallel=false removed: out-of-line special member fix reduced
        # worst-TU peak WS from ~6.3 GB to ~2.8 GB. Normal parallelism is safe.
    endif()
    # Not added to ctest - requires ROM path argument

    # =========================================================================
    # battle_test ÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬Ãƒâ€¦Ã‚Â¡ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â engine-only battle mechanics tests.
    # Links ONLY against enginemon_engine (no enginemon_crystal dependency).
    # No variant-heavy compiler IR headers.  All future battle tests go here.
    # =========================================================================
    add_executable(battle_test
        scripting/battle_test.cpp
    )
    target_link_libraries(battle_test PRIVATE
        enginemon_engine
    )
    target_include_directories(battle_test PRIVATE ${CMAKE_SOURCE_DIR}/tests)
    if(MSVC)
        target_compile_options(battle_test PRIVATE /bigobj)
    endif()
    # Not added to ctest - standalone, no ROM required

    # Focused tests for variable sprite identity model + money text state.
    # Does NOT require a ROM path argument ÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬Ãƒâ€¦Ã‚Â¡ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â runs standalone.
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

    # Stage 7 SemanticLuaEmitter tests + sprite/money emitter tests.
    # Separated to avoid MSVC heap OOM on the large SemanticOp variant.
    # Does NOT require a ROM path argument ÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬Ãƒâ€¦Ã‚Â¡ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â runs standalone.
    add_executable(emitter_test
        scripting/emitter_test.cpp
    )
    target_link_libraries(emitter_test PRIVATE
        enginemon_crystal
        enginemon_engine
    )
    target_include_directories(emitter_test PRIVATE ${CMAKE_SOURCE_DIR}/tests)
    if(MSVC)
        target_compile_options(emitter_test PRIVATE /bigobj /Zm500)
    endif()
    # MSVC /Ob0 on emitter_test.cpp: measured 3.87 GB at /Ob2, ~1.0 GB at /Ob0.
    if(MSVC AND NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        set_source_files_properties(
            scripting/emitter_test.cpp
            PROPERTIES COMPILE_OPTIONS "/Ob0"
        )
    endif()
endif()

# Lua demo (show generated Lua output for a script) ÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬Ãƒâ€¦Ã‚Â¡ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â debug tool, explicit build only
add_executable(lua_demo EXCLUDE_FROM_ALL
    crystal/lua_demo.cpp
)
target_link_libraries(lua_demo PRIVATE
    enginemon_crystal
)

# Text dump (debug text encoding) ÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬Ãƒâ€¦Ã‚Â¡ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â debug tool, explicit build only
add_executable(text_dump EXCLUDE_FROM_ALL
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

# Structural compatibility check ÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬Ãƒâ€¦Ã‚Â¡ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â debug tool, explicit build only
add_executable(structural_check EXCLUDE_FROM_ALL
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

# Stage 6: Linker diagnostic (deep failure investigation) ÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬Ãƒâ€¦Ã‚Â¡ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â debug tool, explicit build only
add_executable(linker_diagnostic EXCLUDE_FROM_ALL
    crystal/linker_diagnostic.cpp
)
target_link_libraries(linker_diagnostic PRIVATE
    enginemon_crystal
    enginemon_engine
)

# Crystal .sav codec Phase 1 tests
# Tests all 10 required categories: valid import, checksum corruption, backup recovery,
# size rejection, round-trip shadow preservation, field independence, export fidelity, UB safety.
# Does NOT require a ROM path argument ÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬Ãƒâ€¦Ã‚Â¡ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â runs standalone.
add_executable(crystal_save_test
    crystal/crystal_save_test.cpp
)
target_link_libraries(crystal_save_test PRIVATE
    enginemon_crystal
)
target_include_directories(crystal_save_test PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/frontends/crystal
)
if(MSVC)
    target_compile_options(crystal_save_test PRIVATE /W4 /wd4566)
endif()
# clang-cl inlines all 115 test bodies into main() at /O2, requiring > 1 MB stack.
# Set stack reserve to 4 MB for this binary only.
if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND WIN32)
    target_link_options(crystal_save_test PRIVATE /STACK:4194304)
endif()
# Not added to ctest - standalone binary

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

# Battle ROM propagation tests — true ROM→extractor→BRLS→runtime pipeline proof
add_executable(battle_rom_test
    crystal/battle_rom_test.cpp
)
target_link_libraries(battle_rom_test PRIVATE
    enginemon_crystal
    enginemon_engine
)
if(MSVC)
    target_compile_options(battle_rom_test PRIVATE /bigobj)
endif()

# Crystal Frontend Oracle ÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬Ãƒâ€¦Ã‚Â¡ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â Phase 1
# Independent source-fidelity tests using RGBDS-derived (or hand-authored) binary fixtures.
# Expected values come from pokecrystal semantics, NOT from Enginemon encoder output.
add_executable(oracle_test
    oracle/oracle_test_helpers.cpp
    oracle/oracle_test_p1a.cpp
    oracle/oracle_test_p1b_p2.cpp
    oracle/oracle_test_p3_p4.cpp
    oracle/oracle_test_p5_p55.cpp
    oracle/oracle_test_main.cpp
)
target_link_libraries(oracle_test PRIVATE
    enginemon_crystal
    enginemon_engine
)
# /bigobj: required for variant-heavy translation units
# /O1:     minimize size ÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬Ãƒâ€¦Ã‚Â¡ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â keeps OBJ small, avoids 550MB+ link bottleneck from /Od
# /Zm2000: raises compiler heap limit
# /wd4566: suppress C4566 unicode escape warnings
if(MSVC)
    target_compile_options(oracle_test PRIVATE /bigobj /Zm2000 /O1 /wd4566)
    # BuildInParallel=false removed: out-of-line special member fix reduced peak WS.
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

# =============================================================================
# CTest registration
# =============================================================================
# Standalone tests (no ROM required) are always registered when engine is enabled.
# ROM-dependent tests are registered only when ENGINEMON_ROM_PATH is set.
#
# Usage:
#   cmake -B build -DENGINEMON_ROM_PATH="path/to/crystal.gbc"
#   ctest --test-dir build -j8 --output-on-failure
#
# Labels:
#   engine-only  -- no Crystal compiler IR, no ROM needed; safe at full parallelism
#   compiler-ir  -- Crystal compiler/IR variants; high memory per TU
#   standalone   -- no ROM, no compiler IR
#   rom-required -- requires ENGINEMON_ROM_PATH
#   oracle       -- heavyweight (2.5s); run last in parallel batch
# =============================================================================

if(ENGINEMON_ENABLE_ENGINE)
    # --- Standalone (no ROM) ---------------------------------------------------
    add_test(NAME battle_test       COMMAND $<TARGET_FILE:battle_test>)
    add_test(NAME emitter_test      COMMAND $<TARGET_FILE:emitter_test>)
    add_test(NAME sprite_money_test COMMAND $<TARGET_FILE:sprite_money_test>)
    set_tests_properties(battle_test emitter_test sprite_money_test
        PROPERTIES LABELS "standalone;engine-only" TIMEOUT 30)

    add_test(NAME crystal_save_test COMMAND $<TARGET_FILE:crystal_save_test>)
    set_tests_properties(crystal_save_test PROPERTIES LABELS "standalone" TIMEOUT 30)

    # --- ROM-dependent --------------------------------------------------------
    if(ENGINEMON_ROM_PATH)
        add_test(NAME runtime_test_engine
            COMMAND $<TARGET_FILE:runtime_test_engine> "${ENGINEMON_ROM_PATH}")
        set_tests_properties(runtime_test_engine
            PROPERTIES LABELS "engine-only;rom-required" TIMEOUT 60)

        add_test(NAME runtime_test_compiler
            COMMAND $<TARGET_FILE:runtime_test_compiler> "${ENGINEMON_ROM_PATH}")
        set_tests_properties(runtime_test_compiler
            PROPERTIES LABELS "compiler-ir;rom-required" TIMEOUT 60)

        add_test(NAME golden_test
            COMMAND $<TARGET_FILE:golden_test> "${ENGINEMON_ROM_PATH}")
        set_tests_properties(golden_test
            PROPERTIES LABELS "rom-required;golden" TIMEOUT 30)

        add_test(NAME legality_gate_test
            COMMAND $<TARGET_FILE:legality_gate_test> "${ENGINEMON_ROM_PATH}")
        set_tests_properties(legality_gate_test
            PROPERTIES LABELS "rom-required;corpus" TIMEOUT 30)

        add_test(NAME corpus_test
            COMMAND $<TARGET_FILE:corpus_test> "${ENGINEMON_ROM_PATH}")
        set_tests_properties(corpus_test
            PROPERTIES LABELS "rom-required;corpus" TIMEOUT 30)

        add_test(NAME linker_test
            COMMAND $<TARGET_FILE:linker_test> "${ENGINEMON_ROM_PATH}")
        set_tests_properties(linker_test
            PROPERTIES LABELS "rom-required;corpus" TIMEOUT 30)

        add_test(NAME compiler_integrity_test
            COMMAND $<TARGET_FILE:compiler_integrity_test> "${ENGINEMON_ROM_PATH}")
        set_tests_properties(compiler_integrity_test
            PROPERTIES LABELS "rom-required;compiler-ir" TIMEOUT 120)

        add_test(NAME battle_rom_test
            COMMAND $<TARGET_FILE:battle_rom_test> "${ENGINEMON_ROM_PATH}")
        set_tests_properties(battle_rom_test
            PROPERTIES LABELS "rom-required;battle" TIMEOUT 30)

        add_test(NAME oracle_test
            COMMAND $<TARGET_FILE:oracle_test> "${ENGINEMON_ROM_PATH}")
        set_tests_properties(oracle_test
            PROPERTIES LABELS "rom-required;oracle" TIMEOUT 60)

        add_test(NAME corpus_lowering_audit
            COMMAND $<TARGET_FILE:corpus_lowering_audit> "${ENGINEMON_ROM_PATH}")
        set_tests_properties(corpus_lowering_audit
            PROPERTIES LABELS "rom-required" TIMEOUT 30)

        add_test(NAME profile_verify
            COMMAND $<TARGET_FILE:profile_verify> "${ENGINEMON_ROM_PATH}")
        set_tests_properties(profile_verify
            PROPERTIES LABELS "rom-required" TIMEOUT 30)

        add_test(NAME trainer_verify
            COMMAND $<TARGET_FILE:trainer_verify> "${ENGINEMON_ROM_PATH}")
        set_tests_properties(trainer_verify
            PROPERTIES LABELS "rom-required" TIMEOUT 30)
    else()
        message(STATUS
            "CTest: ROM-dependent tests not registered (set -DENGINEMON_ROM_PATH=... to enable)")
    endif()
endif()

