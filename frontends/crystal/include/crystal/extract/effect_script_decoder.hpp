#pragma once
// frontends/crystal/include/crystal/extract/effect_script_decoder.hpp
//
// Compiler-side Crystal effect-script decoder.
//
// PURPOSE
//   Decode all Crystal move-effect scripts from ROM bytes into a typed
//   compiler-side representation, without executing any SM83 code.
//
// PIPELINE POSITION
//   ROM bytes
//   → EffectScriptDecoder::resolve_tables()   — locate MEP and BCP from ROM
//   → EffectScriptDecoder::decode_all()        — decode 157 effect scripts
//   → EffectScriptCorpus                       — typed decoded representation
//   → (future) semanticization pass
//
// SCOPE
//   Compiler-side only.  Nothing here is serialised to the EMON package or
//   used at runtime.  Crystal raw opcode values do not cross this boundary.
//
// DESIGN RULES
//   - num_move_effects = 0 or num_effect_commands = 0 → hard failure,
//     no default fallback.
//   - Every MEP pointer is range-checked before a read.
//   - Every command byte is validated ≤ num_effect_commands before storage.
//   - 0xFF (endmove)  = script terminator.
//   - 0xFE (endturn)  = phase boundary; retained in output, does NOT terminate
//     the data read.  The script continues until the next 0xFF.
//   - A script that exceeds MAX_SCRIPT_LENGTH bytes without a 0xFF terminator
//     is rejected as malformed.
//   - Table address resolution policy (both MEP and BCP):
//       profile = 0              → use unique scan result; fail if 0 or >1 hits
//       profile != 0, scan == profile → accept
//       profile != 0, scan != profile → hard failure (stale profile / relocated ROM)
//       scan yields 0 candidates → hard failure (ROM not recognised)
//       scan yields > 1 candidates → hard failure (ambiguous)
//   - "scan wins with diagnostic" is NOT acceptable for dispatcher tables.
//     Dispatcher evidence is deterministic; contradiction means the profile
//     is wrong or the ROM is not the expected version.
//
// SOURCE AUTHORITY
//   Crystal v1.1 symbols (pokecrystal11.sym):
//     MoveEffectsPointers  09:71f4   — 157 × 2-byte LE bank-relative pointers
//     BattleCommandPointers 0f:7d28  — 175 × 2-byte LE bank-relative pointers
//     MoveEffects          09:732e   — effect script data (all scripts, bank 9)
//   Consumer patterns verified from engine/battle/effect_commands.asm.

#include "crystal/rom/loader.hpp"
#include "crystal/rom/profile.hpp"

#include <cstdint>
#include <string>
#include <vector>
#include <optional>

namespace crystal {

// ============================================================================
// EffectCommandByte
//
// A single byte in a decoded effect script.
// Crystal effect scripts are a flat stream of 1-byte opcode values with no
// per-opcode operands.  Two sentinel values carry structural meaning:
//   0xFF = ENDMOVE  — terminates the script (last byte of every script)
//   0xFE = ENDTURN  — phase-boundary marker within multi-phase scripts
//                     (e.g. Skull Bash); retained in the decoded stream
//                     but does NOT terminate decoding.
// All other bytes are command indices into BattleCommandPointers (1..0xAF).
//
// The raw byte is kept here rather than mapping to a named enum so that
// the decoder has zero knowledge of Crystal-version-specific command
// semantics.  The name mapping is left to the semanticization pass.
// ============================================================================
struct EffectCommandByte {
    uint8_t value = 0;

    bool is_endmove()          const { return value == 0xFF; }
    bool is_endturn()          const { return value == 0xFE; }
    bool is_phase_boundary()   const { return value == 0xFE; }
    bool is_command()          const { return value >= 0x01 && value <= 0xAF; }

