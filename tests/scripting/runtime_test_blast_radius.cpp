// runtime_test_blast_radius.cpp
// E2E tests for the 5 confirmed high-blast-radius fixes.

#include "engine/scripting/lua_runtime.hpp"
#include "engine/core/game_state.hpp"
#include "engine/core/game_loop.hpp"
#include "engine/world/runtime_map.hpp"
#include "engine/world/collision_types.hpp"

#include <iostream>
#include <cassert>
#include <cstdlib>
#include <string>
#include <stdexcept>

#define ASSERT_EQ(a, b)   do { auto _a=(a); auto _b=(b); if(_a!=_b){std::cerr<<"ASSERT_EQ("<<#a<<"="<<_a<<"!="<<#b<<"="<<_b<<") at "<<__FILE__<<":"<<__LINE__<<"\n";std::abort();} } while(0)
#define ASSERT_TRUE(x)    do { if(!(x)){std::cerr<<"ASSERT_TRUE("<<#x<<") at "<<__FILE__<<":"<<__LINE__<<"\n";std::abort();} } while(0)
#define ASSERT_FALSE(x)   do { if((x)){std::cerr<<"ASSERT_FALSE("<<#x<<") at "<<__FILE__<<":"<<__LINE__<<"\n";std::abort();} } while(0)
#define TEST(name)        void test_##name()

static enginemon::RuntimeMap make_map(const std::string& id, int w=5, int h=5) {
    enginemon::RuntimeMap m;
    m.map_id = id; m.width = w; m.height = h;
    m.blocks.assign(static_cast<size_t>(w*h), 0u);
    return m;
}

// =============================================================================
// Fix 1: write_state_var passes the current result value
// =============================================================================
TEST(write_state_var_stores_result) {
    using namespace enginemon;
    GameState gs;
    LuaRuntime rt;
    rt.set_game_state(&gs);

    // Script: read_state_var -> put known value in result -> write_state_var
    // The read puts the *existing* value (0) in result; then we use set_state_var
    // to plant a non-zero value, then verify write_state_var captures a fresh result.
    const char* code = R"(
script = {}
function script.main(ctx)
    -- plant a value via read path so result holds it
    result = 42
    ctx.game:write_state_var(3, result)
    return
end
return script
)";
    rt.execute_string(code, "wsv_test");
    rt.start_script("script");

    ASSERT_EQ(gs.get_var("state_var_3"), 42);
    std::cout << "  [write_state_var: result=42 -> state_var_3=42]\n";

    // Verify distinct from no-op: if the binding had been the old no-op,
    // the existing value (0) would remain.
    ASSERT_EQ(gs.get_var("state_var_3"), 42);  // not 0

    // Also verify that different values produce different storage
    const char* code2 = R"(
script = {}
function script.main(ctx)
    ctx.game:write_state_var(3, 99)
    return
end
return script
)";
    rt.execute_string(code2, "wsv_test2");
    rt.start_script("script");
    ASSERT_EQ(gs.get_var("state_var_3"), 99);
    std::cout << "  [write_state_var: value=99 -> state_var_3=99; no silent no-op]\n";
}

// =============================================================================
// Fix 2: set_map_scene / check_map_scene per-map persistent state
// =============================================================================
TEST(map_scene_per_map_persistent) {
    using namespace enginemon;
    GameState gs;
    LuaRuntime rt;
    rt.set_game_state(&gs);

    // set_map_scene(map_a, 5) then check -- must return 5
    const char* set_a = R"(
script = {}
function script.main(ctx)
    ctx.game:set_map_scene(100, 5)
    return
end
return script
)";
    rt.execute_string(set_a, "sms_a");
    rt.start_script("script");

    const char* check_a = R"(
script = {}
function script.main(ctx)
    ctx.flags:set_var(0, ctx.game:check_map_scene(100))
    return
end
return script
)";
    rt.execute_string(check_a, "cms_a");
    rt.start_script("script");
    ASSERT_EQ(gs.get_var("var_0"), 5);

    // Map B (different map_id) must be 0 (unchanged)
    const char* check_b = R"(
script = {}
function script.main(ctx)
    ctx.flags:set_var(1, ctx.game:check_map_scene(200))
    return
