// tests/crystal/compiler_integrity_test.cpp
//
// Adversarial tests for the two confirmed fail-open compiler bugs:
//
//   FINDING 1 — Asset extraction must fail closed
//     Every required asset (tileset, sprite, font, OBJ palettes) whose
//     extraction fails must cause compile() to return false.
//     The previous behaviour was to silently skip the asset and proceed.
//
//   FINDING 2 — Reachable-map discovery must fail closed
//     A map that enters the BFS reachable set but whose extraction fails
//     must cause discovery — and therefore compile() — to return false.
//     The previous behaviour was to continue the BFS and silently drop
//     the failing map, producing a partial (potentially incomplete) graph.
//
// Each test:
//   1. Loads the real Crystal ROM (argv[1])
//   2. Injects a specific failure via the for_test_* seam
//   3. Asserts compile() returns false
//   4. Verifies the package file was NOT written (or is absent/empty)
//
// A positive (no-injection) test confirms the baseline still compiles.
//
// Run: compiler_integrity_test <rom_path>

#include "crystal/rom/loader.hpp"
#include "crystal/rom/profile.hpp"
#include "crystal/compile/full_compiler.hpp"
#include "crystal/extract/map_extractor.hpp"
#include <iostream>
#include <filesystem>
#include <cassert>

using namespace crystal;

//=============================================================================
// TEST FRAMEWORK
//=============================================================================

static int g_tests_passed = 0;
static int g_tests_failed = 0;
static bool g_current_test_failed = false;

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

void run_test(const char* name, void (*test)()) {
    std::cout << "Running " << name << "... ";
    std::cout.flush();
    g_current_test_failed = false;
    try {
        test();
        if (g_current_test_failed) {
            std::cout << "FAIL\n";
            g_tests_failed++;
        } else {
            std::cout << "PASS\n";
            g_tests_passed++;
        }
    } catch (const std::exception& e) {
        std::cout << "EXCEPTION: " << e.what() << "\n";
        g_tests_failed++;
    }
}

//=============================================================================
// GLOBALS
//=============================================================================

static const RomData*         g_rom     = nullptr;
static const ExtractionProfile* g_profile = nullptr;

// Helper: return a unique temp path for the output package
static std::filesystem::path temp_emon_path(const std::string& tag) {
    return std::filesystem::temp_directory_path()
           / ("enginemon_integrity_" + tag + ".emon");
}

// Helper: build a compiler config with caching disabled so every test
// exercises the full pipeline without short-circuiting via cached packages.
static FullCompilerConfig no_cache_config() {
    FullCompilerConfig cfg;
    cfg.use_package_cache = false;
    cfg.worker_count = 1;  // Deterministic single-threaded for tests
    return cfg;
}

//=============================================================================
// FINDING 1 TESTS — Asset extraction fail-closed
//=============================================================================

// ------------------------------------------------------------------
// Positive control: baseline compile succeeds with all assets intact.
// If this test fails, the ROM is unusable for further tests.
// ------------------------------------------------------------------
TEST(baseline_compile_succeeds) {
    auto out = temp_emon_path("baseline");
    std::filesystem::remove(out);

    FullGameCompiler compiler(*g_rom, *g_profile);
    bool ok = compiler.compile(out, no_cache_config());

    ASSERT_TRUE(ok);
    ASSERT_TRUE(std::filesystem::exists(out));
    ASSERT_TRUE(std::filesystem::file_size(out) > 0);

    std::filesystem::remove(out);
    std::cout << "  [baseline: compile() returned true, package written ✓]\n";
}

// ------------------------------------------------------------------
// Tileset extraction failure → compile() must return false.
//
// New Bark Town uses the "johto_outdoor" tileset.  We inject a failure
// for that specific tileset in link_results() Phase 4.
// ------------------------------------------------------------------
TEST(tileset_extraction_failure_fails_compile) {
    auto out = temp_emon_path("tileset_fail");
    std::filesystem::remove(out);

    FullGameCompiler compiler(*g_rom, *g_profile);
    compiler.for_test_fail_tileset("johto_outdoor");

    bool ok = compiler.compile(out, no_cache_config());

    // compile() must return false — missing required tileset
    ASSERT_FALSE(ok);

    // Package must NOT exist or must be empty (was never serialised)
    bool package_absent = !std::filesystem::exists(out) ||
                          std::filesystem::file_size(out) == 0;
    if (std::filesystem::exists(out)) std::filesystem::remove(out);
    ASSERT_TRUE(package_absent);

    std::cout << "  [tileset 'johto_outdoor' failure → compile() returned false ✓]\n";
}

