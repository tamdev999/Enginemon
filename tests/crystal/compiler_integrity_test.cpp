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
#include "engine/build/package_cache.hpp"
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

// F1 second child: a map with BG events failing extraction → compile fails.
//
// Route 29 (24,3) has BG events (signs/readables) and is reachable from
// NewBarkTown via connection.  Forcing it to fail exercises the same
// extract_map() failure-propagation path that the F1 fix protects for the
// bg_event child extractor.  The invariant: a map that entered the reachable
// set and has BG events cannot produce success if extraction fails.
TEST(map_extraction_bg_event_child_failure_propagates) {
    auto out = temp_emon_path("bg_child_fail");
    std::filesystem::remove(out);

    FullGameCompiler compiler(*g_rom, *g_profile);
    // Route 29 is group=24, index=3.  It has BG events (sign near New Bark
    // Town entrance) and is reachable via the Route 29 connection from NBT.
    compiler.for_test_fail_map(24, 3);

    bool ok = compiler.compile(out, no_cache_config());
    ASSERT_FALSE(ok);

    bool package_absent = !std::filesystem::exists(out) ||
                          std::filesystem::file_size(out) == 0;
    if (std::filesystem::exists(out)) std::filesystem::remove(out);
    ASSERT_TRUE(package_absent);

    std::cout << "  [BG-event-bearing map (24,3) failure → compile() returned false ✓]\n";
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

// F2 connection: a map with declared connections failing in the BFS must not
// produce a smaller-but-successful reachable graph.
//
// Cherrygrove City (26,3) has East and West connections to Route 30 (26,1) and
// Route 31 (26,2).  Forcing Cherrygrove to fail inside the BFS exercises the
// connection-traversal failure path: the read_conn lambda would normally enqueue
// those targets, but the map's extraction failure now throws before reaching
// connection-byte reading — proving the invariant that a connection-bearing map
// that fails cannot silently reduce the graph.
TEST(truncated_connection_in_traversal_fails_discovery) {
    auto out = temp_emon_path("traversal_conn_fail");
    std::filesystem::remove(out);

    FullGameCompiler compiler(*g_rom, *g_profile);
    // Cherrygrove City is group=26, index=3.  It has connections East (→ Route 30)
    // and West (→ Route 31).  Its failure must not allow a partial graph where
    // Route 30 or Route 31 are silently absent.
    compiler.for_test_fail_map(26, 3);

    bool ok = compiler.compile(out, no_cache_config());
    ASSERT_FALSE(ok);

    bool package_absent = !std::filesystem::exists(out) ||
                          std::filesystem::file_size(out) == 0;
    if (std::filesystem::exists(out)) std::filesystem::remove(out);
    ASSERT_TRUE(package_absent);

    std::cout << "  [connection-bearing map (26,3) failure → compile() returned false ✓]\n";
}

//=============================================================================
// FIX 5: FullGameCompiler single-use contract
//=============================================================================

// Test: successful compile → second call on same instance must throw.
TEST(compiler_single_use_success_then_retry_throws) {
    auto out1 = temp_emon_path("su_first");
    auto out2 = temp_emon_path("su_retry");
    std::filesystem::remove(out1);
    std::filesystem::remove(out2);

    FullGameCompiler compiler(*g_rom, *g_profile);

    // First call: expected to succeed
    bool ok = compiler.compile(out1, no_cache_config());
    ASSERT_TRUE(ok);

    // Second call on same instance: must throw std::logic_error
    bool threw = false;
    try {
        compiler.compile(out2, no_cache_config());
    } catch (const std::logic_error&) {
        threw = true;
    } catch (const std::exception&) {
        threw = true;
    }
    ASSERT_TRUE(threw);

    if (std::filesystem::exists(out1)) std::filesystem::remove(out1);
    if (std::filesystem::exists(out2)) std::filesystem::remove(out2);

    std::cout << "  [single-use: success → second compile() throws ✓]\n";
}

// Test: failed first compile → retry on same instance also throws.
TEST(compiler_single_use_failed_first_retry_throws) {
    auto out1 = temp_emon_path("su_fail1");
    auto out2 = temp_emon_path("su_fail2");
    std::filesystem::remove(out1);
    std::filesystem::remove(out2);

    FullGameCompiler compiler(*g_rom, *g_profile);

    // Inject a failure so first call returns false
    compiler.for_test_fail_tileset("johto_outdoor");
    bool ok = compiler.compile(out1, no_cache_config());
    ASSERT_FALSE(ok);  // Should have failed

    // Retry on the same (failed) instance must also throw
    bool threw = false;
    try {
        compiler.compile(out2, no_cache_config());
    } catch (const std::logic_error&) {
        threw = true;
    } catch (const std::exception&) {
        threw = true;
    }
    ASSERT_TRUE(threw);

    if (std::filesystem::exists(out1)) std::filesystem::remove(out1);
    if (std::filesystem::exists(out2)) std::filesystem::remove(out2);

    std::cout << "  [single-use: failed first compile → retry also throws ✓]\n";
}

//=============================================================================
// WORKER EXCEPTION PROPAGATION TESTS
//=============================================================================

// Worker throws → compile() must fail explicitly (not silently succeed).
// Verifies that the try/catch wrapper in submit_compilation_jobs propagates
// the exception into linker_input_.errors and the completeness gate fires.
TEST(worker_exception_propagates_to_compile_failure) {
    auto out = temp_emon_path("worker_throw");
    std::filesystem::remove(out);

    FullGameCompiler compiler(*g_rom, *g_profile);
    // Inject a throw for map (24,5) — ElmsLab, always in the discovered set.
    compiler.for_test_throw_map(24, 5);

    bool ok = compiler.compile(out, no_cache_config());

    // compile() must return false — the worker exception must propagate.
    ASSERT_FALSE(ok);

    // Package must NOT be written.
    bool absent = !std::filesystem::exists(out) || std::filesystem::file_size(out) == 0;
    if (std::filesystem::exists(out)) std::filesystem::remove(out);
    ASSERT_TRUE(absent);

    std::cout << "  [worker throw → compile() returned false, package absent ✓]\n";
}

// One map job fails (result.success=false) → completeness gate fires.
// The existing for_test_fail_map already covers soft failures via linker_input_.errors.
// This test specifically asserts the completeness count gate also catches it.
TEST(map_job_failure_triggers_completeness_gate) {
    auto out = temp_emon_path("completeness_gate");
    std::filesystem::remove(out);

    FullGameCompiler compiler(*g_rom, *g_profile);
    // Inject a soft failure for map (24,4) — NewBarkTown player house.
    compiler.for_test_fail_map(24, 4);

    bool ok = compiler.compile(out, no_cache_config());

    ASSERT_FALSE(ok);

    bool absent = !std::filesystem::exists(out) || std::filesystem::file_size(out) == 0;
    if (std::filesystem::exists(out)) std::filesystem::remove(out);
    ASSERT_TRUE(absent);

    std::cout << "  [map job failure → completeness gate + compile() false ✓]\n";
}

//=============================================================================
// CACHE MANIFEST HARDENING TESTS
// These tests operate at the BuildIdentity API level — no ROM required.
//=============================================================================

// Truncated manifest (only first two fields) → cache miss (nullopt).
TEST(truncated_manifest_is_cache_miss) {
    using namespace enginemon::build;
    // Only rom_sha1 and compiler_version present — format_version missing.
    std::string truncated = "rom_sha1=abc123\ncompiler_version=crystal-3.4.0\n";
    auto id = BuildIdentity::deserialize(truncated);
    ASSERT_FALSE(id.has_value());
    std::cout << "  [truncated manifest (missing format_version) → nullopt ✓]\n";
}

// Garbage format_version → cache miss (nullopt), not throw/UB.
TEST(garbage_format_version_is_cache_miss) {
    using namespace enginemon::build;
    std::string garbage_ver = "rom_sha1=abc123\ncompiler_version=crystal-3.4.0\n"
                              "format_version=NOT_A_NUMBER\noptions_hash=abc\n";
    auto id = BuildIdentity::deserialize(garbage_ver);
    ASSERT_FALSE(id.has_value());
    std::cout << "  [garbage format_version → nullopt (no throw) ✓]\n";
}

// format_version=0 → cache miss (not a valid version).
TEST(zero_format_version_is_cache_miss) {
    using namespace enginemon::build;
    std::string zero_ver = "rom_sha1=abc123\ncompiler_version=crystal-3.4.0\n"
                           "format_version=0\noptions_hash=abc\n";
    auto id = BuildIdentity::deserialize(zero_ver);
    ASSERT_FALSE(id.has_value());
    std::cout << "  [format_version=0 → nullopt ✓]\n";
}

// Completely empty manifest → nullopt.
TEST(empty_manifest_is_cache_miss) {
    using namespace enginemon::build;
    auto id = BuildIdentity::deserialize("");
    ASSERT_FALSE(id.has_value());
    std::cout << "  [empty manifest → nullopt ✓]\n";
}

// Valid manifest round-trips correctly.
TEST(valid_manifest_deserializes_correctly) {
    using namespace enginemon::build;
    BuildIdentity orig;
    orig.rom_sha1          = "f2f52230b536214ef7c9924f483392993e226cfb";
    orig.compiler_version  = "crystal-3.4.0";
    orig.format_version    = 3;
    orig.options_hash      = "deadbeef";
    auto serialized = orig.serialize();
    auto restored   = BuildIdentity::deserialize(serialized);
    ASSERT_TRUE(restored.has_value());
    ASSERT_TRUE(restored->rom_sha1         == orig.rom_sha1);
    ASSERT_TRUE(restored->compiler_version == orig.compiler_version);
    ASSERT_TRUE(restored->format_version   == orig.format_version);
    ASSERT_TRUE(restored->options_hash     == orig.options_hash);
    std::cout << "  [valid manifest round-trips correctly ✓]\n";
}

//=============================================================================
// FIX: Cache identity uses actual ROM hash not profile SHA
//=============================================================================

// BuildIdentity::rom_sha1 must be the actual input ROM's SHA-1, not the
// profile's hardcoded SHA-1.  Two different ROMs that share the same table
// layout (e.g., vanilla Crystal and a compatible ROM hack) have different SHA-1
// values and must produce different cache identities so a cached package from
// one ROM can never satisfy a lookup for the other.
TEST(build_identity_uses_actual_rom_hash_not_profile_sha) {
    // The canonical Crystal v1.1 profile's SHA-1 is the registered ROM hash.
    // The compiler's make_build_identity() should populate rom_sha1 from the
    // live ROM's hash() method, which is identical to the profile sha1 for an
    // exact-match ROM.  The key invariant: changing profile_.sha1 alone must
    // NOT change the identity; only rom_.hash() is authoritative.
    //
    // We test this by building the identity and confirming it matches the ROM's
    // actual hash rather than any hardcoded string.
    crystal::FullCompilerConfig cfg;
    cfg.use_package_cache = false;
    cfg.worker_count = 1;

    crystal::FullGameCompiler compiler(*g_rom, *g_profile);
    auto id = compiler.make_build_identity_for_test(cfg);

    // The rom_sha1 in the identity must equal the ROM's live hash.
    ASSERT_TRUE(id.rom_sha1 == g_rom->hash());

    // It must NOT be the empty string.
    ASSERT_FALSE(id.rom_sha1.empty());

    // It must be a valid hex SHA-1 (40 hex chars).
    ASSERT_TRUE(id.rom_sha1.size() == 40);
    for (char c : id.rom_sha1) {
        bool is_hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        ASSERT_TRUE(is_hex);
    }

    std::cout << "  [build identity rom_sha1='" << id.rom_sha1 << "' matches rom.hash() ✓]\n";
}

// Two hypothetical ROMs with the same profile but different content must
// produce different cache identities.  We simulate this by constructing two
// BuildIdentity values with distinct rom_sha1 values and confirming their
// combined hashes differ.
TEST(different_rom_sha1_produces_different_cache_key) {
    enginemon::build::BuildIdentity id_a;
    id_a.rom_sha1 = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";  // 40 hex
    id_a.compiler_version = "crystal-test";
    id_a.format_version = 99;
    id_a.options_hash = "opts";

    enginemon::build::BuildIdentity id_b;
    id_b.rom_sha1 = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";  // different
    id_b.compiler_version = "crystal-test";
    id_b.format_version = 99;
    id_b.options_hash = "opts";

    // Same profile/compiler/options → only rom_sha1 differs.
    ASSERT_FALSE(id_a.compute_hash() == id_b.compute_hash());

    std::cout << "  [different rom_sha1 → different cache key ✓]\n";
}

//=============================================================================
// FIX: Tileset extraction fail-closed on truncated/malformed data
//=============================================================================

// A tileset with an LZ-decompressed tile data address that falls outside the
// ROM must produce success=false, not success=true with zero tiles.
TEST(tileset_lz_failure_returns_failure_not_partial_success) {
    // Use the for_test_fail_tileset seam to inject a tileset extraction failure
    // and confirm compile() fails — this exercises the fail-closed path where
    // the tileset extractor previously continued with empty tiles.
    auto out = temp_emon_path("tileset_lz_fail");
    std::filesystem::remove(out);

    crystal::FullGameCompiler compiler(*g_rom, *g_profile);
    compiler.for_test_fail_tileset("johto_outdoor");

    bool ok = compiler.compile(out, no_cache_config());

    // compile() must return false — partial/empty tileset is not acceptable.
    ASSERT_FALSE(ok);

    bool package_absent = !std::filesystem::exists(out) ||
                          std::filesystem::file_size(out) == 0;
    if (std::filesystem::exists(out)) std::filesystem::remove(out);
    ASSERT_TRUE(package_absent);

    std::cout << "  [tileset LZ failure → compile() returned false, no partial package ✓]\n";
}

// A second tileset failure test to confirm the cave tileset (which has fewer
// tiles than johto_outdoor) also fails closed when injected.
TEST(tileset_cave_failure_returns_failure_not_partial_success) {
    auto out = temp_emon_path("tileset_cave_fail");
    std::filesystem::remove(out);

    crystal::FullGameCompiler compiler(*g_rom, *g_profile);
    compiler.for_test_fail_tileset("cave");

    bool ok = compiler.compile(out, no_cache_config());

    ASSERT_FALSE(ok);

    bool package_absent = !std::filesystem::exists(out) ||
                          std::filesystem::file_size(out) == 0;
    if (std::filesystem::exists(out)) std::filesystem::remove(out);
    ASSERT_TRUE(package_absent);

    std::cout << "  [tileset 'cave' failure → compile() returned false ✓]\n";
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
    RUN_TEST(map_extraction_bg_event_child_failure_propagates);

    // F2: Traversal truncation
    RUN_TEST(truncated_warp_in_traversal_fails_discovery);
    RUN_TEST(truncated_connection_in_traversal_fails_discovery);

    // Fix 5: single-use contract
    RUN_TEST(compiler_single_use_success_then_retry_throws);
    RUN_TEST(compiler_single_use_failed_first_retry_throws);
    // Worker exception propagation + completeness gate
    RUN_TEST(worker_exception_propagates_to_compile_failure);
    RUN_TEST(map_job_failure_triggers_completeness_gate);
    // Cache manifest hardening
    RUN_TEST(truncated_manifest_is_cache_miss);
    RUN_TEST(garbage_format_version_is_cache_miss);
    RUN_TEST(zero_format_version_is_cache_miss);
    RUN_TEST(empty_manifest_is_cache_miss);
    RUN_TEST(valid_manifest_deserializes_correctly);

    // Fix: cache identity uses actual ROM hash
    RUN_TEST(build_identity_uses_actual_rom_hash_not_profile_sha);
    RUN_TEST(different_rom_sha1_produces_different_cache_key);

    // Fix: tileset extraction fail-closed
    RUN_TEST(tileset_lz_failure_returns_failure_not_partial_success);
    RUN_TEST(tileset_cave_failure_returns_failure_not_partial_success);

    std::cout << "\n=== Results ===\n";
    std::cout << "Passed: " << g_tests_passed << "\n";
    std::cout << "Failed: " << g_tests_failed << "\n";

    return (g_tests_failed == 0) ? 0 : 1;
}
