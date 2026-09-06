// tests/crystal/effect_script_test.cpp
//
// Adversarial test suite for EffectScriptDecoder.
//
// Covers:
//   - 157/157 full vanilla-ROM corpus decode
//   - Golden byte sequences for key effects (source-backed against pokecrystal)
//   - Skull Bash (effect 145): 0xFE phase boundary retained, bytes after 0xFE present
//   - Relocated MEP/BCP: profile address = 0, scan must still find tables uniquely
//   - Bad pointer: MEP entry with invalid bank-relative address fails closed
//   - Invalid command byte: script with byte > num_effect_commands fails closed
//   - Missing 0xFF: script exceeding MAX_SCRIPT_LENGTH fails closed
//   - Zero counts: num_move_effects=0 or num_effect_commands=0 fail closed
//   - Profile/ROM mismatch: profile address differs from scan result, diagnostic emitted
//
// Run: effect_script_test <rom_path>
// ROM path required for ROM-backed tests (scan, corpus, golden, Skull Bash).
// Adversarial tests (no ROM) run regardless.

#include "crystal/rom/loader.hpp"
#include "crystal/rom/profile.hpp"
#include "crystal/extract/effect_script_decoder.hpp"
#include "crystal/battle/effect_semanticizer.hpp"
#include "engine/battle/semantic_effect.hpp"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>
#include <optional>

// ============================================================================
// TEST FRAMEWORK
// ============================================================================

static int  g_passed  = 0;
static int  g_failed  = 0;
static bool g_current_failed = false;

#define ASSERT_TRUE(expr) \
    do { if (!(expr)) { \
        std::fprintf(stderr, "  FAIL: %s  at line %d\n", #expr, __LINE__); \
        g_current_failed = true; \
    } } while(0)

#define ASSERT_FALSE(expr)  ASSERT_TRUE(!(expr))
#define ASSERT_EQ(a, b)     ASSERT_TRUE((a) == (b))
#define ASSERT_NE(a, b)     ASSERT_TRUE((a) != (b))

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

// Build a minimal RomData from a raw byte buffer (for adversarial tests).
// Does not parse Crystal header — purely for decoder unit tests.
static crystal::RomData make_rom(const std::vector<uint8_t>& bytes) {
    return crystal::RomData::from_bytes(bytes);
}

// Build a minimal valid ExtractionProfile with configurable counts + table addresses.
static crystal::ExtractionProfile make_profile(
    uint16_t num_effects   = 157,
    uint16_t num_commands  = 175,
    uint32_t mep_flat      = 0,
    uint32_t bcp_flat      = 0)
{
    crystal::ExtractionProfile p;
    p.counts.num_move_effects    = num_effects;
    p.counts.num_effect_commands = num_commands;
    p.offsets.move_effects_pointers    = mep_flat;
    p.offsets.battle_command_pointers  = bcp_flat;
    return p;
}

// Build a minimal ROM containing a complete MEP + BCP + one effect script.
// Layout:
//   [0..dispatcher_mep_start-1]: padding
//   dispatcher_mep_start:        MEP dispatcher pattern (11 bytes)
//   after dispatcher:            padding
//   dispatcher_bcp_start:        BCP dispatcher pattern (13 bytes)
//   mep_flat (table_start):      2 bytes LE pointer to script_flat (bank-relative)
//   mep_flat+2*(N-1):            remaining N-1 dummy entries pointing to same script
//   script_flat:                 script bytes
//
// For tests requiring one effect: num_effects=1, num_commands=0xAF (175).
struct MinimalRom {
    std::vector<uint8_t> bytes;
    uint32_t mep_flat;     // flat address of MEP table[0]
    uint8_t  mep_bank;
    uint32_t bcp_flat;     // flat address of BCP table[0]
    uint8_t  bcp_bank;
    uint32_t script_flat;  // flat address of script data
    uint16_t script_bank_rel;  // bank-relative address of script
};

// Build a minimal ROM for one effect script with given bytes.
// Places everything in "bank 9" (arbitrary; well within bank-local range).
static MinimalRom build_minimal_rom(
    const std::vector<uint8_t>& script_bytes,
    uint16_t num_effects  = 1,
    uint16_t num_commands = 175)
{
    MinimalRom r;
    r.mep_bank = 9;
    r.bcp_bank = 15;

    // ROM layout (all flat addresses):
    // [0x000]   zeros (pad)
    // [0x100]   MEP dispatcher instruction bytes (11 bytes)
    // [0x200]   BCP dispatcher instruction bytes (13 bytes)
    // [0x1000]  MEP table: num_effects × 2 bytes
    // [0x2000]  BCP table: num_commands × 2 bytes (dummy entries, each = 0x4100)
    // [0x3000]  effect script data

    const uint32_t rom_size = 0x80000;  // 512 KB (standard Crystal ROM)
    r.bytes.assign(rom_size, 0x00);

    // Script placement — bank 9 bank-relative
    // flat = bank*0x4000 + (bank_rel - 0x4000)
    // 0x3000 = 9*0x4000 + (x - 0x4000) → x = 0x3000 - 9*0x4000 + 0x4000 = 0x3000 - 0x20000 (underflow!)
    // Use a safe flat address in bank 9 range: [9*0x4000, 10*0x4000-1] = [0x24000, 0x27FFF]
    r.script_flat    = 0x25000u;
    r.script_bank_rel = static_cast<uint16_t>(r.script_flat - static_cast<uint32_t>(r.mep_bank) * 0x4000u + 0x4000u);

    // MEP table at bank 9
    r.mep_flat = 0x24400u;
    const uint16_t mep_bank_rel = static_cast<uint16_t>(
        r.mep_flat - static_cast<uint32_t>(r.mep_bank) * 0x4000u + 0x4000u);

    // BCP table at bank 15
    r.bcp_flat = 0x3E000u;
    const uint16_t bcp_bank_rel = static_cast<uint16_t>(
        r.bcp_flat - static_cast<uint32_t>(r.bcp_bank) * 0x4000u + 0x4000u);

    // Write script bytes at script_flat
    for (size_t k = 0; k < script_bytes.size() && r.script_flat + k < rom_size; ++k)
        r.bytes[r.script_flat + k] = script_bytes[k];

    // Write MEP table entries: all point to the same script
    for (uint16_t e = 0; e < num_effects && r.mep_flat + e * 2u + 1u < rom_size; ++e) {
        r.bytes[r.mep_flat + e * 2u]     = static_cast<uint8_t>(r.script_bank_rel & 0xFF);
        r.bytes[r.mep_flat + e * 2u + 1] = static_cast<uint8_t>(r.script_bank_rel >> 8);
    }

    // Write BCP dummy entries (each pointing to 0x4100 in bank 15, just needs to be valid)
    const uint16_t dummy_bcp_entry = 0x4100u;
    for (uint16_t c = 0; c < num_commands && r.bcp_flat + c * 2u + 1u < rom_size; ++c) {
        r.bytes[r.bcp_flat + c * 2u]     = static_cast<uint8_t>(dummy_bcp_entry & 0xFF);
        r.bytes[r.bcp_flat + c * 2u + 1] = static_cast<uint8_t>(dummy_bcp_entry >> 8);
    }

    // Write MEP dispatcher at offset 0x100:
    // 4F 06 00 21 lo hi 09 09 3E bank CD 00 00
    {
        uint32_t base = 0x100u;
        r.bytes[base+0]  = 0x4F;
        r.bytes[base+1]  = 0x06;
        r.bytes[base+2]  = 0x00;
        r.bytes[base+3]  = 0x21;
        r.bytes[base+4]  = static_cast<uint8_t>(mep_bank_rel & 0xFF);
        r.bytes[base+5]  = static_cast<uint8_t>(mep_bank_rel >> 8);
        r.bytes[base+6]  = 0x09;
        r.bytes[base+7]  = 0x09;
        r.bytes[base+8]  = 0x3E;
        r.bytes[base+9]  = r.mep_bank;
        r.bytes[base+10] = 0xCD;
    }

    // Write BCP dispatcher at offset 0x200:
    // 3D 4F 06 00 21 lo hi 09 09 C1 3E bank CD
    {
        uint32_t base = 0x200u;
        r.bytes[base+0]  = 0x3D;
        r.bytes[base+1]  = 0x4F;
        r.bytes[base+2]  = 0x06;
        r.bytes[base+3]  = 0x00;
        r.bytes[base+4]  = 0x21;
        r.bytes[base+5]  = static_cast<uint8_t>(bcp_bank_rel & 0xFF);
        r.bytes[base+6]  = static_cast<uint8_t>(bcp_bank_rel >> 8);
        r.bytes[base+7]  = 0x09;
        r.bytes[base+8]  = 0x09;
        r.bytes[base+9]  = 0xC1;
        r.bytes[base+10] = 0x3E;
        r.bytes[base+11] = r.bcp_bank;
        r.bytes[base+12] = 0xCD;
    }

    return r;
}

// ============================================================================
// 1. ZERO-COUNT FAIL-CLOSED TESTS (no ROM required)
// ============================================================================

TEST(zero_num_move_effects_fails_closed) {
    // Profile with num_move_effects=0 must fail even if tables.ok().
    const std::vector<uint8_t> script = {0x02, 0xFF};
    auto mr = build_minimal_rom(script);
    auto rom = make_rom(mr.bytes);
    auto profile = make_profile(0, 175, mr.mep_flat, mr.bcp_flat);
    crystal::ResolvedTables tables;
    tables.mep_flat = mr.mep_flat; tables.mep_bank = mr.mep_bank;
    tables.bcp_flat = mr.bcp_flat; tables.bcp_bank = mr.bcp_bank;
    ASSERT_TRUE(tables.ok());
    crystal::EffectScriptDecodeError err;
    auto result = crystal::EffectScriptDecoder::decode_all(rom, profile, tables, &err);
    ASSERT_FALSE(result.has_value());
    ASSERT_FALSE(err.message.empty());
    std::cout << "\n    [zero num_move_effects: \"" << err.message << "\" ✓]\n";
}

TEST(zero_num_effect_commands_fails_closed) {
    // Profile with num_effect_commands=0 must fail.
    const std::vector<uint8_t> script = {0x02, 0xFF};
    auto mr = build_minimal_rom(script);
    auto rom = make_rom(mr.bytes);
    auto profile = make_profile(1, 0, mr.mep_flat, mr.bcp_flat);
    crystal::ResolvedTables tables;
    tables.mep_flat = mr.mep_flat; tables.mep_bank = mr.mep_bank;
    tables.bcp_flat = mr.bcp_flat; tables.bcp_bank = mr.bcp_bank;
    crystal::EffectScriptDecodeError err;
    auto result = crystal::EffectScriptDecoder::decode_all(rom, profile, tables, &err);
    ASSERT_FALSE(result.has_value());
    ASSERT_FALSE(err.message.empty());
    std::cout << "\n    [zero num_effect_commands: \"" << err.message << "\" ✓]\n";
}

TEST(tables_not_ok_fails_closed) {
    // tables with mep_flat=0 must fail.
    const std::vector<uint8_t> script = {0x02, 0xFF};
    auto mr = build_minimal_rom(script);
    auto rom = make_rom(mr.bytes);
    auto profile = make_profile(1, 175, 0, 0);
    crystal::ResolvedTables tables;  // both flat = 0
    ASSERT_FALSE(tables.ok());
    crystal::EffectScriptDecodeError err;
    auto result = crystal::EffectScriptDecoder::decode_all(rom, profile, tables, &err);
    ASSERT_FALSE(result.has_value());
    std::cout << "\n    [tables not ok: fails closed ✓]\n";
}

// ============================================================================
// 2. DECODE_ONE ADVERSARIAL TESTS (no ROM required)
// ============================================================================

TEST(decode_one_normal_script) {
    // Minimal valid 2-byte script: command 0x02 + 0xFF.
    const std::vector<uint8_t> script = {0x02, 0xFF};
    auto mr = build_minimal_rom(script);
    auto rom = make_rom(mr.bytes);
    crystal::EffectScriptDecodeError err;
    auto result = crystal::EffectScriptDecoder::decode_one(
        rom, 0, mr.script_bank_rel, mr.script_flat, 175, &err);
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->is_terminated());
    ASSERT_EQ(result->bytes.size(), 2u);
    ASSERT_EQ(result->bytes[0].value, 0x02u);
    ASSERT_EQ(result->bytes[1].value, 0xFFu);
}

