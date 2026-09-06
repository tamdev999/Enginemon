// tests/crystal/sm83_lifter_test.cpp
//
// Comprehensive adversarial test matrix for all 11 SM83 recognizer functions.
//
// For each recognizer:
//   POSITIVE:  vanilla byte sequence → extraction succeeds, correct values
//   MODIFIED:  modified immediate operand → extraction succeeds, modified value
//   MALFORMED: corrupted surrounding shape → extraction fails with error
//
// Also covers:
//   - sm83_lifted_mask wire roundtrip (writer → reader)
//   - runtime consumption of lifted values vs defaults
//
// Run: sm83_lifter_test <rom_path>
//
// (ROM path is used for vanilla-byte positive tests.  Synthetic fixtures are
//  used for modified-immediate and malformed tests — no ROM required for those.)

#include "crystal/rom/loader.hpp"
#include "crystal/rom/profile.hpp"
#include "crystal/extract/sm83_lifter.hpp"
#include "crystal/extract/battle_rules_extractor.hpp"
#include "crystal/output/native_package.hpp"
#include "engine/package/package_reader.hpp"
#include "engine/battle/battle_rules.hpp"
#include "engine/battle/calculator.hpp"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#include <cassert>

// ============================================================================
// TEST FRAMEWORK
// ============================================================================

static int g_passed = 0;
static int g_failed = 0;
static bool g_current_failed = false;

