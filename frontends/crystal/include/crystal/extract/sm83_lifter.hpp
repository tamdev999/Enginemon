#pragma once
// frontends/crystal/include/crystal/extract/sm83_lifter.hpp
//
// SM83 static analysis framework for Crystal battle parameter extraction.
//
// PURPOSE: Decode known SM83 routine shapes from ROM bytes and extract
// semantic parameters (divisors, addends, multipliers) without runtime
// SM83 execution.
//
// DESIGN RULES:
//   - Recognition is opcode-structure driven, NOT value-driven.
//     A recognizer anchors on the instruction types surrounding a parameter,
//     not on the expected value.  A hack changing +10→+15 is recognized and
//     returns 15; an unrecognized shape fails with an explicit error.
//   - Every recognizer is fail-closed: unexpected opcode → error, no fallback.
//   - SM83 knowledge is strictly frontend-local.  The caller receives only
//     semantic numeric values.  No opcodes, registers, or ROM addresses leak
//     past the recognizer boundary.
//   - All recognizers operate on contiguous ROM byte spans.

#include "crystal/rom/loader.hpp"
#include <cstdint>
#include <optional>
#include <string>

namespace crystal {

// ============================================================================
// SM83 opcode constants used only by recognizers — never exported to engine
// ============================================================================
namespace SM83 {
    // Single-byte instructions
    static constexpr uint8_t LD_A_HL_IND  = 0x7E;  // ld a, [hl]
    static constexpr uint8_t LD_HL_A      = 0x77;  // ld [hl], a
    static constexpr uint8_t RET          = 0xC9;  // ret
    static constexpr uint8_t RRCA         = 0x0F;  // rrca (rotate a right)
    static constexpr uint8_t INC_HL_IND   = 0x34;  // inc [hl]
    static constexpr uint8_t PREFIX_CB    = 0xCB;  // CB prefix for extended ops
    static constexpr uint8_t SRL_C        = 0x39;  // (after CB) srl c
    static constexpr uint8_t RR_C         = 0x19;  // (after CB) rr c
    static constexpr uint8_t SRL_B        = 0x38;  // (after CB) srl b
    static constexpr uint8_t RR_B         = 0x18;  // wait — this is 0x18 = jr n; use LD_HL_IND_N
    // Two-byte instructions: opcode + immediate
    static constexpr uint8_t ADD_A_N      = 0xC6;  // add a, n
    static constexpr uint8_t CP_N         = 0xFE;  // cp n
    static constexpr uint8_t LD_A_N       = 0x3E;  // ld a, n
    static constexpr uint8_t LD_B_N       = 0x06;  // ld b, n
    static constexpr uint8_t LD_C_N       = 0x0E;  // ld c, n
    static constexpr uint8_t LD_HL_IND_N  = 0x36;  // ld [hl], n
    // Three-byte instructions: opcode + 2-byte address
    static constexpr uint8_t CALL         = 0xCD;  // call nn
}  // namespace SM83

// ============================================================================
// Recognition result
// ============================================================================
struct LiftResult {
    bool      ok      = false;
    std::string error;

    // Extracted parameter values (filled by recognizer)
    uint8_t  p[8]{};   // up to 8 extracted byte-sized parameters
    uint8_t  n_params = 0;

    static LiftResult fail(const std::string& msg) {
        LiftResult r; r.ok = false; r.error = msg; return r;
    }
    static LiftResult pass(std::initializer_list<uint8_t> vals) {
        LiftResult r; r.ok = true;
        for (uint8_t v : vals) r.p[r.n_params++] = v;
        return r;
    }
};

// ============================================================================
// ROM byte span helper — thin view over already-loaded ROM bytes
// ============================================================================
struct RomSpan {
    const uint8_t* data;
    uint32_t       size;
    uint32_t       base_flat;  // flat ROM address of data[0]