TEST(decode_one_invalid_command_byte_fails) {
    // Command byte 0xB0 = 176 > num_commands=175 must fail.
    const std::vector<uint8_t> script = {0x02, 0xB0, 0xFF};
    auto mr = build_minimal_rom(script);
    auto rom = make_rom(mr.bytes);
    crystal::EffectScriptDecodeError err;
    auto result = crystal::EffectScriptDecoder::decode_one(
        rom, 0, mr.script_bank_rel, mr.script_flat, 175, &err);
    ASSERT_FALSE(result.has_value());
    ASSERT_FALSE(err.message.empty());
    std::cout << "\n    [invalid command 0xB0: \"" << err.message << "\" ✓]\n";
}

TEST(decode_one_zero_command_byte_fails) {
    // Command byte 0x00 is not a valid command index.
    const std::vector<uint8_t> script = {0x00, 0xFF};
    auto mr = build_minimal_rom(script);
    auto rom = make_rom(mr.bytes);
    crystal::EffectScriptDecodeError err;
    auto result = crystal::EffectScriptDecoder::decode_one(
        rom, 0, mr.script_bank_rel, mr.script_flat, 175, &err);
    ASSERT_FALSE(result.has_value());
    ASSERT_FALSE(err.message.empty());
    std::cout << "\n    [command byte 0x00: \"" << err.message << "\" ✓]\n";
}

TEST(decode_one_missing_0xff_terminator_fails) {
    // Script of MAX_SCRIPT_LENGTH bytes with no 0xFF must fail.
    std::vector<uint8_t> script(crystal::EffectScriptDecoder::MAX_SCRIPT_LENGTH, 0x02);
    auto mr = build_minimal_rom(script);
    auto rom = make_rom(mr.bytes);
    crystal::EffectScriptDecodeError err;
    auto result = crystal::EffectScriptDecoder::decode_one(
        rom, 0, mr.script_bank_rel, mr.script_flat, 175, &err);
    ASSERT_FALSE(result.has_value());
    ASSERT_FALSE(err.message.empty());
    std::cout << "\n    [no 0xFF terminator: \"" << err.message << "\" ✓]\n";
}

TEST(decode_one_0xfe_phase_boundary_retained_not_terminating) {
    // 0xFE must NOT terminate decoding; bytes after 0xFE must appear in output.
    // Script: 0x02 0xFE 0x04 0xFF
    const std::vector<uint8_t> script = {0x02, 0xFE, 0x04, 0xFF};
    auto mr = build_minimal_rom(script);
    auto rom = make_rom(mr.bytes);
    crystal::EffectScriptDecodeError err;
    auto result = crystal::EffectScriptDecoder::decode_one(
        rom, 0, mr.script_bank_rel, mr.script_flat, 175, &err);
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->is_terminated());
    ASSERT_EQ(result->bytes.size(), 4u);
    ASSERT_EQ(result->bytes[0].value, 0x02u);
    ASSERT_EQ(result->bytes[1].value, 0xFEu);  // phase boundary retained
    ASSERT_TRUE(result->bytes[1].is_phase_boundary());
    ASSERT_EQ(result->bytes[2].value, 0x04u);  // bytes AFTER 0xFE present
    ASSERT_EQ(result->bytes[3].value, 0xFFu);
    ASSERT_TRUE(result->is_multi_phase());
    std::cout << "\n    [0xFE retained, not terminating; 4 bytes decoded ✓]\n";
}

TEST(decode_one_bad_flat_addr_fails) {
    // Script flat address out of ROM bounds must fail.
    const std::vector<uint8_t> tiny_rom(16, 0x00);
    auto rom = make_rom(tiny_rom);
    crystal::EffectScriptDecodeError err;
    auto result = crystal::EffectScriptDecoder::decode_one(
        rom, 0, 0x4100, 0x99999, 175, &err);
    ASSERT_FALSE(result.has_value());
    ASSERT_FALSE(err.message.empty());
    std::cout << "\n    [OOB flat addr: \"" << err.message << "\" ✓]\n";
}

