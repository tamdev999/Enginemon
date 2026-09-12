// tests/battle_differential/live_diff_haze_test.cpp
//
// TRUE LIVE DIFFERENTIAL TEST -- Haze (move 114, EFFECT_RESET_STATS)
//
// Definition of "live differential":
//   Same fixture input
//   -> execute ACTUAL SuiCune source code live
//   -> execute Enginemon live
//   -> normalize observable state
//   -> assert suicune_output == enginemon_output
//
// NO handwritten Crystal expected values are used.
// NO existing golden expected values are consulted.
// Expected value source: SuiCune execution result only.
//
// SuiCune entry point: suicune_haze_run() in suicune_haze_driver.c
//   Executes BattleCommand_ResetStats using verbatim SuiCune source compiled here.
//
// Enginemon entry point: Battle::execute_turn() with Haze (move id 114).
//
// Normalization:
//   SuiCune encoding: neutral=7 (BASE_STAT_LEVEL)
//   Enginemon encoding: neutral=0
//   Normalized delta = suicune_raw - 7 = enginemon_raw
//   RNG calls: Haze consumes 0 on both sides.

#include "suicune_haze_driver.h"

#include "engine/battle/battle.hpp"
#include "engine/battle/battle_rules.hpp"
#include "engine/core/types.hpp"
#include "engine/party/party.hpp"
#include "engine/party/pokemon.hpp"
#include "engine/core/registry.hpp"
#include "crystal/extract/battle_rules_extractor.hpp"
#include "crystal/rom/loader.hpp"
#include "crystal/rom/profile.hpp"
#include "crystal/compile/move_semanticizer.hpp"
#include "crystal/battle/crystal_effects.hpp"
#include "crystal/output/native_package.hpp"
#include "engine/package/package_reader.hpp"

#include <iostream>
#include <filesystem>
#include <optional>
#include <vector>

// ============================================================================
// Test framework (mirrors battle_rom_test.cpp conventions)
// ============================================================================
static int  g_passed = 0;
static int  g_failed = 0;
static bool g_current_failed = false;

#define ASSERT_EQ(a, b) \
    do { if (!((a) == (b))) { \
        std::cerr << "  FAIL: " << #a << " == " << #b \
                  << "  got " << (int64_t)(a) << " expected " << (int64_t)(b) \
                  << "  at line " << __LINE__ << "\n"; \
        g_current_failed = true; \
    } } while(0)

#define ASSERT_TRUE(expr) \
    do { if (!(expr)) { \
        std::cerr << "  FAIL: " << #expr << " at line " << __LINE__ << "\n"; \
        g_current_failed = true; \
    } } while(0)

#define RUN_TEST(name) \
    do { \
        g_current_failed = false; \
        std::cout << "  " << #name << " ... "; std::cout.flush(); \
        name(); \
        if (!g_current_failed) { ++g_passed; std::cout << "PASS\n"; } \
        else                   { ++g_failed; std::cout << "FAIL\n"; } \
    } while(0)

// ============================================================================
// ROM globals
// ============================================================================
static const crystal::RomData*           g_rom     = nullptr;
static const crystal::ExtractionProfile* g_profile = nullptr;

// ============================================================================
// Local helpers (same pattern as battle_rom_test.cpp)
// ============================================================================

static std::vector<crystal::PackageWriter::MoveDataEntry>
extract_moves_local(const crystal::RomData& rom,
                    const crystal::ExtractionProfile& profile)
{
    std::vector<crystal::PackageWriter::MoveDataEntry> entries;
    const auto& o   = profile.offsets;
    const auto& fmt = profile.format.move;
    const auto& c   = profile.counts;
    if (o.moves == 0) return entries;
    entries.reserve(c.num_moves);
    for (uint16_t i = 1; i <= c.num_moves; ++i) {
        uint32_t addr = o.moves + static_cast<uint32_t>(i-1) * fmt.move_data_size;
        auto rec = rom.read_bytes(addr, fmt.move_data_size);
        crystal::PackageWriter::MoveDataEntry e;
        e.id = i;
        e.type_id        = rec[fmt.type_offset];
        e.power          = rec[fmt.power_offset];
        e.accuracy       = rec[fmt.accuracy_offset];
        e.pp             = rec[fmt.pp_offset];
        e.effect_chance  = rec[fmt.effect_chance_offset];
        uint8_t raw      = rec[fmt.effect_offset];
        e.effect_id      = crystal::to_semantic_effect(raw);
        e.raw_crystal_effect = raw;
        if (fmt.category_offset != 0xFF && fmt.category_offset < fmt.move_data_size) {
            uint8_t cv = rec[fmt.category_offset];
            e.category = (cv <= 2u) ? cv : (uint8_t)enginemon::MoveCategory::Physical;
        } else {
            e.category = (uint8_t)crystal::crystal_move_category_from_type(e.type_id, e.power);
        }
        entries.push_back(e);
    }
    return entries;
}

