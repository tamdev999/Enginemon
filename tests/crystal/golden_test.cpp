// tests/crystal/golden_test.cpp
// Golden tests for Crystal ROM extraction
// 
// Verifies extracted data matches known vanilla Crystal values.
// These tests ensure extraction correctness without requiring visual inspection.
//
// Run with: golden_test <rom_path>

#include "crystal/rom/loader.hpp"
#include "crystal/rom/profile.hpp"
#include "crystal/rom/symbol_map.hpp"
#include "crystal/extract/map_extractor.hpp"
#include "crystal/output/native_package.hpp"
#include "crystal/script/decoder.hpp"
#include "crystal/script/lua_emitter.hpp"

#include <iostream>
#include <iomanip>
#include <cstring>
#include <filesystem>

using namespace crystal;

//=============================================================================
// ENUM STREAMING HELPERS (for ASSERT_EQ error messages)
//=============================================================================

inline std::ostream& operator<<(std::ostream& os, RomVersion v) {
    switch (v) {
        case RomVersion::Unknown: return os << "Unknown";
        case RomVersion::Crystal_USA_v1_0: return os << "Crystal_USA_v1_0";
        case RomVersion::Crystal_USA_v1_1: return os << "Crystal_USA_v1_1";
        case RomVersion::Crystal_EUR: return os << "Crystal_EUR";
        case RomVersion::Crystal_JPN: return os << "Crystal_JPN";
        case RomVersion::Crystal_AUS: return os << "Crystal_AUS";
        default: return os << "RomVersion(" << static_cast<int>(v) << ")";
    }
}

//=============================================================================
// TEST FRAMEWORK (minimal, no external deps)
//=============================================================================

static int g_tests_passed = 0;
static int g_tests_failed = 0;
static bool g_current_test_failed = false;

#define TEST(name) void test_##name()
#define RUN_TEST(name) run_test(#name, test_##name)

#define ASSERT_TRUE(cond) \
    if (!(cond)) { \
        std::cerr << "  FAIL: " << #cond << " at line " << __LINE__ << "\n"; \
        g_current_test_failed = true; \
        return; \
    }

#define ASSERT_EQ(a, b) \
    if ((a) != (b)) { \
        std::cerr << "  FAIL: " << #a << " == " << #b << "\n"; \
        std::cerr << "    Expected: " << static_cast<int64_t>(b) << "\n"; \
        std::cerr << "    Actual: " << static_cast<int64_t>(a) << "\n"; \
        std::cerr << "    at line " << __LINE__ << "\n"; \
        g_current_test_failed = true; \
        return; \
    }

#define ASSERT_STR_EQ(a, b) \
    if (std::string(a) != std::string(b)) { \
        std::cerr << "  FAIL: " << #a << " == " << #b << "\n"; \
        std::cerr << "    Expected: \"" << (b) << "\"\n"; \
        std::cerr << "    Actual: \"" << (a) << "\"\n"; \
        std::cerr << "    at line " << __LINE__ << "\n"; \
        g_current_test_failed = true; \
        return; \
    }

void run_test(const char* name, void (*test)()) {
    std::cout << "Running " << name << "... ";
    g_current_test_failed = false;
    try {
        test();
        if (g_current_test_failed) {
            std::cout << "FAIL\n";
            g_tests_failed++;
        } else {
            std::cout << "PASS\n";
            g_tests_passed++;
        }
    } catch (const std::exception& e) {
        std::cout << "EXCEPTION: " << e.what() << "\n";
        g_tests_failed++;
    }
}

//=============================================================================
// GLOBALS (set by main)
//=============================================================================

static const RomData* g_rom = nullptr;
static const ExtractionProfile* g_profile = nullptr;

//=============================================================================
// PROFILE TESTS
//=============================================================================

TEST(profile_sha1_match) {
    // Crystal v1.1 expected hash
    ASSERT_STR_EQ(g_rom->hash(), "f2f52230b536214ef7c9924f483392993e226cfb");
}

TEST(profile_identified) {
    auto& registry = ProfileRegistry::instance();
    auto version = registry.identify(g_rom->hash());
    ASSERT_TRUE(version.has_value());
    ASSERT_EQ(*version, RomVersion::Crystal_USA_v1_1);
}

TEST(profile_offsets_valid) {
    // Check key offsets are in valid ROM range
    const auto& o = g_profile->offsets;
    size_t rom_size = g_rom->size();
    
    ASSERT_TRUE(o.map_group_pointers < rom_size);
    ASSERT_TRUE(o.base_data < rom_size);
    ASSERT_TRUE(o.moves < rom_size);
    ASSERT_TRUE(o.item_attributes < rom_size);
    ASSERT_TRUE(o.tilesets < rom_size);
    ASSERT_TRUE(o.pokemon_names < rom_size);
}

//=============================================================================
// POKEMON DATA TESTS
//=============================================================================

TEST(bulbasaur_base_stats) {
    // Bulbasaur (Pokemon #1) base stats are well-known
    const auto& o = g_profile->offsets;
    const auto& fmt = g_profile->format.pokemon;
    
    auto data = g_rom->read_bytes(o.base_data, fmt.base_data_size);
    
    ASSERT_EQ(data[fmt.hp_offset], 45);    // HP
    ASSERT_EQ(data[fmt.atk_offset], 49);   // Attack
    ASSERT_EQ(data[fmt.def_offset], 49);   // Defense
    ASSERT_EQ(data[fmt.spd_offset], 45);   // Speed
    ASSERT_EQ(data[fmt.satk_offset], 65);  // Sp. Attack
    ASSERT_EQ(data[fmt.sdef_offset], 65);  // Sp. Defense
    ASSERT_EQ(data[fmt.type1_offset], 22); // Grass
    ASSERT_EQ(data[fmt.type2_offset], 3);  // Poison
}

TEST(pikachu_base_stats) {
    // Pikachu is Pokemon #25
    const auto& o = g_profile->offsets;
    const auto& fmt = g_profile->format.pokemon;
    
    uint32_t pikachu_offset = o.base_data + (24 * fmt.base_data_size);  // 0-indexed
    auto data = g_rom->read_bytes(pikachu_offset, fmt.base_data_size);
    
    ASSERT_EQ(data[fmt.hp_offset], 35);    // HP
    ASSERT_EQ(data[fmt.atk_offset], 55);   // Attack
    ASSERT_EQ(data[fmt.def_offset], 30);   // Defense
    ASSERT_EQ(data[fmt.spd_offset], 90);   // Speed
    ASSERT_EQ(data[fmt.satk_offset], 50);  // Sp. Attack
    ASSERT_EQ(data[fmt.sdef_offset], 40);  // Sp. Defense
    ASSERT_EQ(data[fmt.type1_offset], 23); // Electric
}

//=============================================================================
// MOVE DATA TESTS
//=============================================================================

