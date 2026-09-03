// tests/crystal/expansion_compat_test.cpp
//
// EXPANSION COMPATIBILITY ADVERSARIAL TESTS
//
// Proves:
//   1. Species > 251 detected by probe_profile_counts, never silently truncated
//   2. Moves > 251 detected by probe_profile_counts, never silently truncated
//   3. StdScripts > 52 detected by probe_profile_counts sentinel scan
//   4. Unknown/unmapped move effect produces SemEffect::Unknown (not stock behavior)
//   5. Physical/Special split: type-based derivation correct for Gen 2 types
//   6. Physical/Special split mutation: vanilla ROM move category roundtrip
//   7. probe_profile_counts agrees with vanilla Crystal v1.1 (no false positives)
//
// Run: expansion_compat_test [rom_path]
//   rom_path optional — ROM-backed tests skip gracefully when absent.

#include "crystal/rom/loader.hpp"
#include "crystal/rom/profile.hpp"
#include "crystal/battle/crystal_effects.hpp"
#include "crystal/output/native_package.hpp"
#include "engine/package/package_reader.hpp"
#include "engine/core/types.hpp"
#include "engine/battle/battle_rules.hpp"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#include <array>

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

#define ASSERT_FALSE(expr)    ASSERT_TRUE(!(expr))
#define ASSERT_EQ(a, b)       ASSERT_TRUE((a) == (b))
#define ASSERT_NE(a, b)       ASSERT_TRUE((a) != (b))

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

// Write BattleRules to temp package and read back.
static std::optional<enginemon::BattleRules> brls_roundtrip(
    const enginemon::BattleRules& rules, const std::string& tag)
{
    crystal::PackageWriter writer;
    writer.set_source_rom("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", "test");
    writer.add_battle_rules(rules);
    auto path = std::filesystem::temp_directory_path()
              / ("expansion_test_" + tag + ".emon");
    if (!writer.write(path)) return std::nullopt;
    auto reader = enginemon::PackageReader::open(path);
    if (!reader) { std::filesystem::remove(path); return std::nullopt; }
    auto r = reader->load_battle_rules();
    std::filesystem::remove(path);
    return r;
}

// Write moves to a temp package and read back the move registry.
// Minimal valid BattleRules included to satisfy PackageWriter.
static std::optional<enginemon::Registry<enginemon::MoveId, enginemon::MoveData>>
    moves_roundtrip(const std::vector<crystal::PackageWriter::MoveDataEntry>& entries,
                    const std::string& tag)
{
    crystal::PackageWriter writer;
    writer.set_source_rom("bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb", "test");

    // Minimal valid BattleRules
    enginemon::BattleRules br;
    br.stat_stage_mult = {{
        {25,100},{28,100},{33,100},{40,100},{50,100},{66,100},
        {1,1},{15,10},{2,1},{25,10},{3,1},{35,10},{4,1}
    }};
    br.acc_stage_mult = {{
        {33,100},{36,100},{43,100},{50,100},{60,100},{75,100},
        {1,1},{133,100},{166,100},{2,1},{233,100},{133,50},{3,1}
    }};
    br.crit_chances = {17,32,64,85,128,128,128};
    br.wobble_probabilities = {{{1,63},{255,255}}};
    br.trainer_class_ai.push_back({0,0,10,enginemon::AIPassSet::basic_only(),0x0000});
    writer.add_battle_rules(br);
    writer.add_move_data(entries);

    auto path = std::filesystem::temp_directory_path()
              / ("expansion_moves_" + tag + ".emon");
    if (!writer.write(path)) return std::nullopt;
    auto reader = enginemon::PackageReader::open(path);
    if (!reader) { std::filesystem::remove(path); return std::nullopt; }
    auto reg = reader->load_move_registry();
    std::filesystem::remove(path);
    return reg;
}

