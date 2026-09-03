# Crystal frontend/compiler
# All ROM offsets, RGBDS symbols, banking, Crystal-specific formats live here
# Emits: GameDefinition + Assets + generated Lua scripts

add_library(enginemon_crystal STATIC
    # ROM handling
    rom/loader.cpp
    rom/validator.cpp
    rom/symbol_map.cpp
    rom/profile.cpp
    
    # Data extractors
    extract/maps.cpp
    extract/tilesets.cpp
    extract/sprites.cpp
    extract/fonts.cpp
    extract/species.cpp
    extract/battle_rules.cpp
    extract/sm83_lifter.cpp
    
    # Output/packaging
    output/native_package.cpp
    
    # Compiler (ROM → EMON package)
    compile/full_compiler.cpp
    compile/corpus_discovery.cpp
    
    # Script compilation (always enabled - does not require engine)
    script/decoder.cpp
    script/lua_emitter.cpp
    script/semantic_lua_emitter.cpp
    script/crystal_command.cpp
    script/typed_decoder.cpp
    script/crystal_cfg.cpp
    script/native_registry.cpp
    script/semantic_legalizer.cpp
    script/legality_gate.cpp
    script/semantic_linker.cpp
    script/elevator_registry.cpp
    script/trainer_registry.cpp
    script/pokemail_registry.cpp
    script/text_registry.cpp

    # Crystal .sav codec (Phase 1-3A)
    save/crystal_sram_layout.cpp
    save/crystal_save_reader.cpp
    save/crystal_save_writer.cpp
    save/crystal_party_codec.cpp
)

# TODO: Add these as they're implemented
# - extract/pokemon.cpp, extract/moves.cpp, etc.
# - output/native_package.cpp
# - world/connection_resolver.cpp

target_include_directories(enginemon_crystal PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/engine/include
)

# Note: stb_image dependency removed - fonts now extracted directly from ROM

# Link engine when available (needed for more complete type definitions)
if(TARGET enginemon_engine)
    target_link_libraries(enginemon_crystal PRIVATE enginemon_engine)
endif()

# MSVC /Ob0 on proven pathological TUs.
# Measured peak: crystal_command.cpp 4.8 GB at /Ob2, 1.02 GB at /Ob0.
# clang-cl does not need this — its /O2 peaks at 2.8 GB on the same TU.
if(MSVC AND NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    set_source_files_properties(
        script/crystal_command.cpp
        PROPERTIES COMPILE_OPTIONS "/Ob0"
    )
endif()
