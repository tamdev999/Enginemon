// battle_test.cpp
// Placeholder for engine-only battle tests.
//
// This target is intentionally minimal:
//   - Links only against enginemon_engine (NOT enginemon_crystal)
//   - No Crystal compiler IR headers
//   - No CrystalCommandData or SemanticOp variant instantiation
//   - Safe to add battle mechanics tests here without parallel OOM risk
//
// Battle implementation tests will be added here as battle systems are built.
// See engine/include/engine/battle/ for the calculator, trainer AI, and battle
// state headers that this target will eventually test.

#include <iostream>
#include <cassert>

// =============================================================================
// Minimal test framework
// =============================================================================

static int g_passed = 0;
static int g_failed = 0;
static bool g_test_failed = false;

#define ASSERT_TRUE(cond) \
    do { if (!(cond)) { std::cerr << "  FAIL: " #cond " at line " << __LINE__ << "\n"; g_test_failed = true; return; } } while (0)

static void run_test(const char* name, void (*fn)()) {
    std::cout << "Running " << name << "... ";
    g_test_failed = false;
    try {
        fn();
        if (g_test_failed) { std::cout << "FAIL\n"; g_failed++; }
        else               { std::cout << "PASS\n"; g_passed++; }
    } catch (const std::exception& e) {
        std::cout << "EXCEPTION: " << e.what() << "\n"; g_failed++;
    }
}

#define RUN(name) run_test(#name, test_##name)
#define TEST(name) static void test_##name()

// =============================================================================
// Smoke test — verifies the target itself compiles and links
// =============================================================================

TEST(battle_test_binary_links) {
    // This test exists purely to confirm the target builds and links.
    // A battle_test binary linked only against enginemon_engine with no
    // Crystal frontend dependencies is the desired state for all future
    // battle mechanics tests.
    ASSERT_TRUE(true);
    std::cout << "  [battle_test binary: engine-only link confirmed]\n";
}

// =============================================================================
// Main
// =============================================================================

int main(int /*argc*/, char* /*argv*/[]) {
    std::cout << "=== Battle Tests ===\n";
    RUN(battle_test_binary_links);
    std::cout << "\n=== Results ===\n";
    std::cout << "Passed: " << g_passed << "\n";
    std::cout << "Failed: " << g_failed << "\n";
    return g_failed > 0 ? 1 : 0;
}
