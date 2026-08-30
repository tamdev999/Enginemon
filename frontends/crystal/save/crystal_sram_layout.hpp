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

// ─── Phase 2 field offsets ────────────────────────────────────────────────────
// All derived from WRAM symbols using the same formula:
//   sav_offset = PRIMARY_GAME_DATA + (wram_addr - wGameData_wram)
//   wGameData_wram = 0xD47B  (sym: 01:d47b)
//
// Fields marked OPTION are in the sOptions block (sav 0x2000..0x2008),
// not in the checksummed region.  They still live in the shadow and are
// read/written as owned fields but via the OPTIONS block path.

// ── Player identity (inside checksummed region) ───────────────────────────────

/// wPlayerID — 2 bytes u16 LE trainer ID.  sym: 01:d47b → sav 0x2009
static constexpr uint32_t PLAYER_ID               = 0x2009;
static constexpr uint32_t PLAYER_ID_SIZE          = 2;

/// wPlayerName — 11 bytes Crystal charmap, 0x50-terminated.  sym: 01:d47d → sav 0x200B
static constexpr uint32_t PLAYER_NAME             = 0x200B;
static constexpr uint32_t PLAYER_NAME_SIZE        = 11;  // NAME_LENGTH

/// wMomsName — 11 bytes Crystal charmap.  sym: 01:d488 → sav 0x2016
static constexpr uint32_t MOMS_NAME               = 0x2016;
static constexpr uint32_t MOMS_NAME_SIZE          = 11;

/// wRivalName — 11 bytes Crystal charmap.  sym: 01:d493 → sav 0x2021
static constexpr uint32_t RIVAL_NAME              = 0x2021;
static constexpr uint32_t RIVAL_NAME_SIZE         = 11;

/// wSecretID — 2 bytes u16 LE.  sym: 01:d84a → sav 0x23D8
static constexpr uint32_t SECRET_ID               = 0x23D8;
static constexpr uint32_t SECRET_ID_SIZE          = 2;

// ── Player gender (OPTIONS block — NOT in checksummed region) ─────────────────
// wPlayerGender is the first byte of wCrystalData which maps to sOptions (0x2000).
// Crystal: 0 = boy, 1 = girl.
// sym: 01:d472 → wram 0xD472, which is sOptions[0] = sav 0x2000.
// This byte is mirrored to sBackupOptions[0] on save.
static constexpr uint32_t PLAYER_GENDER           = PRIMARY_OPTIONS;  // sav 0x2000

// ── Game time (inside checksummed region) ─────────────────────────────────────
// Crystal serialises game time in wPlayerData, so it's inside the checksummed block.

/// wGameTimeCap — 1 byte.  sym: 01:d4c3 → sav 0x2051
/// Bit 7 set when playtime is capped (999 hours).
static constexpr uint32_t GAME_TIME_CAP           = 0x2051;

/// wGameTimeHours — 2 bytes BIG-ENDIAN u16.  sym: 01:d4c4 → sav 0x2052
/// IMPORTANT: This field is BIG-ENDIAN (confirmed by suiCune serialize.c fix 2026-08).
/// High byte at [0x2052], low byte at [0x2053].
static constexpr uint32_t GAME_TIME_HOURS         = 0x2052;
static constexpr uint32_t GAME_TIME_HOURS_SIZE    = 2;

/// wGameTimeMinutes — 1 byte.  sym: 01:d4c6 → sav 0x2054
static constexpr uint32_t GAME_TIME_MINUTES       = 0x2054;

/// wGameTimeSeconds — 1 byte.  sym: 01:d4c7 → sav 0x2055
static constexpr uint32_t GAME_TIME_SECONDS       = 0x2055;

/// wGameTimeFrames — 1 byte.  sym: 01:d4c8 → sav 0x2056
static constexpr uint32_t GAME_TIME_FRAMES        = 0x2056;

// ── RTC / time context (inside checksummed region) ────────────────────────────

/// wStartDay — 1 byte.  Day when new game started.  sym: 01:d4b6 → sav 0x2044
static constexpr uint32_t START_DAY               = 0x2044;

/// wStartHour — 1 byte.  sym: 01:d4b7 → sav 0x2045
static constexpr uint32_t START_HOUR              = 0x2045;

/// wStartMinute — 1 byte.  sym: 01:d4b8 → sav 0x2046
static constexpr uint32_t START_MINUTE            = 0x2046;

