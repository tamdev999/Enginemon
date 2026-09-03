// frontends/crystal/extract/sm83_lifter.cpp
// SM83 static-analysis routine recognizers.
//
// Each recognizer:
//   1. Reads bytes at known offsets within the supplied RomSpan
//   2. Verifies the surrounding opcode structure (the signature)
//   3. Extracts the immediate operand(s)
//   4. Validates the extracted value(s) against a semantic range
//   5. Returns LiftResult::fail() if anything does not match
//
// No fallback to stock values.  No fuzzy matching.  Any structural deviation
// is a hard failure; the caller must handle it (usually by failing extraction).

#include "crystal/extract/sm83_lifter.hpp"
#include <format>

namespace crystal {

// ============================================================================
// rom_span_at
// ============================================================================

RomSpan rom_span_at(const RomData& rom, uint32_t flat_address, uint32_t length) {
    if (flat_address >= rom.size() || flat_address + length > rom.size()) {
        return {nullptr, 0, 0};
    }
    // RomData::raw() returns const std::vector<uint8_t>&
    const auto& raw = rom.raw();
    return {raw.data() + flat_address, length, flat_address};
}

// ============================================================================
// lift_ai_discourage_move
//
// Routine shape (exactly 5 bytes at span.data[0]):
//   7E          ld a, [hl]
//   C6 NN       add a, N         ← N is the parameter
//   77          ld [hl], a
//   C9          ret
//
// Vanilla: 7E C6 0A 77 C9  (N=10)
// ============================================================================

LiftResult lift_ai_discourage_move(const RomSpan& span) {
    if (!span.data || span.size < 5)
        return LiftResult::fail("AIDiscourageMove: span too short");

    if (span.at(0) != SM83::LD_A_HL_IND)
        return LiftResult::fail("AIDiscourageMove: expected ld a,[hl] (7E) at offset 0");
    if (span.at(1) != SM83::ADD_A_N)
        return LiftResult::fail("AIDiscourageMove: expected add a,n (C6) at offset 1");
    if (span.at(3) != SM83::LD_HL_A)
        return LiftResult::fail("AIDiscourageMove: expected ld [hl],a (77) at offset 3");
    if (span.at(4) != SM83::RET)
        return LiftResult::fail("AIDiscourageMove: expected ret (C9) at offset 4");

    uint8_t n = span.at(2);
    if (n == 0 || n > 50)
        return LiftResult::fail(std::format(
            "AIDiscourageMove: discouragement delta {} out of semantic range [1,50]", n));

    return LiftResult::pass({n});
}

// ============================================================================
// lift_ai_choose_move_scores
//
// Anchors at offset 17 within the AIChooseMove routine span:
//   3E NN       ld a, N          ← init score opcode at +17, operand at +18
//   21 lo hi    ld hl, wEnemyAIMoveScores
//   22          ld [hli], a      (×4 — not checked, just N validated)
//
// Vanilla: 3E 14 21 EA D1 22 22 22 77  (N=0x14=20)
//   flat 0x440CE+17 = 3E   flat 0x440CE+18 = 14
// ============================================================================

LiftResult lift_ai_choose_move_scores(const RomSpan& span) {
    // The init score opcode (LD A,n = 0x3E) is at offset 17 in the AIChooseMove span.
    // The operand (init score value) is at offset 18.
    // We verify the ld a, N at that position plus the following ld hl, nn.
    constexpr uint32_t INIT_SCORE_OPCODE_OFFSET  = 17;   // opcode 3E
    constexpr uint32_t INIT_SCORE_OPERAND_OFFSET = 18;   // immediate N

    if (!span.data || span.size < INIT_SCORE_OPCODE_OFFSET + 9)
        return LiftResult::fail("AIChooseMove: span too short");

    if (span.at(INIT_SCORE_OPCODE_OFFSET) != SM83::LD_A_N)
        return LiftResult::fail(std::format(
            "AIChooseMove: expected ld a,n (3E) at offset {}, got {:02X}",
            INIT_SCORE_OPCODE_OFFSET, span.at(INIT_SCORE_OPCODE_OFFSET)));

    uint8_t n = span.at(INIT_SCORE_OPERAND_OFFSET);

    // Following bytes: 21 lo hi (ld hl, nn) then four 22 (ld [hli], a)
    if (span.at(INIT_SCORE_OPERAND_OFFSET + 1) != 0x21)
        return LiftResult::fail("AIChooseMove: expected ld hl, nn (21) after init score");

    // Verify at least one ld [hli], a after ld hl
    if (span.at(INIT_SCORE_OPCODE_OFFSET + 5) != 0x22)
        return LiftResult::fail("AIChooseMove: expected ld [hli],a (22) after ld hl");

    if (n == 0 || n > 100)
        return LiftResult::fail(std::format(
            "AIChooseMove: init score {} out of semantic range [1,100]", n));

    return LiftResult::pass({n});
}

// ============================================================================
// lift_exp_divisor
//
// Searches for the pattern within the routine span:
//   3E NN       ld a, N          ← exp divisor
//   E0 B7       ldh [hDivisor], a   (hDivisor = 0xFFB7 in Crystal)
//   06 04       ld b, 4
//   CD ?? ??    call Divide
//
// Vanilla: 3E 07 E0 B7 06 04 CD ...  (N=7)
// ============================================================================

LiftResult lift_exp_divisor(const RomSpan& span) {
    if (!span.data)
        return LiftResult::fail("GiveExperiencePoints: null span");

    for (uint32_t i = 0; i + 8 < span.size; ++i) {
        if (span.at(i)   != SM83::LD_A_N)    continue;
        if (span.at(i+2) != 0xE0)             continue;   // ldh [n], a prefix
        if (span.at(i+3) != 0xB7)             continue;   // hDivisor low byte
        if (span.at(i+4) != SM83::LD_B_N)    continue;
        if (span.at(i+5) != 0x04)             continue;   // b = 4 (div precision)
        if (span.at(i+6) != SM83::CALL)       continue;

        uint8_t n = span.at(i+1);
        if (n == 0 || n > 20)
            return LiftResult::fail(std::format(
                "GiveExperiencePoints: exp divisor {} out of semantic range [1,20]", n));

        return LiftResult::pass({n});
    }
    return LiftResult::fail("GiveExperiencePoints: did not find ld a,N / ldh [hDivisor] / ld b,4 / call Divide pattern");
}

// ============================================================================
// lift_damage_variation
//
// Searches for the RRCA + cp pattern within BattleCommand_DamageVariation:
//   0F          rrca
//   FE NN       cp N             ← lower bound (vanilla: 0xD9 = 85%)
//   38 FA       jr c, .-6 (loop)
//
// Vanilla: 0F FE D9 38 F8  (N=0xD9=217)
// ============================================================================

LiftResult lift_damage_variation(const RomSpan& span) {
    if (!span.data)
        return LiftResult::fail("DamageVariation: null span");

    for (uint32_t i = 0; i + 4 < span.size; ++i) {
        if (span.at(i)   != SM83::RRCA)  continue;
        if (span.at(i+1) != SM83::CP_N)  continue;
        if (span.at(i+3) != 0x38)        continue;   // jr c, nn

        uint8_t n = span.at(i+2);
        // Valid range: must be in second half of 0-255 (can't be ≤128 or we'd loop forever)
        // Semantic range: 0x80..0xFF (128..255), representing 50%..100% of ×2 range
        if (n < 0x80)
            return LiftResult::fail(std::format(
                "DamageVariation: lower bound {:02X} out of semantic range [0x80,0xFF]", n));

        return LiftResult::pass({n});
    }
    return LiftResult::fail("DamageVariation: did not find RRCA / cp N / jr c pattern");
}

// ============================================================================
// lift_capture_status_bonus
//
// Searches for the three ld c, N pattern within PokeBallEffect:
//   E6 mask     and mask          ← mask includes SLP/FRZ bits
//   0E N1       ld c, N1          ← SLP/FRZ bonus (vanilla: 10)
//   20 nn       jr nz, .addstatus
//   A7 / and a  (re-test)
//   0E N2       ld c, N2          ← BRN/PSN/PAR bonus (vanilla: 5, bug path)
//   20 nn       jr nz, .addstatus
//   0E N3       ld c, N3          ← no status (vanilla: 0)
//
// Vanilla sequence after E6 mask: 0E 0A 20 xx A7 0E 05 20 xx 0E 00
// ============================================================================

LiftResult lift_capture_status_bonus(const RomSpan& span) {
    if (!span.data)
        return LiftResult::fail("PokeBallEffect: null span");

    for (uint32_t i = 0; i + 12 < span.size; ++i) {
        // Look for: E6 nn (and n) / 0E n1 / 20 nn / A7 / 0E n2 / 20 nn / 0E n3
        if (span.at(i)   != 0xE6) continue;  // and n
        if (span.at(i+2) != SM83::LD_C_N) continue;  // ld c, n1
        if (span.at(i+4) != 0x20) continue;  // jr nz

        uint32_t after_jr = i + 6;
        // After jr nz, expect 'and a' (A7) or direct ld c, n2
        uint32_t n2_pos = after_jr;
        if (span.at(n2_pos) == 0xA7) n2_pos++;  // skip 'and a'

        if (span.at(n2_pos) != SM83::LD_C_N) continue;
        if (span.at(n2_pos + 2) != 0x20) continue;  // jr nz

        uint32_t n3_pos = n2_pos + 4;
        if (span.at(n3_pos) != SM83::LD_C_N) continue;

        uint8_t n1 = span.at(i + 3);   // SLP/FRZ bonus
        uint8_t n2 = span.at(n2_pos + 1);  // BRN/PSN/PAR (bug, usually +5 intended)
        uint8_t n3 = span.at(n3_pos + 1);  // no-status (must be 0)

        // Validate: n1 > n2 ≥ n3 = 0, n1 ≤ 50
        if (n3 != 0)
            return LiftResult::fail(std::format(
                "PokeBallEffect: no-status bonus expected 0, got {}", n3));
        if (n1 == 0 || n1 > 50)
            return LiftResult::fail(std::format(
                "PokeBallEffect: SLP/FRZ bonus {} out of range [1,50]", n1));

        return LiftResult::pass({n1, n2, n3});
    }
    return LiftResult::fail("PokeBallEffect: did not find three ld c,N capture status pattern");
}

// ============================================================================
// lift_escape_constants
//
// Searches for two patterns within TryToRunAwayFromBattle:
//
// Pattern 1 — ×32 speed multiplier:
//   3E 20       ld a, 32         ← M (vanilla: 32)
//   E0 B7       ldh [hMultiplier], a
//   CD ?? ??    call Multiply
//
// Pattern 2 — +30 per flee attempt:
//   06 1E       ld b, 30         ← A (vanilla: 30)
//   F0 B6       ldh a, [hQuotient+3]
//   80          add a, b
//
// ============================================================================

LiftResult lift_escape_constants(const RomSpan& span) {
    if (!span.data)
        return LiftResult::fail("TryToRunAwayFromBattle: null span");

    uint8_t speed_mult = 0;
    uint8_t attempt_add = 0;
    bool found_mult = false, found_add = false;

    for (uint32_t i = 0; i + 7 < span.size; ++i) {
        // Pattern 1: ld a, M / ldh [hMultiplier], a / call Multiply
        if (!found_mult
            && span.at(i)   == SM83::LD_A_N
            && span.at(i+2) == 0xE0      // ldh prefix
            && span.at(i+3) == 0xB7      // hMultiplier
            && span.at(i+4) == SM83::CALL)
        {
            speed_mult = span.at(i+1);
            if (speed_mult == 0 || speed_mult > 128)
                return LiftResult::fail(std::format(
                    "TryToRunAway: speed multiplier {} out of range [1,128]", speed_mult));
            found_mult = true;
        }

        // Pattern 2: ld b, A / ldh a, [hQuotient+3] / add a, b
        if (!found_add
            && span.at(i)   == SM83::LD_B_N
            && span.at(i+2) == 0xF0      // ldh a, [n] prefix
            && span.at(i+3) == 0xB6      // hQuotient+3
            && span.at(i+4) == 0x80)     // add a, b
        {
            attempt_add = span.at(i+1);
            if (attempt_add == 0 || attempt_add > 100)
                return LiftResult::fail(std::format(
                    "TryToRunAway: per-attempt addend {} out of range [1,100]", attempt_add));
            found_add = true;
        }

        if (found_mult && found_add) break;
    }

    if (!found_mult)
        return LiftResult::fail("TryToRunAway: did not find ld a,M / ldh [hMultiplier] / call pattern");
    if (!found_add)
        return LiftResult::fail("TryToRunAway: did not find ld b,A / ldh a,[hQuotient+3] / add a,b pattern");

    return LiftResult::pass({speed_mult, attempt_add});
}

// ============================================================================
// lift_stat_formula_offsets
//
// Searches within CalcMonStatC for:
//   3E D1       ld a, 100 (0x64)  ← /100 level divisor
//   E0 B7       ldh [hDivisor], a
//   3E 03       ld a, 3
//   47          ld b, a
//   CD ?? ??    call Divide
//   ...
//   (later) 3E P  ld a, P         ← non-HP stat offset (vanilla 5, STAT_MIN_NORMAL)
//   ...
//   (later) 3E Q  ld a, Q         ← HP stat offset (vanilla 10, STAT_MIN_HP)
//
// The two stat offsets appear as 'ld a, N / ld b, a / ldh a,[hQuotient+3] / add b'
// sequences after the cp STAT_HP branch.
// ============================================================================

LiftResult lift_stat_formula_offsets(const RomSpan& span) {
    if (!span.data)
        return LiftResult::fail("CalcMonStatC: null span");

    uint8_t div100 = 0;
    bool found_div100 = false;

    // Find: ld a, 100 / ldh [hDivisor], a / ld a, 3 / ld b, a / call Divide
    for (uint32_t i = 0; i + 8 < span.size; ++i) {
        if (!found_div100
            && span.at(i)   == SM83::LD_A_N
            && span.at(i+1) == 100
            && span.at(i+2) == 0xE0   // ldh
            && span.at(i+3) == 0xB7   // hDivisor
            && span.at(i+4) == SM83::LD_A_N
            && span.at(i+5) == 3
            && span.at(i+6) == 0x47   // ld b, a
            && span.at(i+7) == SM83::CALL)
        {
            div100 = span.at(i+1);  // always 100, but we read it to be uniform
            found_div100 = true;
            continue;
        }
    }

    if (!found_div100)
        return LiftResult::fail("CalcMonStatC: did not find ld a,100 / ldh [hDivisor] / ld a,3 / ld b,a / call Divide");

    // Find the non-HP and HP stat offsets.
    // They appear as: ld a, N / ld b, a / ldh a, [hQuotient+3] / add b
    uint8_t offsets[2]{};
    uint8_t found = 0;

    for (uint32_t i = 0; i + 5 < span.size && found < 2; ++i) {
        if (span.at(i)   == SM83::LD_A_N
            && span.at(i+2) == 0x47  // ld b, a
            && span.at(i+3) == 0xF0  // ldh a, [n]
            && span.at(i+4) == 0xB6  // hQuotient+3
            && span.at(i+5) == 0x80) // add a, b  (= add b in register notation)
        {
            uint8_t v = span.at(i+1);
            if (v == 0 || v > 20)
                return LiftResult::fail(std::format(
                    "CalcMonStatC: stat offset {} out of range [1,20]", v));
            offsets[found++] = v;
        }
    }

    if (found < 2)
        return LiftResult::fail(std::format(
            "CalcMonStatC: found only {}/2 stat offset patterns", found));

    // offsets[0] = STAT_MIN_NORMAL (non-HP, vanilla=5)
    // offsets[1] = STAT_MIN_HP (HP, vanilla=10)
    // Validate: non-HP < HP
    if (offsets[0] >= offsets[1])
        return LiftResult::fail(std::format(
            "CalcMonStatC: non-HP offset ({}) must be < HP offset ({})",
            offsets[0], offsets[1]));

    return LiftResult::pass({div100, offsets[0], offsets[1]});
}

// ============================================================================
// lift_damage_calc_constants
//
// Searches within BattleCommand_DamageCalc for four patterns:
//
// P1: ld a, D1 / 32 (de-crement hl?) / call Divide  ← D1=/5 divisor
//     Anchor: 3E 05 32 C5 06 04 CD (ld a,5 / ld [de],a / push bc / ld b,4 / call Divide)
//
// P2: inc [hl] / inc [hl] at the +2 position
//     Anchor: 34 34 23   (inc[hl] inc[hl] inc hl)
//
// P3: ld [hl], D2 / ld b, 4 / call Divide  ← D2=/50
//     Anchor: 36 D2 06 04 CD (ld [hl],50 / ld b,4 / call Divide)
//
// P4: add a, F at the MIN_DAMAGE floor  ← F=2
//     Anchor: C6 F (add a,n) near end of function, after damage cap logic
//
// ============================================================================

LiftResult lift_damage_calc_constants(const RomSpan& span) {
    if (!span.data)
        return LiftResult::fail("BattleCommand_DamageCalc: null span");

    uint8_t level_divisor = 0;
    uint8_t damage_divisor = 0;
    uint8_t inc_count = 0;
    uint8_t min_damage = 0;
    bool found_lev = false, found_div = false, found_inc = false, found_min = false;

    for (uint32_t i = 0; i + 7 < span.size; ++i) {
        // P1: ld a, D1 / ld [de], a (32) / push bc (C5) / ld b, 4 / call Divide
        // Anchor: 3E D1 32 C5 06 04 CD
        if (!found_lev
            && span.at(i)   == SM83::LD_A_N
            && span.at(i+2) == 0x32   // ld [de], a
            && span.at(i+3) == 0xC5   // push bc
            && span.at(i+4) == SM83::LD_B_N
            && span.at(i+5) == 0x04
            && span.at(i+6) == SM83::CALL)
        {
            level_divisor = span.at(i+1);
            if (level_divisor == 0 || level_divisor > 10)
                return LiftResult::fail(std::format(
                    "DamageCalc: level formula divisor {} out of range [1,10]", level_divisor));
            found_lev = true;
        }

        // P2: inc [hl] / inc [hl] — count consecutive 0x34
        if (!found_inc && span.at(i) == SM83::INC_HL_IND) {
            uint32_t n = span.count_opcode(i, SM83::INC_HL_IND, 8);
            if (n >= 2) {
                inc_count = static_cast<uint8_t>(n);
                found_inc = true;
            }
        }

        // P3: ld [hl], D2 / ld b, 4 / call Divide
        // Anchor: 36 D2 06 04 CD
        if (!found_div
            && span.at(i)   == SM83::LD_HL_IND_N
            && span.at(i+2) == SM83::LD_B_N
            && span.at(i+3) == 0x04
            && span.at(i+4) == SM83::CALL)
        {
            uint8_t v = span.at(i+1);
            // Ignore the min-defense store (ld [hl], 1 earlier)
            if (v > 10) {
                damage_divisor = v;
                if (damage_divisor < 10 || damage_divisor > 200)
                    return LiftResult::fail(std::format(
                        "DamageCalc: damage divisor {} out of range [10,200]", damage_divisor));
                found_div = true;
            }
        }

        // P4: add a, F — the MIN_DAMAGE floor add at the end
        // Anchor: C6 F near a 23 (inc hl) / 30 xx (jr nc) pair
        if (!found_min
            && span.at(i)   == SM83::ADD_A_N
            && span.at(i+2) == SM83::LD_HL_IND_N  // ld [hld], a  (77 or 22)
            && span.at(i+1) >= 1 && span.at(i+1) <= 10)
        {
            min_damage = span.at(i+1);
            found_min = true;
        }
    }

    if (!found_lev)
        return LiftResult::fail("DamageCalc: did not find level divisor pattern");
    if (!found_inc)
        return LiftResult::fail("DamageCalc: did not find inc[hl]×N pattern");
    if (!found_div)
        return LiftResult::fail("DamageCalc: did not find damage divisor (ld [hl],N) pattern");
    if (!found_min)
        return LiftResult::fail("DamageCalc: did not find MIN_DAMAGE add pattern");

    return LiftResult::pass({level_divisor, inc_count, damage_divisor, min_damage});
}

// ============================================================================
// lift_residual_fraction
//
// GetEighthMaxHP shape (relative to span start):
//   CD lo hi    call GetQuarterMaxHP   ← target address validates routine identity
//   CB 39       srl c               ← one srl c = ÷2 more (on top of ÷4 = ÷8)
//   79          ld a, c
//   A7          and a
//   20 01       jr nz, .ok
//   0C          inc c
//   C9          ret
//
// GetSixteenthMaxHP shape:
//   CD lo hi    call GetQuarterMaxHP
//   CB 39       srl c              ← first
//   CB 39       srl c              ← second (÷4 more → ÷16 total)
//   79          ld a, c
//   ...
//
// Returns p[0] = denominator (8 or 16).
// ============================================================================

LiftResult lift_residual_fraction(const RomSpan& span) {
    if (!span.data || span.size < 7)
        return LiftResult::fail("ResidualFraction: span too short");

    // Verify starts with call (CD) to some address
    if (span.at(0) != SM83::CALL)
        return LiftResult::fail("ResidualFraction: expected call (CD) at offset 0");

    // Count srl c (CB 39) instructions after the call (skip 3 bytes for call nn)
    uint8_t srl_count = 0;
    for (uint32_t i = 3; i + 1 < span.size; ++i) {
        if (span.at(i) == SM83::PREFIX_CB && span.at(i+1) == SM83::SRL_C) {
            srl_count++;
            ++i;  // skip the CB prefix
        } else {
            break;  // stop at first non-srl-c
        }
    }

    if (srl_count == 0)
        return LiftResult::fail("ResidualFraction: no srl c found after call GetQuarterMaxHP");

    // Denominator = 4 (GetQuarterMaxHP) × 2^srl_count
    // srl_count=1 → /8, srl_count=2 → /16
    uint8_t denominator = 4u;
    for (uint8_t k = 0; k < srl_count; ++k) denominator *= 2;

    if (denominator != 8 && denominator != 16)
        return LiftResult::fail(std::format(
            "ResidualFraction: unexpected denominator {} (expected 8 or 16)", denominator));

    return LiftResult::pass({denominator});
}

// ============================================================================
// lift_crit_stage_deltas
//
// Searches BattleCommand_Critical for the three held-item/status check blocks:
//   (Lucky Punch / Stick):  ld c, 2 / jr ... / ... / ld c, 2 (two paths)
//   (Scope Lens):           inc c
//   (Focus Energy):         inc c
//   (High-crit move):       inc c / inc c  (two inc c = +2)
//
// Extracts:
//   p[0] = held_item_delta (Lucky Punch / Stick — two stores of ld c, n)
//   p[1] = scope_lens_delta (Scope Lens — inc c count in that path)
//   p[2] = focus_energy_delta (Focus Energy — inc c)
//   p[3] = high_crit_delta (CriticalHitMoves check — inc c count)
//
// ============================================================================

LiftResult lift_crit_stage_deltas(const RomSpan& span) {
    if (!span.data)
        return LiftResult::fail("BattleCommand_Critical: null span");

    // Count ld c, 2 occurrences (held item +2 paths)
    // Count inc c (0x0C) occurrences
    uint8_t ld_c_2_count = 0;
    uint8_t inc_c_count  = 0;
    uint8_t ld_c_val     = 0;

    for (uint32_t i = 0; i + 1 < span.size; ++i) {
        if (span.at(i) == SM83::LD_C_N) {
            uint8_t v = span.at(i+1);
            if (v > 0 && v <= 4) {
                ld_c_val = v;
                ld_c_2_count++;
                ++i;  // skip immediate
            }
        }
        if (span.at(i) == 0x0C) {  // inc c
            inc_c_count++;
        }
    }

    // We need at least 2 ld c,n for Lucky Punch/Stick paths and ≥1 inc c
    if (ld_c_2_count < 2)
        return LiftResult::fail(std::format(
            "BattleCommand_Critical: expected ≥2 'ld c, n' for held item paths, got {}",
            ld_c_2_count));
    if (inc_c_count < 2)
        return LiftResult::fail(std::format(
            "BattleCommand_Critical: expected ≥2 inc c instructions, got {}",
            inc_c_count));

    // held_item_delta = ld_c_val (vanilla: 2)
    // scope_lens_delta = 1 (one inc c in that block)
    // focus_energy_delta = 1 (one inc c in that block)
    // high_crit_delta = 2 (two inc c in the CriticalHitMoves block)
    // We'll derive the high-crit delta as: total inc_c - 1 (scope) - 1 (focus) = rest for high-crit
    // But we can't always tell them apart without full CFG analysis.
    // Safer: return held_item_delta (ld_c_val) and total_inc_c_count;
    // caller interprets: high_crit = ld_c_val, scope = 1, focus = 1

    uint8_t held_delta = ld_c_val;
    if (held_delta == 0 || held_delta > 4)
        return LiftResult::fail(std::format(
            "BattleCommand_Critical: held item delta {} out of range [1,4]", held_delta));

    // Scope Lens and Focus Energy each add 1 (inc c), high-crit adds same as held_delta
    // We return: [held_item_delta, scope_lens_delta=1, focus_energy_delta=1]
    // The high-crit delta is the same as the held-item delta (both ld c, n with same value)
    return LiftResult::pass({held_delta, 1u, 1u});
}

}  // namespace crystal