// Build a synthetic ROM that mimics Crystal's BaseData table structure.
// 'species_count' valid records followed by a text-like byte (0x94) as type1.
static std::vector<uint8_t> build_synthetic_rom_with_species_count(uint16_t species_count) {
    // Minimal ROM: ROM header area (0x200 bytes) + BaseData at flat 0x51424
    constexpr uint32_t BASE_DATA_FLAT = 0x51424;
    constexpr uint32_t RECORD_SIZE = 32;
    size_t rom_size = BASE_DATA_FLAT + (species_count + 2) * RECORD_SIZE + 0x100;
    std::vector<uint8_t> rom(rom_size, 0x00);

    // Write valid species records
    for (uint16_t i = 0; i < species_count; ++i) {
        uint32_t addr = BASE_DATA_FLAT + i * RECORD_SIZE;
        rom[addr + 0]  = static_cast<uint8_t>(i + 1);  // dex_num
        rom[addr + 1]  = 45;   // hp
        rom[addr + 7]  = 0x00; // type1 = NORMAL (valid)
        rom[addr + 8]  = 0x00; // type2 = NORMAL (valid)
        rom[addr + 9]  = 45;   // catch_rate
        rom[addr + 10] = 64;   // base_exp
    }
    // Write sentinel (invalid type byte)
    uint32_t sentinel_addr = BASE_DATA_FLAT + species_count * RECORD_SIZE;
    rom[sentinel_addr + 7] = 0x94;  // invalid type byte (text data range)
    rom[sentinel_addr + 8] = 0x91;

    return rom;
}

// Build a synthetic ROM with moves_count valid move records.
static std::vector<uint8_t> build_synthetic_rom_with_move_count(uint16_t moves_count) {
    constexpr uint32_t MOVES_FLAT = 0x41AFB;  // bank 0x10, ptr 0x5AFB
    constexpr uint32_t RECORD_SIZE = 7;
    size_t rom_size = MOVES_FLAT + (moves_count + 2) * RECORD_SIZE + 0x100;
    std::vector<uint8_t> rom(rom_size, 0x00);

    for (uint16_t i = 0; i < moves_count; ++i) {
        uint32_t addr = MOVES_FLAT + i * RECORD_SIZE;
        rom[addr + 0] = static_cast<uint8_t>(i + 1);  // anim
        rom[addr + 1] = 0x00;   // effect = NORMAL_HIT
        rom[addr + 2] = 40;     // power
        rom[addr + 3] = 0x00;   // type = NORMAL (valid < 0x1C)
        rom[addr + 4] = 0xFF;   // accuracy
        rom[addr + 5] = 35;     // pp
        rom[addr + 6] = 0;      // effect_chance
    }
    // Sentinel
    uint32_t sent = MOVES_FLAT + moves_count * RECORD_SIZE;
    rom[sent + 3] = 0xAF;  // invalid type byte

    return rom;
}

// ============================================================================
// TEST 1: probe_profile_counts detects species count mismatch (expanded > 251)
//
// The probe scans BaseData type bytes forward until an invalid type is found.
// If the ROM has 274 valid records and the profile says 251, the probe reports
// a mismatch — it does NOT silently pass.
// ============================================================================
TEST(probe_detects_species_expansion_beyond_profile) {
    // Build a synthetic ROM with 274 valid species records
    auto rom_bytes = build_synthetic_rom_with_species_count(274);

    // Build a profile that says 251 (vanilla — wrong for this ROM)
    auto& reg = crystal::ProfileRegistry::instance();
    // Use vanilla profile as base but override counts
    crystal::ExtractionProfile profile;
    profile.format.pokemon.base_data_size = 32;
    profile.format.pokemon.type1_offset   = 7;
    profile.format.pokemon.type2_offset   = 8;
    profile.offsets.base_data = 0x51424;
    profile.counts.num_pokemon = 251;  // Wrong — ROM has 274

    auto mismatches = crystal::ProfileRegistry::probe_profile_counts(
        profile, rom_bytes.data(), rom_bytes.size());

    // Must report num_pokemon mismatch
    bool found_species_mismatch = false;
    for (const auto& m : mismatches) {
        if (m.field == "num_pokemon") {
            found_species_mismatch = true;
            ASSERT_EQ(m.profile_count, 251u);
            ASSERT_EQ(m.rom_derived_count, 274u);
        }
    }
    ASSERT_TRUE(found_species_mismatch);
    std::cout << "\n    [probe correctly reports 274 species vs profile 251]\n";
}

