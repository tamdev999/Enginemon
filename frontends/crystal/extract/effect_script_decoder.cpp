// frontends/crystal/extract/effect_script_decoder.cpp
// Crystal effect-script decoder — compiler-side only.
//
// Decodes Crystal move-effect scripts from ROM bytes into a typed
// EffectScriptCorpus.  See effect_script_decoder.hpp for full documentation.
//
// Source authority:
//   Crystal v1.1 pokecrystal11.sym + engine/battle/effect_commands.asm
//   data/moves/effects.asm, data/moves/effects_pointers.asm
//   macros/scripts/battle_commands.asm (command byte ↔ BattleCommandPointers index)

#include "crystal/extract/effect_script_decoder.hpp"
#include <format>
#include <cstdio>

namespace crystal {

// ============================================================================
// Private helpers
// ============================================================================

uint8_t EffectScriptDecoder::rom_byte(const RomData& rom, uint32_t flat) {
    if (flat >= static_cast<uint32_t>(rom.size())) return 0xFF;
    return rom.read_byte(flat);
}

uint32_t EffectScriptDecoder::bank_rel_to_flat(uint8_t bank, uint16_t bank_rel) {
    if (bank_rel < 0x4000u) {
        // Bank 0 — address maps directly (no offset subtraction).
        return bank_rel;
    }
    return static_cast<uint32_t>(bank) * 0x4000u
         + static_cast<uint32_t>(bank_rel - 0x4000u);
}

bool EffectScriptDecoder::is_valid_bank_pointer(uint8_t bank, uint16_t bank_rel) {
    // A bank-relative pointer must fall within the ROMX window [0x4000, 0x7FFF]
    // for any switchable bank.  Bank 0 uses [0x0000, 0x3FFF] but all the Crystal
    // battle-system tables live in switched banks, so we enforce the ROMX range.
    (void)bank;
    return bank_rel >= 0x4000u && bank_rel <= 0x7FFFu;
}

// ============================================================================
// scan_mep
//
// Searches for the MoveEffectsPointers dispatcher sequence in the full ROM.
//
// Exact pattern at the scan position i:
//   i+0: 4F          ld c, a        (effect_id now in c)
//   i+1: 06          ld b, N
//   i+2: 00          N = 0
//   i+3: 21          ld hl, nn
//   i+4: lo          MEP bank-relative address low byte
//   i+5: hi          MEP bank-relative address high byte
//   i+6: 09          add hl, bc
//   i+7: 09          add hl, bc
//   i+8: 3E          ld a, N
//   i+9: bank        BANK(MoveEffectsPointers)
//   i+10: CD         call GetFarWord
//
// Recovers: mep_bank_relative = rom[i+4] | (rom[i+5] << 8)
//           mep_bank           = rom[i+9]
//           mep_flat           = bank_rel_to_flat(mep_bank, mep_bank_relative)
// ============================================================================
std::vector<std::pair<uint32_t, uint8_t>> EffectScriptDecoder::scan_mep(const RomData& rom) {
    std::vector<std::pair<uint32_t, uint8_t>> results;
    if (rom.size() < 11) return results;
    const uint32_t limit = static_cast<uint32_t>(rom.size()) - 11u;

    for (uint32_t i = 0; i < limit; ++i) {
        if (rom_byte(rom, i)    != 0x4F) continue;  // ld c, a
        if (rom_byte(rom, i+1)  != 0x06) continue;  // ld b, n
        if (rom_byte(rom, i+2)  != 0x00) continue;  // n = 0
        if (rom_byte(rom, i+3)  != 0x21) continue;  // ld hl, nn
        if (rom_byte(rom, i+6)  != 0x09) continue;  // add hl, bc
        if (rom_byte(rom, i+7)  != 0x09) continue;  // add hl, bc
        if (rom_byte(rom, i+8)  != 0x3E) continue;  // ld a, n
        if (rom_byte(rom, i+10) != 0xCD) continue;  // call

        const uint8_t  bank    = rom_byte(rom, i+9);
        const uint16_t bank_rel = static_cast<uint16_t>(rom_byte(rom, i+4))
                                | (static_cast<uint16_t>(rom_byte(rom, i+5)) << 8);

        if (!is_valid_bank_pointer(bank, bank_rel)) continue;

        const uint32_t flat = bank_rel_to_flat(bank, bank_rel);
        if (flat + 4 > static_cast<uint32_t>(rom.size())) continue;

        // ── Validate: first 5 MEP entries must all be plausible effect scripts ──
        bool valid = true;
        for (uint32_t e = 0; e < 5 && valid; ++e) {
            const uint32_t eptr_off = flat + e * 2u;
            if (eptr_off + 2u > static_cast<uint32_t>(rom.size())) { valid = false; break; }
            const uint16_t entry_br =
                  static_cast<uint16_t>(rom_byte(rom, eptr_off))
                | (static_cast<uint16_t>(rom_byte(rom, eptr_off + 1u)) << 8);
            if (!is_valid_bank_pointer(bank, entry_br)) { valid = false; break; }
            const uint32_t script_f = bank_rel_to_flat(bank, entry_br);
            const uint8_t first = rom_byte(rom, script_f);
            if (first < 0x01 || first > 0xAF) { valid = false; break; }
            bool found_end = false;
            for (uint32_t k = 0; k < MAX_SCRIPT_LENGTH; ++k) {
                if (rom_byte(rom, script_f + k) == 0xFF) { found_end = true; break; }
            }
            if (!found_end) { valid = false; break; }
        }
        if (!valid) continue;

        // Deduplicate by flat address (same table may be found at multiple i offsets)
        bool dup = false;
        for (const auto& [f, b] : results) if (f == flat) { dup = true; break; }
        if (!dup) results.push_back({flat, bank});
    }
    return results;
}

// ============================================================================
// scan_bcp
//
// Searches for the BattleCommandPointers dispatcher sequence in the full ROM.
//
// Exact pattern at the scan position i:
//   i+0:  3D          dec a          (cmd byte − 1 for 0-indexed table)
//   i+1:  4F          ld c, a
//   i+2:  06          ld b, n
//   i+3:  00          n = 0
//   i+4:  21          ld hl, nn
//   i+5:  lo          BCP bank-relative address low byte
//   i+6:  hi          BCP bank-relative address high byte
//   i+7:  09          add hl, bc
//   i+8:  09          add hl, bc
//   i+9:  C1          pop bc         (distinguishes BCP from MEP pattern)
//   i+10: 3E          ld a, n
//   i+11: bank        BANK(BattleCommandPointers)
//   i+12: CD          call GetFarWord
//
// Recovers: bcp_bank_relative = rom[i+5] | (rom[i+6] << 8)
//           bcp_bank           = rom[i+11]
//           bcp_flat           = bank_rel_to_flat(bcp_bank, bcp_bank_relative)
// ============================================================================
std::vector<std::pair<uint32_t, uint8_t>> EffectScriptDecoder::scan_bcp(const RomData& rom) {
    std::vector<std::pair<uint32_t, uint8_t>> results;
    if (rom.size() < 13) return results;
    const uint32_t limit = static_cast<uint32_t>(rom.size()) - 13u;

    for (uint32_t i = 0; i < limit; ++i) {
        if (rom_byte(rom, i)    != 0x3D) continue;  // dec a
        if (rom_byte(rom, i+1)  != 0x4F) continue;  // ld c, a
        if (rom_byte(rom, i+2)  != 0x06) continue;  // ld b, n
        if (rom_byte(rom, i+3)  != 0x00) continue;  // n = 0
        if (rom_byte(rom, i+4)  != 0x21) continue;  // ld hl, nn
        if (rom_byte(rom, i+7)  != 0x09) continue;  // add hl, bc
        if (rom_byte(rom, i+8)  != 0x09) continue;  // add hl, bc
        if (rom_byte(rom, i+9)  != 0xC1) continue;  // pop bc  ← key discriminator
        if (rom_byte(rom, i+10) != 0x3E) continue;  // ld a, n
        if (rom_byte(rom, i+12) != 0xCD) continue;  // call

        const uint8_t  bank    = rom_byte(rom, i+11);
        const uint16_t bank_rel = static_cast<uint16_t>(rom_byte(rom, i+5))
                                | (static_cast<uint16_t>(rom_byte(rom, i+6)) << 8);

        if (!is_valid_bank_pointer(bank, bank_rel)) continue;

        const uint32_t flat = bank_rel_to_flat(bank, bank_rel);
        if (flat + 4 > static_cast<uint32_t>(rom.size())) continue;

        bool dup = false;
        for (const auto& [f, b] : results) if (f == flat) { dup = true; break; }
        if (!dup) results.push_back({flat, bank});
    }
    return results;
}

// ============================================================================
// resolve_tables
//
// Policy for both MEP and BCP:
//   profile = 0
//     → scan must yield exactly 1 candidate → use it
//     → 0 candidates → hard fail
//     → > 1 candidates → hard fail (ambiguous)
//
//   profile != 0 and scan == profile
//     → accept
//
//   profile != 0 and scan != profile (includes: scan = 0 candidates,
//     scan > 1 candidates, or single candidate ≠ profile)
//     → hard fail (contradiction: profile is stale or ROM is wrong version)
//
// resolve_tables never silently substitutes the scan result when the profile
// disagrees.  Dispatcher evidence is deterministic; a mismatch means the
// caller cannot trust the table address.
// ============================================================================
ResolvedTables EffectScriptDecoder::resolve_tables(
    const RomData& rom,
    const ExtractionProfile& profile)
{
    ResolvedTables result;

    // ── MoveEffectsPointers ──────────────────────────────────────────────────
    {
        const uint32_t profile_flat = profile.offsets.move_effects_pointers;
        const auto mep_hits = scan_mep(rom);

        if (profile_flat == 0) {
            // No profile address: scan must yield exactly one candidate.
            if (mep_hits.empty()) {
                result.error = "MoveEffectsPointers: structural scan found 0 candidates "
                    "(ROM not recognised; set move_effects_pointers in profile)";
                return result;
            }
            if (mep_hits.size() > 1) {
                result.error = std::format(
                    "MoveEffectsPointers: structural scan found {} candidates (ambiguous); "
                    "set move_effects_pointers in profile to resolve",
                    mep_hits.size());
                return result;
            }
            result.mep_flat = mep_hits[0].first;
            result.mep_bank = mep_hits[0].second;
        } else {
            // Profile address supplied: scan must confirm it exactly.
            // Determine what the scan says.
            uint32_t scan_flat = 0;
            uint8_t  scan_bank = 0;
            if (mep_hits.size() == 1) {
                scan_flat = mep_hits[0].first;
                scan_bank = mep_hits[0].second;
            }
            // Any deviation (0 hits, >1 hits, or single hit ≠ profile) is a failure.
            if (mep_hits.size() != 1 || scan_flat != profile_flat) {
                if (mep_hits.empty()) {
                    result.error = std::format(
                        "MoveEffectsPointers: profile address 0x{:05X} supplied but "
                        "structural scan found 0 candidates — profile may be stale",
                        profile_flat);
                } else if (mep_hits.size() > 1) {
                    result.error = std::format(
                        "MoveEffectsPointers: profile address 0x{:05X} supplied but "
                        "structural scan found {} candidates — profile may be stale",
                        profile_flat, mep_hits.size());
                } else {
                    result.error = std::format(
                        "MoveEffectsPointers: profile address 0x{:05X} contradicts "
                        "scan result 0x{:05X} — profile is stale or ROM is wrong version",
                        profile_flat, scan_flat);
                }
                return result;
            }
            // Profile and scan agree.
            result.mep_flat = scan_flat;
            result.mep_bank = scan_bank;
        }
    }

    // ── BattleCommandPointers ────────────────────────────────────────────────
    {
        const uint32_t profile_flat = profile.offsets.battle_command_pointers;
        const auto bcp_hits = scan_bcp(rom);

        if (profile_flat == 0) {
            if (bcp_hits.empty()) {
                result.error = "BattleCommandPointers: structural scan found 0 candidates "
                    "(ROM not recognised; set battle_command_pointers in profile)";
                return result;
            }
            if (bcp_hits.size() > 1) {
                result.error = std::format(
                    "BattleCommandPointers: structural scan found {} candidates (ambiguous); "
                    "set battle_command_pointers in profile to resolve",
                    bcp_hits.size());
                return result;
            }
            result.bcp_flat = bcp_hits[0].first;
            result.bcp_bank = bcp_hits[0].second;
        } else {
            uint32_t scan_flat = 0;
            uint8_t  scan_bank = 0;
            if (bcp_hits.size() == 1) {
                scan_flat = bcp_hits[0].first;
                scan_bank = bcp_hits[0].second;
            }
            if (bcp_hits.size() != 1 || scan_flat != profile_flat) {
                if (bcp_hits.empty()) {
                    result.error = std::format(
                        "BattleCommandPointers: profile address 0x{:05X} supplied but "
                        "structural scan found 0 candidates — profile may be stale",
                        profile_flat);
                } else if (bcp_hits.size() > 1) {
                    result.error = std::format(
                        "BattleCommandPointers: profile address 0x{:05X} supplied but "
                        "structural scan found {} candidates — profile may be stale",
                        profile_flat, bcp_hits.size());
                } else {
                    result.error = std::format(
                        "BattleCommandPointers: profile address 0x{:05X} contradicts "
                        "scan result 0x{:05X} — profile is stale or ROM is wrong version",
                        profile_flat, scan_flat);
                }
                return result;
            }
            result.bcp_flat = scan_flat;
            result.bcp_bank = scan_bank;
        }
    }

    return result;
}

// ============================================================================
// decode_one
// ============================================================================
std::optional<DecodedEffectScript> EffectScriptDecoder::decode_one(
    const RomData& rom,
    uint16_t effect_id,
    uint16_t mep_pointer,
    uint32_t flat_addr,
    uint16_t num_commands,
    EffectScriptDecodeError* out_error)
{
    auto fail = [&](std::string msg) -> std::optional<DecodedEffectScript> {
        if (out_error) {
            out_error->message   = std::move(msg);
            out_error->effect_id = effect_id;
        }
        return std::nullopt;
    };

    if (flat_addr >= static_cast<uint32_t>(rom.size()))
        return fail(std::format(
            "effect {}: script flat address 0x{:05X} is out of ROM bounds",
            effect_id, flat_addr));

    DecodedEffectScript script;
    script.effect_id   = effect_id;
    script.mep_pointer = mep_pointer;
    script.script_flat = flat_addr;

    uint32_t pos = flat_addr;
    for (uint32_t byte_count = 0; byte_count < MAX_SCRIPT_LENGTH; ++byte_count) {
        if (pos >= static_cast<uint32_t>(rom.size()))
            return fail(std::format(
                "effect {}: script at 0x{:05X} ran past ROM end at byte {}",
                effect_id, flat_addr, byte_count));

        const uint8_t b = rom.read_byte(pos++);

        if (b == 0xFF) {
            // Endmove — normal script termination.
            script.bytes.push_back({b});
            return script;
        }

        if (b == 0xFE) {
            // Endturn — phase boundary.  Retained in the byte stream.
            // Does NOT terminate decoding; the script continues until 0xFF.
            script.bytes.push_back({b});
            continue;
        }

        // Regular command byte.  Must be in [0x01, num_commands].
        if (b == 0x00 || b > num_commands) {
            return fail(std::format(
                "effect {}: invalid command byte 0x{:02X} at flat 0x{:05X} "
                "(valid range [0x01, 0x{:02X}])",
                effect_id, b, pos - 1, num_commands));
        }

        script.bytes.push_back({b});
    }

    // Exhausted MAX_SCRIPT_LENGTH bytes without finding 0xFF.
    return fail(std::format(
        "effect {}: script at 0x{:05X} exceeded max length {} without 0xFF terminator",
        effect_id, flat_addr, MAX_SCRIPT_LENGTH));
}

// ============================================================================
// decode_all
// ============================================================================
std::optional<EffectScriptCorpus> EffectScriptDecoder::decode_all(
    const RomData& rom,
    const ExtractionProfile& profile,
    const ResolvedTables& tables,
    EffectScriptDecodeError* out_error)
{
    auto fail = [&](std::string msg) -> std::optional<EffectScriptCorpus> {
        if (out_error) {
            out_error->message   = std::move(msg);
            out_error->effect_id = 0xFFFF;
        }
        return std::nullopt;
    };

    // ── Precondition: counts must be non-zero (fail-closed) ──────────────────
    const uint16_t num_effects  = profile.counts.num_move_effects;
    const uint16_t num_commands = profile.counts.num_effect_commands;

    if (num_effects == 0)
        return fail("decode_all: profile.counts.num_move_effects = 0 "
                    "(fail-closed; set explicitly in profile)");
    if (num_commands == 0)
        return fail("decode_all: profile.counts.num_effect_commands = 0 "
                    "(fail-closed; set explicitly in profile)");
    if (!tables.ok())
        return fail("decode_all: ResolvedTables is not ok() "
                    "(both mep_flat and bcp_flat must be non-zero)");

    // ── Validate MEP table bounds ────────────────────────────────────────────
    // MEP table: num_effects × 2 bytes LE.
    const uint32_t mep_table_bytes = static_cast<uint32_t>(num_effects) * 2u;
    if (tables.mep_flat + mep_table_bytes > static_cast<uint32_t>(rom.size()))
        return fail(std::format(
            "decode_all: MEP table at 0x{:05X} ({} × 2 = {} bytes) "
            "extends past ROM end (0x{:05X})",
            tables.mep_flat, num_effects, mep_table_bytes, rom.size()));

    // ── Validate BCP table bounds ────────────────────────────────────────────
    // BCP table: num_commands × 2 bytes LE.
    const uint32_t bcp_table_bytes = static_cast<uint32_t>(num_commands) * 2u;
    if (tables.bcp_flat + bcp_table_bytes > static_cast<uint32_t>(rom.size()))
        return fail(std::format(
            "decode_all: BCP table at 0x{:05X} ({} × 2 = {} bytes) "
            "extends past ROM end (0x{:05X})",
            tables.bcp_flat, num_commands, bcp_table_bytes, rom.size()));

    // ── Decode each effect script ────────────────────────────────────────────
    EffectScriptCorpus corpus;
    corpus.scripts.reserve(num_effects);

    for (uint16_t eid = 0; eid < num_effects; ++eid) {
        // Read the 2-byte LE pointer from MEP[eid].
        const uint32_t ptr_offset = tables.mep_flat + static_cast<uint32_t>(eid) * 2u;
        const uint16_t bank_rel =
              static_cast<uint16_t>(rom.read_byte(ptr_offset))
            | (static_cast<uint16_t>(rom.read_byte(ptr_offset + 1u)) << 8);

        // Validate the pointer falls within the ROMX bank window.
        if (!is_valid_bank_pointer(tables.mep_bank, bank_rel)) {
            if (out_error) {
                out_error->message = std::format(
                    "decode_all: MEP[{}] bank-relative pointer 0x{:04X} is outside "
                    "valid ROMX range [0x4000, 0x7FFF]",
                    eid, bank_rel);
                out_error->effect_id = eid;
            }
            return std::nullopt;
        }

        const uint32_t script_flat = bank_rel_to_flat(tables.mep_bank, bank_rel);

        // Decode the script.
        EffectScriptDecodeError per_effect_err;
        auto maybe = decode_one(rom, eid, bank_rel, script_flat,
                                num_commands, &per_effect_err);
        if (!maybe) {
            if (out_error) *out_error = per_effect_err;
            return std::nullopt;
        }
        corpus.scripts.push_back(std::move(*maybe));
    }

    return corpus;
}

} // namespace crystal
