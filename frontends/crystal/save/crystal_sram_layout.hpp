#pragma once
// frontends/crystal/save/crystal_sram_layout.hpp
//
// Named SRAM offset constants for PokÃ©mon Crystal (English).
//
// All offsets are SRAM file offsets (byte 0 = start of the .sav file).
//
// Derivation â€” two sources used and cross-checked:
//
//   (A) SRAM labels from the assembled sym file (pokecrystal11.sym):
//         sav_offset = bank * 0x2000 + (sram_addr - 0xA000)
//       These labels are authoritative for sOptions, sCheckValue1/2,
//       sGameData/End, sChecksum, and all sBackup* equivalents.
//
//   (B) WRAM labels from the sym file, mapped into SRAM via:
//         sav_offset = PRIMARY_GAME_DATA + (wram_addr - wGameData_wram)
//         wGameData = sym 01:d47b â†’ wram = 0xD47B
//         PRIMARY_GAME_DATA = sym 01:a009 â†’ sav = 0x2009
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

// â”€â”€â”€ SRAM image size â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

/// Exact size of the raw SRAM image.
static constexpr uint32_t SRAM_SIZE = 0x8000;  // 32 768 bytes

// â”€â”€â”€ Save integrity constants â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
// Source: constants/misc_constants.asm
//   SAVE_CHECK_VALUE_1 EQU 99   (0x63)
//   SAVE_CHECK_VALUE_2 EQU 127  (0x7F)

static constexpr uint8_t SAVE_CHECK_VALUE_1 = 99;   // 0x63
static constexpr uint8_t SAVE_CHECK_VALUE_2 = 127;  // 0x7F

// â”€â”€â”€ Primary save copy â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
// Sym source (pokecrystal11.sym), formula: bank * 0x2000 + (addr - 0xA000)
//
//   sOptions          01:a000  â†’ 0x2000
//   sCheckValue1      01:a008  â†’ 0x2008
//   sGameData         01:a009  â†’ 0x2009
//   sPlayerData       01:a009  â†’ 0x2009  (same address as sGameData)
//   sGameDataEnd      01:ab83  â†’ 0x2B83  (first byte NOT checksummed)
//   sChecksum         01:ad0d  â†’ 0x2D0D
//   sCheckValue2      01:ad0f  â†’ 0x2D0F

/// sOptions â€” 8 bytes.  sym: 01:a000 â†’ sav 0x2000
static constexpr uint32_t PRIMARY_OPTIONS          = 0x2000;
static constexpr uint32_t PRIMARY_OPTIONS_SIZE     = 8;  // wOptionsEnd - wOptions

/// sCheckValue1 â€” 1 byte sentinel.  sym: 01:a008 â†’ sav 0x2008
static constexpr uint32_t PRIMARY_CHECK_VALUE_1    = 0x2008;

/// sGameData â€” first byte of checksummed region.  sym: 01:a009 â†’ sav 0x2009
static constexpr uint32_t PRIMARY_GAME_DATA        = 0x2009;

/// Last byte (inclusive) of checksummed region.
/// sGameDataEnd sym: 01:ab83 â†’ sav 0x2B83 (exclusive end).
/// Last inclusive byte = 0x2B83 - 1 = 0x2B82.
static constexpr uint32_t PRIMARY_CHECKSUM_END     = 0x2B82;

/// sChecksum â€” u16 LE.  sym: 01:ad0d â†’ sav 0x2D0D
static constexpr uint32_t PRIMARY_CHECKSUM         = 0x2D0D;

/// sCheckValue2 â€” 1 byte sentinel.  sym: 01:ad0f â†’ sav 0x2D0F
static constexpr uint32_t PRIMARY_CHECK_VALUE_2    = 0x2D0F;

/// Size of the checksummed region: sGameDataEnd - sGameData = 0x2B83 - 0x2009 = 2938
static constexpr uint32_t CHECKSUM_REGION_SIZE     = PRIMARY_CHECKSUM_END - PRIMARY_GAME_DATA + 1;