TEST(pound_move_data) {
    // Pound is Move #1
    const auto& o = g_profile->offsets;
    const auto& fmt = g_profile->format.move;
    
    auto data = g_rom->read_bytes(o.moves, fmt.move_data_size);
    
    ASSERT_EQ(data[fmt.power_offset], 40);     // Power
    ASSERT_EQ(data[fmt.type_offset], 0);       // Normal type
    // Accuracy: Crystal stores as value * 255 / 100, so 100% = 255
    ASSERT_EQ(data[fmt.accuracy_offset], 255); // 100% accuracy
    ASSERT_EQ(data[fmt.pp_offset], 35);        // PP
}

TEST(thunderbolt_move_data) {
    // Thunderbolt is Move #85
    const auto& o = g_profile->offsets;
    const auto& fmt = g_profile->format.move;
    
    uint32_t tbolt_offset = o.moves + (84 * fmt.move_data_size);  // 0-indexed
    auto data = g_rom->read_bytes(tbolt_offset, fmt.move_data_size);
    
    ASSERT_EQ(data[fmt.power_offset], 95);     // Power
    ASSERT_EQ(data[fmt.type_offset], 23);      // Electric
    // Accuracy: Crystal stores as value * 255 / 100, so 100% = 255
    ASSERT_EQ(data[fmt.accuracy_offset], 255); // 100% accuracy
    ASSERT_EQ(data[fmt.pp_offset], 15);        // PP
}

//=============================================================================
// MAP EXTRACTION TESTS
//=============================================================================

TEST(new_bark_town_dimensions) {
    MapExtractor extractor(*g_rom, *g_profile);
    // NewBarkTown is Group 24, Index 4 (verified from pokecrystal data/maps/maps.asm)
    auto result = extractor.extract_map(24, 4);
    
    ASSERT_TRUE(result.success);
    ASSERT_EQ(result.map.width, 10);    // NEW_BARK_TOWN_WIDTH = 10
    ASSERT_EQ(result.map.height, 9);    // NEW_BARK_TOWN_HEIGHT = 9
    ASSERT_EQ(result.map.blocks.size(), 90);  // 10 * 9 = 90 blocks
}

TEST(new_bark_town_semantic_id) {
    MapExtractor extractor(*g_rom, *g_profile);
    auto result = extractor.extract_map(24, 4);
    
    ASSERT_TRUE(result.success);
    ASSERT_STR_EQ(result.map.map_id, "new_bark_town");
    
    // Verify no Crystal group/index leaked into semantic fields
    ASSERT_TRUE(result.map.map_id.find("24") == std::string::npos);
    ASSERT_TRUE(result.map.tileset_id.find("0x") == std::string::npos);
}

TEST(new_bark_town_by_semantic_id) {
    MapExtractor extractor(*g_rom, *g_profile);
    auto result = extractor.extract_map("new_bark_town");
    
    ASSERT_TRUE(result.success);
    ASSERT_EQ(result.map.width, 10);
    ASSERT_EQ(result.map.height, 9);
}

TEST(new_bark_town_secondary_metadata) {
    // Verify secondary metadata extracted from 9-byte MapGroup entry
    // From maps.asm: map NewBarkTown, TILESET_JOHTO, TOWN, LANDMARK_NEW_BARK_TOWN, 
    //                MUSIC_NEW_BARK_TOWN, FALSE, PALETTE_AUTO, FISHGROUP_OCEAN
    MapExtractor extractor(*g_rom, *g_profile);
    auto result = extractor.extract_map(24, 4);
    
    ASSERT_TRUE(result.success);
    
    // Tileset should be TILESET_JOHTO (index 0, semantic id "johto_outdoor")
    ASSERT_STR_EQ(result.map.tileset_id, "johto_outdoor");
    
    // Environment should be TOWN (value 1)
    ASSERT_EQ(result.map.environment_type, 1);  // TOWN
    
    // Should be outdoor since it's a TOWN environment
    ASSERT_TRUE(result.map.is_outdoor);
    
    // Phone service should NOT be disabled (FALSE in maps.asm)
    ASSERT_TRUE(!result.map.phone_service_disabled);
    
    // Lighting should be PALETTE_AUTO (value 0)
    ASSERT_EQ(result.map.lighting, 0);  // PALETTE_AUTO
    
    // Fish group should be FISHGROUP_OCEAN
    ASSERT_STR_EQ(result.map.fish_group_id, "ocean");
}

TEST(new_bark_town_connections) {
    // NewBarkTown has connections:
    // - west to Route29 (offset 0)
    // - east to Route27 (offset 0)
    // From attributes.asm: map_attributes NewBarkTown, NEW_BARK_TOWN, $05
    //                      connection west, Route29, ROUTE_29, 0
    //                      connection east, Route27, ROUTE_27, 0
    MapExtractor extractor(*g_rom, *g_profile);
    auto result = extractor.extract_map(24, 4);
    
    ASSERT_TRUE(result.success);
    ASSERT_EQ(result.map.connections.size(), 2);
    
    // Find west connection
    bool has_west = false;
    bool has_east = false;
    for (const auto& conn : result.map.connections) {
        if (conn.direction == crystal::Direction::West) {
            has_west = true;
            ASSERT_STR_EQ(conn.target_map_id, "route_29");
        }
        if (conn.direction == crystal::Direction::East) {
            has_east = true;
            ASSERT_STR_EQ(conn.target_map_id, "route_27");
        }
    }
    ASSERT_TRUE(has_west);
    ASSERT_TRUE(has_east);
}

TEST(new_bark_town_events) {
    // NewBarkTown event counts from maps/NewBarkTown.asm:
    // - 4 warps (Elms Lab, Players House 1F, Players Neighbors House, Elms House)
    // - 2 coord_events (teacher stops you scenes)
    // - 4 bg_events (town sign + 3 house/lab signs)
    // - 3 objects (teacher, fisher, rival)
    MapExtractor extractor(*g_rom, *g_profile);
    auto result = extractor.extract_map(24, 4);
    
    ASSERT_TRUE(result.success);
    ASSERT_EQ(result.map.warps.size(), 4);
    ASSERT_EQ(result.map.coord_events.size(), 2);
    ASSERT_EQ(result.map.bg_events.size(), 4);
    ASSERT_EQ(result.map.objects.size(), 3);
    
    // Verify warp destinations
    bool has_elms_lab_warp = false;
    bool has_players_house_warp = false;
    for (const auto& warp : result.map.warps) {
        if (warp.target_map_id == "elms_lab") has_elms_lab_warp = true;
        if (warp.target_map_id == "players_house_1f") has_players_house_warp = true;
    }
    ASSERT_TRUE(has_elms_lab_warp);
    ASSERT_TRUE(has_players_house_warp);
}

TEST(elms_lab_dimensions) {
    // Elm's Lab is Group 24, Index 5
    // From map_constants.asm: map_const ELMS_LAB, 5, 6  (5 wide, 6 tall)
    MapExtractor extractor(*g_rom, *g_profile);
    auto result = extractor.extract_map(24, 5);
    
    ASSERT_TRUE(result.success);
    ASSERT_EQ(result.map.width, 5);
    ASSERT_EQ(result.map.height, 6);
    ASSERT_STR_EQ(result.map.map_id, "elms_lab");
}

