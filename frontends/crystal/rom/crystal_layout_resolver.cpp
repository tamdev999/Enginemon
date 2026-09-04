// frontends/crystal/rom/crystal_layout_resolver.cpp
//
// Crystal-family generic table/routine discovery.
//
// Evidence classification for each resolver:
//   XREF:       the SM83 opcode pattern that encodes the table address
//   STRUCTURAL: the format invariants used to validate the resolved address
//   CROSS:      consistency against another already-resolved structure
//
// Content anchors (specific game data values like Pound's power, exact type
// names, or specific WRAM addresses) are intentionally absent from the primary
// evidence chain.  Where a content value appears as a secondary sanity check
// it is marked HINT-ONLY and can never alone define a table bound.

#include "crystal/rom/crystal_layout_resolver.hpp"
#include <cstdio>
#include <format>
#include <vector>
#include <array>

namespace crystal {

// ============================================================================
// Internal helpers
// ============================================================================

namespace {

// Read a little-endian 16-bit word from raw ROM bytes, bounds-checked.
inline uint16_t read16(const RomData& rom, uint32_t off) {
    if (off + 2u > rom.size()) return 0xFFFF;
    return static_cast<uint16_t>(rom.read_byte(off))
         | (static_cast<uint16_t>(rom.read_byte(off + 1u)) << 8);
}

// Resolve a bank:ptr pair to a flat address (Crystal banking).
inline uint32_t flat_of(uint8_t bank, uint16_t ptr) {
    if (ptr < 0x4000u) return ptr;                            // home bank
    return static_cast<uint32_t>(bank) * 0x4000u + (ptr - 0x4000u);
}

// Determine the bank for a flat address (inverse of bank_to_flat).
inline uint8_t bank_of(uint32_t flat) {
    return static_cast<uint8_t>(flat / 0x4000u);
}

// Check whether a pointer value is in the valid switchable-bank window.
inline bool valid_banked_ptr(uint16_t ptr) {
    return ptr >= 0x4000u && ptr <= 0x7FFFu;
}

// Check whether a pointer is either banked or in home-bank range.
inline bool valid_any_ptr(uint16_t ptr) {
    return ptr <= 0x7FFFu;  // home bank [0,0x3FFF] or switchable [0x4000,0x7FFF]
}

// Validate that at least min_entries valid dba (3-byte bank+ptr) records exist
// at flat_addr, where each record has bank in [1,127] and ptr in [0x4000,0x7FFF].
// Returns the number of valid consecutive entries.
uint32_t count_dba_entries(const RomData& rom, uint32_t flat_addr, uint32_t max_check = 256) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < max_check; ++i) {
        uint32_t off = flat_addr + i * 3u;
        if (off + 3u > rom.size()) break;
        uint8_t  bk = rom.read_byte(off);
        uint16_t pt = read16(rom, off + 1u);
        if (bk < 1u || bk >= 128u || !valid_banked_ptr(pt)) break;
        ++n;
    }
    return n;
}

// Count consecutive valid 2-byte bank-local pointers at flat_addr.
uint32_t count_dw_entries(const RomData& rom, uint32_t flat_addr, uint32_t max_check = 512) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < max_check; ++i) {
        uint32_t off = flat_addr + i * 2u;
        if (off + 2u > rom.size()) break;
        uint16_t pt = read16(rom, off);
        if (!valid_banked_ptr(pt)) break;
        ++n;
    }
    return n;
}

// Validate a TypeMatchups run starting at flat_addr.
// Accepts multiplier set = {0,5,8,10,16,20,32} (union of vanilla and Polished).
// Returns {entry_count, found_sentinel}.
std::pair<uint32_t,bool> scan_type_matchups(const RomData& rom, uint32_t flat_addr,
                                             uint32_t max_type_id = 0x3Fu) {
    uint32_t cnt = 0; bool sentinel = false;
    uint32_t p = flat_addr;
    for (uint32_t i = 0; i < 2048u && p + 3u <= rom.size(); ++i) {
        uint8_t a = rom.read_byte(p);
        if (a == 0xFF) { sentinel = true; break; }
        if (a == 0xFE) { p += 1; continue; }   // Gen2 separator
        uint8_t d = rom.read_byte(p + 1u);
        uint8_t m = rom.read_byte(p + 2u);
        if (a > max_type_id || d > max_type_id) break;
        // Multiplier set: union of {0,5,20} (vanilla) and {0,8,16,32} (Polished).
        // This is STRUCTURAL — the value set is format-defined, not game-content.
        static const std::array<uint8_t,7> valid_mults = {0,5,8,10,16,20,32};
        bool ok = false;
        for (uint8_t vm : valid_mults) { if (m == vm) { ok = true; break; } }
        if (!ok) break;
        ++cnt; p += 3u;
    }
    return {cnt, sentinel};
}

// Validate BaseData records starting at flat_addr with given record_size.
// Stops when hp==0 OR type1>0x3F OR type2>0x3F (STRUCTURAL termination).
// NOTE: type_id<=0x3F is used as a termination hint, not a hard rule.
//       It is generous enough for all known expansions (Fairy=0x1C, etc.)
//       and is labeled as HINT-ONLY — it cannot be used to define exact count.
uint32_t count_base_data_records(const RomData& rom, uint32_t flat_addr,
                                  uint8_t record_size, uint8_t type1_off, uint8_t type2_off,
                                  uint8_t hp_off, uint32_t max_records = 512) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < max_records; ++i) {
        uint32_t off = flat_addr + i * record_size;
        if (off + record_size > rom.size()) break;
        uint8_t hp = rom.read_byte(off + hp_off);
        uint8_t t1 = rom.read_byte(off + type1_off);
        uint8_t t2 = rom.read_byte(off + type2_off);
        // HINT-ONLY: type <=0x3F and hp>0 as structural termination hints.
        if (hp == 0 || t1 > 0x3Fu || t2 > 0x3Fu) break;
        ++n;
    }
    return n;
}

// Validate a Moves run starting at flat_addr with given record_size.
// HINT-ONLY: type_id<=0x3F as termination hint.
uint32_t count_move_records(const RomData& rom, uint32_t flat_addr,
                             uint8_t record_size, uint8_t type_off,
                             uint32_t max_records = 512) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < max_records; ++i) {
        uint32_t off = flat_addr + i * record_size;
        if (off + record_size > rom.size()) break;
        uint8_t t = rom.read_byte(off + type_off);
        if (t > 0x3Fu) break;  // HINT-ONLY termination
        ++n;
    }
    return n;
}

}  // anonymous namespace


// ============================================================================
// resolve_std_scripts
//
// XREF pattern: StdScript dispatch routine.
// Source (home/scripting.asm or engine/overworld/scripting.asm):
//   ld e, a           ; 5F
//   ld d, 0           ; 16 00
//   ld hl, StdScripts ; 21 lo hi     ← StdScripts table address
//   add hl, de        ; 19
//   add hl, de        ; 19           ← double for 2-byte stride (dw variant)
//   ld b, BANK(...)   ; 06 bb        ← bb=0 if dw (same bank), else bank number
//
// Vanilla: 5F 16 00 21 lo hi 19 19 06 bb   with bb = BANK(StdScripts)
// Polished: same bytes but bb=0x2F (still bank 0x2F, ptr 0x4000)
//
// The table format is determined by bb:
//   bb > 0  AND ptr matches table bank → 3-byte dba entries (bank+dw)
//   bb == bank_of(ptr site)            → may be 2-byte dw (all same bank)
//   Distinguisher: at the resolved flat, count_dba_entries vs count_dw_entries
// ============================================================================