// ============================================================================
// TEST 2: probe_profile_counts passes for vanilla Crystal (no false positives)
//
// The vanilla ROM has exactly 251 species. Probe must return 0 mismatches.
// ============================================================================
TEST(probe_no_mismatch_for_vanilla_species) {
    if (!g_rom || !g_profile) {
        std::cout << "\n    [SKIP: no ROM]\n";
        return;
    }
    const auto& raw = g_rom->raw();
    auto mismatches = crystal::ProfileRegistry::probe_profile_counts(
        *g_profile, raw.data(), raw.size());

    // Filter to just species-related mismatches
    for (const auto& m : mismatches) {
        if (m.field == "num_pokemon") {
            std::fprintf(stderr, "  False positive for num_pokemon: profile=%u rom=%u detail=%s\n",
                         m.profile_count, m.rom_derived_count, m.detail.c_str());
            g_current_failed = true;
        }
        if (m.field == "num_moves") {
            std::fprintf(stderr, "  False positive for num_moves: profile=%u rom=%u detail=%s\n",
                         m.profile_count, m.rom_derived_count, m.detail.c_str());
            g_current_failed = true;
        }
        if (m.field == "std_scripts_count") {
            std::fprintf(stderr, "  False positive for std_scripts_count: profile=%u rom=%u detail=%s\n",
                         m.profile_count, m.rom_derived_count, m.detail.c_str());
            g_current_failed = true;
        }
    }
    if (!g_current_failed) {
        std::cout << "\n    [no false positives on vanilla Crystal v1.1]\n";
    }
}

// ============================================================================
// TEST 3: probe_profile_counts detects moves expansion
// ============================================================================
TEST(probe_detects_moves_expansion_beyond_profile) {
    auto rom_bytes = build_synthetic_rom_with_move_count(300);

    crystal::ExtractionProfile profile;
    profile.format.move.move_data_size = 7;
    profile.format.move.type_offset    = 3;
    profile.offsets.moves    = 0x41AFB;
    profile.counts.num_moves = 251;  // Wrong

    auto mismatches = crystal::ProfileRegistry::probe_profile_counts(
        profile, rom_bytes.data(), rom_bytes.size());

    bool found = false;
    for (const auto& m : mismatches) {
        if (m.field == "num_moves") {
            found = true;
            ASSERT_EQ(m.profile_count, 251u);
            ASSERT_EQ(m.rom_derived_count, 300u);
        }
    }
    ASSERT_TRUE(found);
    std::cout << "\n    [probe correctly reports 300 moves vs profile 251]\n";
}

// ============================================================================
// TEST 4: probe_profile_counts detects StdScripts truncation
//
// If a hack adds StdScripts and we leave the count at 52, the probe must report
// the mismatch rather than silently compiling with truncated StdScripts.
// ============================================================================
TEST(probe_detects_stdscripts_truncation) {
    // Build a synthetic ROM with 60 valid StdScript entries followed by sentinel
    const uint32_t STD_BASE = 0xBC000;  // 0x2F * 0x4000
    const uint16_t NEW_COUNT = 60;
    std::vector<uint8_t> rom(STD_BASE + (NEW_COUNT + 2) * 3 + 0x100, 0x00);
    for (uint16_t i = 0; i < NEW_COUNT; ++i) {
        uint32_t addr = STD_BASE + i * 3;
        rom[addr + 0] = 0x2F;   // valid bank
        rom[addr + 1] = 0x9C;   // ptr lo = 0x409C (in range [0x4000, 0x7FFF])
        rom[addr + 2] = 0x40;   // ptr hi
    }
    // Sentinel: ptr = 0x012B < 0x4000 → invalid
    uint32_t sent = STD_BASE + NEW_COUNT * 3;
    rom[sent + 0] = 0x47;
    rom[sent + 1] = 0x2B;
    rom[sent + 2] = 0x01;

    crystal::ExtractionProfile profile;
    profile.offsets.std_scripts       = STD_BASE;
    profile.offsets.std_scripts_count = 52;  // Wrong — ROM has 60

    auto mismatches = crystal::ProfileRegistry::probe_profile_counts(
        profile, rom.data(), rom.size());

    bool found = false;
    for (const auto& m : mismatches) {
        if (m.field == "std_scripts_count") {
            found = true;
            ASSERT_EQ(m.profile_count, 52u);
            ASSERT_EQ(m.rom_derived_count, 60u);
        }
    }
    ASSERT_TRUE(found);
    std::cout << "\n    [probe correctly reports 60 StdScripts vs profile 52]\n";
}