    uint8_t at(uint32_t offset) const {
        if (offset >= size) return 0xFF;
        return data[offset];
    }
    // Verify exact opcode sequence at given offset.
    // Returns false if any byte mismatches.
    bool match(uint32_t offset, std::initializer_list<uint8_t> expected) const {
        uint32_t i = offset;
        for (uint8_t b : expected) {
            if (i >= size || data[i] != b) return false;
            ++i;
        }
        return true;
    }
    // Count consecutive identical opcodes from offset.
    uint32_t count_opcode(uint32_t offset, uint8_t opcode, uint32_t max = 16) const {
        uint32_t n = 0;
        while (n < max && (offset + n) < size && data[offset + n] == opcode) ++n;
        return n;
    }
    // Read the 2-byte call target at a CALL nn instruction (little-endian).
    uint16_t call_target(uint32_t call_offset) const {
        if (call_offset + 2 >= size) return 0;
        return static_cast<uint16_t>(data[call_offset + 1])
             | (static_cast<uint16_t>(data[call_offset + 2]) << 8);
    }
};

// ============================================================================
// Build a RomSpan from the ROM, given bank:address and desired length.
// Returns {nullptr,0,0} if out of bounds.
// ============================================================================
RomSpan rom_span_at(const RomData& rom, uint32_t flat_address, uint32_t length);

// ============================================================================
// Individual routine recognizers
// ============================================================================

// --- AIDiscourageMove ---
// Shape: ld a,[hl] | add a, N | ld [hl],a | ret
// Extracts: N = discouragement delta (vanilla: 10)
// Fails if shape does not match exactly.
LiftResult lift_ai_discourage_move(const RomSpan& span);

// --- AIChooseMove init score ---
// Shape: ... ld a, N1 | ld [hl_reg], a | ld [hl_reg+1], a | ... (four stores)
// Extracts: N1 = init score (vanilla: 20)
// Anchors on pair of ld a, N followed by four consecutive ld [hli], a
LiftResult lift_ai_choose_move_scores(const RomSpan& span);

// --- GiveExperiencePoints exp divisor ---
// Shape: ... ld a, N | ldh [hDivisor], a | ld b, 4 | call Divide ...
// Extracts: N = experience base divisor (vanilla: 7)
LiftResult lift_exp_divisor(const RomSpan& span);

// --- BattleCommand_DamageVariation ---
// Shape: ... RRCA | cp N | jr c, .loop ...
// Extracts: N = lower bound byte (vanilla: 0xD9 = 85 percent+1)
LiftResult lift_damage_variation(const RomSpan& span);

// --- PokeBallEffect capture status bonus ---
// Shape: ... and mask | ld c, N1 | jr nz, .add | and a | ld c, N2 | jr nz | ld c, N3 ...
// Extracts: N1 = SLP/FRZ bonus (vanilla: 10), N2 = BRN/PSN/PAR (vanilla: 5, bug), N3 = none (0)
LiftResult lift_capture_status_bonus(const RomSpan& span);

// --- TryToRunAwayFromBattle escape constants ---
// Shape: ... ld a, M | ldh [hMultiplier], a | call Multiply | ...
//        ... ld b, A | ldh a, [hQuotient+3] | add b | ...
// Extracts: M = speed multiplier (vanilla: 32), A = per-attempt addend (vanilla: 30)
LiftResult lift_escape_constants(const RomSpan& span);

// --- CalcMonStatC stat formula offsets ---
// Shape: ... ld a, D | ldh [hDivisor], a | ld a, 3 | ld b, a | call Divide |
//        ... ld a, P | ld b, a | ldh a, [hQuotient+3] | add b | ...
//        ... ld a, Q | ld b, a | ... (HP offset, non-HP offset)
// Extracts: D = /100, P = HP offset (vanilla: 10), Q = non-HP offset (vanilla: 5)
LiftResult lift_stat_formula_offsets(const RomSpan& span);

// --- BattleCommand_DamageCalc formula constants ---
// Shape: ... ld a, D1 | ... | call Divide | ... | inc [hl] ×N | ...
//         ... | ld [hl], D2 | ld b, 4 | call Divide | ... | add a, F | ld [hld], a ...
// Extracts: D1=/5, N=+2 count, D2=/50, F=MIN_DAMAGE addend
LiftResult lift_damage_calc_constants(const RomSpan& span);

// --- GetEighthMaxHP / GetSixteenthMaxHP shift-count derivation ---
// Shape: call GetQuarterMaxHP | (srl c)×K | min-1 check | ret
// Verifies the shape and derives the fraction 1/(4 × 2^K):
//   K=1 → /8,  K=2 → /16
// Returns the denominator (8 or 16) as p[0].
LiftResult lift_residual_fraction(const RomSpan& span);

// --- BattleCommand_Critical crit stage deltas ---
// Shape: ...(Lucky Punch) ld c, 2 | ... (Scope Lens) inc c | ... (FocusEnergy) inc c |
//        ... (High crit)  inc c; inc c  (=+2)
// Extracts: high_crit_delta, scope_lens_delta, lucky_punch_delta, focus_energy_delta
LiftResult lift_crit_stage_deltas(const RomSpan& span);

// ============================================================================
// SM83 ROUTINE CANDIDATE SEARCH
//
// When a profile does not have a vanilla address for an SM83 routine (field=0)
// or the vanilla address is wrong for a relocated ROM, sm83_locate_candidates()
// scans the entire ROM for structural byte patterns that match the routine's
// known signature, then validates each candidate with the strict recognizer.
//
// Each recognizer has a distinct opcode pattern. The search looks for that
// pattern, building a list of candidate flat addresses where the recognizer
// would succeed. The final recognizer remains strict and fail-closed — no
// fuzzy matching occurs. Only exact structural matches pass.
//
// Usage:
//   auto candidates = sm83_locate_candidates<lift_ai_discourage_move>(rom);
//   for (uint32_t addr : candidates) {
//       auto span = rom_span_at(rom, addr, SM83_SPAN_AI_DISCOURAGE);
//       auto result = lift_ai_discourage_move(span);
//       // result.ok == true guaranteed by the scan filter
//   }
// ============================================================================

struct Sm83Candidate {
    uint32_t flat_address;
    LiftResult lift_result;   // Always ok=true (failed candidates are excluded)
};

// Locate AIDiscourageMove candidates: 7E C6 NN 77 C9  (NN in [1,50])
// Strict: exact 5-byte pattern, all bytes checked.
std::vector<Sm83Candidate> sm83_find_ai_discourage_move(const RomData& rom);

// Locate DamageVariation candidates: 0F FE NN 38 xx  (NN >= 0x80)
// Strict: RRCA / CP N / JR C pattern.
std::vector<Sm83Candidate> sm83_find_damage_variation(const RomData& rom);

// Locate ExpDivisor candidates: 3E NN E0 B7 06 04 CD  (NN in [1,20])
// Strict: LD A,N / LDH [hDivisor],a / LD B,4 / CALL pattern.
std::vector<Sm83Candidate> sm83_find_exp_divisor(const RomData& rom);

// Locate GetEighthMaxHP candidates: CD ?? ?? CB 39  (call + exactly 1 srl c)
// Strict: CALL followed by exactly one SRL C; denominator must be 8.
std::vector<Sm83Candidate> sm83_find_eighth_max_hp(const RomData& rom);

// Locate GetSixteenthMaxHP candidates: CD ?? ?? CB 39 CB 39  (call + 2 srl c)
// Strict: CALL followed by exactly two SRL C; denominator must be 16.
std::vector<Sm83Candidate> sm83_find_sixteenth_max_hp(const RomData& rom);

// Generic helper: given a profile sm83 address and a find function,
// return the first candidate address that passes the strict recognizer.
// If profile_address != 0, tries that address first (span-based).
// Then runs the structural scan if needed.
// Returns 0 if no valid candidate is found.
using Sm83FindFn = std::vector<Sm83Candidate>(*)(const RomData&);

uint32_t sm83_resolve_address(
    const RomData& rom,
    uint32_t profile_address,
    uint32_t span_size,
    Sm83FindFn find_fn,
    const char* routine_name,
    std::string* out_diagnostic = nullptr);

}  // namespace crystal
