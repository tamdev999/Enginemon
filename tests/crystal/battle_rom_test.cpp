// tests/crystal/battle_rom_test.cpp
//
// TRUE ROM→EXTRACTOR→BRLS→RUNTIME PROPAGATION TESTS
//
// These tests prove the complete battle rules pipeline from ROM bytes through to
// runtime calculator behavior. Each test:
//
//   1. Loads the real Crystal v1.1 ROM (argv[1])
//   2. Mutates one or more known table bytes in memory
//   3. Writes the mutated ROM bytes to a temp file and loads as RomData
//   4. Runs extract_battle_rules() on the mutated ROM
//   5. Serializes BattleRules to a BRLS package chunk via PackageWriter
//   6. Reads the package back via PackageReader
//   7. Verifies the runtime BattleRules reflects the ROM mutation — not stock data
//
// A test FAILS if:
//   - The extractor ignores the ROM change and returns stock data
//   - PackageWriter emits stock BRLS bytes instead of the modified values
//   - PackageReader deserializes incorrectly
//   - The runtime BattleRules falls back to hardcoded stock behavior
//
// Source addresses (Crystal v1.1):
//   CriticalHitMoves         0x0D:0x46A3  flat 0x346A3  (8 bytes: 7 IDs + 0xFF sentinel)
//   StatLevelMultipliers      0x0F:0x6D2B  flat 0x3ED2B  (13 × 2 bytes = 26 bytes)
//   AIDiscourageMove          0x0E:0x5503  flat 0x39503  (5 bytes: 7E C6 0A 77 C9)
//   AIChooseMove (init score) 0x11:0x40CE  flat 0x440CE  (30+ bytes)
//
// Run: battle_rom_test <rom_path>

#include "crystal/rom/loader.hpp"
#include "crystal/rom/profile.hpp"
#include "crystal/extract/battle_rules_extractor.hpp"
#include "crystal/output/native_package.hpp"
#include "engine/package/package_reader.hpp"
#include "engine/battle/battle_rules.hpp"
#include "engine/battle/calculator.hpp"
#include "engine/battle/battle.hpp"
#include "engine/core/types.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

// ============================================================================
// TEST FRAMEWORK (mirrors compiler_integrity_test.cpp conventions)
// ============================================================================

static int g_passed = 0;
static int g_failed = 0;
static bool g_current_failed = false;

#define ASSERT_TRUE(expr) \
    do { if (!(expr)) { \
        std::cerr << "  FAIL: " << #expr << " at line " << __LINE__ << "\n"; \
        g_current_failed = true; \
    } } while(0)

#define ASSERT_FALSE(expr)  ASSERT_TRUE(!(expr))
#define ASSERT_EQ(a, b)     ASSERT_TRUE((a) == (b))
#define ASSERT_NE(a, b)     ASSERT_TRUE((a) != (b))

#define TEST(name) static void test_##name()
#define RUN_TEST(name) \
    do { \
        g_current_failed = false; \
        std::cout << "  " << #name << " ... "; \
        std::cout.flush(); \
        test_##name(); \
        if (!g_current_failed) { ++g_passed; std::cout << "PASS\n"; } \
        else                   { ++g_failed; std::cout << "FAIL\n"; } \
    } while(0)

// ============================================================================
// GLOBALS
// ============================================================================

static const crystal::RomData*         g_rom     = nullptr;
static const crystal::ExtractionProfile* g_profile = nullptr;

// ============================================================================
// HELPERS
// ============================================================================

// Load a raw byte vector as RomData by writing to a temp file.
static std::unique_ptr<crystal::RomData> rom_from_bytes(
    const std::vector<uint8_t>& bytes, const std::string& tag)
{
    auto path = std::filesystem::temp_directory_path()
                / ("battle_rom_test_" + tag + ".gbc");
    {
        std::ofstream f(path, std::ios::binary);
        if (!f) return nullptr;
        f.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    }
    auto result = crystal::RomData::load(path);
    std::filesystem::remove(path);
    return result;
}