static enginemon::BattleRules load_rules() {
    auto res = crystal::extract_battle_rules(*g_rom, *g_profile);
    if (!res.success) return {};
    return res.rules;
}

static std::optional<enginemon::Registry<enginemon::MoveId, enginemon::MoveData>>
load_moves() {
    auto entries = extract_moves_local(*g_rom, *g_profile);
    if (!crystal::semanticize_move_entries(*g_rom, *g_profile, entries)) return std::nullopt;
    crystal::PackageWriter w;
    w.set_source_rom(std::string(40, 'x'), "live_diff_haze");
    w.add_move_data(entries);
    auto pkg = std::filesystem::temp_directory_path() / "live_diff_haze_moves.emon";
    if (!w.write(pkg)) return std::nullopt;
    auto rdr = enginemon::PackageReader::open(pkg);
    if (!rdr) { std::filesystem::remove(pkg); return std::nullopt; }
    auto reg = rdr->load_move_registry();
    std::filesystem::remove(pkg);
    return reg;
}

static enginemon::BattlePokemon make_mon(
    enginemon::MoveId move_id,
    int16_t base_atk, int16_t base_def, int16_t base_spd,
    int16_t base_spatk, int16_t base_spdef, int16_t spd_override,
    int8_t s_atk, int8_t s_def, int8_t s_spd,
    int8_t s_satk, int8_t s_sdef,
    int8_t s_acc, int8_t s_eva)
{
    enginemon::BattlePokemon bp{};
    bp.species = 1; bp.type1 = 0; bp.type2 = 0; bp.level = 50;
    bp.stats.hp = bp.stats.max_hp = 300;
    bp.base_stats.hp = bp.base_stats.max_hp = 300;
    bp.stats.attack  = bp.base_stats.attack  = base_atk;
    bp.stats.defense = bp.base_stats.defense = base_def;
    bp.stats.speed   = bp.base_stats.speed   = (spd_override > 0 ? spd_override : base_spd);
    bp.stats.special_attack  = bp.base_stats.special_attack  = base_spatk;
    bp.stats.special_defense = bp.base_stats.special_defense = base_spdef;
    bp.happiness = 200;
    bp.dv_atk = bp.dv_def = bp.dv_spd = bp.dv_spc = 15;
    bp.moves[0].move = move_id;
    bp.moves[0].pp = bp.moves[0].max_pp = 10;
    bp.stages.attack         = s_atk;
    bp.stages.defense        = s_def;
    bp.stages.speed          = s_spd;
    bp.stages.special_attack = s_satk;
    bp.stages.special_defense= s_sdef;
    bp.stages.accuracy       = s_acc;
    bp.stages.evasion        = s_eva;
    return bp;
}

// ============================================================================
// Live differential test
// ============================================================================

