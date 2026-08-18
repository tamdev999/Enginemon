#pragma once
// crystal/rom/bank_utils.hpp
// Canonical Crystal ROM bank address resolution helpers.
//
// These are the single source of truth for Crystal bank arithmetic in the
// frontend. All call sites that previously inlined the formula
//   bank * 0x4000 + (ptr - 0x4000)
// or
//   address / 0x4000
// must use these helpers instead.
//
// Relationship to RomData:
//   RomData::bank_to_flat() and RomData::flat_to_bank() implement the same
//   formulas but require a RomData instance. These free functions provide
//   the same semantics for contexts where RomData is not available (e.g.
//   semantic lowering rules that receive only the script entry address).
//
// Crystal ROM banking model (MBC3):
//   Bank 0 occupies flat range 0x0000–0x3FFF (ROM0).
//   Bank N occupies flat range N*0x4000–(N+1)*0x4000-1.
//   The banked window is visible at GB address 0x4000–0x7FFF, so local
//   pointers within a bank are in the range 0x4000–0x7FFF.
//   A local pointer < 0x4000 addresses ROM0 directly (bank 0 region).

#include <cstdint>

namespace crystal {

// ---------------------------------------------------------------------------
// crystal_flat_to_bank
//
// Identical to RomData::flat_to_bank(). Provided for contexts without ROM.
//
// flat_address → Crystal bank number
//   bank 0: flat  0x0000–0x3FFF
//   bank N: flat  N*0x4000–(N+1)*0x4000-1
// ---------------------------------------------------------------------------
inline uint8_t crystal_flat_to_bank(uint32_t flat_address) noexcept {
    constexpr uint32_t BANK_SIZE = 0x4000;
    if (flat_address < BANK_SIZE) return 0;
    return static_cast<uint8_t>(flat_address / BANK_SIZE);
}

// ---------------------------------------------------------------------------
// crystal_bank_to_flat
//
// Identical to RomData::bank_to_flat(). Provided for contexts without ROM.
//
// bank + local 16-bit ROM pointer → flat address
//   ptr < 0x4000  → ROM0 (bank 0 region), flat = ptr
//   ptr >= 0x4000 → flat = bank * 0x4000 + (ptr - 0x4000)
// ---------------------------------------------------------------------------
inline uint32_t crystal_bank_to_flat(uint8_t bank, uint16_t ptr) noexcept {
    constexpr uint32_t BANK_SIZE = 0x4000;
    if (ptr < BANK_SIZE) {
        return ptr;  // ROM0: bank number is irrelevant
    }
    return static_cast<uint32_t>(bank) * BANK_SIZE + (ptr - BANK_SIZE);
}

// ---------------------------------------------------------------------------
// crystal_local_ptr_to_flat
//
// Resolve a bank-local 16-bit ROM pointer using the flat address of the
// currently executing/calling script as the bank source.
//
// This is the canonical call pattern for sdefer and getstring lowering:
//   the 16-bit pointer operand is local to the script's bank, and the
//   bank is inferred from the script's own flat entry address.
//
// Source-proven from pokecrystal engine/overworld/scripting.asm:
//   Script_sdefer  line 1389: ld a, [wScriptBank]   → bank = current script bank
//   Script_getstring line 1688: ld a, [wScriptBank] → bank = current script bank
//
// Parameters:
//   script_entry_flat  flat address of the script that contains the pointer
//   local_ptr          16-bit bank-relative pointer operand from the script
//
// Returns:
//   flat ROM address of the pointer target
// ---------------------------------------------------------------------------
inline uint32_t crystal_local_ptr_to_flat(uint32_t script_entry_flat,
                                           uint16_t local_ptr) noexcept {
    const uint8_t bank = crystal_flat_to_bank(script_entry_flat);
    return crystal_bank_to_flat(bank, local_ptr);
}

} // namespace crystal