// Write BattleRules to a temp EMON package and read back via PackageReader.
// Returns loaded BattleRules or std::nullopt on any failure.
// Note: load_battle_rules() re-opens the file by path, so removal must happen AFTER load.
static std::optional<enginemon::BattleRules> brls_roundtrip(
    const enginemon::BattleRules& rules, const std::string& tag)
{
    crystal::PackageWriter writer;
    writer.set_source_rom("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", "test_v1");
    writer.add_battle_rules(rules);

    auto pkg_path = std::filesystem::temp_directory_path()
                    / ("battle_rom_test_pkg_" + tag + ".emon");
    if (!writer.write(pkg_path)) return std::nullopt;

    auto reader = enginemon::PackageReader::open(pkg_path);
    if (!reader) {
        std::filesystem::remove(pkg_path);
        return std::nullopt;
    }

    auto result = reader->load_battle_rules();
    std::filesystem::remove(pkg_path);  // Remove AFTER load (lazy re-open by path)
    return result;
}

// ============================================================================
// TEST 1: high_crit_moves mutation propagates to runtime
//
// CriticalHitMoves at flat 0x346A3 (0x0D:0x46A3):
//   [0]=0x02 KARATE_CHOP, [1]=0x0D RAZOR_WIND, [2]=0x4B RAZOR_LEAF,
//   [3]=0x98 CRABHAMMER, [4]=0xA3 SLASH,       [5]=0xB1 AEROBLAST,
//   [6]=0xEE CROSS_CHOP, [7]=0xFF sentinel
//
// Mutation: change index 4 (SLASH=0xA3) to 0x01 (POUND — not in vanilla list).
// After mutation: 0xA3 must not be high-crit; 0x01 must be high-crit.
// ============================================================================
TEST(high_crit_mutation_propagates_through_full_pipeline) {
    // Verified ROM address from profile:
    //   crystal profile: flat_offset(0x0d, 0x46a3) = 0x346A3
    constexpr uint32_t CRIT_MOVES_FLAT = 0x346A3u;
    constexpr uint8_t  SLASH_ID         = 0xA3u;
    constexpr uint8_t  POUND_ID         = 0x01u;  // Not in vanilla high-crit list

    // Baseline: verify vanilla ROM has SLASH=0xA3 at index 4
    {
        const auto& raw = g_rom->raw();
        ASSERT_EQ(raw[CRIT_MOVES_FLAT + 4], SLASH_ID);
    }

    // Build mutated ROM bytes: replace SLASH with POUND at index 4
    std::vector<uint8_t> mutated = g_rom->raw();
    mutated[CRIT_MOVES_FLAT + 4] = POUND_ID;  // 0xA3 → 0x01

    auto mut_rom = rom_from_bytes(mutated, "crit_mut");
    ASSERT_TRUE(mut_rom != nullptr);

    // Extract BattleRules from mutated ROM
    auto result = crystal::extract_battle_rules(*mut_rom, *g_profile);
    ASSERT_TRUE(result.success);
    if (!result.success) {
        std::cerr << "  extract_battle_rules failed: " << result.error << "\n";
        return;
    }

    const enginemon::BattleRules& extracted = result.rules;

    // SLASH (0xA3) must NOT be in the high-crit list
    ASSERT_FALSE(extracted.is_high_crit_move(static_cast<enginemon::MoveId>(SLASH_ID)));
    // POUND (0x01) MUST be in the high-crit list (it replaced SLASH)
    ASSERT_TRUE(extracted.is_high_crit_move(static_cast<enginemon::MoveId>(POUND_ID)));
    // Unchanged entries must still be present
    ASSERT_TRUE(extracted.is_high_crit_move(static_cast<enginemon::MoveId>(0x02)));  // KARATE_CHOP
    ASSERT_TRUE(extracted.is_high_crit_move(static_cast<enginemon::MoveId>(0xEE)));  // CROSS_CHOP

    // Roundtrip through BRLS package
    auto loaded_opt = brls_roundtrip(extracted, "crit_mut");
    ASSERT_TRUE(loaded_opt.has_value());
    if (!loaded_opt) return;
    const enginemon::BattleRules& loaded = *loaded_opt;

    // After roundtrip: same assertions must hold
    ASSERT_FALSE(loaded.is_high_crit_move(static_cast<enginemon::MoveId>(SLASH_ID)));
    ASSERT_TRUE( loaded.is_high_crit_move(static_cast<enginemon::MoveId>(POUND_ID)));
    ASSERT_TRUE( loaded.is_high_crit_move(static_cast<enginemon::MoveId>(0x02)));
    ASSERT_TRUE( loaded.is_high_crit_move(static_cast<enginemon::MoveId>(0xEE)));

    // Verify runtime calculator uses loaded rules (build_crit_stage):
    // With vanilla rules (SLASH in list): SLASH → stage=2
    // With mutated rules (SLASH not in list): SLASH → stage=0
    enginemon::BattlePokemon dummy_user{};
    dummy_user.volatile_status = 0;

    enginemon::MoveData slash_md{};
    slash_md.id = static_cast<enginemon::MoveId>(SLASH_ID);
    slash_md.animation_id = 0;

    // Vanilla rules should give crit stage 2 for SLASH
    {
        auto baseline_opt = crystal::extract_battle_rules(*g_rom, *g_profile);
        ASSERT_TRUE(baseline_opt.success);
        uint8_t vanilla_stage = enginemon::build_crit_stage(dummy_user, slash_md, baseline_opt.rules);
        ASSERT_EQ(vanilla_stage, 2u);  // SLASH is high-crit in vanilla
    }

    // Mutated rules: SLASH removed from list → crit stage 0
    uint8_t mut_stage = enginemon::build_crit_stage(dummy_user, slash_md, loaded);
    ASSERT_EQ(mut_stage, 0u);  // SLASH was removed — no high-crit bonus

    // POUND now gets stage 2
    enginemon::MoveData pound_md{};
    pound_md.id = static_cast<enginemon::MoveId>(POUND_ID);
    pound_md.animation_id = 0;
    uint8_t pound_stage = enginemon::build_crit_stage(dummy_user, pound_md, loaded);
    ASSERT_EQ(pound_stage, 2u);  // POUND is now high-crit

    std::cout << "\n    [ROM mut: SLASH(0xA3)→removed, POUND(0x01)→added; "
                 "stage 2 for POUND, 0 for SLASH after roundtrip]\n";
}