    // Zero-based index into BattleCommandPointers (value - 1).
    // Only valid when is_command() is true.
    uint8_t command_index()    const { return static_cast<uint8_t>(value - 1); }
};

// ============================================================================
// DecodedEffectScript
//
// A single fully-decoded Crystal move-effect script.
//
// Fields:
//   effect_id      — index into MoveEffectsPointers (0-based)
//   mep_pointer    — the bank-relative address read from MoveEffectsPointers
//   script_flat    — flat ROM address where the script bytes begin
//   bytes          — the decoded byte sequence INCLUDING the terminal 0xFF
//
// The byte sequence retains 0xFE (endturn) phase boundaries so that the
// semanticization pass can correctly reconstruct multi-phase effect scripts
// such as Skull Bash.
// ============================================================================
struct DecodedEffectScript {
    uint16_t effect_id   = 0;    // index into MoveEffectsPointers
    uint16_t mep_pointer = 0;    // bank-relative pointer from MEP table
    uint32_t script_flat = 0;    // flat ROM address of first script byte
    std::vector<EffectCommandByte> bytes;  // includes terminal 0xFF

    // Returns true if the script terminates normally (last byte is 0xFF).
    // Always true for correctly decoded scripts; included for assertion support.
    bool is_terminated() const {
        return !bytes.empty() && bytes.back().is_endmove();
    }

    // Number of command bytes (excluding phase boundaries and the terminal).
    size_t command_count() const {
        size_t n = 0;
        for (const auto& b : bytes) if (b.is_command()) ++n;
        return n;
    }

    // True if the script contains at least one endturn phase boundary.
    bool is_multi_phase() const {
        for (const auto& b : bytes) if (b.is_phase_boundary()) return true;
        return false;
    }
};

// ============================================================================
// ResolvedTables
//
// The result of EffectScriptDecoder::resolve_tables().
// Carries the confirmed flat addresses of both tables plus the bank values
// required to convert bank-relative script pointers to flat addresses.
// ============================================================================
struct ResolvedTables {
    uint32_t mep_flat  = 0;    // flat address of MoveEffectsPointers[0]
    uint8_t  mep_bank  = 0;    // ROM bank containing MEP and the effect scripts
    uint32_t bcp_flat  = 0;    // flat address of BattleCommandPointers[0]
    uint8_t  bcp_bank  = 0;    // ROM bank containing BCP

    // Non-empty when resolve_tables() failed.  Describes the exact failure
    // reason: missing scan result, multiple candidates, or profile/scan mismatch.
    std::string error;

    bool ok() const { return mep_flat != 0 && bcp_flat != 0 && error.empty(); }
};

// ============================================================================
// EffectScriptCorpus
//
// The complete set of decoded effect scripts for one ROM.
// ============================================================================
struct EffectScriptCorpus {
    std::vector<DecodedEffectScript> scripts;  // indexed by effect_id

    // Convenience: look up by effect_id.  Returns nullptr if out of range.
    const DecodedEffectScript* get(uint16_t effect_id) const {
        if (effect_id >= scripts.size()) return nullptr;
        return &scripts[effect_id];
    }

    size_t size() const { return scripts.size(); }
};

// ============================================================================
// EffectScriptDecodeError
//
// Hard failure description returned when decoding cannot complete.
// ============================================================================
struct EffectScriptDecodeError {
    std::string message;
    // If the error is per-effect, effect_id is set; otherwise 0xFFFF.
    uint16_t effect_id = 0xFFFF;
};

// ============================================================================
// EffectScriptDecoder
//
// Stateless decoder.  All methods are static.
//
// Usage:
//   auto tables = EffectScriptDecoder::resolve_tables(rom, profile);
//   if (!tables.ok()) { /* handle */ }
//   auto result = EffectScriptDecoder::decode_all(rom, profile, tables);
//   if (!result) { /* handle error */ }
//   const auto& corpus = *result;
// ============================================================================
class EffectScriptDecoder {
public:
    // Maximum number of bytes per effect script (including the 0xFF terminal).
    // Scripts exceeding this without a terminal are malformed.
    static constexpr uint32_t MAX_SCRIPT_LENGTH = 64;