// ------------------------------------------------------------------
// Sprite extraction failure → compile() must return false.
//
// "chris" is the player sprite, always in content_.sprites.
// ------------------------------------------------------------------
TEST(sprite_extraction_failure_fails_compile) {
    auto out = temp_emon_path("sprite_fail");
    std::filesystem::remove(out);

    FullGameCompiler compiler(*g_rom, *g_profile);
    compiler.for_test_fail_sprite("chris");

    bool ok = compiler.compile(out, no_cache_config());

    ASSERT_FALSE(ok);

    bool package_absent = !std::filesystem::exists(out) ||
                          std::filesystem::file_size(out) == 0;
    if (std::filesystem::exists(out)) std::filesystem::remove(out);
    ASSERT_TRUE(package_absent);

    std::cout << "  [sprite 'chris' failure → compile() returned false ✓]\n";
}

// ------------------------------------------------------------------
// Font extraction failure → compile() must return false.
// ------------------------------------------------------------------
TEST(font_extraction_failure_fails_compile) {
    auto out = temp_emon_path("font_fail");
    std::filesystem::remove(out);

    FullGameCompiler compiler(*g_rom, *g_profile);
    compiler.for_test_fail_font();

    bool ok = compiler.compile(out, no_cache_config());

    ASSERT_FALSE(ok);

    bool package_absent = !std::filesystem::exists(out) ||
                          std::filesystem::file_size(out) == 0;
    if (std::filesystem::exists(out)) std::filesystem::remove(out);
    ASSERT_TRUE(package_absent);

    std::cout << "  [font failure → compile() returned false ✓]\n";
}

// ------------------------------------------------------------------
// OBJ palette extraction failure → compile() must return false.
// ------------------------------------------------------------------
TEST(obj_palettes_extraction_failure_fails_compile) {
    auto out = temp_emon_path("palettes_fail");
    std::filesystem::remove(out);

    FullGameCompiler compiler(*g_rom, *g_profile);
    compiler.for_test_fail_palettes();

    bool ok = compiler.compile(out, no_cache_config());

    ASSERT_FALSE(ok);

    bool package_absent = !std::filesystem::exists(out) ||
                          std::filesystem::file_size(out) == 0;
    if (std::filesystem::exists(out)) std::filesystem::remove(out);
    ASSERT_TRUE(package_absent);

    std::cout << "  [OBJ palette failure → compile() returned false ✓]\n";
}

//=============================================================================
// FINDING 2 TESTS — Map discovery fail-closed
//=============================================================================

// ------------------------------------------------------------------
// A→B→C graph: seed (A=NewBarkTown, 24,4) → warp → B=ElmsLab (24,5)
// → B's interior warps → C=... 
// Force B=ElmsLab to fail extraction.
// Expected: discover_reachable_maps() throws → discover_content() returns
// false → compile() returns false.
// The old behaviour was: B silently dropped, BFS continued, compile succeeded.
// ------------------------------------------------------------------
TEST(reachable_map_B_extraction_failure_fails_discovery) {
    auto out = temp_emon_path("map_B_fail");
    std::filesystem::remove(out);

    FullGameCompiler compiler(*g_rom, *g_profile);
    // ElmsLab is group=24, index=5 (verified from maps.asm and golden tests)
    compiler.for_test_fail_map(24, 5);

    bool ok = compiler.compile(out, no_cache_config());

    ASSERT_FALSE(ok);

    bool package_absent = !std::filesystem::exists(out) ||
                          std::filesystem::file_size(out) == 0;
    if (std::filesystem::exists(out)) std::filesystem::remove(out);
    ASSERT_TRUE(package_absent);

    std::cout << "  [reachable map B=(24,5) failure → compile() returned false ✓]\n";
}

// ------------------------------------------------------------------
// Root/seed map extraction failure → compile() must fail.
// NewBarkTown (24,4) is the primary seed.  Forcing it to fail must
// cause immediate discovery failure, not a smaller reachable set.
// ------------------------------------------------------------------
TEST(seed_map_extraction_failure_fails_discovery) {
    auto out = temp_emon_path("map_seed_fail");
    std::filesystem::remove(out);

    FullGameCompiler compiler(*g_rom, *g_profile);
    // NewBarkTown is the primary seed: group=24, index=4
    compiler.for_test_fail_map(24, 4);

    bool ok = compiler.compile(out, no_cache_config());

    ASSERT_FALSE(ok);

    bool package_absent = !std::filesystem::exists(out) ||
                          std::filesystem::file_size(out) == 0;
    if (std::filesystem::exists(out)) std::filesystem::remove(out);
    ASSERT_TRUE(package_absent);

    std::cout << "  [seed map (24,4) failure → compile() returned false ✓]\n";
}