// ============================================================================
// TEST 2: stat multiplier mutation propagates to runtime
//
// StatLevelMultipliers at flat 0x3ED2B (0x0F:0x6D2B):
//   Each entry is {numerator u8, denominator u8}.
//   Entry index 4 (= stage -2) is the 5th entry: {0x32, 0x64} = 50/100 = 0.5×
//   (Crystal uses [stage+6] indexing: stage -2 = index 4)
//   Mutation: change numerator from 0x32 (50) to 0x10 (16) → ~0.16× instead of 0.5×
// ============================================================================
TEST(stat_mult_mutation_propagates_through_full_pipeline) {
    // StatLevelMultipliers: 13 entries × 2 bytes each
    // index 4 = stage -2: {0x32, 0x64} = 50/100 in vanilla
    constexpr uint32_t STAT_MULT_FLAT  = 0x3ED2Bu;
    constexpr uint32_t ENTRY4_OFFSET   = 4u * 2u;  // byte offset for index 4
    constexpr uint8_t  VANILLA_NUM     = 0x32u;     // 50
    constexpr uint8_t  MUTATED_NUM     = 0x10u;     // 16 (distinct from 50)

    // Verify vanilla ROM has expected value
    {
        const auto& raw = g_rom->raw();
        ASSERT_EQ(raw[STAT_MULT_FLAT + ENTRY4_OFFSET], VANILLA_NUM);
    }

    // Mutate: stage -2 numerator 0x32 → 0x10
    std::vector<uint8_t> mutated = g_rom->raw();
    mutated[STAT_MULT_FLAT + ENTRY4_OFFSET] = MUTATED_NUM;

    auto mut_rom = rom_from_bytes(mutated, "stat_mut");
    ASSERT_TRUE(mut_rom != nullptr);

    // Extract BattleRules from mutated ROM
    auto result = crystal::extract_battle_rules(*mut_rom, *g_profile);
    ASSERT_TRUE(result.success);
    if (!result.success) {
        std::cerr << "  extract_battle_rules failed: " << result.error << "\n";
        return;
    }
    const enginemon::BattleRules& extracted = result.rules;

    // stat_stage_mult[4] (stage -2) numerator must be 0x10, not 0x32
    ASSERT_EQ(extracted.stat_stage_mult[4].numerator, static_cast<uint8_t>(MUTATED_NUM));
    ASSERT_NE(extracted.stat_stage_mult[4].numerator, static_cast<uint8_t>(VANILLA_NUM));

    // Roundtrip through BRLS package
    auto loaded_opt = brls_roundtrip(extracted, "stat_mut");
    ASSERT_TRUE(loaded_opt.has_value());
    if (!loaded_opt) return;
    const enginemon::BattleRules& loaded = *loaded_opt;

    ASSERT_EQ(loaded.stat_stage_mult[4].numerator, static_cast<uint8_t>(MUTATED_NUM));
    ASSERT_NE(loaded.stat_stage_mult[4].numerator, static_cast<uint8_t>(VANILLA_NUM));

    // Verify runtime apply_stat_stage uses loaded rules:
    // stage -2 (index 4): mutated = 16/100 of base; vanilla = 50/100 of base
    // apply_stat_stage(100, -2, rules):
    //   vanilla:  100 * 50  / 100 = 50
    //   mutated:  100 * 16  / 100 = 16
    int32_t mutated_result = enginemon::apply_stat_stage(100, -2, loaded);
    ASSERT_EQ(mutated_result, 16);  // 100 * 16 / 100 = 16

    // Vanilla gives 50 — prove the mutated rules are actually used
    {
        auto baseline_opt = crystal::extract_battle_rules(*g_rom, *g_profile);
        ASSERT_TRUE(baseline_opt.success);
        int32_t vanilla_result = enginemon::apply_stat_stage(100, -2, baseline_opt.rules);
        ASSERT_EQ(vanilla_result, 50);  // 100 * 50 / 100 = 50
    }
    ASSERT_NE(mutated_result, 50);  // Confirms mutated rules used, not stock fallback

    std::cout << "\n    [ROM mut: stage-2 stat mult 50/100→16/100; "
                 "apply_stat_stage(100,-2)=16 not 50 after roundtrip]\n";
}