// ============================================================================
// 3. DECODE_ALL ADVERSARIAL TESTS (no ROM required)
// ============================================================================

TEST(decode_all_bad_mep_pointer_fails) {
    // MEP entry with bank_rel outside [0x4000, 0x7FFF] must fail.
    // Build a ROM where MEP[0] = 0x0100 (below valid ROMX range).
    const std::vector<uint8_t> script = {0x02, 0xFF};
    auto mr = build_minimal_rom(script);
    // Overwrite MEP[0] with an invalid pointer (0x0100 = below 0x4000)
    mr.bytes[mr.mep_flat]     = 0x00;
    mr.bytes[mr.mep_flat + 1] = 0x01;  // = 0x0100 < 0x4000 → invalid
    auto rom = make_rom(mr.bytes);
    auto profile = make_profile(1, 175, mr.mep_flat, mr.bcp_flat);
    crystal::ResolvedTables tables;
    tables.mep_flat = mr.mep_flat; tables.mep_bank = mr.mep_bank;
    tables.bcp_flat = mr.bcp_flat; tables.bcp_bank = mr.bcp_bank;
    crystal::EffectScriptDecodeError err;
    auto result = crystal::EffectScriptDecoder::decode_all(rom, profile, tables, &err);
    ASSERT_FALSE(result.has_value());
    ASSERT_EQ(err.effect_id, 0u);
    ASSERT_FALSE(err.message.empty());
    std::cout << "\n    [bad MEP pointer: \"" << err.message << "\" ✓]\n";
}

TEST(decode_all_invalid_command_in_script_fails) {
    // Script containing command byte 0xB0 (> 0xAF) must fail.
    const std::vector<uint8_t> script = {0x02, 0xB0, 0xFF};
    auto mr = build_minimal_rom(script);
    auto rom = make_rom(mr.bytes);
    auto profile = make_profile(1, 175, mr.mep_flat, mr.bcp_flat);
    crystal::ResolvedTables tables;
    tables.mep_flat = mr.mep_flat; tables.mep_bank = mr.mep_bank;
    tables.bcp_flat = mr.bcp_flat; tables.bcp_bank = mr.bcp_bank;
    crystal::EffectScriptDecodeError err;
    auto result = crystal::EffectScriptDecoder::decode_all(rom, profile, tables, &err);
    ASSERT_FALSE(result.has_value());
    ASSERT_EQ(err.effect_id, 0u);
    std::cout << "\n    [invalid command 0xB0 in decode_all: fails ✓]\n";
}

TEST(decode_all_unterminated_script_fails) {
    // Script with no 0xFF within MAX_SCRIPT_LENGTH must fail.
    std::vector<uint8_t> script(crystal::EffectScriptDecoder::MAX_SCRIPT_LENGTH, 0x02);
    auto mr = build_minimal_rom(script);
    auto rom = make_rom(mr.bytes);
    auto profile = make_profile(1, 175, mr.mep_flat, mr.bcp_flat);
    crystal::ResolvedTables tables;
    tables.mep_flat = mr.mep_flat; tables.mep_bank = mr.mep_bank;
    tables.bcp_flat = mr.bcp_flat; tables.bcp_bank = mr.bcp_bank;
    crystal::EffectScriptDecodeError err;
    auto result = crystal::EffectScriptDecoder::decode_all(rom, profile, tables, &err);
    ASSERT_FALSE(result.has_value());
    std::cout << "\n    [unterminated script in decode_all: fails ✓]\n";
}

TEST(decode_all_success_one_effect) {
    // Minimal success case: one effect, one valid script.
    const std::vector<uint8_t> script = {0x02, 0x03, 0xFF};
    auto mr = build_minimal_rom(script);
    auto rom = make_rom(mr.bytes);
    auto profile = make_profile(1, 175, mr.mep_flat, mr.bcp_flat);
    crystal::ResolvedTables tables;
    tables.mep_flat = mr.mep_flat; tables.mep_bank = mr.mep_bank;
    tables.bcp_flat = mr.bcp_flat; tables.bcp_bank = mr.bcp_bank;
    crystal::EffectScriptDecodeError err;
    auto result = crystal::EffectScriptDecoder::decode_all(rom, profile, tables, &err);
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->size(), 1u);
    ASSERT_TRUE(result->scripts[0].is_terminated());
    ASSERT_EQ(result->scripts[0].bytes.size(), 3u);
    std::cout << "\n    [decode_all 1 effect: success ✓]\n";
}

// ============================================================================
// 4. RESOLVE_TABLES: profile/ROM mismatch test (no ROM required)
// ============================================================================

TEST(resolve_tables_profile_mismatch_emits_diagnostic_uses_scan) {
    // profile != 0, scan finds a DIFFERENT address → hard fail (not silent scan-wins).
    // Use 10 effects so scan_mep finds a valid candidate.
    const std::vector<uint8_t> script = {0x02, 0xFF};
    auto mr = build_minimal_rom(script, 10, 175);
    auto rom = make_rom(mr.bytes);

    // Supply a wrong (non-zero) MEP profile address — deliberately off by 0x100
    const uint32_t wrong_mep = mr.mep_flat + 0x100u;
    auto profile = make_profile(10, 175, wrong_mep, mr.bcp_flat);

    auto tables = crystal::EffectScriptDecoder::resolve_tables(rom, profile);

    // Must FAIL — contradiction between profile and scan
    ASSERT_FALSE(tables.ok());
    ASSERT_FALSE(tables.error.empty());
    std::cout << "\n    [profile mismatch → hard fail: \"" << tables.error << "\" ✓]\n";
}

TEST(resolve_tables_profile_zero_uses_scan) {
    // profile addresses = 0 → scan must yield exactly one candidate.
    // Use 10 effects so scan_mep 5-entry validation passes.
    const std::vector<uint8_t> script = {0x02, 0xFF};
    auto mr = build_minimal_rom(script, 10, 175);
    auto rom = make_rom(mr.bytes);
    auto profile = make_profile(10, 175, 0, 0);  // no profile addresses

    auto tables = crystal::EffectScriptDecoder::resolve_tables(rom, profile);

    ASSERT_TRUE(tables.ok());
    ASSERT_TRUE(tables.error.empty());
    ASSERT_EQ(tables.mep_flat, mr.mep_flat);
    ASSERT_EQ(tables.bcp_flat, mr.bcp_flat);
    std::cout << "\n    [profile=0: scan finds mep=0x" << std::hex << tables.mep_flat
              << " bcp=0x" << tables.bcp_flat << std::dec << " ✓]\n";
}

TEST(resolve_tables_mep_stale_profile_hard_fail) {
    // MEP stale-but-plausible profile/ROM mismatch test.
    // The profile address points to a completely different location than the scan.
    // Must hard-fail — no silent substitution.
    const std::vector<uint8_t> script = {0x02, 0x03, 0xFF};
    auto mr = build_minimal_rom(script, 10, 175);
    auto rom = make_rom(mr.bytes);

    // Supply a profile MEP address that is valid-looking but different from the
    // actual scanned address (e.g., 0x1000 bytes away from the real table).
    const uint32_t stale_mep = mr.mep_flat + 0x1000u;
    // BCP is correct so only MEP triggers the failure.
    auto profile = make_profile(10, 175, stale_mep, mr.bcp_flat);

    auto tables = crystal::EffectScriptDecoder::resolve_tables(rom, profile);

    ASSERT_FALSE(tables.ok());
    ASSERT_FALSE(tables.error.empty());
    // Error message must mention MoveEffectsPointers and the address contradiction.
    ASSERT_TRUE(tables.error.find("MoveEffectsPointers") != std::string::npos);
    std::cout << "\n    [MEP stale profile hard fail: \"" << tables.error << "\" ✓]\n";
}

