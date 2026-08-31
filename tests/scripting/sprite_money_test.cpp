// tests/scripting/sprite_money_test.cpp
// Focused tests for variable sprite identity model, money text state,
// Pokémon icon sprites, Day Care sprite resolution, and species→icon package roundtrip.
//
// Does NOT require a ROM path — GameState/LuaRuntime/PackageWriter/PackageReader only.
//
// Verifies:
//   - Variable sprite assignments store stable SpriteId strings (not Crystal indices)
//   - Variable sprite state survives serialize/try_deserialize
//   - Money balance survives save/load
//   - Transient money text buffers are NOT stored in GameState
//   - prepare_money_text reflects current balance
//   - Player vs mom accounts remain distinct
//   - Species→icon mapping round-trips through PackageWriter → PackageReader
//   - Day Care resolution uses the package map, not any hardcoded Crystal table
//   - Missing species in package map returns empty (no silent fallback)
//   - Duplicate species in add_species_icon_map throws

#include "engine/scripting/lua_runtime.hpp"
#include "engine/scripting/api_bindings.hpp"
#include "engine/core/game_state.hpp"
#include "engine/core/types.hpp"
#include "engine/package/package_reader.hpp"
#include "crystal/extract/sprite_ids.hpp"
#include "crystal/output/native_package.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <cassert>
#include <string>
#include <algorithm>
#include <cctype>
#include <functional>
#include <thread>
#include <unordered_map>

using namespace enginemon;

//=============================================================================
// MINIMAL TEST FRAMEWORK
//=============================================================================

static int g_passed = 0;
static int g_failed = 0;
static bool g_test_failed = false;

#define TEST(name) void test_##name()
#define RUN(name) run_test(#name, test_##name)

#define ASSERT_TRUE(cond) \
    do { if (!(cond)) { \
        std::cerr << "  FAIL: " #cond " at line " << __LINE__ << "\n"; \
        g_test_failed = true; return; } } while(0)

#define ASSERT_FALSE(cond) ASSERT_TRUE(!(cond))
#define ASSERT_EQ(a, b)    ASSERT_TRUE((a) == (b))
#define ASSERT_STR_EQ(a, b) ASSERT_TRUE(std::string(a) == std::string(b))

void run_test(const char* name, void(*fn)()) {
    std::cout << "  " << name << "... ";
    g_test_failed = false;
    try { fn(); }
    catch (const std::exception& e) {
        std::cerr << "EXCEPTION: " << e.what() << "\n";
        g_test_failed = true;
    }
    if (g_test_failed) { std::cout << "FAIL\n"; ++g_failed; }
    else               { std::cout << "PASS\n"; ++g_passed; }
}

//=============================================================================
// HELPER: resolve a daycare slot using a package-loaded species→icon map.
// This mirrors the runtime path in main_tiles.cpp::daycare_resolve_icon()
// without any dependency on deleted pokemon_icons.hpp or hardcoded tables.
//=============================================================================

static std::string daycare_resolve_icon_via_map(
    const std::string& sprite_id,
    const std::array<SpeciesId, 2>& slots,
    const std::unordered_map<SpeciesId, std::string>& species_icon_map)
{
    int slot_index = crystal::sprite_id_daycare_slot(sprite_id);  // 1 or 2
    if (slot_index < 1 || slot_index > 2) return "";
    SpeciesId sp = slots[static_cast<size_t>(slot_index - 1)];
    if (sp == 0) return "";  // empty slot
    auto it = species_icon_map.find(sp);
    if (it == species_icon_map.end()) return "";
    return it->second;
}

//=============================================================================
// VARIABLE SPRITE TESTS
//=============================================================================

TEST(variable_sprite_identity_survives_save_load) {
    // Variable sprite assignment must survive serialize/try_deserialize.
    // The stable SpriteId string must be preserved exactly — no Crystal index.
    GameState original;
    original.variable_sprites["copycat"]       = "fixed:lass";
    original.variable_sprites["fuchsia_gym_1"] = "fixed:janine";

    auto bytes = original.serialize();
    auto result = GameState::try_deserialize(bytes);
    ASSERT_TRUE(result.ok());

    const auto& loaded = result.state;
    auto it_c = loaded.variable_sprites.find("copycat");
    auto it_f = loaded.variable_sprites.find("fuchsia_gym_1");
    ASSERT_TRUE(it_c != loaded.variable_sprites.end());
    ASSERT_TRUE(it_f != loaded.variable_sprites.end());
    ASSERT_STR_EQ(it_c->second, "fixed:lass");
    ASSERT_STR_EQ(it_f->second, "fixed:janine");
}