// ============================================================================
// TEST 3: AIDiscourageMove mutation propagates to runtime AI scoring
//
// AIDiscourageMove at flat 0x39503 (0x0E:0x5503):
//   Bytes: 7E C6 0A 77 C9
//   The routine adds A += 0x0A (10) to discourage a strong move.
//   Mutation: patch byte +2 from 0x0A (10) to 0x0F (15).
//   After mutation: ai_scores.discourage_strong == 15 (not 10).
//   Proves: recognizer extracts the actual operand, not a hardcoded constant.
// ============================================================================
TEST(ai_discourage_mutation_propagates_through_full_pipeline) {
    // Verified ROM address (Crystal v1.1):
    //   AIDiscourageMove: flat 0x39503, byte sequence: 7E C6 0A 77 C9
    //   Byte +2 is the ADD A,n immediate operand (= 0x0A = 10 in vanilla).
    constexpr uint32_t AI_DISC_FLAT      = 0x39503u;
    constexpr uint32_t OPERAND_OFFSET    = 2u;           // byte +2 of the routine
    constexpr uint8_t  VANILLA_DELTA     = 0x0Au;        // 10 in vanilla
    constexpr uint8_t  MUTATED_DELTA     = 0x0Fu;        // 15 — test value

    // Verify vanilla ROM has the expected byte
    {
        const auto& raw = g_rom->raw();
        ASSERT_EQ(raw[AI_DISC_FLAT + OPERAND_OFFSET], VANILLA_DELTA);
    }

    // Mutate: 0x0A → 0x0F (change discourage delta from 10 to 15)
    std::vector<uint8_t> mutated = g_rom->raw();
    mutated[AI_DISC_FLAT + OPERAND_OFFSET] = MUTATED_DELTA;

    auto mut_rom = rom_from_bytes(mutated, "ai_disc_mut");
    ASSERT_TRUE(mut_rom != nullptr);

    // Build profile using the mutated ROM's hash by passing the real profile's offsets.
    // Profile hash check uses the real ROM's hash; for mutated ROM we use the known
    // profile directly since the structure is identical (only data value changed).
    auto mut_result = crystal::extract_battle_rules(*mut_rom, *g_profile);
    ASSERT_TRUE(mut_result.success);
    if (!mut_result.success) {
        std::cerr << "  extract_battle_rules failed: " << mut_result.error << "\n";
        return;
    }
    const enginemon::BattleRules& extracted = mut_result.rules;

    // The recognizer must have extracted the mutated operand 0x0F = 15
    ASSERT_EQ(extracted.ai_scores.discourage_strong, static_cast<uint8_t>(MUTATED_DELTA));
    ASSERT_NE(extracted.ai_scores.discourage_strong, static_cast<uint8_t>(VANILLA_DELTA));

    // Roundtrip through BRLS package
    auto loaded_opt = brls_roundtrip(extracted, "ai_disc_mut");
    ASSERT_TRUE(loaded_opt.has_value());
    if (!loaded_opt) return;
    const enginemon::BattleRules& loaded = *loaded_opt;

    // discourage_strong must survive serialization
    ASSERT_EQ(loaded.ai_scores.discourage_strong, static_cast<uint8_t>(MUTATED_DELTA));
    ASSERT_NE(loaded.ai_scores.discourage_strong, static_cast<uint8_t>(VANILLA_DELTA));

    // Verify the loaded value reaches the MoveScores constructor.
    // decide() with rules builds MoveScores{rules} so discourage_strong=15.
    // We verify via a direct getter rather than spinning up a full Battle:
    ASSERT_EQ(loaded.get_ai_discourage_strong(), static_cast<uint8_t>(MUTATED_DELTA));

    // Prove vanilla gives 10 — confirms the test would catch a stock-data fallback
    {
        auto baseline = crystal::extract_battle_rules(*g_rom, *g_profile);
        ASSERT_TRUE(baseline.success);
        ASSERT_EQ(baseline.rules.ai_scores.discourage_strong, static_cast<uint8_t>(VANILLA_DELTA));
    }

    std::cout << "\n    [ROM mut: AIDiscourageMove +2: 0x0A→0x0F; "
                 "discourage_strong=15 (not 10) after full roundtrip]\n";
}

