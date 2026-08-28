// runtime_test_integration.cpp — Production bootstrap + flag identity adversarial tests
//
// These tests verify:
//   1. Production bootstrap: LuaRuntime bound to GameState mutates GameState, not stubs
//   2. Flag identity: ctx.flags:set(N) and GameState::check_flag("flag_NNNN") use same key
//      even when N comes from Crystal flag encoding (which map extractor also uses)
//
// Execution contract:
//   LuaRuntime::execute_string() throws std::runtime_error on Lua syntax OR runtime error.
//   LuaRuntime::start_script()   throws std::runtime_error if script/function not found.
//   Therefore: a plain call (no ASSERT_TRUE) is the correct assertion —
//   any Lua failure terminates the test with a clear thrown exception.
//   Downstream state assertions prove the mutation actually occurred.

#include "engine/scripting/lua_runtime.hpp"
#include "engine/scripting/api_bindings.hpp"
#include "engine/core/game_state.hpp"
#include "engine/core/game_loop.hpp"

#include <iostream>
#include <cassert>
#include <format>
#include <stdexcept>

#define ASSERT_EQ(a, b)   do { auto _a = (a); auto _b = (b); if (_a != _b) { std::cerr << "ASSERT_EQ failed: " << #a << " (" << _a << ") != " << #b << " (" << _b << ") at " << __FILE__ << ":" << __LINE__ << "\n"; std::abort(); } } while(0)
#define ASSERT_TRUE(x)    do { if (!(x)) { std::cerr << "ASSERT_TRUE failed: " << #x << " at " << __FILE__ << ":" << __LINE__ << "\n"; std::abort(); } } while(0)
#define ASSERT_FALSE(x)   do { if ((x)) { std::cerr << "ASSERT_FALSE failed: " << #x << " at " << __FILE__ << ":" << __LINE__ << "\n"; std::abort(); } } while(0)
// ASSERT_THROWS: verifies that expr throws the expected type.
// This proves explicit failure surfaces rather than silently succeeding.
#define ASSERT_THROWS(ExcType, expr) \
    do { \
        bool _threw = false; \
        try { expr; } catch (const ExcType&) { _threw = true; } catch (...) {} \
        if (!_threw) { \
            std::cerr << "ASSERT_THROWS(" #ExcType ") failed: " #expr " did not throw at " \
                      << __FILE__ << ":" << __LINE__ << "\n"; \
            std::abort(); \
        } \
    } while(0)
#define TEST(name)        void test_##name()

// =============================================================================
// TEST 1: Production bootstrap — script mutates authoritative GameState
//
// When lua_runtime.set_game_state(&gs) is called before any script,
// ctx.flags:set(N) must write to GameState::flags, not StubServices::flags.
//
// execute_string throws on Lua error — plain call IS the no-throw assertion.
// Downstream ASSERT_TRUE(gs.check_flag(...)) proves mutation reached GameState.
// =============================================================================
TEST(production_bootstrap_script_mutates_gamestate) {
    using namespace enginemon;
    GameState gs;
    LuaRuntime rt;

    // Production bootstrap: bind authoritative GameState before any script
    rt.set_game_state(&gs);

    const char* code = R"(
script = {}
function script.main(ctx)
    ctx.flags:set(42)
    ctx.flags:set(100)
    return
end
return script
)";
    // execute_string throws std::runtime_error on Lua syntax or runtime error.
    // No throw here proves Lua compilation and chunk execution succeeded.
    rt.execute_string(code, "prod_bootstrap");
    // start_script throws if "script" global not found or has no main function.
    rt.start_script("script");

    // Flag 42 (0x2A) -> canonical key "flag_002a"
    // Flag 100 (0x64) -> canonical key "flag_0064"
    // These must be in GameState (not StubServices) because set_game_state was called.
    ASSERT_TRUE(gs.check_flag("flag_002a"));
    ASSERT_TRUE(gs.check_flag("flag_0064"));

    // StubServices::flags uses int keys. With GameState bound, no write goes there.
    auto& stubs = rt.get_stub_services();
    // stubs.flags should not have a true entry for key 42
    ASSERT_FALSE(stubs.flags.count(42) && stubs.flags.at(42));

    std::cout << "  [production bootstrap: ctx.flags:set -> GameState, not StubServices]\n";
}

// =============================================================================
// TEST 2: No-GameState fallback — script falls back to stubs when unbound
//
// Without set_game_state, ctx.flags:set writes to StubServices::flags only.
// GameState is untouched. Lua still executes successfully (no throw).
// =============================================================================
TEST(no_gamestate_flags_fallback_to_stubs) {
    using namespace enginemon;
    GameState gs;  // NOT bound
    LuaRuntime rt;
    // Deliberately NOT calling rt.set_game_state(&gs)

    const char* code = R"(
script = {}
function script.main(ctx)
    ctx.flags:set(42)
    return
end
return script
)";
    // No throw = Lua executed successfully even without GameState bound
    rt.execute_string(code, "no_gs_fallback");
    rt.start_script("script");

    // GameState must be completely untouched
    ASSERT_FALSE(gs.check_flag("flag_002a"));  // canonical hex format
    ASSERT_FALSE(gs.check_flag("flag_26"));    // old decimal format also absent

    // StubServices must have received the write
    auto& stubs = rt.get_stub_services();
    ASSERT_TRUE(stubs.flags.count(42) && stubs.flags.at(42));

    std::cout << "  [no-GameState: ctx.flags:set falls back to StubServices]\n";
}