TEST(elms_lab_is_indoor) {
    // ElmsLab is INDOOR, not outdoor
    // From maps.asm: map ElmsLab, TILESET_LAB, INDOOR, ...
    MapExtractor extractor(*g_rom, *g_profile);
    auto result = extractor.extract_map(24, 5);
    
    ASSERT_TRUE(result.success);
    ASSERT_EQ(result.map.environment_type, 3);  // INDOOR
    ASSERT_TRUE(!result.map.is_outdoor);
}

TEST(route26_dimensions) {
    // Route26 is Group 24, Index 1
    MapExtractor extractor(*g_rom, *g_profile);
    auto result = extractor.extract_map(24, 1);
    
    ASSERT_TRUE(result.success);
    ASSERT_STR_EQ(result.map.map_id, "route_26");
    // Route26 is 10x81 blocks (tall vertical route)
    ASSERT_EQ(result.map.width, 10);
    ASSERT_TRUE(result.map.height > 50);  // It's a tall route
}

TEST(map_block_data_valid) {
    MapExtractor extractor(*g_rom, *g_profile);
    auto result = extractor.extract_map("new_bark_town");
    
    ASSERT_TRUE(result.success);
    
    // Block values should be reasonable metatile indices
    // Crystal tilesets typically have 128 metatiles max
    for (uint8_t block : result.map.blocks) {
        ASSERT_TRUE(block < 0x80);  // Most vanilla blocks are <128
    }
}

TEST(invalid_map_fails_gracefully) {
    MapExtractor extractor(*g_rom, *g_profile);
    
    // Group 99 doesn't exist
    auto result = extractor.extract_map(99, 1);
    ASSERT_TRUE(!result.success);
    ASSERT_TRUE(!result.error.empty());
}

//=============================================================================
// BOUNDS CHECKING TESTS
//=============================================================================

TEST(bounds_check_stats) {
    MapExtractor extractor(*g_rom, *g_profile);
    
    // Extract a valid map
    extractor.extract_map("new_bark_town");
    
    // Should have 0 bounds check failures for valid extraction
    ASSERT_EQ(extractor.stats().bounds_check_failures, 0);
}

TEST(bounds_check_invalid) {
    MapExtractor extractor(*g_rom, *g_profile);
    
    // Try to extract invalid maps
    extractor.extract_map(99, 1);
    extractor.extract_map(0, 0);
    
    // Should have recorded bounds check failures
    // (exact count depends on how many checks fail)
    ASSERT_TRUE(extractor.stats().maps_failed > 0);
}

//=============================================================================
// PACKAGE ROUND-TRIP TESTS
//=============================================================================

static std::filesystem::path g_test_package_path;

TEST(package_write) {
    // Extract NewBarkTown and write to package
    MapExtractor extractor(*g_rom, *g_profile);
    auto result = extractor.extract_map("new_bark_town");
    ASSERT_TRUE(result.success);
    
    PackageWriter writer;
    writer.set_source_rom(g_rom->hash(), "Crystal USA v1.1");
    writer.add_map(result.map);
    
    g_test_package_path = std::filesystem::temp_directory_path() / "enginemon_test.pkg";
    ASSERT_TRUE(writer.write(g_test_package_path));
    ASSERT_TRUE(std::filesystem::exists(g_test_package_path));
    ASSERT_TRUE(std::filesystem::file_size(g_test_package_path) > 0);
}

TEST(package_read_header) {
    auto reader = PackageReader::open(g_test_package_path);
    ASSERT_TRUE(reader != nullptr);
    
    ASSERT_EQ(reader->header().magic, PackageHeader::MAGIC);
    ASSERT_EQ(reader->header().version, PackageHeader::VERSION);
    ASSERT_STR_EQ(reader->source_sha1(), g_rom->hash());
}

TEST(package_validate) {
    auto reader = PackageReader::open(g_test_package_path);
    ASSERT_TRUE(reader != nullptr);
    ASSERT_TRUE(reader->validate());
}

TEST(package_list_maps) {
    auto reader = PackageReader::open(g_test_package_path);
    ASSERT_TRUE(reader != nullptr);
    
    auto maps = reader->list_maps();
    ASSERT_EQ(maps.size(), 1);
    ASSERT_STR_EQ(maps[0], "new_bark_town");
}

TEST(package_load_map) {
    auto reader = PackageReader::open(g_test_package_path);
    ASSERT_TRUE(reader != nullptr);
    
    auto map_opt = reader->load_map("new_bark_town");
    ASSERT_TRUE(map_opt.has_value());
    
    auto& map = *map_opt;
    ASSERT_STR_EQ(map.map_id, "new_bark_town");
    ASSERT_EQ(map.width, 10);
    ASSERT_EQ(map.height, 9);
    ASSERT_STR_EQ(map.tileset_id, "johto_outdoor");
}

TEST(package_roundtrip_multiple_maps) {
    // Extract multiple maps, write, read back, verify all
    MapExtractor extractor(*g_rom, *g_profile);
    
    auto nbt = extractor.extract_map("new_bark_town");
    auto elms = extractor.extract_map("elms_lab");
    auto r26 = extractor.extract_map("route_26");
    
    ASSERT_TRUE(nbt.success);
    ASSERT_TRUE(elms.success);
    ASSERT_TRUE(r26.success);
    
    PackageWriter writer;
    writer.set_source_rom(g_rom->hash(), "Crystal USA v1.1");
    writer.add_map(nbt.map);
    writer.add_map(elms.map);
    writer.add_map(r26.map);
    
    auto multi_path = std::filesystem::temp_directory_path() / "enginemon_multi.pkg";
    ASSERT_TRUE(writer.write(multi_path));
    
    // Read back
    auto reader = PackageReader::open(multi_path);
    ASSERT_TRUE(reader != nullptr);
    
    auto maps = reader->list_maps();
    ASSERT_EQ(maps.size(), 3);
    
    // Verify each map
    auto loaded_nbt = reader->load_map("new_bark_town");
    auto loaded_elms = reader->load_map("elms_lab");
    auto loaded_r26 = reader->load_map("route_26");
    
    ASSERT_TRUE(loaded_nbt.has_value());
    ASSERT_TRUE(loaded_elms.has_value());
    ASSERT_TRUE(loaded_r26.has_value());
    
    // Verify dimensions match original extraction
    ASSERT_EQ(loaded_nbt->width, nbt.map.width);
    ASSERT_EQ(loaded_nbt->height, nbt.map.height);
    ASSERT_EQ(loaded_elms->width, elms.map.width);
    ASSERT_EQ(loaded_elms->height, elms.map.height);
    ASSERT_EQ(loaded_r26->width, r26.map.width);
    
    // Cleanup
    std::filesystem::remove(multi_path);
}

TEST(package_cleanup) {
    // Cleanup test package
    if (std::filesystem::exists(g_test_package_path)) {
        std::filesystem::remove(g_test_package_path);
    }
    ASSERT_TRUE(!std::filesystem::exists(g_test_package_path));
}