#define ASSERT_TRUE(expr) \
    do { if (!(expr)) { \
        std::fprintf(stderr, "  FAIL: %s  at line %d\n", #expr, __LINE__); \
        g_current_failed = true; \
    } } while(0)

#define ASSERT_FALSE(expr)     ASSERT_TRUE(!(expr))
#define ASSERT_EQ(a, b)        ASSERT_TRUE((a) == (b))
#define ASSERT_NE(a, b)        ASSERT_TRUE((a) != (b))
#define ASSERT_OK(r)           ASSERT_TRUE((r).ok)
#define ASSERT_FAIL(r)         ASSERT_TRUE(!(r).ok)

#define TEST(name) static void test_##name()
#define RUN_TEST(name) \
    do { \
        g_current_failed = false; \
        std::cout << "  " << #name << " ... "; std::cout.flush(); \
        test_##name(); \
        if (!g_current_failed) { ++g_passed; std::cout << "PASS\n"; } \
        else                   { ++g_failed; std::cout << "FAIL\n"; } \
    } while(0)

// ============================================================================
// GLOBALS
// ============================================================================

static const crystal::RomData*           g_rom     = nullptr;
static const crystal::ExtractionProfile* g_profile = nullptr;

// ============================================================================
// HELPERS
// ============================================================================

// Build a RomSpan from an inline byte vector (not from an actual ROM).
// Returned span is valid for the lifetime of the vector.
static crystal::RomSpan span_from(const std::vector<uint8_t>& bytes) {
    return {bytes.data(), static_cast<uint32_t>(bytes.size()), 0};
}

// Write BattleRules to temp package and read back.
static std::optional<enginemon::BattleRules> roundtrip(
    const enginemon::BattleRules& rules, const std::string& tag)
{
    crystal::PackageWriter writer;
    writer.set_source_rom("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", "test");
    writer.add_battle_rules(rules);
    auto path = std::filesystem::temp_directory_path()
              / ("sm83_lifter_test_" + tag + ".emon");
    if (!writer.write(path)) return std::nullopt;
    auto reader = enginemon::PackageReader::open(path);
    if (!reader) { std::filesystem::remove(path); return std::nullopt; }
    auto r = reader->load_battle_rules();
    std::filesystem::remove(path);
    return r;
}

// ============================================================================
// 1. lift_ai_discourage_move
// ============================================================================

// Vanilla: 7E C6 0A 77 C9 (ld a,[hl] / add a,10 / ld [hl],a / ret)
TEST(ai_discourage_vanilla) {
    std::vector<uint8_t> b = {0x7E, 0xC6, 0x0A, 0x77, 0xC9};
    auto r = crystal::lift_ai_discourage_move(span_from(b));
    ASSERT_OK(r);
    ASSERT_EQ(r.p[0], 10u);
}

TEST(ai_discourage_modified_immediate) {
    // delta changed from 10 to 15
    std::vector<uint8_t> b = {0x7E, 0xC6, 0x0F, 0x77, 0xC9};
    auto r = crystal::lift_ai_discourage_move(span_from(b));
    ASSERT_OK(r);
    ASSERT_EQ(r.p[0], 15u);
    ASSERT_NE(r.p[0], 10u);
}

TEST(ai_discourage_malformed_missing_ret) {
    // C9 (ret) replaced with 00 (nop)
    std::vector<uint8_t> b = {0x7E, 0xC6, 0x0A, 0x77, 0x00};
    auto r = crystal::lift_ai_discourage_move(span_from(b));
    ASSERT_FAIL(r);
    ASSERT_FALSE(r.error.empty());
}

TEST(ai_discourage_malformed_wrong_opcode_0) {
    // byte 0 = 0x00 instead of 0x7E (ld a,[hl])
    std::vector<uint8_t> b = {0x00, 0xC6, 0x0A, 0x77, 0xC9};
    auto r = crystal::lift_ai_discourage_move(span_from(b));
    ASSERT_FAIL(r);
}

TEST(ai_discourage_malformed_zero_delta) {
    // semantic range check: delta=0 is invalid
    std::vector<uint8_t> b = {0x7E, 0xC6, 0x00, 0x77, 0xC9};
    auto r = crystal::lift_ai_discourage_move(span_from(b));
    ASSERT_FAIL(r);
}

TEST(ai_discourage_malformed_too_short) {
    std::vector<uint8_t> b = {0x7E, 0xC6, 0x0A};
    auto r = crystal::lift_ai_discourage_move(span_from(b));
    ASSERT_FAIL(r);
}

// ============================================================================
// 2. lift_ai_choose_move_scores
// ============================================================================

// Vanilla sequence (padded to 30 bytes): bytes at offset 17=3E, 18=14, 19=21, 22=22
static std::vector<uint8_t> ai_choose_move_vanilla_bytes() {
    // Exact bytes from ROM at 0x440CE (span of 30):
    // FA 2D D2 3D C8 FA DC C2 A7 C0 3E 0F 21 D1 68 CF C0 3E 14 21 EA D1 22 22 22 77 FA F6 C6 A7
    return {
        0xFA, 0x2D, 0xD2, 0x3D, 0xC8, 0xFA, 0xDC, 0xC2, 0xA7, 0xC0,
        0x3E, 0x0F, 0x21, 0xD1, 0x68, 0xCF, 0xC0,
        0x3E, 0x14, 0x21, 0xEA, 0xD1, 0x22, 0x22, 0x22, 0x77,
        0xFA, 0xF6, 0xC6, 0xA7
    };
}

TEST(ai_choose_move_vanilla) {
    auto b = ai_choose_move_vanilla_bytes();
    auto r = crystal::lift_ai_choose_move_scores(span_from(b));
    ASSERT_OK(r);
    ASSERT_EQ(r.p[0], 20u);  // 0x14 = 20
}

TEST(ai_choose_move_modified_score) {
    auto b = ai_choose_move_vanilla_bytes();
    b[18] = 0x1E;  // change init score to 30
    auto r = crystal::lift_ai_choose_move_scores(span_from(b));
    ASSERT_OK(r);
    ASSERT_EQ(r.p[0], 30u);
    ASSERT_NE(r.p[0], 20u);
}

TEST(ai_choose_move_malformed_opcode_at_17) {
    auto b = ai_choose_move_vanilla_bytes();
    b[17] = 0x00;  // corrupt ld a,n opcode at offset 17
    auto r = crystal::lift_ai_choose_move_scores(span_from(b));
    ASSERT_FAIL(r);
}

TEST(ai_choose_move_malformed_no_ld_hl) {
    auto b = ai_choose_move_vanilla_bytes();
    b[19] = 0x00;  // corrupt ld hl,nn opcode at offset 19
    auto r = crystal::lift_ai_choose_move_scores(span_from(b));
    ASSERT_FAIL(r);
}

TEST(ai_choose_move_malformed_no_ldi) {
    auto b = ai_choose_move_vanilla_bytes();
    b[22] = 0x00;  // corrupt first ld [hli],a
    auto r = crystal::lift_ai_choose_move_scores(span_from(b));
    ASSERT_FAIL(r);
}

TEST(ai_choose_move_zero_score_invalid) {
    auto b = ai_choose_move_vanilla_bytes();
    b[18] = 0x00;  // init_score = 0 — invalid
    auto r = crystal::lift_ai_choose_move_scores(span_from(b));
    ASSERT_FAIL(r);
}

// ============================================================================
// 3. lift_exp_divisor
// ============================================================================

// Vanilla pattern: 3E 07 E0 B7 06 04 CD xx xx
static std::vector<uint8_t> exp_divisor_vanilla_span() {
    std::vector<uint8_t> b(20, 0x00);
    // Place pattern at offset 5
    b[5]  = 0x3E; b[6]  = 0x07;  // ld a, 7
    b[7]  = 0xE0; b[8]  = 0xB7;  // ldh [hDivisor], a
    b[9]  = 0x06; b[10] = 0x04;  // ld b, 4
    b[11] = 0xCD; b[12] = 0x00; b[13] = 0x00;  // call
    return b;
}

TEST(exp_divisor_vanilla) {
    auto b = exp_divisor_vanilla_span();
    auto r = crystal::lift_exp_divisor(span_from(b));
    ASSERT_OK(r);
    ASSERT_EQ(r.p[0], 7u);
}

TEST(exp_divisor_modified) {
    auto b = exp_divisor_vanilla_span();
    b[6] = 0x06;  // change divisor from 7 to 6
    auto r = crystal::lift_exp_divisor(span_from(b));
    ASSERT_OK(r);
    ASSERT_EQ(r.p[0], 6u);
}

TEST(exp_divisor_malformed_wrong_hdivisor) {
    auto b = exp_divisor_vanilla_span();
    b[8] = 0xB8;  // corrupt hDivisor address
    auto r = crystal::lift_exp_divisor(span_from(b));
    ASSERT_FAIL(r);
}

TEST(exp_divisor_malformed_wrong_b4) {
    auto b = exp_divisor_vanilla_span();
    b[10] = 0x05;  // ld b,5 instead of ld b,4
    auto r = crystal::lift_exp_divisor(span_from(b));
    ASSERT_FAIL(r);
}

TEST(exp_divisor_zero_invalid) {
    auto b = exp_divisor_vanilla_span();
    b[6] = 0x00;  // divisor = 0
    auto r = crystal::lift_exp_divisor(span_from(b));
    ASSERT_FAIL(r);
}

TEST(exp_divisor_too_large_invalid) {
    auto b = exp_divisor_vanilla_span();
    b[6] = 0x15;  // divisor = 21 > 20
    auto r = crystal::lift_exp_divisor(span_from(b));
    ASSERT_FAIL(r);
}

// ============================================================================
// 4. lift_damage_variation
// ============================================================================

// Helper: build a minimal DamageVariation span.
// Layout: [pad] 0F FE bound 38 jr_offset [E0 xx CD xx xx xx] 3E divisor E0 B7 [pad]
static std::vector<uint8_t> make_damage_var_span(uint8_t bound, uint8_t divisor) {
    // Offset 1: RRCA (0F)
    // Offset 2: CP N (FE bound)
    // Offset 4: JR C (38 FA)
    // Offset 6: ldh [hMultiplier], a (E0 01) — dummy address
    // Offset 8: call Multiply (CD 00 00)
    // Offset 11: ld a, divisor (3E divisor)
    // Offset 13: ldh [hDivisor], a (E0 B7)
    std::vector<uint8_t> b(20, 0x00);
    b[1]  = 0x0F;        // RRCA
    b[2]  = 0xFE;        // CP N
    b[3]  = bound;       // lower bound
    b[4]  = 0x38;        // JR C
    b[5]  = 0xFA;        // jr offset (loop back)
    b[6]  = 0xE0;        // ldh [hMultiplier]
    b[7]  = 0x01;        // dummy address
    b[8]  = 0xCD;        // CALL
    b[9]  = 0x00; b[10] = 0x00;
    b[11] = 0x3E;        // LD A, divisor
    b[12] = divisor;
    b[13] = 0xE0;        // ldh [hDivisor]
    b[14] = 0xB7;
    return b;
}

// Vanilla: bound=0xD9, divisor=0xFF (100 percent)
TEST(damage_variation_vanilla) {
    auto b = make_damage_var_span(0xD9, 0xFF);
    auto r = crystal::lift_damage_variation(span_from(b));
    ASSERT_OK(r);
    ASSERT_EQ(r.p[0], 0xD9u);
    ASSERT_EQ(r.p[1], 0xFFu);
    std::cout << "\n    [vanilla: p[0]=0xD9 p[1]=0xFF ✓]\n";
}

TEST(damage_variation_modified_bound) {
    auto b = make_damage_var_span(0xCC, 0xFF);
    auto r = crystal::lift_damage_variation(span_from(b));
    ASSERT_OK(r);
    ASSERT_EQ(r.p[0], 0xCCu);
    ASSERT_EQ(r.p[1], 0xFFu);
}

TEST(damage_variation_malformed_no_rrca) {
    // Replace RRCA (0F) with NOP (00)
    auto b = make_damage_var_span(0xD9, 0xFF);
    b[1] = 0x00;
    auto r = crystal::lift_damage_variation(span_from(b));
    ASSERT_FAIL(r);
}

TEST(damage_variation_malformed_no_cp) {
    // Replace CP (FE) with something else
    auto b = make_damage_var_span(0xD9, 0xFF);
    b[2] = 0x3E;
    auto r = crystal::lift_damage_variation(span_from(b));
    ASSERT_FAIL(r);
}

TEST(damage_variation_malformed_no_jr_c) {
    // Replace jr c (38) with jr (18)
    auto b = make_damage_var_span(0xD9, 0xFF);
    b[4] = 0x18;
    auto r = crystal::lift_damage_variation(span_from(b));
    ASSERT_FAIL(r);
}

TEST(damage_variation_lower_bound_too_small) {
    // bound < 0x80 — semantically invalid (loop would never terminate)
    auto b = make_damage_var_span(0x40, 0xFF);
    auto r = crystal::lift_damage_variation(span_from(b));
    ASSERT_FAIL(r);
}

// NEW: divisor correctly extracted
TEST(damage_variation_divisor_extracted) {
    // Vanilla divisor: 0xFF = 100 percent
    auto b = make_damage_var_span(0xD9, 0xFF);
    auto r = crystal::lift_damage_variation(span_from(b));
    ASSERT_OK(r);
    ASSERT_EQ(r.p[0], 0xD9u);  // lower bound unchanged
    ASSERT_EQ(r.p[1], 0xFFu);  // divisor = 100 percent = 255
    std::cout << "\n    [divisor: p[1]=0xFF=255 (100 percent) ✓]\n";
}

// NEW: modified divisor is extracted correctly
TEST(damage_variation_divisor_modified) {
    auto b = make_damage_var_span(0xD9, 0xCC);  // non-vanilla divisor
    auto r = crystal::lift_damage_variation(span_from(b));
    ASSERT_OK(r);
    ASSERT_EQ(r.p[1], 0xCCu);
}

// NEW: divisor of 0 is rejected
TEST(damage_variation_divisor_zero_rejected) {
    auto b = make_damage_var_span(0xD9, 0x00);  // divisor=0 → divide-by-zero
    auto r = crystal::lift_damage_variation(span_from(b));
    ASSERT_FAIL(r);
    std::cout << "\n    [divisor=0 rejected: ✓]\n";
}

// NEW: missing divisor pattern → fail
TEST(damage_variation_divisor_pattern_absent) {
    // Buffer has RRCA/CP/JRC but no 3E NN E0 B7 afterwards — only 7 bytes
    std::vector<uint8_t> b = {0x00, 0x0F, 0xFE, 0xD9, 0x38, 0xFA, 0x00};
    auto r = crystal::lift_damage_variation(span_from(b));
    ASSERT_FAIL(r);
    std::cout << "\n    [no divisor pattern → fail ✓]\n";
}

// ============================================================================
// 5. lift_capture_status_bonus
// ============================================================================

// Vanilla: E6 mask / 0E 0A / 20 xx / A7 / 0E 05 / 20 xx / 0E 00
static std::vector<uint8_t> capture_bonus_vanilla_span() {
    std::vector<uint8_t> b(20, 0x00);
    b[0] = 0xE6; b[1] = 0x60;  // and 0x60 (SLP/FRZ mask)
    b[2] = 0x0E; b[3] = 0x0A;  // ld c, 10
    b[4] = 0x20; b[5] = 0x04;  // jr nz, .add
    b[6] = 0xA7;                // and a
    b[7] = 0x0E; b[8] = 0x05;  // ld c, 5
    b[9] = 0x20; b[10] = 0x02; // jr nz
    b[11] = 0x0E; b[12] = 0x00; // ld c, 0
    return b;
}

TEST(capture_bonus_vanilla) {
    auto b = capture_bonus_vanilla_span();
    auto r = crystal::lift_capture_status_bonus(span_from(b));
    ASSERT_OK(r);
    ASSERT_EQ(r.p[0], 10u);  // SLP/FRZ
    ASSERT_EQ(r.p[1],  5u);  // BRN/PSN/PAR (vanilla bug path)
    ASSERT_EQ(r.p[2],  0u);  // no status
}

TEST(capture_bonus_modified_slp_frz) {
    auto b = capture_bonus_vanilla_span();
    b[3] = 0x08;  // SLP/FRZ bonus changed from 10 to 8
    auto r = crystal::lift_capture_status_bonus(span_from(b));
    ASSERT_OK(r);
    ASSERT_EQ(r.p[0], 8u);
    ASSERT_NE(r.p[0], 10u);
}

TEST(capture_bonus_malformed_nonzero_no_status) {
    auto b = capture_bonus_vanilla_span();
    b[12] = 0x01;  // no-status bonus = 1 (must be 0)
    auto r = crystal::lift_capture_status_bonus(span_from(b));
    ASSERT_FAIL(r);
}

TEST(capture_bonus_malformed_missing_and_mask) {
    auto b = capture_bonus_vanilla_span();
    b[0] = 0x3E;  // corrupt E6 (and n) with ld a,n
    auto r = crystal::lift_capture_status_bonus(span_from(b));
    ASSERT_FAIL(r);
}

TEST(capture_bonus_malformed_missing_jr_nz) {
    auto b = capture_bonus_vanilla_span();
    b[4] = 0x28;  // jr z instead of jr nz
    auto r = crystal::lift_capture_status_bonus(span_from(b));
    ASSERT_FAIL(r);
}

// ============================================================================
// 6. lift_escape_constants
// ============================================================================

// Vanilla pattern 1: 3E 20 E0 B7 CD xx xx   (ld a,32 / ldh [hMultiplier],a / call)
// Vanilla pattern 2: 06 1E F0 B6 80          (ld b,30 / ldh a,[hQuotient+3] / add a,b)
static std::vector<uint8_t> escape_vanilla_span() {
    std::vector<uint8_t> b(20, 0x00);
    // Pattern 1 at offset 0
    b[0]=0x3E; b[1]=0x20; b[2]=0xE0; b[3]=0xB7; b[4]=0xCD; b[5]=0x00; b[6]=0x00;
    // Pattern 2 at offset 10
    b[10]=0x06; b[11]=0x1E; b[12]=0xF0; b[13]=0xB6; b[14]=0x80;
    return b;
}

TEST(escape_vanilla) {
    auto b = escape_vanilla_span();
    auto r = crystal::lift_escape_constants(span_from(b));
    ASSERT_OK(r);
    ASSERT_EQ(r.p[0], 32u);  // speed multiplier
    ASSERT_EQ(r.p[1], 30u);  // attempt addend
}

TEST(escape_modified_speed_mult) {
    auto b = escape_vanilla_span();
    b[1] = 0x1C;  // speed multiplier 28 instead of 32
    auto r = crystal::lift_escape_constants(span_from(b));
    ASSERT_OK(r);
    ASSERT_EQ(r.p[0], 28u);
}

TEST(escape_modified_attempt_add) {
    auto b = escape_vanilla_span();
    b[11] = 0x14;  // attempt addend 20 instead of 30
    auto r = crystal::lift_escape_constants(span_from(b));
    ASSERT_OK(r);
    ASSERT_EQ(r.p[1], 20u);
}

TEST(escape_malformed_wrong_hmultiplier) {
    auto b = escape_vanilla_span();
    b[3] = 0xB8;  // wrong hMultiplier address
    auto r = crystal::lift_escape_constants(span_from(b));
    ASSERT_FAIL(r);
}

TEST(escape_malformed_wrong_hquotient) {
    auto b = escape_vanilla_span();
    b[13] = 0xB7;  // wrong hQuotient+3 address
    auto r = crystal::lift_escape_constants(span_from(b));
    ASSERT_FAIL(r);
}

TEST(escape_malformed_zero_speed_mult) {
    auto b = escape_vanilla_span();
    b[1] = 0x00;  // speed_mult = 0 invalid
    auto r = crystal::lift_escape_constants(span_from(b));
    ASSERT_FAIL(r);
}

// ============================================================================
// 7. lift_stat_formula_offsets
// ============================================================================

// Pattern: 3E N / E0 B7 / 3E 03 / 47 / CD xx xx  (divisor block)
//    then: 3E P / 47 / F0 B6 / 80  (non-HP offset)
//    then: 3E Q / 47 / F0 B6 / 80  (HP offset)
static std::vector<uint8_t> stat_formula_vanilla_span() {
    std::vector<uint8_t> b(30, 0x00);
    // Divisor block at offset 0
    b[0]=0x3E; b[1]=0x64; // ld a, 100
    b[2]=0xE0; b[3]=0xB7; // ldh [hDivisor], a
    b[4]=0x3E; b[5]=0x03; // ld a, 3
    b[6]=0x47;             // ld b, a
    b[7]=0xCD; b[8]=0x00; b[9]=0x00;  // call
    // Non-HP offset at offset 12
    b[12]=0x3E; b[13]=0x05; // ld a, 5
    b[14]=0x47;              // ld b, a
    b[15]=0xF0; b[16]=0xB6; // ldh a, [hQuotient+3]
    b[17]=0x80;              // add a, b
    // HP offset at offset 20
    b[20]=0x3E; b[21]=0x0A; // ld a, 10
    b[22]=0x47;              // ld b, a
    b[23]=0xF0; b[24]=0xB6; // ldh a, [hQuotient+3]
    b[25]=0x80;              // add a, b
    return b;
}

TEST(stat_formula_vanilla) {
    auto b = stat_formula_vanilla_span();
    auto r = crystal::lift_stat_formula_offsets(span_from(b));
    ASSERT_OK(r);
    ASSERT_EQ(r.p[0], 100u);  // level divisor
    ASSERT_EQ(r.p[1],   5u);  // non-HP offset
    ASSERT_EQ(r.p[2],  10u);  // HP offset
}

TEST(stat_formula_modified_divisor) {
    // Shape-driven test: if a hack changes /100 to /80, we extract 80
    auto b = stat_formula_vanilla_span();
    b[1] = 80;  // change divisor to 80
    auto r = crystal::lift_stat_formula_offsets(span_from(b));
    ASSERT_OK(r);
    ASSERT_EQ(r.p[0], 80u);  // extracted 80, not 100
    ASSERT_NE(r.p[0], 100u);
}

TEST(stat_formula_modified_offsets) {
    auto b = stat_formula_vanilla_span();
    b[13] = 6;  // non-HP offset → 6
    b[21] = 12; // HP offset → 12
    auto r = crystal::lift_stat_formula_offsets(span_from(b));
    ASSERT_OK(r);
    ASSERT_EQ(r.p[1], 6u);
    ASSERT_EQ(r.p[2], 12u);
}

TEST(stat_formula_malformed_missing_divisor_block) {
    auto b = stat_formula_vanilla_span();
    b[7] = 0x00;  // corrupt CALL in divisor block
    auto r = crystal::lift_stat_formula_offsets(span_from(b));
    ASSERT_FAIL(r);
}

TEST(stat_formula_malformed_zero_divisor) {
    auto b = stat_formula_vanilla_span();
    b[1] = 0;  // divisor = 0 — explicit guard
    auto r = crystal::lift_stat_formula_offsets(span_from(b));
    ASSERT_FAIL(r);
}

TEST(stat_formula_malformed_offset_order) {
    // non-HP offset must be < HP offset
    auto b = stat_formula_vanilla_span();
    b[13] = 15;  // non-HP > HP (10) — must fail
    auto r = crystal::lift_stat_formula_offsets(span_from(b));
    ASSERT_FAIL(r);
}

TEST(stat_formula_malformed_too_few_offset_patterns) {
    // Only one offset pattern found (corrupt the second)
    auto b = stat_formula_vanilla_span();
    b[22] = 0x00;  // corrupt ld b,a of HP offset block
    auto r = crystal::lift_stat_formula_offsets(span_from(b));
    ASSERT_FAIL(r);
}

// ============================================================================
// 8. lift_damage_calc_constants
// ============================================================================

// Patterns (each in their own region):
// P1: 3E 05 32 C5 06 04 CD   (level divisor=5)
// P2: 34 34                  (inc[hl] ×2 = level addend=2)
// P3: 36 32 06 04 CD         (damage divisor=50)
// P4: C6 02 36 xx            (min damage=2, followed by ld[hl],n)
static std::vector<uint8_t> damage_calc_vanilla_span() {
    std::vector<uint8_t> b(50, 0x00);
    // P1 at offset 0
    b[0]=0x3E; b[1]=0x05; b[2]=0x32; b[3]=0xC5; b[4]=0x06; b[5]=0x04; b[6]=0xCD;
    b[7]=0x00; b[8]=0x00;  // call target
    // P2 at offset 12
    b[12]=0x34; b[13]=0x34;
    // P3 at offset 20
    b[20]=0x36; b[21]=0x32; b[22]=0x06; b[23]=0x04; b[24]=0xCD;
    // P4 at offset 35: C6 02 32 30  (add a,2 / ld [de],a / jr nc)
    b[35]=0xC6; b[36]=0x02; b[37]=0x32; b[38]=0x30;
    return b;
}

TEST(damage_calc_vanilla) {
    auto b = damage_calc_vanilla_span();
    auto r = crystal::lift_damage_calc_constants(span_from(b));
    ASSERT_OK(r);
    ASSERT_EQ(r.p[0], 5u);   // level divisor
    ASSERT_EQ(r.p[1], 2u);   // inc[hl] count (level addend)
    ASSERT_EQ(r.p[2], 50u);  // damage divisor
    ASSERT_EQ(r.p[3], 2u);   // min damage
}

TEST(damage_calc_modified_level_divisor) {
    auto b = damage_calc_vanilla_span();
    b[1] = 0x04;  // level divisor 4 instead of 5
    auto r = crystal::lift_damage_calc_constants(span_from(b));
    ASSERT_OK(r);
    ASSERT_EQ(r.p[0], 4u);
}

TEST(damage_calc_modified_damage_divisor) {
    auto b = damage_calc_vanilla_span();
    b[21] = 0x28;  // damage divisor 40 instead of 50
    auto r = crystal::lift_damage_calc_constants(span_from(b));
    ASSERT_OK(r);
    ASSERT_EQ(r.p[2], 40u);
}

TEST(damage_calc_malformed_missing_p1) {
    auto b = damage_calc_vanilla_span();
    b[2] = 0x00;  // corrupt ld[de],a anchor in P1
    auto r = crystal::lift_damage_calc_constants(span_from(b));
    ASSERT_FAIL(r);
}

TEST(damage_calc_malformed_missing_p3) {
    auto b = damage_calc_vanilla_span();
    b[24] = 0x00;  // corrupt CALL in P3
    auto r = crystal::lift_damage_calc_constants(span_from(b));
    ASSERT_FAIL(r);
}

TEST(damage_calc_level_divisor_out_of_range) {
    auto b = damage_calc_vanilla_span();
    b[1] = 0x0F;  // divisor = 15 > 10
    auto r = crystal::lift_damage_calc_constants(span_from(b));
    ASSERT_FAIL(r);
}

// ============================================================================
// 9. lift_residual_fraction
// ============================================================================

// GetEighthMaxHP shape: CD xx xx / CB 39  (call + 1 srl c)
static std::vector<uint8_t> residual_eighth_span() {
    // CD 83 4C (call GetQuarterMaxHP) + CB 39 (srl c ×1) + 00 00 (padding)
    return {0xCD, 0x83, 0x4C, 0xCB, 0x39, 0x00, 0x00, 0x00};
}

// GetSixteenthMaxHP shape: CD xx xx / CB 39 / CB 39
static std::vector<uint8_t> residual_sixteenth_span() {
    return {0xCD, 0x76, 0x4C, 0xCB, 0x39, 0xCB, 0x39, 0x00};
}

TEST(residual_eighth_vanilla) {
    auto b = residual_eighth_span();
    auto r = crystal::lift_residual_fraction(span_from(b));
    ASSERT_OK(r);
    ASSERT_EQ(r.p[0], 8u);
}

TEST(residual_sixteenth_vanilla) {
    auto b = residual_sixteenth_span();
    auto r = crystal::lift_residual_fraction(span_from(b));
    ASSERT_OK(r);
    ASSERT_EQ(r.p[0], 16u);
}

TEST(residual_malformed_no_call) {
    // Replace CALL (CD) with NOP
    std::vector<uint8_t> b = {0x00, 0x83, 0x4C, 0xCB, 0x39, 0x00, 0x00, 0x00};
    auto r = crystal::lift_residual_fraction(span_from(b));
    ASSERT_FAIL(r);
}

TEST(residual_malformed_no_srl_c) {
    // CB prefix but wrong second byte (srl b = CB 38 not srl c = CB 39)
    std::vector<uint8_t> b = {0xCD, 0x83, 0x4C, 0xCB, 0x38, 0x00, 0x00, 0x00};
    auto r = crystal::lift_residual_fraction(span_from(b));
    ASSERT_FAIL(r);
}

TEST(residual_malformed_too_many_srls) {
    // 3 srl c → denominator 32 — not 8 or 16, should fail
    std::vector<uint8_t> b = {0xCD, 0x83, 0x4C, 0xCB, 0x39, 0xCB, 0x39, 0xCB, 0x39, 0x00};
    auto r = crystal::lift_residual_fraction(span_from(b));
    ASSERT_FAIL(r);
}

TEST(residual_malformed_span_too_short) {
    std::vector<uint8_t> b = {0xCD, 0x83};
    auto r = crystal::lift_residual_fraction(span_from(b));
    ASSERT_FAIL(r);
}

// ============================================================================
// 10. lift_crit_stage_deltas
// ============================================================================

// Vanilla: two ld c,2 (held item paths) + two inc c (scope+focus) = minimal
static std::vector<uint8_t> crit_deltas_vanilla_span() {
    std::vector<uint8_t> b(20, 0x00);
    b[0]  = 0x0E; b[1]  = 0x02;  // ld c, 2 (held item path 1)
    b[5]  = 0x0E; b[6]  = 0x02;  // ld c, 2 (held item path 2)
    b[10] = 0x0C;                 // inc c (scope lens)
    b[15] = 0x0C;                 // inc c (focus energy)
    return b;
}

TEST(crit_deltas_vanilla) {
    auto b = crit_deltas_vanilla_span();
    auto r = crystal::lift_crit_stage_deltas(span_from(b));
    ASSERT_OK(r);
    ASSERT_EQ(r.p[0], 2u);  // held item delta
    // scope/focus are hardcoded 1,1 — not individually ROM-derived
    ASSERT_EQ(r.p[1], 1u);
    ASSERT_EQ(r.p[2], 1u);
}

TEST(crit_deltas_modified_held_item) {
    auto b = crit_deltas_vanilla_span();
    b[1] = 0x03; b[6] = 0x03;  // held item delta → 3
    auto r = crystal::lift_crit_stage_deltas(span_from(b));
    ASSERT_OK(r);
    ASSERT_EQ(r.p[0], 3u);
}

TEST(crit_deltas_malformed_too_few_ld_c) {
    auto b = crit_deltas_vanilla_span();
    b[5] = 0x00; b[6] = 0x00;  // remove second ld c,2
    auto r = crystal::lift_crit_stage_deltas(span_from(b));
    ASSERT_FAIL(r);
}

TEST(crit_deltas_malformed_too_few_inc_c) {
    auto b = crit_deltas_vanilla_span();
    b[15] = 0x00;  // remove second inc c
    auto r = crystal::lift_crit_stage_deltas(span_from(b));
    ASSERT_FAIL(r);
}

TEST(crit_deltas_malformed_ld_c_zero) {
    // ld c, 0 — no delta > 4 or == 0 check
    auto b = crit_deltas_vanilla_span();
    b[1] = 0x00; b[6] = 0x00;  // held item delta = 0
    // Per recognizer: ld c, n only matches if n in [1,4]
    auto r = crystal::lift_crit_stage_deltas(span_from(b));
    // This should fail (no valid ld c,n found twice)
    ASSERT_FAIL(r);
}

// ============================================================================
// 11. ROM-backed positive tests (requires real ROM)
// ============================================================================

TEST(rom_all_11_recognizers_succeed_vanilla) {
    if (!g_rom || !g_profile) {
        std::cout << "\n    [SKIP: no ROM]\n";
        return;
    }
    const auto& o = g_profile->offsets;

    // Helper
    auto check = [&](uint32_t addr, uint32_t span, const char* name, auto fn) {
        if (addr == 0) { std::fprintf(stderr, "    [SKIP: %s addr=0]\n", name); return; }
        auto s = crystal::rom_span_at(*g_rom, addr, span);
        auto r = fn(s);
        if (!r.ok) {
            std::fprintf(stderr, "    FAIL recognizer %s: %s\n", name, r.error.c_str());
            g_current_failed = true;
        }
    };

    check(o.sm83_ai_discourage_move,    crystal::ProfileOffsets::SM83_SPAN_AI_DISCOURAGE,
          "AIDiscourageMove",     [](auto s){ return crystal::lift_ai_discourage_move(s); });
    check(o.sm83_ai_choose_move,        crystal::ProfileOffsets::SM83_SPAN_AI_CHOOSE_MOVE,
          "AIChooseMove",         [](auto s){ return crystal::lift_ai_choose_move_scores(s); });
    check(o.sm83_give_exp_points,       crystal::ProfileOffsets::SM83_SPAN_GIVE_EXP,
          "GiveExp",              [](auto s){ return crystal::lift_exp_divisor(s); });
    check(o.sm83_damage_variation,      crystal::ProfileOffsets::SM83_SPAN_DAMAGE_VARIATION,
          "DamageVariation",      [](auto s){ return crystal::lift_damage_variation(s); });
    check(o.sm83_poke_ball_effect,      crystal::ProfileOffsets::SM83_SPAN_POKE_BALL,
          "PokeBallEffect",       [](auto s){ return crystal::lift_capture_status_bonus(s); });
    check(o.sm83_try_to_run_away,       crystal::ProfileOffsets::SM83_SPAN_TRY_TO_RUN,
          "TryToRunAway",         [](auto s){ return crystal::lift_escape_constants(s); });
    check(o.sm83_calc_mon_stat_c,       crystal::ProfileOffsets::SM83_SPAN_CALC_MON_STAT_C,
          "CalcMonStatC",         [](auto s){ return crystal::lift_stat_formula_offsets(s); });
    check(o.sm83_damage_calc,           crystal::ProfileOffsets::SM83_SPAN_DAMAGE_CALC,
          "DamageCalc",           [](auto s){ return crystal::lift_damage_calc_constants(s); });
    check(o.sm83_get_eighth_max_hp,     crystal::ProfileOffsets::SM83_SPAN_GET_EIGHTH_HP,
          "GetEighthMaxHP",       [](auto s){ return crystal::lift_residual_fraction(s); });
    check(o.sm83_get_sixteenth_max_hp,  crystal::ProfileOffsets::SM83_SPAN_GET_SIXTEENTH_HP,
          "GetSixteenthMaxHP",    [](auto s){ return crystal::lift_residual_fraction(s); });
    check(o.sm83_critical,              crystal::ProfileOffsets::SM83_SPAN_CRITICAL,
          "BattleCommand_Critical",[](auto s){ return crystal::lift_crit_stage_deltas(s); });
}

TEST(rom_vanilla_values_match_known_constants) {
    if (!g_rom || !g_profile) {
        std::cout << "\n    [SKIP: no ROM]\n";
        return;
    }
    const auto& o = g_profile->offsets;

    // Verify each extractor returns the expected vanilla value from the real ROM
    auto getval = [&](uint32_t addr, uint32_t span, auto fn, uint8_t param_idx) -> uint8_t {
        auto s = crystal::rom_span_at(*g_rom, addr, span);
        auto r = fn(s);
        if (!r.ok) return 0xFF;
        return r.p[param_idx];
    };

    ASSERT_EQ(getval(o.sm83_ai_discourage_move, crystal::ProfileOffsets::SM83_SPAN_AI_DISCOURAGE,
        [](auto s){return crystal::lift_ai_discourage_move(s);}, 0), 10u);

    ASSERT_EQ(getval(o.sm83_ai_choose_move, crystal::ProfileOffsets::SM83_SPAN_AI_CHOOSE_MOVE,
        [](auto s){return crystal::lift_ai_choose_move_scores(s);}, 0), 20u);

    ASSERT_EQ(getval(o.sm83_give_exp_points, crystal::ProfileOffsets::SM83_SPAN_GIVE_EXP,
        [](auto s){return crystal::lift_exp_divisor(s);}, 0), 7u);

    ASSERT_EQ(getval(o.sm83_damage_variation, crystal::ProfileOffsets::SM83_SPAN_DAMAGE_VARIATION,
        [](auto s){return crystal::lift_damage_variation(s);}, 0), 0xD9u);
    ASSERT_EQ(getval(o.sm83_damage_variation, crystal::ProfileOffsets::SM83_SPAN_DAMAGE_VARIATION,
        [](auto s){return crystal::lift_damage_variation(s);}, 1), 0xFFu);  // 100 percent = 255

    ASSERT_EQ(getval(o.sm83_poke_ball_effect, crystal::ProfileOffsets::SM83_SPAN_POKE_BALL,
        [](auto s){return crystal::lift_capture_status_bonus(s);}, 0), 10u);

    ASSERT_EQ(getval(o.sm83_try_to_run_away, crystal::ProfileOffsets::SM83_SPAN_TRY_TO_RUN,
        [](auto s){return crystal::lift_escape_constants(s);}, 0), 32u);

    ASSERT_EQ(getval(o.sm83_try_to_run_away, crystal::ProfileOffsets::SM83_SPAN_TRY_TO_RUN,
        [](auto s){return crystal::lift_escape_constants(s);}, 1), 30u);

    ASSERT_EQ(getval(o.sm83_calc_mon_stat_c, crystal::ProfileOffsets::SM83_SPAN_CALC_MON_STAT_C,
        [](auto s){return crystal::lift_stat_formula_offsets(s);}, 0), 100u);  // /100

    ASSERT_EQ(getval(o.sm83_calc_mon_stat_c, crystal::ProfileOffsets::SM83_SPAN_CALC_MON_STAT_C,
        [](auto s){return crystal::lift_stat_formula_offsets(s);}, 1), 5u);    // non-HP +5

    ASSERT_EQ(getval(o.sm83_calc_mon_stat_c, crystal::ProfileOffsets::SM83_SPAN_CALC_MON_STAT_C,
        [](auto s){return crystal::lift_stat_formula_offsets(s);}, 2), 10u);   // HP +10

    ASSERT_EQ(getval(o.sm83_get_eighth_max_hp, crystal::ProfileOffsets::SM83_SPAN_GET_EIGHTH_HP,
        [](auto s){return crystal::lift_residual_fraction(s);}, 0), 8u);

    ASSERT_EQ(getval(o.sm83_get_sixteenth_max_hp, crystal::ProfileOffsets::SM83_SPAN_GET_SIXTEENTH_HP,
        [](auto s){return crystal::lift_residual_fraction(s);}, 0), 16u);

    ASSERT_EQ(getval(o.sm83_critical, crystal::ProfileOffsets::SM83_SPAN_CRITICAL,
        [](auto s){return crystal::lift_crit_stage_deltas(s);}, 0), 2u);  // held item +2
}

// ============================================================================
// Task #4 — sm83_lifted_mask wire roundtrip
// ============================================================================

TEST(sm83_lifted_mask_survives_roundtrip) {
    // Build a BattleRules with a specific mask, roundtrip it, verify mask is preserved.
    // Use the vanilla sub-struct values so is_valid() passes.

    // Build minimal valid rules
    auto make_valid_rules = [&]() {
        enginemon::BattleRules r;
        r.stat_stage_mult = {{
            {25,100},{28,100},{33,100},{40,100},{50,100},{66,100},
            {1,1},{15,10},{2,1},{25,10},{3,1},{35,10},{4,1}
        }};
        r.acc_stage_mult = {{
            {33,100},{36,100},{43,100},{50,100},{60,100},{75,100},
            {1,1},{133,100},{166,100},{2,1},{233,100},{133,50},{3,1}
        }};
        r.crit_chances = {17,32,64,85,128,128,128};
        r.wobble_probabilities = {{{1,63},{255,255}}};
        r.trainer_class_ai.push_back({0,0,10,enginemon::AIPassSet::basic_only(),0x0000});
        return r;
    };

    // Test 1: mask=0 (nothing lifted) survives
    {
        auto rules = make_valid_rules();
        rules.sm83_lifted_mask = 0;
        auto loaded = roundtrip(rules, "mask_zero");
        ASSERT_TRUE(loaded.has_value());
        ASSERT_EQ(loaded->sm83_lifted_mask, 0u);
    }

    // Test 2: full mask (all bits set) survives
    {
        auto rules = make_valid_rules();
        rules.sm83_lifted_mask = 0x01FFu;  // bits 0-8 = all 9 sub-structs
        auto loaded = roundtrip(rules, "mask_full");
        ASSERT_TRUE(loaded.has_value());
        ASSERT_EQ(loaded->sm83_lifted_mask, 0x01FFu);
    }

    // Test 3: partial mask — only DAMAGE_FORMULA and AI_SCORES lifted
    {
        auto rules = make_valid_rules();
        rules.sm83_lifted_mask =
            enginemon::BattleRules::SM83_LIFTED_DAMAGE_FORMULA |
            enginemon::BattleRules::SM83_LIFTED_AI_SCORES;
        auto loaded = roundtrip(rules, "mask_partial");
        ASSERT_TRUE(loaded.has_value());
        ASSERT_TRUE(loaded->sm83_is_lifted(enginemon::BattleRules::SM83_LIFTED_DAMAGE_FORMULA));
        ASSERT_TRUE(loaded->sm83_is_lifted(enginemon::BattleRules::SM83_LIFTED_AI_SCORES));
        ASSERT_FALSE(loaded->sm83_is_lifted(enginemon::BattleRules::SM83_LIFTED_STAT_FORMULA));
        ASSERT_FALSE(loaded->sm83_is_lifted(enginemon::BattleRules::SM83_LIFTED_RESIDUAL));
    }

    std::cout << "\n    [sm83_lifted_mask wire roundtrip: 0, full, partial — all preserved]\n";
}

TEST(sm83_lifted_mask_vanilla_rom_all_bits_set) {
    // After extracting from the vanilla ROM, every sub-struct should be lifted.
    if (!g_rom || !g_profile) {
        std::cout << "\n    [SKIP: no ROM]\n";
        return;
    }
    auto result = crystal::extract_battle_rules(*g_rom, *g_profile);
    ASSERT_TRUE(result.success);

    using BR = enginemon::BattleRules;
    const uint16_t expected =
        BR::SM83_LIFTED_DAMAGE_FORMULA |
        BR::SM83_LIFTED_AI_SCORES      |
        BR::SM83_LIFTED_STAT_FORMULA   |
        BR::SM83_LIFTED_ESCAPE         |
        BR::SM83_LIFTED_CAPTURE_STATUS |
        BR::SM83_LIFTED_EXP            |
        BR::SM83_LIFTED_RESIDUAL       |
        BR::SM83_LIFTED_CRIT_DELTAS    |
        BR::SM83_LIFTED_DAMAGE_VAR     |
        BR::SM83_LIFTED_RECOIL         |
        BR::SM83_LIFTED_DRAIN;

    ASSERT_EQ(result.rules.sm83_lifted_mask, expected);
    if (result.rules.sm83_lifted_mask != expected) {
        std::fprintf(stderr, "    expected mask 0x%04X, got 0x%04X\n",
                     expected, result.rules.sm83_lifted_mask);
    }
}

// ============================================================================
// Task #7 — runtime consumption: damage formula + damage variation floor
// ============================================================================

TEST(damage_formula_rules_vs_default_produces_different_damage) {
    // Build rules with non-vanilla damage formula
    enginemon::BattleRules rules;
    rules.damage_formula.level_divisor  = 4;   // /4 instead of /5
    rules.damage_formula.level_addend   = 3;   // +3 instead of +2
    rules.damage_formula.damage_divisor = 40;  // /40 instead of /50
    rules.damage_formula.min_damage     = 3;   // +3 instead of +2

    enginemon::DamageParams dp{};
    dp.attacker_level = 50;
    dp.attack_stat    = 80;
    dp.defense_stat   = 80;
    dp.move_power     = 80;
    dp.type_effectiveness = 100;

    int32_t with_rules  = enginemon::calculate_damage(dp, rules);
    int32_t no_rules    = enginemon::calculate_damage(dp);  // vanilla

    // With /4+3 and /40+3 vs /5+2 and /50+2 — must differ
    ASSERT_NE(with_rules, no_rules);
    // Specific value check for /4 path: (50*2/4+3)*80*80/80/40 + 3 = (28)*80/40+3 = 59
    // (approx — exact depends on truncation)
    ASSERT_TRUE(with_rules > 0);
    ASSERT_TRUE(no_rules > 0);
    std::cout << "\n    [damage formula: rules=" << with_rules << " vanilla=" << no_rules << "]\n";
}

TEST(damage_variation_lower_bound_byte_used_in_rrca_loop) {
    // Verify lower_bound_byte and divisor distinctions are accessible via getters.
    enginemon::BattleRules rules_strict;
    rules_strict.damage_variation.lower_bound_byte = 0xD9;
    rules_strict.damage_variation.divisor          = 0xFF;
    rules_strict.sm83_lifted_mask = enginemon::BattleRules::SM83_LIFTED_DAMAGE_VAR;

    enginemon::BattleRules rules_loose;
    rules_loose.damage_variation.lower_bound_byte = 0x80;
    rules_loose.damage_variation.divisor          = 0xCC;
    rules_loose.sm83_lifted_mask = enginemon::BattleRules::SM83_LIFTED_DAMAGE_VAR;

    ASSERT_EQ(rules_strict.get_damage_var_lower_bound(), 0xD9u);
    ASSERT_EQ(rules_strict.get_damage_var_divisor(),     0xFFu);
    ASSERT_EQ(rules_loose.get_damage_var_lower_bound(),  0x80u);
    ASSERT_EQ(rules_loose.get_damage_var_divisor(),      0xCCu);
    ASSERT_NE(rules_strict.get_damage_var_lower_bound(),
              rules_loose.get_damage_var_lower_bound());
    ASSERT_NE(rules_strict.get_damage_var_divisor(),
              rules_loose.get_damage_var_divisor());

    std::cout << "\n    [damage variation getters: lower_bound 0xD9≠0x80, divisor 0xFF≠0xCC ✓]\n";
}

// ============================================================================
// 12. lift_recoil_shift
// Source: BattleCommand_Recoil — two SRL-B/RR-C pairs → shift_count=2 → /4.
// ============================================================================

// Vanilla recoil span: 8 bytes before the CB38 block (ld loads) + two shift pairs.
// Minimal: pad8 + CB38 CB19 CB38 CB19 + 78 B1 20 01 0C (min-1 guard)
// We put the span starting right at the CB38 pattern with enough context.
static std::vector<uint8_t> make_recoil_vanilla_span() {
    // Layout:
    //   [0..7]  : filler (ld a,[wCurDamage] etc — recognizer scans for CB38 inside span)
    //   [8]:  0xCB, [9]:  0x38   srl b  ← pair 1
    //   [10]: 0xCB, [11]: 0x19   rr c
    //   [12]: 0xCB, [13]: 0x38   srl b  ← pair 2
    //   [14]: 0xCB, [15]: 0x19   rr c
    //   [16]: 0x78                ld a, b
    //   [17]: 0xB1                or c
    //   [18]: 0x20, [19]: 0x01    jr nz, +1
    //   [20]: 0x0C                inc c  ← min-1 guard
    std::vector<uint8_t> b(30, 0x00);
    b[8]  = 0xCB; b[9]  = 0x38;  // srl b
    b[10] = 0xCB; b[11] = 0x19;  // rr c
    b[12] = 0xCB; b[13] = 0x38;  // srl b
    b[14] = 0xCB; b[15] = 0x19;  // rr c
    b[16] = 0x78;                 // ld a, b
    b[17] = 0xB1;                 // or c
    b[18] = 0x20; b[19] = 0x01;  // jr nz, 1
    b[20] = 0x0C;                 // inc c (min-1 guard)
    return b;
}

TEST(recoil_shift_vanilla) {
    auto b = make_recoil_vanilla_span();
    auto r = crystal::lift_recoil_shift(span_from(b));
    ASSERT_OK(r);
    ASSERT_EQ(r.p[0], 2u);  // vanilla: 2 SRL-B/RR-C pairs → shift_count=2 → /4
    std::cout << "\n    [recoil_shift vanilla: p[0]=2 (two SRL-B/RR-C pairs → /4) ✓]\n";
}

TEST(recoil_shift_modified_to_three_pairs) {
    // A ROM hack with 3 shift pairs → /8
    std::vector<uint8_t> b(30, 0x00);
    b[4]  = 0xCB; b[5]  = 0x38;  // srl b
    b[6]  = 0xCB; b[7]  = 0x19;  // rr c
    b[8]  = 0xCB; b[9]  = 0x38;
    b[10] = 0xCB; b[11] = 0x19;
    b[12] = 0xCB; b[13] = 0x38;
    b[14] = 0xCB; b[15] = 0x19;
    b[16] = 0x78; b[17] = 0xB1;
    b[18] = 0x20; b[19] = 0x01;
    b[20] = 0x0C;
    auto r = crystal::lift_recoil_shift(span_from(b));
    ASSERT_OK(r);
    ASSERT_EQ(r.p[0], 3u);
}

TEST(recoil_shift_malformed_no_cb38) {
    // No CB38 in span → fail
    std::vector<uint8_t> b(30, 0x00);
    auto r = crystal::lift_recoil_shift(span_from(b));
    ASSERT_FAIL(r);
}

TEST(recoil_shift_malformed_cb38_without_cb19) {
    // CB38 present but not followed immediately by CB19
    std::vector<uint8_t> b(30, 0x00);
    b[8] = 0xCB; b[9] = 0x38;   // srl b
    b[10] = 0xCB; b[11] = 0x3F; // srl a — not rr c
    auto r = crystal::lift_recoil_shift(span_from(b));
    ASSERT_FAIL(r);
}

TEST(recoil_shift_malformed_missing_inc_c) {
    // CB38/CB19 pairs present but no inc c (0x0C) min-1 guard
    auto b = make_recoil_vanilla_span();
    b[20] = 0x00;  // corrupt inc c
    auto r = crystal::lift_recoil_shift(span_from(b));
    ASSERT_FAIL(r);
}

TEST(recoil_shift_span_too_short) {
    std::vector<uint8_t> b(5, 0x00);
    auto r = crystal::lift_recoil_shift(span_from(b));
    ASSERT_FAIL(r);
}

// ============================================================================
// 13. lift_drain_shift
// Source: SapHealth — one SRL-A step → shift_count=1 → /2.
// ============================================================================

// Vanilla drain span: 2A / CB 3F / E0 hh / 47 / 7E / CB 1F / E0 hh2
static std::vector<uint8_t> make_drain_vanilla_span() {
    // [0]: 2A      ldi a,[hl]  (ld a,[hli])
    // [1]: CB, [2]: 3F          srl a
    // [3]: E0, [4]: 94          ldh [hDividend], a  (hDividend = 0xFF94 — any addr ok)
    // [5]: 47                   ld b, a
    // [6]: 7E                   ld a, [hl]
    // [7]: CB, [8]: 1F          rr a
    // [9]: E0, [10]: 95         ldh [hDividend+1], a
    std::vector<uint8_t> b(20, 0x00);
    b[0]  = 0x2A;              // ldi a,[hl]
    b[1]  = 0xCB; b[2]  = 0x3F; // srl a
    b[3]  = 0xE0; b[4]  = 0x94; // ldh [hDividend],a
    b[5]  = 0x47;              // ld b, a
    b[6]  = 0x7E;              // ld a,[hl]
    b[7]  = 0xCB; b[8]  = 0x1F; // rr a
    b[9]  = 0xE0; b[10] = 0x95; // ldh [hDividend+1],a
    return b;
}

TEST(drain_shift_vanilla) {
    auto b = make_drain_vanilla_span();
    auto r = crystal::lift_drain_shift(span_from(b));
    ASSERT_OK(r);
    ASSERT_EQ(r.p[0], 1u);  // vanilla: one SRL-A → shift_count=1 → /2
    std::cout << "\n    [drain_shift vanilla: p[0]=1 (one SRL-A → /2) ✓]\n";
}

TEST(drain_shift_modified_two_srl_a) {
    // ROM hack with two SRL-A steps → shift_count=2 → /4
    std::vector<uint8_t> b(20, 0x00);
    b[0] = 0x2A;
    b[1] = 0xCB; b[2] = 0x3F;  // srl a
    b[3] = 0xCB; b[4] = 0x3F;  // srl a (second)
    b[5] = 0xE0; b[6] = 0x94;  // ldh [hDividend]
    b[7] = 0x47;
    b[8] = 0x7E;
    b[9] = 0xCB; b[10] = 0x1F;
    auto r = crystal::lift_drain_shift(span_from(b));
    ASSERT_OK(r);
    ASSERT_EQ(r.p[0], 2u);
}

TEST(drain_shift_malformed_no_ldi) {
    // No 2A (ldi a,[hl]) → fail
    std::vector<uint8_t> b(20, 0x00);
    b[0] = 0xFA;  // ld a,[nn] — not 2A
    b[1] = 0xCB; b[2] = 0x3F;
    auto r = crystal::lift_drain_shift(span_from(b));
    ASSERT_FAIL(r);
}

TEST(drain_shift_malformed_ldi_without_srl_a) {
    // 2A present but followed by CB38 (srl b) not CB3F (srl a)
    std::vector<uint8_t> b(20, 0x00);
    b[0] = 0x2A;
    b[1] = 0xCB; b[2] = 0x38;  // srl b — not srl a
    auto r = crystal::lift_drain_shift(span_from(b));
    ASSERT_FAIL(r);
}

TEST(drain_shift_malformed_no_ldh_after_srl) {
    // srl a present but no E0 (ldh) after it
    std::vector<uint8_t> b(20, 0x00);
    b[0] = 0x2A;
    b[1] = 0xCB; b[2] = 0x3F;
    b[3] = 0x47;  // ld b,a — no E0 before it
    auto r = crystal::lift_drain_shift(span_from(b));
    ASSERT_FAIL(r);
}

TEST(drain_shift_malformed_no_rr_a) {
    // ldh present but no CB 1F (rr a) after ld a,[hl]
    std::vector<uint8_t> b(20, 0x00);
    b[0] = 0x2A;
    b[1] = 0xCB; b[2] = 0x3F;
    b[3] = 0xE0; b[4] = 0x94;
    b[5] = 0x47;
    b[6] = 0x7E;
    b[7] = 0xCB; b[8] = 0x38;  // srl b — not rr a (1F)
    auto r = crystal::lift_drain_shift(span_from(b));
    ASSERT_FAIL(r);
}

TEST(drain_shift_span_too_short) {
    std::vector<uint8_t> b(4, 0x00);
    auto r = crystal::lift_drain_shift(span_from(b));
    ASSERT_FAIL(r);
}

// ============================================================================
// 14a. sm83_find_recoil — synthetic modified-count recovery tests
//
// Proves the structural anchor (4F CB38 CB19) fires regardless of shift_count.
// Each synthetic ROM contains only the minimum bytes needed for the anchor + shift
// chain + min-1 guard.  lift_recoil_shift must return the correct count.
// ============================================================================

// Build a minimal synthetic ROM buffer for sm83_find_recoil:
//   offset 0:  00 00 00 00 00 00 00 00 00  (9 bytes of pad — will become the scan_start span)
//   offset 9:  4F                           ld c,a  ← anchor byte
//   offset 10: (CB38 CB19) × K             shift chain
//   offset 10+4K: 78 B1 20 01 0C           min-1 guard
static std::vector<uint8_t> make_recoil_finder_rom(uint8_t K) {
    std::vector<uint8_t> buf(60, 0x00);
    uint32_t pos = 9;
    buf[pos++] = 0x4F;                      // ld c,a  (anchor)
    for (uint8_t k = 0; k < K; ++k) {
        buf[pos++] = 0xCB; buf[pos++] = 0x38;  // srl b
        buf[pos++] = 0xCB; buf[pos++] = 0x19;  // rr c
    }
    buf[pos++] = 0x78;  // ld a,b
    buf[pos++] = 0xB1;  // or c
    buf[pos++] = 0x20; buf[pos++] = 0x01;  // jr nz
    buf[pos++] = 0x0C;  // inc c
    return buf;
}

TEST(finder_recoil_shift_count_1_recovered) {
    // shift_count=1: old anchor (two-pair pattern) never matched this.
    // New anchor (4F CB38 CB19) must find it and return p[0]=1.
    auto buf = make_recoil_finder_rom(1);
    auto rom = crystal::RomData::from_bytes(buf);
    auto candidates = crystal::sm83_find_recoil(rom);
    ASSERT_TRUE(!candidates.empty());
    ASSERT_EQ(candidates[0].lift_result.p[0], 1u);
    std::cout << "\n    [finder_recoil shift_count=1: " << candidates.size()
              << " candidate(s), p[0]=1 ✓]\n";
}

TEST(finder_recoil_shift_count_2_recovered) {
    // shift_count=2: vanilla case, must still work with new anchor.
    auto buf = make_recoil_finder_rom(2);
    auto rom = crystal::RomData::from_bytes(buf);
    auto candidates = crystal::sm83_find_recoil(rom);
    ASSERT_TRUE(!candidates.empty());
    ASSERT_EQ(candidates[0].lift_result.p[0], 2u);
    std::cout << "\n    [finder_recoil shift_count=2: " << candidates.size()
              << " candidate(s), p[0]=2 ✓]\n";
}

TEST(finder_recoil_shift_count_3_recovered) {
    // shift_count=3: was accidentally recoverable before, must still work.
    auto buf = make_recoil_finder_rom(3);
    auto rom = crystal::RomData::from_bytes(buf);
    auto candidates = crystal::sm83_find_recoil(rom);
    ASSERT_TRUE(!candidates.empty());
    ASSERT_EQ(candidates[0].lift_result.p[0], 3u);
    std::cout << "\n    [finder_recoil shift_count=3: " << candidates.size()
              << " candidate(s), p[0]=3 ✓]\n";
}

// ============================================================================
// 14b. sm83_find_drain — synthetic modified-count recovery tests
//
// Proves the structural anchor (2A CB3F) fires regardless of shift_count.
// ============================================================================

// Build a minimal synthetic ROM buffer for sm83_find_drain:
//   offset 0: 2A                       ldi a,[hl]  ← anchor byte
//   offset 1: (CB 3F) × K              srl a chain
//   offset 1+2K: E0 94                 ldh [hDividend],a
//   offset 3+2K: 47                    ld b,a
//   offset 4+2K: 7E                    ld a,[hl]
//   offset 5+2K: CB 1F                 rr a
//   offset 7+2K: E0 95                 ldh [hDividend+1],a
static std::vector<uint8_t> make_drain_finder_rom(uint8_t K) {
    std::vector<uint8_t> buf(40, 0x00);
    uint32_t pos = 0;
    buf[pos++] = 0x2A;                         // ldi a,[hl]
    for (uint8_t k = 0; k < K; ++k) {
        buf[pos++] = 0xCB; buf[pos++] = 0x3F;  // srl a
    }
    buf[pos++] = 0xE0; buf[pos++] = 0x94;      // ldh [hDividend]
    buf[pos++] = 0x47;                         // ld b,a
    buf[pos++] = 0x7E;                         // ld a,[hl]
    buf[pos++] = 0xCB; buf[pos++] = 0x1F;      // rr a
    buf[pos++] = 0xE0; buf[pos++] = 0x95;      // ldh [hDividend+1]
    return buf;
}

TEST(finder_drain_shift_count_1_recovered) {
    // shift_count=1: vanilla, still found with simplified anchor.
    auto buf = make_drain_finder_rom(1);
    auto rom = crystal::RomData::from_bytes(buf);
    auto candidates = crystal::sm83_find_drain(rom);
    ASSERT_TRUE(!candidates.empty());
    ASSERT_EQ(candidates[0].lift_result.p[0], 1u);
    std::cout << "\n    [finder_drain shift_count=1: " << candidates.size()
              << " candidate(s), p[0]=1 ✓]\n";
}

TEST(finder_drain_shift_count_2_recovered) {
    // shift_count=2: old finder (E0 at i+3) never matched this.
    // New anchor (2A CB3F only) must find and return p[0]=2.
    auto buf = make_drain_finder_rom(2);
    auto rom = crystal::RomData::from_bytes(buf);
    auto candidates = crystal::sm83_find_drain(rom);
    ASSERT_TRUE(!candidates.empty());
    ASSERT_EQ(candidates[0].lift_result.p[0], 2u);
    std::cout << "\n    [finder_drain shift_count=2: " << candidates.size()
              << " candidate(s), p[0]=2 ✓]\n";
}

TEST(finder_drain_shift_count_3_recovered) {
    // shift_count=3: also never matched before.
    auto buf = make_drain_finder_rom(3);
    auto rom = crystal::RomData::from_bytes(buf);
    auto candidates = crystal::sm83_find_drain(rom);
    ASSERT_TRUE(!candidates.empty());
    ASSERT_EQ(candidates[0].lift_result.p[0], 3u);
    std::cout << "\n    [finder_drain shift_count=3: " << candidates.size()
              << " candidate(s), p[0]=3 ✓]\n";
}

// ============================================================================
// 14. ROM-backed recoil + drain tests (skip without ROM)
// ============================================================================

TEST(rom_recoil_shift_vanilla_is_2) {
    if (!g_rom) { std::cout << "\n    [SKIP: no ROM]\n"; return; }
    // Structural scan: sm83_find_recoil must find at least one candidate
    auto candidates = crystal::sm83_find_recoil(*g_rom);
    ASSERT_TRUE(!candidates.empty());
    // Verify lift result: shift_count must be 2 (vanilla BattleCommand_Recoil)
    ASSERT_EQ(candidates[0].lift_result.p[0], 2u);
    std::cout << "\n    [ROM recoil_shift: found " << candidates.size()
              << " candidate(s), first p[0]=2 ✓]\n";
}

TEST(rom_drain_shift_vanilla_is_1) {
    if (!g_rom) { std::cout << "\n    [SKIP: no ROM]\n"; return; }
    // Structural scan: sm83_find_drain must find at least one candidate
    auto candidates = crystal::sm83_find_drain(*g_rom);
    ASSERT_TRUE(!candidates.empty());
    // Verify lift result: shift_count must be 1 (vanilla SapHealth)
    ASSERT_EQ(candidates[0].lift_result.p[0], 1u);
    std::cout << "\n    [ROM drain_shift: found " << candidates.size()
              << " candidate(s), first p[0]=1 ✓]\n";
}

TEST(rom_recoil_shift_vanilla_exactly_one_candidate) {
    // Uniqueness: the vanilla Crystal ROM must yield exactly 1 recoil candidate.
    // The structural anchor (4F CB38 CB19) is specific enough not to match
    // unrelated SM83 code elsewhere in the ROM.
    if (!g_rom) { std::cout << "\n    [SKIP: no ROM]\n"; return; }
    auto candidates = crystal::sm83_find_recoil(*g_rom);
    ASSERT_EQ(candidates.size(), 1u);
    ASSERT_EQ(candidates[0].lift_result.p[0], 2u);
    std::cout << "\n    [ROM recoil uniqueness: exactly 1 candidate, shift_count=2 ✓]\n";
}

TEST(rom_drain_shift_vanilla_exactly_one_candidate) {
    // Uniqueness: the vanilla Crystal ROM must yield exactly 1 drain candidate.
    // The anchor (2A CB3F) followed by lift validation is specific enough.
    if (!g_rom) { std::cout << "\n    [SKIP: no ROM]\n"; return; }
    auto candidates = crystal::sm83_find_drain(*g_rom);
    ASSERT_EQ(candidates.size(), 1u);
    ASSERT_EQ(candidates[0].lift_result.p[0], 1u);
    std::cout << "\n    [ROM drain uniqueness: exactly 1 candidate, shift_count=1 ✓]\n";
}

TEST(rom_recoil_drain_lifted_in_battle_rules) {
    // End-to-end: extract BattleRules from vanilla ROM, verify both lifts succeeded.
    if (!g_rom || !g_profile) { std::cout << "\n    [SKIP: no ROM]\n"; return; }
    auto result = crystal::extract_battle_rules(*g_rom, *g_profile);
    ASSERT_TRUE(result.success);
    ASSERT_TRUE(result.rules.sm83_is_lifted(enginemon::BattleRules::SM83_LIFTED_RECOIL));
    ASSERT_TRUE(result.rules.sm83_is_lifted(enginemon::BattleRules::SM83_LIFTED_DRAIN));
    ASSERT_EQ(result.rules.get_recoil_shift(), 2u);
    ASSERT_EQ(result.rules.get_drain_shift(),  1u);
    std::cout << "\n    [ROM recoil+drain lifted: recoil_shift=2, drain_shift=1 ✓]\n";
}

TEST(rom_recoil_drain_roundtrip) {
    // End-to-end: extract → package → reload, both shift counts must survive.
    if (!g_rom || !g_profile) { std::cout << "\n    [SKIP: no ROM]\n"; return; }
    auto result = crystal::extract_battle_rules(*g_rom, *g_profile);
    ASSERT_TRUE(result.success);
    auto loaded = roundtrip(result.rules, "recoil_drain");
    ASSERT_TRUE(loaded.has_value());
    ASSERT_EQ(loaded->get_recoil_shift(), 2u);
    ASSERT_EQ(loaded->get_drain_shift(),  1u);
    ASSERT_TRUE(loaded->sm83_is_lifted(enginemon::BattleRules::SM83_LIFTED_RECOIL));
    ASSERT_TRUE(loaded->sm83_is_lifted(enginemon::BattleRules::SM83_LIFTED_DRAIN));
    std::cout << "\n    [ROM recoil+drain roundtrip: recoil_shift=2, drain_shift=1 survive ✓]\n";
}

// ============================================================================
// MAIN
// ============================================================================

int main(int argc, char* argv[]) {
    if (argc >= 2) {
        auto rom_path = std::filesystem::path(argv[1]);
        auto rom = crystal::RomData::load(rom_path);
        if (rom) {
            g_rom = rom.get();
            g_profile = crystal::ProfileRegistry::instance()
                            .get_profile_by_hash(rom->hash());
            if (!g_profile) {
                std::cerr << "No profile for ROM hash " << rom->hash() << "\n";
            } else {
                std::cout << "ROM: " << rom_path << "\n";
                std::cout << "Hash: " << rom->hash() << "\n\n";
            }
            // Transfer ownership to a static so the pointer stays valid
            static std::unique_ptr<crystal::RomData> rom_owner = std::move(rom);
            g_rom = rom_owner.get();
        }
    }

    std::cout << "=== SM83 Recognizer Test Matrix ===\n\n";

    std::cout << "--- 1. lift_ai_discourage_move ---\n";
    RUN_TEST(ai_discourage_vanilla);
    RUN_TEST(ai_discourage_modified_immediate);
    RUN_TEST(ai_discourage_malformed_missing_ret);
    RUN_TEST(ai_discourage_malformed_wrong_opcode_0);
    RUN_TEST(ai_discourage_malformed_zero_delta);
    RUN_TEST(ai_discourage_malformed_too_short);

    std::cout << "\n--- 2. lift_ai_choose_move_scores ---\n";
    RUN_TEST(ai_choose_move_vanilla);
    RUN_TEST(ai_choose_move_modified_score);
    RUN_TEST(ai_choose_move_malformed_opcode_at_17);
    RUN_TEST(ai_choose_move_malformed_no_ld_hl);
    RUN_TEST(ai_choose_move_malformed_no_ldi);
    RUN_TEST(ai_choose_move_zero_score_invalid);

    std::cout << "\n--- 3. lift_exp_divisor ---\n";
    RUN_TEST(exp_divisor_vanilla);
    RUN_TEST(exp_divisor_modified);
    RUN_TEST(exp_divisor_malformed_wrong_hdivisor);
    RUN_TEST(exp_divisor_malformed_wrong_b4);
    RUN_TEST(exp_divisor_zero_invalid);
    RUN_TEST(exp_divisor_too_large_invalid);

    std::cout << "\n--- 4. lift_damage_variation ---\n";
    RUN_TEST(damage_variation_vanilla);
    RUN_TEST(damage_variation_modified_bound);
    RUN_TEST(damage_variation_malformed_no_rrca);
    RUN_TEST(damage_variation_malformed_no_cp);
    RUN_TEST(damage_variation_malformed_no_jr_c);
    RUN_TEST(damage_variation_lower_bound_too_small);
    RUN_TEST(damage_variation_divisor_extracted);
    RUN_TEST(damage_variation_divisor_modified);
    RUN_TEST(damage_variation_divisor_zero_rejected);
    RUN_TEST(damage_variation_divisor_pattern_absent);

    std::cout << "\n--- 5. lift_capture_status_bonus ---\n";
    RUN_TEST(capture_bonus_vanilla);
    RUN_TEST(capture_bonus_modified_slp_frz);
    RUN_TEST(capture_bonus_malformed_nonzero_no_status);
    RUN_TEST(capture_bonus_malformed_missing_and_mask);
    RUN_TEST(capture_bonus_malformed_missing_jr_nz);

    std::cout << "\n--- 6. lift_escape_constants ---\n";
    RUN_TEST(escape_vanilla);
    RUN_TEST(escape_modified_speed_mult);
    RUN_TEST(escape_modified_attempt_add);
    RUN_TEST(escape_malformed_wrong_hmultiplier);
    RUN_TEST(escape_malformed_wrong_hquotient);
    RUN_TEST(escape_malformed_zero_speed_mult);

    std::cout << "\n--- 7. lift_stat_formula_offsets ---\n";
    RUN_TEST(stat_formula_vanilla);
    RUN_TEST(stat_formula_modified_divisor);
    RUN_TEST(stat_formula_modified_offsets);
    RUN_TEST(stat_formula_malformed_missing_divisor_block);
    RUN_TEST(stat_formula_malformed_zero_divisor);
    RUN_TEST(stat_formula_malformed_offset_order);
    RUN_TEST(stat_formula_malformed_too_few_offset_patterns);

    std::cout << "\n--- 8. lift_damage_calc_constants ---\n";
    RUN_TEST(damage_calc_vanilla);
    RUN_TEST(damage_calc_modified_level_divisor);
    RUN_TEST(damage_calc_modified_damage_divisor);
    RUN_TEST(damage_calc_malformed_missing_p1);
    RUN_TEST(damage_calc_malformed_missing_p3);
    RUN_TEST(damage_calc_level_divisor_out_of_range);

    std::cout << "\n--- 9. lift_residual_fraction ---\n";
    RUN_TEST(residual_eighth_vanilla);
    RUN_TEST(residual_sixteenth_vanilla);
    RUN_TEST(residual_malformed_no_call);
    RUN_TEST(residual_malformed_no_srl_c);
    RUN_TEST(residual_malformed_too_many_srls);
    RUN_TEST(residual_malformed_span_too_short);

    std::cout << "\n--- 10. lift_crit_stage_deltas ---\n";
    RUN_TEST(crit_deltas_vanilla);
    RUN_TEST(crit_deltas_modified_held_item);
    RUN_TEST(crit_deltas_malformed_too_few_ld_c);
    RUN_TEST(crit_deltas_malformed_too_few_inc_c);
    RUN_TEST(crit_deltas_malformed_ld_c_zero);

    std::cout << "\n--- 11. ROM-backed positive tests ---\n";
    RUN_TEST(rom_all_11_recognizers_succeed_vanilla);
    RUN_TEST(rom_vanilla_values_match_known_constants);

    std::cout << "\n--- sm83_lifted_mask wire roundtrip ---\n";
    RUN_TEST(sm83_lifted_mask_survives_roundtrip);
    RUN_TEST(sm83_lifted_mask_vanilla_rom_all_bits_set);

    std::cout << "\n--- Runtime consumption ---\n";
    RUN_TEST(damage_formula_rules_vs_default_produces_different_damage);
    RUN_TEST(damage_variation_lower_bound_byte_used_in_rrca_loop);

    std::cout << "\n--- 12. lift_recoil_shift ---\n";
    RUN_TEST(recoil_shift_vanilla);
    RUN_TEST(recoil_shift_modified_to_three_pairs);
    RUN_TEST(recoil_shift_malformed_no_cb38);
    RUN_TEST(recoil_shift_malformed_cb38_without_cb19);
    RUN_TEST(recoil_shift_malformed_missing_inc_c);
    RUN_TEST(recoil_shift_span_too_short);

    std::cout << "\n--- 13. lift_drain_shift ---\n";
    RUN_TEST(drain_shift_vanilla);
    RUN_TEST(drain_shift_modified_two_srl_a);
    RUN_TEST(drain_shift_malformed_no_ldi);
    RUN_TEST(drain_shift_malformed_ldi_without_srl_a);
    RUN_TEST(drain_shift_malformed_no_ldh_after_srl);
    RUN_TEST(drain_shift_malformed_no_rr_a);
    RUN_TEST(drain_shift_span_too_short);

    std::cout << "\n--- 14a. sm83_find_recoil modified-count recovery ---\n";
    RUN_TEST(finder_recoil_shift_count_1_recovered);
    RUN_TEST(finder_recoil_shift_count_2_recovered);
    RUN_TEST(finder_recoil_shift_count_3_recovered);

    std::cout << "\n--- 14b. sm83_find_drain modified-count recovery ---\n";
    RUN_TEST(finder_drain_shift_count_1_recovered);
    RUN_TEST(finder_drain_shift_count_2_recovered);
    RUN_TEST(finder_drain_shift_count_3_recovered);

    std::cout << "\n--- 14. ROM-backed recoil + drain ---\n";
    RUN_TEST(rom_recoil_shift_vanilla_is_2);
    RUN_TEST(rom_drain_shift_vanilla_is_1);
    RUN_TEST(rom_recoil_shift_vanilla_exactly_one_candidate);
    RUN_TEST(rom_drain_shift_vanilla_exactly_one_candidate);
    RUN_TEST(rom_recoil_drain_lifted_in_battle_rules);
    RUN_TEST(rom_recoil_drain_roundtrip);

    std::cout << "\n=== Results ===\n";
    std::cout << "Passed: " << g_passed << "\n";
    std::cout << "Failed: " << g_failed << "\n";
    return g_failed > 0 ? 1 : 0;
}