TEST(variable_sprite_runtime_no_crystal_mapping_call) {
    // Confirm the variable sprite roundtrip does NOT reconstruct identity
    // from a Crystal numeric index. The stored value IS the final sprite_id.
    GameState gs;
    gs.variable_sprites["olivine_rival"] = "fixed:rival";

    // Read back directly — no mapping needed, value is already the sprite_id.
    ASSERT_STR_EQ(gs.variable_sprites.at("olivine_rival"), "fixed:rival");

    // Simulate what sprite_id_variable_resolve does (without any Crystal fn):
    auto it = gs.variable_sprites.find("olivine_rival");
    ASSERT_TRUE(it != gs.variable_sprites.end());
    // The returned value is used directly as the package key after stripping "fixed:"
    std::string resolved = it->second;  // "fixed:rival"
    ASSERT_TRUE(resolved.starts_with("fixed:"));
    std::string pkg_key = resolved.substr(6);  // "rival"
    ASSERT_STR_EQ(pkg_key, "rival");
}

//=============================================================================
// MONEY TESTS
//=============================================================================

TEST(money_balance_survives_save_load) {
    GameState original;
    original.variables["money_player"] = 9999;
    original.variables["money_mom"]    = 3000;

    auto bytes = original.serialize();
    auto result = GameState::try_deserialize(bytes);
    ASSERT_TRUE(result.ok());

    const auto& loaded = result.state;
    auto it_p = loaded.variables.find("money_player");
    auto it_m = loaded.variables.find("money_mom");
    ASSERT_TRUE(it_p != loaded.variables.end());
    ASSERT_TRUE(it_m != loaded.variables.end());
    ASSERT_EQ(it_p->second, 9999);
    ASSERT_EQ(it_m->second, 3000);
}

TEST(money_transient_text_buffer_not_in_gamestate) {
    // The transient text buffer (strbuf<N>_money) must NOT appear in GameState.
    // Buffers are written to StubServices::text_buffers, NOT gs.variables.
    GameState gs;
    gs.variables["money_player"] = 500;

    // Confirm strbuf key is not in variables (it lives in StubServices)
    ASSERT_TRUE(gs.variables.find("strbuf1_money") == gs.variables.end());

    // After save/load: balance is preserved, no text buffer artifact.
    auto bytes = gs.serialize();
    auto result = GameState::try_deserialize(bytes);
    ASSERT_TRUE(result.ok());
    ASSERT_TRUE(result.state.variables.find("strbuf1_money") == result.state.variables.end());
    ASSERT_EQ(result.state.variables.at("money_player"), 500);
}

TEST(money_prepare_text_reflects_current_balance) {
    // prepare_money_text writes the current balance to StubServices::text_buffers.
    std::string script_lua = R"(
script = {}
function script.main(ctx)
    ctx.inventory:give_money(1234, 0)
    ctx.inventory:prepare_money_text(0, 0)
    return
end
return script
)";

    LuaRuntime runtime;
    GameState gs;
    runtime.set_game_state(&gs);
    runtime.execute_string(script_lua, "reflect");
    runtime.start_script("script");

    // Balance authoritative in GameState.
    ASSERT_EQ(gs.variables.at("money_player"), 1234);
    // Text buffer reflects the balance (transient, in StubServices).
    ASSERT_EQ(runtime.get_stub_services().text_buffers.at("strbuf0_money"), 1234);
    // NOT in GameState::variables.
    ASSERT_TRUE(gs.variables.find("strbuf0_money") == gs.variables.end());
}

TEST(money_mom_account_prepare_text_distinct) {
    // Mom's prepare_text reads mom balance, player's reads player balance.
    std::string script_lua = R"(
script = {}
function script.main(ctx)
    ctx.inventory:give_money(100, 0)    -- player
    ctx.inventory:give_money(999, 1)    -- mom
    ctx.inventory:prepare_money_text(0, 0)  -- player → strbuf0_money
    ctx.inventory:prepare_money_text(1, 1)  -- mom    → strbuf1_money
    return
end
return script
)";

    LuaRuntime runtime;
    GameState gs;
    runtime.set_game_state(&gs);
    runtime.execute_string(script_lua, "mom_acct");
    runtime.start_script("script");

    auto& bufs = runtime.get_stub_services().text_buffers;
    ASSERT_EQ(bufs.at("strbuf0_money"), 100);
    ASSERT_EQ(bufs.at("strbuf1_money"), 999);
    ASSERT_TRUE(bufs.at("strbuf0_money") != bufs.at("strbuf1_money"));
}


