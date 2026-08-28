#pragma once
// engine/core/rtc.hpp
// Real-Time Clock abstraction for Enginemon.
//
// Architecture:
//   System Clock (system_clock::now())
//       +
//   Persistent RTC Offset (rtc_offset_seconds stored in GameState)
//       =
//   Effective Game Time
//
// Effective time = system_now + rtc_offset_seconds
//
// Setting the in-game clock recomputes rtc_offset so that
//   effective_time = desired_time
//   => rtc_offset = desired_time - system_now
//
// Frame rate, pause, fast-forward, and tick rate do NOT advance the RTC.
// The RTC is driven by the host's system_clock, not by simulation ticks.
//
// DST: handled by adjusting rtc_offset_seconds by ±3600.  The engine does
// not maintain separate DST state; the offset absorbs it.
//
// Timezone: no conversion.  system_clock provides UTC.  All hour/minute
// arithmetic is done on UTC hours modulo 24.  Game-calendar semantics that
// require local civil time are the frontend's / game-definition's concern.
// The runtime owns clock primitives, not locale-specific scheduling.
//
// Testability: ClockSource is injectable.
//   Production:  SystemClockSource (calls std::chrono::system_clock::now)
//   Tests:       FakeClockSource   (returns a manually advanced instant)

#include <chrono>
#include <cstdint>
#include <functional>

namespace enginemon {

// ─────────────────────────────────────────────────────────────────────────────
// Clock Source abstraction — injectable for deterministic tests.
// ─────────────────────────────────────────────────────────────────────────────

using UnixSeconds = int64_t;  // seconds since Unix epoch

// Returns current wall-clock time as seconds since Unix epoch.
// Production: system_clock::now().  Tests: FakeClockSource.
class ClockSource {
public:
    virtual ~ClockSource() = default;
    virtual UnixSeconds now_seconds() const = 0;
};

class SystemClockSource final : public ClockSource {
public:
    UnixSeconds now_seconds() const override {
        using namespace std::chrono;
        return duration_cast<std::chrono::seconds>(
            system_clock::now().time_since_epoch()).count();
    }
};

// Fake clock for tests — never sleeps, never touches the OS clock.
// Call advance() to move the fake wall clock forward.
class FakeClockSource final : public ClockSource {
public:
    explicit FakeClockSource(UnixSeconds initial = 0) : now_(initial) {}

    UnixSeconds now_seconds() const override { return now_; }

    // Advance the fake clock by delta seconds (may be negative for rewind).
    void advance(int64_t delta_seconds) { now_ += delta_seconds; }

    // Set to a specific instant.
    void set(UnixSeconds t) { now_ = t; }

private:
    UnixSeconds now_ = 0;
};

// ─────────────────────────────────────────────────────────────────────────────
// GameRtc — derives semantic time values from the clock source + offset.
//
// Owns a const pointer to a ClockSource (caller manages lifetime).
// Typically the global SystemClockSource is used; tests pass a FakeClockSource.
// ─────────────────────────────────────────────────────────────────────────────

struct RtcComponents {
    int32_t hour;       // 0–23 (UTC + offset)
    int32_t minute;     // 0–59
    int32_t second;     // 0–59
    int32_t day_of_week; // 0=Sunday … 6=Saturday  (ISO/Crystal convention)
    int64_t unix_day;   // floor(effective_unix / 86400) — stable daily identity
};

class GameRtc {
public:
    // hour boundaries for Morning/Day/Night (UTC after offset).
    // Source: engine/core/types.hpp TimeOfDay comment + pokecrystal checktime.asm
    static constexpr int32_t MORNING_START_H = 4;   //  4:00
    static constexpr int32_t DAY_START_H     = 10;  // 10:00
    static constexpr int32_t NIGHT_START_H   = 18;  // 18:00

    // Construct with an injected clock source.
    // clock must outlive GameRtc.  Production code passes &SystemClockSource::instance().
    explicit GameRtc(const ClockSource& clock) : clock_(clock) {}

    // Compute current effective time = now + offset.
    UnixSeconds effective_now(int64_t rtc_offset_seconds) const {
        return clock_.now_seconds() + rtc_offset_seconds;
    }

    // Decompose an effective instant into semantic components.
    static RtcComponents decompose(UnixSeconds effective) {
        // We work with plain integer arithmetic to avoid locale, DST, or
        // platform-specific time_t behaviour.  All values are in UTC.
        int64_t t = effective;

        // Unix day (floors toward -∞ even for negative t)
        int64_t day = (t >= 0) ? (t / 86400) : ((t - 86399) / 86400);

        // Time within the day [0, 86400)
        int64_t tod = t - day * 86400;
        if (tod < 0) tod += 86400;

        int32_t h = static_cast<int32_t>(tod / 3600);
        int32_t m = static_cast<int32_t>((tod % 3600) / 60);
        int32_t s = static_cast<int32_t>(tod % 60);

        // Day of week: Unix epoch (1970-01-01) was a Thursday = 4.
        // weekday = (day + 4) mod 7, where 0=Sunday.
        int64_t raw_dow = (day % 7 + 4 + 7) % 7;  // handle negative day
        int32_t dow = static_cast<int32_t>(raw_dow);

        return RtcComponents{h, m, s, dow, day};
    }

    // ── Period predicates ────────────────────────────────────────────────────

    static bool is_morning(int32_t hour) noexcept {
        return hour >= MORNING_START_H && hour < DAY_START_H;
    }

    static bool is_day(int32_t hour) noexcept {
        return hour >= DAY_START_H && hour < NIGHT_START_H;
    }

    static bool is_night(int32_t hour) noexcept {
        return !is_morning(hour) && !is_day(hour);
    }

    static const char* period_name(int32_t hour) noexcept {
        if (is_morning(hour)) return "morning";
        if (is_day(hour))     return "day";
        return "night";
    }

    // ── Clock setting ────────────────────────────────────────────────────────
    // Compute the offset so that effective_now() == desired_effective_seconds.
    int64_t compute_offset_for(UnixSeconds desired_effective_seconds) const {
        return desired_effective_seconds - clock_.now_seconds();
    }

    // Global production singleton accessor.
    // Tests should not use this; they pass FakeClockSource directly.
    static SystemClockSource& system_source() {
        static SystemClockSource instance;
        return instance;
    }

private:
    const ClockSource& clock_;
};

} // namespace enginemon