    // -------------------------------------------------------------------------
    // resolve_tables
    //
    // Determines the flat ROM addresses of MoveEffectsPointers and
    // BattleCommandPointers.
    //
    // Resolution order for each table:
    //   1. Profile address non-zero → try it, read 4 bytes, verify they are
    //      valid pointer entries for the respective bank.
    //   2. Structural ROM scan → search for the dispatcher pattern.
    //   3. If neither yields a result → error.
    //
    // The profile address and scan result are compared; if they disagree a
    // diagnostic is stored in the returned ResolvedTables but the scan result
    // is used (scan has stronger authority than a potentially stale profile).
    // -------------------------------------------------------------------------
    static ResolvedTables resolve_tables(
        const RomData& rom,
        const ExtractionProfile& profile);

    // -------------------------------------------------------------------------
    // decode_all
    //
    // Decodes all num_move_effects effect scripts from ROM.
    //
    // Preconditions (all checked; returns nullopt with error on violation):
    //   - profile.counts.num_move_effects > 0
    //   - profile.counts.num_effect_commands > 0
    //   - tables.ok()
    //
    // Per-effect:
    //   - Reads the 2-byte LE pointer from MEP[effect_id].
    //   - Computes flat script address from bank + pointer.
    //   - Reads bytes until 0xFF, retaining 0xFE phase boundaries.
    //   - Validates each command byte is in [0x01, num_effect_commands].
    //   - Fails closed on any bounds or validation error.
    // -------------------------------------------------------------------------
    static std::optional<EffectScriptCorpus> decode_all(
        const RomData& rom,
        const ExtractionProfile& profile,
        const ResolvedTables& tables,
        EffectScriptDecodeError* out_error = nullptr);

    // -------------------------------------------------------------------------
    // decode_one
    //
    // Decodes a single effect script starting at flat_addr.
    // Used directly in tests and by decode_all.
    // num_commands is the valid command count (bytes in [1..num_commands] pass).
    // -------------------------------------------------------------------------
    static std::optional<DecodedEffectScript> decode_one(
        const RomData& rom,
        uint16_t effect_id,
        uint16_t mep_pointer,
        uint32_t flat_addr,
        uint16_t num_commands,
        EffectScriptDecodeError* out_error = nullptr);

    // -------------------------------------------------------------------------
    // scan_mep
    //
    // Scans the full ROM for the MoveEffectsPointers dispatcher pattern.
    // Pattern: 4F 06 00 21 lo hi 09 09 3E bank CD
    //
    // Returns ALL matching hits as a vector of {mep_flat, mep_bank} pairs.
    // Each candidate has passed the 5-entry table-validation filter.
    // Exactly one candidate is required; zero or multiple → resolve_tables fails.
    // -------------------------------------------------------------------------
    static std::vector<std::pair<uint32_t, uint8_t>> scan_mep(const RomData& rom);

    // -------------------------------------------------------------------------
    // scan_bcp
    //
    // Scans the full ROM for the BattleCommandPointers dispatcher pattern.
    // Pattern: 3D 4F 06 00 21 lo hi 09 09 C1 3E bank CD
    //
    // Returns ALL matching hits as a vector of {bcp_flat, bcp_bank} pairs.
    // Exactly one candidate is required; zero or multiple → resolve_tables fails.
    // -------------------------------------------------------------------------
    static std::vector<std::pair<uint32_t, uint8_t>> scan_bcp(const RomData& rom);

private:
    // Read a ROM byte at a flat address.  Returns 0xFF if OOB.
    static uint8_t rom_byte(const RomData& rom, uint32_t flat);

    // Convert a bank-relative address + bank to flat.
    // For ROMX banks: bank * 0x4000 + (bank_rel - 0x4000).
    static uint32_t bank_rel_to_flat(uint8_t bank, uint16_t bank_rel);

    // Validate that a flat address is a plausible pointer entry in the given bank.
    // Used to cross-check profile addresses against scan results.
    static bool is_valid_bank_pointer(uint8_t bank, uint16_t bank_rel);
};

} // namespace crystal