ResolvedAddress resolve_std_scripts(
    const RomData& rom,
    uint32_t profile_address,
    uint8_t* out_entry_size,
    std::string* out_diagnostic)
{
    // Try profile address first if set
    if (profile_address != 0 && profile_address < rom.size()) {
        // Determine entry format: probe dba vs dw
        uint32_t dba_cnt = count_dba_entries(rom, profile_address, 200);
        uint32_t dw_cnt  = count_dw_entries(rom, profile_address, 200);
        // Choose whichever gives more entries (tie: prefer dba, vanilla default)
        if (dba_cnt >= 5 || dw_cnt >= 5) {
            uint8_t esz = (dw_cnt > dba_cnt) ? 2 : 3;
            if (out_entry_size) *out_entry_size = esz;
            return { profile_address,
                     std::format("profile address 0x{:05X} validated ({}-byte entries, {} valid)",
                                 profile_address, esz, std::max(dba_cnt, dw_cnt)) };
        }
    }

    // XREF scan: 5F 16 00 21 lo hi 19 19 06 bb
    constexpr uint8_t P0 = 0x5F; // ld e, a
    constexpr uint8_t P1 = 0x16; // ld d, n
    constexpr uint8_t P2 = 0x00; //   n=0
    constexpr uint8_t P3 = 0x21; // ld hl, nn
    // bytes [4],[5] = lo, hi
    constexpr uint8_t P6 = 0x19; // add hl, de
    constexpr uint8_t P7 = 0x19; // add hl, de
    constexpr uint8_t P8 = 0x06; // ld b, n
    // byte [9] = bank byte

    std::vector<uint32_t> candidates;
    const uint32_t limit = (rom.size() >= 10) ? static_cast<uint32_t>(rom.size()) - 10u : 0u;
    for (uint32_t i = 0; i < limit; ++i) {
        if (rom.read_byte(i)   != P0) continue;
        if (rom.read_byte(i+1) != P1) continue;
        if (rom.read_byte(i+2) != P2) continue;
        if (rom.read_byte(i+3) != P3) continue;
        if (rom.read_byte(i+6) != P6) continue;
        if (rom.read_byte(i+7) != P7) continue;
        if (rom.read_byte(i+8) != P8) continue;
        uint8_t  lo = rom.read_byte(i+4);
        uint8_t  hi = rom.read_byte(i+5);
        uint8_t  bb = rom.read_byte(i+9);
        uint16_t ptr = static_cast<uint16_t>(lo) | (static_cast<uint16_t>(hi) << 8);
        if (!valid_any_ptr(ptr)) continue;
        uint32_t tbl_flat = flat_of(bb ? bb : bank_of(i), ptr);
        if (tbl_flat >= rom.size()) continue;
        // Validate: need at least 5 valid entries in either format
        uint32_t dba_cnt = count_dba_entries(rom, tbl_flat, 150);
        uint32_t dw_cnt  = count_dw_entries(rom, tbl_flat, 150);
        if (dba_cnt < 5 && dw_cnt < 5) continue;
        candidates.push_back(tbl_flat);
    }

    if (candidates.size() == 1) {
        uint32_t tbl = candidates[0];
        uint32_t dba_cnt = count_dba_entries(rom, tbl, 150);
        uint32_t dw_cnt  = count_dw_entries(rom, tbl, 150);
        uint8_t esz = (dw_cnt > dba_cnt) ? 2 : 3;
        if (out_entry_size) *out_entry_size = esz;
        return { tbl, std::format("XREF scan → 0x{:05X} ({}-byte entries, {} valid)",
                                   tbl, esz, std::max(dba_cnt, dw_cnt)) };
    }
    if (candidates.size() > 1) {
        if (out_diagnostic) {
            *out_diagnostic = std::format("StdScripts: {} candidates found by XREF scan — "
                                          "ambiguous; profile address required", candidates.size());
        }
        return { 0, "", true };
    }
    if (out_diagnostic) {
        *out_diagnostic = "StdScripts: XREF pattern not found in ROM";
    }
    return {};
}


// ============================================================================
// resolve_base_data
//
// XREF pattern: _GetBaseData (home/pokemon.asm).
// Source:
//   ld a, BASE_DATA_SIZE   ; 3E sz
//   ld hl, BaseData        ; 21 lo hi    ← table address
//   rst AddNTimes          ; [D7|DF|E7]  (varies by ROM)
//   ld de, wCurBaseData    ; 11 wl wh    (WRAM address in [0xC000,0xDFFF])
//   ld bc, BASE_DATA_SIZE  ; 01 sz 00
//   ld a, BANK(BaseData)   ; 3E bb       ← bank
//   call FarCopyBytes      ; CD ?? ??
//
// Pattern: 3E sz 21 lo hi [D7|DF|E7] 11 wl wh 01 sz 00 3E bb CD
// ============================================================================

ResolvedAddress resolve_base_data(
    const RomData& rom,
    uint32_t profile_address,
    uint8_t* out_record_size,
    std::string* out_diagnostic)
{
    if (profile_address != 0 && profile_address < rom.size()) {
        const auto& fmt = ExtractionProfile{}.format.pokemon; // defaults
        uint32_t cnt = count_base_data_records(rom, profile_address,
                           fmt.base_data_size,
                           fmt.type1_offset, fmt.type2_offset,
                           fmt.hp_offset, 512);
        if (cnt >= 10) {
            if (out_record_size) *out_record_size = fmt.base_data_size;
            return { profile_address,
                     std::format("profile address 0x{:05X} validated ({} records)", profile_address, cnt) };
        }
    }

    // XREF scan: 3E sz 21 lo hi rst 11 wl wh 01 sz 00 3E bb CD
    // rst opcodes used for AddNTimes in Crystal family: 0xD7, 0xDF, 0xE7
    static const uint8_t rst_opcodes[] = {0xD7, 0xDF, 0xE7};

    std::vector<std::pair<uint32_t,uint8_t>> candidates; // {flat, record_size}
    const uint32_t limit = (rom.size() >= 16) ? static_cast<uint32_t>(rom.size()) - 16u : 0u;

    for (uint32_t i = 0; i < limit; ++i) {
        if (rom.read_byte(i) != 0x3E) continue;  // ld a, sz
        uint8_t sz = rom.read_byte(i+1);
        if (sz < 0x10 || sz > 0x60) continue;    // BASE_DATA_SIZE in [16,96]
        if (rom.read_byte(i+2) != 0x21) continue; // ld hl, nn
        uint8_t lo = rom.read_byte(i+3);
        uint8_t hi = rom.read_byte(i+4);
        uint16_t ptr = static_cast<uint16_t>(lo) | (static_cast<uint16_t>(hi) << 8);
        if (!valid_any_ptr(ptr)) continue;
        // Check rst opcode at byte 5
        uint8_t rst = rom.read_byte(i+5);
        bool rst_ok = false;
        for (uint8_t r : rst_opcodes) { if (rst == r) { rst_ok = true; break; } }
        if (!rst_ok) continue;
        if (rom.read_byte(i+6) != 0x11) continue; // ld de, nn
        // wCurBaseData at bytes [7,8] — must be in WRAM [0xC000,0xDFFF]
        uint16_t wram = read16(rom, i+7);
        if (wram < 0xC000u || wram > 0xDFFFu) continue;
        if (rom.read_byte(i+9) != 0x01) continue;  // ld bc, nn
        if (rom.read_byte(i+10) != sz)   continue;  // size matches
        if (rom.read_byte(i+11) != 0x00) continue;  // bc_hi = 0
        if (rom.read_byte(i+12) != 0x3E) continue;  // ld a, BANK
        uint8_t bb = rom.read_byte(i+13);
        if (bb == 0 || bb >= 128u) continue;
        if (rom.read_byte(i+14) != 0xCD) continue;  // call
        // Resolve table
        uint32_t tbl_flat = flat_of(bb, ptr);
        if (tbl_flat >= rom.size()) continue;
        // Structural validation: count records
        // Use field offsets from profile defaults (type1=7,type2=8,hp=1 for sz=32)
        // Adjust for larger record sizes by keeping same offsets (always at 7,8,1)
        uint32_t cnt = count_base_data_records(rom, tbl_flat, sz, 7, 8, 1, 512);
        if (cnt < 10) continue;
        candidates.push_back({tbl_flat, sz});
    }

    if (candidates.size() == 1) {
        if (out_record_size) *out_record_size = candidates[0].second;
        return { candidates[0].first,
                 std::format("XREF scan → 0x{:05X} (record_size={}, {} records)",
                             candidates[0].first, candidates[0].second,
                             count_base_data_records(rom, candidates[0].first,
                                 candidates[0].second, 7, 8, 1, 512)) };
    }
    if (candidates.size() > 1) {
        if (out_diagnostic) {
            *out_diagnostic = std::format("BaseData: {} candidates — ambiguous; "
                                          "profile address required", candidates.size());
        }
        return { 0, "", true };
    }
    if (out_diagnostic) {
        *out_diagnostic = "BaseData: XREF pattern not found in ROM";
    }
    return {};
}