//=============================================================================
// SCRIPT DECODER TESTS
//=============================================================================

// Address calculations for Crystal USA v1.1:
// NewBarkTownSign: bank 0x6A (106), addr 0x40C8
// Flat = 106 * 0x4000 + (0x40C8 - 0x4000) = 0x1A80C8
// NewBarkTownSignText: bank 0x6A, addr 0x42E8
// Flat = 0x1A82E8

TEST(script_decoder_jumptext_opcode) {
    // NewBarkTownSign script is: jumptext NewBarkTownSignText
    // Which is opcode 0x53 followed by 2-byte pointer to text
    SymbolMap symbols;  // Empty - we'll use addresses directly
    ScriptDecoder decoder(*g_rom, symbols);
    
    // Calculate flat address for NewBarkTownSign
    // Bank 0x6A, addr 0x40C8
    uint32_t script_addr = g_rom->bank_to_flat(0x6A, 0x40C8);
    
    // Read the opcode byte directly to verify
    uint8_t opcode = g_rom->read_byte(script_addr);
    ASSERT_EQ(opcode, 0x53);  // jumptext opcode
}

TEST(script_decoder_decode_jumptext) {
    SymbolMap symbols;
    ScriptDecoder decoder(*g_rom, symbols);
    
    uint32_t script_addr = g_rom->bank_to_flat(0x6A, 0x40C8);
    auto script = decoder.decode_script(script_addr, "NewBarkTownSign");
    
    ASSERT_STR_EQ(script.name, "NewBarkTownSign");
    ASSERT_EQ(script.instructions.size(), 1);
    
    // The instruction should be Op_JumpText
    bool is_jumptext = std::holds_alternative<Op_JumpText>(script.instructions[0].op);
    ASSERT_TRUE(is_jumptext);
}

TEST(script_decoder_text_content) {
    SymbolMap symbols;
    ScriptDecoder decoder(*g_rom, symbols);
    
    uint32_t script_addr = g_rom->bank_to_flat(0x6A, 0x40C8);
    auto script = decoder.decode_script(script_addr, "NewBarkTownSign");
    
    ASSERT_EQ(script.instructions.size(), 1);
    
    auto& op = std::get<Op_JumpText>(script.instructions[0].op);
    
    // NewBarkTownSignText from maps/NewBarkTown.asm:
    // text "NEW BARK TOWN"
    // para "The Town Where the"
    // line "Winds of a New"
    // cont "Beginning Blow"
    // done
    
    // Verify text starts with "NEW BARK TOWN"
    // Use debug_string() to get flattened text for simple verification
    ASSERT_TRUE(op.sequence.debug_string().find("NEW BARK TOWN") != std::string::npos);
}

TEST(script_decoder_text_decoding) {
    // Test direct text decoding
    SymbolMap symbols;
    ScriptDecoder decoder(*g_rom, symbols);
    
    // NewBarkTownSignText at bank 0x6A, addr 0x42E8
    uint32_t text_addr = g_rom->bank_to_flat(0x6A, 0x42E8);
    std::string text = decoder.decode_text(text_addr);
    
    ASSERT_TRUE(text.find("NEW BARK TOWN") != std::string::npos);
    ASSERT_TRUE(text.find("Town Where") != std::string::npos);
    ASSERT_TRUE(text.find("Beginning") != std::string::npos);
}

TEST(script_decoder_stats) {
    SymbolMap symbols;
    ScriptDecoder decoder(*g_rom, symbols);
    
    uint32_t script_addr = g_rom->bank_to_flat(0x6A, 0x40C8);
    decoder.decode_script(script_addr, "NewBarkTownSign");
    
    auto stats = decoder.get_stats();
    ASSERT_EQ(stats.scripts_decoded, 1);
    ASSERT_EQ(stats.instructions_decoded, 1);
    ASSERT_EQ(stats.unknown_opcodes, 0);
    ASSERT_TRUE(stats.opcode_counts[0x53] > 0);  // jumptext was decoded
}

TEST(script_lua_emitter_jumptext) {
    SymbolMap symbols;
    ScriptDecoder decoder(*g_rom, symbols);
    
    uint32_t script_addr = g_rom->bank_to_flat(0x6A, 0x40C8);
    auto script = decoder.decode_script(script_addr, "NewBarkTownSign");
    
    LuaEmitter emitter;
    std::string lua = emitter.emit(script);
    
    // Verify Lua output structure
    ASSERT_TRUE(lua.find("Script: NewBarkTownSign") != std::string::npos);
    ASSERT_TRUE(lua.find("function script.main(ctx)") != std::string::npos);
    ASSERT_TRUE(lua.find("ctx.ui:open_text()") != std::string::npos);
    // Now emits text_sequence() instead of text()
    ASSERT_TRUE(lua.find("ctx.ui:text_sequence(") != std::string::npos);
    ASSERT_TRUE(lua.find("NEW BARK TOWN") != std::string::npos);
    ASSERT_TRUE(lua.find("coroutine.yield") != std::string::npos);
    ASSERT_TRUE(lua.find("ctx.ui:close_text()") != std::string::npos);
    ASSERT_TRUE(lua.find("return script") != std::string::npos);
}

TEST(script_decoder_jumptextfaceplayer) {
    // NewBarkTownFisherScript at bank 0x6A, addr 0x409B
    // jumptextfaceplayer Text_ElmDiscoveredNewMon
    SymbolMap symbols;
    ScriptDecoder decoder(*g_rom, symbols);
    
    uint32_t script_addr = g_rom->bank_to_flat(0x6A, 0x409B);
    
    // First verify the opcode
    uint8_t opcode = g_rom->read_byte(script_addr);
    ASSERT_EQ(opcode, 0x51);  // jumptextfaceplayer
    
    auto script = decoder.decode_script(script_addr, "NewBarkTownFisherScript");
    ASSERT_EQ(script.instructions.size(), 1);
    
    bool is_jtfp = std::holds_alternative<Op_JumpTextFacePlayer>(script.instructions[0].op);
    ASSERT_TRUE(is_jtfp);
    
    auto& op = std::get<Op_JumpTextFacePlayer>(script.instructions[0].op);
    // Text_ElmDiscoveredNewMon mentions "PROF.ELM" and "POKéMON"
    ASSERT_TRUE(op.sequence.debug_string().find("ELM") != std::string::npos);
}

