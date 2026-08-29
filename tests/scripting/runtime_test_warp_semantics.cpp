// runtime_test_warp_semantics.cpp
// E2E tests for scripted warp semantics:
//   - coordinate warp uses explicit x/y (no physical warp-index required)
//   - warp_to_spawn reads GameState::warp_memory, errors explicitly when unset
//   - warp_to_spawn without warp_fn wired errors explicitly (no silent no-op)

#include "engine/scripting/lua_runtime.hpp"
#include "engine/core/game_state.hpp"
#include "engine/core/game_loop.hpp"
#include "engine/world/world_manager.hpp"
#include "engine/world/runtime_map.hpp"
#include "engine/world/collision_types.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

#define ASSERT_EQ(a, b)   do { auto _a=(a); auto _b=(b); if(_a!=_b){std::cerr<<"ASSERT_EQ("<<#a<<"="<<_a<<"!="<<#b<<"="<<_b<<") at "<<__FILE__<<":"<<__LINE__<<"\n";std::abort();} } while(0)
#define ASSERT_TRUE(x)    do { if(!(x)){std::cerr<<"ASSERT_TRUE("<<#x<<") at "<<__FILE__<<":"<<__LINE__<<"\n";std::abort();} } while(0)
#define ASSERT_FALSE(x)   do { if(x){std::cerr<<"ASSERT_FALSE("<<#x<<") at "<<__FILE__<<":"<<__LINE__<<"\n";std::abort();} } while(0)
#define ASSERT_THROWS(Ex, expr) do { bool _t=false; try{expr;}catch(const Ex&){_t=true;}catch(...){} if(!_t){std::cerr<<"ASSERT_THROWS failed: "<<#expr<<" at "<<__FILE__<<":"<<__LINE__<<"\n";std::abort();} } while(0)
#define TEST(name) void test_##name()

// Helper: build a minimal map with no warp events
static enginemon::RuntimeMap make_map(const std::string& id, int w=5, int h=5) {
    enginemon::RuntimeMap m;
    m.map_id = id; m.width = w; m.height = h;
    m.blocks.assign(static_cast<size_t>(w*h), 0u);
    return m;
}

// Helper: production-style warp_fn using explicit_coords=true
static std::function<bool(const std::string&, int32_t, int32_t)>
make_warp_fn(enginemon::WorldManager& wm, enginemon::GameState& gs) {
    return [&wm, &gs](const std::string& map_id, int32_t x, int32_t y) -> bool {
        enginemon::RuntimeWarp syn;
        syn.target_map_id    = map_id;
        syn.target_warp_index = 0;
        syn.x                = static_cast<uint8_t>(x);
        syn.y                = static_cast<uint8_t>(y);
        syn.explicit_coords  = true;
        auto r = wm.prepare_warp(syn, gs);
        if (!r.success) return false;
        wm.commit_warp(r, gs);
        return true;
    };
}

// =============================================================================
// TEST: Scripted coordinate warp uses explicit x/y, no warp-index needed
// =============================================================================
TEST(scripted_coordinate_warp_uses_explicit_coords) {
    using namespace enginemon;

    auto src  = make_map("src_map");
    auto dest = make_map("dest_map", 8, 8);
    // Neither map has warp events — a physical-warp-index resolver would fail

    GameState gs;
    gs.player.current_map_id = "src_map";
    gs.player.x = 2; gs.player.y = 2;

    WorldManager wm;
    wm.set_map_loader([&src, &dest](const std::string& id) -> std::optional<RuntimeMap> {
        if (id == "src_map")  return src;
        if (id == "dest_map") return dest;
        return std::nullopt;
    });
    wm.load_map("src_map");

    auto warp = make_warp_fn(wm, gs);

    // Warp to dest_map at (3,5) — must succeed despite no warp events
    ASSERT_TRUE(warp("dest_map", 3, 5));
    ASSERT_EQ(gs.player.x, 3);
    ASSERT_EQ(gs.player.y, 5);
    ASSERT_EQ(gs.player.current_map_id, std::string("dest_map"));
    const RuntimeMap* cur = wm.current_map();
    ASSERT_TRUE(cur && cur->map_id == "dest_map");
    std::cout << "  [coord warp: explicit_coords=true -> dest(3,5) OK, no warp events needed]\n";

    // Invalid destination leaves old state unchanged
    ASSERT_FALSE(warp("__bogus__", 0, 0));
    ASSERT_EQ(gs.player.current_map_id, std::string("dest_map"));
    ASSERT_EQ(gs.player.x, 3);
    ASSERT_EQ(gs.player.y, 5);
    std::cout << "  [coord warp: invalid dest rejected, old state unchanged]\n";
}

// =============================================================================
// TEST: warp_to_spawn reads GameState backup warp and calls warp_fn
// =============================================================================
TEST(warp_to_spawn_uses_gamestate_backup_warp) {
    using namespace enginemon;

    auto home = make_map("home_map");
    auto away = make_map("away_map");

    GameState gs;
    WorldManager wm;
    wm.set_map_loader([&home, &away](const std::string& id) -> std::optional<RuntimeMap> {
        if (id == "home_map") return home;
        if (id == "away_map") return away;
        return std::nullopt;
    });
    wm.load_map("away_map");
    gs.player.current_map_id = "away_map";
    gs.player.x = 3; gs.player.y = 3;
    gs.warp_memory.backup_map_id = "home_map";
    gs.warp_memory.backup_x = 1;
    gs.warp_memory.backup_y = 2;

    LuaRuntime rt;
    rt.set_game_state(&gs);
    rt.get_stub_services().warp_fn = make_warp_fn(wm, gs);

    const char* code = R"(
script = {}
function script.main(ctx) ctx.world:warp_to_spawn() return end
return script
)";
    rt.execute_string(code, "wts_test");
    rt.start_script("script");

    ASSERT_EQ(gs.player.current_map_id, std::string("home_map"));
    ASSERT_EQ(gs.player.x, 1);
    ASSERT_EQ(gs.player.y, 2);
    std::cout << "  [warp_to_spawn: backup_map(1,2) -> home_map OK]\n";

    // Empty backup must produce an error (via error handler, not C++ throw)
    gs.warp_memory.backup_map_id = "";
    bool got_error = false;
    rt.set_error_handler([&got_error](const std::string&, const std::string&) {
        got_error = true;
    });
    rt.execute_string(code, "wts_empty");
    rt.start_script("script");
    ASSERT_TRUE(got_error);  // must error because backup is empty
    std::cout << "  [warp_to_spawn: empty backup errors explicitly OK]\n";
}

// =============================================================================
// TEST: warp_to_spawn without warp_fn wired throws explicitly (no silent no-op)
// =============================================================================
TEST(warp_to_spawn_no_fn_errors_explicitly) {
    using namespace enginemon;

    GameState gs;
    gs.warp_memory.backup_map_id = "some_map";
    gs.warp_memory.backup_x = 0; gs.warp_memory.backup_y = 0;

    LuaRuntime rt;
    rt.set_game_state(&gs);
    // warp_fn intentionally NOT set

    const char* code = R"(
script = {}
function script.main(ctx) ctx.world:warp_to_spawn() return end
return script
)";
    rt.execute_string(code, "wts_no_fn");
    bool got_error = false;
    rt.set_error_handler([&got_error](const std::string&, const std::string&) {
        got_error = true;
    });
    rt.execute_string(code, "wts_no_fn2");
    rt.start_script("script");
    ASSERT_TRUE(got_error);  // must error — no silent no-op allowed
    std::cout << "  [warp_to_spawn: no warp_fn -> explicit error, no silent no-op]\n";
}
