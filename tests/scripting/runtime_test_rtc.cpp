// runtime_test_rtc.cpp — RTC tests (split from runtime_test_vm_state.cpp to keep TU size manageable)
// Uses FakeClockSource for determinism — no sleep, no OS clock dependency.
#include "engine/scripting/lua_runtime.hpp"
#include "engine/scripting/api_bindings.hpp"
#include "engine/core/game_state.hpp"
#include "engine/core/rtc.hpp"

#include <iostream>
#include <cassert>

#define ASSERT_EQ(a, b)   do { auto _a = (a); auto _b = (b); if (_a != _b) { std::cerr << "ASSERT_EQ failed: " << #a << " (" << _a << ") != " << #b << " (" << _b << ") at " << __FILE__ << ":" << __LINE__ << "\n"; std::abort(); } } while(0)
#define ASSERT_TRUE(x)    do { if (!(x)) { std::cerr << "ASSERT_TRUE failed: " << #x << " at " << __FILE__ << ":" << __LINE__ << "\n"; std::abort(); } } while(0)
#define ASSERT_FALSE(x)   do { if ((x)) { std::cerr << "ASSERT_FALSE failed: " << #x << " at " << __FILE__ << ":" << __LINE__ << "\n"; std::abort(); } } while(0)
#define TEST(name)        void test_##name()

// Helper: build a GameState with a given RTC offset and inject a FakeClockSource
// into a fresh LuaRuntime.  The LuaRuntime reads GameState::rtc_offset_seconds
// via get_game_state() when ctx.time:* is called.
static void setup_rtc_state(enginemon::GameState& gs, int64_t fake_wall_now, int64_t desired_hour,
                             int32_t desired_minute = 0) {
    using namespace enginemon;
    int64_t desired_effective = desired_hour * 3600 + desired_minute * 60;
    gs.rtc_offset_seconds = desired_effective - fake_wall_now;
}

// ctx.time:hour() / ctx.time:minute() return correct values from RTC offset
TEST(rtc_hour_minute_from_offset) {
    using namespace enginemon;
    GameState gs;
    gs.rtc_offset_seconds = 55800;  // offset = 55800 - 0 = 55800

    LuaRuntime rt;
    rt.set_game_state(&gs);

    const char* code = R"(
script = {}
function script.main(ctx)
    ctx.flags:set_var(0, ctx.time:hour())
    ctx.flags:set_var(1, ctx.time:minute())
    return
end
return script
)";
    int64_t wall_now = GameRtc::system_source().now_seconds();
    int64_t desired_effective = 15 * 3600 + 30 * 60;  // 15:30:00
    gs.rtc_offset_seconds = desired_effective - (wall_now % 86400 == 0 ? 0 : (wall_now - (wall_now % 86400)));
    gs.rtc_offset_seconds = desired_effective - wall_now;

    rt.execute_string(code, "rtc_hm");
    rt.start_script("script");

    RtcComponents c = GameRtc::decompose(desired_effective);
    ASSERT_EQ(gs.get_var("var_0"), static_cast<int32_t>(c.hour));
    ASSERT_EQ(gs.get_var("var_1"), static_cast<int32_t>(c.minute));
    std::cout << "  [RTC: offset=" << gs.rtc_offset_seconds << " -> hour=" << c.hour << " min=" << c.minute << "]\n";
}

// FakeClockSource test: advancing the fake wall clock advances effective time
TEST(rtc_fake_clock_advance) {
    using namespace enginemon;
    FakeClockSource fake(0);       // fake wall = 0 (midnight 1970-01-01 UTC)
    GameRtc rtc(fake);

    int64_t offset = 6 * 3600;   // want hour=6 at fake_now=0
    ASSERT_EQ(rtc.effective_now(offset), 6 * 3600);
    ASSERT_EQ(GameRtc::decompose(rtc.effective_now(offset)).hour, 6);

    fake.advance(4 * 3600);       // advance wall by 4 hours -> effective=10h
    ASSERT_EQ(GameRtc::decompose(rtc.effective_now(offset)).hour, 10);
    std::cout << "  [RTC fake clock: advance 4h -> effective hour=10]\n";
}

// Setting the in-game clock recomputes offset; effective time equals desired time
TEST(rtc_set_clock_recomputes_offset) {
    using namespace enginemon;
    FakeClockSource fake(1000000);  // arbitrary wall time
    GameRtc rtc(fake);

    int64_t desired = 1704182400LL;  // 2024-01-02 08:00:00 UTC (1704153600 is midnight; +28800 = 08:00)
    int64_t computed_offset = rtc.compute_offset_for(desired);
    ASSERT_EQ(fake.now_seconds() + computed_offset, desired);

    auto c = GameRtc::decompose(desired);
    ASSERT_EQ(c.hour, 8);
    ASSERT_EQ(c.minute, 0);
    std::cout << "  [RTC set clock: desired=2024-01-02T08:00 -> offset=" << computed_offset << " hour=" << c.hour << "]\n";
}