// Test that LINE and CONT remain distinguishable in the semantic TextSequence
// This verifies the fix for the lossy \n/\n\n encoding that was previously destroying
// the distinction between LINE (move to line 2, no wait) and CONT (wait, scroll, continue).
//
// Reference: NewBarkTownSignText from maps/NewBarkTown.asm:
//   text "NEW BARK TOWN"
//   para "The Town Where the"
//   line "Winds of a New"
//   cont "Beginning Blow"
//   done
TEST(script_semantic_text_line_vs_cont) {
    SymbolMap symbols;
    ScriptDecoder decoder(*g_rom, symbols);
    
    uint32_t script_addr = g_rom->bank_to_flat(0x6A, 0x40C8);  // NewBarkTownSign
    auto script = decoder.decode_script(script_addr, "NewBarkTownSign");
    
    ASSERT_EQ(script.instructions.size(), 1);
    bool is_jumptext = std::holds_alternative<Op_JumpText>(script.instructions[0].op);
    ASSERT_TRUE(is_jumptext);
    
    auto& op = std::get<Op_JumpText>(script.instructions[0].op);
    const TextSequence& seq = op.sequence;
    
    // Verify the exact semantic sequence matches the ROM:
    // Text("NEW BARK TOWN"), Para, Text("The Town Where the"), Line,
    // Text("Winds of a New"), Cont, Text("Beginning Blow"), Done
    //
    // We MUST have exactly 8 elements with the correct opcodes
    ASSERT_EQ(seq.elements.size(), 8);
    
    // Element 0: Text "NEW BARK TOWN"
    ASSERT_EQ(static_cast<int>(seq.elements[0].op), static_cast<int>(TextOp::Text));
    ASSERT_TRUE(seq.elements[0].text.find("NEW BARK TOWN") != std::string::npos);
    
    // Element 1: Para
    ASSERT_EQ(static_cast<int>(seq.elements[1].op), static_cast<int>(TextOp::Para));
    
    // Element 2: Text "The Town Where the"
    ASSERT_EQ(static_cast<int>(seq.elements[2].op), static_cast<int>(TextOp::Text));
    ASSERT_TRUE(seq.elements[2].text.find("Town Where") != std::string::npos);
    
    // Element 3: Line (NOT Cont! This is the critical distinction)
    ASSERT_EQ(static_cast<int>(seq.elements[3].op), static_cast<int>(TextOp::Line));
    
    // Element 4: Text "Winds of a New"
    ASSERT_EQ(static_cast<int>(seq.elements[4].op), static_cast<int>(TextOp::Text));
    ASSERT_TRUE(seq.elements[4].text.find("Winds") != std::string::npos);
    
    // Element 5: Cont (NOT Line! This is the critical distinction)
    ASSERT_EQ(static_cast<int>(seq.elements[5].op), static_cast<int>(TextOp::Cont));
    
    // Element 6: Text "Beginning Blow"
    ASSERT_EQ(static_cast<int>(seq.elements[6].op), static_cast<int>(TextOp::Text));
    ASSERT_TRUE(seq.elements[6].text.find("Beginning") != std::string::npos);
    
    // Element 7: Done
    ASSERT_EQ(static_cast<int>(seq.elements[7].op), static_cast<int>(TextOp::Done));
}

// Verify that the Lua emitter produces text_sequence with semantic operations
TEST(script_lua_emitter_text_sequence) {
    SymbolMap symbols;
    ScriptDecoder decoder(*g_rom, symbols);
    
    uint32_t script_addr = g_rom->bank_to_flat(0x6A, 0x40C8);
    auto script = decoder.decode_script(script_addr, "NewBarkTownSign");
    
    LuaEmitter emitter;
    std::string lua = emitter.emit(script);
    
    // The emitter must produce ctx.ui:text_sequence({...}) with semantic ops
    ASSERT_TRUE(lua.find("ctx.ui:text_sequence({") != std::string::npos);
    
    // Verify semantic operations appear in the Lua table
    ASSERT_TRUE(lua.find("op=\"text\"") != std::string::npos);
    ASSERT_TRUE(lua.find("op=\"para\"") != std::string::npos);
    ASSERT_TRUE(lua.find("op=\"line\"") != std::string::npos);
    ASSERT_TRUE(lua.find("op=\"cont\"") != std::string::npos);
    ASSERT_TRUE(lua.find("op=\"done\"") != std::string::npos);
    
    // Verify the text content is also present
    ASSERT_TRUE(lua.find("NEW BARK TOWN") != std::string::npos);
    ASSERT_TRUE(lua.find("Beginning Blow") != std::string::npos);
}

//=============================================================================
// TILESET / PALETTE TESTS
//=============================================================================

#include "crystal/extract/tileset_extractor.hpp"

TEST(tileset_palette_map_extracted) {
    // Verify palette map is extracted for johto_outdoor tileset
    TilesetExtractor extractor(*g_rom, *g_profile);
    auto result = extractor.extract_tileset("johto_outdoor");
    
    ASSERT_TRUE(result.success);
    
    // Crystal palette map has 256 entries (covers both VRAM banks 0 and 1)
    // Bank 0: tiles 0-95, Bank 1: tiles 128-223
    // Tiles 96-127 and 224-255 are unused/filler
    ASSERT_EQ(result.tileset.palette_map.size(), 256);
    
    // Palette IDs should be 0-6 (7 BG palettes) for valid tiles
    for (size_t i = 0; i < 96; ++i) {
        ASSERT_TRUE(result.tileset.palette_map[i] <= 6);
    }
    for (size_t i = 128; i < 224; ++i) {
        ASSERT_TRUE(result.tileset.palette_map[i] <= 6);
    }
}

TEST(tileset_palette_map_known_values) {
    // From pokecrystal/gfx/tilesets/johto_palette_map.asm:
    // First line: tilepal 0, GRAY, BROWN, BROWN, RED, GREEN, GREEN, GRAY, RED
    // This means tile 0=GRAY(0), tile 1=BROWN(5), tile 2=BROWN(5), tile 3=RED(1),
    //              tile 4=GREEN(2), tile 5=GREEN(2), tile 6=GRAY(0), tile 7=RED(1)
    TilesetExtractor extractor(*g_rom, *g_profile);
    auto result = extractor.extract_tileset("johto_outdoor");
    
    ASSERT_TRUE(result.success);
    
    // Crystal palette constants: GRAY=0, RED=1, GREEN=2, WATER=3, YELLOW=4, BROWN=5, ROOF=6
    // Verify first 8 tiles from johto_palette_map.asm first row
    ASSERT_EQ(result.tileset.palette_map[0], 0);  // GRAY
    ASSERT_EQ(result.tileset.palette_map[1], 5);  // BROWN
    ASSERT_EQ(result.tileset.palette_map[2], 5);  // BROWN
    ASSERT_EQ(result.tileset.palette_map[3], 1);  // RED
    ASSERT_EQ(result.tileset.palette_map[4], 2);  // GREEN
    ASSERT_EQ(result.tileset.palette_map[5], 2);  // GREEN
    ASSERT_EQ(result.tileset.palette_map[6], 0);  // GRAY
    ASSERT_EQ(result.tileset.palette_map[7], 1);  // RED
}

TEST(tileset_time_palettes_extracted) {
    // Verify all 5 time-of-day palette sets are extracted
    TilesetExtractor extractor(*g_rom, *g_profile);
    auto result = extractor.extract_tileset("johto_outdoor");
    
    ASSERT_TRUE(result.success);
    
    // Should have 5 time-of-day sets × 7 palettes each
    // Represented as time_palettes[5][7]
    for (int tod = 0; tod < 5; ++tod) {
        for (int pal = 0; pal < 7; ++pal) {
            // Each palette should have 4 valid colors (non-zero except possibly color 3)
            // At minimum, color 0 (background) should have some RGB values
            const auto& palette = result.tileset.time_palettes[tod][pal];
            
            // Verify colors are plausible (5-bit RGB values 0-31)
            for (int c = 0; c < 4; ++c) {
                ASSERT_TRUE(palette.colors[c].r <= 31);
                ASSERT_TRUE(palette.colors[c].g <= 31);
                ASSERT_TRUE(palette.colors[c].b <= 31);
            }
        }
    }
}