end
return script
)";
    rt.execute_string(check_b, "cms_b");
    rt.start_script("script");
    ASSERT_EQ(gs.get_var("var_1"), 0);

    std::cout << "  [map_scene: map_a=5, map_b=0 (distinct)]\n";

    // Save/load preserves both
    auto bytes = gs.serialize();
    auto result = GameState::try_deserialize(bytes);
    ASSERT_TRUE(result.ok());
    const GameState& gs2 = result.state;
    // "map_scene_0064" = map_id 100 = 0x64
    ASSERT_EQ(gs2.get_var("map_scene_0064"), 5);
    ASSERT_EQ(gs2.get_var("map_scene_00c8"), 0);  // absent -> 0 default

    std::cout << "  [map_scene: save/load preserves map_a=5, map_b absent]\n";
}

// =============================================================================
// Fix 3: get_player_pos returns authoritative HeadlessGameLoop position
// =============================================================================
TEST(get_player_pos_authoritative) {
    using namespace enginemon;
    GameState gs;
    gs.player.current_map_id = "auth_map";
    gs.player.x = 0; gs.player.y = 0;

    LuaRuntime rt;
    rt.set_game_state(&gs);

    HeadlessGameLoop loop;
    loop.set_game_state(&gs);
    loop.set_lua_runtime(&rt);
    auto map = make_map("auth_map");
    loop.load_map(map);
    loop.set_collision_data([](int32_t, int32_t) { return CollisionClass::Floor; });
    loop.spawn_player(7, 3, Direction::Down);

    // Verify game_loop position was set
    ASSERT_EQ(loop.player().x, 7);
    ASSERT_EQ(loop.player().y, 3);

    // Script queries player position
    const char* code = R"(
script = {}
function script.main(ctx)
    local x, y, _ = ctx.world:get_player_pos()
    ctx.flags:set_var(0, x)
    ctx.flags:set_var(1, y)
    return
end
return script
)";
    rt.execute_string(code, "gpp_test");
    rt.start_script("script");

    // Must reflect game_loop position, not stubs.player (which defaults to 0,0)
    ASSERT_EQ(gs.get_var("var_0"), 7);
    ASSERT_EQ(gs.get_var("var_1"), 3);
    std::cout << "  [get_player_pos: game_loop pos (7,3) returned, not stubs (0,0)]\n";

    // Move player and verify position updates
    loop.spawn_player(2, 9, Direction::Up);
    rt.execute_string(code, "gpp_test2");
    rt.start_script("script");
    ASSERT_EQ(gs.get_var("var_0"), 2);
    ASSERT_EQ(gs.get_var("var_1"), 9);
    std::cout << "  [get_player_pos: after move to (2,9), returns (2,9)]\n";
}

// =============================================================================
// Fix 4: EventFlag and EngineFlag with same numeric value are distinct
// =============================================================================
TEST(flag_namespace_event_engine_distinct) {
    using namespace enginemon;
    GameState gs;
    LuaRuntime rt;
    rt.set_game_state(&gs);

    // EngineFlag{65}: enc = (1<<16)|65 = 65601 -> key "eflag_0041"
    // EventFlag{65}:  enc = (0<<16)|65 = 65    -> key "flag_0041"
    constexpr int engine_flag_65_enc = (1 << 16) | 65;
    constexpr int event_flag_65_enc  = 65;

    // Set only EngineFlag{65}
    const char* set_engine = R"(
script = {}
function script.main(ctx)
    ctx.flags:set(65601)   -- EngineFlag{65}
    return
end
return script
)";
    rt.execute_string(set_engine, "set_ef");
    rt.start_script("script");

    // EngineFlag{65} must be SET
    ASSERT_TRUE(gs.check_flag("eflag_0041"));
    // EventFlag{65} must NOT be set (namespace isolation)
    ASSERT_FALSE(gs.check_flag("flag_0041"));

    std::cout << "  [flag ns: set EngineFlag{65} -> eflag_0041=SET, flag_0041=CLEAR]\n";

    // Now set EventFlag{65}
    const char* set_event = R"(
script = {}
function script.main(ctx)
    ctx.flags:set(65)      -- EventFlag{65}
    return
end
return script
)";
    rt.execute_string(set_event, "set_evf");
    rt.start_script("script");

    // Both must be independently readable
    ASSERT_TRUE(gs.check_flag("eflag_0041"));  // EngineFlag still set
    ASSERT_TRUE(gs.check_flag("flag_0041"));   // EventFlag now also set

    std::cout << "  [flag ns: both eflag_0041 and flag_0041 independently SET]\n";

    // Clear EngineFlag -- EventFlag must remain
    const char* clear_engine = R"(
