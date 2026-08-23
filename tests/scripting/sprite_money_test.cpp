// tests/scripting/sprite_money_test.cpp
// Focused tests for variable sprite identity model and money text state.
// These do NOT require a ROM path — GameState/LuaRuntime only.
//
// Verifies:
//   - Variable sprite assignments store stable SpriteId strings (not Crystal indices)
//   - Variable sprite state survives serialize/try_deserialize
//   - Money balance survives save/load
//   - Transient money text buffers are NOT stored in GameState
//   - prepare_money_text reflects current balance
//   - Player vs mom accounts remain distinct

#include "engine/scripting/lua_runtime.hpp"
#include "engine/scripting/api_bindings.hpp"
#include "engine/core/game_state.hpp"
#include "engine/core/types.hpp"
#include <iostream>
#include <cassert>
#include <string>

using namespace enginemon;

//=============================================================================
// MINIMAL TEST FRAMEWORK
//=============================================================================

static int g_passed = 0;
static int g_failed = 0;
static bool g_test_failed = false;

#define TEST(name) void test_##name()
#define RUN(name) run_test(#name, test_##name)

#define ASSERT_TRUE(cond) \
    do { if (!(cond)) { \
        std::cerr << "  FAIL: " #cond " at line " << __LINE__ << "\n"; \
        g_test_failed = true; return; } } while(0)

#define ASSERT_FALSE(cond) ASSERT_TRUE(!(cond))
#define ASSERT_EQ(a, b)    ASSERT_TRUE((a) == (b))
#define ASSERT_STR_EQ(a, b) ASSERT_TRUE(std::string(a) == std::string(b))

void run_test(const char* name, void(*fn)()) {
    std::cout << "  " << name << "... ";
    g_test_failed = false;
    try { fn(); }
    catch (const std::exception& e) {
        std::cerr << "EXCEPTION: " << e.what() << "\n";
        g_test_failed = true;
    }
    if (g_test_failed) { std::cout << "FAIL\n"; ++g_failed; }
    else               { std::cout << "PASS\n"; ++g_passed; }
}

//=============================================================================
// TESTS
//=============================================================================

TEST(variable_sprite_identity_survives_save_load) {
    // Variable sprite assignment must survive serialize/try_deserialize.
    // The stable SpriteId string must be preserved exactly — no Crystal index.
    GameState original;
    original.variable_sprites["copycat"]       = "fixed:lass";
    original.variable_sprites["fuchsia_gym_1"] = "fixed:janine";

    auto bytes = original.serialize();
    auto result = GameState::try_deserialize(bytes);
    ASSERT_TRUE(result.ok());

    const auto& loaded = result.state;
    auto it_c = loaded.variable_sprites.find("copycat");
    auto it_f = loaded.variable_sprites.find("fuchsia_gym_1");
    ASSERT_TRUE(it_c != loaded.variable_sprites.end());
    ASSERT_TRUE(it_f != loaded.variable_sprites.end());
    ASSERT_STR_EQ(it_c->second, "fixed:lass");
    ASSERT_STR_EQ(it_f->second, "fixed:janine");
}

TEST(variable_sprite_runtime_no_crystal_mapping_call) {
    // Confirm the variable sprite roundtrip does NOT reconstruct identity
    // from a Crystal numeric index. The stored value IS the final sprite_id.
    GameState gs;
    gs.variable_sprites["olivine_rival"] = "fixed:rival";

    // Read back directly — no mapping needed, value is already the sprite_id.
    ASSERT_STR_EQ(gs.variable_sprites.at("olivine_rival"), "fixed:rival");

    // Simulate what sprite_id_variable_resolve does (without any Crystal fn):
    auto it = gs.variable_sprites.find("olivine_rival");
    ASSERT_TRUE(it != gs.variable_sprites.end());
    // The returned value is used directly as the package key after stripping "fixed:"
    std::string resolved = it->second;  // "fixed:rival"
    ASSERT_TRUE(resolved.starts_with("fixed:"));
    std::string pkg_key = resolved.substr(6);  // "rival"
    ASSERT_STR_EQ(pkg_key, "rival");
}