TEST(tileset_day_palette_colors) {
    // Verify specific Day palette colors from gfx/tilesets/bg_tiles.pal:
    // ; day
    //   RGB 27,31,27, 21,21,21, 13,13,13, 07,07,07 ; gray
    //   RGB 27,31,27, 31,19,24, 30,10,06, 07,07,07 ; red
    //   RGB 22,31,10, 12,25,01, 05,14,00, 07,07,07 ; green
    //   RGB 31,31,31, 08,12,31, 01,04,31, 07,07,07 ; water
    //   RGB 27,31,27, 31,31,07, 31,16,01, 07,07,07 ; yellow
    //   RGB 27,31,27, 24,18,07, 20,15,03, 07,07,07 ; brown
    //   RGB 27,31,27, 15,31,31, 05,17,31, 07,07,07 ; roof
    TilesetExtractor extractor(*g_rom, *g_profile);
    auto result = extractor.extract_tileset("johto_outdoor");
    
    ASSERT_TRUE(result.success);
    
    // Day = index 1
    const auto& day_pals = result.tileset.time_palettes[1];
    
    // GRAY palette (index 0): RGB 27,31,27 for color 0
    ASSERT_EQ(day_pals[0].colors[0].r, 27);
    ASSERT_EQ(day_pals[0].colors[0].g, 31);
    ASSERT_EQ(day_pals[0].colors[0].b, 27);
    // Color 3 (darkest): RGB 7,7,7
    ASSERT_EQ(day_pals[0].colors[3].r, 7);
    ASSERT_EQ(day_pals[0].colors[3].g, 7);
    ASSERT_EQ(day_pals[0].colors[3].b, 7);
    
    // GREEN palette (index 2): RGB 22,31,10 for color 0
    ASSERT_EQ(day_pals[2].colors[0].r, 22);
    ASSERT_EQ(day_pals[2].colors[0].g, 31);
    ASSERT_EQ(day_pals[2].colors[0].b, 10);
    // Color 1: RGB 12,25,01
    ASSERT_EQ(day_pals[2].colors[1].r, 12);
    ASSERT_EQ(day_pals[2].colors[1].g, 25);
    ASSERT_EQ(day_pals[2].colors[1].b, 1);
    
    // WATER palette (index 3): RGB 31,31,31 for color 0 (white)
    ASSERT_EQ(day_pals[3].colors[0].r, 31);
    ASSERT_EQ(day_pals[3].colors[0].g, 31);
    ASSERT_EQ(day_pals[3].colors[0].b, 31);
    // Color 2: RGB 01,04,31 (deep blue)
    ASSERT_EQ(day_pals[3].colors[2].r, 1);
    ASSERT_EQ(day_pals[3].colors[2].g, 4);
    ASSERT_EQ(day_pals[3].colors[2].b, 31);
}

TEST(tileset_night_palette_colors) {
    // Verify specific Night palette colors (darker/bluish tint)
    // ; nite
    //   RGB 15,14,24, 11,11,19, 07,07,12, 00,00,00 ; gray
    //   RGB 15,14,24, 14,07,17, 13,00,08, 00,00,00 ; red
    TilesetExtractor extractor(*g_rom, *g_profile);
    auto result = extractor.extract_tileset("johto_outdoor");
    
    ASSERT_TRUE(result.success);
    
    // Night = index 2
    const auto& night_pals = result.tileset.time_palettes[2];
    
    // GRAY palette (index 0): RGB 15,14,24 for color 0 (bluish tint)
    ASSERT_EQ(night_pals[0].colors[0].r, 15);
    ASSERT_EQ(night_pals[0].colors[0].g, 14);
    ASSERT_EQ(night_pals[0].colors[0].b, 24);
    // Color 3 (darkest): RGB 0,0,0 (black at night)
    ASSERT_EQ(night_pals[0].colors[3].r, 0);
    ASSERT_EQ(night_pals[0].colors[3].g, 0);
    ASSERT_EQ(night_pals[0].colors[3].b, 0);
}

TEST(tileset_render_atlas_uses_palettes) {
    // Verify that render_tileset_atlas uses per-tile palettes
    TilesetExtractor extractor(*g_rom, *g_profile);
    auto result = extractor.extract_tileset("johto_outdoor");
    
    ASSERT_TRUE(result.success);
    
    // Render atlas with Day palettes
    auto atlas = render_tileset_atlas(result.tileset, crystal::TimeOfDay::Day);
    
    // Atlas should be 256x512 (8 metatiles × 32px wide, 16 rows × 32px tall)
    ASSERT_EQ(atlas.atlas_width, 256);
    ASSERT_EQ(atlas.atlas_height, 512);
    
    // Verify pixels are not all the same (would indicate broken palette mapping)
    bool has_variety = false;
    uint32_t first_pixel = atlas.pixels[0];
    for (size_t i = 1; i < atlas.pixels.size(); ++i) {
        if (atlas.pixels[i] != first_pixel) {
            has_variety = true;
            break;
        }
    }
    ASSERT_TRUE(has_variety);
}

//=============================================================================
// FONT EXTRACTION TESTS
//=============================================================================

#include "crystal/extract/font_extractor.hpp"

TEST(font_extraction_succeeds) {
    // Verify font extraction from ROM succeeds
    FontExtractor extractor(*g_rom, *g_profile);
    auto result = extractor.extract_font();
    
    ASSERT_TRUE(result.success);
    ASSERT_TRUE(result.error.empty());
    ASSERT_STR_EQ(result.font.font_id, "crystal_main");
}

TEST(font_glyph_count) {
    // Font should have 160 glyphs total:
    // - 32 FontExtra glyphs (2bpp, codes 0x60-0x7F)
    // - 128 Font glyphs (1bpp, codes 0x80-0xFF)
    FontExtractor extractor(*g_rom, *g_profile);
    auto result = extractor.extract_font();
    
    ASSERT_TRUE(result.success);
    ASSERT_EQ(result.font.glyphs.size(), 160);
}

TEST(font_charmap_populated) {
    // Charmap should have entries for control chars + printable chars
    FontExtractor extractor(*g_rom, *g_profile);
    auto result = extractor.extract_font();
    
    ASSERT_TRUE(result.success);
    ASSERT_TRUE(result.font.charmap.size() > 0);
    ASSERT_TRUE(result.font.code_to_charmap.size() > 0);
}

