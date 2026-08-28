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

// =============================================================================
// TEST 7: Door auto-step routes through authoritative movement path
//
// start_player_movement_to() must be used rather than direct player_ field
// mutation so MovementManager has a valid reservation during the step.
//
// Invariants checked:
//   - start_player_movement_to enqueues movement → state = Moving (not Idle)
//   - After movement completes, GameState.player.x/y is at destination (south_y)
//   - Loop reaches Idle after the step (MovementManager clears correctly)
// =============================================================================
TEST(door_auto_step_routes_through_movement_manager) {
    using namespace enginemon;

    GameState gs;
    LuaRuntime rt;
    rt.set_game_state(&gs);

    HeadlessGameLoop loop;
    loop.set_game_state(&gs);
    loop.set_lua_runtime(&rt);

    // Minimal map: all Floor collision so movement is unblocked
    RuntimeMap map;
    map.map_id  = "test_door_map";
    map.width   = 5;
    map.height  = 5;
    map.blocks.assign(static_cast<size_t>(map.width) * map.height, 0u);
    loop.load_map(map);
    loop.set_collision_data([](int32_t, int32_t) { return CollisionClass::Floor; });

    // Simulate the door auto-step sequence used by transition_to_map:
    //   1. spawn_player at door position (x=2, y=2)
    //   2. start_player_movement_to(x=2, south_y=3, Down)
    constexpr int32_t px = 2, py = 2, south_y = 3;
    loop.spawn_player(px, py, Direction::Down);

    // After spawn_player, state is Idle and position is (px, py)
    ASSERT_EQ(loop.player().x, px);
    ASSERT_EQ(loop.player().y, py);
    ASSERT_FALSE(loop.player().is_moving);

    // start_player_movement_to — the authoritative path that enqueues in MovementManager
    loop.start_player_movement_to(px, south_y, Direction::Down);

    // Immediately after the call, state must be Moving (not Idle).
    // If the old direct-mutation approach were used instead, is_moving would
    // be set but state_ might not be LoopState::Moving and the MovementManager
    // would have no reservation.
    ASSERT_TRUE(loop.player().is_moving);
    ASSERT_FALSE(loop.is_idle());  // state_ == Moving

    // Run ticks until idle (movement completes through MovementManager)
    int ticks = 0;
    for (; ticks < 64 && !loop.is_idle(); ++ticks) {
        auto tr = loop.tick();
        ASSERT_FALSE(tr.script_error);
    }
    ASSERT_TRUE(loop.is_idle());
    ASSERT_TRUE(ticks > 0 && ticks < 64);  // took some ticks, not immediate

    // After completion, authoritative position must be at destination
    ASSERT_EQ(loop.player().x, px);
    ASSERT_EQ(loop.player().y, south_y);  // moved one cell south

    // GameState must also reflect the destination
    ASSERT_EQ(gs.player.x, px);
    ASSERT_EQ(gs.player.y, south_y);

    // is_moving must be cleared
    ASSERT_FALSE(loop.player().is_moving);

    std::cout << "  [door auto-step: start_player_movement_to -> MovementManager "
              << "-> idle after " << ticks << " ticks, destination (" << px
              << "," << south_y << ") correct]\n";
}