// ============================================================================
// resolve_moves
//
// XREF pattern: GetFixedMoveStruct (home/battle.asm or similar).
// Source:
//   dec a              ; 3D
//   ld hl, Moves       ; 21 lo hi    ← table address
//   ld bc, MOVE_LENGTH ; 01 sz 00
//   rst AddNTimes      ; [D7|DF|E7]
//   ld a, BANK(Moves)  ; 3E bb       ← bank
//   [call/jmp FarCopyBytes] ; CD|C3|CF...
//
// Pattern: 3D 21 lo hi 01 sz 00 [D7|DF|E7] 3E bb [CD|C3|CF]
// ============================================================================

ResolvedAddress resolve_moves(
    const RomData& rom,
    uint32_t profile_address,
    uint8_t* out_record_size,
    std::string* out_diagnostic)
{
    if (profile_address != 0 && profile_address < rom.size()) {
        const auto& fmt = ExtractionProfile{}.format.move; // defaults
        uint32_t cnt = count_move_records(rom, profile_address,
                           fmt.move_data_size, fmt.type_offset, 512);
        if (cnt >= 20) {
            if (out_record_size) *out_record_size = fmt.move_data_size;
            return { profile_address,
                     std::format("profile address 0x{:05X} validated ({} records)", profile_address, cnt) };
        }
    }

    static const uint8_t rst_opcodes[] = {0xD7, 0xDF, 0xE7};

    std::vector<std::pair<uint32_t,uint8_t>> candidates;
    const uint32_t limit = (rom.size() >= 12) ? static_cast<uint32_t>(rom.size()) - 12u : 0u;

    for (uint32_t i = 0; i < limit; ++i) {
        if (rom.read_byte(i) != 0x3D) continue;   // dec a
        if (rom.read_byte(i+1) != 0x21) continue;  // ld hl, nn
        uint8_t lo = rom.read_byte(i+2);
        uint8_t hi = rom.read_byte(i+3);
        uint16_t ptr = static_cast<uint16_t>(lo) | (static_cast<uint16_t>(hi) << 8);
        if (!valid_any_ptr(ptr)) continue;
        if (rom.read_byte(i+4) != 0x01) continue;  // ld bc, nn
        uint8_t sz = rom.read_byte(i+5);
        if (sz < 6u || sz > 16u) continue;          // MOVE_LENGTH in [6,16]
        if (rom.read_byte(i+6) != 0x00) continue;  // bc_hi = 0
        uint8_t rst = rom.read_byte(i+7);
        bool rst_ok = false;
        for (uint8_t r : rst_opcodes) { if (rst == r) { rst_ok = true; break; } }
        if (!rst_ok) continue;
        if (rom.read_byte(i+8) != 0x3E) continue;  // ld a, BANK
        uint8_t bb = rom.read_byte(i+9);
        if (bb == 0 || bb >= 128u) continue;
        // byte 10: call/jmp opcode — accept CD (call), CF (rst $08 = rst 8), C3 (jp)
        uint8_t next_op = rom.read_byte(i+10);
        if (next_op != 0xCD && next_op != 0xCF && next_op != 0xC3) continue;
        uint32_t tbl_flat = flat_of(bb, ptr);
        if (tbl_flat >= rom.size()) continue;
        // type_offset=3 for vanilla 7-byte record; but we don't know exact offset.
        // Use MOVE_TYPE at byte 3 for record sizes 7-8, which covers vanilla+Polished.
        // HINT-ONLY: type byte at offset 3 <= 0x3F as termination.
        uint8_t type_off = (sz >= 7u) ? 3u : 2u;
        uint32_t cnt = count_move_records(rom, tbl_flat, sz, type_off, 512);
        if (cnt < 20) continue;
        candidates.push_back({tbl_flat, sz});
    }

    if (candidates.size() == 1) {
        if (out_record_size) *out_record_size = candidates[0].second;
        return { candidates[0].first,
                 std::format("XREF scan → 0x{:05X} (record_size={}, {} records)",
                             candidates[0].first, candidates[0].second,
                             count_move_records(rom, candidates[0].first,
                                 candidates[0].second,
                                 (candidates[0].second >= 7u) ? 3u : 2u, 512)) };
    }
    if (candidates.size() > 1) {
        if (out_diagnostic) {
            *out_diagnostic = std::format("Moves: {} candidates — ambiguous; "
                                          "profile address required", candidates.size());
        }
        return { 0, "", true };
    }
    if (out_diagnostic) {
        *out_diagnostic = "Moves: XREF pattern not found in ROM";
    }
    return {};
}


// ============================================================================
// resolve_trainer_groups
//
// XREF pattern: RandomPhoneMon (engine/overworld/wildmons.asm).
// Source:
//   ld hl, TrainerGroups   ; 21 lo hi
//   ld a, d                ; 7A
//   dec a                  ; 3D
//   ld c, a                ; 4F
//   ld b, 0                ; 06 00
//   add hl, bc             ; 09
//   add hl, bc             ; 09
//   add hl, bc             ; 09            ← ×3 for 3-byte dba stride
//   ld a, BANK(TrainerGroups) ; 3E bb
//
// Pattern: 21 lo hi 7A 3D 4F 06 00 09 09 09 3E bb
// ============================================================================