TEST(resolve_tables_bcp_stale_profile_hard_fail) {
    // BCP stale-but-plausible profile/ROM mismatch test.
    // MEP is correct, BCP profile address is stale.
    const std::vector<uint8_t> script = {0x02, 0x03, 0xFF};
    auto mr = build_minimal_rom(script, 10, 175);
    auto rom = make_rom(mr.bytes);

    // BCP address off by 0x800 from the real scanned address.
    const uint32_t stale_bcp = mr.bcp_flat + 0x800u;
    auto profile = make_profile(10, 175, mr.mep_flat, stale_bcp);

    auto tables = crystal::EffectScriptDecoder::resolve_tables(rom, profile);

    ASSERT_FALSE(tables.ok());
    ASSERT_FALSE(tables.error.empty());
    ASSERT_TRUE(tables.error.find("BattleCommandPointers") != std::string::npos);
    std::cout << "\n    [BCP stale profile hard fail: \"" << tables.error << "\" ✓]\n";
}

// ============================================================================
// 5. ROM-BACKED TESTS (require ROM)
// ============================================================================

TEST(rom_scan_mep_finds_exactly_one_plausible_candidate) {
    if (!g_rom) { std::cout << "\n    [SKIP: no ROM]\n"; return; }
    auto hits = crystal::EffectScriptDecoder::scan_mep(*g_rom);
    // Must find exactly one candidate: MoveEffectsPointers at bank 9, 0x71F4
    ASSERT_EQ(hits.size(), 1u);
    ASSERT_EQ(hits[0].first,  0x271F4u);  // flat_offset(9, 0x71F4)
    ASSERT_EQ(hits[0].second, 9u);
    std::cout << "\n    [scan_mep: 1 candidate, flat=0x" << std::hex << hits[0].first
              << " bank=" << std::dec << (int)hits[0].second << " ✓]\n";
}

TEST(rom_scan_bcp_finds_exactly_one_candidate) {
    if (!g_rom) { std::cout << "\n    [SKIP: no ROM]\n"; return; }
    auto hits = crystal::EffectScriptDecoder::scan_bcp(*g_rom);
    // Must find exactly one candidate: BattleCommandPointers at bank 15, 0x7D28
    ASSERT_EQ(hits.size(), 1u);
    ASSERT_EQ(hits[0].first,  0x3FD28u);  // flat_offset(15, 0x7D28)
    ASSERT_EQ(hits[0].second, 15u);
    std::cout << "\n    [scan_bcp: 1 candidate, flat=0x" << std::hex << hits[0].first
              << " bank=" << std::dec << (int)hits[0].second << " ✓]\n";
}

TEST(rom_resolve_tables_profile_matches_scan) {
    if (!g_rom || !g_profile) { std::cout << "\n    [SKIP: no ROM]\n"; return; }
    auto tables = crystal::EffectScriptDecoder::resolve_tables(*g_rom, *g_profile);
    ASSERT_TRUE(tables.ok());
    ASSERT_EQ(tables.mep_flat, 0x271F4u);
    ASSERT_EQ(tables.mep_bank, 9u);
    ASSERT_EQ(tables.bcp_flat, 0x3FD28u);
    ASSERT_EQ(tables.bcp_bank, 15u);
    // No error when profile and scan agree
    ASSERT_TRUE(tables.error.empty());
    std::cout << "\n    [resolve_tables: profile matches scan, no error ✓]\n";
}

TEST(rom_resolve_tables_profile_zero_uses_scan) {
    // With profile addresses zeroed out, scan must find the tables.
    if (!g_rom) { std::cout << "\n    [SKIP: no ROM]\n"; return; }
    crystal::ExtractionProfile profile_no_addr;
    profile_no_addr.counts.num_move_effects    = 157;
    profile_no_addr.counts.num_effect_commands = 175;
    // leave offsets at 0 (default)
    auto tables = crystal::EffectScriptDecoder::resolve_tables(*g_rom, profile_no_addr);
    ASSERT_TRUE(tables.ok());
    ASSERT_EQ(tables.mep_flat, 0x271F4u);
    ASSERT_EQ(tables.bcp_flat, 0x3FD28u);
    std::cout << "\n    [resolve_tables profile=0: scan finds both tables ✓]\n";
}

TEST(rom_157_scripts_decode_all) {
    if (!g_rom || !g_profile) { std::cout << "\n    [SKIP: no ROM]\n"; return; }
    auto tables = crystal::EffectScriptDecoder::resolve_tables(*g_rom, *g_profile);
    ASSERT_TRUE(tables.ok());
    crystal::EffectScriptDecodeError err;
    auto corpus = crystal::EffectScriptDecoder::decode_all(*g_rom, *g_profile, tables, &err);
    if (!corpus.has_value()) {
        std::fprintf(stderr, "  FAIL: decode_all failed: %s (effect %u)\n",
                     err.message.c_str(), err.effect_id);
        ASSERT_TRUE(false);
        return;
    }
    ASSERT_EQ(corpus->size(), 157u);
    // Every script must be terminated
    for (size_t i = 0; i < corpus->size(); ++i) {
        ASSERT_TRUE(corpus->scripts[i].is_terminated());
        ASSERT_TRUE(!corpus->scripts[i].bytes.empty());
    }
    std::cout << "\n    [157/157 scripts decoded and terminated ✓]\n";
}

TEST(rom_golden_effect_0_normal_hit) {
    // Effect 0 = NormalHit.
    // Source-backed golden sequence (pokecrystal data/moves/effects.asm NormalHit:):
    //   checkobedience(02) usedmovetext(03) doturn(04) critical(05) damagestats(06)
    //   damagecalc(62) stab(07) damagevariation(08) checkhit(09) moveanim(AB)
    //   failuretext(0D) applydamage(0E) criticaltext(0F) supereffectivetext(10)
    //   checkfaint(11) buildopponentrage(12) kingsrock(4D) endmove(FF)
    if (!g_rom || !g_profile) { std::cout << "\n    [SKIP: no ROM]\n"; return; }
    auto tables = crystal::EffectScriptDecoder::resolve_tables(*g_rom, *g_profile);
    ASSERT_TRUE(tables.ok());
    crystal::EffectScriptDecodeError err;
    auto corpus = crystal::EffectScriptDecoder::decode_all(*g_rom, *g_profile, tables, &err);
    ASSERT_TRUE(corpus.has_value());

    const auto* s = corpus->get(0);
    ASSERT_TRUE(s != nullptr);
    const std::vector<uint8_t> expected = {
        0x02, 0x03, 0x04, 0x05, 0x06, 0x62, 0x07, 0x08, 0x09,
        0xAB, 0x0D, 0x0E, 0x0F, 0x10, 0x11, 0x12, 0x4D, 0xFF
    };
    ASSERT_EQ(s->bytes.size(), expected.size());
    for (size_t i = 0; i < expected.size(); ++i)
        ASSERT_EQ(s->bytes[i].value, expected[i]);
    std::cout << "\n    [NormalHit (effect 0): 18 bytes match golden ✓]\n";
}

TEST(rom_golden_effect_48_recoil_hit) {
    // Effect 48 = RecoilHit.
    // Same as NormalHit with `recoil`(0x27) inserted before checkfaint.
    // Golden: 02 03 04 05 06 62 07 08 09 AB 0D 0E 0F 10 27 11 12 4D FF
    if (!g_rom || !g_profile) { std::cout << "\n    [SKIP: no ROM]\n"; return; }
    auto tables = crystal::EffectScriptDecoder::resolve_tables(*g_rom, *g_profile);
    ASSERT_TRUE(tables.ok());
    crystal::EffectScriptDecodeError err;
    auto corpus = crystal::EffectScriptDecoder::decode_all(*g_rom, *g_profile, tables, &err);
    ASSERT_TRUE(corpus.has_value());

    const auto* s = corpus->get(48);
    ASSERT_TRUE(s != nullptr);
    const std::vector<uint8_t> expected = {
        0x02, 0x03, 0x04, 0x05, 0x06, 0x62, 0x07, 0x08, 0x09,
        0xAB, 0x0D, 0x0E, 0x0F, 0x10, 0x27, 0x11, 0x12, 0x4D, 0xFF
    };
    ASSERT_EQ(s->bytes.size(), expected.size());
    for (size_t i = 0; i < expected.size(); ++i)
        ASSERT_EQ(s->bytes[i].value, expected[i]);
    // recoil command is at index 14 = 0x27
    ASSERT_EQ(s->bytes[14].value, 0x27u);  // recoil
    std::cout << "\n    [RecoilHit (effect 48): 19 bytes, recoil(0x27) at index 14 ✓]\n";
}