// â”€â”€â”€ Backup save copy â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
// Sym source (pokecrystal11.sym), formula: bank * 0x2000 + (addr - 0xA000)
//
//   sBackupOptions        00:b200  â†’ 0x1200
//   sBackupCheckValue1    00:b208  â†’ 0x1208
//   sBackupGameData       00:b209  â†’ 0x1209
//   sBackupPlayerData     00:b209  â†’ 0x1209  (same address)
//   sBackupGameDataEnd    00:bd83  â†’ 0x1D83  (first byte NOT checksummed)
//   sBackupChecksum       00:bf0d  â†’ 0x1F0D
//   sBackupCheckValue2    00:bf0f  â†’ 0x1F0F

/// sBackupOptions â€” 8 bytes.  sym: 00:b200 â†’ sav 0x1200
static constexpr uint32_t BACKUP_OPTIONS           = 0x1200;

/// sBackupCheckValue1 â€” 1 byte sentinel.  sym: 00:b208 â†’ sav 0x1208
static constexpr uint32_t BACKUP_CHECK_VALUE_1     = 0x1208;

/// sBackupGameData â€” first byte of backup checksummed region.  sym: 00:b209 â†’ sav 0x1209
static constexpr uint32_t BACKUP_GAME_DATA         = 0x1209;

/// Last byte (inclusive) of backup checksummed region.
/// sBackupGameDataEnd sym: 00:bd83 â†’ sav 0x1D83 (exclusive).
/// Last inclusive = 0x1D83 - 1 = 0x1D82.
static constexpr uint32_t BACKUP_CHECKSUM_END      = 0x1D82;

/// sBackupChecksum â€” u16 LE.  sym: 00:bf0d â†’ sav 0x1F0D
static constexpr uint32_t BACKUP_CHECKSUM          = 0x1F0D;

/// sBackupCheckValue2 â€” 1 byte sentinel.  sym: 00:bf0f â†’ sav 0x1F0F
static constexpr uint32_t BACKUP_CHECK_VALUE_2     = 0x1F0F;

// â”€â”€â”€ Field offsets within the primary checksummed region â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
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

/// wMoney â€” 3 bytes BCD big-endian.  sym: 01:d84e â†’ sav 0x23DC
static constexpr uint32_t MONEY                    = 0x23DC;
static constexpr uint32_t MONEY_SIZE               = 3;

/// wMomsMoney â€” 3 bytes BCD big-endian.  sym: 01:d851 â†’ sav 0x23DF
static constexpr uint32_t MOMS_MONEY               = 0x23DF;
static constexpr uint32_t MOMS_MONEY_SIZE          = 3;

/// wMomSavingMoney â€” 1 byte flag.  sym: 01:d854 â†’ sav 0x23E2
static constexpr uint32_t MOM_SAVING_MONEY         = 0x23E2;

/// wCoins â€” 2 bytes big-endian (high byte first).  sym: 01:d855 â†’ sav 0x23E3
static constexpr uint32_t COINS                    = 0x23E3;
static constexpr uint32_t COINS_SIZE               = 2;

/// wJohtoBadges â€” 1 byte bitmask.  sym: 01:d857 â†’ sav 0x23E5
static constexpr uint32_t JOHTO_BADGES             = 0x23E5;

/// wKantoBadges â€” 1 byte bitmask.  sym: 01:d858 â†’ sav 0x23E6
static constexpr uint32_t KANTO_BADGES             = 0x23E6;

/// wEventFlags â€” 800 flags, 100 bytes bitfield.
/// sym: 01:da72 â†’ sav 0x2600
/// (Previously 0x2601 â€” corrected from sym file 2026-08.)
static constexpr uint32_t EVENT_FLAGS              = 0x2600;
static constexpr uint32_t EVENT_FLAGS_SIZE         = 100;  // ceil(800 / 8)
static constexpr uint32_t NUM_EVENT_FLAGS          = 800;

// â”€â”€â”€ Phase 2 field offsets â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
// All derived from WRAM symbols using the same formula:
//   sav_offset = PRIMARY_GAME_DATA + (wram_addr - wGameData_wram)
//   wGameData_wram = 0xD47B  (sym: 01:d47b)
//
// Fields marked OPTION are in the sOptions block (sav 0x2000..0x2008),
// not in the checksummed region.  They still live in the shadow and are
// read/written as owned fields but via the OPTIONS block path.

// â”€â”€ Player identity (inside checksummed region) â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

/// wPlayerID â€” 2 bytes u16 LE trainer ID.  sym: 01:d47b â†’ sav 0x2009
static constexpr uint32_t PLAYER_ID               = 0x2009;
static constexpr uint32_t PLAYER_ID_SIZE          = 2;

