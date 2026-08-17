# Standalone tools for development/debugging

# Crystal compiler CLI - compiles ROM to native format
add_executable(crystal_compile
    crystal_compile.cpp
)
target_link_libraries(crystal_compile PRIVATE
    enginemon_crystal
)

# EMON package compiler - compiles ROM to EMON package with all maps and scripts
add_executable(emon_compile
    emon_compile.cpp
)
target_link_libraries(emon_compile PRIVATE
    enginemon_crystal
    enginemon_engine
)

# ROM profile verifier - tests profile system with actual ROM
add_executable(profile_verify
    profile_verify.cpp
)
target_link_libraries(profile_verify PRIVATE
    enginemon_crystal
)

# Map extraction test - extracts single map to verify extractor
add_executable(extract_map
    extract_map.cpp
)
target_link_libraries(extract_map PRIVATE
    enginemon_crystal
)

# Metatile dump tool - dumps metatile data for debugging
add_executable(dump_metatile
    dump_metatile.cpp
)
target_link_libraries(dump_metatile PRIVATE
    enginemon_crystal
)

# Palette audit tool - read-only diagnostic for palette verification
add_executable(palette_audit
    palette_audit.cpp
)
target_include_directories(palette_audit PRIVATE
    ${CMAKE_SOURCE_DIR}/runtime
)
target_link_libraries(palette_audit PRIVATE
    enginemon_crystal
    enginemon_engine
)

# TODO: Add these when dependencies are ready:
# - asset_inspector (needs enginemon_engine)
# - script_debugger (needs enginemon_engine + Lua)
# - parity_runner (needs enginemon_crystal + enginemon_engine)

# TrainerRegistry verification tool - validates trainer extraction against pokecrystal source
add_executable(trainer_verify
    trainer_verify.cpp
)
target_link_libraries(trainer_verify PRIVATE
    enginemon_crystal
)

# Special inventory tool - dumps all Special IDs encountered in corpus
add_executable(special_inventory
    special_inventory.cpp
)
target_link_libraries(special_inventory PRIVATE
    enginemon_crystal
    enginemon_engine
)

# Trace Special 166 duplication through pipeline
add_executable(trace_166
    trace_166.cpp
)
target_link_libraries(trace_166 PRIVATE
    enginemon_crystal
    enginemon_engine
)

# Corpus duplicate command scan
add_executable(corpus_dupe_scan
    corpus_dupe_scan.cpp
)
target_link_libraries(corpus_dupe_scan PRIVATE
    enginemon_crystal
    enginemon_engine
)

# Blast radius scan - measures duplicate command identity bug
add_executable(blast_radius_scan
    blast_radius_scan.cpp
)
target_link_libraries(blast_radius_scan PRIVATE
    enginemon_crystal
    enginemon_engine
)

# CFG integrity check - verifies CFG structure across corpus
add_executable(cfg_integrity_check
    cfg_integrity_check.cpp
)
target_link_libraries(cfg_integrity_check PRIVATE
    enginemon_crystal
    enginemon_engine
)

# Corpus verification tool - expanded corpus verification for scene script discovery
add_executable(corpus_verify
    corpus_verify.cpp
)
target_link_libraries(corpus_verify PRIVATE
    enginemon_crystal
    enginemon_engine
)

# Special 152 trace - traces all occurrences of SetPlayerPalette in expanded corpus
add_executable(special152_trace
    special152_trace.cpp
)
target_link_libraries(special152_trace PRIVATE
    enginemon_crystal
    enginemon_engine
)

# Pokecenter2F trace - detailed trace of Pokecenter2F scripts
add_executable(pokecenter2f_trace
    pokecenter2f_trace.cpp
)
target_link_libraries(pokecenter2f_trace PRIVATE
    enginemon_crystal
    enginemon_engine
)

# Special 152 verification - verifies all 5 known addresses are in corpus
add_executable(special152_verify
    special152_verify.cpp
)
target_link_libraries(special152_verify PRIVATE
    enginemon_crystal
    enginemon_engine
)

# Corpus lowering audit - enumerates ALL lowering failures in expanded corpus
add_executable(corpus_lowering_audit
    corpus_lowering_audit.cpp
)
target_link_libraries(corpus_lowering_audit PRIVATE
    enginemon_crystal
    enginemon_engine
)

# Batch 9 detailed analysis - analyzes setval→special context patterns
add_executable(batch9_analysis
    batch9_analysis.cpp
)
target_link_libraries(batch9_analysis PRIVATE
    enginemon_crystal
    enginemon_engine
)
