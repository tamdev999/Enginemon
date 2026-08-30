#pragma once
// frontends/crystal/save/crystal_save_snapshot.hpp
//
// CrystalSaveSnapshot — Phase 1 minimal semantic layer.
//
// Only the fields currently represented in GameState are decoded here.
// Party, boxes, Pokédex, trainer identity, badges, items are not yet included.
// They will be added when the corresponding GameState domains land (v7 bump).
//
// This type is SEMANTIC: no SRAM offsets, no raw bytes.
// The raw shadow lives in CrystalImport::shadow (crystal_sram.hpp).

#include <array>
#include <cstdint>

namespace crystal {

struct CrystalSaveSnapshot {
    // ── Money ─────────────────────────────────────────────────────────────────
    /// wMoney: player money, 0–999 999.  Decoded from 3-byte BCD.
    uint32_t money_player = 0;

    /// wMomsMoney: mom's saved money, 0–999 999.  Decoded from 3-byte BCD.
    uint32_t money_mom = 0;

    /// wCoins: Game Corner coins, 0–9 999.  Decoded from 2-byte big-endian.
    uint16_t coins = 0;

    // ── Event flags ───────────────────────────────────────────────────────────
    /// wEventFlags bitfield: 800 flags, 100 bytes.
    /// Bit i is set when the flag with index i is set in Crystal SRAM.
    /// Stored here as raw bytes so the codec can manipulate individual bits.
    ///
    /// Semantic flag IDs map as:  flag index i → GameState flag "flag_XXXX"
    /// where XXXX is the zero-padded 4-digit hex representation of i.
    std::array<uint8_t, 100> event_flags{};  // 800 flags, bit-packed

    // ── Note on EngineFlags ───────────────────────────────────────────────────
    // EngineFlags ("eflag_XXXX") are Enginemon-only; they have no presence in
    // Crystal SRAM.  Import leaves EngineFlags untouched.  Export ignores them.
};

}  // namespace crystal