/// wPlayerName â€” 11 bytes Crystal charmap, 0x50-terminated.  sym: 01:d47d â†’ sav 0x200B
static constexpr uint32_t PLAYER_NAME             = 0x200B;
static constexpr uint32_t PLAYER_NAME_SIZE        = 11;  // NAME_LENGTH

/// wMomsName â€” 11 bytes Crystal charmap.  sym: 01:d488 â†’ sav 0x2016
static constexpr uint32_t MOMS_NAME               = 0x2016;
static constexpr uint32_t MOMS_NAME_SIZE          = 11;

/// wRivalName â€” 11 bytes Crystal charmap.  sym: 01:d493 â†’ sav 0x2021
static constexpr uint32_t RIVAL_NAME              = 0x2021;
static constexpr uint32_t RIVAL_NAME_SIZE         = 11;

/// wSecretID â€” 2 bytes u16 LE.  sym: 01:d84a â†’ sav 0x23D8
static constexpr uint32_t SECRET_ID               = 0x23D8;
static constexpr uint32_t SECRET_ID_SIZE          = 2;

// â”€â”€ Player gender (OPTIONS block â€” NOT in checksummed region) â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
// wPlayerGender is the first byte of wCrystalData which maps to sOptions (0x2000).
// Crystal: 0 = boy, 1 = girl.
// sym: 01:d472 â†’ wram 0xD472, which is sOptions[0] = sav 0x2000.
// This byte is mirrored to sBackupOptions[0] on save.
static constexpr uint32_t PLAYER_GENDER           = PRIMARY_OPTIONS;  // sav 0x2000

// â”€â”€ Game time (inside checksummed region) â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
// Crystal serialises game time in wPlayerData, so it's inside the checksummed block.

/// wGameTimeCap â€” 1 byte.  sym: 01:d4c3 â†’ sav 0x2051
/// Bit 7 set when playtime is capped (999 hours).
static constexpr uint32_t GAME_TIME_CAP           = 0x2051;

/// wGameTimeHours â€” 2 bytes BIG-ENDIAN u16.  sym: 01:d4c4 â†’ sav 0x2052
/// IMPORTANT: This field is BIG-ENDIAN (confirmed by suiCune serialize.c fix 2026-08).
/// High byte at [0x2052], low byte at [0x2053].
static constexpr uint32_t GAME_TIME_HOURS         = 0x2052;
static constexpr uint32_t GAME_TIME_HOURS_SIZE    = 2;

/// wGameTimeMinutes â€” 1 byte.  sym: 01:d4c6 â†’ sav 0x2054
static constexpr uint32_t GAME_TIME_MINUTES       = 0x2054;

/// wGameTimeSeconds â€” 1 byte.  sym: 01:d4c7 â†’ sav 0x2055
static constexpr uint32_t GAME_TIME_SECONDS       = 0x2055;

/// wGameTimeFrames â€” 1 byte.  sym: 01:d4c8 â†’ sav 0x2056
static constexpr uint32_t GAME_TIME_FRAMES        = 0x2056;

// â”€â”€ RTC / time context (inside checksummed region) â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

/// wStartDay â€” 1 byte.  Day when new game started.  sym: 01:d4b6 â†’ sav 0x2044
static constexpr uint32_t START_DAY               = 0x2044;

/// wStartHour â€” 1 byte.  sym: 01:d4b7 â†’ sav 0x2045
static constexpr uint32_t START_HOUR              = 0x2045;

/// wStartMinute â€” 1 byte.  sym: 01:d4b8 â†’ sav 0x2046
static constexpr uint32_t START_MINUTE            = 0x2046;

/// wStartSecond â€” 1 byte.  sym: 01:d4b9 â†’ sav 0x2047
static constexpr uint32_t START_SECOND            = 0x2047;

/// wRTC â€” 4 bytes: [day, hour, min, sec] hardware mirror.
/// sym: 01:d4ba â†’ sav 0x2048
static constexpr uint32_t RTC_BYTES               = 0x2048;
static constexpr uint32_t RTC_BYTES_SIZE          = 4;

/// wDST â€” 1 byte, bit 7 = DST active.  sym: 01:d4c2 â†’ sav 0x2050
static constexpr uint32_t DST                     = 0x2050;

