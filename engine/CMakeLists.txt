# Engine library - native C++ runtime
# No ROM knowledge, no Game Boy concepts, only semantic game data

add_library(enginemon_engine STATIC
    # Core data model
    core/game_definition.cpp
    core/registry.cpp
    core/types.cpp
    core/game_loop.cpp
    core/headless_runtime.cpp
    core/game_state.cpp
    core/timing.cpp
    
    # Package loading (runtime side of compiler/runtime boundary)
    package/package_reader.cpp
    
    # World / movement / collision / interaction / warps
    world/movement_manager.cpp
    world/collision.cpp
    world/interaction.cpp
    world/world_manager.cpp
    world/runtime_tileset.cpp
    world/sprite_atlas.cpp
    
    # Input system
    input/input_system.cpp
    
    # Battle system (mechanics, AI)
    battle/calculator.cpp
    battle/battle.cpp
    battle/trainer_ai.cpp

    # Party/Pokemon management
    party/party.cpp
    party/pokemon.cpp
    
    # Scripting (Lua orchestration)
    scripting/lua_runtime.cpp
    scripting/api_bindings.cpp
    scripting/coroutine_manager.cpp
    scripting/semantic_ir.cpp  # heavy: 154-alt SemanticOp variant — /Ob0 on MSVC (see below)
    
    # Build system (shared across frontends)
    build/thread_pool.cpp
    build/build_stats.cpp
    build/package_cache.cpp
)

target_include_directories(enginemon_engine PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(enginemon_engine PUBLIC
    ${LUA_TARGET}
)

# Include directories - vcpkg target handles this, but add for FindLua fallback
if(LUA_INCLUDE_DIR)
    target_include_directories(enginemon_engine PRIVATE
        ${LUA_INCLUDE_DIR}
    )
endif()

# MSVC /Ob0 on proven pathological TUs.
# Measured peak: semantic_ir.cpp 3.68 GB at /Ob2, 0.65 GB at /Ob0.
# clang-cl does not need this — its /O2 peaks at 1.9 GB on the same TU.
# Applied per-file via set_source_files_properties to avoid global flag pollution.
if(MSVC AND NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    set_source_files_properties(
        scripting/semantic_ir.cpp
        PROPERTIES COMPILE_OPTIONS "/Ob0"
    )
endif()