TEST(rom_golden_effect_3_leech_hit) {
    // Effect 3 = LeechHit (Drain).
    // Golden: 02 03 04 05 06 62 07 08 09 AB 0D 0E 0F 15 11 12 4D FF
    // draintarget = 0x15
    if (!g_rom || !g_profile) { std::cout << "\n    [SKIP: no ROM]\n"; return; }
    auto tables = crystal::EffectScriptDecoder::resolve_tables(*g_rom, *g_profile);
    ASSERT_TRUE(tables.ok());
    crystal::EffectScriptDecodeError err;
    auto corpus = crystal::EffectScriptDecoder::decode_all(*g_rom, *g_profile, tables, &err);
    ASSERT_TRUE(corpus.has_value());

    const auto* s = corpus->get(3);
    ASSERT_TRUE(s != nullptr);
    const std::vector<uint8_t> expected = {
        0x02, 0x03, 0x04, 0x05, 0x06, 0x62, 0x07, 0x08, 0x09,
        0xAB, 0x0D, 0x0E, 0x0F, 0x10, 0x15, 0x11, 0x12, 0x4D, 0xFF
    };
    ASSERT_EQ(s->bytes.size(), expected.size());
    for (size_t i = 0; i < expected.size(); ++i)
        ASSERT_EQ(s->bytes[i].value, expected[i]);
    // draintarget = 0x15 at index 14
    ASSERT_EQ(s->bytes[14].value, 0x15u);  // draintarget
    std::cout << "\n    [LeechHit (effect 3): 19 bytes, draintarget(0x15) at index 14 ✓]\n";
}

TEST(rom_golden_effect_29_multi_hit) {
    // Effect 29 = MultiHit.
    // Contains startloop(0xAE) and endloop(0x24).
    // Golden: 02 03 04 AE 0A 09 05 06 62 07 08 A9 0B 0D 0E 0F 38 AD 11 12 24 0C 4D FF
    if (!g_rom || !g_profile) { std::cout << "\n    [SKIP: no ROM]\n"; return; }
    auto tables = crystal::EffectScriptDecoder::resolve_tables(*g_rom, *g_profile);
    ASSERT_TRUE(tables.ok());
    crystal::EffectScriptDecodeError err;
    auto corpus = crystal::EffectScriptDecoder::decode_all(*g_rom, *g_profile, tables, &err);
    ASSERT_TRUE(corpus.has_value());

    const auto* s = corpus->get(29);
    ASSERT_TRUE(s != nullptr);
    // Must contain startloop (0xAE) and endloop (0x24)
    bool has_startloop = false, has_endloop = false;
    for (const auto& b : s->bytes) {
        if (b.value == 0xAEu) has_startloop = true;
        if (b.value == 0x24u) has_endloop   = true;
    }
    ASSERT_TRUE(has_startloop);
    ASSERT_TRUE(has_endloop);
    ASSERT_TRUE(s->is_terminated());
    std::cout << "\n    [MultiHit (effect 29): has startloop(0xAE) and endloop(0x24) ✓]\n";
}

TEST(rom_golden_effect_38_ohko) {
    // Effect 38 = OHKOHit.  Contains ohko(0x26).
    // Golden: 02 03 04 07 26 AB 0D 0E 0F 10 11 12 FF
    if (!g_rom || !g_profile) { std::cout << "\n    [SKIP: no ROM]\n"; return; }
    auto tables = crystal::EffectScriptDecoder::resolve_tables(*g_rom, *g_profile);
    ASSERT_TRUE(tables.ok());
    crystal::EffectScriptDecodeError err;
    auto corpus = crystal::EffectScriptDecoder::decode_all(*g_rom, *g_profile, tables, &err);
    ASSERT_TRUE(corpus.has_value());

    const auto* s = corpus->get(38);
    ASSERT_TRUE(s != nullptr);
    bool has_ohko = false;
    for (const auto& b : s->bytes) if (b.value == 0x26u) { has_ohko = true; break; }
    ASSERT_TRUE(has_ohko);
    ASSERT_TRUE(s->is_terminated());
    std::cout << "\n    [OHKOHit (effect 38): has ohko(0x26) ✓]\n";
}

TEST(rom_golden_effect_7_selfdestruct) {
    // Effect 7 = Selfdestruct.  Contains selfdestruct(0x1A).
    // Golden: 02 03 04 05 06 62 07 08 09 1A 0B 0D 0E 0F 10 11 12 4D FF
    if (!g_rom || !g_profile) { std::cout << "\n    [SKIP: no ROM]\n"; return; }
    auto tables = crystal::EffectScriptDecoder::resolve_tables(*g_rom, *g_profile);
    ASSERT_TRUE(tables.ok());
    crystal::EffectScriptDecodeError err;
    auto corpus = crystal::EffectScriptDecoder::decode_all(*g_rom, *g_profile, tables, &err);
    ASSERT_TRUE(corpus.has_value());

    const auto* s = corpus->get(7);
    ASSERT_TRUE(s != nullptr);
    bool has_sd = false;
    for (const auto& b : s->bytes) if (b.value == 0x1Au) { has_sd = true; break; }
    ASSERT_TRUE(has_sd);
    std::cout << "\n    [Selfdestruct (effect 7): has selfdestruct(0x1A) ✓]\n";
}

TEST(rom_golden_effect_126_magnitude) {
    // Effect 126 = Magnitude.  Contains getmagnitude(0x66).
    // Golden: 02 03 04 05 06 66 62 07 08 09 99 AB 0D 0E 0F 10 11 12 4D FF
    if (!g_rom || !g_profile) { std::cout << "\n    [SKIP: no ROM]\n"; return; }
    auto tables = crystal::EffectScriptDecoder::resolve_tables(*g_rom, *g_profile);
    ASSERT_TRUE(tables.ok());
    crystal::EffectScriptDecodeError err;
    auto corpus = crystal::EffectScriptDecoder::decode_all(*g_rom, *g_profile, tables, &err);
    ASSERT_TRUE(corpus.has_value());

    const auto* s = corpus->get(126);
    ASSERT_TRUE(s != nullptr);
    bool has_getmag = false;
    for (const auto& b : s->bytes) if (b.value == 0x66u) { has_getmag = true; break; }
    ASSERT_TRUE(has_getmag);
    std::cout << "\n    [Magnitude (effect 126): has getmagnitude(0x66) ✓]\n";
}

TEST(rom_golden_effect_122_present) {
    // Effect 122 = Present.  Contains present(0x61).
    // Golden: 02 03 04 09 05 06 61 62 07 08 A9 0D 0E 0F 10 11 12 4D FF
    if (!g_rom || !g_profile) { std::cout << "\n    [SKIP: no ROM]\n"; return; }
    auto tables = crystal::EffectScriptDecoder::resolve_tables(*g_rom, *g_profile);
    ASSERT_TRUE(tables.ok());
    crystal::EffectScriptDecodeError err;
    auto corpus = crystal::EffectScriptDecoder::decode_all(*g_rom, *g_profile, tables, &err);
    ASSERT_TRUE(corpus.has_value());

    const auto* s = corpus->get(122);
    ASSERT_TRUE(s != nullptr);
    bool has_present = false;
    for (const auto& b : s->bytes) if (b.value == 0x61u) { has_present = true; break; }
    ASSERT_TRUE(has_present);
    std::cout << "\n    [Present (effect 122): has present(0x61) ✓]\n";
}