/// wCurDay â€” 1 byte, current real-time day-of-week (0â€“6).
/// sym: 01:d4cb â†’ sav 0x2059
static constexpr uint32_t CUR_DAY                 = 0x2059;

// â”€â”€ Map position (inside checksummed region, in wCurMapData section) â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
// Note: these are in wCurMapData which begins after wPlayerDataEnd (sav 0x2833).

/// wWarpNumber â€” 1 byte, warp index within current map.  sym: 01:dcb4 â†’ sav 0x2842
static constexpr uint32_t WARP_NUMBER             = 0x2842;

/// wMapGroup â€” 1 byte.  sym: 01:dcb5 â†’ sav 0x2843
static constexpr uint32_t MAP_GROUP               = 0x2843;

/// wMapNumber â€” 1 byte.  sym: 01:dcb6 â†’ sav 0x2844
static constexpr uint32_t MAP_NUMBER              = 0x2844;

/// wYCoord â€” 1 byte (tile row).  sym: 01:dcb7 â†’ sav 0x2845
static constexpr uint32_t PLAYER_Y                = 0x2845;

/// wXCoord â€” 1 byte (tile column).  sym: 01:dcb8 â†’ sav 0x2846
static constexpr uint32_t PLAYER_X                = 0x2846;

// â”€â”€ Per-map scene state array (inside checksummed region) â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
// 79 scene slots, 1 byte each: sav 0x2500..0x254E.
// Each slot corresponds to a named map's scene state variable.
// Slot index = (wXxxSceneID offset - wPokecenter2FSceneID offset).
// sym first: wPokecenter2FSceneID â†’ 01:d972 â†’ sav 0x2500
// sym last:  wMobileBattleRoomSceneID â†’ 01:d9c0 â†’ sav 0x254E

static constexpr uint32_t SCENE_IDS_BASE          = 0x2500;
static constexpr uint32_t SCENE_IDS_COUNT         = 79;   // wPokecenter2F .. wMobileBattleRoom
static constexpr uint32_t SCENE_IDS_SIZE          = SCENE_IDS_COUNT;

/// Per-slot index for the scene ID slots.
/// scene_sav_offset(slot_i) = SCENE_IDS_BASE + slot_i
/// The mapping symbol â†’ slot_i is defined in the scene table below.

// â”€â”€â”€ Phase 2 compile-time assertions â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

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

// â”€â”€â”€ Backup mirror offsets â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
// Backup region mirrors primary with a fixed negative offset:
//   BACKUP_OFFSET = PRIMARY_GAME_DATA - BACKUP_GAME_DATA = 0x2009 - 0x1209 = 0xE00
//   backup_addr = primary_addr - BACKUP_OFFSET

static constexpr uint32_t BACKUP_OFFSET            = PRIMARY_GAME_DATA - BACKUP_GAME_DATA;

static constexpr uint32_t BACKUP_MONEY             = MONEY        - BACKUP_OFFSET;
static constexpr uint32_t BACKUP_MOMS_MONEY        = MOMS_MONEY   - BACKUP_OFFSET;
static constexpr uint32_t BACKUP_COINS             = COINS        - BACKUP_OFFSET;
static constexpr uint32_t BACKUP_EVENT_FLAGS       = EVENT_FLAGS  - BACKUP_OFFSET;

// â”€â”€â”€ Compile-time assertions â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

// Region sizes must match sym-derived values exactly.
static_assert(CHECKSUM_REGION_SIZE == 2938,
    "Checksum region must be exactly 2938 bytes (sGameDataEnd - sGameData)");
static_assert(BACKUP_CHECKSUM_END - BACKUP_GAME_DATA + 1 == CHECKSUM_REGION_SIZE,
    "Backup checksummed region must be the same size as primary");
static_assert(BACKUP_OFFSET == 0xE00,
    "Backup mirror offset must be 0xE00 (0x2009 - 0x1209)");