ResolvedAddress resolve_trainer_groups(
    const RomData& rom,
    uint32_t profile_address,
    std::string* out_diagnostic)
{
    if (profile_address != 0 && profile_address < rom.size()) {
        uint32_t cnt = count_dba_entries(rom, profile_address, 256);
        if (cnt >= 10) {
            return { profile_address,
                     std::format("profile address 0x{:05X} validated ({} dba entries)", profile_address, cnt) };
        }
    }

    std::vector<uint32_t> candidates;
    const uint32_t limit = (rom.size() >= 14) ? static_cast<uint32_t>(rom.size()) - 14u : 0u;

    for (uint32_t i = 0; i < limit; ++i) {
        if (rom.read_byte(i)    != 0x21) continue; // ld hl, nn
        if (rom.read_byte(i+3)  != 0x7A) continue; // ld a, d
        if (rom.read_byte(i+4)  != 0x3D) continue; // dec a
        if (rom.read_byte(i+5)  != 0x4F) continue; // ld c, a
        if (rom.read_byte(i+6)  != 0x06) continue; // ld b, n
        if (rom.read_byte(i+7)  != 0x00) continue; //   n=0
        if (rom.read_byte(i+8)  != 0x09) continue; // add hl, bc
        if (rom.read_byte(i+9)  != 0x09) continue; // add hl, bc
        if (rom.read_byte(i+10) != 0x09) continue; // add hl, bc
        if (rom.read_byte(i+11) != 0x3E) continue; // ld a, BANK
        uint8_t lo = rom.read_byte(i+1);
        uint8_t hi = rom.read_byte(i+2);
        uint8_t bb = rom.read_byte(i+12);
        uint16_t ptr = static_cast<uint16_t>(lo) | (static_cast<uint16_t>(hi) << 8);
        if (!valid_any_ptr(ptr)) continue;
        if (bb == 0 || bb >= 128u) continue;
        uint32_t tbl_flat = flat_of(bb, ptr);
        if (tbl_flat >= rom.size()) continue;
        uint32_t cnt = count_dba_entries(rom, tbl_flat, 256);
        if (cnt < 10) continue;
        candidates.push_back(tbl_flat);
    }

    if (candidates.size() == 1) {
        return { candidates[0],
                 std::format("XREF scan → 0x{:05X} ({} dba entries)",
                             candidates[0], count_dba_entries(rom, candidates[0], 256)) };
    }
    if (candidates.size() > 1) {
        if (out_diagnostic) {
            *out_diagnostic = std::format("TrainerGroups: {} candidates — ambiguous", candidates.size());
        }
        return { 0, "", true };
    }
    if (out_diagnostic) {
        *out_diagnostic = "TrainerGroups: XREF pattern not found in ROM";
    }
    return {};
}


// ============================================================================
// resolve_type_matchups
//
// XREF pattern: Type-effectiveness branch (engine/battle/effect_commands.asm).
// Source:
//   ld hl, InverseTypeMatchups ; 21 i_lo i_hi
//   ld a, [wBattleType]        ; FA xx xx
//   cp BATTLETYPE_INVERSE      ; FE nn
//   jr z, .TypesLoop           ; 28 xx
//   ld hl, TypeMatchups        ; 21 t_lo t_hi  ← table address
//  .TypesLoop:
//   ld a, [hli]                ; 2A
//   cp $ff                     ; FE FF          ← sentinel check
//
// Pattern: 21 i_lo i_hi FA ?? ?? FE ?? 28 ?? 21 t_lo t_hi 2A FE FF
//
// Multiplier validation uses {0,5,8,10,16,20,32} — union of vanilla and
// Polished Crystal — so this is generic across Crystal-family ROMs.
// ============================================================================

ResolvedAddress resolve_type_matchups(
    const RomData& rom,
    uint32_t profile_address,
    uint32_t* out_inverse_flat,
    std::string* out_diagnostic)
{
    if (profile_address != 0 && profile_address < rom.size()) {
        auto [cnt, sentinel] = scan_type_matchups(rom, profile_address);
        if (sentinel && cnt >= 30) {
            return { profile_address,
                     std::format("profile address 0x{:05X} validated ({} entries)", profile_address, cnt) };
        }
    }

    std::vector<std::pair<uint32_t,uint32_t>> candidates; // {tm_flat, inv_flat}
    const uint32_t limit = (rom.size() >= 16) ? static_cast<uint32_t>(rom.size()) - 16u : 0u;

    for (uint32_t i = 0; i < limit; ++i) {
        if (rom.read_byte(i)    != 0x21) continue; // ld hl, InvTM
        if (rom.read_byte(i+3)  != 0xFA) continue; // ld a, [nn]
        if (rom.read_byte(i+6)  != 0xFE) continue; // cp n
        if (rom.read_byte(i+8)  != 0x28) continue; // jr z
        if (rom.read_byte(i+10) != 0x21) continue; // ld hl, TypeMatchups
        if (rom.read_byte(i+13) != 0x2A) continue; // ld a, [hli]
        if (rom.read_byte(i+14) != 0xFE) continue; // cp $ff (sentinel check)
        if (rom.read_byte(i+15) != 0xFF) continue;

        uint8_t i_lo = rom.read_byte(i+1);
        uint8_t i_hi = rom.read_byte(i+2);
        uint8_t t_lo = rom.read_byte(i+11);
        uint8_t t_hi = rom.read_byte(i+12);
        uint16_t inv_ptr = static_cast<uint16_t>(i_lo) | (static_cast<uint16_t>(i_hi) << 8);
        uint16_t tm_ptr  = static_cast<uint16_t>(t_lo) | (static_cast<uint16_t>(t_hi) << 8);
        if (!valid_any_ptr(tm_ptr) || !valid_any_ptr(inv_ptr)) continue;

        uint8_t caller_bank = bank_of(i);
        uint32_t tm_flat  = flat_of(caller_bank, tm_ptr);
        uint32_t inv_flat = flat_of(caller_bank, inv_ptr);

        if (tm_flat >= rom.size()) continue;

        auto [cnt, sentinel] = scan_type_matchups(rom, tm_flat);
        if (!sentinel || cnt < 30) continue;

        candidates.push_back({tm_flat, inv_flat});
    }

    if (candidates.size() == 1) {
        if (out_inverse_flat) *out_inverse_flat = candidates[0].second;
        auto [cnt, _] = scan_type_matchups(rom, candidates[0].first);
        return { candidates[0].first,
                 std::format("XREF scan → 0x{:05X} ({} entries)", candidates[0].first, cnt) };
    }
    if (candidates.size() > 1) {
        if (out_diagnostic) {
            *out_diagnostic = std::format("TypeMatchups: {} candidates — ambiguous", candidates.size());
        }
        return { 0, "", true };
    }
    if (out_diagnostic) {
        *out_diagnostic = "TypeMatchups: XREF pattern not found in ROM";
    }
    return {};
}


// ============================================================================
// resolve_script_command_table
//
// XREF pattern: RunScriptCommand (engine/overworld/scripting.asm).
// Source:
//   RunScriptCommand:
//     call GetScriptByte      ; CD xx xx
//     call StackJumpTable     ; CD xx xx
//   .Jumptable:
//     table_width 2
//     dw Script_scall         ; dw entries start here
//     dw Script_farscall
//     ...
//
// Pattern: CD ?? ?? CD ?? ?? [then >=20 consecutive valid bank-local dw ptrs]
// The table address is the byte immediately after the two call instructions.
// ============================================================================

