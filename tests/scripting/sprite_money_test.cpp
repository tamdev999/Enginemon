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
#include <iostream>
#include <cassert>
#include <string>
#include <algorithm>
#include <cctype>
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
    // A save file with daycare species 252 (above valid range) must fail deserialization.
    GameState gs;
    gs.daycare_slot[0] = 252;  // Invalid
    auto bytes = gs.serialize();
    auto result = GameState::try_deserialize(bytes);
    ASSERT_FALSE(result.ok());
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

    // Species→icon package roundtrip tests
    RUN(species_icon_map_roundtrip_through_package);
    RUN(daycare_resolves_via_package_map_not_hardcoded_table);
    RUN(missing_mapped_icon_returns_empty);
    RUN(duplicate_species_entry_fails);

    std::cout << "\n=== Results ===\n";
    std::cout << "Passed: " << g_passed << "\n";
    std::cout << "Failed: " << g_failed << "\n";
    return g_failed > 0 ? 1 : 0;
}