// Sym-pinned absolute values â€” these fail to compile if any constant drifts.
static_assert(PRIMARY_OPTIONS       == 0x2000, "sym: 01:a000");
static_assert(PRIMARY_CHECK_VALUE_1 == 0x2008, "sym: 01:a008");
static_assert(PRIMARY_GAME_DATA     == 0x2009, "sym: 01:a009");
static_assert(PRIMARY_CHECKSUM_END  == 0x2B82, "sym: sGameDataEnd 01:ab83 â†’ end = 0x2B82");
static_assert(PRIMARY_CHECKSUM      == 0x2D0D, "sym: 01:ad0d");
static_assert(PRIMARY_CHECK_VALUE_2 == 0x2D0F, "sym: 01:ad0f");
static_assert(BACKUP_OPTIONS        == 0x1200, "sym: 00:b200");
static_assert(BACKUP_CHECK_VALUE_1  == 0x1208, "sym: 00:b208");
static_assert(BACKUP_GAME_DATA      == 0x1209, "sym: 00:b209");
static_assert(BACKUP_CHECKSUM_END   == 0x1D82, "sym: sBackupGameDataEnd 00:bd83 â†’ end = 0x1D82");
static_assert(BACKUP_CHECKSUM       == 0x1F0D, "sym: 00:bf0d");
static_assert(BACKUP_CHECK_VALUE_2  == 0x1F0F, "sym: 00:bf0f");
static_assert(MONEY                 == 0x23DC, "sym: 01:d84e â†’ 0x2009 + (0xD84E - 0xD47B)");
static_assert(MOMS_MONEY            == 0x23DF, "sym: 01:d851 â†’ 0x2009 + (0xD851 - 0xD47B)");
static_assert(COINS                 == 0x23E3, "sym: 01:d855 â†’ 0x2009 + (0xD855 - 0xD47B)");
static_assert(EVENT_FLAGS           == 0x2600, "sym: 01:da72 â†’ 0x2009 + (0xDA72 - 0xD47B)");

// All owned fields must fall inside the checksummed region.
static_assert(MONEY       >= PRIMARY_GAME_DATA && MONEY       + MONEY_SIZE       - 1 <= PRIMARY_CHECKSUM_END, "");
static_assert(MOMS_MONEY  >= PRIMARY_GAME_DATA && MOMS_MONEY  + MOMS_MONEY_SIZE  - 1 <= PRIMARY_CHECKSUM_END, "");
static_assert(COINS       >= PRIMARY_GAME_DATA && COINS       + COINS_SIZE       - 1 <= PRIMARY_CHECKSUM_END, "");
static_assert(EVENT_FLAGS >= PRIMARY_GAME_DATA && EVENT_FLAGS + EVENT_FLAGS_SIZE - 1 <= PRIMARY_CHECKSUM_END, "");
static_assert(SRAM_SIZE   == 0x8000, "");

// â”€â”€â”€ Symbol-validation helper (runtime) â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
// Loads pokecrystal11.sym and verifies every sym-derived constant against the
// actual assembled addresses.  Called from the test suite to prove the constants
// remain correct if the sym file ever changes.
//
// Returns empty string on success.  Returns an error description on any mismatch.
// Skips silently if the sym file is not found (CI without references/).
[[nodiscard]] std::string validate_layout_against_sym(
    const std::filesystem::path& sym_file);