// ============================================================================
// TEST 4: AIChooseMove init score mutation propagates to runtime
//
// AIChooseMove at flat 0x440CE (0x11:0x40CE):
//   The LD A,n initializer at byte offset +17..+18 is: 3E 14
//   Byte +18 = 0x14 = 20 (vanilla init score).
//   Mutation: patch byte +18 from 0x14 (20) to 0x1E (30).
//   After mutation: ai_scores.init_score == 30 (not 20).
// ============================================================================
TEST(ai_init_score_mutation_propagates_through_full_pipeline) {
    // Verified ROM address (Crystal v1.1):
    //   AIChooseMove: flat 0x440CE, byte +18 is the LD A,n immediate = 0x14 (20).
    constexpr uint32_t AI_INIT_FLAT      = 0x440CEu;
    constexpr uint32_t OPERAND_OFFSET    = 18u;           // byte +18 of the routine
    constexpr uint8_t  VANILLA_SCORE     = 0x14u;         // 20 in vanilla
    constexpr uint8_t  MUTATED_SCORE     = 0x1Eu;         // 30 — test value

    // Verify vanilla ROM has the expected byte
    {
        const auto& raw = g_rom->raw();
        ASSERT_EQ(raw[AI_INIT_FLAT + OPERAND_OFFSET], VANILLA_SCORE);
    }

    // Mutate: 0x14 → 0x1E (change init score from 20 to 30)
    std::vector<uint8_t> mutated = g_rom->raw();
    mutated[AI_INIT_FLAT + OPERAND_OFFSET] = MUTATED_SCORE;

    auto mut_rom = rom_from_bytes(mutated, "ai_init_mut");
    ASSERT_TRUE(mut_rom != nullptr);

    auto mut_result = crystal::extract_battle_rules(*mut_rom, *g_profile);
    ASSERT_TRUE(mut_result.success);
    if (!mut_result.success) {
        std::cerr << "  extract_battle_rules failed: " << mut_result.error << "\n";
        return;
    }
    const enginemon::BattleRules& extracted = mut_result.rules;

    // The recognizer must have extracted the mutated operand 0x1E = 30
    ASSERT_EQ(extracted.ai_scores.init_score, static_cast<uint8_t>(MUTATED_SCORE));
    ASSERT_NE(extracted.ai_scores.init_score, static_cast<uint8_t>(VANILLA_SCORE));

    // Roundtrip through BRLS package
    auto loaded_opt = brls_roundtrip(extracted, "ai_init_mut");
    ASSERT_TRUE(loaded_opt.has_value());
    if (!loaded_opt) return;
    const enginemon::BattleRules& loaded = *loaded_opt;

    // init_score must survive serialization
    ASSERT_EQ(loaded.ai_scores.init_score, static_cast<uint8_t>(MUTATED_SCORE));
    ASSERT_NE(loaded.ai_scores.init_score, static_cast<uint8_t>(VANILLA_SCORE));
    ASSERT_EQ(loaded.get_ai_init_score(), static_cast<uint8_t>(MUTATED_SCORE));

    // Prove vanilla gives 20
    {
        auto baseline = crystal::extract_battle_rules(*g_rom, *g_profile);
        ASSERT_TRUE(baseline.success);
        ASSERT_EQ(baseline.rules.ai_scores.init_score, static_cast<uint8_t>(VANILLA_SCORE));
    }

    std::cout << "\n    [ROM mut: AIChooseMove +18: 0x14→0x1E; "
                 "ai_scores.init_score=30 (not 20) after full roundtrip]\n";
}