TEST(money_balance_survives_save_load) {
    GameState original;
    original.variables["money_player"] = 9999;
    original.variables["money_mom"]    = 3000;

    auto bytes = original.serialize();
    auto result = GameState::try_deserialize(bytes);
    ASSERT_TRUE(result.ok());

    const auto& loaded = result.state;
    auto it_p = loaded.variables.find("money_player");
    auto it_m = loaded.variables.find("money_mom");
    ASSERT_TRUE(it_p != loaded.variables.end());
    ASSERT_TRUE(it_m != loaded.variables.end());
    ASSERT_EQ(it_p->second, 9999);
    ASSERT_EQ(it_m->second, 3000);
}

TEST(money_transient_text_buffer_not_in_gamestate) {
    // The transient text buffer (strbuf<N>_money) must NOT appear in GameState.
    // Buffers are written to StubServices::text_buffers, NOT gs.variables.
    GameState gs;
    gs.variables["money_player"] = 500;

    // Confirm strbuf key is not in variables (it lives in StubServices)
    ASSERT_TRUE(gs.variables.find("strbuf1_money") == gs.variables.end());

    // After save/load: balance is preserved, no text buffer artifact.
    auto bytes = gs.serialize();
    auto result = GameState::try_deserialize(bytes);
    ASSERT_TRUE(result.ok());
    ASSERT_TRUE(result.state.variables.find("strbuf1_money") == result.state.variables.end());
    ASSERT_EQ(result.state.variables.at("money_player"), 500);
}

TEST(money_prepare_text_reflects_current_balance) {
    // prepare_money_text writes the current balance to StubServices::text_buffers.
    std::string script_lua = R"(
script = {}
function script.main(ctx)
    ctx.inventory:give_money(1234, 0)
    ctx.inventory:prepare_money_text(0, 0)
    return
end
return script
)";

    LuaRuntime runtime;
    GameState gs;
    runtime.set_game_state(&gs);
    runtime.execute_string(script_lua, "reflect");
    runtime.start_script("script");

    // Balance authoritative in GameState.
    ASSERT_EQ(gs.variables.at("money_player"), 1234);
    // Text buffer reflects the balance (transient, in StubServices).
    ASSERT_EQ(runtime.get_stub_services().text_buffers.at("strbuf0_money"), 1234);
    // NOT in GameState::variables.
    ASSERT_TRUE(gs.variables.find("strbuf0_money") == gs.variables.end());
}

TEST(money_mom_account_prepare_text_distinct) {
    // Mom's prepare_text reads mom balance, player's reads player balance.
    std::string script_lua = R"(
script = {}
function script.main(ctx)
    ctx.inventory:give_money(100, 0)    -- player
    ctx.inventory:give_money(999, 1)    -- mom
    ctx.inventory:prepare_money_text(0, 0)  -- player → strbuf0_money
    ctx.inventory:prepare_money_text(1, 1)  -- mom    → strbuf1_money
    return
end
return script
)";

    LuaRuntime runtime;
    GameState gs;
    runtime.set_game_state(&gs);
    runtime.execute_string(script_lua, "mom_acct");
    runtime.start_script("script");

    auto& bufs = runtime.get_stub_services().text_buffers;
    ASSERT_EQ(bufs.at("strbuf0_money"), 100);
    ASSERT_EQ(bufs.at("strbuf1_money"), 999);
    ASSERT_TRUE(bufs.at("strbuf0_money") != bufs.at("strbuf1_money"));
}

//=============================================================================
// MAIN
//=============================================================================

int main(int /*argc*/, char* /*argv*/[]) {
    std::cout << "=== Sprite/Money State Tests ===\n";

    RUN(variable_sprite_identity_survives_save_load);
    RUN(variable_sprite_runtime_no_crystal_mapping_call);
    RUN(money_balance_survives_save_load);
    RUN(money_transient_text_buffer_not_in_gamestate);
    RUN(money_prepare_text_reflects_current_balance);
    RUN(money_mom_account_prepare_text_distinct);

    std::cout << "\n=== Results ===\n";
    std::cout << "Passed: " << g_passed << "\n";
    std::cout << "Failed: " << g_failed << "\n";
    return g_failed > 0 ? 1 : 0;
}