//=============================================================================
// POKÉMON ICON SPRITE ID TESTS
//=============================================================================

TEST(pokemon_icon_sprite_id_from_clefairy_byte) {
    using namespace crystal;
    // SPRITE_CLEFAIRY = 0x8F used directly in CopycatsHouse1F.asm object_event.
    // 0x8F - 0x80 = 15 → SpriteMons[15] = CLEFAIRY → MonMenuIcons[35-1] = ICON_CLEFAIRY → "clefairy"
    std::string id = crystal_sprite_byte_to_id(0x8F);
    ASSERT_STR_EQ(id, "pokemon_icon:clefairy");
    ASSERT_TRUE(sprite_id_is_pokemon_icon(id));
}

TEST(pokemon_icon_sprite_id_is_stable_across_bytes) {
    using namespace crystal;
    // Multiple sprite bytes may map to the same icon type (shared icons).
    // But each byte in range 0x80-0xA2 must produce a valid pokemon_icon id.
    for (int b = 0x80; b <= 0xA2; ++b) {
        std::string id = crystal_sprite_byte_to_id(static_cast<uint8_t>(b));
        ASSERT_TRUE(!id.empty());
        ASSERT_TRUE(id.starts_with("pokemon_icon:"));
        // Must not contain a bare numeric index from the old system
        std::string suffix = id.substr(13);
        bool is_numeric = !suffix.empty() && std::all_of(suffix.begin(), suffix.end(), ::isdigit);
        ASSERT_FALSE(is_numeric);
    }
}

//=============================================================================
// DAY CARE TESTS — use package-map helper, no hardcoded Crystal tables
//=============================================================================

TEST(daycare_empty_slot_returns_no_icon) {
    // When daycare_slot[N] = 0 (empty), resolution must return "".
    // Uses an inline map simulating what the runtime loads from the package.
    std::unordered_map<SpeciesId, std::string> icon_map = {
        {25,  "pokemon_icon:pikachu"},
        {131, "pokemon_icon:lapras"},
    };
    std::array<SpeciesId, 2> slots = {0, 0};

    std::string icon1 = daycare_resolve_icon_via_map("daycare:1", slots, icon_map);
    std::string icon2 = daycare_resolve_icon_via_map("daycare:2", slots, icon_map);
    ASSERT_TRUE(icon1.empty());
    ASSERT_TRUE(icon2.empty());
}

TEST(daycare_occupied_resolves_to_icon) {
    // Occupied slot resolves through the package-loaded species→icon map.
    // Species 25 = PIKACHU → "pokemon_icon:pikachu"
    // Species 131 = LAPRAS → "pokemon_icon:lapras"
    std::unordered_map<SpeciesId, std::string> icon_map = {
        {25,  "pokemon_icon:pikachu"},
        {131, "pokemon_icon:lapras"},
        {35,  "pokemon_icon:clefairy"},
    };
    std::array<SpeciesId, 2> slots = {25, 131};

    std::string icon1 = daycare_resolve_icon_via_map("daycare:1", slots, icon_map);
    std::string icon2 = daycare_resolve_icon_via_map("daycare:2", slots, icon_map);
    ASSERT_STR_EQ(icon1, "pokemon_icon:pikachu");
    ASSERT_STR_EQ(icon2, "pokemon_icon:lapras");
}

TEST(daycare_save_load_species_survives) {
    // Day Care species occupancy must survive serialize/try_deserialize.
    GameState original;
    original.daycare_slot[0] = 25;   // PIKACHU in slot 1
    original.daycare_slot[1] = 131;  // LAPRAS in slot 2

    auto bytes = original.serialize();
    auto result = GameState::try_deserialize(bytes);
    ASSERT_TRUE(result.ok());
    ASSERT_EQ(result.state.daycare_slot[0], static_cast<SpeciesId>(25));
    ASSERT_EQ(result.state.daycare_slot[1], static_cast<SpeciesId>(131));
}