TEST(rom_skull_bash_effect_145_retains_0xFE_bytes_after) {
    // Effect 145 = SkullBash.
    // This is the only stock Crystal effect script containing a 0xFE phase boundary.
    // Golden (full): 3A 02 04 39 03 05 06 62 07 08 09 AB 0D 0E 0F 10 11 12 4D FE 71 8C FF
    //   0xFE is at index 19.
    //   After 0xFE: defenseup(0x71) statupmessage(0x8C) endmove(0xFF)
    if (!g_rom || !g_profile) { std::cout << "\n    [SKIP: no ROM]\n"; return; }
    auto tables = crystal::EffectScriptDecoder::resolve_tables(*g_rom, *g_profile);
    ASSERT_TRUE(tables.ok());
    crystal::EffectScriptDecodeError err;
    auto corpus = crystal::EffectScriptDecoder::decode_all(*g_rom, *g_profile, tables, &err);
    ASSERT_TRUE(corpus.has_value());

    const auto* s = corpus->get(145);
    ASSERT_TRUE(s != nullptr);

    const std::vector<uint8_t> expected = {
        0x3A, 0x02, 0x04, 0x39, 0x03, 0x05, 0x06, 0x62, 0x07, 0x08, 0x09,
        0xAB, 0x0D, 0x0E, 0x0F, 0x10, 0x11, 0x12, 0x4D,
        0xFE,       // phase boundary (endturn) — index 19
        0x71, 0x8C, // defenseup, statupmessage — AFTER 0xFE
        0xFF        // endmove terminator — index 22
    };
    ASSERT_EQ(s->bytes.size(), expected.size());
    for (size_t i = 0; i < expected.size(); ++i)
        ASSERT_EQ(s->bytes[i].value, expected[i]);

    // 0xFE is present at index 19
    ASSERT_EQ(s->bytes[19].value, 0xFEu);
    ASSERT_TRUE(s->bytes[19].is_phase_boundary());
    // Bytes after 0xFE: defenseup(0x71), statupmessage(0x8C), endmove(0xFF)
    ASSERT_EQ(s->bytes[20].value, 0x71u);  // defenseup
    ASSERT_EQ(s->bytes[21].value, 0x8Cu);  // statupmessage
    ASSERT_EQ(s->bytes[22].value, 0xFFu);  // endmove
    ASSERT_TRUE(s->is_multi_phase());
    ASSERT_TRUE(s->is_terminated());
    std::cout << "\n    [SkullBash (effect 145): 23 bytes, 0xFE at[19], "
              << "defenseup/statupmessage after 0xFE, 0xFF at[22] ✓]\n";
}

TEST(rom_all_scripts_have_no_invalid_bytes) {
    // Every decoded byte must be in {command [0x01..0xAF], 0xFE, 0xFF}.
    // 0x00 and anything > 0xAF but != 0xFE/0xFF must never appear.
    if (!g_rom || !g_profile) { std::cout << "\n    [SKIP: no ROM]\n"; return; }
    auto tables = crystal::EffectScriptDecoder::resolve_tables(*g_rom, *g_profile);
    ASSERT_TRUE(tables.ok());
    crystal::EffectScriptDecodeError err;
    auto corpus = crystal::EffectScriptDecoder::decode_all(*g_rom, *g_profile, tables, &err);
    ASSERT_TRUE(corpus.has_value());

    for (const auto& script : corpus->scripts) {
        for (size_t j = 0; j < script.bytes.size(); ++j) {
            const uint8_t v = script.bytes[j].value;
            const bool valid = (v >= 0x01 && v <= 0xAFu)  // command
                             || v == 0xFEu                  // phase boundary
                             || v == 0xFFu;                 // endmove
            if (!valid) {
                std::fprintf(stderr,
                    "  FAIL: effect %u byte[%zu] = 0x%02X is invalid\n",
                    script.effect_id, j, v);
                ASSERT_TRUE(false);
                return;
            }
        }
    }
    std::cout << "\n    [all 157 scripts: every byte is valid command/0xFE/0xFF ✓]\n";
}

// ============================================================================
// SEMANTICIZER TESTS
//
// These tests exercise EffectSemanticizer directly with hand-built
// DecodedEffectScript byte sequences.  No ROM required.
// ============================================================================

// Helper: build a DecodedEffectScript from a flat byte vector.
static crystal::DecodedEffectScript make_script(uint16_t effect_id,
                                                  const std::vector<uint8_t>& bytes)
{
    crystal::DecodedEffectScript s;
    s.effect_id = effect_id;
    for (uint8_t b : bytes) {
        crystal::EffectCommandByte cmd;
        cmd.value = b;
        s.bytes.push_back(cmd);
    }
    return s;
}

// ── attackdown2 (0x85) — must produce AttackDown2 stat_change, never Defrost ──

TEST(semanticize_attackdown2_stat_change_not_defrost) {
    // AttackDown2 effect script: checkhit(0x09) attackdown2(0x85) endmove(0xFF)
    // No damagecalc → has_standard_damage=false → attackdown2 must set stat_change=AttackDown2.
    // Previously fell through to case 0x53 (defrost), producing secondary_effect=Defrost.
    auto script = make_script(58,  // Crystal EFFECT_ATTACK_DOWN2 index
        {0x09, 0x85, 0xFF});
    auto desc = crystal::EffectSemanticizer::semanticize(script, 0);

    using namespace enginemon;
    ASSERT_EQ(static_cast<int>(desc.secondary_effect),
              static_cast<int>(SecondaryEffectType::None));
    ASSERT_EQ(static_cast<int>(desc.stat_change),
              static_cast<int>(StatChangeTarget::AttackDown2));
    ASSERT_FALSE(desc.has_standard_damage);
    std::cout << "\n    [attackdown2: stat_change=AttackDown2, secondary=None (not Defrost) ✓]\n";
}

TEST(semanticize_attackdown2_no_defrost_secondary) {
    // Adversarial: explicitly prove SecondaryEffectType::Defrost is NOT set.
    auto script = make_script(58, {0x09, 0x85, 0xFF});
    auto desc = crystal::EffectSemanticizer::semanticize(script, 0);
    ASSERT_NE(static_cast<int>(desc.secondary_effect),
              static_cast<int>(enginemon::SecondaryEffectType::Defrost));
    std::cout << "\n    [attackdown2: secondary_effect != Defrost ✓]\n";
}

TEST(semanticize_defrost_0x53_still_produces_defrost) {
    // Regression: 0x53 (defrost, FlameWheel/SacredFire) must still produce Defrost.
    // damagecalc(0x62) effectchance(0x90) defrost(0x53) endmove(0xFF)
    auto script = make_script(99, {0x62, 0x90, 0x53, 0xFF});
    auto desc = crystal::EffectSemanticizer::semanticize(script, 0);
    ASSERT_EQ(static_cast<int>(desc.secondary_effect),
              static_cast<int>(enginemon::SecondaryEffectType::Defrost));
    ASSERT_TRUE(desc.has_standard_damage);
    std::cout << "\n    [defrost (0x53): secondary=Defrost, has_damage=true ✓]\n";
}

// ── Capability gate: needs_substitute, needs_kingsrock, needs_rage ──

TEST(capability_gate_needs_substitute_is_informational) {
    // needs_substitute is an informational flag — does NOT block is_supported.
    // The substitute triggering state (SUBSTATUS_SUBSTITUTE) is impossible
    // in the current production runtime; the hook is conditionally unreachable.
    auto script = make_script(0, {0x62, 0x0A, 0xFF});
    auto desc = crystal::EffectSemanticizer::semanticize(script, 0);
    ASSERT_TRUE(desc.needs_substitute);
    ASSERT_TRUE(desc.is_supported);  // hook present but NOT blocking
    std::cout << "`n    [needs_substitute: lowersub(0x0A) is informational, is_supported=true ]`n";
}
TEST(capability_gate_needs_substitute_raisesub_informational) {
    // raisesub(0x0C) also sets needs_substitute — informational only.
    auto script = make_script(0, {0x62, 0x0C, 0xFF});
    auto desc = crystal::EffectSemanticizer::semanticize(script, 0);
    ASSERT_TRUE(desc.needs_substitute);
    ASSERT_TRUE(desc.is_supported);
    std::cout << "`n    [needs_substitute: raisesub(0x0C) is informational, is_supported=true ]`n";
}
TEST(capability_gate_needs_kingsrock_is_informational) {
    // kingsrock(0x4D) is an informational flag — does NOT block is_supported.
    // Triggering state (held King's Rock) impossible: held items always ITEM_NONE.
    auto script = make_script(0, {0x62, 0x4D, 0xFF});
    auto desc = crystal::EffectSemanticizer::semanticize(script, 0);
    ASSERT_TRUE(desc.needs_kingsrock);
    ASSERT_TRUE(desc.is_supported);
    std::cout << "`n    [needs_kingsrock: 0x4D is informational, is_supported=true ]`n";
}
TEST(capability_gate_needs_rage_is_informational) {
    // buildopponentrage(0x12) is informational — does NOT block is_supported.
    // Triggering state (Rage volatile) impossible: never set in production.
    auto script = make_script(0, {0x62, 0x12, 0xFF});
    auto desc = crystal::EffectSemanticizer::semanticize(script, 0);
    ASSERT_TRUE(desc.needs_rage);
    ASSERT_TRUE(desc.is_supported);
    std::cout << "`n    [needs_rage: 0x12 is informational, is_supported=true ]`n";
}