// =============================================================================
// TEST 3: Lua syntax error surfaces via thrown exception (explicit failure proof)
//
// A script with a syntax error must throw std::runtime_error from execute_string.
// This proves the failure contract — Lua errors are never silent.
// =============================================================================
TEST(lua_syntax_error_throws_explicitly) {
    using namespace enginemon;
    LuaRuntime rt;

    const char* bad_code = "function broken( invalid syntax !!!";

    ASSERT_THROWS(std::runtime_error, rt.execute_string(bad_code, "bad_syntax"));

    std::cout << "  [Lua syntax error: execute_string throws std::runtime_error explicitly]\n";
}

// =============================================================================
// TEST 4: Canonical flag identity — hex key matches map extractor format
//
// The semantic lua emitter encodes flag as (ns<<16)|value. For an event flag
// with value 0x001A (26 decimal), the encoding is 26. api_bindings must produce
// "flag_001a" (lowercase hex, 4-padded) — identical to MapExtractor::make_flag_id.
//
// Adversarial check: the old broken decimal format "flag_26" must NOT appear.
// =============================================================================
TEST(flag_identity_hex_canonical) {
    using namespace enginemon;
    GameState gs;
    LuaRuntime rt;
    rt.set_game_state(&gs);

    const char* code = R"(
script = {}
function script.main(ctx)
    ctx.flags:set(26)    -- event flag 0x001A encoded as int 26 by semantic emitter
    ctx.flags:set(255)   -- 0x00FF -> "flag_00ff"
    ctx.flags:set(256)   -- 0x0100 -> "flag_0100"
    return
end
return script
)";
    // No throw = Lua executed successfully
    rt.execute_string(code, "flag_identity");
    rt.start_script("script");

    // Must use lowercase hex with 4 digits (canonical format matching make_flag_id)
    ASSERT_TRUE(gs.check_flag("flag_001a"));
    ASSERT_TRUE(gs.check_flag("flag_00ff"));
    ASSERT_TRUE(gs.check_flag("flag_0100"));

    // Must NOT use old decimal format (that was the bug)
    ASSERT_FALSE(gs.check_flag("flag_26"));
    ASSERT_FALSE(gs.check_flag("flag_255"));
    ASSERT_FALSE(gs.check_flag("flag_256"));

    std::cout << "  [flag identity: ctx.flags:set(26) -> 'flag_001a' (hex), not 'flag_26' (decimal)]\n";
}

// =============================================================================
// TEST 5: Flag set/clear/check roundtrip through GameState
// =============================================================================
TEST(flag_gamestate_set_clear_check_roundtrip) {
    using namespace enginemon;
    GameState gs;
    LuaRuntime rt;
    rt.set_game_state(&gs);

    const char* code = R"(
script = {}
function script.main(ctx)
    ctx.flags:set(0x1A)
    ctx.flags:set(0x2B)
    ctx.flags:clear(0x1A)
    ctx.flags:set_var(0, ctx.flags:check(0x1A))   -- 0 (cleared)
    ctx.flags:set_var(1, ctx.flags:check(0x2B))   -- 1 (set)
    return
end
return script
)";
    rt.execute_string(code, "flag_roundtrip");
    rt.start_script("script");

    ASSERT_FALSE(gs.check_flag("flag_001a"));  // cleared
    ASSERT_TRUE(gs.check_flag("flag_002b"));   // still set

    ASSERT_EQ(gs.get_var("var_0"), 0);  // check(0x1A) after clear -> 0
    ASSERT_EQ(gs.get_var("var_1"), 1);  // check(0x2B) -> 1

    std::cout << "  [flag roundtrip: set/clear/check all go through GameState with hex keys]\n";
}

// =============================================================================
// TEST 6: Adversarial — map condition_flag format matches script flag format
//
// Simulates the map extraction side: a BG event with condition_flag="flag_001a"
// (produced by make_flag_id(0x001A)) must be observable after a script sets
// event flag 0x001A via ctx.flags:set(26).
// =============================================================================
TEST(flag_map_condition_matches_script_set) {
    using namespace enginemon;
    GameState gs;
    LuaRuntime rt;
    rt.set_game_state(&gs);

    const char* code = R"(
script = {}
function script.main(ctx)
    ctx.flags:set(26)   -- event flag 0x001A: semantic emitter enc = (0<<16)|26 = 26
    return
end
return script
)";
    rt.execute_string(code, "flag_map_match");
    rt.start_script("script");

    // Map extraction side: make_flag_id(0x001A) -> "flag_001a"
    // game_loop.cpp flag_checker path: gs->check_flag(evt.condition_flag)
    const std::string condition_flag_from_map = std::format("flag_{:04x}", uint16_t(0x001A));
    ASSERT_EQ(condition_flag_from_map, std::string("flag_001a"));

    // The flag set by the script must be visible to the map condition check
    ASSERT_TRUE(gs.check_flag(condition_flag_from_map));

    std::cout << "  [adversarial: script set(26) observed by map condition_flag='flag_001a']\n";
}