static void live_diff_haze_stat_stages_and_rng() {
    std::cout << "\n=== live_diff_haze_stat_stages_and_rng ===\n";

    // Fixture: base stats and pre-Haze stage deltas.
    // Deliberately non-neutral to prove reset on both sides.
    constexpr uint16_t P_ATK=80, P_DEF=60, P_SPD=95, P_SATK=110, P_SDEF=70;
    constexpr uint16_t E_ATK=125, E_DEF=75, E_SPD=55, E_SATK=45, E_SDEF=100;
    constexpr int8_t PS_ATK=+2, PS_DEF=-3, PS_SPD=+1, PS_SATK=-1, PS_SDEF=+3, PS_ACC=+4, PS_EVA=-2;
    constexpr int8_t ES_ATK=-4, ES_DEF=+6, ES_SPD=-2, ES_SATK=+3, ES_SDEF=-1, ES_ACC=-3, ES_EVA=+5;

    // ----------------------------------------------------------------
    // SUICUNE: run live
    // ----------------------------------------------------------------
    SuicuneHazeInput sc_in{};
    // SuiCune: neutral=7, encode delta as 7+delta
    sc_in.player_stages[0] = uint8_t(7+PS_ATK);
    sc_in.player_stages[1] = uint8_t(7+PS_DEF);
    sc_in.player_stages[2] = uint8_t(7+PS_SPD);
    sc_in.player_stages[3] = uint8_t(7+PS_SATK);
    sc_in.player_stages[4] = uint8_t(7+PS_SDEF);
    sc_in.player_stages[5] = uint8_t(7+PS_ACC);
    sc_in.player_stages[6] = uint8_t(7+PS_EVA);
    sc_in.player_stages[7] = 7; // ABILITY slot
    sc_in.enemy_stages[0] = uint8_t(7+ES_ATK);
    sc_in.enemy_stages[1] = uint8_t(7+ES_DEF);
    sc_in.enemy_stages[2] = uint8_t(7+ES_SPD);
    sc_in.enemy_stages[3] = uint8_t(7+ES_SATK);
    sc_in.enemy_stages[4] = uint8_t(7+ES_SDEF);
    sc_in.enemy_stages[5] = uint8_t(7+ES_ACC);
    sc_in.enemy_stages[6] = uint8_t(7+ES_EVA);
    sc_in.enemy_stages[7] = 7;
    sc_in.player_base_attack=P_ATK; sc_in.player_base_defense=P_DEF; sc_in.player_base_speed=P_SPD;
    sc_in.player_base_spatk=P_SATK; sc_in.player_base_spdef=P_SDEF;
    sc_in.enemy_base_attack=E_ATK;  sc_in.enemy_base_defense=E_DEF;  sc_in.enemy_base_speed=E_SPD;
    sc_in.enemy_base_spatk=E_SATK;  sc_in.enemy_base_spdef=E_SDEF;

    SuicuneHazeOutput sc_out{};
    suicune_haze_run(&sc_in, &sc_out);

    std::cout << "  SUICUNE player stages (raw,neutral=7):"
              << " atk=" << (int)sc_out.player_stages[0]
              << " def=" << (int)sc_out.player_stages[1]
              << " spd=" << (int)sc_out.player_stages[2]
              << " satk=" << (int)sc_out.player_stages[3]
              << " sdef=" << (int)sc_out.player_stages[4]
              << " acc=" << (int)sc_out.player_stages[5]
              << " eva=" << (int)sc_out.player_stages[6]
              << " rng=" << sc_out.rng_calls << "\n";
    std::cout << "  SUICUNE enemy stages (raw,neutral=7):"
              << " atk=" << (int)sc_out.enemy_stages[0]
              << " def=" << (int)sc_out.enemy_stages[1]
              << " spd=" << (int)sc_out.enemy_stages[2]
              << " satk=" << (int)sc_out.enemy_stages[3]
              << " sdef=" << (int)sc_out.enemy_stages[4]
              << " acc=" << (int)sc_out.enemy_stages[5]
              << " eva=" << (int)sc_out.enemy_stages[6] << "\n";

    // ----------------------------------------------------------------
    // ENGINEMON: run live
    // ----------------------------------------------------------------
    auto rules    = load_rules();
    auto moves_opt = load_moves();
    ASSERT_TRUE(moves_opt.has_value());
    if (!moves_opt) return;
    auto& moves = *moves_opt;

    constexpr enginemon::MoveId HAZE_ID = 114;
    const enginemon::MoveData* haze_md = moves.get(HAZE_ID);
    ASSERT_TRUE(haze_md != nullptr && haze_md->effect_desc.is_supported);
    if (!haze_md) return;

    enginemon::Registries reg{};
    reg.moves = moves;
    enginemon::Party party;
    { enginemon::Pokemon pm{}; pm.species=1; pm.level=50; pm.current_hp=pm.max_hp=300; pm.friendship=200; party.add(pm); }
    enginemon::Battle bat(enginemon::BattleType::Wild, party, reg, rules);

    // Player uses Haze (speed=200, goes first), opponent idle (speed=1)
    bat.player_pokemon()   = make_mon(HAZE_ID,  P_ATK,P_DEF,P_SPD,P_SATK,P_SDEF,200, PS_ATK,PS_DEF,PS_SPD,PS_SATK,PS_SDEF,PS_ACC,PS_EVA);
    bat.opponent_pokemon() = make_mon(enginemon::MOVE_NONE, E_ATK,E_DEF,E_SPD,E_SATK,E_SDEF,1, ES_ATK,ES_DEF,ES_SPD,ES_SATK,ES_SDEF,ES_ACC,ES_EVA);

    uint32_t em_rng = 0;
    bat.set_rng_callback([&em_rng]()->uint32_t{ ++em_rng; return 0xFF; });
    bat.set_player_action(enginemon::ActionFight{0,0});
    bat.set_opponent_action(enginemon::ActionFight{0,0});
    bat.execute_turn();

    const auto& ps = bat.player_pokemon().stages;
    const auto& os = bat.opponent_pokemon().stages;
    std::cout << "  ENGINEMON player stages (delta,neutral=0):"
              << " atk=" << (int)ps.attack << " def=" << (int)ps.defense
              << " spd=" << (int)ps.speed  << " satk=" << (int)ps.special_attack
              << " sdef=" << (int)ps.special_defense
              << " acc=" << (int)ps.accuracy << " eva=" << (int)ps.evasion
              << " rng=" << em_rng << "\n";
    std::cout << "  ENGINEMON enemy stages (delta,neutral=0):"
              << " atk=" << (int)os.attack << " def=" << (int)os.defense
              << " spd=" << (int)os.speed  << " satk=" << (int)os.special_attack
              << " sdef=" << (int)os.special_defense
              << " acc=" << (int)os.accuracy << " eva=" << (int)os.evasion << "\n";

    // ----------------------------------------------------------------
    // NORMALIZATION: convert both to delta-from-neutral
    //   SuiCune: delta = raw - 7
    //   Enginemon: delta = raw  (already -6..+6)
    // ----------------------------------------------------------------
    auto sc_delta = [](uint8_t r) -> int8_t { return int8_t(int(r) - 7); };

    // ----------------------------------------------------------------
    // ASSERTIONS: normalized_suicune == normalized_enginemon
    // No handwritten expected values. SuiCune execution IS the expectation.
    // ----------------------------------------------------------------
    ASSERT_EQ(sc_delta(sc_out.player_stages[0]), ps.attack);
    ASSERT_EQ(sc_delta(sc_out.player_stages[1]), ps.defense);
    ASSERT_EQ(sc_delta(sc_out.player_stages[2]), ps.speed);
    ASSERT_EQ(sc_delta(sc_out.player_stages[3]), ps.special_attack);
    ASSERT_EQ(sc_delta(sc_out.player_stages[4]), ps.special_defense);
    ASSERT_EQ(sc_delta(sc_out.player_stages[5]), ps.accuracy);
    ASSERT_EQ(sc_delta(sc_out.player_stages[6]), ps.evasion);

    ASSERT_EQ(sc_delta(sc_out.enemy_stages[0]), os.attack);
    ASSERT_EQ(sc_delta(sc_out.enemy_stages[1]), os.defense);
    ASSERT_EQ(sc_delta(sc_out.enemy_stages[2]), os.speed);
    ASSERT_EQ(sc_delta(sc_out.enemy_stages[3]), os.special_attack);
    ASSERT_EQ(sc_delta(sc_out.enemy_stages[4]), os.special_defense);
    ASSERT_EQ(sc_delta(sc_out.enemy_stages[5]), os.accuracy);
    ASSERT_EQ(sc_delta(sc_out.enemy_stages[6]), os.evasion);

    ASSERT_EQ((uint32_t)sc_out.rng_calls, em_rng);

    const bool match =
        sc_delta(sc_out.player_stages[0])==ps.attack  &&
        sc_delta(sc_out.player_stages[1])==ps.defense  &&
        sc_delta(sc_out.player_stages[2])==ps.speed    &&
        sc_delta(sc_out.player_stages[3])==ps.special_attack  &&
        sc_delta(sc_out.player_stages[4])==ps.special_defense &&
        sc_delta(sc_out.player_stages[5])==ps.accuracy &&
        sc_delta(sc_out.player_stages[6])==ps.evasion  &&
        sc_delta(sc_out.enemy_stages[0])==os.attack    &&
        sc_delta(sc_out.enemy_stages[1])==os.defense   &&
        sc_delta(sc_out.enemy_stages[2])==os.speed     &&
        sc_delta(sc_out.enemy_stages[3])==os.special_attack   &&
        sc_delta(sc_out.enemy_stages[4])==os.special_defense  &&
        sc_delta(sc_out.enemy_stages[5])==os.accuracy  &&
        sc_delta(sc_out.enemy_stages[6])==os.evasion;

    std::cout << "  normalized_suicune == normalized_enginemon: " << (match ? "YES" : "NO") << "\n";
    std::cout << "  RNG: suicune=" << sc_out.rng_calls << " enginemon=" << em_rng
              << (sc_out.rng_calls==em_rng ? " MATCH" : " MISMATCH") << "\n";
}

// ============================================================================
// main
// ============================================================================
int main(int argc, char* argv[]) {
    if (argc < 2) { std::cerr << "Usage: live_diff_haze_test <rom_path>\n"; return 1; }

    auto rom = crystal::RomData::load(std::filesystem::path(argv[1]));
    if (!rom) { std::cerr << "Failed to load ROM: " << argv[1] << "\n"; return 1; }

    const crystal::ExtractionProfile* profile =
        crystal::ProfileRegistry::instance().get_profile_by_hash(rom->hash());
    if (!profile) { std::cerr << "No profile for ROM hash " << rom->hash() << "\n"; return 1; }

    g_rom = rom.get(); g_profile = profile;

    std::cout << "=== Live Differential Test: Haze (move 114) ===\n";
    std::cout << "  SuiCune live: BattleCommand_ResetStats (verbatim source)\n";
    std::cout << "  Enginemon live: Battle::execute_turn(Haze)\n\n";

    RUN_TEST(live_diff_haze_stat_stages_and_rng);

    std::cout << "\n=== Results ===\n";
    std::cout << "Passed: " << g_passed << "\n";
    std::cout << "Failed: " << g_failed << "\n";
    return g_failed > 0 ? 1 : 0;
}