// ============================================================================
// TEST 5: Unknown move effect maps to SemEffect::Unknown, not stock behavior
//
// A Crystal effect byte not in crystal::to_semantic_effect()'s switch returns
// SemEffect::Unknown = 0. Specifically: effect byte 0xFF (hypothetical hack
// effect) must produce Unknown, not any of the named SemEffect constants.
// ============================================================================
TEST(unknown_move_effect_produces_unknown_not_stock) {
    // Effect bytes that should produce Unknown (not mapped to any named SemEffect)
    // 0xFF: not a valid Crystal effect
    // 0x80: not in the known Crystal EFFECT_* range
    // 0x45: MINIMIZE (valid Crystal effect, but not mapped to a named SemEffect)
    const std::vector<std::pair<uint8_t, const char*>> test_cases = {
        {0xFF, "0xFF (out of range)"},
        {0x80, "0x80 (out of range)"},
        {0x45, "0x45 MINIMIZE (valid Crystal, unmapped SemEffect)"},
        {0x2E, "0x2E CONVERSION (valid Crystal, unmapped SemEffect)"},
    };

    for (const auto& [raw_effect, name] : test_cases) {
        enginemon::EffectId result = crystal::to_semantic_effect(raw_effect);
        ASSERT_EQ(result, enginemon::SemEffect::Unknown);
        if (result != enginemon::SemEffect::Unknown) {
            std::fprintf(stderr, "  Effect %s mapped to %u instead of Unknown(0)\n",
                         name, result);
        }
    }

    // Prove that KNOWN effects are NOT Unknown (sanity check)
    ASSERT_NE(crystal::to_semantic_effect(crystal::EffectId::SLEEP),
              enginemon::SemEffect::Unknown);
    ASSERT_NE(crystal::to_semantic_effect(crystal::EffectId::TOXIC),
              enginemon::SemEffect::Unknown);

    std::cout << "\n    [unknown effects → Unknown; known effects → named SemEffect]\n";
}

// ============================================================================
// TEST 6: unknown effect survives package roundtrip as Unknown (not converted)
//
// A MoveDataEntry with an Unknown effect_id must roundtrip without being
// "upgraded" to a named effect. Proves no silent stock-behavior substitution.
// ============================================================================
TEST(unknown_effect_roundtrip_stays_unknown) {
    crystal::PackageWriter::MoveDataEntry e;
    e.id            = 252;          // hypothetical expanded move
    e.type_id       = 0x00;         // Normal
    e.power         = 80;
    e.accuracy      = 0xFF;
    e.pp            = 15;
    e.effect_id     = enginemon::SemEffect::Unknown;  // 0 — unmapped effect
    e.effect_chance = 0;
    e.category      = static_cast<uint8_t>(enginemon::MoveCategory::Physical);

    auto reg_opt = moves_roundtrip({e}, "unknown_effect");
    ASSERT_TRUE(reg_opt.has_value());
    if (!reg_opt) return;

    const enginemon::MoveData* md = reg_opt->get(252);
    ASSERT_TRUE(md != nullptr);
    if (!md) return;

    // effect_id must still be Unknown after roundtrip — NOT upgraded to anything
    ASSERT_EQ(md->effect_id, enginemon::SemEffect::Unknown);
    ASSERT_NE(md->effect_id, enginemon::SemEffect::Sleep);
    ASSERT_NE(md->effect_id, enginemon::SemEffect::Toxic);

    std::cout << "\n    [Unknown effect survives roundtrip; not promoted to stock behavior]\n";
}