ResolvedAddress resolve_script_command_table(
    const RomData& rom,
    uint32_t profile_address,
    std::string* out_diagnostic)
{
    if (profile_address != 0 && profile_address < rom.size()) {
        uint32_t cnt = count_dw_entries(rom, profile_address, 512);
        if (cnt >= 20) {
            return { profile_address,
                     std::format("profile address 0x{:05X} validated ({} entries)", profile_address, cnt) };
        }
    }

    std::vector<uint32_t> candidates;
    const uint32_t limit = (rom.size() >= 12) ? static_cast<uint32_t>(rom.size()) - 12u : 0u;

    for (uint32_t i = 0; i < limit; ++i) {
        if (rom.read_byte(i)   != 0xCD) continue; // call
        if (rom.read_byte(i+3) != 0xCD) continue; // call
        uint32_t tbl_flat = i + 6u;                // table starts right after
        uint32_t cnt = count_dw_entries(rom, tbl_flat, 300);
        if (cnt < 20) continue;
        candidates.push_back(tbl_flat);
    }

    if (candidates.size() == 1) {
        return { candidates[0],
                 std::format("XREF scan → 0x{:05X} ({} dw entries)",
                             candidates[0], count_dw_entries(rom, candidates[0], 300)) };
    }
    if (candidates.size() > 1) {
        // Multiple hits: prefer the one with the most entries (most specific)
        uint32_t best_flat = 0, best_cnt = 0;
        for (uint32_t c : candidates) {
            uint32_t cnt = count_dw_entries(rom, c, 300);
            if (cnt > best_cnt) { best_cnt = cnt; best_flat = c; }
        }
        // Check if the winner is clearly dominant (2× more entries than runner-up)
        uint32_t second_best = 0;
        for (uint32_t c : candidates) {
            if (c == best_flat) continue;
            uint32_t cnt = count_dw_entries(rom, c, 300);
            if (cnt > second_best) second_best = cnt;
        }
        if (best_cnt >= second_best * 2 + 20) {
            return { best_flat,
                     std::format("XREF scan → 0x{:05X} ({} entries, best of {} candidates)",
                                 best_flat, best_cnt, candidates.size()) };
        }
        if (out_diagnostic) {
            *out_diagnostic = std::format("ScriptCommandTable: {} candidates, ambiguous "
                                          "(best={} cnt={}, runner-up cnt={})",
                                          candidates.size(), best_flat, best_cnt, second_best);
        }
        return { 0, "", true };
    }
    if (out_diagnostic) {
        *out_diagnostic = "ScriptCommandTable: XREF pattern not found in ROM";
    }
    return {};
}


// ============================================================================
// resolve_crystal_layout — composite entry point
// ============================================================================

int resolve_crystal_layout(const RomData& rom, ExtractionProfile& profile, bool verbose)
{
    int resolved = 0;
    auto& o = profile.offsets;
    auto& fmt = profile.format;

    auto try_resolve = [&](const char* name, uint32_t& addr_field,
                            ResolvedAddress result,
                            std::string* diag = nullptr) {
        if (result.flat != 0) {
            if (addr_field == 0) {
                addr_field = result.flat;
                ++resolved;
                if (verbose) {
                    std::fprintf(stderr, "[layout] %-26s resolved 0x%05X (%s)\n",
                                 name, result.flat, result.source.c_str());
                }
            } else if (verbose && result.flat != addr_field) {
                std::fprintf(stderr, "[layout] %-26s profile=0x%05X scan=0x%05X — using profile\n",
                             name, addr_field, result.flat);
            }
        } else if (result.ambiguous) {
            if (verbose) {
                std::fprintf(stderr, "[layout] %-26s AMBIGUOUS — %s\n",
                             name, diag ? diag->c_str() : "multiple candidates");
            }
        } else {
            if (verbose && addr_field == 0) {
                std::fprintf(stderr, "[layout] %-26s NOT FOUND — %s\n",
                             name, diag ? diag->c_str() : "pattern not found");
            }
        }
    };

    // --- StdScripts ---
    {
        std::string diag;
        uint8_t esz = 3;
        auto r = resolve_std_scripts(rom, o.std_scripts, &esz, &diag);
        try_resolve("StdScripts", o.std_scripts, r, &diag);
        if (r.flat != 0) {
            fmt.script.std_scripts_entry_size = esz;
            // Derive count from entry size and table structure
            if (o.std_scripts_count == 0) {
                uint32_t cnt = (esz == 2)
                    ? count_dw_entries(rom, o.std_scripts, 256)
                    : count_dba_entries(rom, o.std_scripts, 256);
                o.std_scripts_count = static_cast<uint16_t>(cnt);
                if (verbose && cnt > 0) {
                    std::fprintf(stderr, "[layout] %-26s std_scripts_count=%u (entry_size=%u)\n",
                                 "StdScripts.count", cnt, esz);
                }
            }
        }
    }

    // --- BaseData ---
    {
        std::string diag;
        uint8_t rec_size = fmt.pokemon.base_data_size;
        auto r = resolve_base_data(rom, o.base_data, &rec_size, &diag);
        try_resolve("BaseData", o.base_data, r, &diag);
        if (r.flat != 0 && rec_size != fmt.pokemon.base_data_size) {
            fmt.pokemon.base_data_size = rec_size;
            if (verbose) {
                std::fprintf(stderr, "[layout] %-26s base_data_size=%u (discovered)\n",
                             "BaseData.record_size", rec_size);
            }
        }
    }

    // --- Moves ---
    {
        std::string diag;
        uint8_t rec_size = fmt.move.move_data_size;
        auto r = resolve_moves(rom, o.moves, &rec_size, &diag);
        try_resolve("Moves", o.moves, r, &diag);
        if (r.flat != 0 && rec_size != fmt.move.move_data_size) {
            fmt.move.move_data_size = rec_size;
            // Adjust category_offset if size increased
            if (rec_size > 7 && fmt.move.category_offset == 0xFF) {
                fmt.move.category_offset = 7;  // Polished-style category byte at +7
            }
            if (verbose) {
                std::fprintf(stderr, "[layout] %-26s move_data_size=%u (discovered)\n",
                             "Moves.record_size", rec_size);
            }
        }
    }

    // --- TrainerGroups ---
    {
        std::string diag;
        auto r = resolve_trainer_groups(rom, o.trainer_groups, &diag);
        try_resolve("TrainerGroups", o.trainer_groups, r, &diag);
    }

    // --- TypeMatchups ---
    {
        std::string diag;
        uint32_t inv_flat = 0;
        auto r = resolve_type_matchups(rom, o.type_matchups, &inv_flat, &diag);
        try_resolve("TypeMatchups", o.type_matchups, r, &diag);
        // InverseTypeMatchups: not stored in profile yet, but logged for diagnostic value
        if (inv_flat != 0 && verbose) {
            std::fprintf(stderr, "[layout] %-26s resolved 0x%05X (via TypeMatchups xref)\n",
                         "InvTypeMatchups", inv_flat);
        }
    }

    // --- ScriptCommandTable ---
    {
        std::string diag;
        auto r = resolve_script_command_table(rom, o.script_command_table, &diag);
        try_resolve("ScriptCommandTable", o.script_command_table, r, &diag);
    }

    // --- ROM-derived layout constants ---
    // These replace hardcoded defaults with values extracted from the compiled
    // SM83 code in the ROM's home bank.  Fail gracefully (keep existing) if
    // the pattern is not found.

    // --- num_map_groups from MapGroupPointers table boundary ---
    // Count consecutive valid bank-local 2-byte ptrs (each in [0x4000, 0x7FFF]).
    // The first entry that falls outside that range terminates the table.
    // This is exact — it uses the same criterion as Probe 2 in probe_profile_counts
    // but applies the result rather than just flagging a mismatch.
    // Must run before resolve_group_attr_banks() which uses num_map_groups.
    if (o.map_group_pointers != 0) {
        const uint32_t mgp = o.map_group_pointers;
        uint16_t rom_group_count = 0;
        for (uint32_t i = 0; i < 256u; ++i) {
            uint32_t ea = mgp + i * 2u;
            if (ea + 2u > static_cast<uint32_t>(rom.size())) break;
            uint16_t ptr = static_cast<uint16_t>(rom.read_byte(ea))
                         | (static_cast<uint16_t>(rom.read_byte(ea + 1u)) << 8);
            if (ptr < 0x4000u || ptr > 0x7FFFu) break;
            ++rom_group_count;
        }
        if (rom_group_count > 0 &&
            rom_group_count != static_cast<uint16_t>(profile.counts.num_map_groups)) {
            if (verbose) {
                std::fprintf(stderr,
                    "[layout] %-26s num_map_groups=%u (ROM table boundary, was %u)\n",
                    "Counts.num_map_groups", rom_group_count, profile.counts.num_map_groups);
            }
            profile.counts.num_map_groups = rom_group_count;
            ++resolved;
        }
    }
    {
        // scene_script_size (map_script_header_size)
        uint8_t ss = resolve_scene_script_size(rom);
        if (ss != 0 && ss != fmt.map.map_script_header_size) {
            fmt.map.map_script_header_size = ss;
            ++resolved;
            if (verbose) {
                std::fprintf(stderr, "[layout] %-26s scene_script_size=%u (ROM xref)\n",
                             "MapFormat.scene_size", ss);
            }
        } else if (ss != 0 && verbose) {
            // Already correct — no change, no increment
        } else if (ss == 0 && verbose) {
            std::fprintf(stderr, "[layout] %-26s NOT FOUND — using default %u\n",
                         "MapFormat.scene_size", fmt.map.map_script_header_size);
        }
    }
    {
        // map_entry_size (MAP_LENGTH)
        uint8_t ms = resolve_map_entry_stride(rom);
        if (ms != 0 && ms != fmt.map.map_entry_size) {
            fmt.map.map_entry_size = ms;
            ++resolved;
            if (verbose) {
                std::fprintf(stderr, "[layout] %-26s map_entry_size=%u (ROM xref)\n",
                             "MapFormat.entry_stride", ms);
            }
        } else if (ms == 0 && verbose) {
            std::fprintf(stderr, "[layout] %-26s NOT FOUND — using default %u\n",
                         "MapFormat.entry_stride", fmt.map.map_entry_size);
        }
    }
    {
        // coord_event_size (COORD_EVENT_SIZE)
        uint8_t cs = resolve_coord_event_size(rom);
        if (cs != 0 && cs != fmt.map.coord_event_size) {
            fmt.map.coord_event_size = cs;
            ++resolved;
            if (verbose) {
                std::fprintf(stderr, "[layout] %-26s coord_event_size=%u (ROM xref)\n",
                             "MapFormat.coord_size", cs);
            }
        } else if (cs == 0 && verbose) {
            std::fprintf(stderr, "[layout] %-26s NOT FOUND — using default %u\n",
                         "MapFormat.coord_size", fmt.map.coord_event_size);
        }
    }

    // --- Group-level MapAttributes bank resolution ---
    // Only fires when map entries lack an explicit bank byte (Polished Crystal).
    // Populates profile.offsets.group_attr_banks[1..num_map_groups].
    // Must run AFTER the scene_script_size and map_entry_size resolvers above
    // so that fmt.map.map_script_header_size and fmt.map.map_entry_size are correct.
    if (!fmt.map.attr_bank_in_entry && fmt.map.resolve_attr_bank_by_scan) {
        int gb = resolve_group_attr_banks(rom, profile, verbose);
        resolved += gb;
        if (verbose && gb == 0) {
            std::fprintf(stderr,
                "[layout] %-26s no groups resolved\n", "GroupAttrBanks");
        }
        // max_map_dimension and max_environment_value are NOT tightened here.
        // Observed maxima are not semantic maxima: a map or environment value that
        // exceeds the largest value seen in this ROM is not necessarily invalid.
        // These fields remain at their profile-declared values, which represent the
        // correct structural acceptance range for this ROM family.
    }

    return resolved;
}