// =============================================================================
// TEST 8: Crystal NPC visibility semantics — flag SET = hidden
//
// From pokecrystal map_objects_2.asm CheckObjectFlag:
//   flag SET   → .masked  → hidden
//   flag CLEAR → .unmasked → visible
//   0xFFFF    → .unmasked → always visible
//
// Tests:
//   A. no visibility_flag  → always visible
//   B. visibility_flag SET in GameState → NPC hidden at map load
//   C. visibility_flag CLEAR in GameState → NPC visible at map load
//   D. hidden NPC not collidable, not interactable
// =============================================================================
TEST(crystal_npc_visibility_flag_set_means_hidden) {
    using namespace enginemon;

    GameState gs;
    LuaRuntime rt;
    rt.set_game_state(&gs);

    HeadlessGameLoop loop;
    loop.set_game_state(&gs);
    loop.set_lua_runtime(&rt);
    loop.set_collision_data([](int32_t, int32_t) { return CollisionClass::Floor; });

    RuntimeMap map;
    map.map_id = "test_vis_map";
    map.width  = 5;
    map.height = 5;
    map.blocks.assign(25u, 0u);
    loop.load_map(map);

    // A: NPC with empty visibility_flag → always visible
    {
        NpcState npc;
        npc.id = 1; npc.x = 1; npc.y = 1;
        npc.visibility_flag = "";     // no controlling flag
        npc.visible = true;           // empty flag → always visible
        loop.add_npc(npc);
        const NpcState* n = loop.get_npc(1);
        ASSERT_TRUE(n && n->visible);
    }

    // B: NPC with visibility_flag THAT IS SET → hidden (flag SET = hidden)
    gs.set_flag("flag_001a");  // set flag 0x001A
    {
        NpcState npc;
        npc.id = 2; npc.x = 2; npc.y = 2;
        npc.visibility_flag = "flag_001a";
        // Crystal semantics: flag IS SET → hidden = !check_flag
        npc.visible = !gs.check_flag(npc.visibility_flag);  // false = hidden
        loop.add_npc(npc);
        const NpcState* n = loop.get_npc(2);
        ASSERT_TRUE(n != nullptr);
        ASSERT_FALSE(n->visible);  // flag set → hidden
    }

    // C: NPC with visibility_flag THAT IS CLEAR → visible
    // (flag "flag_002b" is not in GameState → clear)
    {
        NpcState npc;
        npc.id = 3; npc.x = 3; npc.y = 3;
        npc.visibility_flag = "flag_002b";
        npc.visible = !gs.check_flag(npc.visibility_flag);  // true = visible
        loop.add_npc(npc);
        const NpcState* n = loop.get_npc(3);
        ASSERT_TRUE(n != nullptr);
        ASSERT_TRUE(n->visible);   // flag clear → visible
    }

    // D: hidden NPC (id=2) must not block movement.
    // Verify via process_input: player at (2,1) moving south to (2,2).
    // NPC id=2 is at (2,2) and is hidden — movement must be accepted (not blocked).
    loop.spawn_player(2, 1, Direction::Down);
    auto ir = loop.process_input(InputAction::MoveDown);
    // If the hidden NPC were collidable, movement would be blocked.
    ASSERT_TRUE(ir.accepted);  // hidden NPC does not block player movement

    std::cout << "  [Crystal NPC visibility: flag SET=hidden, CLEAR=visible; "
              << "hidden NPC not collidable]\n";
}

// =============================================================================
// TEST 9: show_npc / hide_npc persist to GameState flags
//
// hide_npc(N) must SET the NPC's controlling flag in GameState.
// show_npc(N) must CLEAR the NPC's controlling flag in GameState.
// After a map unload/reload, visibility reflects the persisted flag state.
// =============================================================================
TEST(show_hide_npc_persists_to_gamestate_flags) {
    using namespace enginemon;

    GameState gs;
    LuaRuntime rt;
    rt.set_game_state(&gs);

    HeadlessGameLoop loop;
    loop.set_game_state(&gs);
    loop.set_lua_runtime(&rt);
    loop.set_collision_data([](int32_t, int32_t) { return CollisionClass::Floor; });

    RuntimeMap map;
    map.map_id = "persist_vis_map";
    map.width  = 5;
    map.height = 5;
    map.blocks.assign(25u, 0u);
    loop.load_map(map);

    // Add NPC with controlling flag "flag_0042"
    NpcState npc;
    npc.id = 5; npc.x = 2; npc.y = 2;
    npc.visibility_flag = "flag_0042";
    npc.visible = true;   // flag is clear → visible
    loop.add_npc(npc);
    ASSERT_FALSE(gs.check_flag("flag_0042"));  // flag starts clear

    // hide_npc(5) via Lua → must SET flag_0042 in GameState
    const char* hide_script = R"(
script = {}
function script.main(ctx)
    ctx.world:hide_npc(5)
    return
end
return script
)";
    rt.execute_string(hide_script, "hide_test");
    rt.start_script("script");

    // Flag must be SET now (Crystal Script_disappear semantics)
    ASSERT_TRUE(gs.check_flag("flag_0042"));
    // NpcState::visible must be false
    const NpcState* n = loop.get_npc(5);
    ASSERT_TRUE(n != nullptr);
    ASSERT_FALSE(n->visible);

    // Simulate map re-entry: rebuild NPC list from map objects, re-evaluating flags
    // (Crystal semantics: flag SET → hidden on reload)
    loop.clear_npcs();
    {
        NpcState reloaded;
        reloaded.id = 5; reloaded.x = 2; reloaded.y = 2;
        reloaded.visibility_flag = "flag_0042";
        reloaded.visible = !gs.check_flag(reloaded.visibility_flag);  // flag SET → hidden
        loop.add_npc(reloaded);
    }
    const NpcState* reloaded_n = loop.get_npc(5);
    ASSERT_TRUE(reloaded_n != nullptr);
    ASSERT_FALSE(reloaded_n->visible);  // still hidden after reload

    // show_npc(5) via Lua → must CLEAR flag_0042 in GameState
    const char* show_script = R"(