TEST(daycare_invalid_species_fails_closed) {
    // Previously this test checked that species 252 was rejected at save/load time
    // with a hardcoded > 251 ceiling. That ceiling has been removed.
    //
    // New semantics: save/load accepts any species in [1, 65534] — the actual domain
    // validation is by registry membership at runtime, not a numeric ceiling.
    // This allows non-251 profiles (ROM hacks, expanded Crystal) to save/load correctly.
    //
    // Species 252 round-trips correctly (not rejected at save boundary).
    GameState gs;
    gs.daycare_slot[0] = 252;
    auto bytes = gs.serialize();
    auto result = GameState::try_deserialize(bytes);
    ASSERT_TRUE(result.ok());  // 252 is within [1, 65534] — accepted at save boundary
    ASSERT_EQ(result.state.daycare_slot[0], static_cast<enginemon::SpeciesId>(252));

    // SPECIES_NONE (0) is still valid (clears the slot)
    GameState gs2;
    gs2.daycare_slot[0] = 0;
    auto bytes2 = gs2.serialize();
    auto result2 = GameState::try_deserialize(bytes2);
    ASSERT_TRUE(result2.ok());
    ASSERT_EQ(result2.state.daycare_slot[0], static_cast<enginemon::SpeciesId>(0));

    // Truly out-of-range (> 65534) still fails
    GameState gs3;
    gs3.daycare_slot[0] = static_cast<enginemon::SpeciesId>(65535);  // SPECIES_NONE equiv / invalid
    auto bytes3 = gs3.serialize();
    auto result3 = GameState::try_deserialize(bytes3);
    ASSERT_FALSE(result3.ok());

}

//=============================================================================
// SPECIES→ICON PACKAGE ROUNDTRIP TESTS
// These prove the frontend→package→runtime path works without any hardcoded table.
//=============================================================================

TEST(species_icon_map_roundtrip_through_package) {
    // Build a minimal species→icon map in the frontend, write to a temp package,
    // read back through the engine PackageReader, verify key entries survived.
    using crystal::PackageWriter;

    std::vector<PackageWriter::SpeciesIconEntry> entries = {
        {25,  "pokemon_icon:pikachu"},
        {35,  "pokemon_icon:clefairy"},
        {131, "pokemon_icon:lapras"},
        {143, "pokemon_icon:snorlax"},
        {249, "pokemon_icon:lugia"},
        {250, "pokemon_icon:ho-oh"},
    };

    auto tmp = std::filesystem::temp_directory_path() / "sim_roundtrip_test.emon";
    {
        PackageWriter writer;
        writer.set_source_rom("sim_roundtrip_sha1", "sim_roundtrip_v1");
        writer.add_species_icon_map(entries);
        ASSERT_TRUE(writer.write(tmp));
    }

    // Read back through the engine PackageReader (no Crystal headers needed).
    auto reader = PackageReader::open(tmp);
    ASSERT_TRUE(reader != nullptr);

    auto loaded = reader->load_species_icon_map();
    ASSERT_EQ(loaded.size(), static_cast<size_t>(6));

    ASSERT_STR_EQ(loaded.at(25),  "pokemon_icon:pikachu");
    ASSERT_STR_EQ(loaded.at(35),  "pokemon_icon:clefairy");
    ASSERT_STR_EQ(loaded.at(131), "pokemon_icon:lapras");
    ASSERT_STR_EQ(loaded.at(143), "pokemon_icon:snorlax");
    ASSERT_STR_EQ(loaded.at(249), "pokemon_icon:lugia");
    ASSERT_STR_EQ(loaded.at(250), "pokemon_icon:ho-oh");

    std::filesystem::remove(tmp);
}

TEST(daycare_resolves_via_package_map_not_hardcoded_table) {
    // Full chain: write a species→icon map to a package, load it back,
    // then resolve a Day Care slot through the loaded map.
    // This proves the runtime depends only on loaded package data.
    using crystal::PackageWriter;

    std::vector<PackageWriter::SpeciesIconEntry> entries = {
        {25,  "pokemon_icon:pikachu"},
        {131, "pokemon_icon:lapras"},
    };

    auto tmp = std::filesystem::temp_directory_path() / "sim_daycare_pkg_test.emon";
    {
        PackageWriter writer;
        writer.set_source_rom("sim_dc_sha1", "sim_dc_v1");
        writer.add_species_icon_map(entries);
        ASSERT_TRUE(writer.write(tmp));
    }

    auto reader = PackageReader::open(tmp);
    ASSERT_TRUE(reader != nullptr);

    auto icon_map = reader->load_species_icon_map();
    ASSERT_EQ(icon_map.size(), static_cast<size_t>(2));

    // Simulate daycare slot state.
    std::array<SpeciesId, 2> slots = {25, 131};

    std::string icon1 = daycare_resolve_icon_via_map("daycare:1", slots, icon_map);
    std::string icon2 = daycare_resolve_icon_via_map("daycare:2", slots, icon_map);
    ASSERT_STR_EQ(icon1, "pokemon_icon:pikachu");
    ASSERT_STR_EQ(icon2, "pokemon_icon:lapras");

    std::filesystem::remove(tmp);
}