// ============================================================================
// Layout constant resolvers — home-bank SM83 xrefs
// ============================================================================

uint8_t resolve_scene_script_size(const RomData& rom) {
    // SCENE_SCRIPT_SIZE xref: map-loading routine in home bank.
    //
    // The map-loading code parses the MapScriptHeader with two sequential patterns,
    // one for scene entries (size = SCENE_SCRIPT_SIZE) and one for callbacks (size = 3).
    // Both follow the same template: lead / ld bc,SIZE / call|rst AddNTimes.
    //
    // Lead bytes differ between versions:
    //   Vanilla Crystal: C8 (ret z) 01 NN 00 CD  — ret-z guarded ld bc
    //   Polished Crystal: 2A (ld a,[hli]) 01 NN 00 DF  — inline ld a,[hli]
    //
    // Vanilla  (scene=4): C8 01 04 00 CD  at home bank ~0x023BB,
    //          callback=3 at ~0x023D2 — 23 bytes after scene pattern.
    // Polished (scene=2): 2A 01 02 00 DF  at home bank ~0x01E9C,
    //          callback=3 at ~0x01EA4 — 8 bytes after scene pattern (2A 01 03 00 DF).
    //
    // Secondary validation: 01 03 00 (ld bc,3 for CALLBACK_SIZE) must appear within
    // 28 bytes after the scene pattern.  Window is 28 to cover vanilla's 23-byte gap.
    const uint32_t home_bank_end = std::min(static_cast<uint32_t>(rom.size()),
                                             static_cast<uint32_t>(0x4000u));
    for (uint32_t i = 0; i + 6u < home_bank_end; ++i) {
        uint8_t lead = rom.read_byte(i);
        if (lead != 0x2A && lead != 0xC8) continue;  // ld a,[hli] or ret z
        if (rom.read_byte(i+1) != 0x01) continue;     // ld bc, nn
        uint8_t nn = rom.read_byte(i+2);
        if (nn < 2u || nn > 8u) continue;
        if (rom.read_byte(i+3) != 0x00) continue;
        uint8_t next = rom.read_byte(i+4);
        if (next != 0xCD && next != 0xDF && next != 0xE7 && next != 0xD7) continue;
        // Secondary: CALLBACK_SIZE=3 must appear within 28 bytes as ld bc,3 (01 03 00)
        bool cb_found = false;
        for (uint32_t j = i + 5u; j < i + 28u && j + 3u < home_bank_end; ++j) {
            if (rom.read_byte(j)   == 0x01 &&
                rom.read_byte(j+1) == 0x03 &&
                rom.read_byte(j+2) == 0x00) { cb_found = true; break; }
        }
        if (!cb_found) continue;
        return nn;
    }
    return 0;
}