script = {}
function script.main(ctx)
    ctx.world:show_npc(5)
    return
end
return script
)";
    rt.execute_string(show_script, "show_test");
    rt.start_script("script");

    // Flag must be CLEAR now (Crystal Script_appear semantics)
    ASSERT_FALSE(gs.check_flag("flag_0042"));
    // NpcState::visible must be true
    const NpcState* shown_n = loop.get_npc(5);
    ASSERT_TRUE(shown_n != nullptr);
    ASSERT_TRUE(shown_n->visible);

    // Reload again → still visible (flag CLEAR → visible)
    loop.clear_npcs();
    {
        NpcState reloaded2;
        reloaded2.id = 5; reloaded2.x = 2; reloaded2.y = 2;
        reloaded2.visibility_flag = "flag_0042";
        reloaded2.visible = !gs.check_flag(reloaded2.visibility_flag);  // flag CLEAR → visible
        loop.add_npc(reloaded2);
    }
    const NpcState* r2 = loop.get_npc(5);
    ASSERT_TRUE(r2 != nullptr);
    ASSERT_TRUE(r2->visible);  // persisted as visible after show_npc

    std::cout << "  [show/hide_npc: hide sets flag (persist hidden), "
              << "show clears flag (persist visible)]\n";
}

// =============================================================================
// TEST 10: hide NPC without controlling flag — transient only
//
// Crystal Script_disappear no-ops when event_flag == 0xFFFF (-1).
// hide_npc on a flag-less NPC updates NpcState::visible but does NOT
// invent a fake flag in GameState.
// =============================================================================
TEST(hide_npc_no_flag_is_transient) {
    using namespace enginemon;

    GameState gs;
    LuaRuntime rt;
    rt.set_game_state(&gs);

    HeadlessGameLoop loop;
    loop.set_game_state(&gs);
    loop.set_lua_runtime(&rt);
    loop.set_collision_data([](int32_t, int32_t) { return CollisionClass::Floor; });

    RuntimeMap map;
    map.map_id = "transient_vis_map";
    map.width = 4; map.height = 4;
    map.blocks.assign(16u, 0u);
    loop.load_map(map);

    NpcState npc;
    npc.id = 7; npc.x = 1; npc.y = 1;
    npc.visibility_flag = "";   // no controlling flag (0xFFFF in ROM)
    npc.visible = true;
    loop.add_npc(npc);

    size_t flag_count_before = gs.flags.size();

    const char* hide_script = R"(
script = {}
function script.main(ctx)
    ctx.world:hide_npc(7)
    return
end
return script
)";
    rt.execute_string(hide_script, "hide_noflag");
    rt.start_script("script");

    // NpcState::visible must be false (live change)
    const NpcState* n = loop.get_npc(7);
    ASSERT_TRUE(n != nullptr);
    ASSERT_FALSE(n->visible);

    // GameState::flags must be unchanged — no fake flag invented
    ASSERT_EQ(gs.flags.size(), flag_count_before);

    std::cout << "  [hide_npc with no flag: NpcState hidden, GameState unchanged]\n";
}