TEST(font_border_glyph_indices) {
    // Border glyphs should be in FontExtra range (indices 0-31)
    // Border chars are at 0x79-0x7E, which maps to indices 25-30
    FontExtractor extractor(*g_rom, *g_profile);
    auto result = extractor.extract_font();
    
    ASSERT_TRUE(result.success);
    
    // Border indices should be 0x79-0x60 = 25, 0x7A-0x60 = 26, etc.
    ASSERT_EQ(result.font.border_top_left, 25);     // ┌ = 0x79
    ASSERT_EQ(result.font.border_top, 26);          // ─ = 0x7A
    ASSERT_EQ(result.font.border_top_right, 27);    // ┐ = 0x7B
    ASSERT_EQ(result.font.border_left, 28);         // │ = 0x7C
    ASSERT_EQ(result.font.border_bottom_left, 29);  // └ = 0x7D
    ASSERT_EQ(result.font.border_bottom_right, 30); // ┘ = 0x7E
    ASSERT_EQ(result.font.space_glyph, 31);         // space = 0x7F
}

TEST(font_cursor_glyph_index) {
    // Cursor glyph (▼) is at 0xEE in main font
    // Main font starts at index 32, so 0xEE - 0x80 + 32 = 142
    FontExtractor extractor(*g_rom, *g_profile);
    auto result = extractor.extract_font();
    
    ASSERT_TRUE(result.success);
    ASSERT_EQ(result.font.cursor_glyph, 32 + (0xEE - 0x80));  // 142
}

TEST(font_e_acute_mapping) {
    // é (critical for POKéMON) is at Crystal code 0xEA
    // Glyph index = 32 + (0xEA - 0x80) = 32 + 106 = 138
    FontExtractor extractor(*g_rom, *g_profile);
    auto result = extractor.extract_font();
    
    ASSERT_TRUE(result.success);
    
    // Lookup é by Crystal code
    auto entry = result.font.lookup(0xEA);
    ASSERT_TRUE(entry != nullptr);
    ASSERT_EQ(entry->glyph_index, 32 + (0xEA - 0x80));  // 138
    ASSERT_STR_EQ(entry->utf8_char, "é");
    ASSERT_TRUE(!entry->is_control);
}

TEST(font_uppercase_mapping) {
    // A-Z are at 0x80-0x99, indices 32-57
    FontExtractor extractor(*g_rom, *g_profile);
    auto result = extractor.extract_font();
    
    ASSERT_TRUE(result.success);
    
    // Lookup 'A' (0x80)
    auto a_entry = result.font.lookup(0x80);
    ASSERT_TRUE(a_entry != nullptr);
    ASSERT_EQ(a_entry->glyph_index, 32);  // First main font glyph
    ASSERT_STR_EQ(a_entry->utf8_char, "A");
    
    // Lookup 'Z' (0x99)
    auto z_entry = result.font.lookup(0x99);
    ASSERT_TRUE(z_entry != nullptr);
    ASSERT_EQ(z_entry->glyph_index, 32 + 25);  // 57
    ASSERT_STR_EQ(z_entry->utf8_char, "Z");
}

TEST(font_lowercase_mapping) {
    // a-z are at 0xA0-0xB9, indices 32 + (0xA0-0x80) to 32 + (0xB9-0x80)
    FontExtractor extractor(*g_rom, *g_profile);
    auto result = extractor.extract_font();
    
    ASSERT_TRUE(result.success);
    
    // Lookup 'a' (0xA0)
    auto a_entry = result.font.lookup(0xA0);
    ASSERT_TRUE(a_entry != nullptr);
    ASSERT_EQ(a_entry->glyph_index, 32 + (0xA0 - 0x80));  // 64
    ASSERT_STR_EQ(a_entry->utf8_char, "a");
    
    // Lookup 'z' (0xB9)
    auto z_entry = result.font.lookup(0xB9);
    ASSERT_TRUE(z_entry != nullptr);
    ASSERT_EQ(z_entry->glyph_index, 32 + (0xB9 - 0x80));  // 89
    ASSERT_STR_EQ(z_entry->utf8_char, "z");
}

TEST(font_digit_mapping) {
    // 0-9 are at 0xF6-0xFF, indices 32 + (0xF6-0x80) to 32 + (0xFF-0x80)
    FontExtractor extractor(*g_rom, *g_profile);
    auto result = extractor.extract_font();
    
    ASSERT_TRUE(result.success);
    
    // Lookup '0' (0xF6)
    auto zero_entry = result.font.lookup(0xF6);
    ASSERT_TRUE(zero_entry != nullptr);
    ASSERT_EQ(zero_entry->glyph_index, 32 + (0xF6 - 0x80));  // 150
    ASSERT_STR_EQ(zero_entry->utf8_char, "0");
    
    // Lookup '9' (0xFF)
    auto nine_entry = result.font.lookup(0xFF);
    ASSERT_TRUE(nine_entry != nullptr);
    ASSERT_EQ(nine_entry->glyph_index, 32 + (0xFF - 0x80));  // 159
    ASSERT_STR_EQ(nine_entry->utf8_char, "9");
}

TEST(font_control_chars) {
    // Control characters should be marked as such
    FontExtractor extractor(*g_rom, *g_profile);
    auto result = extractor.extract_font();
    
    ASSERT_TRUE(result.success);
    
    // LINE (0x4F) - jump to line 2
    auto line_entry = result.font.lookup(0x4F);
    ASSERT_TRUE(line_entry != nullptr);
    ASSERT_TRUE(line_entry->is_control);
    ASSERT_STR_EQ(line_entry->control_name, "LINE");
    
    // NEXT (0x4E) - advance 2 rows
    auto next_entry = result.font.lookup(0x4E);
    ASSERT_TRUE(next_entry != nullptr);
    ASSERT_TRUE(next_entry->is_control);
    ASSERT_STR_EQ(next_entry->control_name, "NEXT");
    
    // PARA (0x51) - wait → clear → continue
    auto para_entry = result.font.lookup(0x51);
    ASSERT_TRUE(para_entry != nullptr);
    ASSERT_TRUE(para_entry->is_control);
    ASSERT_STR_EQ(para_entry->control_name, "PARA");
    
    // DONE (0x57) - terminate text
    auto done_entry = result.font.lookup(0x57);
    ASSERT_TRUE(done_entry != nullptr);
    ASSERT_TRUE(done_entry->is_control);
    ASSERT_STR_EQ(done_entry->control_name, "DONE");
    
    // PROMPT (0x58) - wait then terminate
    auto prompt_entry = result.font.lookup(0x58);
    ASSERT_TRUE(prompt_entry != nullptr);
    ASSERT_TRUE(prompt_entry->is_control);
    ASSERT_STR_EQ(prompt_entry->control_name, "PROMPT");
}

TEST(font_atlas_render) {
    // Verify font atlas renders to correct dimensions
    FontExtractor extractor(*g_rom, *g_profile);
    auto result = extractor.extract_font();
    
    ASSERT_TRUE(result.success);
    
    auto atlas = render_font_atlas(result.font, default_text_palette());
    
    // 16 glyphs per row × 8px = 128px wide
    // 160 glyphs / 16 per row = 10 rows × 8px = 80px tall
    ASSERT_EQ(atlas.atlas_width, 128);
    ASSERT_EQ(atlas.atlas_height, 80);
    
    // Pixel count should match dimensions
    ASSERT_EQ(atlas.pixels.size(), 128 * 80);
}

