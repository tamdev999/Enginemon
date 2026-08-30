#pragma once
// frontends/crystal/save/crystal_sram_layout.hpp
//
// Named SRAM offset constants for Pokémon Crystal (English).
//
// All offsets are SRAM file offsets (byte 0 = start of the .sav file).
//
// Derivation — two sources used and cross-checked:
//
//   (A) SRAM labels from the assembled sym file (pokecrystal11.sym):
//         sav_offset = bank * 0x2000 + (sram_addr - 0xA000)
//       These labels are authoritative for sOptions, sCheckValue1/2,
//       sGameData/End, sChecksum, and all sBackup* equivalents.
//
//   (B) WRAM labels from the sym file, mapped into SRAM via:
//         sav_offset = PRIMARY_GAME_DATA + (wram_addr - wGameData_wram)
//         wGameData = sym 01:d47b → wram = 0xD47B
//         PRIMARY_GAME_DATA = sym 01:a009 → sav = 0x2009
//       These are used for wMoney, wCoins, wEventFlags, etc.
//
// Every constant below has a comment showing the sym file line and value used
// to derive or verify it.
//
// RULES:
//   - No packed structs. Use sram.data[OFFSET] explicitly.
//   - Crystal-specific knowledge stays in this file and frontends/crystal/save/ only.

#include <cstdint>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>