// ------------------------------------------------------------------
// Successful reachable chain with no injected failures must still
// discover and compile the normal graph.  This is the positive
// companion to the A→B→C failure tests.
// ------------------------------------------------------------------
TEST(successful_reachable_chain_no_injection) {
    auto out = temp_emon_path("chain_ok");
    std::filesystem::remove(out);

    FullGameCompiler compiler(*g_rom, *g_profile);
    // No failure injection

    bool ok = compiler.compile(out, no_cache_config());

    ASSERT_TRUE(ok);
    ASSERT_TRUE(std::filesystem::exists(out));
    ASSERT_TRUE(std::filesystem::file_size(out) > 0);

    std::filesystem::remove(out);
    std::cout << "  [no injection → compile() succeeds with full reachable graph ✓]\n";
}

//=============================================================================
// F1 — MapExtractor partial-success propagation
//=============================================================================

// Truncated warp in a reachable map → extract_map fails → compile fails.
// We use the for_test_fail_map seam to make a map fail completely.
// The warp-truncation path itself is proven by the adversarial ROM test in
// oracle_test.cpp; here we prove the compile() pipeline propagates map
// extraction failure from the child-extractor path.
TEST(map_extraction_child_failure_propagates) {
    auto out = temp_emon_path("child_fail");
    std::filesystem::remove(out);

    FullGameCompiler compiler(*g_rom, *g_profile);
    // ElmsLab is directly reachable from NewBarkTown (warp from NBT → Elm's Lab).
    // Forcing it to fail tests that a reachable map whose extraction fails
    // propagates as a hard compile failure — not a reduced-but-successful graph.
    compiler.for_test_fail_map(24, 5);

    bool ok = compiler.compile(out, no_cache_config());
    ASSERT_FALSE(ok);

    bool package_absent = !std::filesystem::exists(out) ||
                          std::filesystem::file_size(out) == 0;
    if (std::filesystem::exists(out)) std::filesystem::remove(out);
    ASSERT_TRUE(package_absent);

    std::cout << "  [map child failure → compile() returned false ✓]\n";
}

//=============================================================================
// F2 — Reachable-map traversal truncation
//=============================================================================

// Injected warp-truncation path: a reachable map whose warp data is truncated
// must cause discover_reachable_maps() to throw, which discover_content()
// catches and converts to a hard compile failure.
// We simulate this by forcing the BFS to encounter a map that fails extraction,
// which then exercises the same throw path.
TEST(truncated_warp_in_traversal_fails_discovery) {
    auto out = temp_emon_path("traversal_warp_fail");
    std::filesystem::remove(out);

    FullGameCompiler compiler(*g_rom, *g_profile);
    // Player's house 1F (24,6) is reachable from NBT via warp.
    // Forcing it to fail exercises the traversal-failure path for a
    // non-seed, non-primary map in the BFS graph.
    compiler.for_test_fail_map(24, 6);

    bool ok = compiler.compile(out, no_cache_config());
    ASSERT_FALSE(ok);

    bool package_absent = !std::filesystem::exists(out) ||
                          std::filesystem::file_size(out) == 0;
    if (std::filesystem::exists(out)) std::filesystem::remove(out);
    ASSERT_TRUE(package_absent);

    std::cout << "  [traversal warp failure → compile() returned false ✓]\n";
}

//=============================================================================
// MAIN
//=============================================================================

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <rom_path>\n";
        return 1;
    }

    std::cout << "Loading ROM: " << argv[1] << "\n";
    auto rom = RomData::load(argv[1]);
    if (!rom) {
        std::cerr << "Failed to load ROM\n";
        return 1;
    }

    auto& registry = ProfileRegistry::instance();
    auto profile = registry.get_profile_by_hash(rom->hash());
    if (!profile) {
        std::cerr << "ROM profile not recognised — requires Crystal USA v1.1\n";
        return 1;
    }

    g_rom     = rom.get();
    g_profile = profile;

    std::cout << "ROM: " << profile->version_string << "\n";
    std::cout << "\n=== Compiler Integrity Tests ===\n\n";

    // Finding 1: asset fail-closed
    RUN_TEST(baseline_compile_succeeds);
    RUN_TEST(tileset_extraction_failure_fails_compile);
    RUN_TEST(sprite_extraction_failure_fails_compile);
    RUN_TEST(font_extraction_failure_fails_compile);
    RUN_TEST(obj_palettes_extraction_failure_fails_compile);

    // Finding 2: map discovery fail-closed
    RUN_TEST(reachable_map_B_extraction_failure_fails_discovery);
    RUN_TEST(seed_map_extraction_failure_fails_discovery);
    RUN_TEST(successful_reachable_chain_no_injection);

    // F1: MapExtractor child failure propagation
    RUN_TEST(map_extraction_child_failure_propagates);

    // F2: Traversal truncation
    RUN_TEST(truncated_warp_in_traversal_fails_discovery);

    std::cout << "\n=== Results ===\n";
    std::cout << "Passed: " << g_tests_passed << "\n";
    std::cout << "Failed: " << g_tests_failed << "\n";

    return (g_tests_failed == 0) ? 0 : 1;
}