// ============================================================================
// MAIN
// ============================================================================

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: battle_rom_test <rom_path>\n";
        return 1;
    }

    std::filesystem::path rom_path = argv[1];
    auto rom = crystal::RomData::load(rom_path);
    if (!rom) {
        std::cerr << "Failed to load ROM: " << rom_path << "\n";
        return 1;
    }

    const crystal::ExtractionProfile* profile =
        crystal::ProfileRegistry::instance().get_profile_by_hash(rom->hash());
    if (!profile) {
        std::cerr << "No registered profile for ROM hash " << rom->hash() << "\n";
        return 1;
    }

    g_rom     = rom.get();
    g_profile = profile;

    std::cout << "=== Battle ROM Propagation Tests ===\n";
    std::cout << "ROM: " << rom_path << "\n";
    std::cout << "Hash: " << rom->hash() << "\n\n";

    RUN_TEST(high_crit_mutation_propagates_through_full_pipeline);
    RUN_TEST(stat_mult_mutation_propagates_through_full_pipeline);
    RUN_TEST(ai_discourage_mutation_propagates_through_full_pipeline);
    RUN_TEST(ai_init_score_mutation_propagates_through_full_pipeline);

    std::cout << "\n=== Results ===\n";
    std::cout << "Passed: " << g_passed << "\n";
    std::cout << "Failed: " << g_failed << "\n";
    return g_failed > 0 ? 1 : 0;
}