TEST(capability_gate_pure_damage_without_sub_kingsrock_rage_supported) {
    // Regression: a pure-damage script with none of the three capability flags
    // must still produce is_supported=true.
    // damagecalc(0x62) checkhit(0x09) applydamage(0x0E) endmove(0xFF)
    auto script = make_script(0, {0x62, 0x09, 0x0E, 0xFF});
    auto desc = crystal::EffectSemanticizer::semanticize(script, 0);
    ASSERT_FALSE(desc.needs_substitute);
    ASSERT_FALSE(desc.needs_kingsrock);
    ASSERT_FALSE(desc.needs_rage);
    ASSERT_TRUE(desc.is_supported);
    std::cout << "\n    [pure damage: needs_* all false → is_supported=true ✓]\n";
}

// ── AttackDown2 ROM-backed: effect 58 must produce AttackDown2 stat_change ──

TEST(rom_attackdown2_effect_58_stat_change) {
    // Crystal EFFECT_ATTACK_DOWN2 = effect index 58.
    // The decoded script contains attackdown2(0x85) and lowersub(0x0A)/raisesub(0x0C).
    // After fix: stat_change=AttackDown2, needs_substitute=true, is_supported=false.
    if (!g_rom || !g_profile) { std::cout << "\n    [SKIP: no ROM]\n"; return; }
    auto tables = crystal::EffectScriptDecoder::resolve_tables(*g_rom, *g_profile);
    ASSERT_TRUE(tables.ok());
    crystal::EffectScriptDecodeError err;
    auto corpus = crystal::EffectScriptDecoder::decode_all(*g_rom, *g_profile, tables, &err);
    ASSERT_TRUE(corpus.has_value());

    const auto* s = corpus->get(58);  // EFFECT_ATTACK_DOWN2
    ASSERT_TRUE(s != nullptr);

    auto desc = crystal::EffectSemanticizer::semanticize(*s, 0);
    // Core correctness: stat_change=AttackDown2, NOT Defrost
    ASSERT_EQ(static_cast<int>(desc.stat_change),
              static_cast<int>(enginemon::StatChangeTarget::AttackDown2));
    ASSERT_EQ(static_cast<int>(desc.secondary_effect),
              static_cast<int>(enginemon::SecondaryEffectType::None));
    ASSERT_TRUE(desc.needs_substitute);  // informational flag — does NOT block support
    ASSERT_FALSE(desc.is_supported);     // unsupported because stat_change dispatch not implemented
    std::cout << "`n    [ROM effect 58 AttackDown2: stat_change=AttackDown2, needs_substitute=true (info only), is_supported=false (no stat_change dispatch) ]`n";

    std::cout << "\n    [ROM effect 58 AttackDown2: stat_change=AttackDown2, needs_substitute=true, is_supported=false ✓]\n";
}

// ============================================================================
// ROM-backed: production move semanticization (capability-gate regression)
//
// Proves that needs_kingsrock / needs_rage / needs_substitute are informational
// flags that do NOT block is_supported for ordinary damaging moves.
// ============================================================================

TEST(rom_normalhit_effect_0_is_supported) {
    // Effect 0 = NormalHit (Tackle, Quick Attack).
    // Script contains buildopponentrage + kingsrock.
    // Both are conditionally unreachable: triggering state impossible in production.
    // Proves: needs_kingsrock=true, needs_rage=true, is_supported=true.
    if (!g_rom || !g_profile) { std::cout << "`n    [SKIP: no ROM]`n"; return; }
    auto tables = crystal::EffectScriptDecoder::resolve_tables(*g_rom, *g_profile);
    ASSERT_TRUE(tables.ok());
    crystal::EffectScriptDecodeError err;
    auto corpus = crystal::EffectScriptDecoder::decode_all(*g_rom, *g_profile, tables, &err);
    ASSERT_TRUE(corpus.has_value());
    const auto* s = corpus->get(0);
    ASSERT_TRUE(s != nullptr);
    auto desc = crystal::EffectSemanticizer::semanticize(*s, 0);
    ASSERT_TRUE(desc.needs_kingsrock);
    ASSERT_TRUE(desc.needs_rage);
    ASSERT_FALSE(desc.needs_substitute);
    ASSERT_TRUE(desc.has_standard_damage);
    ASSERT_TRUE(desc.is_supported);
    std::cout << "`n    [NormalHit(0): kingsrock=true rage=true is_supported=true ]`n";
}

TEST(rom_swift_effect_17_is_supported) {
    // Effect 17 = EFFECT_ALWAYS_HIT → NormalHit pointer (same script as effect 0).
    if (!g_rom || !g_profile) { std::cout << "`n    [SKIP: no ROM]`n"; return; }
    auto tables = crystal::EffectScriptDecoder::resolve_tables(*g_rom, *g_profile);
    ASSERT_TRUE(tables.ok());
    crystal::EffectScriptDecodeError err;
    auto corpus = crystal::EffectScriptDecoder::decode_all(*g_rom, *g_profile, tables, &err);
    ASSERT_TRUE(corpus.has_value());
    const auto* s = corpus->get(17);
    ASSERT_TRUE(s != nullptr);
    auto desc = crystal::EffectSemanticizer::semanticize(*s, 0);
    ASSERT_TRUE(desc.is_supported);
    std::cout << "`n    [Swift/effect-17 (NormalHit): is_supported=true ]`n";
}

TEST(rom_absorb_effect_3_is_supported) {
    // Effect 3 = EFFECT_LEECH_HIT -> LeechHit (Absorb).
    // Script: kingsrock + buildopponentrage — both conditionally unreachable.
    if (!g_rom || !g_profile) { std::cout << "`n    [SKIP: no ROM]`n"; return; }
    auto tables = crystal::EffectScriptDecoder::resolve_tables(*g_rom, *g_profile);
    ASSERT_TRUE(tables.ok());
    crystal::EffectScriptDecodeError err;
    auto corpus = crystal::EffectScriptDecoder::decode_all(*g_rom, *g_profile, tables, &err);
    ASSERT_TRUE(corpus.has_value());
    const auto* s = corpus->get(3);
    ASSERT_TRUE(s != nullptr);
    auto desc = crystal::EffectSemanticizer::semanticize(*s, 0);
    ASSERT_TRUE(desc.needs_kingsrock);
    ASSERT_TRUE(desc.needs_rage);
    ASSERT_TRUE(desc.has_drain);
    ASSERT_TRUE(desc.is_supported);
    std::cout << "`n    [LeechHit(3)/Absorb: drain=true is_supported=true ]`n";
}

TEST(rom_ember_effect_4_is_supported) {
    // Effect 4 = EFFECT_BURN_HIT -> BurnHit (Ember).
    // Script: buildopponentrage only (no kingsrock).
    if (!g_rom || !g_profile) { std::cout << "`n    [SKIP: no ROM]`n"; return; }
    auto tables = crystal::EffectScriptDecoder::resolve_tables(*g_rom, *g_profile);
    ASSERT_TRUE(tables.ok());
    crystal::EffectScriptDecodeError err;
    auto corpus = crystal::EffectScriptDecoder::decode_all(*g_rom, *g_profile, tables, &err);
    ASSERT_TRUE(corpus.has_value());
    const auto* s = corpus->get(4);
    ASSERT_TRUE(s != nullptr);
    auto desc = crystal::EffectSemanticizer::semanticize(*s, 0);
    ASSERT_FALSE(desc.needs_kingsrock);
    ASSERT_TRUE(desc.needs_rage);
    ASSERT_TRUE(desc.is_supported);
    std::cout << "`n    [BurnHit(4)/Ember: needs_rage=true is_supported=true ]`n";
}