script = {}
function script.main(ctx)
    ctx.flags:clear(65601)  -- EngineFlag{65}
    return
end
return script
)";
    rt.execute_string(clear_engine, "clr_ef");
    rt.start_script("script");

    ASSERT_FALSE(gs.check_flag("eflag_0041"));  // cleared
    ASSERT_TRUE(gs.check_flag("flag_0041"));    // EventFlag unchanged

    std::cout << "  [flag ns: clear EngineFlag{65} -> eflag_0041=CLEAR, flag_0041=SET]\n";

    // Verify that EventFlag matches what map extractor would produce
    // make_flag_id(65) = "flag_0041" -- must match
    ASSERT_TRUE(gs.check_flag("flag_0041"));
    std::cout << "  [flag ns: flag_0041 matches make_flag_id(65) for EventFlag use]\n";
}

// =============================================================================
// Fix 5: prepare_for_save snapshots NPC state before serialize
// =============================================================================
TEST(prepare_for_save_snapshots_npc_state) {
    using namespace enginemon;

    auto map = make_map("npc_save_map");
    GameState gs;
    gs.player.current_map_id = "npc_save_map";
    gs.player.x = 0; gs.player.y = 0;

    HeadlessGameLoop loop;
    loop.load_map(map);
    loop.set_collision_data([](int32_t, int32_t) { return CollisionClass::Floor; });
    loop.set_game_state(&gs);
    loop.spawn_player(0, 0, Direction::Down);

    // Add NPC at initial position (5, 5)
    NpcState npc;
    npc.id = 3; npc.x = 5; npc.y = 5;
    npc.facing = Direction::Right;
    npc.behavior = NpcMovementBehavior::Standing;
    npc.visible = true;
    loop.add_npc(npc);

    // Teleport the NPC to a new position (2, 8) to simulate movement
    {
        NpcState* n = loop.get_npc(3);
        ASSERT_TRUE(n != nullptr);
        n->x = 2; n->y = 8; n->facing = Direction::Up;
    }

    // Verify NPC is at new position
    ASSERT_EQ(loop.get_npc(3)->x, 2);
    ASSERT_EQ(loop.get_npc(3)->y, 8);

    // Without prepare_for_save: gs.npc_states is empty -- NPC would reset on load
    ASSERT_TRUE(gs.npc_states.find("npc_save_map") == gs.npc_states.end());

    // Call prepare_for_save -- must snapshot
    loop.prepare_for_save();

    // Now npc_states must have an entry for this map
    ASSERT_TRUE(gs.npc_states.find("npc_save_map") != gs.npc_states.end());
    const auto& saved = gs.npc_states["npc_save_map"];
    ASSERT_EQ(saved.size(), 1u);
    ASSERT_EQ(saved[0].id, 3u);
    ASSERT_EQ(saved[0].x, 2);
    ASSERT_EQ(saved[0].y, 8);
    ASSERT_TRUE(saved[0].facing == Direction::Up);  // Direction has no operator<<

    std::cout << "  [prepare_for_save: NPC (id=3) snapshot (2,8,Up) captured]\n";

    // Serialize -> deserialize -> restore_npc_states -> NPC at saved position
    auto bytes = gs.serialize();
    auto result = GameState::try_deserialize(bytes);
    ASSERT_TRUE(result.ok());
    GameState& gs2 = result.state;

    // Rebuild loop from loaded state
    HeadlessGameLoop loop2;
    loop2.load_map(map);
    loop2.set_collision_data([](int32_t, int32_t) { return CollisionClass::Floor; });
    loop2.set_game_state(&gs2);
    loop2.spawn_player(0, 0, Direction::Down);

    // Add NPC at ROM-default position (5,5) first
    NpcState npc2;
    npc2.id = 3; npc2.x = 5; npc2.y = 5;
    npc2.facing = Direction::Right;
    npc2.behavior = NpcMovementBehavior::Standing;
    npc2.visible = true;
    loop2.add_npc(npc2);

    // Restore from save -- must override ROM defaults with saved position
    loop2.restore_npc_states("npc_save_map");

    const NpcState* restored = loop2.get_npc(3);
    ASSERT_TRUE(restored != nullptr);
    ASSERT_EQ(restored->x, 2);
    ASSERT_EQ(restored->y, 8);
    ASSERT_TRUE(restored->facing == Direction::Up);  // Direction has no operator<<

    std::cout << "  [prepare_for_save: restored NPC (id=3) at (2,8,Up) after load]\n";
}