// â”€â”€â”€ Phase 3A: Party SRAM layout â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
//
// WRAM â†’ SRAM derivation: sav = PRIMARY_GAME_DATA + (wram - 0xD47B)
// Party section is in WRAMX (bank 01, wPokemonData == wPartyCount).
//
//   wPartyCount              sym: 01:dcd7  wram=0xDCD7  sav=0x2865
//   wPartySpecies            sym: 01:dcd8  wram=0xDCD8  sav=0x2866  (6 bytes)
//   wPartyEnd                sym: 01:dcde  wram=0xDCDE  sav=0x286C  (0xFF terminator)
//   wPartyMons/wPartyMon1    sym: 01:dcdf  wram=0xDCDF  sav=0x286D
//   wPartyMon2               sym: 01:dd0f  wram=0xDD0F  sav=0x289D  (+0x30 stride)
//   wPartyMon3..6            +0x30 each
//   wPartyMonOTs             sym: 01:ddff  wram=0xDDFF  sav=0x298D  (6Ã—11=66 bytes)
//   wPartyMonNicknames       sym: 01:de41  wram=0xDE41  sav=0x29CF  (6Ã—11=66 bytes)
//   wPartyMonNicknamesEnd    sym: 01:de83  wram=0xDE83  sav=0x2A11
//   [ds 22 padding before wPokedexCaught at 0x2A27]
//
// Source: macros/ram.asm, constants/pokemon_data_constants.asm,
//         constants/battle_constants.asm
//
//   PARTYMON_STRUCT_LENGTH = 48  BOXMON_STRUCT_LENGTH = 32
//   PARTY_LENGTH = 6  NUM_MOVES = 4
//   NAME_LENGTH = MON_NAME_LENGTH = 11
//
// party_struct field offsets (rsreset in pokemon_data_constants.asm):
//   +0  (1)  species
//   +1  (1)  held item
//   +2  (4)  moves[4]
//   +6  (2)  OT_ID              u16 LE
//   +8  (3)  exp                3-byte big-endian
//   +11 (2)  HP_EXP             u16 LE
//   +13 (2)  ATK_EXP            u16 LE
//   +15 (2)  DEF_EXP            u16 LE
//   +17 (2)  SPD_EXP            u16 LE
//   +19 (2)  SPC_EXP            u16 LE  (shared SpAtk+SpDef EXP)
//   +21 (2)  DVs                u16 LE; byte0=ATK|DEF nibbles, byte1=SPD|SPC nibbles
//   +23 (4)  PP[4]              bits 7-6=PP_UP count, bits 5-0=current PP
//   +27 (1)  happiness
//   +28 (1)  pokerus
//   +29 (1)  caught_time_level  bits7-6=time(0=unk,1=morn,2=day,3=nite), bits5-0=level
//   +30 (1)  caught_gender_loc  bit7=catcher_gender(1=boy), bits6-0=location_landmark
//   +31 (1)  level
//   --- BOXMON_STRUCT_LENGTH = 32 ---
//   +32 (1)  status
//   +33 (1)  unused (rb_skip)
//   +34 (2)  HP       big-endian  (current HP â€” NOT recomputed on load, preserve as-is)
//   +36 (2)  MaxHP    big-endian
//   +38 (2)  ATK      big-endian  (CalcMonStats: hMultiplicand[1]=high, [2]=low)
//   +40 (2)  DEF      big-endian
//   +42 (2)  SPD      big-endian
//   +44 (2)  SAT      big-endian
//   +46 (2)  SDF      big-endian
//   --- PARTYMON_STRUCT_LENGTH = 48 ---
//
// Stats are a Crystal-managed cache â€” recomputed from DVs+statEXP+level by
// CorrectPartyErrors on every load.  The codec preserves stored stats verbatim.
// Only current HP is authoritative runtime state that cannot be recomputed.

static constexpr uint32_t PARTY_LENGTH            = 6;
static constexpr uint32_t PARTYMON_STRUCT_LENGTH  = 48;
static constexpr uint32_t BOXMON_STRUCT_LENGTH    = 32;
static constexpr uint32_t NUM_MOVES               = 4;
static constexpr uint32_t OT_NAME_SIZE            = 11;   // NAME_LENGTH
static constexpr uint32_t NICKNAME_SIZE           = 11;   // MON_NAME_LENGTH

/// wPartyCount â€” 1 byte.  sym: 01:dcd7 â†’ sav 0x2865
static constexpr uint32_t PARTY_COUNT             = 0x2865;
/// wPartySpecies â€” 6 bytes.  sym: 01:dcd8 â†’ sav 0x2866
static constexpr uint32_t PARTY_SPECIES           = 0x2866;
/// wPartyEnd â€” 0xFF terminator.  sym: 01:dcde â†’ sav 0x286C
static constexpr uint32_t PARTY_END               = 0x286C;
/// wPartyMon1 â€” first party mon struct.  sym: 01:dcdf â†’ sav 0x286D
static constexpr uint32_t PARTY_MON_1             = 0x286D;
/// wPartyMonOTs â€” 6Ã—11 OT names.  sym: 01:ddff â†’ sav 0x298D
static constexpr uint32_t PARTY_OT_NAMES          = 0x298D;
/// wPartyMonNicknames â€” 6Ã—11 nicknames.  sym: 01:de41 â†’ sav 0x29CF
static constexpr uint32_t PARTY_NICKNAMES         = 0x29CF;
/// wPartyMonNicknamesEnd.  sym: 01:de83 â†’ sav 0x2A11
static constexpr uint32_t PARTY_NICKNAMES_END     = 0x2A11;