uint8_t resolve_map_entry_stride(const RomData& rom) {
    // MAP_LENGTH xref: GetAnyMapPointer in home bank.
    // Source: "dec c / ld b, 0 / ld a, MAP_LENGTH / rst|call AddNTimes / ret"
    // Pattern: 0D 06 00 3E NN [DF|E7|D7|CD]   where NN = MAP_LENGTH ∈ [5,16]
    //   Vanilla: NN=9, Polished: NN=7
    const uint32_t home_bank_end = std::min(static_cast<uint32_t>(rom.size()),
                                             static_cast<uint32_t>(0x4000u));
    for (uint32_t i = 0; i + 6u < home_bank_end; ++i) {
        if (rom.read_byte(i)   != 0x0D) continue;  // dec c
        if (rom.read_byte(i+1) != 0x06) continue;  // ld b, n
        if (rom.read_byte(i+2) != 0x00) continue;  //   n=0
        if (rom.read_byte(i+3) != 0x3E) continue;  // ld a, n
        uint8_t nn = rom.read_byte(i+4);
        if (nn < 5u || nn > 16u) continue;
        uint8_t next = rom.read_byte(i+5);
        if (next != 0xDF && next != 0xE7 && next != 0xD7 && next != 0xCD) continue;
        return nn;
    }
    return 0;
}

uint8_t resolve_coord_event_size(const RomData& rom) {
    // COORD_EVENT_SIZE xref: map-events counting loop in home bank.
    //
    // The map-event-parsing code iterates each event type with:
    //   ret z                       ; C8  — return if count is zero
    //   ld bc, EVENT_SIZE           ; 01 NN 00
    //   call|rst AddNTimes          ; CD|DF|E7|D7
    //
    // Vanilla Crystal (8 bytes per coord event):
    //   → NN=8; warp=5 and bg=5 produce identical patterns with NN=5.
    //   To isolate the coord pattern, search first for NN >= 6 (larger than warp/bg).
    //
    // Polished Crystal (5 bytes per coord event):
    //   All event sizes are 5.  No NN>=6 hit exists.  Fall back to NN in [4,6]
    //   with C8 lead, excluding the false-positive "2A 66 6F 79" prefix (which
    //   is a different loop that uses a similar ld bc,5 sequence).
    //
    // Two-pass strategy guarantees exactly one correct hit per ROM:
    //   Pass 1: C8 01 NN 00 (call|rst)  where NN ∈ [6,12]  — vanilla coord (NN=8)
    //   Pass 2: C8 01 NN 00 (call|rst)  where NN ∈ [4,6],
    //           NOT preceded by [2A 66 6F 79]              — Polished coord (NN=5)
    const uint32_t home_bank_end = std::min(static_cast<uint32_t>(rom.size()),
                                             static_cast<uint32_t>(0x4000u));

    // Pass 1: require NN >= 6 (unambiguous in vanilla; absent in Polished)
    for (uint32_t i = 0; i + 5u < home_bank_end; ++i) {
        if (rom.read_byte(i) != 0xC8) continue;         // ret z — strict lead
        if (rom.read_byte(i+1) != 0x01) continue;       // ld bc, nn
        uint8_t nn = rom.read_byte(i+2);
        if (nn < 6u || nn > 12u) continue;              // coord > warp/bg size
        if (rom.read_byte(i+3) != 0x00) continue;
        uint8_t next = rom.read_byte(i+4);
        if (next != 0xCD && next != 0xDF && next != 0xE7 && next != 0xD7) continue;
        return nn;
    }

    // Pass 2: Polished fallback — C8 lead with NN in [4,6], excluding the
    // scene/callback-loop false positive identified by the "2A 66 6F 79" prefix.
    for (uint32_t i = 0; i + 5u < home_bank_end; ++i) {
        if (rom.read_byte(i) != 0xC8) continue;
        if (rom.read_byte(i+1) != 0x01) continue;
        uint8_t nn = rom.read_byte(i+2);
        if (nn < 4u || nn > 6u) continue;
        if (rom.read_byte(i+3) != 0x00) continue;
        uint8_t next = rom.read_byte(i+4);
        if (next != 0xCD && next != 0xDF && next != 0xE7 && next != 0xD7) continue;
        // Exclude "2A 66 6F 79" (ld a,[hli] / ld h,a / ld l,a / ld a,c) prefix —
        // this marks the scene-skip loop, not the coord-event counting loop.
        if (i >= 4u &&
            rom.read_byte(i-4) == 0x2A && rom.read_byte(i-3) == 0x66 &&
            rom.read_byte(i-2) == 0x6F && rom.read_byte(i-1) == 0x79) continue;
        return nn;
    }

    return 0;  // not found
}

// ============================================================================
// Group-level MapAttributes bank resolver
// ============================================================================