TEST(font_atlas_uvs) {
    // Verify UV coordinates are generated for each glyph
    FontExtractor extractor(*g_rom, *g_profile);
    auto result = extractor.extract_font();
    
    ASSERT_TRUE(result.success);
    
    auto atlas = render_font_atlas(result.font, default_text_palette());
    
    // Should have UV for each glyph
    ASSERT_EQ(atlas.glyph_uvs.size(), result.font.glyphs.size());
    
    // All UVs should be in [0,1] range
    for (const auto& uv : atlas.glyph_uvs) {
        ASSERT_TRUE(uv.u0 >= 0.0f && uv.u0 <= 1.0f);
        ASSERT_TRUE(uv.v0 >= 0.0f && uv.v0 <= 1.0f);
        ASSERT_TRUE(uv.u1 >= 0.0f && uv.u1 <= 1.0f);
        ASSERT_TRUE(uv.v1 >= 0.0f && uv.v1 <= 1.0f);
        ASSERT_TRUE(uv.u1 > uv.u0);  // Width > 0
        ASSERT_TRUE(uv.v1 > uv.v0);  // Height > 0
    }
}

TEST(font_glyph_pixels_populated) {
    // Verify glyphs have actual pixel data (not all zeros)
    FontExtractor extractor(*g_rom, *g_profile);
    auto result = extractor.extract_font();
    
    ASSERT_TRUE(result.success);
    
    // 'A' glyph should have some non-zero pixels
    auto a_entry = result.font.lookup(0x80);
    ASSERT_TRUE(a_entry != nullptr);
    
    const auto& glyph = result.font.glyphs[a_entry->glyph_index];
    bool has_pixels = false;
    for (int i = 0; i < 64; ++i) {
        if (glyph.pixels[i] != 0) {
            has_pixels = true;
            break;
        }
    }
    ASSERT_TRUE(has_pixels);
}

TEST(font_border_glyphs_populated) {
    // Verify border glyphs have actual pixel data
    FontExtractor extractor(*g_rom, *g_profile);
    auto result = extractor.extract_font();
    
    ASSERT_TRUE(result.success);
    
    // Top-left corner should have pixels
    const auto& corner = result.font.glyphs[result.font.border_top_left];
    bool has_pixels = false;
    for (int i = 0; i < 64; ++i) {
        if (corner.pixels[i] != 0) {
            has_pixels = true;
            break;
        }
    }
    ASSERT_TRUE(has_pixels);
}

//=============================================================================
// MAIN
//=============================================================================

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <rom_path>\n";
        std::cerr << "\nRuns golden tests against known Crystal v1.1 data.\n";
        return 1;
    }
    
    // Load ROM
    std::cout << "Loading ROM: " << argv[1] << "\n";
    auto rom = RomData::load(argv[1]);
    if (!rom) {
        std::cerr << "Failed to load ROM\n";
        return 1;
    }
    
    // Get profile
    auto& registry = ProfileRegistry::instance();
    auto profile = registry.get_profile_by_hash(rom->hash());
    if (!profile) {
        std::cerr << "ROM not supported (SHA-1: " << rom->hash() << ")\n";
        std::cerr << "Golden tests require Crystal USA v1.1\n";
        return 1;
    }
    
    g_rom = rom.get();
    g_profile = profile;
    
    std::cout << "\n=== Running Golden Tests ===\n\n";
    
    // Profile tests
    RUN_TEST(profile_sha1_match);
    RUN_TEST(profile_identified);
    RUN_TEST(profile_offsets_valid);
    
    // Pokemon data tests
    RUN_TEST(bulbasaur_base_stats);
    RUN_TEST(pikachu_base_stats);
    
    // Move data tests
    RUN_TEST(pound_move_data);
    RUN_TEST(thunderbolt_move_data);
    
    // Map extraction tests
    RUN_TEST(new_bark_town_dimensions);
    RUN_TEST(new_bark_town_semantic_id);
    RUN_TEST(new_bark_town_by_semantic_id);
    RUN_TEST(new_bark_town_secondary_metadata);
    RUN_TEST(new_bark_town_connections);
    RUN_TEST(new_bark_town_events);
    RUN_TEST(elms_lab_dimensions);
    RUN_TEST(elms_lab_is_indoor);
    RUN_TEST(route26_dimensions);
    RUN_TEST(map_block_data_valid);
    RUN_TEST(invalid_map_fails_gracefully);
    
    // Bounds checking tests
    RUN_TEST(bounds_check_stats);
    RUN_TEST(bounds_check_invalid);
    
    // Package round-trip tests
    RUN_TEST(package_write);
    RUN_TEST(package_read_header);
    RUN_TEST(package_validate);
    RUN_TEST(package_list_maps);
    RUN_TEST(package_load_map);
    RUN_TEST(package_roundtrip_multiple_maps);
    RUN_TEST(package_cleanup);
    
    // Script decoder tests
    RUN_TEST(script_decoder_jumptext_opcode);
    RUN_TEST(script_decoder_decode_jumptext);
    RUN_TEST(script_decoder_text_content);
    RUN_TEST(script_decoder_text_decoding);
    RUN_TEST(script_decoder_stats);
    RUN_TEST(script_lua_emitter_jumptext);
    RUN_TEST(script_decoder_jumptextfaceplayer);
    RUN_TEST(script_semantic_text_line_vs_cont);
    RUN_TEST(script_lua_emitter_text_sequence);
    
    // Tileset / palette tests
    RUN_TEST(tileset_palette_map_extracted);
    RUN_TEST(tileset_palette_map_known_values);
    RUN_TEST(tileset_time_palettes_extracted);
    RUN_TEST(tileset_day_palette_colors);
    RUN_TEST(tileset_night_palette_colors);
    RUN_TEST(tileset_render_atlas_uses_palettes);
    
    // Font extraction tests
    RUN_TEST(font_extraction_succeeds);
    RUN_TEST(font_glyph_count);
    RUN_TEST(font_charmap_populated);
    RUN_TEST(font_border_glyph_indices);
    RUN_TEST(font_cursor_glyph_index);
    RUN_TEST(font_e_acute_mapping);
    RUN_TEST(font_uppercase_mapping);
    RUN_TEST(font_lowercase_mapping);
    RUN_TEST(font_digit_mapping);
    RUN_TEST(font_control_chars);
    RUN_TEST(font_atlas_render);
    RUN_TEST(font_atlas_uvs);
    RUN_TEST(font_glyph_pixels_populated);
    RUN_TEST(font_border_glyphs_populated);
    
    // Summary
    std::cout << "\n=== Results ===\n";
    std::cout << "Passed: " << g_tests_passed << "\n";
    std::cout << "Failed: " << g_tests_failed << "\n";
    
    return g_tests_failed > 0 ? 1 : 0;
}