TEST(rom_take_down_effect_48_is_supported) {
    // Effect 48 = EFFECT_RECOIL_HIT -> RecoilHit (Take Down).
    // Script: kingsrock + buildopponentrage — both conditionally unreachable.
    if (!g_rom || !g_profile) { std::cout << "`n    [SKIP: no ROM]`n"; return; }
    auto tables = crystal::EffectScriptDecoder::resolve_tables(*g_rom, *g_profile);
    ASSERT_TRUE(tables.ok());
    crystal::EffectScriptDecodeError err;
    auto corpus = crystal::EffectScriptDecoder::decode_all(*g_rom, *g_profile, tables, &err);
    ASSERT_TRUE(corpus.has_value());
    const auto* s = corpus->get(48);
    ASSERT_TRUE(s != nullptr);
    auto desc = crystal::EffectSemanticizer::semanticize(*s, 0);
    ASSERT_TRUE(desc.needs_kingsrock);
    ASSERT_TRUE(desc.needs_rage);
    ASSERT_TRUE(desc.has_recoil);
    ASSERT_TRUE(desc.is_supported);
    std::cout << "`n    [RecoilHit(48)/TakeDown: kingsrock/rage=true recoil=true is_supported=true ]`n";
}

TEST(rom_capability_hooks_do_not_block_support) {
    // Adversarial synthetic scripts: prove that each hook alone and together
    // does NOT prevent is_supported=true on a pure-damage script.
    using namespace enginemon;
    auto s_kr   = make_script(0, {0x62, 0x4D, 0xFF});
    auto d_kr   = crystal::EffectSemanticizer::semanticize(s_kr,   0);
    ASSERT_TRUE(d_kr.needs_kingsrock);   ASSERT_TRUE(d_kr.is_supported);
    auto s_rage = make_script(0, {0x62, 0x12, 0xFF});
    auto d_rage = crystal::EffectSemanticizer::semanticize(s_rage, 0);
    ASSERT_TRUE(d_rage.needs_rage);      ASSERT_TRUE(d_rage.is_supported);
    auto s_sub  = make_script(0, {0x62, 0x0A, 0xFF});
    auto d_sub  = crystal::EffectSemanticizer::semanticize(s_sub,  0);
    ASSERT_TRUE(d_sub.needs_substitute); ASSERT_TRUE(d_sub.is_supported);
    auto s_all  = make_script(0, {0x62, 0x4D, 0x12, 0x0A, 0xFF});
    auto d_all  = crystal::EffectSemanticizer::semanticize(s_all,  0);
    ASSERT_TRUE(d_all.needs_kingsrock && d_all.needs_rage && d_all.needs_substitute);
    ASSERT_TRUE(d_all.is_supported);
    std::cout << "`n    [capability hooks: kingsrock/rage/sub alone+together: is_supported=true ]`n";
}

// ============================================================================
// MAIN
// ============================================================================

int main(int argc, char* argv[]) {
    if (argc >= 2) {
        auto rom_path = std::filesystem::path(argv[1]);
        auto rom = crystal::RomData::load(rom_path);
        if (rom) {
            g_profile = crystal::ProfileRegistry::instance()
                            .get_profile_by_hash(rom->hash());
            if (!g_profile) {
                std::cerr << "[effect_script_test] No profile for ROM hash "
                          << rom->hash() << " — ROM-backed tests will skip\n";
            }
            static std::unique_ptr<crystal::RomData> rom_owner = std::move(rom);
            g_rom = rom_owner.get();
        } else {
            std::cerr << "[effect_script_test] Could not load ROM from "
                      << rom_path << "\n";
        }
    }

    std::cout << "=== Effect Script Decoder Tests ===\n\n";

    std::cout << "--- 1. Zero-count fail-closed ---\n";
    RUN_TEST(zero_num_move_effects_fails_closed);
    RUN_TEST(zero_num_effect_commands_fails_closed);
    RUN_TEST(tables_not_ok_fails_closed);

    std::cout << "\n--- 2. decode_one adversarial ---\n";
    RUN_TEST(decode_one_normal_script);
    RUN_TEST(decode_one_invalid_command_byte_fails);
    RUN_TEST(decode_one_zero_command_byte_fails);
    RUN_TEST(decode_one_missing_0xff_terminator_fails);
    RUN_TEST(decode_one_0xfe_phase_boundary_retained_not_terminating);
    RUN_TEST(decode_one_bad_flat_addr_fails);

    std::cout << "\n--- 3. decode_all adversarial ---\n";
    RUN_TEST(decode_all_bad_mep_pointer_fails);
    RUN_TEST(decode_all_invalid_command_in_script_fails);
    RUN_TEST(decode_all_unterminated_script_fails);
    RUN_TEST(decode_all_success_one_effect);

    std::cout << "\n--- 4. resolve_tables mismatch ---\n";
    RUN_TEST(resolve_tables_profile_mismatch_emits_diagnostic_uses_scan);
    RUN_TEST(resolve_tables_profile_zero_uses_scan);
    RUN_TEST(resolve_tables_mep_stale_profile_hard_fail);
    RUN_TEST(resolve_tables_bcp_stale_profile_hard_fail);

    std::cout << "\n--- 5. ROM-backed: table resolution ---\n";
    RUN_TEST(rom_scan_mep_finds_exactly_one_plausible_candidate);
    RUN_TEST(rom_scan_bcp_finds_exactly_one_candidate);
    RUN_TEST(rom_resolve_tables_profile_matches_scan);
    RUN_TEST(rom_resolve_tables_profile_zero_uses_scan);

    std::cout << "\n--- 6. ROM-backed: 157/157 corpus ---\n";
    RUN_TEST(rom_157_scripts_decode_all);
    RUN_TEST(rom_all_scripts_have_no_invalid_bytes);

    std::cout << "\n--- 7. ROM-backed: golden sequences ---\n";
    RUN_TEST(rom_golden_effect_0_normal_hit);
    RUN_TEST(rom_golden_effect_48_recoil_hit);
    RUN_TEST(rom_golden_effect_3_leech_hit);
    RUN_TEST(rom_golden_effect_29_multi_hit);
    RUN_TEST(rom_golden_effect_38_ohko);
    RUN_TEST(rom_golden_effect_7_selfdestruct);
    RUN_TEST(rom_golden_effect_126_magnitude);
    RUN_TEST(rom_golden_effect_122_present);

    std::cout << "\n--- 8. ROM-backed: Skull Bash 0xFE ---\n";
    RUN_TEST(rom_skull_bash_effect_145_retains_0xFE_bytes_after);

    std::cout << "`n--- 9. Semanticizer: attackdown2 + capability hooks (informational) ---`n";
    RUN_TEST(semanticize_attackdown2_stat_change_not_defrost);
    RUN_TEST(semanticize_attackdown2_no_defrost_secondary);
    RUN_TEST(semanticize_defrost_0x53_still_produces_defrost);
    RUN_TEST(capability_gate_needs_substitute_is_informational);
    RUN_TEST(capability_gate_needs_substitute_raisesub_informational);
    RUN_TEST(capability_gate_needs_kingsrock_is_informational);
    RUN_TEST(capability_gate_needs_rage_is_informational);
    RUN_TEST(capability_gate_pure_damage_without_sub_kingsrock_rage_supported);

    std::cout << "`n--- 10. ROM-backed: AttackDown2 effect 58 ---`n";
    RUN_TEST(rom_attackdown2_effect_58_stat_change);

    std::cout << "`n--- 11. ROM-backed: capability hook regression (Tackle/Swift/QA/Ember/TakeDown/Absorb) ---`n";
    RUN_TEST(rom_normalhit_effect_0_is_supported);
    RUN_TEST(rom_swift_effect_17_is_supported);
    RUN_TEST(rom_absorb_effect_3_is_supported);
    RUN_TEST(rom_ember_effect_4_is_supported);
    RUN_TEST(rom_take_down_effect_48_is_supported);
    RUN_TEST(rom_capability_hooks_do_not_block_support);

    std::cout << "\n--- 10. ROM-backed: AttackDown2 effect 58 ---\n";
    RUN_TEST(rom_attackdown2_effect_58_stat_change);

    std::cout << "\n=== Results ===\n";
    std::cout << "Passed: " << g_passed << "\n";
    std::cout << "Failed: " << g_failed << "\n";
    return g_failed > 0 ? 1 : 0;
}