/// wStartSecond — 1 byte.  sym: 01:d4b9 → sav 0x2047
static constexpr uint32_t START_SECOND            = 0x2047;

/// wRTC — 4 bytes: [day, hour, min, sec] hardware mirror.
/// sym: 01:d4ba → sav 0x2048
static constexpr uint32_t RTC_BYTES               = 0x2048;
static constexpr uint32_t RTC_BYTES_SIZE          = 4;

/// wDST — 1 byte, bit 7 = DST active.  sym: 01:d4c2 → sav 0x2050
static constexpr uint32_t DST                     = 0x2050;

/// wCurDay — 1 byte, current real-time day-of-week (0–6).
/// sym: 01:d4cb → sav 0x2059
static constexpr uint32_t CUR_DAY                 = 0x2059;

// ── Map position (inside checksummed region, in wCurMapData section) ──────────
// Note: these are in wCurMapData which begins after wPlayerDataEnd (sav 0x2833).

/// wWarpNumber — 1 byte, warp index within current map.  sym: 01:dcb4 → sav 0x2842
static constexpr uint32_t WARP_NUMBER             = 0x2842;

/// wMapGroup — 1 byte.  sym: 01:dcb5 → sav 0x2843
static constexpr uint32_t MAP_GROUP               = 0x2843;

/// wMapNumber — 1 byte.  sym: 01:dcb6 → sav 0x2844
static constexpr uint32_t MAP_NUMBER              = 0x2844;

/// wYCoord — 1 byte (tile row).  sym: 01:dcb7 → sav 0x2845
static constexpr uint32_t PLAYER_Y                = 0x2845;

/// wXCoord — 1 byte (tile column).  sym: 01:dcb8 → sav 0x2846
static constexpr uint32_t PLAYER_X                = 0x2846;

// ── Per-map scene state array (inside checksummed region) ────────────────────
// 79 scene slots, 1 byte each: sav 0x2500..0x254E.
// Each slot corresponds to a named map's scene state variable.
// Slot index = (wXxxSceneID offset - wPokecenter2FSceneID offset).
// sym first: wPokecenter2FSceneID → 01:d972 → sav 0x2500
// sym last:  wMobileBattleRoomSceneID → 01:d9c0 → sav 0x254E

static constexpr uint32_t SCENE_IDS_BASE          = 0x2500;
static constexpr uint32_t SCENE_IDS_COUNT         = 79;   // wPokecenter2F .. wMobileBattleRoom
static constexpr uint32_t SCENE_IDS_SIZE          = SCENE_IDS_COUNT;

/// Per-slot index for the scene ID slots.
/// scene_sav_offset(slot_i) = SCENE_IDS_BASE + slot_i
/// The mapping symbol → slot_i is defined in the scene table below.

// ─── Phase 2 compile-time assertions ─────────────────────────────────────────

static_assert(PLAYER_ID     == 0x2009, "sym: 01:d47b");
static_assert(PLAYER_NAME   == 0x200B, "sym: 01:d47d");
static_assert(RIVAL_NAME    == 0x2021, "sym: 01:d493");
static_assert(SECRET_ID     == 0x23D8, "sym: 01:d84a");
static_assert(GAME_TIME_HOURS == 0x2052, "sym: 01:d4c4, big-endian u16");
static_assert(RTC_BYTES     == 0x2048, "sym: 01:d4ba");
static_assert(DST           == 0x2050, "sym: 01:d4c2");
static_assert(MAP_GROUP     == 0x2843, "sym: 01:dcb5");
static_assert(MAP_NUMBER    == 0x2844, "sym: 01:dcb6");
static_assert(PLAYER_Y      == 0x2845, "sym: 01:dcb7");
static_assert(PLAYER_X      == 0x2846, "sym: 01:dcb8");
static_assert(SCENE_IDS_BASE == 0x2500, "sym: wPokecenter2FSceneID 01:d972");

// wCurMapData fields are past wPlayerDataEnd (sav 0x2833) and still inside checksum region.
static_assert(MAP_GROUP     >= PRIMARY_GAME_DATA && MAP_GROUP     <= PRIMARY_CHECKSUM_END, "");
static_assert(SCENE_IDS_BASE >= PRIMARY_GAME_DATA && SCENE_IDS_BASE + SCENE_IDS_SIZE - 1 <= PRIMARY_CHECKSUM_END, "");

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
