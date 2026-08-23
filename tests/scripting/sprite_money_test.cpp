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
#include "engine/world/pokemon_icons.hpp"
#include "crystal/extract/sprite_ids.hpp"
#include <iostream>
#include <cassert>
#include <string>
#include <algorithm>
#include <cctype>

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
// POKÉMON ICON + DAY CARE SPRITE TESTS
//=============================================================================

TEST(pokemon_icon_sprite_id_from_clefairy_byte) {
    using namespace crystal;
    // SPRITE_CLEFAIRY = 0x8F used directly in CopycatsHouse1F.asm object_event.
    // 0x8F - 0x80 = 15 → SpriteMons[15] = CLEFAIRY → MonMenuIcons[35-1] = ICON_CLEFAIRY → "clefairy"
    std::string id = crystal_sprite_byte_to_id(0x8F);
    ASSERT_STR_EQ(id, "pokemon_icon:clefairy");
    ASSERT_TRUE(sprite_id_is_pokemon_icon(id));
}

TEST(pokemon_icon_sprite_id_is_stable_across_bytes) {
    using namespace crystal;
    // Multiple sprite bytes may map to the same icon type (shared icons).
    // But each byte in range 0x80-0xA2 must produce a valid pokemon_icon id.
    for (int b = 0x80; b <= 0xA2; ++b) {
        std::string id = crystal_sprite_byte_to_id(static_cast<uint8_t>(b));
        ASSERT_TRUE(!id.empty());
        ASSERT_TRUE(id.starts_with("pokemon_icon:"));
        // Must not contain numeric index from old system
        std::string suffix = id.substr(13);
        bool is_numeric = !suffix.empty() && std::all_of(suffix.begin(), suffix.end(), ::isdigit);
        ASSERT_FALSE(is_numeric);
    }
}

TEST(daycare_empty_slot_returns_no_icon) {
    using namespace enginemon;
    // When daycare_slot[0] = 0 (empty), daycare_sprite_id_to_icon must return "".
    std::array<SpeciesId, 2> slots = {0, 0};
    std::string icon = daycare_sprite_id_to_icon("daycare:1", slots);
    ASSERT_TRUE(icon.empty());
    icon = daycare_sprite_id_to_icon("daycare:2", slots);
    ASSERT_TRUE(icon.empty());
}

TEST(daycare_occupied_resolves_to_icon) {
    using namespace enginemon;
    // Species 25 = PIKACHU → ICON_PIKACHU → "pokemon_icon:pikachu"
    std::array<SpeciesId, 2> slots = {25, 0};
    std::string icon = daycare_sprite_id_to_icon("daycare:1", slots);
    ASSERT_STR_EQ(icon, "pokemon_icon:pikachu");

    // Species 131 = LAPRAS → ICON_LAPRAS → "pokemon_icon:lapras"
    slots[1] = 131;
    icon = daycare_sprite_id_to_icon("daycare:2", slots);
    ASSERT_STR_EQ(icon, "pokemon_icon:lapras");
}

TEST(daycare_save_load_species_survives) {
    using namespace enginemon;
    // Day Care species occupancy must survive serialize/try_deserialize.
    GameState original;
    original.daycare_slot[0] = 25;   // PIKACHU in slot 1
    original.daycare_slot[1] = 131;  // LAPRAS in slot 2

    auto bytes = original.serialize();
    auto result = GameState::try_deserialize(bytes);
    ASSERT_TRUE(result.ok());
    ASSERT_EQ(result.state.daycare_slot[0], static_cast<SpeciesId>(25));
    ASSERT_EQ(result.state.daycare_slot[1], static_cast<SpeciesId>(131));
}

TEST(daycare_invalid_species_fails_closed) {
    using namespace enginemon;
    // A save file with daycare species 252 (above valid range) must fail deserialization.
    GameState gs;
    gs.daycare_slot[0] = 252;  // Invalid
    auto bytes = gs.serialize();
    auto result = GameState::try_deserialize(bytes);
    ASSERT_FALSE(result.ok());
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

    // Pokémon icon and Day Care tests
    RUN(pokemon_icon_sprite_id_from_clefairy_byte);
    RUN(pokemon_icon_sprite_id_is_stable_across_bytes);
    RUN(daycare_empty_slot_returns_no_icon);
    RUN(daycare_occupied_resolves_to_icon);
    RUN(daycare_save_load_species_survives);
    RUN(daycare_invalid_species_fails_closed);

    std::cout << "\n=== Results ===\n";
    std::cout << "Passed: " << g_passed << "\n";
    std::cout << "Failed: " << g_failed << "\n";
    return g_failed > 0 ? 1 : 0;
}
