#pragma once
// frontends/crystal/save/crystal_sram_layout.hpp
//
// Named SRAM offset constants for Pokémon Crystal (English).
// Source-verified against pret/pokecrystal ram/sram.asm and ram/wram.asm.
//
// RULES:
//   - No packed structs. Use sram.data[OFFSET] explicitly.
//   - All offsets are absolute within the 32 KB SRAM image (byte 0 = start of file).
//   - Crystal-specific knowledge stays in this file and frontends/crystal/save/ only.

#include <cstdint>
#include <cstddef>

namespace crystal {
namespace sram_layout {

// ─── SRAM image size ─────────────────────────────────────────────────────────

/// Exact size of the raw SRAM image. Emulator .sav files begin with this many
/// bytes followed by an optional trailer (RTC state, etc.).
static constexpr uint32_t SRAM_SIZE = 0x8000;  // 32 768 bytes

// ─── Save integrity constants ─────────────────────────────────────────────────
// Source: constants/misc_constants.asm — SAVE_CHECK_VALUE_1 / SAVE_CHECK_VALUE_2

static constexpr uint8_t SAVE_CHECK_VALUE_1 = 99;   // 0x63
static constexpr uint8_t SAVE_CHECK_VALUE_2 = 127;  // 0x7F

// ─── Primary save copy ────────────────────────────────────────────────────────
// Source: ram/sram.asm  SECTION "Save", SRAM

/// sOptions — 8 bytes, at absolute offset 0x2000.
static constexpr uint32_t PRIMARY_OPTIONS          = 0x2000;
static constexpr uint32_t PRIMARY_OPTIONS_SIZE     = 8;

/// sCheckValue1 — 1 byte sentinel, must equal SAVE_CHECK_VALUE_1.
static constexpr uint32_t PRIMARY_CHECK_VALUE_1    = 0x2008;

/// sGameData begins here — first byte of checksummed region.
static constexpr uint32_t PRIMARY_GAME_DATA        = 0x2009;

/// Last byte (inclusive) of checksummed region.
static constexpr uint32_t PRIMARY_CHECKSUM_END     = 0x2B82;

/// sChecksum — u16 LE, sum of bytes [PRIMARY_GAME_DATA .. PRIMARY_CHECKSUM_END].
static constexpr uint32_t PRIMARY_CHECKSUM         = 0x2D0D;

/// sCheckValue2 — 1 byte sentinel, must equal SAVE_CHECK_VALUE_2.
static constexpr uint32_t PRIMARY_CHECK_VALUE_2    = 0x2D0F;

/// Size of the checksummed region in bytes.
static constexpr uint32_t CHECKSUM_REGION_SIZE     = PRIMARY_CHECKSUM_END - PRIMARY_GAME_DATA + 1;
// = 0x2B82 - 0x2009 + 1 = 0xB7A = 2938

// ─── Backup save copy ─────────────────────────────────────────────────────────
// Source: ram/sram.asm  SECTION "Backup Save", SRAM

/// sBackupOptions — 8 bytes, at absolute offset 0x1200.
static constexpr uint32_t BACKUP_OPTIONS           = 0x1200;

/// sBackupCheckValue1 — 1 byte sentinel.
static constexpr uint32_t BACKUP_CHECK_VALUE_1     = 0x1208;

/// sBackupGameData begins here — first byte of backup checksummed region.
static constexpr uint32_t BACKUP_GAME_DATA         = 0x1209;

/// Last byte (inclusive) of backup checksummed region.
static constexpr uint32_t BACKUP_CHECKSUM_END      = 0x1D82;

/// sBackupChecksum — u16 LE.
static constexpr uint32_t BACKUP_CHECKSUM          = 0x1F0D;

/// sBackupCheckValue2 — 1 byte sentinel.
static constexpr uint32_t BACKUP_CHECK_VALUE_2     = 0x1F0F;

// ─── Key fields within the primary checksummed region ────────────────────────
// Offsets are absolute within the 32 KB image.
// Source: ram/wram.asm  wPlayerData section, mapped to sPlayerData.

/// wMoney — 3 bytes BCD big-endian.  Max 999 999.
static constexpr uint32_t MONEY                    = 0x23DC;
static constexpr uint32_t MONEY_SIZE               = 3;

/// wMomsMoney — 3 bytes BCD big-endian.
static constexpr uint32_t MOMS_MONEY               = 0x23DF;
static constexpr uint32_t MOMS_MONEY_SIZE          = 3;

/// wMomSavingMoney — 1 byte flag (bit 7 = active; bit 0 = saving some).
static constexpr uint32_t MOM_SAVING_MONEY         = 0x23E2;

/// wCoins — 2 bytes big-endian.  Max 9 999.
static constexpr uint32_t COINS                    = 0x23E3;
static constexpr uint32_t COINS_SIZE               = 2;

/// wJohtoBadges — 1 byte bitmask (bit 7 = Zephyr … bit 0 = Rising).
static constexpr uint32_t JOHTO_BADGES             = 0x23E5;

/// wKantoBadges — 1 byte bitmask.
static constexpr uint32_t KANTO_BADGES             = 0x23E6;

/// wEventFlags — 800 flags, 100 bytes bitfield.
/// Source-verified from wram.asm by counting wMoney→wEventFlags forward.
static constexpr uint32_t EVENT_FLAGS              = 0x2601;
static constexpr uint32_t EVENT_FLAGS_SIZE         = 100;  // ceil(800 / 8)
static constexpr uint32_t NUM_EVENT_FLAGS          = 800;

// ─── Backup mirror offsets ────────────────────────────────────────────────────
// The backup region mirrors the primary with a fixed offset of (PRIMARY - BACKUP)
// applied to every field address.
//
//   backup_offset = primary_offset - (PRIMARY_GAME_DATA - BACKUP_GAME_DATA)
//   PRIMARY_GAME_DATA - BACKUP_GAME_DATA = 0x2009 - 0x1209 = 0xE00
//
// Usage:  backup_addr = primary_addr - BACKUP_OFFSET

static constexpr uint32_t BACKUP_OFFSET            = PRIMARY_GAME_DATA - BACKUP_GAME_DATA;
// = 0x2009 - 0x1209 = 0xE00

static constexpr uint32_t BACKUP_MONEY             = MONEY        - BACKUP_OFFSET;
static constexpr uint32_t BACKUP_MOMS_MONEY        = MOMS_MONEY   - BACKUP_OFFSET;
static constexpr uint32_t BACKUP_COINS             = COINS        - BACKUP_OFFSET;
static constexpr uint32_t BACKUP_EVENT_FLAGS       = EVENT_FLAGS  - BACKUP_OFFSET;

// ─── Compile-time sanity checks ───────────────────────────────────────────────
static_assert(CHECKSUM_REGION_SIZE == 2938,
    "Primary checksummed region must be exactly 2938 bytes");
static_assert(BACKUP_CHECKSUM_END - BACKUP_GAME_DATA + 1 == CHECKSUM_REGION_SIZE,
    "Backup checksummed region must be the same size as primary");
static_assert(BACKUP_OFFSET == 0xE00,
    "Backup mirror offset must be 0xE00");
static_assert(EVENT_FLAGS + EVENT_FLAGS_SIZE - 1 <= PRIMARY_CHECKSUM_END,
    "wEventFlags must fall inside checksummed region");
static_assert(MONEY          >= PRIMARY_GAME_DATA && MONEY          <= PRIMARY_CHECKSUM_END, "");
static_assert(MOMS_MONEY     >= PRIMARY_GAME_DATA && MOMS_MONEY     <= PRIMARY_CHECKSUM_END, "");
static_assert(COINS          >= PRIMARY_GAME_DATA && COINS          <= PRIMARY_CHECKSUM_END, "");
static_assert(EVENT_FLAGS    >= PRIMARY_GAME_DATA && EVENT_FLAGS    <= PRIMARY_CHECKSUM_END, "");
static_assert(SRAM_SIZE == 0x8000, "");

}  // namespace sram_layout
}  // namespace crystal