// ============================================================================
// TEST 7: Physical/Special split — Gen 2 type-based derivation correct
//
// Verifies crystal_move_category_from_type() for representative types:
//   Physical: NORMAL(00), FIGHTING(01), ROCK(05), STEEL(09)
//   Special:  FIRE(14), WATER(15), PSYCHIC(18), DARK(1B)
//   Status:   power=0 (any type)
// ============================================================================
TEST(gen2_type_based_ps_split_correct) {
    using Cat = enginemon::MoveCategory;

    // Physical types (< 0x13 = 19 decimal)
    ASSERT_EQ(crystal::crystal_move_category_from_type(0x00, 40), Cat::Physical);  // NORMAL
    ASSERT_EQ(crystal::crystal_move_category_from_type(0x01, 75), Cat::Physical);  // FIGHTING
    ASSERT_EQ(crystal::crystal_move_category_from_type(0x02, 70), Cat::Physical);  // FLYING
    ASSERT_EQ(crystal::crystal_move_category_from_type(0x03, 60), Cat::Physical);  // POISON
    ASSERT_EQ(crystal::crystal_move_category_from_type(0x04, 80), Cat::Physical);  // GROUND
    ASSERT_EQ(crystal::crystal_move_category_from_type(0x05, 50), Cat::Physical);  // ROCK
    ASSERT_EQ(crystal::crystal_move_category_from_type(0x07, 40), Cat::Physical);  // BUG
    ASSERT_EQ(crystal::crystal_move_category_from_type(0x08, 65), Cat::Physical);  // GHOST
    ASSERT_EQ(crystal::crystal_move_category_from_type(0x09, 70), Cat::Physical);  // STEEL

    // Special types (>= 0x13)
    ASSERT_EQ(crystal::crystal_move_category_from_type(0x14, 95), Cat::Special);   // FIRE
    ASSERT_EQ(crystal::crystal_move_category_from_type(0x15, 90), Cat::Special);   // WATER
    ASSERT_EQ(crystal::crystal_move_category_from_type(0x16, 75), Cat::Special);   // GRASS
    ASSERT_EQ(crystal::crystal_move_category_from_type(0x17, 90), Cat::Special);   // ELECTRIC
    ASSERT_EQ(crystal::crystal_move_category_from_type(0x18, 90), Cat::Special);   // PSYCHIC
    ASSERT_EQ(crystal::crystal_move_category_from_type(0x19, 90), Cat::Special);   // ICE
    ASSERT_EQ(crystal::crystal_move_category_from_type(0x1A, 80), Cat::Special);   // DRAGON
    ASSERT_EQ(crystal::crystal_move_category_from_type(0x1B, 80), Cat::Special);   // DARK

    // Status: power = 0, any type
    ASSERT_EQ(crystal::crystal_move_category_from_type(0x18, 0), Cat::Status);  // PSYCHIC, power=0
    ASSERT_EQ(crystal::crystal_move_category_from_type(0x00, 0), Cat::Status);  // NORMAL, power=0
    ASSERT_EQ(crystal::crystal_move_category_from_type(0x14, 0), Cat::Status);  // FIRE, power=0

    std::cout << "\n    [Gen 2 type-based P/S split: Physical<0x13, Special>=0x13, power=0→Status]\n";
}

// ============================================================================
// TEST 8: P/S category survives MoveData package roundtrip
//
// A Physical Fire move (type=FIRE, power>0) must roundtrip as Special.
// A Physical Normal move (type=NORMAL, power>0) must roundtrip as Physical.
// A Status Psychic move (type=PSYCHIC, power=0) must roundtrip as Status.
// ============================================================================
TEST(ps_category_survives_package_roundtrip) {
    using Cat = enginemon::MoveCategory;

    auto make_entry = [](enginemon::MoveId id, uint8_t type_id, uint8_t power) {
        crystal::PackageWriter::MoveDataEntry e;
        e.id         = id;
        e.type_id    = type_id;
        e.power      = power;
        e.accuracy   = 0xFF;
        e.pp         = 10;
        e.effect_id  = enginemon::SemEffect::Unknown;
        e.effect_chance = 0;
        e.category   = static_cast<uint8_t>(
            crystal::crystal_move_category_from_type(type_id, power));
        return e;
    };

    std::vector<crystal::PackageWriter::MoveDataEntry> entries = {
        make_entry(1,  0x14, 95),  // FIRE, power>0 → Special
        make_entry(2,  0x00, 40),  // NORMAL, power>0 → Physical
        make_entry(3,  0x18,  0),  // PSYCHIC, power=0 → Status
        make_entry(4,  0x1B, 80),  // DARK, power>0 → Special
        make_entry(5,  0x05, 50),  // ROCK, power>0 → Physical
    };

    auto reg_opt = moves_roundtrip(entries, "ps_roundtrip");
    ASSERT_TRUE(reg_opt.has_value());
    if (!reg_opt) return;

    const auto& reg = *reg_opt;

    // FIRE → Special
    const enginemon::MoveData* fire = reg.get(1);
    ASSERT_TRUE(fire != nullptr);
    if (fire) ASSERT_EQ(fire->category, Cat::Special);

    // NORMAL → Physical
    const enginemon::MoveData* normal = reg.get(2);
    ASSERT_TRUE(normal != nullptr);
    if (normal) ASSERT_EQ(normal->category, Cat::Physical);

    // PSYCHIC power=0 → Status
    const enginemon::MoveData* psychic = reg.get(3);
    ASSERT_TRUE(psychic != nullptr);
    if (psychic) ASSERT_EQ(psychic->category, Cat::Status);

    // DARK → Special
    const enginemon::MoveData* dark = reg.get(4);
    ASSERT_TRUE(dark != nullptr);
    if (dark) ASSERT_EQ(dark->category, Cat::Special);

    // ROCK → Physical
    const enginemon::MoveData* rock = reg.get(5);
    ASSERT_TRUE(rock != nullptr);
    if (rock) ASSERT_EQ(rock->category, Cat::Physical);

    std::cout << "\n    [P/S category roundtrip: Fire→Special, Normal→Physical, "
                 "Psychic(power=0)→Status, Dark→Special, Rock→Physical]\n";
}

