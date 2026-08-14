cmake_minimum_required(VERSION 3.25)
project(Enginemon VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Build options
option(ENGINEMON_BUILD_TOOLS "Build standalone tools" ON)
option(ENGINEMON_BUILD_TESTS "Build tests" ON)
option(ENGINEMON_ENABLE_RT "Enable raytracing support" OFF)
option(ENGINEMON_ENABLE_VULKAN "Enable Vulkan renderer (required for runtime)" OFF)
option(ENGINEMON_ENABLE_ENGINE "Build engine library (requires Lua)" OFF)

# Find packages conditionally
# Lua only required when building engine
if(ENGINEMON_ENABLE_ENGINE)
    # vcpkg provides lua as unofficial-lua
    # Try vcpkg's unofficial target first, then fall back to FindLua
    find_package(unofficial-lua CONFIG QUIET)
    if(unofficial-lua_FOUND)
        # vcpkg provides unofficial::lua::lua target
        set(LUA_FOUND TRUE)
        set(LUA_TARGET unofficial::lua::lua)
    else()
        find_package(Lua REQUIRED)
        set(LUA_TARGET ${LUA_LIBRARIES})
    endif()
endif()

# Vulkan and SDL3 only required when building the runtime
if(ENGINEMON_ENABLE_VULKAN)
    find_package(Vulkan REQUIRED)
    find_package(SDL3 REQUIRED)
endif()

# Frontends can always be built (no graphics/Lua dependencies)
add_subdirectory(frontends)

# Engine requires Lua
if(ENGINEMON_ENABLE_ENGINE)
    add_subdirectory(engine)
else()
    message(STATUS "Engine disabled - enable with -DENGINEMON_ENABLE_ENGINE=ON (requires Lua)")
endif()

# Runtime requires Vulkan + Engine
if(ENGINEMON_ENABLE_VULKAN AND ENGINEMON_ENABLE_ENGINE)
    add_subdirectory(runtime)
elseif(ENGINEMON_ENABLE_VULKAN)
    message(STATUS "Runtime requires engine - enable with -DENGINEMON_ENABLE_ENGINE=ON")
else()
    message(STATUS "Vulkan disabled - runtime will not be built")
endif()

if(ENGINEMON_BUILD_TOOLS)
    add_subdirectory(tools)
endif()

if(ENGINEMON_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
