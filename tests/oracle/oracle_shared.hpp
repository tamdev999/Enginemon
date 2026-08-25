// tests/oracle/oracle_shared.hpp
// Shared declarations for the split Oracle translation units.
// Definitions live in oracle_test_helpers.cpp.
// All oracle_test_*.cpp files include this header.
#pragma once

#include "crystal/rom/loader.hpp"
#include "crystal/rom/symbol_map.hpp"
#include "crystal/rom/bank_utils.hpp"
#include "crystal/rom/profile.hpp"
#include "crystal/script/typed_decoder.hpp"
#include "crystal/script/decoder.hpp"
#include "crystal/script/crystal_command.hpp"
#include "crystal/script/crystal_cfg.hpp"
#include "crystal/script/semantic_legalizer.hpp"
#include "crystal/script/legality_gate.hpp"
#include "crystal/script/semantic_lua_emitter.hpp"
#include "crystal/extract/map_extractor.hpp"
#include "crystal/extract/sprite_ids.hpp"
#include "crystal/extract/sprite_extractor.hpp"
#include "crystal/compile/full_compiler.hpp"
#include "crystal/output/native_package.hpp"
#include "engine/scripting/semantic_ir.hpp"
#include "engine/scripting/lua_runtime.hpp"
#include "engine/scripting/api_bindings.hpp"
#include "engine/core/game_loop.hpp"
#include "engine/core/game_state.hpp"
#include "engine/world/runtime_map.hpp"
#include "engine/world/runtime_tileset.hpp"
#include "engine/world/sprite_atlas.hpp"
#include "engine/world/collision_types.hpp"
#include "engine/package/package_reader.hpp"
#include "engine/build/package_cache.hpp"

#include <iostream>
#include <iomanip>
#include <sstream>
#include <fstream>
#include <filesystem>
#include <cassert>
#include <vector>
#include <cstdint>
#include <algorithm>
#include <memory>

// =============================================================================
// MINIMAL TEST FRAMEWORK
// =============================================================================

extern int  g_tests_passed;
extern int  g_tests_failed;
extern bool g_current_test_failed;

#define TEST(name) void test_##name()
#define RUN_TEST(name) run_test(#name, test_##name)

#define ASSERT_TRUE(cond) \
    do { \
        if (!(cond)) { \
            std::cerr << "  FAIL: " << #cond << " at line " << __LINE__ << "\n"; \
            g_current_test_failed = true; \
            return; \
        } \
    } while(0)

#define ASSERT_FALSE(cond) ASSERT_TRUE(!(cond))

#define ASSERT_EQ(a, b) \
    do { \
        if ((a) != (b)) { \
            std::cerr << "  FAIL: " << #a << " == " << #b \
                      << "  got " << static_cast<int64_t>(a) \
                      << " expected " << static_cast<int64_t>(b) \
                      << " at line " << __LINE__ << "\n"; \
            g_current_test_failed = true; \
            return; \
        } \
    } while(0)

#define ASSERT_STR_EQ(a, b) \
    do { \
        if ((a) != (b)) { \
            std::cerr << "  FAIL: " << #a << " == " << #b \
                      << "\n    got:      \"" << (a) << "\"" \
                      << "\n    expected: \"" << (b) << "\"" \
                      << " at line " << __LINE__ << "\n"; \
            g_current_test_failed = true; \
            return; \
        } \
    } while(0)

void run_test(const char* name, void (*test)());

// =============================================================================
// GLOBALS (set in main, declared extern for use across TUs)
// =============================================================================

extern const crystal::RomData*           g_rom;
extern const crystal::ExtractionProfile* g_profile;

// Phase 5+ globals
extern std::filesystem::path                     g_oracle_package_path;
extern std::unique_ptr<enginemon::PackageReader> g_oracle_reader;

// =============================================================================
// SHARED ORACLE HELPER DECLARATIONS
// =============================================================================

std::filesystem::path oracle_dir();
std::vector<uint8_t>  load_fixture(const std::string& relative_path);
std::unique_ptr<crystal::RomData> make_rom_from_bytes(const std::vector<uint8_t>& bytes);

crystal::CrystalScriptIR make_single_cmd_ir_with_entry(
    crystal::CrystalCommand cmd,
    uint32_t entry_address);

enginemon::LoweringResult lower_ir(const crystal::CrystalScriptIR& ir);