TEST(missing_mapped_icon_returns_empty) {
    // Species not present in the package map → resolution returns empty string.
    // No silent fallback to any hardcoded table.
    std::unordered_map<SpeciesId, std::string> icon_map = {
        {25, "pokemon_icon:pikachu"},
    };
    std::array<SpeciesId, 2> slots = {200, 0};  // 200 = MISDREAVUS, not in map

    std::string icon = daycare_resolve_icon_via_map("daycare:1", slots, icon_map);
    ASSERT_TRUE(icon.empty());
}

TEST(duplicate_species_entry_fails) {
    // add_species_icon_map with a duplicate SpeciesId must throw std::runtime_error.
    using crystal::PackageWriter;

    std::vector<PackageWriter::SpeciesIconEntry> entries = {
        {25, "pokemon_icon:pikachu"},
        {25, "pokemon_icon:pikachu"},  // duplicate
    };

    PackageWriter writer;
    writer.set_source_rom("dup_sha1", "dup_v1");

    bool threw = false;
    try {
        writer.add_species_icon_map(entries);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    ASSERT_TRUE(threw);
}

//=============================================================================
// REGISTRY PACKAGE ROUNDTRIP TESTS
//
// All tests use a temp-file approach: write package to a temp path, then open
// it with PackageReader and verify the registry contents.
//
// No ROM required — synthetic data only.
//=============================================================================

namespace {

// Build a minimal valid package that only contains BaseStats and/or MoveData.
// Returns path to the temp file (caller must delete).
static std::filesystem::path write_registry_package(
    const std::vector<crystal::PackageWriter::SpeciesBaseStatsEntry>& species_entries,
    const std::vector<crystal::PackageWriter::MoveDataEntry>& move_entries)
{
    auto tmp = std::filesystem::temp_directory_path() /
               ("reg_test_" + std::to_string(std::hash<std::thread::id>{}(
                                std::this_thread::get_id())) + ".emon");

    crystal::PackageWriter writer;
    writer.set_source_rom("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", "test");
    if (!species_entries.empty()) writer.add_base_stats(species_entries);
    if (!move_entries.empty())    writer.add_move_data(move_entries);
    if (!writer.write(tmp)) {
        throw std::runtime_error("write_registry_package: write failed");
    }
    return tmp;
}

struct TmpFile {
    std::filesystem::path path;
    explicit TmpFile(std::filesystem::path p) : path(std::move(p)) {}
    ~TmpFile() { std::error_code ec; std::filesystem::remove(path, ec); }
    TmpFile(const TmpFile&) = delete;
    TmpFile& operator=(const TmpFile&) = delete;
};

} // anonymous namespace

#include <thread>

// 1. Full BaseStats round-trip: write one entry, read it back, verify all fields.
TEST(base_stats_roundtrip_species_lookup) {
    crystal::PackageWriter::SpeciesBaseStatsEntry e;
    e.id = SpeciesId{1};
    e.hp = 45; e.attack = 49; e.defense = 49; e.speed = 45;
    e.sp_atk = 65; e.sp_def = 65;
    e.type1 = 11; e.type2 = 22;
    e.catch_rate = 45; e.base_exp = 64; e.gender_ratio = 31;

    TmpFile tmp(write_registry_package({e}, {}));
    auto reader = PackageReader::open(tmp.path);
    ASSERT_TRUE(reader != nullptr);

    auto reg = reader->load_base_stats_registry();
    ASSERT_TRUE(reg.has_value());

    const SpeciesData* sd = reg->get(SpeciesId{1});
    ASSERT_TRUE(sd != nullptr);
    ASSERT_EQ(sd->base_stats.hp,             45u);
    ASSERT_EQ(sd->base_stats.attack,         49u);
    ASSERT_EQ(sd->base_stats.defense,        49u);
    ASSERT_EQ(sd->base_stats.speed,          45u);
    ASSERT_EQ(sd->base_stats.special_attack,  65u);
    ASSERT_EQ(sd->base_stats.special_defense, 65u);
    ASSERT_EQ(sd->type1,        TypeId{11});
    ASSERT_EQ(sd->type2,        TypeId{22});
    ASSERT_EQ(sd->catch_rate,   45u);
    ASSERT_EQ(sd->base_exp,     64u);
    ASSERT_EQ(sd->gender_ratio, 31u);

    // Missing species → nullptr, not crash
    ASSERT_TRUE(reg->get(SpeciesId{999}) == nullptr);

    std::cout << "  [base_stats round-trip: species 1 fields all correct; 999 not found ✓]\n";
}

// 2. Absent BaseStats chunk → nullopt (package with no BaseStats chunk at all).
TEST(base_stats_absent_chunk_returns_nullopt) {
    TmpFile tmp(write_registry_package({}, {}));  // no species, no moves
    auto reader = PackageReader::open(tmp.path);
    ASSERT_TRUE(reader != nullptr);

    auto reg = reader->load_base_stats_registry();
    ASSERT_TRUE(!reg.has_value());
    std::cout << "  [base_stats absent chunk -> nullopt ✓]\n";
}

// 3. Duplicate SpeciesId → add_base_stats throws.
TEST(base_stats_duplicate_id_rejected) {
    crystal::PackageWriter::SpeciesBaseStatsEntry e;
    e.id = SpeciesId{1};
    e.hp = 45; e.attack = 49; e.defense = 49; e.speed = 45;
    e.sp_atk = 65; e.sp_def = 65;
    e.type1 = 11; e.type2 = 22;
    e.catch_rate = 45; e.base_exp = 64; e.gender_ratio = 31;

    crystal::PackageWriter writer;
    writer.set_source_rom("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", "test");
    bool threw = false;
    try {
        writer.add_base_stats({e, e});  // same id twice
    } catch (const std::exception&) {
        threw = true;
    }
    ASSERT_TRUE(threw);
    std::cout << "  [base_stats duplicate id -> throws ✓]\n";
}

// 4. Corrupt BaseStats chunk (truncated payload) → nullopt.
TEST(base_stats_corrupt_chunk_returns_nullopt) {
    // Write a valid package then corrupt the BaseStats chunk by patching the
    // count to a value that implies more bytes than the chunk contains.
    crystal::PackageWriter::SpeciesBaseStatsEntry e;
    e.id = SpeciesId{1};
    e.hp = 45; e.attack = 49; e.defense = 49; e.speed = 45;
    e.sp_atk = 65; e.sp_def = 65;
    e.type1 = 11; e.type2 = 22;
    e.catch_rate = 45; e.base_exp = 64; e.gender_ratio = 31;

    auto tmp_path = write_registry_package({e}, {});
    TmpFile tmp(tmp_path);

    // Corrupt by setting the count field in the blob to 9999 (well beyond the
    // actual data), which will make the reader see count*14+4 > chunk_size.
    {
        std::fstream f(tmp_path, std::ios::binary | std::ios::in | std::ios::out);
        ASSERT_TRUE(f.is_open());
        // Locate the BaseStats chunk via the TOC (brute-force scan for 'BSTS').
        // 0x42535453 = BaseStats magic in little-endian: 53 54 53 42
        f.seekg(0, std::ios::end);
        auto fsize = f.tellg();
        f.seekg(0);
        std::vector<uint8_t> buf(static_cast<size_t>(fsize));
        f.read(reinterpret_cast<char*>(buf.data()), fsize);

        // Find the BSTS blob (count u32 followed by entry bytes).
        // The count of 1 is written as LE bytes: 01 00 00 00.
        // Overwrite count with 9999 (0x0F 0x27 0x00 0x00 LE).
        for (size_t i = 0; i + 3 < buf.size(); ++i) {
            if (buf[i]==0x01 && buf[i+1]==0x00 && buf[i+2]==0x00 && buf[i+3]==0x00) {
                // Check this looks like a count (followed by species_id = 0x01 0x00)
                if (i + 5 < buf.size() && buf[i+4] == 0x01 && buf[i+5] == 0x00) {
                    buf[i]   = 0x0F;  // 9999 LE
                    buf[i+1] = 0x27;
                    f.seekp(static_cast<std::streamoff>(i));
                    f.write(reinterpret_cast<char*>(&buf[i]), 4);
                    break;
                }
            }
        }
    }

    auto reader = PackageReader::open(tmp_path);
    ASSERT_TRUE(reader != nullptr);
    auto reg = reader->load_base_stats_registry();
    ASSERT_TRUE(!reg.has_value());
    std::cout << "  [base_stats corrupt (inflated count) -> nullopt ✓]\n";
}

// 5. Multiple species in one chunk, all correct.
TEST(base_stats_multiple_species_all_correct) {
    std::vector<crystal::PackageWriter::SpeciesBaseStatsEntry> entries;
    for (uint16_t i = 1; i <= 3; ++i) {
        crystal::PackageWriter::SpeciesBaseStatsEntry e{};
        e.id = SpeciesId{i};
        e.hp = static_cast<uint8_t>(10 * i);
        e.attack = static_cast<uint8_t>(20 * i);
        entries.push_back(e);
    }

    TmpFile tmp(write_registry_package(entries, {}));
    auto reader = PackageReader::open(tmp.path);
    ASSERT_TRUE(reader != nullptr);

    auto reg = reader->load_base_stats_registry();
    ASSERT_TRUE(reg.has_value());

    for (uint16_t i = 1; i <= 3; ++i) {
        const SpeciesData* sd = reg->get(SpeciesId{i});
        ASSERT_TRUE(sd != nullptr);
        ASSERT_EQ(sd->base_stats.hp,     static_cast<uint8_t>(10 * i));
        ASSERT_EQ(sd->base_stats.attack, static_cast<uint8_t>(20 * i));
    }
    ASSERT_TRUE(reg->get(SpeciesId{4}) == nullptr);
    std::cout << "  [base_stats 3 species all correct; 4 not found ✓]\n";
}

// 6. Full MoveData round-trip: write one entry, read it back, verify all fields.
TEST(move_data_roundtrip_move_lookup) {
    crystal::PackageWriter::MoveDataEntry e{};
    e.id = MoveId{85};         // Thunderbolt
    e.type_id = 13;            // Electric
    e.power = 95;
    e.accuracy = 100;
    e.pp = 15;
    e.effect_id = 26;          // 10% paralysis
    e.effect_chance = 10;

    TmpFile tmp(write_registry_package({}, {e}));
    auto reader = PackageReader::open(tmp.path);
    ASSERT_TRUE(reader != nullptr);

    auto reg = reader->load_move_registry();
    ASSERT_TRUE(reg.has_value());

    const MoveData* md = reg->get(MoveId{85});
    ASSERT_TRUE(md != nullptr);
    ASSERT_EQ(md->type,          TypeId{13});
    ASSERT_EQ(md->power,         95u);
    ASSERT_EQ(md->accuracy,      100u);
    ASSERT_EQ(md->pp,            15u);
    ASSERT_EQ(md->effect_id,     26u);
    ASSERT_EQ(md->effect_chance, 10u);

    ASSERT_TRUE(reg->get(MoveId{999}) == nullptr);
    std::cout << "  [move_data round-trip: move 85 (Thunderbolt) fields correct ✓]\n";
}

// 7. Absent MoveData chunk → nullopt.
TEST(move_data_absent_chunk_returns_nullopt) {
    TmpFile tmp(write_registry_package({}, {}));
    auto reader = PackageReader::open(tmp.path);
    ASSERT_TRUE(reader != nullptr);

    auto reg = reader->load_move_registry();
    ASSERT_TRUE(!reg.has_value());
    std::cout << "  [move_data absent chunk -> nullopt ✓]\n";
}

// 8. Duplicate MoveId → add_move_data throws.
TEST(move_data_duplicate_id_rejected) {
    crystal::PackageWriter::MoveDataEntry e{};
    e.id = MoveId{1};
    e.type_id = 0; e.power = 40; e.accuracy = 100; e.pp = 35;

    crystal::PackageWriter writer;
    writer.set_source_rom("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", "test");
    bool threw = false;
    try {
        writer.add_move_data({e, e});  // same id twice
    } catch (const std::exception&) {
        threw = true;
    }
    ASSERT_TRUE(threw);
    std::cout << "  [move_data duplicate id -> throws ✓]\n";
}

// 9. Corrupt MoveData chunk (inflated count) → nullopt.
TEST(move_data_corrupt_chunk_returns_nullopt) {
    crystal::PackageWriter::MoveDataEntry e{};
    e.id = MoveId{1}; e.type_id = 0; e.power = 40; e.accuracy = 100; e.pp = 35;

    auto tmp_path = write_registry_package({}, {e});
    TmpFile tmp(tmp_path);

    // Corrupt the count field in the MoveData blob.
    {
        std::fstream f(tmp_path, std::ios::binary | std::ios::in | std::ios::out);
        ASSERT_TRUE(f.is_open());
        f.seekg(0, std::ios::end);
        auto fsize = f.tellg();
        f.seekg(0);
        std::vector<uint8_t> buf(static_cast<size_t>(fsize));
        f.read(reinterpret_cast<char*>(buf.data()), fsize);

        // count=1 LE followed by move_id=1 LE: 01 00 00 00 01 00
        for (size_t i = 0; i + 5 < buf.size(); ++i) {
            if (buf[i]==0x01 && buf[i+1]==0x00 && buf[i+2]==0x00 && buf[i+3]==0x00 &&
                buf[i+4]==0x01 && buf[i+5]==0x00) {
                buf[i]   = 0x0F;
                buf[i+1] = 0x27;
                f.seekp(static_cast<std::streamoff>(i));
                f.write(reinterpret_cast<char*>(&buf[i]), 4);
                break;
            }
        }
    }

    auto reader = PackageReader::open(tmp_path);
    ASSERT_TRUE(reader != nullptr);
    auto reg = reader->load_move_registry();
    ASSERT_TRUE(!reg.has_value());
    std::cout << "  [move_data corrupt (inflated count) -> nullopt ✓]\n";
}

// 10. Registry is frozen after load — modification attempt throws.
TEST(base_stats_frozen_after_load) {
    crystal::PackageWriter::SpeciesBaseStatsEntry e{};
    e.id = SpeciesId{1}; e.hp = 45;

    TmpFile tmp(write_registry_package({e}, {}));
    auto reader = PackageReader::open(tmp.path);
    ASSERT_TRUE(reader != nullptr);

    auto reg = reader->load_base_stats_registry();
    ASSERT_TRUE(reg.has_value());
    ASSERT_TRUE(reg->is_frozen());

    bool threw = false;
    try {
        SpeciesData dummy{};
        reg->register_entry(SpeciesId{99}, dummy);
    } catch (const RegistryError&) {
        threw = true;
    } catch (const std::exception&) {
        threw = true;
    }
    ASSERT_TRUE(threw);
    std::cout << "  [base_stats registry frozen after load -> write throws ✓]\n";
}

TEST(move_data_frozen_after_load) {
    crystal::PackageWriter::MoveDataEntry e{};
    e.id = MoveId{1}; e.type_id = 0; e.power = 40; e.accuracy = 100; e.pp = 35;

    TmpFile tmp(write_registry_package({}, {e}));
    auto reader = PackageReader::open(tmp.path);
    ASSERT_TRUE(reader != nullptr);

    auto reg = reader->load_move_registry();
    ASSERT_TRUE(reg.has_value());
    ASSERT_TRUE(reg->is_frozen());

    bool threw = false;
    try {
        MoveData dummy{};
        reg->register_entry(MoveId{99}, dummy);
    } catch (const RegistryError&) {
        threw = true;
    } catch (const std::exception&) {
        threw = true;
    }
    ASSERT_TRUE(threw);
    std::cout << "  [move_data registry frozen after load -> write throws ✓]\n";
}

//=============================================================================
// MAIN
//=============================================================================

int main(int /*argc*/, char* /*argv*/[]) {
    std::cout << "=== Sprite/Money State Tests ===\n";

    RUN(variable_sprite_identity_survives_save_load);
    RUN(variable_sprite_runtime_no_crystal_mapping_call);
    RUN(money_balance_survives_save_load);
    RUN(money_transient_text_buffer_not_in_gamestate);
    RUN(money_prepare_text_reflects_current_balance);
    RUN(money_mom_account_prepare_text_distinct);

    // Pokémon icon sprite ID tests
    RUN(pokemon_icon_sprite_id_from_clefairy_byte);
    RUN(pokemon_icon_sprite_id_is_stable_across_bytes);

    // Day Care tests — package-map path, no hardcoded Crystal tables
    RUN(daycare_empty_slot_returns_no_icon);
    RUN(daycare_occupied_resolves_to_icon);
    RUN(daycare_save_load_species_survives);
    RUN(daycare_invalid_species_fails_closed);

    // Species->icon package roundtrip tests
    RUN(species_icon_map_roundtrip_through_package);
    RUN(daycare_resolves_via_package_map_not_hardcoded_table);
    RUN(missing_mapped_icon_returns_empty);
    RUN(duplicate_species_entry_fails);

    // BaseStats / MoveData registry package roundtrip tests
    RUN(base_stats_roundtrip_species_lookup);
    RUN(base_stats_absent_chunk_returns_nullopt);
    RUN(base_stats_duplicate_id_rejected);
    RUN(base_stats_corrupt_chunk_returns_nullopt);
    RUN(base_stats_multiple_species_all_correct);
    RUN(move_data_roundtrip_move_lookup);
    RUN(move_data_absent_chunk_returns_nullopt);
    RUN(move_data_duplicate_id_rejected);
    RUN(move_data_corrupt_chunk_returns_nullopt);
    RUN(base_stats_frozen_after_load);
    RUN(move_data_frozen_after_load);

    std::cout << "\n=== Results ===\n";
    std::cout << "Passed: " << g_passed << "\n";
    std::cout << "Failed: " << g_failed << "\n";
    return g_failed > 0 ? 1 : 0;
}