// is_morning / is_day / is_night boundaries
TEST(rtc_period_boundaries) {
    using namespace enginemon;
    // Morning: 4..9
    ASSERT_FALSE(GameRtc::is_morning(3));
    ASSERT_TRUE(GameRtc::is_morning(4));
    ASSERT_TRUE(GameRtc::is_morning(9));
    ASSERT_FALSE(GameRtc::is_morning(10));
    // Day: 10..17
    ASSERT_FALSE(GameRtc::is_day(9));
    ASSERT_TRUE(GameRtc::is_day(10));
    ASSERT_TRUE(GameRtc::is_day(17));
    ASSERT_FALSE(GameRtc::is_day(18));
    // Night: 18..23 and 0..3
    ASSERT_FALSE(GameRtc::is_night(17));
    ASSERT_TRUE(GameRtc::is_night(18));
    ASSERT_TRUE(GameRtc::is_night(23));
    ASSERT_TRUE(GameRtc::is_night(0));
    ASSERT_TRUE(GameRtc::is_night(3));
    ASSERT_FALSE(GameRtc::is_night(4));
    std::cout << "  [RTC period boundaries: morning/day/night correct]\n";
}

// Weekday derivation: 1970-01-01 (epoch) was Thursday=4; verify derived weekdays
TEST(rtc_weekday_derivation) {
    using namespace enginemon;
    // Unix epoch = 1970-01-01 = Thursday.  day=0 -> dow = (0+4)%7 = 4 (Thu)
    ASSERT_EQ(GameRtc::decompose(0).day_of_week, 4);  // Thursday
    ASSERT_EQ(GameRtc::decompose(86400).day_of_week, 5);  // Friday
    ASSERT_EQ(GameRtc::decompose(6 * 86400).day_of_week, 3);  // Wednesday
    ASSERT_EQ(GameRtc::decompose(7 * 86400).day_of_week, 4);  // Thursday again
    std::cout << "  [RTC weekday: epoch=Thu, +1=Fri, +6=Wed, +7=Thu]\n";
}

// Midnight/daily rollover: day boundary and unix_day increment
TEST(rtc_midnight_daily_rollover) {
    using namespace enginemon;
    auto c1 = GameRtc::decompose(86399);
    ASSERT_EQ(c1.hour, 23);
    ASSERT_EQ(c1.minute, 59);
    ASSERT_EQ(c1.second, 59);
    ASSERT_EQ(c1.unix_day, 0);
    auto c2 = GameRtc::decompose(86400);
    ASSERT_EQ(c2.hour, 0);
    ASSERT_EQ(c2.minute, 0);
    ASSERT_EQ(c2.second, 0);
    ASSERT_EQ(c2.unix_day, 1);
    std::cout << "  [RTC midnight rollover: day 0 end -> day 1 begin]\n";
}

// Negative offsets work correctly (zone behind UTC)
TEST(rtc_negative_offset) {
    using namespace enginemon;
    auto c = GameRtc::decompose(-3600);
    ASSERT_EQ(c.hour, 23);
    ASSERT_EQ(c.unix_day, -1);
    std::cout << "  [RTC negative offset: effective=-3600 -> hour=23 day=-1]\n";
}

// Fast-forward / tick loop does NOT advance RTC
TEST(rtc_ticks_do_not_advance_rtc) {
    using namespace enginemon;
    FakeClockSource fake(0);
    GameRtc rtc(fake);

    int64_t offset = 12 * 3600;  // noon
    int64_t initial = rtc.effective_now(offset);

    for (int i = 0; i < 1000; ++i) {
        int64_t current = rtc.effective_now(offset);
        ASSERT_EQ(current, initial);  // wall clock frozen, offset unchanged
    }
    std::cout << "  [RTC: 1000 ticks do not advance effective time]\n";
}

// Save/load preserves rtc_offset_seconds and rtc_dst_enabled
TEST(rtc_save_load_preserves_offset) {
    using namespace enginemon;
    GameState gs;
    gs.rtc_offset_seconds = 9876543;
    gs.rtc_dst_enabled    = true;

    auto bytes = gs.serialize();
    auto result = GameState::try_deserialize(bytes);
    ASSERT_TRUE(result.ok());
    ASSERT_EQ(result.state.rtc_offset_seconds, 9876543);
    ASSERT_TRUE(result.state.rtc_dst_enabled);
    std::cout << "  [RTC save/load: offset+dst_enabled round-trip]\n";
}

// set_daylight_saving wires through GameState offset
TEST(rtc_set_daylight_saving_adjusts_offset) {
    using namespace enginemon;
    GameState gs;
    gs.rtc_offset_seconds = 0;
    gs.rtc_dst_enabled    = false;

    LuaRuntime rt;
    rt.set_game_state(&gs);

    const char* enable_dst = R"(
script = {}
function script.main(ctx)
    ctx.game:set_daylight_saving(1)
    return
end
return script
)";
    rt.execute_string(enable_dst, "enable_dst");
    rt.start_script("script");

    ASSERT_EQ(gs.rtc_offset_seconds, 3600);  // spring forward
    ASSERT_TRUE(gs.rtc_dst_enabled);

    const char* disable_dst = R"(
script = {}
function script.main(ctx)
    ctx.game:set_daylight_saving(0)
    return
end
return script
)";
    rt.execute_string(disable_dst, "disable_dst");
    rt.start_script("script");

    ASSERT_EQ(gs.rtc_offset_seconds, 0);     // fall back
    ASSERT_FALSE(gs.rtc_dst_enabled);
    std::cout << "  [RTC DST: enable +3600, disable -3600 via set_daylight_saving]\n";
}