// Field offsets within a single party_struct (relative to struct base)
static constexpr uint32_t MON_OFF_SPECIES         = 0;
static constexpr uint32_t MON_OFF_ITEM            = 1;
static constexpr uint32_t MON_OFF_MOVES           = 2;
static constexpr uint32_t MON_OFF_OT_ID           = 6;
static constexpr uint32_t MON_OFF_EXP             = 8;
static constexpr uint32_t MON_OFF_HP_EXP          = 11;
static constexpr uint32_t MON_OFF_ATK_EXP         = 13;
static constexpr uint32_t MON_OFF_DEF_EXP         = 15;
static constexpr uint32_t MON_OFF_SPD_EXP         = 17;
static constexpr uint32_t MON_OFF_SPC_EXP         = 19;
static constexpr uint32_t MON_OFF_DVS             = 21;
static constexpr uint32_t MON_OFF_PP              = 23;
static constexpr uint32_t MON_OFF_HAPPINESS       = 27;
static constexpr uint32_t MON_OFF_POKERUS         = 28;
static constexpr uint32_t MON_OFF_CAUGHT_TIME_LVL = 29;
static constexpr uint32_t MON_OFF_CAUGHT_GND_LOC  = 30;
static constexpr uint32_t MON_OFF_LEVEL           = 31;
static constexpr uint32_t MON_OFF_STATUS          = 32;
static constexpr uint32_t MON_OFF_UNUSED          = 33;
static constexpr uint32_t MON_OFF_HP              = 34;
static constexpr uint32_t MON_OFF_MAXHP           = 36;
static constexpr uint32_t MON_OFF_ATK             = 38;
static constexpr uint32_t MON_OFF_DEF             = 40;
static constexpr uint32_t MON_OFF_SPD             = 42;
static constexpr uint32_t MON_OFF_SAT             = 44;
static constexpr uint32_t MON_OFF_SDF             = 46;

static constexpr uint8_t  PP_UP_MASK              = 0b11000000;
static constexpr uint8_t  PP_MASK                 = 0b00111111;
static constexpr uint8_t  CAUGHT_TIME_MASK        = 0b11000000;
static constexpr uint8_t  CAUGHT_LEVEL_MASK       = 0b00111111;
static constexpr uint8_t  CAUGHT_GENDER_MASK      = 0b10000000;
static constexpr uint8_t  CAUGHT_LOCATION_MASK    = 0b01111111;

// â”€â”€â”€ Phase 3A assertions â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
static_assert(PARTY_COUNT         == 0x2865, "sym: 01:dcd7");
static_assert(PARTY_MON_1         == 0x286D, "sym: 01:dcdf");
static_assert(PARTY_OT_NAMES      == 0x298D, "sym: 01:ddff");
static_assert(PARTY_NICKNAMES     == 0x29CF, "sym: 01:de41");
static_assert(PARTY_NICKNAMES_END == 0x2A11, "sym: 01:de83");
static_assert(PARTY_MON_1 + PARTY_LENGTH * PARTYMON_STRUCT_LENGTH == PARTY_OT_NAMES,
    "party mon block must abut OT names block");
static_assert(PARTY_OT_NAMES   + PARTY_LENGTH * OT_NAME_SIZE  == PARTY_NICKNAMES,
    "OT names block must abut nicknames block");
static_assert(PARTY_NICKNAMES  + PARTY_LENGTH * NICKNAME_SIZE  == PARTY_NICKNAMES_END,
    "nicknames block size must match");
static_assert(PARTY_COUNT      >= PRIMARY_GAME_DATA && PARTY_COUNT         <= PRIMARY_CHECKSUM_END, "");
static_assert(PARTY_NICKNAMES_END >  PRIMARY_GAME_DATA && PARTY_NICKNAMES_END <= PRIMARY_CHECKSUM_END, "");
static_assert(PARTYMON_STRUCT_LENGTH == 48, "");
static_assert(BOXMON_STRUCT_LENGTH   == 32, "");
static_assert(MON_OFF_LEVEL == BOXMON_STRUCT_LENGTH - 1, "level is last boxmon field");
static_assert(MON_OFF_SDF + 2 == PARTYMON_STRUCT_LENGTH, "SpDef closes party struct");

}  // namespace sram_layout
}  // namespace crystal