namespace crystal {
namespace sram_layout {

// ─── SRAM image size ─────────────────────────────────────────────────────────

/// Exact size of the raw SRAM image.
static constexpr uint32_t SRAM_SIZE = 0x8000;  // 32 768 bytes

// ─── Save integrity constants ─────────────────────────────────────────────────
// Source: constants/misc_constants.asm
//   SAVE_CHECK_VALUE_1 EQU 99   (0x63)
//   SAVE_CHECK_VALUE_2 EQU 127  (0x7F)

static constexpr uint8_t SAVE_CHECK_VALUE_1 = 99;   // 0x63
static constexpr uint8_t SAVE_CHECK_VALUE_2 = 127;  // 0x7F

// ─── Primary save copy ────────────────────────────────────────────────────────
// Sym source (pokecrystal11.sym), formula: bank * 0x2000 + (addr - 0xA000)
//
//   sOptions          01:a000  → 0x2000
//   sCheckValue1      01:a008  → 0x2008
//   sGameData         01:a009  → 0x2009
//   sPlayerData       01:a009  → 0x2009  (same address as sGameData)
//   sGameDataEnd      01:ab83  → 0x2B83  (first byte NOT checksummed)
//   sChecksum         01:ad0d  → 0x2D0D
//   sCheckValue2      01:ad0f  → 0x2D0F

/// sOptions — 8 bytes.  sym: 01:a000 → sav 0x2000
static constexpr uint32_t PRIMARY_OPTIONS          = 0x2000;
static constexpr uint32_t PRIMARY_OPTIONS_SIZE     = 8;  // wOptionsEnd - wOptions

/// sCheckValue1 — 1 byte sentinel.  sym: 01:a008 → sav 0x2008
static constexpr uint32_t PRIMARY_CHECK_VALUE_1    = 0x2008;

/// sGameData — first byte of checksummed region.  sym: 01:a009 → sav 0x2009
static constexpr uint32_t PRIMARY_GAME_DATA        = 0x2009;

/// Last byte (inclusive) of checksummed region.
/// sGameDataEnd sym: 01:ab83 → sav 0x2B83 (exclusive end).
/// Last inclusive byte = 0x2B83 - 1 = 0x2B82.
static constexpr uint32_t PRIMARY_CHECKSUM_END     = 0x2B82;

/// sChecksum — u16 LE.  sym: 01:ad0d → sav 0x2D0D
static constexpr uint32_t PRIMARY_CHECKSUM         = 0x2D0D;

/// sCheckValue2 — 1 byte sentinel.  sym: 01:ad0f → sav 0x2D0F
static constexpr uint32_t PRIMARY_CHECK_VALUE_2    = 0x2D0F;

/// Size of the checksummed region: sGameDataEnd - sGameData = 0x2B83 - 0x2009 = 2938
static constexpr uint32_t CHECKSUM_REGION_SIZE     = PRIMARY_CHECKSUM_END - PRIMARY_GAME_DATA + 1;

// ─── Backup save copy ─────────────────────────────────────────────────────────
// Sym source (pokecrystal11.sym), formula: bank * 0x2000 + (addr - 0xA000)
//
//   sBackupOptions        00:b200  → 0x1200
//   sBackupCheckValue1    00:b208  → 0x1208
//   sBackupGameData       00:b209  → 0x1209
//   sBackupPlayerData     00:b209  → 0x1209  (same address)
//   sBackupGameDataEnd    00:bd83  → 0x1D83  (first byte NOT checksummed)
//   sBackupChecksum       00:bf0d  → 0x1F0D
//   sBackupCheckValue2    00:bf0f  → 0x1F0F

/// sBackupOptions — 8 bytes.  sym: 00:b200 → sav 0x1200
static constexpr uint32_t BACKUP_OPTIONS           = 0x1200;

/// sBackupCheckValue1 — 1 byte sentinel.  sym: 00:b208 → sav 0x1208
static constexpr uint32_t BACKUP_CHECK_VALUE_1     = 0x1208;

/// sBackupGameData — first byte of backup checksummed region.  sym: 00:b209 → sav 0x1209
static constexpr uint32_t BACKUP_GAME_DATA         = 0x1209;

/// Last byte (inclusive) of backup checksummed region.
/// sBackupGameDataEnd sym: 00:bd83 → sav 0x1D83 (exclusive).
/// Last inclusive = 0x1D83 - 1 = 0x1D82.
static constexpr uint32_t BACKUP_CHECKSUM_END      = 0x1D82;

/// sBackupChecksum — u16 LE.  sym: 00:bf0d → sav 0x1F0D
static constexpr uint32_t BACKUP_CHECKSUM          = 0x1F0D;

/// sBackupCheckValue2 — 1 byte sentinel.  sym: 00:bf0f → sav 0x1F0F
static constexpr uint32_t BACKUP_CHECK_VALUE_2     = 0x1F0F;

// ─── Field offsets within the primary checksummed region ─────────────────────
// Derived from WRAM symbols via:
//   sav_offset = PRIMARY_GAME_DATA + (wram_addr - wGameData_wram)
//   wGameData  sym: 01:d47b  wram = 0xD47B
//
//   wMoney         sym: 01:d84e  wram=0xD84E  sav = 0x2009 + (0xD84E - 0xD47B) = 0x23DC
//   wMomsMoney     sym: 01:d851  wram=0xD851  sav = 0x2009 + (0xD851 - 0xD47B) = 0x23DF
//   wMomSavingMoney sym: 01:d854 wram=0xD854  sav = 0x2009 + (0xD854 - 0xD47B) = 0x23E2
//   wCoins         sym: 01:d855  wram=0xD855  sav = 0x2009 + (0xD855 - 0xD47B) = 0x23E3
//   wJohtoBadges   sym: 01:d857  wram=0xD857  sav = 0x2009 + (0xD857 - 0xD47B) = 0x23E5
//   wKantoBadges   sym: 01:d858  wram=0xD858  sav = 0x2009 + (0xD858 - 0xD47B) = 0x23E6
//   wEventFlags    sym: 01:da72  wram=0xDA72  sav = 0x2009 + (0xDA72 - 0xD47B) = 0x2600
//
// NOTE: EVENT_FLAGS was previously 0x2601 (off-by-one from manual byte count).
//       Corrected to 0x2600 from the sym file (01:da72).

/// wMoney — 3 bytes BCD big-endian.  sym: 01:d84e → sav 0x23DC
static constexpr uint32_t MONEY                    = 0x23DC;
static constexpr uint32_t MONEY_SIZE               = 3;

/// wMomsMoney — 3 bytes BCD big-endian.  sym: 01:d851 → sav 0x23DF
static constexpr uint32_t MOMS_MONEY               = 0x23DF;
static constexpr uint32_t MOMS_MONEY_SIZE          = 3;

/// wMomSavingMoney — 1 byte flag.  sym: 01:d854 → sav 0x23E2
static constexpr uint32_t MOM_SAVING_MONEY         = 0x23E2;

/// wCoins — 2 bytes big-endian (high byte first).  sym: 01:d855 → sav 0x23E3
static constexpr uint32_t COINS                    = 0x23E3;
static constexpr uint32_t COINS_SIZE               = 2;

/// wJohtoBadges — 1 byte bitmask.  sym: 01:d857 → sav 0x23E5
static constexpr uint32_t JOHTO_BADGES             = 0x23E5;

/// wKantoBadges — 1 byte bitmask.  sym: 01:d858 → sav 0x23E6
static constexpr uint32_t KANTO_BADGES             = 0x23E6;

/// wEventFlags — 800 flags, 100 bytes bitfield.
/// sym: 01:da72 → sav 0x2600
/// (Previously 0x2601 — corrected from sym file 2026-08.)
static constexpr uint32_t EVENT_FLAGS              = 0x2600;
static constexpr uint32_t EVENT_FLAGS_SIZE         = 100;  // ceil(800 / 8)
static constexpr uint32_t NUM_EVENT_FLAGS          = 800;

// ─── Backup mirror offsets ────────────────────────────────────────────────────
// Backup region mirrors primary with a fixed negative offset:
//   BACKUP_OFFSET = PRIMARY_GAME_DATA - BACKUP_GAME_DATA = 0x2009 - 0x1209 = 0xE00
//   backup_addr = primary_addr - BACKUP_OFFSET

static constexpr uint32_t BACKUP_OFFSET            = PRIMARY_GAME_DATA - BACKUP_GAME_DATA;

static constexpr uint32_t BACKUP_MONEY             = MONEY        - BACKUP_OFFSET;
static constexpr uint32_t BACKUP_MOMS_MONEY        = MOMS_MONEY   - BACKUP_OFFSET;
static constexpr uint32_t BACKUP_COINS             = COINS        - BACKUP_OFFSET;
static constexpr uint32_t BACKUP_EVENT_FLAGS       = EVENT_FLAGS  - BACKUP_OFFSET;

// ─── Compile-time assertions ──────────────────────────────────────────────────

// Region sizes must match sym-derived values exactly.
static_assert(CHECKSUM_REGION_SIZE == 2938,
    "Checksum region must be exactly 2938 bytes (sGameDataEnd - sGameData)");
static_assert(BACKUP_CHECKSUM_END - BACKUP_GAME_DATA + 1 == CHECKSUM_REGION_SIZE,
    "Backup checksummed region must be the same size as primary");
static_assert(BACKUP_OFFSET == 0xE00,
    "Backup mirror offset must be 0xE00 (0x2009 - 0x1209)");

// Sym-pinned absolute values — these fail to compile if any constant drifts.
static_assert(PRIMARY_OPTIONS       == 0x2000, "sym: 01:a000");
static_assert(PRIMARY_CHECK_VALUE_1 == 0x2008, "sym: 01:a008");
static_assert(PRIMARY_GAME_DATA     == 0x2009, "sym: 01:a009");
static_assert(PRIMARY_CHECKSUM_END  == 0x2B82, "sym: sGameDataEnd 01:ab83 → end = 0x2B82");
static_assert(PRIMARY_CHECKSUM      == 0x2D0D, "sym: 01:ad0d");
static_assert(PRIMARY_CHECK_VALUE_2 == 0x2D0F, "sym: 01:ad0f");
static_assert(BACKUP_OPTIONS        == 0x1200, "sym: 00:b200");
static_assert(BACKUP_CHECK_VALUE_1  == 0x1208, "sym: 00:b208");
static_assert(BACKUP_GAME_DATA      == 0x1209, "sym: 00:b209");
static_assert(BACKUP_CHECKSUM_END   == 0x1D82, "sym: sBackupGameDataEnd 00:bd83 → end = 0x1D82");
static_assert(BACKUP_CHECKSUM       == 0x1F0D, "sym: 00:bf0d");
static_assert(BACKUP_CHECK_VALUE_2  == 0x1F0F, "sym: 00:bf0f");
static_assert(MONEY                 == 0x23DC, "sym: 01:d84e → 0x2009 + (0xD84E - 0xD47B)");
static_assert(MOMS_MONEY            == 0x23DF, "sym: 01:d851 → 0x2009 + (0xD851 - 0xD47B)");
static_assert(COINS                 == 0x23E3, "sym: 01:d855 → 0x2009 + (0xD855 - 0xD47B)");
static_assert(EVENT_FLAGS           == 0x2600, "sym: 01:da72 → 0x2009 + (0xDA72 - 0xD47B)");

// All owned fields must fall inside the checksummed region.
static_assert(MONEY       >= PRIMARY_GAME_DATA && MONEY       + MONEY_SIZE       - 1 <= PRIMARY_CHECKSUM_END, "");
static_assert(MOMS_MONEY  >= PRIMARY_GAME_DATA && MOMS_MONEY  + MOMS_MONEY_SIZE  - 1 <= PRIMARY_CHECKSUM_END, "");
static_assert(COINS       >= PRIMARY_GAME_DATA && COINS       + COINS_SIZE       - 1 <= PRIMARY_CHECKSUM_END, "");
static_assert(EVENT_FLAGS >= PRIMARY_GAME_DATA && EVENT_FLAGS + EVENT_FLAGS_SIZE - 1 <= PRIMARY_CHECKSUM_END, "");
static_assert(SRAM_SIZE   == 0x8000, "");

// ─── Symbol-validation helper (runtime) ──────────────────────────────────────
// Loads pokecrystal11.sym and verifies every sym-derived constant against the
// actual assembled addresses.  Called from the test suite to prove the constants
// remain correct if the sym file ever changes.
//
// Returns empty string on success.  Returns an error description on any mismatch.
// Skips silently if the sym file is not found (CI without references/).
[[nodiscard]] std::string validate_layout_against_sym(
    const std::filesystem::path& sym_file);

}  // namespace sram_layout
}  // namespace crystal
