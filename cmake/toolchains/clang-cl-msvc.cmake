# cmake/toolchains/clang-cl-msvc.cmake
# clang-cl with MSVC ABI, Windows SDK, lld-link.
# Default Windows developer toolchain for Enginemon.
# Requires: LLVM 18+ at C:/Program Files/LLVM/bin

set(CMAKE_C_COMPILER   "C:/Program Files/LLVM/bin/clang-cl.exe")
set(CMAKE_CXX_COMPILER "C:/Program Files/LLVM/bin/clang-cl.exe")
set(CMAKE_LINKER       "C:/Program Files/LLVM/bin/lld-link.exe")
set(CMAKE_AR           "C:/Program Files/LLVM/bin/llvm-lib.exe")

# Use MSVC ABI and shared CRT — set unconditionally below in the compat block.

# Release flags: /O2 only. Explicitly override to prevent CMake's MSVC-like
# compiler detection path from injecting /Ob2, which causes 4-5 GB peaks on
# the large variant TUs (measured: crystal_command 4.8 GB, semantic_ir 3.68 GB).
set(CMAKE_CXX_FLAGS_RELEASE "/O2 /DNDEBUG" CACHE STRING "" FORCE)
set(CMAKE_C_FLAGS_RELEASE   "/O2 /DNDEBUG" CACHE STRING "" FORCE)

# ---------------------------------------------------------------------------
# MSVC STL compatibility
# clang 18: MSVC STL ships __clang_major__ guards requiring clang 19+.
# clang 18 needs _ALLOW_COMPILER_AND_STL_VERSION_MISMATCH to compile with MSVC STL.
# Set the complete CXX flags string here, including the compat define when needed.
# Remove the _ALLOW define block once LLVM is upgraded to 19+.
# ---------------------------------------------------------------------------
execute_process(
    COMMAND "C:/Program Files/LLVM/bin/clang-cl.exe" --version
    OUTPUT_VARIABLE _CLANG_VER_OUT ERROR_QUIET OUTPUT_STRIP_TRAILING_WHITESPACE
)
if(_CLANG_VER_OUT MATCHES "clang version ([0-9]+)")
    set(_CLANG_MAJOR "${CMAKE_MATCH_1}")
else()
    set(_CLANG_MAJOR "18")  # assume 18 if detection fails
endif()

if(_CLANG_MAJOR VERSION_LESS "19")
    # Temporary: required until LLVM 19+ is installed.
    # _ALLOW_COMPILER_AND_STL_VERSION_MISMATCH suppresses the version gate only.
    set(CMAKE_CXX_FLAGS "/EHsc /MD /D_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH" CACHE STRING "" FORCE)
    set(CMAKE_C_FLAGS   "/MD /D_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH"        CACHE STRING "" FORCE)
    message(STATUS
        "clang-cl ${_CLANG_MAJOR}: _ALLOW_COMPILER_AND_STL_VERSION_MISMATCH active "
        "(remove when LLVM >= 19 is installed)")
else()
    set(CMAKE_CXX_FLAGS "/EHsc /MD" CACHE STRING "" FORCE)
    set(CMAKE_C_FLAGS   "/MD"       CACHE STRING "" FORCE)
endif()

# ---------------------------------------------------------------------------
# sccache — optional compiler cache.
# Activated when ENGINEMON_SCCACHE=1 is set in the environment.
# Falls back silently if sccache.exe is not present.
# Auto-detected from build/ sibling directory.
# Does not affect correctness; cache is keyed on compiler + flags + source.
# ---------------------------------------------------------------------------
if(DEFINED ENV{ENGINEMON_SCCACHE} AND "$ENV{ENGINEMON_SCCACHE}" STREQUAL "1")
    # Resolve relative to this toolchain file: cmake/toolchains/ → ../../build/
    get_filename_component(_REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
    set(_SCCACHE "${_REPO_ROOT}/build/sccache.exe")
    if(EXISTS "${_SCCACHE}")
        set(CMAKE_CXX_COMPILER_LAUNCHER "${_SCCACHE}" CACHE STRING "" FORCE)
        set(CMAKE_C_COMPILER_LAUNCHER   "${_SCCACHE}" CACHE STRING "" FORCE)
        message(STATUS "sccache enabled: ${_SCCACHE}")
    else()
        message(WARNING
            "ENGINEMON_SCCACHE=1 but sccache.exe not found at ${_SCCACHE} — "
            "run: Invoke-WebRequest to download, or disable ENGINEMON_SCCACHE")
    endif()
    unset(_SCCACHE)
    unset(_REPO_ROOT)
endif()