int resolve_group_attr_banks(const RomData& rom, ExtractionProfile& profile, bool verbose)
{
    const auto& fmt = profile.format.map;
    const auto& o   = profile.offsets;
    auto& c         = profile.counts;

    // Only applies when the map entry has no explicit bank byte.
    if (fmt.attr_bank_in_entry) return 0;

    const uint32_t rom_size   = static_cast<uint32_t>(rom.size());
    const uint32_t max_bank   = rom_size / 0x4000u;
    const uint8_t  num_groups = static_cast<uint8_t>(c.num_map_groups);
    const uint8_t  UNRESOLVED = ProfileOffsets::ATTR_BANK_UNRESOLVED;

    // Derive MapScriptHeader scene_count and callback_count limits from format.
    // scene_script_size already resolved; use it here.
    const uint8_t  scene_entry_sz = fmt.map_script_header_size;  // 2 or 4
    const uint8_t  cb_entry_sz    = 3;                           // CALLBACK_SIZE always 3

    // Validate that MapGroupPointers is available.
    if (o.map_group_pointers == 0) return 0;
    const uint32_t mgp_flat     = o.map_group_pointers;
    const uint8_t  groups_bank  = o.map_groups_bank;
    const uint8_t  entry_stride = fmt.map_entry_size;

    // Helper: compute the flat ROM address of a group's first entry.
    // Returns 0 on invalid pointer.
    auto group_data_flat = [&](uint8_t grp) -> uint32_t {
        uint32_t tbl_off = mgp_flat + static_cast<uint32_t>(grp - 1u) * 2u;
        if (tbl_off + 2u > rom_size) return 0;
        uint16_t ptr = static_cast<uint16_t>(rom.read_byte(tbl_off))
                     | (static_cast<uint16_t>(rom.read_byte(tbl_off + 1u)) << 8);
        if (ptr < 0x4000u || ptr >= 0x8000u) return 0;
        return static_cast<uint32_t>(groups_bank) * 0x4000u + (ptr - 0x4000u);
    };

    // Derive observed max tileset ID from groups whose entry count is exactly known
    // (pointer-difference is divisible by entry_stride).  Used to tighten the
    // sentinel scan for groups that don't have a clean pointer difference.
    uint8_t observed_max_ts = 0;
    for (uint8_t grp = 1u; grp <= num_groups && grp < ProfileOffsets::MAX_MAP_GROUPS; ++grp) {
        uint32_t gflat = group_data_flat(grp);
        if (gflat == 0) continue;
        if (grp < num_groups) {
            uint32_t nflat = group_data_flat(static_cast<uint8_t>(grp + 1u));
            if (nflat == 0 || nflat <= gflat) continue;
            uint32_t diff = nflat - gflat;
            if (diff % entry_stride != 0) continue;  // not exact — skip for max-ts derivation
            uint32_t ec = diff / entry_stride;
            for (uint32_t i = 0; i < ec; ++i) {
                uint32_t ef = gflat + i * entry_stride;
                if (ef + entry_stride > rom_size) break;
                uint8_t ts = rom.read_byte(ef);
                if (ts > observed_max_ts) observed_max_ts = ts;
            }
        }
    }
    // Clamp to at least 36 (vanilla minimum) and fall back to profile count if
    // no observed value could be derived.
    if (observed_max_ts == 0) observed_max_ts = static_cast<uint8_t>(
        std::min<uint32_t>(c.num_tilesets, 255u));
    // Add a small headroom factor (×1.5) to tolerate additional tilesets in
    // extended groups without false negatives.
    {
        uint8_t ts_ceil = static_cast<uint8_t>(
            std::min<uint32_t>(static_cast<uint32_t>(observed_max_ts) * 3u / 2u + 1u, 255u));
        observed_max_ts = ts_ceil;
    }

    // Helper: derive the exact entry count for group G from the pointer difference.
    // group G ends where group G+1 begins; last group uses a strict sentinel scan.
    auto group_entry_count = [&](uint8_t grp) -> uint32_t {
        uint32_t gf = group_data_flat(grp);
        if (gf == 0) return 0;
        if (grp < num_groups) {
            uint32_t next_gf = group_data_flat(static_cast<uint8_t>(grp + 1u));
            if (next_gf > gf && (next_gf - gf) % entry_stride == 0) {
                return (next_gf - gf) / entry_stride;
            }
            // Pointer difference is not divisible by stride.  Fall through to
            // sentinel scan but use strict tileset+environment validity to
            // avoid counting SM83 opcodes as map entries.
        }
        // Sentinel scan: stop at first entry whose tileset byte exceeds the
        // observed max (with headroom) or whose environment nibble is 0 or > max_env.
        const uint8_t max_env = fmt.max_environment_value;
        for (uint32_t i = 0; i < 256u; ++i) {
            uint32_t ef = gf + i * entry_stride;
            if (ef + entry_stride > rom_size) return i;
            uint8_t ts = rom.read_byte(ef);
            if (ts == 0 || ts > observed_max_ts) return i;
            // Additional env validity (only for sign_env_nibble format; vanilla
            // uses a different byte layout and this check is not needed there
            // since vanilla uses exact pointer differences).
            if (fmt.sign_env_nibble) {
                uint8_t env = rom.read_byte(ef + 1u) & 0x0Fu;
                if (env == 0 || env > max_env) return i;
            }
        }
        return 0;
    };

    // Full structural validity test for a single map entry's attr_ptr at a given bank.
    // Returns true only when ALL nine criteria pass.
    auto entry_valid_at_bank = [&](uint32_t ef, uint32_t b) -> bool {
        uint32_t ptr_off = ef + fmt.attr_ptr_field_offset;
        if (ptr_off + 2u > rom_size) return false;
        uint16_t attr_ptr = static_cast<uint16_t>(rom.read_byte(ptr_off))
                          | (static_cast<uint16_t>(rom.read_byte(ptr_off + 1u)) << 8);
        if (attr_ptr < 0x4000u || attr_ptr >= 0x8000u) return false;
        uint32_t flat = b * 0x4000u + (attr_ptr - 0x4000u);
        if (flat + fmt.header_size > rom_size) return false;

        // 1. Height / width
        uint8_t h  = rom.read_byte(flat + fmt.height_offset);
        uint8_t w  = rom.read_byte(flat + fmt.width_offset);
        if (h == 0 || h > fmt.max_map_dimension || w == 0 || w > fmt.max_map_dimension) return false;

        // 2. Blockdata bank / ptr
        uint8_t  bb = rom.read_byte(flat + fmt.blockdata_bank_offset);
        if (bb >= 128u) return false;
        uint16_t bp = static_cast<uint16_t>(rom.read_byte(flat + fmt.blockdata_ptr_offset))
                    | (static_cast<uint16_t>(rom.read_byte(flat + fmt.blockdata_ptr_offset + 1u)) << 8);
        if (bb > 0 && (bp < 0x4000u || bp >= 0x8000u)) return false;

        // 3. Script bank / ptr
        uint8_t  sb = rom.read_byte(flat + fmt.script_bank_offset);
        if (sb >= 128u) return false;
        uint16_t sp = static_cast<uint16_t>(rom.read_byte(flat + fmt.script_ptr_offset))
                    | (static_cast<uint16_t>(rom.read_byte(flat + fmt.script_ptr_offset + 1u)) << 8);
        if (sp >= 0x8000u) return false;
        if (sb > 0 && sp < 0x4000u) return false;

        // 4. MapScriptHeader: scene_count / callback_count / warp_count
        uint32_t sh_flat = (sp < 0x4000u)
                         ? static_cast<uint32_t>(sp)
                         : (static_cast<uint32_t>(sb) * 0x4000u + sp - 0x4000u);
        if (sh_flat + 4u > rom_size) return false;

        uint8_t sh_sc = rom.read_byte(sh_flat);
        if (sh_sc > 30u) return false;
        uint32_t sh_p = sh_flat + 1u + static_cast<uint32_t>(sh_sc) * scene_entry_sz;
        if (sh_p + 1u > rom_size) return false;

        uint8_t sh_cc = rom.read_byte(sh_p);
        if (sh_cc > 20u) return false;
        sh_p += 1u + static_cast<uint32_t>(sh_cc) * cb_entry_sz;
        if (sh_p + 1u > rom_size) return false;

        uint8_t sh_wc = rom.read_byte(sh_p);
        if (sh_wc > 50u) return false;

        return true;
    };

    int resolved = 0;

    for (uint8_t grp = 1u; grp <= num_groups; ++grp) {
        if (grp >= ProfileOffsets::MAX_MAP_GROUPS) break;
        // Already resolved (e.g. by a prior run or explicit profile metadata).
        if (profile.offsets.group_attr_banks[grp] != UNRESOLVED) {
            ++resolved;
            continue;
        }

        uint32_t gflat = group_data_flat(grp);
        if (gflat == 0) continue;
        uint32_t ec = group_entry_count(grp);
        if (ec == 0) continue;

        // Score each candidate bank: count entries that pass the full 9-point test.
        uint8_t  best_bank  = UNRESOLVED;
        uint32_t best_score = 0;
        bool     tied       = false;

        for (uint32_t b = 1u; b < max_bank; ++b) {
            uint32_t score = 0;
            for (uint32_t i = 0; i < ec; ++i) {
                uint32_t ef = gflat + i * entry_stride;
                if (ef + entry_stride > rom_size) break;
                if (entry_valid_at_bank(ef, b)) ++score;
            }
            if (score == ec && ec > 0) {
                // Perfect: all entries pass at this bank.
                if (best_bank == UNRESOLVED) {
                    best_bank  = static_cast<uint8_t>(b);
                    best_score = score;
                    tied       = false;
                } else {
                    // Two banks both score perfect for all entries.
                    // Cannot prove uniqueness — will be treated as ambiguous below.
                    tied = true;
                }
            }
        }

        if (!tied && best_bank != UNRESOLVED && best_score == ec) {
            // Unique perfect-score bank: proven.
            profile.offsets.group_attr_banks[grp] = best_bank;
            ++resolved;
            if (verbose) {
                std::fprintf(stderr,
                    "[layout] %-26s group %2u → bank 0x%02X (%u entries proven)\n",
                    "GroupAttrBanks", grp, best_bank, ec);
            }
        } else {
            // Ambiguous or not found: leave as UNRESOLVED so extraction fails explicitly.
            if (verbose) {
                std::fprintf(stderr,
                    "[layout] %-26s group %2u NOT RESOLVED (ec=%u best=%u tied=%d)\n",
                    "GroupAttrBanks", grp, ec, best_score, static_cast<int>(tied));
            }
        }
    }

    return resolved;
}

}  // namespace crystal
