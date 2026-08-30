#pragma once
// frontends/crystal/save/crystal_save_snapshot.hpp
//
// CrystalSaveSnapshot — semantic save state decoded from a Crystal SRAM image.
//
// Phase 1 (committed 2026-08):
//   money_player, money_mom, coins, event_flags
//
// Phase 2 (this version):
//   player_id, secret_id, player_name, moms_name, rival_name
//   player_gender
//   map_group, map_number, map_y, map_x (Crystal map coords)
//   scene_ids[79] (per-map scene state)
//   playtime_hours, playtime_minutes, playtime_seconds, playtime_capped
//   rtc_day, rtc_hour, rtc_min, rtc_sec (hardware RTC state)
//   dst
//
// This type is SEMANTIC at the Crystal boundary: no raw SRAM bytes, no packed
// structs, no ROM offsets.  Crystal charmap strings are decoded to UTF-8.
// Crystal map (group, number) pairs are stored as-is — conversion to/from
// semantic string map IDs happens at the GameState integration layer.
//
// Not yet in this snapshot: party, boxes, Pokédex, items, daycare, phone,
// mail, TM/HM inventory.

#include "crystal_sram_layout.hpp"
#include <array>
#include <cstdint>
#include <string>

namespace crystal {

/// Crystal map identifier: (group, number) pair.
/// Encodes directly as Crystal's wMapGroup / wMapNumber bytes.
/// The semantic string map ID (e.g. "new_bark_town") is NOT stored here —
/// that conversion requires the compiled map registry and is done outside the codec.
struct CrystalMapCoord {
    uint8_t group  = 0;
    uint8_t number = 0;
    uint8_t y      = 0;   // wYCoord, tile row (0-based)
    uint8_t x      = 0;   // wXCoord, tile column (0-based)
    uint8_t warp   = 0;   // wWarpNumber, last-used warp index

    bool operator==(const CrystalMapCoord& o) const {
        return group == o.group && number == o.number
            && y == o.y && x == o.x && warp == o.warp;
    }
    bool operator!=(const CrystalMapCoord& o) const { return !(*this == o); }
};

/// Crystal RTC state captured at save time.
/// These are the hardware RTC values as latched by SaveRTC / StageRTCTimeForSave.
struct CrystalRtcState {
    uint8_t day    = 0;  // wRTC[0]: day byte (low 8 bits of day counter)
    uint8_t hour   = 0;  // wRTC[1]
    uint8_t minute = 0;  // wRTC[2]
    uint8_t second = 0;  // wRTC[3]
    bool    dst    = false;  // wDST bit 7

    bool operator==(const CrystalRtcState& o) const {
        return day == o.day && hour == o.hour && minute == o.minute
            && second == o.second && dst == o.dst;
    }
    bool operator!=(const CrystalRtcState& o) const { return !(*this == o); }
};

struct CrystalSaveSnapshot {
    // ── Phase 1: Money / coins / event flags ──────────────────────────────────

    /// wMoney: player money, 0–999 999.  Decoded from 3-byte BCD.
    uint32_t money_player = 0;

    /// wMomsMoney: mom's saved money, 0–999 999.  Decoded from 3-byte BCD.
    uint32_t money_mom = 0;

    /// wCoins: Game Corner coins, 0–9 999.  Decoded from 2-byte big-endian.
    uint16_t coins = 0;

    /// wEventFlags bitfield: 800 flags, 100 bytes.
    /// Bit i is set when the flag with index i is set in Crystal SRAM.
    std::array<uint8_t, 100> event_flags{};  // 800 flags, bit-packed

    // ── Phase 2: Player identity ──────────────────────────────────────────────

    /// wPlayerID — trainer ID, 2 bytes LE.
    uint16_t player_id = 0;

    /// wSecretID — 2 bytes LE.
    uint16_t secret_id = 0;

    /// wPlayerName — decoded from Crystal charmap to UTF-8.  Max 10 chars + NUL.
    std::string player_name;

    /// wMomsName — decoded from Crystal charmap to UTF-8.
    std::string moms_name;

    /// wRivalName — decoded from Crystal charmap to UTF-8.
    std::string rival_name;

    /// wPlayerGender — 0 = boy, 1 = girl.
    /// Stored in sOptions[0] (byte 0 of sOptions block, sav 0x2000).
    /// NOT in the checksummed region.
    uint8_t player_gender = 0;

    // ── Phase 2: Map position ─────────────────────────────────────────────────

    /// (wMapGroup, wMapNumber, wYCoord, wXCoord, wWarpNumber)
    CrystalMapCoord location;

    // ── Phase 2: Per-map scene state ──────────────────────────────────────────

    /// 79 scene-state bytes: sav 0x2500..0x254E.
    /// slot_index maps 1:1 to the scene ID array order in Crystal SRAM
    /// (wPokecenter2FSceneID at slot 0 through wMobileBattleRoomSceneID at slot 78).
    std::array<uint8_t, sram_layout::SCENE_IDS_COUNT> scene_ids{};

    // ── Phase 2: Play time ────────────────────────────────────────────────────

    /// wGameTimeHours — decoded as big-endian u16 (suiCune fix 2026-08).
    /// Capped at 999 when wGameTimeCap bit 7 is set.
    uint16_t playtime_hours   = 0;

    /// wGameTimeMinutes — 0–59.
    uint8_t  playtime_minutes = 0;

    /// wGameTimeSeconds — 0–59.
    uint8_t  playtime_seconds = 0;

    /// wGameTimeCap bit 7: playtime display is capped at 999:59.
    bool     playtime_capped  = false;

    // ── Phase 2: RTC / time context ───────────────────────────────────────────

    /// wRTC hardware mirror + wDST.
    CrystalRtcState rtc;

    // ── EngineFlags note ──────────────────────────────────────────────────────
    // EngineFlags ("eflag_XXXX") are Enginemon-only; they have no presence in
    // Crystal SRAM.  Import leaves EngineFlags untouched.  Export ignores them.
};

}  // namespace crystal
