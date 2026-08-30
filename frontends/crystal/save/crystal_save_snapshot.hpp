#pragma once
// frontends/crystal/save/crystal_save_snapshot.hpp
//
// CrystalSaveSnapshot — semantic save state decoded from a Crystal SRAM image.
//
// Phase 1: money_player, money_mom, coins, event_flags
// Phase 2: player_id, secret_id, names, gender, map, scene_ids, playtime, RTC
// Phase 3A: party (6 Pokémon max)
//
// This type is SEMANTIC at the Crystal boundary: no raw SRAM bytes, no packed
// structs, no ROM offsets.  Crystal charmap strings are decoded to UTF-8.
// Crystal numeric IDs (species, moves, items) map 1:1 to native IDs for the
// Crystal frontend; translation happens at this codec boundary only.

#include "crystal_sram_layout.hpp"
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace crystal {

// ─── Map coordinate ──────────────────────────────────────────────────────────

struct CrystalMapCoord {
    uint8_t group  = 0;
    uint8_t number = 0;
    uint8_t y      = 0;
    uint8_t x      = 0;
    uint8_t warp   = 0;
    bool operator==(const CrystalMapCoord& o) const {
        return group==o.group && number==o.number && y==o.y && x==o.x && warp==o.warp;
    }
    bool operator!=(const CrystalMapCoord& o) const { return !(*this == o); }
};

// ─── RTC state ───────────────────────────────────────────────────────────────

struct CrystalRtcState {
    uint8_t day    = 0;
    uint8_t hour   = 0;
    uint8_t minute = 0;
    uint8_t second = 0;
    bool    dst    = false;
    bool operator==(const CrystalRtcState& o) const {
        return day==o.day && hour==o.hour && minute==o.minute
            && second==o.second && dst==o.dst;
    }
    bool operator!=(const CrystalRtcState& o) const { return !(*this == o); }
};

// ─── Phase 3A: Pokémon ───────────────────────────────────────────────────────

/// DVs as four 4-bit values, matching Crystal's encoding exactly.
/// Source: macros/ram.asm MON_DVS layout.
///   byte0 = ATK(7-4) | DEF(3-0)
///   byte1 = SPD(7-4) | SPC(3-0)   (SPC covers both SpAtk and SpDef DVs)
///   HP DV is derived: (ATK&1)<<3 | (DEF&1)<<2 | (SPD&1)<<1 | (SPC&1)
struct CrystalDVs {
    uint8_t atk = 0;  // 0–15
    uint8_t def = 0;  // 0–15
    uint8_t spd = 0;  // 0–15
    uint8_t spc = 0;  // 0–15  (covers both SpAtk DV and SpDef DV in Gen2)

    uint8_t hp_dv() const {
        return static_cast<uint8_t>(
            ((atk & 1u) << 3) | ((def & 1u) << 2) | ((spd & 1u) << 1) | (spc & 1u));
    }

    bool operator==(const CrystalDVs& o) const {
        return atk==o.atk && def==o.def && spd==o.spd && spc==o.spc;
    }
    bool operator!=(const CrystalDVs& o) const { return !(*this == o); }
};

/// Caught data decoded from the two packed bytes.
/// Source: constants/pokemon_data_constants.asm CAUGHT_* masks.
struct CrystalCaughtData {
    // byte 0 (offset +29):
    uint8_t time_of_day    = 0;  // 0=unknown, 1=morning, 2=day, 3=night  (bits 7-6)
    uint8_t caught_level   = 0;  // 1–63; 1 = egg                          (bits 5-0)
    // byte 1 (offset +30):
    bool    caught_by_boy  = false;  // bit 7: catcher gender (1=boy, 0=girl/unknown)
    uint8_t location       = 0;     // Landmark ID, bits 6-0

    bool operator==(const CrystalCaughtData& o) const {
        return time_of_day==o.time_of_day && caught_level==o.caught_level
            && caught_by_boy==o.caught_by_boy && location==o.location;
    }
};

/// One Pokémon's decoded state from a party_struct + OT name + nickname.
///
/// All numeric IDs (species, moves, item) are Crystal-native values at the
/// codec boundary; they map 1:1 to SpeciesId/MoveId/ItemId for Crystal-profile
/// saves.  The codec rejects IDs outside the profile's known ranges.
///
/// Stats (MaxHP, ATK, DEF, SPD, SAT, SDF) are stored as a cached copy exactly
/// as Crystal stores them.  Crystal recomputes them from DVs+statEXP+level on
/// every load (CorrectPartyErrors → CalcMonStats).  Current HP is the only
/// runtime-authoritative field that cannot be recomputed.
struct CrystalPartyMon {
    // ── Identity ─────────────────────────────────────────────────────────────
    uint8_t  species   = 0;    // Crystal species ID (1–251, or EGG=254)
    uint8_t  item      = 0;    // Held item ID (0 = none)
    uint16_t ot_id     = 0;    // Trainer ID, little-endian u16
    uint8_t  level     = 0;    // 2–100
    std::string nickname;      // UTF-8, decoded from Crystal charmap (≤10 chars)
    std::string ot_name;       // UTF-8, decoded from Crystal charmap (≤10 chars)

    // ── Moves and PP ─────────────────────────────────────────────────────────
    std::array<uint8_t, 4> moves{};    // Crystal move IDs (0 = none)
    std::array<uint8_t, 4> pp{};       // Current PP for each move (bits 5-0 of PP byte)
    std::array<uint8_t, 4> pp_ups{};   // PP Up count 0–3 (bits 7-6 of PP byte, shifted)

    // ── Experience ───────────────────────────────────────────────────────────
    uint32_t exp = 0;          // 3-byte big-endian in SRAM; stored as u32 here

    // ── Stat experience (one value per stat, u16 LE each) ────────────────────
    uint16_t stat_exp_hp  = 0;
    uint16_t stat_exp_atk = 0;
    uint16_t stat_exp_def = 0;
    uint16_t stat_exp_spd = 0;
    uint16_t stat_exp_spc = 0;  // shared SpAtk+SpDef stat EXP (Gen2)

    // ── Determinant values ───────────────────────────────────────────────────
    CrystalDVs dvs;

    // ── Status ───────────────────────────────────────────────────────────────
    uint8_t  status     = 0;   // status condition byte (BRN/FRZ/PSN/PAR/SLP bits)
    uint8_t  happiness  = 0;
    uint8_t  pokerus    = 0;   // Pokérus status byte

    // ── Caught data ──────────────────────────────────────────────────────────
    CrystalCaughtData caught;

    // ── Current battle-ready stats (Crystal-managed cache) ───────────────────
    // Stored as big-endian u16 in SRAM (CalcMonStats: hMultiplicand[1]=high).
    // Preserved verbatim to avoid altering checksums unnecessarily.
    // Crystal recomputes all except current_hp on load.
    uint16_t current_hp = 0;   // authoritative: damage taken, preserved as-is
    uint16_t max_hp     = 0;   // cache: recomputed by CalcMonStats
    uint16_t stat_atk   = 0;
    uint16_t stat_def   = 0;
    uint16_t stat_spd   = 0;
    uint16_t stat_sat   = 0;
    uint16_t stat_sdf   = 0;

    bool operator==(const CrystalPartyMon& o) const;
    bool operator!=(const CrystalPartyMon& o) const { return !(*this == o); }
};

/// The decoded party: up to 6 Pokémon.
/// party_count is the authoritative count; party_mons[0..party_count-1] are valid.
struct CrystalParty {
    uint8_t  party_count = 0;
    std::array<CrystalPartyMon, 6> party_mons{};

    bool operator==(const CrystalParty& o) const {
        if (party_count != o.party_count) return false;
        for (uint8_t i = 0; i < party_count; ++i)
            if (party_mons[i] != o.party_mons[i]) return false;
        return true;
    }
};

// ─── Full snapshot ───────────────────────────────────────────────────────────

struct CrystalSaveSnapshot {
    // ── Phase 1 ───────────────────────────────────────────────────────────────
    uint32_t money_player = 0;
    uint32_t money_mom    = 0;
    uint16_t coins        = 0;
    std::array<uint8_t, 100> event_flags{};

    // ── Phase 2 ───────────────────────────────────────────────────────────────
    uint16_t    player_id     = 0;
    uint16_t    secret_id     = 0;
    std::string player_name;
    std::string moms_name;
    std::string rival_name;
    uint8_t     player_gender = 0;
    CrystalMapCoord location;
    std::array<uint8_t, sram_layout::SCENE_IDS_COUNT> scene_ids{};
    uint16_t    playtime_hours   = 0;
    uint8_t     playtime_minutes = 0;
    uint8_t     playtime_seconds = 0;
    bool        playtime_capped  = false;
    CrystalRtcState rtc;

    // ── Phase 3A ──────────────────────────────────────────────────────────────
    CrystalParty party;
};

// CrystalPartyMon equality (defined out-of-line to avoid header bloat)
inline bool CrystalPartyMon::operator==(const CrystalPartyMon& o) const {
    return species   == o.species
        && item      == o.item
        && ot_id     == o.ot_id
        && level     == o.level
        && nickname  == o.nickname
        && ot_name   == o.ot_name
        && moves     == o.moves
        && pp        == o.pp
        && pp_ups    == o.pp_ups
        && exp       == o.exp
        && stat_exp_hp  == o.stat_exp_hp
        && stat_exp_atk == o.stat_exp_atk
        && stat_exp_def == o.stat_exp_def
        && stat_exp_spd == o.stat_exp_spd
        && stat_exp_spc == o.stat_exp_spc
        && dvs       == o.dvs
        && status    == o.status
        && happiness == o.happiness
        && pokerus   == o.pokerus
        && caught    == o.caught
        && current_hp == o.current_hp
        && max_hp    == o.max_hp
        && stat_atk  == o.stat_atk
        && stat_def  == o.stat_def
        && stat_spd  == o.stat_spd
        && stat_sat  == o.stat_sat
        && stat_sdf  == o.stat_sdf;
}

}  // namespace crystal