// ============================================================================
// TEST 9: P/S split mutation — real ROM, two moves with different categories
//
// Using the real Crystal ROM: verifies that Flamethrower (FIRE type, power=95)
// gets Special, and Body Slam (NORMAL type, power=85) gets Physical.
// Proves the type-based derivation produces distinct categories for these two
// representative moves from the real ROM.
// ============================================================================
TEST(ps_split_real_rom_fire_vs_normal_distinct) {
    if (!g_rom || !g_profile) {
        std::cout << "\n    [SKIP: no ROM]\n";
        return;
    }

    // Crystal move IDs:
    //   FLAMETHROWER = 53 (0x35), type=FIRE(0x14), power=95
    //   BODY_SLAM    = 34 (0x22), type=NORMAL(0x00), power=85
    // These are 1-indexed; ROM entry = (id-1) * 7 bytes from o.moves
    const auto& o   = g_profile->offsets;
    const auto& fmt = g_profile->format.move;

    auto read_move_entry = [&](uint16_t move_id) {
        uint32_t addr = o.moves + static_cast<uint32_t>(move_id - 1) * fmt.move_data_size;
        return g_rom->read_bytes(addr, fmt.move_data_size);
    };

    // Flamethrower (move 53)
    auto ft_rec = read_move_entry(53);
    uint8_t ft_type  = ft_rec[fmt.type_offset];
    uint8_t ft_power = ft_rec[fmt.power_offset];
    auto ft_cat = crystal::crystal_move_category_from_type(ft_type, ft_power);

    // Body Slam (move 34)
    auto bs_rec = read_move_entry(34);
    uint8_t bs_type  = bs_rec[fmt.type_offset];
    uint8_t bs_power = bs_rec[fmt.power_offset];
    auto bs_cat = crystal::crystal_move_category_from_type(bs_type, bs_power);

    // Flamethrower: FIRE = 0x14 ≥ 0x13 → Special
    ASSERT_EQ(ft_type, 0x14u);  // confirm it's actually FIRE in the ROM
    ASSERT_EQ(ft_cat, enginemon::MoveCategory::Special);

    // Body Slam: NORMAL = 0x00 < 0x13 → Physical
    ASSERT_EQ(bs_type, 0x00u);  // confirm it's NORMAL in the ROM
    ASSERT_EQ(bs_cat, enginemon::MoveCategory::Physical);

    // They must differ (this is the mutation test)
    ASSERT_NE(ft_cat, bs_cat);

    std::cout << "\n    [ROM mutation: Flamethrower(FIRE)→Special, BodySlam(NORMAL)→Physical; "
                 "categories differ as expected]\n";
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
            if (g_profile) {
                std::cout << "ROM: " << rom_path << "\n";
                std::cout << "Hash: " << rom->hash() << "\n\n";
                static std::unique_ptr<crystal::RomData> rom_owner = std::move(rom);
                g_rom = rom_owner.get();
            } else {
                std::cerr << "No profile for ROM hash — ROM-backed tests will skip\n";
            }
        }
    }

    std::cout << "=== Expansion Compatibility Tests ===\n\n";

    std::cout << "--- Count probe tests ---\n";
    RUN_TEST(probe_detects_species_expansion_beyond_profile);
    RUN_TEST(probe_no_mismatch_for_vanilla_species);
    RUN_TEST(probe_detects_moves_expansion_beyond_profile);
    RUN_TEST(probe_detects_stdscripts_truncation);

    std::cout << "\n--- Move effect boundary tests ---\n";
    RUN_TEST(unknown_move_effect_produces_unknown_not_stock);
    RUN_TEST(unknown_effect_roundtrip_stays_unknown);

    std::cout << "\n--- Physical/Special split tests ---\n";
    RUN_TEST(gen2_type_based_ps_split_correct);
    RUN_TEST(ps_category_survives_package_roundtrip);
    RUN_TEST(ps_split_real_rom_fire_vs_normal_distinct);

    std::cout << "\n=== Results ===\n";
    std::cout << "Passed: " << g_passed << "\n";
    std::cout << "Failed: " << g_failed << "\n";
    return g_failed > 0 ? 1 : 0;
}
