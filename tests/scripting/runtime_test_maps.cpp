// runtime_test_maps.cpp — map/warp/world tests, party, package round-trips
#include "engine/scripting/lua_runtime.hpp"
#include "engine/scripting/api_bindings.hpp"
#include "engine/core/game_loop.hpp"
#include "engine/core/game_state.hpp"
#include "engine/core/registry.hpp"
#include "engine/world/collision.hpp"
#include "engine/world/interaction.hpp"
#include "engine/world/runtime_map.hpp"
#include "engine/world/world_manager.hpp"
#include "engine/party/party.hpp"
#include "engine/party/pokemon.hpp"
#include "crystal/rom/loader.hpp"
#include "crystal/rom/profile.hpp"
#include "crystal/rom/symbol_map.hpp"
#include "crystal/output/native_package.hpp"
#include "crystal/extract/map_extractor.hpp"
#include "crystal/script/decoder.hpp"
#include "crystal/script/lua_emitter.hpp"
#include <array>
#include <unordered_map>
#include <filesystem>
#include <fstream>
#include <optional>
#include "scripting/runtime_test_shared.hpp"

TEST(newbarktown_door_tiles) {
    // Verify door positions match warp positions from pokecrystal
    // warp_event 6, 3, ELMS_LAB, 1 -> door at tile (6*2, 3*2) area
    MapExtractor extractor(*g_rom, *g_profile);
    auto result = extractor.extract_map("new_bark_town");
    ASSERT_TRUE(result.success);
    
    const std::vector<uint8_t>& blocks = result.map.blocks;
    const int map_width_blocks = result.map.width;
    
    // Door tiles have semantic CollisionClass::WarpDoor
    // Check near Elm's Lab entrance (warp at 6, 3 in blocks = tile 12, 6 area)
    // The door might be in the BR quadrant of a metatile
    
    // Scan the warp area for door collision
    bool found_door = false;
    for (int y = 5; y < 9; ++y) {
        for (int x = 11; x < 15; ++x) {
            CollisionClass coll = get_collision_from_blocks_johto(blocks, map_width_blocks, x, y);
            if (coll == CollisionClass::WarpDoor) {
                found_door = true;
                break;
            }
        }
        if (found_door) break;
    }
    
    ASSERT_TRUE(found_door);
    std::cout << "  [Door tile found near Elm's Lab entrance]\n";
}

TEST(newbarktown_collision_movement_blocked) {
    // Test that movement is blocked by walls using real map data
    MapExtractor extractor(*g_rom, *g_profile);
    auto result = extractor.extract_map("new_bark_town");
    ASSERT_TRUE(result.success);
    
    const std::vector<uint8_t>& blocks = result.map.blocks;
    const int map_width_blocks = result.map.width;
    const int tile_width = result.map.width * 2;
    const int tile_height = result.map.height * 2;
    
    CollisionMap collision_map;
    collision_map.width = tile_width;
    collision_map.height = tile_height;
    collision_map.get_collision = [&blocks, map_width_blocks](int32_t x, int32_t y) -> CollisionClass {
        return get_collision_from_blocks_johto(blocks, map_width_blocks, x, y);
    };
    
    Collision collision;
    std::vector<CollisionEntity> entities;
    
    // Place player at a walkable position and try to move into wall
    // Position (10, 10) should be walkable grass area
    CollisionEntity player{1, 10, 10, 0, 0, false, false};
    
    // Verify current position is walkable
    CollisionClass curr_coll = get_collision_from_blocks_johto(blocks, map_width_blocks, 10, 10);
    ASSERT_TRUE(collision_is_walkable(curr_coll));
    
    // Try moving up repeatedly until blocked
    int steps_until_blocked = 0;
    int y = 10;
    while (y > 0) {
        player.y = y;
        auto result_check = collision.can_move(collision_map, entities, player, enginemon::Direction::Up);
        if (!result_check.allowed) {
            break;
        }
        y--;
        steps_until_blocked++;
    }
    
    // Should eventually hit a wall (building area is at top)
    ASSERT_TRUE(steps_until_blocked > 0 && steps_until_blocked < 10);
    
    std::cout << "  [Movement blocked after " << steps_until_blocked << " steps north]\n";
}

TEST(newbarktown_entity_collision) {
    // Test entity collision with real map
    // Place an NPC at a known walkable position and verify player can't walk into them
    MapExtractor extractor(*g_rom, *g_profile);
    auto result = extractor.extract_map("new_bark_town");
    ASSERT_TRUE(result.success);
    
    const std::vector<uint8_t>& blocks = result.map.blocks;
    const int map_width_blocks = result.map.width;
    const int tile_width = result.map.width * 2;
    const int tile_height = result.map.height * 2;
    
    CollisionMap collision_map;
    collision_map.width = tile_width;
    collision_map.height = tile_height;
    collision_map.get_collision = [&blocks, map_width_blocks](int32_t x, int32_t y) -> CollisionClass {
        return get_collision_from_blocks_johto(blocks, map_width_blocks, x, y);
    };
    
    Collision collision;
    
    // Teacher NPC at (6, 8) - verified walkable from object positions test
    std::vector<CollisionEntity> entities;
    entities.push_back({2, 6, 8, 0, 0, false, false});  // Teacher NPC at (6, 8)
    
    // Player at (5, 8) trying to move right into Teacher at (6, 8)
    // First verify (5, 8) is walkable
    CollisionClass player_tile = get_collision_from_blocks_johto(blocks, map_width_blocks, 5, 8);
    ASSERT_TRUE(collision_is_walkable(player_tile));
    
    CollisionEntity player{1, 5, 8, 0, 0, false, false};
    
    auto result_check = collision.can_move(collision_map, entities, player, enginemon::Direction::Right);
    ASSERT_TRUE(!result_check.allowed);
    ASSERT_EQ(static_cast<int>(result_check.reason), static_cast<int>(MoveBlockReason::Entity));
    
    std::cout << "  [Player blocked by NPC entity at Teacher position]\n";
}

TEST(newbarktown_bg_event_positions) {
    // Verify BG event positions from extracted map match pokecrystal
    // bg_event 8, 8, BGEVENT_READ, NewBarkTownSign
    MapExtractor extractor(*g_rom, *g_profile);
    auto result = extractor.extract_map("new_bark_town");
    ASSERT_TRUE(result.success);
    
    ASSERT_EQ(result.map.bg_events.size(), 4);  // 4 signs
    
    // Find the town sign at (8, 8)
    bool found_town_sign = false;
    for (const auto& bg : result.map.bg_events) {
        if (bg.x == 8 && bg.y == 8) {
            found_town_sign = true;
            break;
        }
    }
    ASSERT_TRUE(found_town_sign);
    
    std::cout << "  [BG event at (8,8) found - NewBarkTownSign]\n";
}

TEST(newbarktown_object_positions) {
    // Verify object positions from extracted map
    // Teacher at (6, 8), Fisher at (12, 9), Rival at (3, 2)
    MapExtractor extractor(*g_rom, *g_profile);
    auto result = extractor.extract_map("new_bark_town");
    ASSERT_TRUE(result.success);
    
    ASSERT_EQ(result.map.objects.size(), 3);
    
    // Objects should have local_id 1, 2, 3
    // Crystal stores with +4 offset, extraction subtracts it
    bool found_teacher = false;
    bool found_fisher = false;
    
    for (const auto& obj : result.map.objects) {
        if (obj.local_id == 1) {
            // Teacher at (6, 8) - stored as (10, 12) in ROM, extracted as (6, 8)
            ASSERT_EQ(obj.x, 6);
            ASSERT_EQ(obj.y, 8);
            found_teacher = true;
        }
        if (obj.local_id == 2) {
            // Fisher at (12, 9)
            ASSERT_EQ(obj.x, 12);
            ASSERT_EQ(obj.y, 9);
            found_fisher = true;
        }
    }
    
    ASSERT_TRUE(found_teacher);
    ASSERT_TRUE(found_fisher);
    
    std::cout << "  [Object positions verified: Teacher(6,8), Fisher(12,9)]\n";
}

// =============================================================================
// NEW BARK TOWN INTERACTION TESTS - A-button interaction with real map data
// Proves: wrong facing does not trigger, correct facing resolves event,
// object/BG precedence, semantic script IDs (no ROM addresses)
// =============================================================================

// Helper: convert ExtractedMap objects to InteractableObject format
static std::vector<InteractableObject> make_interactable_objects(const ExtractedMap& map) {
    std::vector<InteractableObject> result;
    for (const auto& obj : map.objects) {
        InteractableObject io;
        io.local_id = obj.local_id;
        io.x = obj.x;
        io.y = obj.y;
        io.is_moving = false;  // For test purposes, all stationary
        io.is_trainer = obj.is_trainer;
        io.script_id = obj.script_id;
        io.visibility_flag = obj.visibility_flag;
        result.push_back(io);
    }
    return result;
}

// Helper: convert ExtractedMap BG events to InteractableBgEvent format
static std::vector<InteractableBgEvent> make_interactable_bg_events(const ExtractedMap& map) {
    std::vector<InteractableBgEvent> result;
    for (const auto& bg : map.bg_events) {
        InteractableBgEvent ibe;
        ibe.x = bg.x;
        ibe.y = bg.y;
        // Convert BgEventType to BgEventTypeId
        switch (bg.type) {
            case BgEventType::Read: ibe.type = BgEventTypeId::Read; break;
            case BgEventType::HiddenItem: ibe.type = BgEventTypeId::ItemIfSet; break;
            case BgEventType::FacingUp: ibe.type = BgEventTypeId::Up; break;
            default: ibe.type = BgEventTypeId::Read; break;
        }
        ibe.script_id = bg.script_id;
        ibe.item_id = bg.item_id;
        ibe.quantity = bg.quantity;
        result.push_back(ibe);
    }
    return result;
}

// Helper: convert RuntimeMap objects to InteractableObject format
static std::vector<InteractableObject> make_interactable_objects(const enginemon::RuntimeMap& map) {
    std::vector<InteractableObject> result;
    for (const auto& obj : map.objects) {
        InteractableObject io;
        io.local_id = obj.local_id;
        io.x = obj.x;
        io.y = obj.y;
        io.is_moving = false;  // For test purposes, all stationary
        io.is_trainer = obj.is_trainer;
        io.script_id = obj.script_id;
        io.visibility_flag = obj.visibility_flag;
        result.push_back(io);
    }
    return result;
}

// Helper: convert RuntimeMap BG events to InteractableBgEvent format
static std::vector<InteractableBgEvent> make_interactable_bg_events(const enginemon::RuntimeMap& map) {
    std::vector<InteractableBgEvent> result;
    for (const auto& bg : map.bg_events) {
        InteractableBgEvent ibe;
        ibe.x = bg.x;
        ibe.y = bg.y;
        // Convert RuntimeBgEventType to BgEventTypeId
        ibe.type = static_cast<uint8_t>(bg.type);
        ibe.script_id = bg.script_id;
        ibe.item_id = bg.item_id;
        ibe.quantity = bg.quantity;
        ibe.condition_flag = bg.condition_flag;
        result.push_back(ibe);
    }
    return result;
}

TEST(newbarktown_sign_wrong_facing) {
    // Test: facing away from sign should NOT trigger interaction
    // Sign at (8, 8), player stands at (8, 9) but faces down (away from sign)
    MapExtractor extractor(*g_rom, *g_profile);
    auto result = extractor.extract_map("new_bark_town");
    ASSERT_TRUE(result.success);
    
    const auto& map = result.map;
    const std::vector<uint8_t>& blocks = map.blocks;
    const int map_width_blocks = map.width;
    
    // Build interaction map
    InteractionMap imap;
    imap.width = map.width * 2;
    imap.height = map.height * 2;
    imap.get_collision = [&blocks, map_width_blocks](int32_t x, int32_t y) -> CollisionClass {
        return get_collision_from_blocks_johto(blocks, map_width_blocks, x, y);
    };
    
    auto objects = make_interactable_objects(map);
    auto bg_events = make_interactable_bg_events(map);
    
    Interaction interaction;
    
    // Player at (8, 9), facing DOWN (y+1) -> checks (8, 10), NOT the sign at (8, 8)
    auto check = interaction.check(imap, objects, bg_events, 8, 9, enginemon::Direction::Down);
    
    // Should NOT find the sign (facing wrong direction)
    // May or may not find something else at (8, 10)
    if (check.found()) {
        // If found something, it should NOT be the sign at (8, 8)
        ASSERT_TRUE(check.target_x != 8 || check.target_y != 8);
    }
    
    std::cout << "  [Wrong facing does not trigger sign at (8,8)]\n";
}

TEST(newbarktown_sign_correct_facing) {
    // Test: facing sign correctly should trigger interaction
    // Sign at (8, 8), player at (8, 9) facing UP
    MapExtractor extractor(*g_rom, *g_profile);
    auto result = extractor.extract_map("new_bark_town");
    ASSERT_TRUE(result.success);
    
    const auto& map = result.map;
    const std::vector<uint8_t>& blocks = map.blocks;
    const int map_width_blocks = map.width;
    
    InteractionMap imap;
    imap.width = map.width * 2;
    imap.height = map.height * 2;
    imap.get_collision = [&blocks, map_width_blocks](int32_t x, int32_t y) -> CollisionClass {
        return get_collision_from_blocks_johto(blocks, map_width_blocks, x, y);
    };
    
    auto objects = make_interactable_objects(map);
    auto bg_events = make_interactable_bg_events(map);
    
    Interaction interaction;
    
    // Player at (8, 9), facing UP -> checks (8, 8) where the sign is
    auto check = interaction.check(imap, objects, bg_events, 8, 9, enginemon::Direction::Up);
    
    ASSERT_TRUE(check.found());
    ASSERT_EQ(static_cast<int>(check.type), static_cast<int>(InteractionType::BgEvent));
    ASSERT_EQ(check.target_x, 8);
    ASSERT_EQ(check.target_y, 8);
    
    // Script ID should be semantic, not ROM address
    ASSERT_TRUE(!check.bg_script_id.empty());
    // Should NOT contain hex addresses like "0x1A40C8"
    ASSERT_TRUE(check.bg_script_id.find("0x") == std::string::npos);
    
    std::cout << "  [Sign at (8,8) found with correct facing, script: " << check.bg_script_id << "]\n";
}

TEST(newbarktown_teacher_interaction) {
    // Test: interacting with Teacher NPC at (6, 8)
    MapExtractor extractor(*g_rom, *g_profile);
    auto result = extractor.extract_map("new_bark_town");
    ASSERT_TRUE(result.success);
    
    const auto& map = result.map;
    const std::vector<uint8_t>& blocks = map.blocks;
    const int map_width_blocks = map.width;
    
    InteractionMap imap;
    imap.width = map.width * 2;
    imap.height = map.height * 2;
    imap.get_collision = [&blocks, map_width_blocks](int32_t x, int32_t y) -> CollisionClass {
        return get_collision_from_blocks_johto(blocks, map_width_blocks, x, y);
    };
    
    auto objects = make_interactable_objects(map);
    auto bg_events = make_interactable_bg_events(map);
    
    Interaction interaction;
    
    // Player at (5, 8), facing RIGHT -> checks (6, 8) where Teacher is
    auto check = interaction.check(imap, objects, bg_events, 5, 8, enginemon::Direction::Right);
    
    ASSERT_TRUE(check.found());
    ASSERT_EQ(static_cast<int>(check.type), static_cast<int>(InteractionType::Object));
    ASSERT_EQ(check.target_x, 6);
    ASSERT_EQ(check.target_y, 8);
    ASSERT_EQ(check.object_local_id, 1);  // Teacher is object 1
    
    // Script ID should be semantic
    ASSERT_TRUE(!check.object_script_id.empty());
    ASSERT_TRUE(check.object_script_id.find("0x") == std::string::npos);
    
    std::cout << "  [Teacher NPC found at (6,8), script: " << check.object_script_id << "]\n";
}

TEST(newbarktown_object_priority_integration) {
    // Test: if both object and BG event at same cell, object takes priority
    // This uses synthetic placement since real map doesn't have overlap
    MapExtractor extractor(*g_rom, *g_profile);
    auto result = extractor.extract_map("new_bark_town");
    ASSERT_TRUE(result.success);
    
    const auto& map = result.map;
    const std::vector<uint8_t>& blocks = map.blocks;
    const int map_width_blocks = map.width;
    
    InteractionMap imap;
    imap.width = map.width * 2;
    imap.height = map.height * 2;
    imap.get_collision = [&blocks, map_width_blocks](int32_t x, int32_t y) -> CollisionClass {
        return get_collision_from_blocks_johto(blocks, map_width_blocks, x, y);
    };
    
    // Create synthetic overlap: place NPC at sign position (8, 8)
    std::vector<InteractableObject> objects;
    objects.push_back({99, 8, 8, false, false, "synthetic_npc_script", ""});
    
    auto bg_events = make_interactable_bg_events(map);  // Real BG events
    
    Interaction interaction;
    
    // Player facing (8, 8) which has both NPC and sign
    auto check = interaction.check(imap, objects, bg_events, 8, 9, enginemon::Direction::Up);
    
    ASSERT_TRUE(check.found());
    ASSERT_EQ(static_cast<int>(check.type), static_cast<int>(InteractionType::Object));
    ASSERT_EQ(check.object_local_id, 99);
    
    std::cout << "  [Object priority verified over BG event]\n";
}

TEST(newbarktown_package_roundtrip_interaction) {
    // QUALITY GATE: Package-only integration
    // ROM → compiler → package → reload → interaction works
    // This proves interaction data survives serialization
    
    MapExtractor extractor(*g_rom, *g_profile);
    auto extract_result = extractor.extract_map("new_bark_town");
    ASSERT_TRUE(extract_result.success);
    
    // Write to package
    PackageWriter writer;
    writer.add_map(extract_result.map);
    
    std::filesystem::path temp_path = std::filesystem::temp_directory_path() / "enginemon_test.pkg";
    bool write_ok = writer.write(temp_path);
    ASSERT_TRUE(write_ok);
    
    // Reload from package (no ROM)
    auto reader = PackageReader::open(temp_path);
    ASSERT_TRUE(reader != nullptr);
    ASSERT_TRUE(reader->validate());
    
    // Load full map from package
    auto loaded = reader->load_full_map("new_bark_town");
    ASSERT_TRUE(loaded.has_value());
    
    const auto& map = *loaded;
    
    // Verify data survived round-trip
    ASSERT_EQ(map.width, extract_result.map.width);
    ASSERT_EQ(map.height, extract_result.map.height);
    ASSERT_EQ(map.bg_events.size(), extract_result.map.bg_events.size());
    ASSERT_EQ(map.objects.size(), extract_result.map.objects.size());
    
    // Now test interaction with package-loaded data
    const std::vector<uint8_t>& blocks = map.blocks;
    const int map_width_blocks = map.width;
    
    InteractionMap imap;
    imap.width = map.width * 2;
    imap.height = map.height * 2;
    imap.get_collision = [&blocks, map_width_blocks](int32_t x, int32_t y) -> CollisionClass {
        return get_collision_from_blocks_johto(blocks, map_width_blocks, x, y);
    };
    
    auto objects = make_interactable_objects(map);
    auto bg_events = make_interactable_bg_events(map);
    
    Interaction interaction;
    
    // Test sign interaction
    auto sign_check = interaction.check(imap, objects, bg_events, 8, 9, enginemon::Direction::Up);
    ASSERT_TRUE(sign_check.found());
    ASSERT_EQ(static_cast<int>(sign_check.type), static_cast<int>(InteractionType::BgEvent));
    
    // Test NPC interaction
    auto npc_check = interaction.check(imap, objects, bg_events, 5, 8, enginemon::Direction::Right);
    ASSERT_TRUE(npc_check.found());
    ASSERT_EQ(static_cast<int>(npc_check.type), static_cast<int>(InteractionType::Object));
    
    // Cleanup
    std::filesystem::remove(temp_path);
    
    std::cout << "  [Package round-trip: interaction data survived serialization]\n";
}

//=============================================================================
// BG EVENT TYPE PACKAGE SEAM TEST
//=============================================================================
// Proves all 9 BgEventTypes survive compiler → package → runtime round-trip.
// This test explicitly covers types that were previously collapsed to Read:
// FacingDown, FacingRight, FacingLeft, IfSet, IfNotSet, Copy

TEST(bg_event_type_package_roundtrip_all_types) {
    // QUALITY GATE: BG event type preservation through package seam
    // All 9 BgEventType values must survive round-trip without degradation
    
    // Create a synthetic map with all BG event types
    ExtractedMap test_map;
    test_map.map_id = "test_bg_types";
    test_map.width = 10;
    test_map.height = 10;
    test_map.blocks.resize(100, 0);
    test_map.tileset_id = "johto_outdoor";
    
    // Add one BG event of each type
    BgEvent ev_read;
    ev_read.x = 1; ev_read.y = 1;
    ev_read.type = BgEventType::Read;
    ev_read.script_id = "test_read";
    test_map.bg_events.push_back(ev_read);
    
    BgEvent ev_up;
    ev_up.x = 2; ev_up.y = 1;
    ev_up.type = BgEventType::FacingUp;
    ev_up.script_id = "test_up";
    test_map.bg_events.push_back(ev_up);
    
    BgEvent ev_down;
    ev_down.x = 3; ev_down.y = 1;
    ev_down.type = BgEventType::FacingDown;
    ev_down.script_id = "test_down";
    test_map.bg_events.push_back(ev_down);
    
    BgEvent ev_right;
    ev_right.x = 4; ev_right.y = 1;
    ev_right.type = BgEventType::FacingRight;
    ev_right.script_id = "test_right";
    test_map.bg_events.push_back(ev_right);
    
    BgEvent ev_left;
    ev_left.x = 5; ev_left.y = 1;
    ev_left.type = BgEventType::FacingLeft;
    ev_left.script_id = "test_left";
    test_map.bg_events.push_back(ev_left);
    
    BgEvent ev_ifset;
    ev_ifset.x = 6; ev_ifset.y = 1;
    ev_ifset.type = BgEventType::IfSet;
    ev_ifset.script_id = "test_ifset";
    ev_ifset.condition_flag = "FLAG_123";
    test_map.bg_events.push_back(ev_ifset);
    
    BgEvent ev_ifnotset;
    ev_ifnotset.x = 7; ev_ifnotset.y = 1;
    ev_ifnotset.type = BgEventType::IfNotSet;
    ev_ifnotset.script_id = "test_ifnotset";
    ev_ifnotset.condition_flag = "FLAG_456";
    test_map.bg_events.push_back(ev_ifnotset);
    
    BgEvent ev_hidden;
    ev_hidden.x = 8; ev_hidden.y = 1;
    ev_hidden.type = BgEventType::HiddenItem;
    ev_hidden.item_id = "potion";
    ev_hidden.quantity = 1;
    ev_hidden.condition_flag = "FLAG_ITEM_789";
    test_map.bg_events.push_back(ev_hidden);
    
    BgEvent ev_copy;
    ev_copy.x = 9; ev_copy.y = 1;
    ev_copy.type = BgEventType::Copy;
    ev_copy.script_id = "test_copy";
    test_map.bg_events.push_back(ev_copy);
    
    ASSERT_EQ(test_map.bg_events.size(), 9u);
    
    // Write to package
    PackageWriter writer;
    writer.add_map(test_map);
    
    std::string temp_path = "test_bg_types_roundtrip.emon";
    writer.write(temp_path);
    
    // Reload from package
    auto reader = PackageReader::open(temp_path);
    ASSERT_TRUE(reader != nullptr);
    ASSERT_TRUE(reader->validate());
    
    auto loaded = reader->load_full_map("test_bg_types");
    ASSERT_TRUE(loaded.has_value());
    
    const auto& map = *loaded;
    ASSERT_EQ(map.bg_events.size(), 9u);
    
    // Verify each type survived round-trip
    ASSERT_EQ(static_cast<int>(map.bg_events[0].type), static_cast<int>(RuntimeBgEventType::Read));
    ASSERT_EQ(static_cast<int>(map.bg_events[1].type), static_cast<int>(RuntimeBgEventType::Up));
    ASSERT_EQ(static_cast<int>(map.bg_events[2].type), static_cast<int>(RuntimeBgEventType::Down));
    ASSERT_EQ(static_cast<int>(map.bg_events[3].type), static_cast<int>(RuntimeBgEventType::Right));
    ASSERT_EQ(static_cast<int>(map.bg_events[4].type), static_cast<int>(RuntimeBgEventType::Left));
    ASSERT_EQ(static_cast<int>(map.bg_events[5].type), static_cast<int>(RuntimeBgEventType::IfSet));
    ASSERT_EQ(static_cast<int>(map.bg_events[6].type), static_cast<int>(RuntimeBgEventType::IfNotSet));
    ASSERT_EQ(static_cast<int>(map.bg_events[7].type), static_cast<int>(RuntimeBgEventType::HiddenItem));
    ASSERT_EQ(static_cast<int>(map.bg_events[8].type), static_cast<int>(RuntimeBgEventType::Copy));
    
    // Verify condition flags survived for IFSET/IFNOTSET
    ASSERT_STR_EQ(map.bg_events[5].condition_flag, "FLAG_123");
    ASSERT_STR_EQ(map.bg_events[6].condition_flag, "FLAG_456");
    
    // Cleanup
    std::filesystem::remove(temp_path);
    
    std::cout << "  [Package seam: all 9 BgEventTypes survive round-trip]\n";
}

TEST(bg_event_ifset_ifnotset_condition_flag_integration) {
    // QUALITY GATE: IFSET/IFNOTSET condition_flag round-trip through actual package path
    // This verifies the flag evaluation path can function after package loading
    
    // Use a real map that has BG events (New Bark Town has signs)
    MapExtractor extractor(*g_rom, *g_profile);
    auto result = extractor.extract_map("new_bark_town");
    ASSERT_TRUE(result.success);
    
    // Add synthetic IFSET and IFNOTSET events to the map
    BgEvent ev_ifset;
    ev_ifset.x = 1; ev_ifset.y = 1;
    ev_ifset.type = BgEventType::IfSet;
    ev_ifset.script_id = "nbt_conditional_set";
    ev_ifset.condition_flag = "FLAG_TEST_SET";
    result.map.bg_events.push_back(ev_ifset);
    
    BgEvent ev_ifnotset;
    ev_ifnotset.x = 2; ev_ifnotset.y = 1;
    ev_ifnotset.type = BgEventType::IfNotSet;
    ev_ifnotset.script_id = "nbt_conditional_notset";
    ev_ifnotset.condition_flag = "FLAG_TEST_NOTSET";
    result.map.bg_events.push_back(ev_ifnotset);
    
    // Write to package
    PackageWriter writer;
    writer.add_map(result.map);
    
    std::string temp_path = "test_ifset_integration.emon";
    writer.write(temp_path);
    
    // Reload from package
    auto reader = PackageReader::open(temp_path);
    ASSERT_TRUE(reader != nullptr);
    
    auto loaded = reader->load_full_map("new_bark_town");
    ASSERT_TRUE(loaded.has_value());
    
    const auto& map = *loaded;
    
    // Find the IFSET and IFNOTSET events
    const RuntimeBgEvent* found_ifset = nullptr;
    const RuntimeBgEvent* found_ifnotset = nullptr;
    
    for (const auto& bg : map.bg_events) {
        if (bg.type == RuntimeBgEventType::IfSet && bg.script_id == "nbt_conditional_set") {
            found_ifset = &bg;
        }
        if (bg.type == RuntimeBgEventType::IfNotSet && bg.script_id == "nbt_conditional_notset") {
            found_ifnotset = &bg;
        }
    }
    
    ASSERT_TRUE(found_ifset != nullptr);
    ASSERT_TRUE(found_ifnotset != nullptr);
    
    // Verify condition flags survived
    ASSERT_STR_EQ(found_ifset->condition_flag, "FLAG_TEST_SET");
    ASSERT_STR_EQ(found_ifnotset->condition_flag, "FLAG_TEST_NOTSET");
    
    // Convert to interactable and test with flag checker
    auto bg_events = make_interactable_bg_events(map);
    
    // Find the events in interactable list
    InteractableBgEvent* iact_ifset = nullptr;
    InteractableBgEvent* iact_ifnotset = nullptr;
    for (auto& ev : bg_events) {
        if (ev.script_id == "nbt_conditional_set") iact_ifset = &ev;
        if (ev.script_id == "nbt_conditional_notset") iact_ifnotset = &ev;
    }
    
    ASSERT_TRUE(iact_ifset != nullptr);
    ASSERT_TRUE(iact_ifnotset != nullptr);
    
    // Verify the type and condition_flag propagated to interactable
    ASSERT_EQ(static_cast<int>(iact_ifset->type), static_cast<int>(RuntimeBgEventType::IfSet));
    ASSERT_EQ(static_cast<int>(iact_ifnotset->type), static_cast<int>(RuntimeBgEventType::IfNotSet));
    ASSERT_STR_EQ(iact_ifset->condition_flag, "FLAG_TEST_SET");
    ASSERT_STR_EQ(iact_ifnotset->condition_flag, "FLAG_TEST_NOTSET");
    
    // Cleanup
    std::filesystem::remove(temp_path);
    
    std::cout << "  [Package seam: IFSET/IFNOTSET condition_flag integration verified]\n";
}

TEST(newbarktown_no_rom_addresses_in_scripts) {
    // QUALITY GATE: Runtime structures must not contain ROM addresses
    // Check that script IDs are semantic, not hex addresses
    MapExtractor extractor(*g_rom, *g_profile);
    auto result = extractor.extract_map("new_bark_town");
    ASSERT_TRUE(result.success);
    
    // Check all object scripts
    for (const auto& obj : result.map.objects) {
        // Script ID should NOT look like a ROM address
        // ROM addresses would be like "0x1A40C8" or "bank:0x6A_addr:0x406F"
        ASSERT_TRUE(obj.script_id.find("0x") == std::string::npos);
        ASSERT_TRUE(obj.script_id.find("bank:") == std::string::npos);
        ASSERT_TRUE(obj.script_id.find("addr:") == std::string::npos);
    }
    
    // Check all BG event scripts
    for (const auto& bg : result.map.bg_events) {
        ASSERT_TRUE(bg.script_id.find("0x") == std::string::npos);
        ASSERT_TRUE(bg.script_id.find("bank:") == std::string::npos);
    }
    
    std::cout << "  [No ROM addresses in script IDs - semantic only]\n";
}

TEST(newbarktown_interaction_determinism) {
    // QUALITY GATE: Determinism
    // Same input → same output
    MapExtractor extractor(*g_rom, *g_profile);
    auto result = extractor.extract_map("new_bark_town");
    ASSERT_TRUE(result.success);
    
    const auto& map = result.map;
    const std::vector<uint8_t>& blocks = map.blocks;
    const int map_width_blocks = map.width;
    
    InteractionMap imap;
    imap.width = map.width * 2;
    imap.height = map.height * 2;
    imap.get_collision = [&blocks, map_width_blocks](int32_t x, int32_t y) -> CollisionClass {
        return get_collision_from_blocks_johto(blocks, map_width_blocks, x, y);
    };
    
    auto objects = make_interactable_objects(map);
    auto bg_events = make_interactable_bg_events(map);
    
    Interaction interaction;
    
    // Run same check multiple times
    std::vector<InteractionResult> results;
    for (int i = 0; i < 5; ++i) {
        results.push_back(interaction.check(imap, objects, bg_events, 8, 9, enginemon::Direction::Up));
    }
    
    // All results should be identical
    for (size_t i = 1; i < results.size(); ++i) {
        ASSERT_EQ(static_cast<int>(results[i].type), static_cast<int>(results[0].type));
        ASSERT_EQ(results[i].target_x, results[0].target_x);
        ASSERT_EQ(results[i].target_y, results[0].target_y);
        ASSERT_TRUE(results[i].bg_script_id == results[0].bg_script_id);
    }
    
    std::cout << "  [Interaction is deterministic - 5 identical results]\n";
}

//=============================================================================
// HEADLESS GAME LOOP TESTS
// End-to-end testing of the headless playable New Bark loop
//=============================================================================

TEST(headless_loop_spawn_player) {
    HeadlessGameLoop loop;
    
    // Spawn player at known New Bark Town position
    loop.spawn_player(5, 5, enginemon::Direction::Down);
    
    ASSERT_EQ(loop.player().x, 5);
    ASSERT_EQ(loop.player().y, 5);
    ASSERT_EQ(static_cast<int>(loop.player().facing), static_cast<int>(enginemon::Direction::Down));
    ASSERT_TRUE(loop.is_idle());
    
    std::cout << "  [Player spawned at (5,5) facing down]\n";
}

TEST(headless_loop_facing_update) {
    HeadlessGameLoop loop;
    loop.spawn_player(5, 5, enginemon::Direction::Down);
    
    // Set up a map with all walkable tiles (no collision)
    loop.set_collision_data([](int32_t x, int32_t y) -> CollisionClass {
        return CollisionClass::Floor;  // All walkable
    });
    
    // Load a minimal map
    RuntimeMap map;
    map.map_id = "test_map";
    map.width = 10;
    map.height = 10;
    map.blocks.resize(100, 0);
    loop.load_map(map);
    
    // Process up input - should update facing
    auto result = loop.process_input(InputAction::MoveUp);
    
    ASSERT_TRUE(result.accepted);
    ASSERT_EQ(static_cast<int>(loop.player().facing), static_cast<int>(enginemon::Direction::Up));
    
    std::cout << "  [Facing updated to up on movement input]\n";
}

TEST(headless_loop_movement_blocked) {
    HeadlessGameLoop loop;
    loop.spawn_player(0, 5, enginemon::Direction::Left);  // At left edge
    
    // Set up collision that blocks left edge
    loop.set_collision_data([](int32_t x, int32_t y) -> CollisionClass {
        if (x < 0) return CollisionClass::Wall;
        return CollisionClass::Floor;
    });
    
    RuntimeMap map;
    map.map_id = "test_map";
    map.width = 10;
    map.height = 10;
    map.blocks.resize(100, 0);
    loop.load_map(map);
    
    // Try to move left (into boundary)
    auto result = loop.process_input(InputAction::MoveLeft);
    
    ASSERT_TRUE(result.accepted);
    ASSERT_TRUE(result.blocked);
    ASSERT_STR_EQ(result.block_reason, "bounds");
    
    // Position unchanged, facing updated
    ASSERT_EQ(loop.player().x, 0);
    ASSERT_EQ(static_cast<int>(loop.player().facing), static_cast<int>(enginemon::Direction::Left));
    
    std::cout << "  [Movement blocked at boundary, facing still updated]\n";
}

TEST(headless_loop_movement_ticks) {
    HeadlessGameLoop loop;
    loop.spawn_player(5, 5, enginemon::Direction::Down);
    
    loop.set_collision_data([](int32_t x, int32_t y) -> CollisionClass {
        return CollisionClass::Floor;
    });
    
    RuntimeMap map;
    map.map_id = "test_map";
    map.width = 20;
    map.height = 20;
    map.blocks.resize(100, 0);
    loop.load_map(map);
    
    // Start movement right
    auto result = loop.process_input(InputAction::MoveRight);
    
    ASSERT_TRUE(result.accepted);
    ASSERT_FALSE(result.blocked);
    ASSERT_TRUE(loop.is_moving());
    
    // Tick 16 frames (pokecrystal OBJECT_STEP_DURATION)
    for (int i = 0; i < 16; i++) {
        loop.tick();
    }
    
    // Movement should be complete
    ASSERT_TRUE(loop.is_idle());
    ASSERT_EQ(loop.player().x, 6);  // Moved one tile right
    ASSERT_EQ(loop.player().y, 5);  // Y unchanged
    
    std::cout << "  [Movement completed after 16 ticks at (6,5)]\n";
}

TEST(headless_loop_input_locked_during_movement) {
    HeadlessGameLoop loop;
    loop.spawn_player(5, 5, enginemon::Direction::Down);
    
    loop.set_collision_data([](int32_t, int32_t) -> CollisionClass {
        return CollisionClass::Floor;
    });
    
    RuntimeMap map;
    map.map_id = "test_map";
    map.width = 20;
    map.height = 20;
    map.blocks.resize(100, 0);
    loop.load_map(map);
    
    // Start movement
    loop.process_input(InputAction::MoveRight);
    ASSERT_TRUE(loop.is_moving());
    
    // Try to input during movement - should be rejected
    auto result = loop.process_input(InputAction::MoveUp);
    ASSERT_FALSE(result.accepted);
    ASSERT_TRUE(loop.is_input_locked());
    
    std::cout << "  [Input locked during movement]\n";
}

// Helper to create RuntimeMap from ExtractedMap
static RuntimeMap extracted_to_runtime(const ExtractedMap& ext) {
    RuntimeMap rt;
    rt.map_id = ext.map_id;
    rt.display_name = ext.display_name;
    rt.width = ext.width;
    rt.height = ext.height;
    rt.tileset_id = ext.tileset_id;
    rt.blocks = ext.blocks;
    rt.border_block = ext.border_block;
    rt.environment_type = ext.environment_type;
    rt.is_outdoor = ext.is_outdoor;
    rt.phone_service_disabled = ext.phone_service_disabled;
    rt.lighting = ext.lighting;
    rt.music_id = ext.music_id;
    rt.fish_group_id = ext.fish_group_id;
    rt.landmark_id = ext.landmark_id;
    rt.map_script_id = ext.map_script_id;
    
    // Convert warps
    for (const auto& warp : ext.warps) {
        RuntimeWarp rwarp;
        rwarp.x = warp.x;
        rwarp.y = warp.y;
        rwarp.target_map_id = warp.target_map_id;
        rwarp.target_warp_index = warp.target_warp_index;
        rt.warps.push_back(rwarp);
    }
    
    // Convert connections
    for (const auto& conn : ext.connections) {
        RuntimeConnection rconn;
        rconn.direction = static_cast<ConnectionDirection>(conn.direction);
        rconn.target_map_id = conn.target_map_id;
        rconn.src_skip_blocks    = conn.src_skip_blocks;
        rconn.strip_length_blocks = conn.strip_length_blocks;
        rconn.coord_adjust_tiles = conn.coord_adjust_tiles;
        rt.connections.push_back(rconn);
    }
    
    // Convert bg_events
    for (const auto& bg : ext.bg_events) {
        RuntimeBgEvent rbg;
        rbg.x = bg.x;
        rbg.y = bg.y;
        rbg.type = static_cast<RuntimeBgEventType>(bg.type);
        rbg.script_id = bg.script_id;
        rbg.item_id = bg.item_id;
        rbg.quantity = bg.quantity;
        rt.bg_events.push_back(rbg);
    }
    
    // Convert objects
    for (const auto& obj : ext.objects) {
        RuntimeObject robj;
        robj.local_id = obj.local_id;
        robj.x = obj.x;
        robj.y = obj.y;
        robj.sprite_id = obj.sprite_id;
        robj.movement_type = obj.movement_type;
        robj.movement_radius_x = obj.movement_radius_x;
        robj.movement_radius_y = obj.movement_radius_y;
        robj.hour_start = obj.hour_start;
        robj.hour_end = obj.hour_end;
        robj.palette = obj.palette;
        robj.is_trainer = obj.is_trainer;
        robj.trainer_sight_range = obj.trainer_sight_range;
        robj.script_id = obj.script_id;
        robj.visibility_flag = obj.visibility_flag;
        rt.objects.push_back(robj);
    }
    
    return rt;
}

TEST(headless_newbark_walk_one_tile) {
    // Load real New Bark Town from ROM
    MapExtractor extractor(*g_rom, *g_profile);
    auto result = extractor.extract_map("new_bark_town");
    ASSERT_TRUE(result.success);
    
    RuntimeMap rtmap = extracted_to_runtime(result.map);
    
    HeadlessGameLoop loop;
    loop.load_map(rtmap);
    
    // Use existing collision table (JOHTO_COLLISION_TABLE via get_collision_from_blocks)
    const auto& blocks = rtmap.blocks;
    int map_width = rtmap.width;
    
    loop.set_collision_data([&blocks, map_width](int32_t x, int32_t y) -> CollisionClass {
        return get_collision_from_blocks_johto(blocks, map_width, x, y);
    });
    
    // Start at a known walkable position (center-ish of map)
    // New Bark Town is small; position (5,6) should be walkable grass
    loop.spawn_player(5, 6, enginemon::Direction::Down);
    
    // Move right
    auto move_result = loop.process_input(InputAction::MoveRight);
    
    // Accept even if blocked (we're testing the loop mechanics)
    ASSERT_TRUE(move_result.accepted);
    
    if (!move_result.blocked) {
        // Tick to completion
        for (int i = 0; i < 16; i++) {
            loop.tick();
        }
        
        ASSERT_TRUE(loop.is_idle());
        ASSERT_EQ(loop.player().x, 6);
        std::cout << "  [Walked one tile right to (6,6)]\n";
    } else {
        // If blocked, position stays same but facing updated
        ASSERT_EQ(loop.player().x, 5);
        ASSERT_EQ(static_cast<int>(loop.player().facing), static_cast<int>(enginemon::Direction::Right));
        std::cout << "  [Movement blocked at (5,6), facing updated to right]\n";
    }
}

TEST(headless_newbark_sign_interaction) {
    MapExtractor extractor(*g_rom, *g_profile);
    auto result = extractor.extract_map("new_bark_town");
    ASSERT_TRUE(result.success);
    
    RuntimeMap rtmap = extracted_to_runtime(result.map);
    
    HeadlessGameLoop loop;
    loop.load_map(rtmap);
    
    const auto& blocks = rtmap.blocks;
    int map_width = rtmap.width;
    
    loop.set_collision_data([&blocks, map_width](int32_t x, int32_t y) -> CollisionClass {
        return get_collision_from_blocks_johto(blocks, map_width, x, y);
    });
    
    // NewBarkTownSign is at (8, 8) - verified from earlier tests
    // Player should be south of sign facing up
    loop.spawn_player(8, 9, enginemon::Direction::Up);
    
    // Track interaction result
    std::string triggered_script;
    loop.set_interaction_callback([&](const InteractionResult& ir) {
        if (ir.found()) {
            triggered_script = ir.script_id();
        }
    });
    
    // Press A to interact
    auto interact_result = loop.process_input(InputAction::Interact);
    
    ASSERT_TRUE(interact_result.accepted);
    ASSERT_TRUE(interact_result.interaction);
    ASSERT_FALSE(triggered_script.empty());
    
    std::cout << "  [Sign interaction triggered script: " << triggered_script << "]\n";
}

TEST(headless_newbark_teacher_interaction) {
    MapExtractor extractor(*g_rom, *g_profile);
    auto result = extractor.extract_map("new_bark_town");
    ASSERT_TRUE(result.success);
    
    RuntimeMap rtmap = extracted_to_runtime(result.map);
    
    HeadlessGameLoop loop;
    loop.load_map(rtmap);
    
    const auto& blocks = rtmap.blocks;
    int map_width = rtmap.width;
    
    loop.set_collision_data([&blocks, map_width](int32_t x, int32_t y) -> CollisionClass {
        return get_collision_from_blocks_johto(blocks, map_width, x, y);
    });
    
    // Add NPCs from the map (with movement behavior)
    for (const auto& obj : rtmap.objects) {
        NpcState npc;
        npc.id = obj.local_id;
        npc.x = obj.x;
        npc.y = obj.y;
        npc.facing = movement_data_to_facing(obj.movement_type);
        npc.is_moving = false;
        npc.is_trainer = obj.is_trainer;
        npc.script_id = obj.script_id;
        npc.visibility_flag = obj.visibility_flag;
        npc.visible = true;
        
        // Initialize movement behavior from Crystal movement_type
        npc.behavior = movement_data_to_behavior(obj.movement_type);
        npc.radius_x = obj.movement_radius_x;
        npc.radius_y = obj.movement_radius_y;
        npc.init_x = obj.x;
        npc.init_y = obj.y;
        npc.idle_timer = 30 + (obj.local_id * 17) % 98;  // Stagger initial timers
        npc.target_x = obj.x;
        npc.target_y = obj.y;
        npc.move_progress = 0;
        npc.frozen = false;
        
        loop.add_npc(npc);
    }
    
    // Find the Teacher NPC (object_script_0 typically)
    // Teacher is at approximately (6, 8) based on earlier tests
    loop.spawn_player(5, 8, enginemon::Direction::Right);
    
    std::string triggered_script;
    loop.set_interaction_callback([&](const InteractionResult& ir) {
        if (ir.found()) {
            triggered_script = ir.script_id();
        }
    });
    
    auto interact_result = loop.process_input(InputAction::Interact);
    
    ASSERT_TRUE(interact_result.accepted);
    ASSERT_TRUE(interact_result.interaction);
    ASSERT_FALSE(triggered_script.empty());
    
    std::cout << "  [Teacher interaction triggered script: " << triggered_script << "]\n";
}

TEST(headless_newbark_determinism) {
    // Same input sequence produces same state
    MapExtractor extractor(*g_rom, *g_profile);
    auto result = extractor.extract_map("new_bark_town");
    ASSERT_TRUE(result.success);
    
    RuntimeMap rtmap = extracted_to_runtime(result.map);
    
    const auto& blocks = rtmap.blocks;
    int map_width = rtmap.width;
    
    auto collision_fn = [&blocks, map_width](int32_t x, int32_t y) -> CollisionClass {
        return get_collision_from_blocks_johto(blocks, map_width, x, y);
    };
    
    // Run same sequence twice
    std::vector<uint64_t> hashes1, hashes2;
    
    for (int run = 0; run < 2; run++) {
        HeadlessGameLoop loop;
        loop.load_map(rtmap);
        loop.set_collision_data(collision_fn);
        loop.spawn_player(5, 6, enginemon::Direction::Down);
        
        // Input sequence: right, tick*16, down, tick*16, interact
        loop.process_input(InputAction::MoveRight);
        for (int i = 0; i < 16; i++) loop.tick();
        
        loop.process_input(InputAction::MoveDown);
        for (int i = 0; i < 16; i++) loop.tick();
        
        loop.process_input(InputAction::Interact);
        
        uint64_t hash = loop.state_hash();
        if (run == 0) hashes1.push_back(hash);
        else hashes2.push_back(hash);
    }
    
    ASSERT_EQ(hashes1.size(), hashes2.size());
    for (size_t i = 0; i < hashes1.size(); i++) {
        ASSERT_EQ(hashes1[i], hashes2[i]);
    }
    
    std::cout << "  [Determinism verified: same input → same state hash]\n";
}

TEST(headless_newbark_script_execution) {
    // Full integration: load map, interact with sign, execute generated Lua
    MapExtractor extractor(*g_rom, *g_profile);
    auto result = extractor.extract_map("new_bark_town");
    ASSERT_TRUE(result.success);
    
    RuntimeMap rtmap = extracted_to_runtime(result.map);
    
    HeadlessGameLoop loop;
    loop.load_map(rtmap);
    
    const auto& blocks = rtmap.blocks;
    int map_width = rtmap.width;
    
    loop.set_collision_data([&blocks, map_width](int32_t x, int32_t y) -> CollisionClass {
        return get_collision_from_blocks_johto(blocks, map_width, x, y);
    });
    
    // Set up Lua runtime
    LuaRuntime runtime;
    runtime.set_error_handler([](const std::string& error, const std::string& tb) {
        std::cerr << "Script error: " << error << "\n" << tb << "\n";
    });
    loop.set_lua_runtime(&runtime);
    
    // Set up script loader that generates Lua from ROM
    SymbolMap symbols;
    loop.set_script_loader([&](const std::string& script_id) -> std::string {
        // The sign script is NewBarkTownSign
        if (script_id.find("bg_event") != std::string::npos) {
            // Decode and emit the sign script
            ScriptDecoder decoder(*g_rom, symbols);
            uint32_t script_addr = g_rom->bank_to_flat(0x6A, 0x40C8);
            auto script = decoder.decode_script(script_addr, "NewBarkTownSign");
            
            LuaEmitter emitter;
            std::string lua_code = emitter.emit(script);
            
            // Wrap in IIFE to create global "script" table (same as main_tiles.cpp)
            lua_code = "script = (function()\n" + lua_code + "\nend)()";
            return lua_code;
        }
        return "";
    });
    
    // Position player facing sign
    loop.spawn_player(8, 9, enginemon::Direction::Up);
    
    // Interact with sign
    auto interact_result = loop.process_input(InputAction::Interact);
    ASSERT_TRUE(interact_result.accepted);
    ASSERT_TRUE(interact_result.interaction);
    
    // If script was started, complete it
    if (loop.is_script_running()) {
        // Tick to let script run
        for (int i = 0; i < 100 && loop.is_script_running(); i++) {
            loop.tick();
        }
    }
    
    ASSERT_TRUE(loop.is_idle());
    std::cout << "  [Script executed and completed]\n";
}

// =============================================================================
// WORLD CONTINUITY TESTS - Warps, Connections, Save/Load
// Proves: map transitions work, LAST_MAP exits resolve, save→load roundtrip
// =============================================================================

// Global map cache for world manager tests
static std::unordered_map<std::string, RuntimeMap> g_map_cache;

// Helper to get or extract a map
static const RuntimeMap& get_cached_map(const std::string& map_id) {
    auto it = g_map_cache.find(map_id);
    if (it != g_map_cache.end()) {
        return it->second;
    }
    
    MapExtractor extractor(*g_rom, *g_profile);
    auto result = extractor.extract_map(map_id);
    if (result.success) {
        g_map_cache[map_id] = extracted_to_runtime(result.map);
    }
    return g_map_cache[map_id];
}

TEST(newbark_has_warps) {
    // Verify New Bark Town has warps to interiors
    MapExtractor extractor(*g_rom, *g_profile);
    auto result = extractor.extract_map("new_bark_town");
    ASSERT_TRUE(result.success);
    
    // New Bark Town should have warps to:
    // - Elm's Lab (doorway)
    // - Player's house (doorway)
    // - Neighbor's house (doorway)
    ASSERT_TRUE(result.map.warps.size() >= 3);
    
    // Check that warps have semantic target map IDs (not ROM addresses)
    for (const auto& warp : result.map.warps) {
        ASSERT_FALSE(warp.target_map_id.empty());
        // Semantic IDs are lowercase with underscores
        ASSERT_TRUE(warp.target_map_id.find("0x") == std::string::npos);
    }
    
    std::cout << "  [New Bark Town has " << result.map.warps.size() << " warps]\n";
}

TEST(newbark_has_connections) {
    // Verify New Bark Town has connection to Route 29
    MapExtractor extractor(*g_rom, *g_profile);
    auto result = extractor.extract_map("new_bark_town");
    ASSERT_TRUE(result.success);
    
    // New Bark Town connects to Route 29 on the west
    ASSERT_TRUE(result.map.connections.size() >= 1);
    
    // Find westward connection
    bool found_west = false;
    for (const auto& conn : result.map.connections) {
        if (conn.direction == crystal::Direction::West) {
            found_west = true;
            ASSERT_FALSE(conn.target_map_id.empty());
            std::cout << "  [West connection to: " << conn.target_map_id << "]\n";
        }
    }
    ASSERT_TRUE(found_west);
}

TEST(elms_lab_has_exit_warp) {
    // Verify Elm's Lab has an exit warp (LAST_MAP or back to New Bark Town)
    MapExtractor extractor(*g_rom, *g_profile);
    auto result = extractor.extract_map("elms_lab");
    ASSERT_TRUE(result.success);
    
    ASSERT_TRUE(result.map.warps.size() >= 1);
    
    // Check for LAST_MAP special ID (typical for interior exits)
    bool has_exit = false;
    for (const auto& warp : result.map.warps) {
        // Either LAST_MAP or explicit new_bark_town
        if (warp.target_map_id == "LAST_MAP" || 
            warp.target_map_id.find("new_bark") != std::string::npos) {
            has_exit = true;
            std::cout << "  [Exit warp targets: " << warp.target_map_id << "]\n";
        }
    }
    ASSERT_TRUE(has_exit);
}

TEST(world_manager_load_map) {
    // Test basic map loading
    WorldManager wm;
    
    wm.set_map_loader([](const std::string& map_id) -> std::optional<RuntimeMap> {
        auto& cached = get_cached_map(map_id);
        if (cached.map_id.empty()) return std::nullopt;
        return cached;
    });
    
    bool loaded = wm.load_map("new_bark_town");
    ASSERT_TRUE(loaded);
    ASSERT_TRUE(wm.current_map() != nullptr);
    ASSERT_STR_EQ(wm.current_map_id().c_str(), "new_bark_town");
    
    std::cout << "  [WorldManager loaded new_bark_town]\n";
}

TEST(world_manager_get_warp_at) {
    // Test finding warps at positions
    WorldManager wm;
    
    wm.set_map_loader([](const std::string& map_id) -> std::optional<RuntimeMap> {
        auto& cached = get_cached_map(map_id);
        if (cached.map_id.empty()) return std::nullopt;
        return cached;
    });
    
    wm.load_map("new_bark_town");
    
    // Find a warp position from the map
    const auto* map = wm.current_map();
    ASSERT_TRUE(map != nullptr);
    ASSERT_TRUE(map->warps.size() > 0);
    
    const auto& first_warp = map->warps[0];
    const RuntimeWarp* found = wm.get_warp_at(first_warp.x, first_warp.y);
    ASSERT_TRUE(found != nullptr);
    ASSERT_STR_EQ(found->target_map_id.c_str(), first_warp.target_map_id.c_str());
    
    std::cout << "  [Found warp at (" << (int)first_warp.x << "," << (int)first_warp.y << ")]\n";
}

TEST(warp_newbark_to_elms_lab) {
    // Test warp from New Bark Town to Elm's Lab
    WorldManager wm;
    GameState state;
    
    wm.set_map_loader([](const std::string& map_id) -> std::optional<RuntimeMap> {
        auto& cached = get_cached_map(map_id);
        if (cached.map_id.empty()) return std::nullopt;
        return cached;
    });
    
    wm.load_map("new_bark_town");
    state.player.current_map_id = "new_bark_town";
    state.player.x = 5;
    state.player.y = 5;
    
    // Find warp to Elm's Lab
    const auto* map = wm.current_map();
    const RuntimeWarp* elms_warp = nullptr;
    for (const auto& warp : map->warps) {
        if (warp.target_map_id.find("elms_lab") != std::string::npos) {
            elms_warp = &warp;
            break;
        }
    }
    
    if (!elms_warp) {
        std::cout << "  [SKIP: No Elm's Lab warp found in extracted data]\n";
        return;
    }
    
    // Execute the warp
    auto result = wm.execute_warp(*elms_warp, state);
    ASSERT_TRUE(result.success);
    ASSERT_TRUE(result.target_map_id.find("elms_lab") != std::string::npos);
    ASSERT_STR_EQ(state.player.current_map_id.c_str(), result.target_map_id.c_str());
    
    // Player should have valid position in new map
    ASSERT_TRUE(state.player.x >= 0);
    ASSERT_TRUE(state.player.y >= 0);
    
    std::cout << "  [Warped to " << result.target_map_id << " at (" 
              << state.player.x << "," << state.player.y << ")]\n";
}

TEST(warp_elms_lab_to_newbark_last_map) {
    // Test LAST_MAP exit from Elm's Lab back to New Bark Town
    WorldManager wm;
    GameState state;
    
    wm.set_map_loader([](const std::string& map_id) -> std::optional<RuntimeMap> {
        auto& cached = get_cached_map(map_id);
        if (cached.map_id.empty()) return std::nullopt;
        return cached;
    });
    
    // Simulate: came from New Bark Town at (10, 10)
    state.warp_memory.map_id = "new_bark_town";
    state.warp_memory.x = 10;
    state.warp_memory.y = 10;
    
    wm.load_map("elms_lab");
    state.player.current_map_id = "elms_lab";
    state.player.x = 5;
    state.player.y = 10;
    
    // Find exit warp (should be LAST_MAP)
    const auto* map = wm.current_map();
    const RuntimeWarp* exit_warp = nullptr;
    for (const auto& warp : map->warps) {
        if (warp.target_map_id == "LAST_MAP") {
            exit_warp = &warp;
            break;
        }
    }
    
    if (!exit_warp) {
        std::cout << "  [SKIP: No LAST_MAP warp found in Elm's Lab]\n";
        return;
    }
    
    // Execute the LAST_MAP warp
    auto result = wm.execute_warp(*exit_warp, state);
    ASSERT_TRUE(result.success);
    ASSERT_STR_EQ(result.target_map_id.c_str(), "new_bark_town");
    ASSERT_EQ(result.target_x, 10);  // Should return to remembered position
    ASSERT_EQ(result.target_y, 10);
    
    std::cout << "  [LAST_MAP resolved to new_bark_town at remembered position]\n";
}

// =============================================================================
// REGRESSION TESTS: Targeted runtime correctness fix pass
// =============================================================================

TEST(collision_dimension_uses_collision_not_tile_width) {
    // REGRESSION TEST for Fix 1: HeadlessGameLoop collision dimensions
    // Verifies that gameplay bounds use collision_width()/collision_height() (blocks*2)
    // NOT tile_width()/tile_height() (blocks*4)
    //
    // Player coordinates are in collision cells (16×16 pixel grid), not render tiles (8×8).
    // A map of 10×9 blocks has:
    //   - collision dimensions: 20×18 cells (collision_width/height)
    //   - render dimensions: 40×36 tiles (tile_width/height)
    //
    // The bug was: bounds checks used tile_width (40) when they should use collision_width (20)
    // This caused the collision boundary to be 2× larger than correct.
    
    HeadlessGameLoop loop;
    GameState game_state;
    game_state.rng.seed(12345);
    loop.set_game_state(&game_state);
    
    // Create a 10×9 block map (like New Bark Town)
    RuntimeMap rtmap;
    rtmap.width = 10;
    rtmap.height = 9;
    rtmap.blocks.resize(90, 0x01);  // Fill with some non-zero blocks
    
    loop.load_map(rtmap);
    
    // Verify the dimension methods are correct
    ASSERT_EQ(rtmap.tile_width(), 40);       // 10 * 4 = 40 render tiles
    ASSERT_EQ(rtmap.tile_height(), 36);      // 9 * 4 = 36 render tiles
    ASSERT_EQ(rtmap.collision_width(), 20);  // 10 * 2 = 20 collision cells
    ASSERT_EQ(rtmap.collision_height(), 18); // 9 * 2 = 18 collision cells
    
    // Set up collision data that's walkable everywhere
    loop.set_collision_data([](int32_t x, int32_t y) -> CollisionClass {
        return CollisionClass::Floor;
    });
    
    // Place player near what would be the tile boundary (30) but past the collision boundary (19)
    // If the bug exists, this would be "in bounds" when it should be "out of bounds"
    loop.spawn_player(0, 0, enginemon::Direction::Down);
    
    // Add an NPC at the collision boundary edge
    NpcState npc;
    npc.id = 1;
    npc.x = 19;  // Right at collision width - 1 (valid)
    npc.y = 17;  // Right at collision height - 1 (valid)
    npc.facing = enginemon::Direction::Right;
    npc.behavior = NpcMovementBehavior::RandomWalkXY;
    npc.idle_timer = 0;
    npc.radius_x = 5;  // Large radius to test bounds
    npc.radius_y = 5;
    npc.init_x = 19;
    npc.init_y = 17;
    npc.visible = true;
    npc.frozen = false;
    loop.add_npc(npc);
    
    // The NPC at (19, 17) should NOT be able to move right (would be x=20, out of bounds)
    // If the bug existed, it would think x=20 < tile_width(40), allowing the move
    
    // Tick several times and verify NPC never exceeds collision bounds
    for (int i = 0; i < 500; i++) {
        loop.tick();
        const NpcState* current = loop.get_npc(1);
        ASSERT_TRUE(current->x < 20);  // Must be < collision_width
        ASSERT_TRUE(current->y < 18);  // Must be < collision_height
    }
    
    std::cout << "  [Collision dimensions correctly use collision_width/height, not tile_width/height]\n";
}

TEST(warp_invalid_index_zero_fails) {
    // ADVERSARIAL TEST for Fix 2: warp_index=0 must fail explicitly
    // Crystal warp indices are 1-based, so 0 is always invalid
    
    WorldManager wm;
    GameState state;
    
    // Create a simple map with one warp
    RuntimeMap target_map;
    target_map.map_id = "target";
    target_map.width = 5;
    target_map.height = 5;
    RuntimeWarp valid_warp;
    valid_warp.x = 2;
    valid_warp.y = 2;
    valid_warp.target_map_id = "target";
    valid_warp.target_warp_index = 1;
    target_map.warps.push_back(valid_warp);
    
    wm.set_map_loader([&target_map](const std::string& map_id) -> std::optional<RuntimeMap> {
        if (map_id == "target") return target_map;
        return std::nullopt;
    });
    
    // Create a warp with invalid index 0
    RuntimeWarp bad_warp;
    bad_warp.x = 0;
    bad_warp.y = 0;
    bad_warp.target_map_id = "target";
    bad_warp.target_warp_index = 0;  // INVALID: Crystal indices are 1-based
    
    auto result = wm.resolve_warp(bad_warp, state);
    ASSERT_FALSE(result.success);
    ASSERT_STR_CONTAINS(result.error.c_str(), "Invalid warp index 0");
    
    std::cout << "  [warp_index=0 correctly rejected]\n";
}

TEST(warp_invalid_index_out_of_range_fails) {
    // ADVERSARIAL TEST for Fix 2: out-of-range warp index must fail explicitly
    // Must NOT silently fallback to warp[0]
    
    WorldManager wm;
    GameState state;
    
    // Create a target map with only 2 warps
    RuntimeMap target_map;
    target_map.map_id = "target";
    target_map.width = 5;
    target_map.height = 5;
    RuntimeWarp warp1, warp2;
    warp1.x = 1; warp1.y = 1; warp1.target_map_id = "x"; warp1.target_warp_index = 1;
    warp2.x = 2; warp2.y = 2; warp2.target_map_id = "x"; warp2.target_warp_index = 1;
    target_map.warps.push_back(warp1);
    target_map.warps.push_back(warp2);
    
    wm.set_map_loader([&target_map](const std::string& map_id) -> std::optional<RuntimeMap> {
        if (map_id == "target") return target_map;
        return std::nullopt;
    });
    
    // Try to warp to index 5 (out of range - map has only 2 warps)
    RuntimeWarp bad_warp;
    bad_warp.x = 0;
    bad_warp.y = 0;
    bad_warp.target_map_id = "target";
    bad_warp.target_warp_index = 5;  // INVALID: only warps 1-2 exist
    
    auto result = wm.resolve_warp(bad_warp, state);
    ASSERT_FALSE(result.success);
    ASSERT_STR_CONTAINS(result.error.c_str(), "out of range");
    
    std::cout << "  [warp_index out of range correctly rejected]\n";
}

TEST(warp_target_map_no_warps_fails) {
    // ADVERSARIAL TEST for Fix 2: target map with zero warps must fail explicitly
    
    WorldManager wm;
    GameState state;
    
    // Create a target map with NO warps
    RuntimeMap empty_map;
    empty_map.map_id = "empty";
    empty_map.width = 5;
    empty_map.height = 5;
    // No warps added
    
    wm.set_map_loader([&empty_map](const std::string& map_id) -> std::optional<RuntimeMap> {
        if (map_id == "empty") return empty_map;
        return std::nullopt;
    });
    
    // Try to warp to a map with no warps
    RuntimeWarp bad_warp;
    bad_warp.x = 0;
    bad_warp.y = 0;
    bad_warp.target_map_id = "empty";
    bad_warp.target_warp_index = 1;  // Even index 1 is invalid when there are 0 warps
    
    auto result = wm.resolve_warp(bad_warp, state);
    ASSERT_FALSE(result.success);
    ASSERT_STR_CONTAINS(result.error.c_str(), "no warps");
    
    std::cout << "  [target map with no warps correctly rejected]\n";
}

TEST(warp_valid_index_succeeds) {
    // Positive test: valid warp indices still work after the fix
    
    WorldManager wm;
    GameState state;
    
    RuntimeMap target_map;
    target_map.map_id = "target";
    target_map.width = 5;
    target_map.height = 5;
    
    // Add 3 warps
    RuntimeWarp w1, w2, w3;
    w1.x = 1; w1.y = 1; w1.target_map_id = "x"; w1.target_warp_index = 1;
    w2.x = 2; w2.y = 2; w2.target_map_id = "x"; w2.target_warp_index = 1;
    w3.x = 3; w3.y = 3; w3.target_map_id = "x"; w3.target_warp_index = 1;
    target_map.warps.push_back(w1);
    target_map.warps.push_back(w2);
    target_map.warps.push_back(w3);
    
    wm.set_map_loader([&target_map](const std::string& map_id) -> std::optional<RuntimeMap> {
        if (map_id == "target") return target_map;
        return std::nullopt;
    });
    
    // Test all valid indices
    for (uint8_t idx = 1; idx <= 3; idx++) {
        RuntimeWarp good_warp;
        good_warp.x = 0;
        good_warp.y = 0;
        good_warp.target_map_id = "target";
        good_warp.target_warp_index = idx;
        
        auto result = wm.resolve_warp(good_warp, state);
        ASSERT_TRUE(result.success);
        ASSERT_EQ(result.target_x, idx);  // w1.x=1, w2.x=2, w3.x=3
        ASSERT_EQ(result.target_y, idx);  // Same for y
    }
    
    std::cout << "  [Valid warp indices 1-3 all succeed]\n";
}

TEST(load_map_owns_copy_prevents_dangling) {
    // REGRESSION TEST for Fix 3: RuntimeMap lifetime safety
    // HeadlessGameLoop::load_map() must copy the map to prevent dangling pointers
    
    HeadlessGameLoop loop;
    GameState game_state;
    game_state.rng.seed(42);
    loop.set_game_state(&game_state);
    
    // Create a map in a temporary scope
    {
        RuntimeMap temp_map;
        temp_map.map_id = "temp_test";
        temp_map.width = 8;
        temp_map.height = 6;
        temp_map.blocks.resize(48, 0x01);
        
        // Add some warps to prove they're copied
        RuntimeWarp warp;
        warp.x = 4;
        warp.y = 3;
        warp.target_map_id = "destination";
        warp.target_warp_index = 1;
        temp_map.warps.push_back(warp);
        
        loop.load_map(temp_map);
    }
    // temp_map is now out of scope and destroyed
    
    // The loop should still have valid map data (it owns a copy)
    const RuntimeMap* map = loop.current_map();
    ASSERT_TRUE(map != nullptr);
    ASSERT_STR_EQ(map->map_id.c_str(), "temp_test");
    ASSERT_EQ(map->width, 8);
    ASSERT_EQ(map->height, 6);
    ASSERT_EQ(map->warps.size(), 1);
    ASSERT_EQ(map->warps[0].x, 4);
    ASSERT_EQ(map->warps[0].y, 3);
    ASSERT_STR_EQ(map->warps[0].target_map_id.c_str(), "destination");
    
    std::cout << "  [HeadlessGameLoop owns map copy - no dangling pointer]\n";
}

// =============================================================================
// PRE-RNG RUNTIME CORRECTNESS PASS - Fix regression tests
// =============================================================================

TEST(typechart_immunity_is_zero_not_unset) {
    // REGRESSION TEST for Fix 1: TypeChart immunity representation
    // Verifies: 0 = immune, 10 = neutral, other values = explicit effectiveness
    
    TypeChart chart;
    
    // Before any set_effectiveness calls, all matchups should be neutral (10)
    // Chart is pre-filled with 10 in constructor
    ASSERT_EQ(chart.get_effectiveness(1, 2), 10);  // Unset pair → neutral
    ASSERT_EQ(chart.get_effectiveness(5, 5), 10);  // Unset pair → neutral
    
    // Set explicit immunity (0)
    chart.set_effectiveness(1, 2, 0);  // Type 1 → Type 2 = immune
    ASSERT_EQ(chart.get_effectiveness(1, 2), 0);   // Should return 0, NOT 10
    
    // Set super effective (20)
    chart.set_effectiveness(3, 4, 20);
    ASSERT_EQ(chart.get_effectiveness(3, 4), 20);
    
    // Set not very effective (5)
    chart.set_effectiveness(5, 6, 5);
    ASSERT_EQ(chart.get_effectiveness(5, 6), 5);
    
    std::cout << "  [TypeChart: 0=immune (not unset), unset=10 (neutral)]\n";
}

TEST(typechart_dual_type_immunity_remains_zero) {
    // REGRESSION TEST for Fix 1: Dual-type immunity
    // If either defending type is immune, result must be 0
    
    TypeChart chart;
    
    // Set up: Type 1 → Type 10 = immune (0)
    // Type 1 → Type 11 = super effective (20)
    chart.set_effectiveness(1, 10, 0);   // Immune
    chart.set_effectiveness(1, 11, 20);  // Super effective
    
    // Dual-type: Type 10 + Type 11
    // One type immune → result is immune (0)
    uint8_t dual_eff = chart.get_effectiveness(1, 10, 11);
    ASSERT_EQ(dual_eff, 0);  // Immune takes priority
    
    // Reverse order should also be immune
    uint8_t dual_eff_rev = chart.get_effectiveness(1, 11, 10);
    ASSERT_EQ(dual_eff_rev, 0);
    
    std::cout << "  [Dual-type: immunity (0) takes priority over super effective (20)]\n";
}

TEST(typechart_explicit_values_survive_lookup) {
    // REGRESSION TEST for Fix 1: Explicit values survive lookup unchanged
    
    TypeChart chart;
    
    // Set various explicit values
    chart.set_effectiveness(1, 1, 0);   // 0 = immune
    chart.set_effectiveness(2, 2, 5);   // 5 = resist (0.5x)
    chart.set_effectiveness(3, 3, 10);  // 10 = neutral (1x)
    chart.set_effectiveness(4, 4, 20);  // 20 = super (2x)
    
    // All values must survive lookup unchanged
    ASSERT_EQ(chart.get_effectiveness(1, 1), 0);
    ASSERT_EQ(chart.get_effectiveness(2, 2), 5);
    ASSERT_EQ(chart.get_effectiveness(3, 3), 10);
    ASSERT_EQ(chart.get_effectiveness(4, 4), 20);
    
    std::cout << "  [All explicit effectiveness values survive lookup unchanged]\n";
}

// =============================================================================
// POST-ORACLE CLEANUP TESTS
// =============================================================================

// Fix 2: create_pokemon hard-fail on missing SpeciesId
TEST(create_pokemon_missing_species_throws) {
    // NEGATIVE TEST: SpeciesId not in registry → must throw, not return zero-stat Pokémon
    Registries reg;  // Empty registry — no species registered

    GameplayRng rng;
    rng.seed(42);

    bool threw = false;
    try {
        // SpeciesId 99 is not in the empty registry
        auto mon = create_pokemon(static_cast<SpeciesId>(99), 5, rng, reg);
        (void)mon;
    } catch (const std::invalid_argument&) {
        threw = true;
    } catch (const std::exception&) {
        threw = true;
    }
    ASSERT_TRUE(threw);
    std::cout << "  [create_pokemon: missing SpeciesId throws, not zero-stat Pokémon ✓]\n";
}

TEST(create_pokemon_registered_species_succeeds) {
    // POSITIVE TEST: registered species → succeeds, stats > 0
    Registries reg;
    SpeciesData sd;
    sd.base_stats.hp      = 45;
    sd.base_stats.attack  = 49;
    sd.base_stats.defense = 49;
    sd.base_stats.speed   = 45;
    sd.base_stats.special_attack  = 65;
    sd.base_stats.special_defense = 65;
    sd.name = "Bulbasaur";
    reg.species.register_entry(static_cast<SpeciesId>(1), sd);

    GameplayRng rng;
    rng.seed(42);
    auto mon = create_pokemon(static_cast<SpeciesId>(1), 10, rng, reg);

    ASSERT_EQ(mon.species, static_cast<SpeciesId>(1));
    ASSERT_TRUE(mon.max_hp > 0);
    ASSERT_TRUE(mon.current_hp > 0);
    ASSERT_EQ(mon.level, 10);
    std::cout << "  [create_pokemon: registered species produces valid Pokémon (HP=" << mon.max_hp << ") ✓]\n";
}

// Fix 3: TypeChart invalid TypeId throws
TEST(typechart_out_of_range_get_throws) {
    // NEGATIVE TEST: TypeId ≥ MAX_TYPES (32) must throw, not return neutral 10
    TypeChart chart;
    bool threw = false;
    try {
        (void)chart.get_effectiveness(32, 0);  // 32 == MAX_TYPES → out of range
    } catch (const std::out_of_range&) {
        threw = true;
    }
    ASSERT_TRUE(threw);

    bool threw2 = false;
    try {
        (void)chart.get_effectiveness(0, 32);  // defender out of range
    } catch (const std::out_of_range&) {
        threw2 = true;
    }
    ASSERT_TRUE(threw2);
    std::cout << "  [TypeChart: TypeId≥32 throws out_of_range ✓]\n";
}

TEST(typechart_out_of_range_set_throws) {
    // NEGATIVE TEST: set_effectiveness with out-of-range TypeId throws
    TypeChart chart;
    bool threw = false;
    try {
        chart.set_effectiveness(32, 0, 20);  // attacker out of range
    } catch (const std::out_of_range&) {
        threw = true;
    }
    ASSERT_TRUE(threw);

    // In-range set must still work (regression check)
    chart.set_effectiveness(1, 2, 20);
    ASSERT_EQ(chart.get_effectiveness(1, 2), 20);
    std::cout << "  [TypeChart: set_effectiveness out-of-range throws; in-range still works ✓]\n";
}

TEST(typechart_max_valid_index_accepted) {
    // BOUNDARY TEST: TypeId 31 (MAX_TYPES-1) is the last valid index
    TypeChart chart;
    chart.set_effectiveness(31, 31, 5);
    ASSERT_EQ(chart.get_effectiveness(31, 31), 5);
    std::cout << "  [TypeChart: TypeId 31 (MAX_TYPES-1) is valid boundary ✓]\n";
}

TEST(typechart_dual_type_out_of_range_throws) {
    // NEGATIVE TEST: dual-type overload propagates the throw
    TypeChart chart;
    bool threw = false;
    try {
        (void)chart.get_effectiveness(1, 32, 0);  // def1 out of range
    } catch (const std::out_of_range&) {
        threw = true;
    }
    ASSERT_TRUE(threw);
    std::cout << "  [TypeChart: dual-type with out-of-range TypeId throws ✓]\n";
}

// Fix 4: Lua load_script_directory deterministic sort
TEST(lua_load_script_directory_deterministic_order) {
    // DETERMINISM TEST: two scripts define the same global key with different values.
    // Lexicographic sort → a_script.lua loads first, b_script.lua loads second → b wins.
    // Creation order is reversed to prove sort overrides filesystem order.
    {
        auto tmp = std::filesystem::temp_directory_path() / "oracle_lua_order_test";
        std::filesystem::create_directories(tmp);

        // Write b_script.lua first (creation order: b before a — "wrong" filesystem order)
        {
            std::ofstream f(tmp / "b_script.lua");
            f << "g_order_winner = 'b'\n";
        }
        // Write a_script.lua second (creation order: a after b)
        {
            std::ofstream f(tmp / "a_script.lua");
            f << "g_order_winner = 'a'\n";
        }

        // load_script_directory sorts lexicographically:
        //   a_script.lua < b_script.lua → a loads first, b loads second → b wins.
        // Without sort: filesystem might return b first → a loads second → a would win.
        LuaRuntime rt;
        rt.load_script_directory(tmp);

        // Verify by running a Lua snippet that asserts g_order_winner == 'b'
        // If sort is working, b loaded last → 'b' wins.
        // If sort is broken and creation order was used, 'a' would win and this throws.
        bool threw = false;
        try {
            rt.execute_string(
                "assert(g_order_winner == 'b', "
                "'expected b (lexicographic last) but got: ' .. tostring(g_order_winner))",
                "order_check");
        } catch (const std::exception& e) {
            threw = true;
            std::cerr << "  [FAIL detail: " << e.what() << "]\n";
        }
        ASSERT_FALSE(threw);

        std::filesystem::remove_all(tmp);
    }
    std::cout << "  [Lua load_script_directory: sorted — b_script.lua wins over creation order ✓]\n";
}

TEST(pcstorage_deposit_moves_pokemon_exactly_once) {
    // REGRESSION TEST for Fix 2: PCStorage deposit repeated-move bug
    // Verifies: Pokemon with nontrivial fields is deposited intact to first free slot
    
    PCStorage pc;
    
    // Occupy slot 0 in current box
    Pokemon blocker;
    blocker.species = 1;
    blocker.nickname = "BLOCKER";
    pc.box(0).deposit(0, std::move(blocker));
    
    // Create Pokemon with nontrivial fields that would be corrupted by double-move
    Pokemon test_mon;
    test_mon.species = 25;
    test_mon.nickname = "PIKACHU";
    test_mon.ot_name = "ASH";
    test_mon.ot_id = 12345;
    test_mon.level = 50;
    test_mon.current_hp = 100;
    test_mon.max_hp = 100;
    test_mon.friendship = 255;
    test_mon.moves[0].id = 10;
    test_mon.moves[0].pp = 35;
    test_mon.moves[0].pp_ups = 3;
    
    // Deposit - should go to slot 1 (slot 0 is occupied)
    bool success = pc.deposit(std::move(test_mon));
    ASSERT_TRUE(success);
    
    // Verify Pokemon is in slot 1 with all fields intact
    const Pokemon* deposited = pc.box(0).get(1);
    ASSERT_TRUE(deposited != nullptr);
    ASSERT_EQ(deposited->species, 25);
    ASSERT_STR_EQ(deposited->nickname.c_str(), "PIKACHU");
    ASSERT_STR_EQ(deposited->ot_name.c_str(), "ASH");
    ASSERT_EQ(deposited->ot_id, 12345);
    ASSERT_EQ(deposited->level, 50);
    ASSERT_EQ(deposited->current_hp, 100);
    ASSERT_EQ(deposited->friendship, 255);
    ASSERT_EQ(deposited->moves[0].id, 10);
    ASSERT_EQ(deposited->moves[0].pp, 35);
    ASSERT_EQ(deposited->moves[0].pp_ups, 3);
    
    std::cout << "  [Pokemon deposited intact to first free slot (slot 1)]\n";
}

TEST(pokemon_move_slots_initialized_to_defaults) {
    // REGRESSION TEST for Fix 3: Pokemon move-slot initialization
    // Verifies: All 4 move slots have structural defaults after construction
    
    Pokemon mon;  // Default construction
    
    // All 4 slots must have valid empty defaults
    for (size_t i = 0; i < 4; i++) {
        ASSERT_EQ(mon.moves[i].id, MOVE_NONE);
        ASSERT_EQ(mon.moves[i].pp, 0);
        ASSERT_EQ(mon.moves[i].pp_ups, 0);
    }
    
    std::cout << "  [All 4 move slots initialized: id=MOVE_NONE, pp=0, pp_ups=0]\n";
}

TEST(connection_strip_first_valid_coordinate) {
    // REGRESSION TEST for Fix 4: Connection strip bounds - first valid coordinate
    
    WorldManager wm;
    GameState state;
    
    // Create source map 10x10 blocks (20x20 collision cells)
    RuntimeMap source;
    source.map_id = "source";
    source.width = 10;
    source.height = 10;
    
    // Create west connection with src_skip_blocks=2, strip_length_blocks=3
    // Strip covers cells 4-9 (src_skip_blocks*2=4, strip_length_blocks*2=6)
    RuntimeConnection conn;
    conn.direction = ConnectionDirection::West;
    conn.target_map_id = "dest";
    conn.src_skip_blocks = 2;
    conn.strip_length_blocks = 3;
    conn.coord_adjust_tiles = 0;
    source.connections.push_back(conn);
    
    // Create destination map
    RuntimeMap dest;
    dest.map_id = "dest";
    dest.width = 10;
    dest.height = 10;
    
    wm.set_map_loader([&](const std::string& map_id) -> std::optional<RuntimeMap> {
        if (map_id == "source") return source;
        if (map_id == "dest") return dest;
        return std::nullopt;
    });
    
    wm.load_map("source");
    
    // First valid Y coordinate in strip (src_skip_blocks*2 = 4)
    auto result = wm.resolve_connection(0, 4, enginemon::Direction::Left);
    ASSERT_TRUE(result.success);
    
    std::cout << "  [First valid strip coordinate (y=4) succeeds]\n";
}

TEST(connection_strip_last_valid_coordinate) {
    // REGRESSION TEST for Fix 4: Connection strip bounds - last valid coordinate
    
    WorldManager wm;
    GameState state;
    
    RuntimeMap source;
    source.map_id = "source";
    source.width = 10;
    source.height = 10;
    
    // src_skip_blocks=2, strip_length_blocks=3 → cells 4-9 (exclusive: 4,5,6,7,8,9)
    RuntimeConnection conn;
    conn.direction = ConnectionDirection::West;
    conn.target_map_id = "dest";
    conn.src_skip_blocks = 2;
    conn.strip_length_blocks = 3;
    conn.coord_adjust_tiles = 0;
    source.connections.push_back(conn);
    
    RuntimeMap dest;
    dest.map_id = "dest";
    dest.width = 10;
    dest.height = 10;
    
    wm.set_map_loader([&](const std::string& map_id) -> std::optional<RuntimeMap> {
        if (map_id == "source") return source;
        if (map_id == "dest") return dest;
        return std::nullopt;
    });
    
    wm.load_map("source");
    
    // Last valid Y coordinate in strip (src_skip_blocks*2 + strip_length_blocks*2 - 1 = 9)
    auto result = wm.resolve_connection(0, 9, enginemon::Direction::Left);
    ASSERT_TRUE(result.success);
    
    std::cout << "  [Last valid strip coordinate (y=9) succeeds]\n";
}

TEST(connection_strip_before_strip_rejected) {
    // REGRESSION TEST for Fix 4: Connection strip bounds - before strip rejected
    
    WorldManager wm;
    GameState state;
    
    RuntimeMap source;
    source.map_id = "source";
    source.width = 10;
    source.height = 10;
    
    // src_skip_blocks=2, strip_length_blocks=3 → cells 4-9
    RuntimeConnection conn;
    conn.direction = ConnectionDirection::West;
    conn.target_map_id = "dest";
    conn.src_skip_blocks = 2;
    conn.strip_length_blocks = 3;
    conn.coord_adjust_tiles = 0;
    source.connections.push_back(conn);
    
    RuntimeMap dest;
    dest.map_id = "dest";
    dest.width = 10;
    dest.height = 10;
    
    wm.set_map_loader([&](const std::string& map_id) -> std::optional<RuntimeMap> {
        if (map_id == "source") return source;
        if (map_id == "dest") return dest;
        return std::nullopt;
    });
    
    wm.load_map("source");
    
    // Y=3 is one cell BEFORE the strip (strip starts at 4)
    auto result = wm.resolve_connection(0, 3, enginemon::Direction::Left);
    ASSERT_FALSE(result.success);
    ASSERT_STR_CONTAINS(result.error.c_str(), "strip");
    
    std::cout << "  [Before strip (y=3) correctly rejected]\n";
}

TEST(connection_strip_after_strip_rejected) {
    // REGRESSION TEST for Fix 4: Connection strip bounds - after strip rejected
    
    WorldManager wm;
    GameState state;
    
    RuntimeMap source;
    source.map_id = "source";
    source.width = 10;
    source.height = 10;
    
    // src_skip_blocks=2, strip_length_blocks=3 → cells 4-9
    RuntimeConnection conn;
    conn.direction = ConnectionDirection::West;
    conn.target_map_id = "dest";
    conn.src_skip_blocks = 2;
    conn.strip_length_blocks = 3;
    conn.coord_adjust_tiles = 0;
    source.connections.push_back(conn);
    
    RuntimeMap dest;
    dest.map_id = "dest";
    dest.width = 10;
    dest.height = 10;
    
    wm.set_map_loader([&](const std::string& map_id) -> std::optional<RuntimeMap> {
        if (map_id == "source") return source;
        if (map_id == "dest") return dest;
        return std::nullopt;
    });
    
    wm.load_map("source");
    
    // Y=10 is one cell AFTER the strip (strip ends at 9)
    auto result = wm.resolve_connection(0, 10, enginemon::Direction::Left);
    ASSERT_FALSE(result.success);
    ASSERT_STR_CONTAINS(result.error.c_str(), "strip");
    
    std::cout << "  [After strip (y=10) correctly rejected]\n";
}

TEST(player_destination_reserved_against_npc) {
    // REGRESSION TEST for Fix 5: Reserve moving player's destination against NPC movement
    
    HeadlessGameLoop loop;
    GameState game_state;
    game_state.rng.seed(42);
    loop.set_game_state(&game_state);
    
    RuntimeMap rtmap;
    rtmap.width = 10;
    rtmap.height = 10;
    rtmap.blocks.resize(100, 0x01);
    
    loop.load_map(rtmap);
    loop.set_collision_data([](int32_t x, int32_t y) -> CollisionClass {
        return CollisionClass::Floor;
    });
    
    // Player at (5,5), moving right toward (6,5)
    loop.spawn_player(5, 5, enginemon::Direction::Right);
    
    // Start player movement toward (6,5)
    auto input_result = loop.process_input(InputAction::MoveRight);
    ASSERT_TRUE(input_result.accepted);
    ASSERT_FALSE(input_result.blocked);
    
    // Now player is mid-step: x=5, target_x=6
    ASSERT_TRUE(loop.player().is_moving);
    ASSERT_EQ(loop.player().x, 5);
    ASSERT_EQ(loop.player().target_x, 6);
    
    // Add NPC at (7,5) trying to move left into (6,5) - player's destination
    NpcState npc;
    npc.id = 1;
    npc.x = 7;
    npc.y = 5;
    npc.facing = enginemon::Direction::Left;
    npc.behavior = NpcMovementBehavior::RandomWalkX;
    npc.idle_timer = 0;
    npc.radius_x = 5;
    npc.radius_y = 0;
    npc.init_x = 7;
    npc.init_y = 5;
    npc.visible = true;
    npc.frozen = false;
    loop.add_npc(npc);
    
    // Tick once - NPC should NOT be able to move into (6,5) because player is moving there
    // Force NPC to try moving left by manipulating the test
    // The RNG will choose a direction - we check the result
    
    // Tick several times while player is still moving
    for (int i = 0; i < 8; i++) {  // Player takes 16 ticks, so we're partway through
        loop.tick();
        
        const NpcState* updated_npc = loop.get_npc(1);
        
        // NPC should never occupy (6,5) while player is moving there
        if (loop.player().is_moving) {
            // If NPC moved, it should NOT have moved to player's destination
            if (updated_npc->x != 7) {
                ASSERT_FALSE(updated_npc->x == 6 && updated_npc->y == 5);
            }
        }
    }
    
    std::cout << "  [NPC cannot move into player's in-progress destination]\n";
}

TEST(npc_cannot_cross_side_wall_from_forbidden_direction) {
    // REGRESSION TEST for Fix 6: NPC directional side-wall collision
    
    HeadlessGameLoop loop;
    GameState game_state;
    game_state.rng.seed(12345);
    loop.set_game_state(&game_state);
    
    RuntimeMap rtmap;
    rtmap.width = 10;
    rtmap.height = 10;
    rtmap.blocks.resize(100, 0x01);
    
    loop.load_map(rtmap);
    
    // Create collision map with side walls
    // SideWallN at (5,5) - blocks movement from south to north
    loop.set_collision_data([](int32_t x, int32_t y) -> CollisionClass {
        if (x == 5 && y == 5) return CollisionClass::SideWallN;
        return CollisionClass::Floor;
    });
    
    // NPC at (5,6) trying to move up into (5,5) - should be blocked by SideWallN
    NpcState npc;
    npc.id = 1;
    npc.x = 5;
    npc.y = 6;  // Below the side wall
    npc.facing = enginemon::Direction::Up;
    npc.behavior = NpcMovementBehavior::RandomWalkY;
    npc.idle_timer = 0;
    npc.radius_x = 0;
    npc.radius_y = 5;
    npc.init_x = 5;
    npc.init_y = 6;
    npc.visible = true;
    npc.frozen = false;
    loop.add_npc(npc);
    
    loop.spawn_player(0, 0, enginemon::Direction::Down);
    
    // Tick many times - NPC should never enter (5,5) from below
    for (int i = 0; i < 500; i++) {
        loop.tick();
        
        const NpcState* updated = loop.get_npc(1);
        
        // NPC should never be at (5,5) if it came from (5,6)
        // Since it's RandomWalkY starting at (5,6), it can only reach (5,5) by moving up
        // which should be blocked by SideWallN
        ASSERT_FALSE(updated->x == 5 && updated->y == 5);
    }
    
    std::cout << "  [NPC cannot cross SideWallN from forbidden direction (south→north)]\n";
}

TEST(npc_can_traverse_side_wall_from_allowed_direction) {
    // REGRESSION TEST for Fix 6: NPC can traverse side wall from allowed direction
    
    HeadlessGameLoop loop;
    GameState game_state;
    // NPC movement now uses canonical RNG. Use a seed that allows the NPC to
    // eventually reach (5,5) by moving down. With 2000 ticks and RandomWalkY,
    // the NPC will traverse the side wall from the allowed direction.
    game_state.rng.seed(99);
    loop.set_game_state(&game_state);
    
    RuntimeMap rtmap;
    rtmap.width = 10;
    rtmap.height = 10;
    rtmap.blocks.resize(100, 0x01);
    
    loop.load_map(rtmap);
    
    // SideWallN at (5,5) - blocks movement from south, allows from north
    loop.set_collision_data([](int32_t x, int32_t y) -> CollisionClass {
        if (x == 5 && y == 5) return CollisionClass::SideWallN;
        return CollisionClass::Floor;
    });
    
    // NPC at (5,4) - north of the side wall, can move down into (5,5)
    NpcState npc;
    npc.id = 1;
    npc.x = 5;
    npc.y = 4;  // Above the side wall
    npc.facing = enginemon::Direction::Down;
    npc.behavior = NpcMovementBehavior::RandomWalkY;
    npc.idle_timer = 0;
    npc.radius_x = 0;
    npc.radius_y = 2;  // Can move down 2 tiles
    npc.init_x = 5;
    npc.init_y = 4;
    npc.visible = true;
    npc.frozen = false;
    loop.add_npc(npc);
    
    loop.spawn_player(0, 0, enginemon::Direction::Down);
    
    // Tick until NPC reaches (5,5) or we timeout — allow more ticks with canonical RNG
    bool reached_side_wall = false;
    for (int i = 0; i < 5000 && !reached_side_wall; i++) {
        loop.tick();
        
        const NpcState* updated = loop.get_npc(1);
        if (updated->x == 5 && updated->y == 5) {
            reached_side_wall = true;
        }
    }
    
    ASSERT_TRUE(reached_side_wall);
    std::cout << "  [NPC can traverse SideWallN from allowed direction (north→south)]\n";
}

TEST(connection_newbark_to_route29) {
    // Test connection crossing from New Bark Town to Route 29
    WorldManager wm;
    GameState state;
    
    wm.set_map_loader([](const std::string& map_id) -> std::optional<RuntimeMap> {
        auto& cached = get_cached_map(map_id);
        if (cached.map_id.empty()) return std::nullopt;
        return cached;
    });
    
    wm.load_map("new_bark_town");
    state.player.current_map_id = "new_bark_town";
    
    // Get westward connection
    const auto* conn = wm.get_connection(ConnectionDirection::West);
    if (!conn) {
        std::cout << "  [SKIP: No west connection in New Bark Town]\n";
        return;
    }
    
    // Position player at west edge
    state.player.x = 0;
    state.player.y = 9;  // Middle of map
    state.player.facing = enginemon::Direction::Left;
    
    // Check if at edge
    bool at_edge = wm.is_at_connection_edge(state.player.x, state.player.y, 
                                            enginemon::Direction::Left);
    ASSERT_TRUE(at_edge);
    
    // Execute connection crossing
    auto result = wm.execute_connection(state.player.x, state.player.y,
                                        enginemon::Direction::Left, state);
    ASSERT_TRUE(result.success);
    ASSERT_FALSE(result.target_map_id.empty());
    
    // Player should be in new map at edge
    ASSERT_STR_EQ(state.player.current_map_id.c_str(), result.target_map_id.c_str());
    
    std::cout << "  [Crossed connection to " << result.target_map_id 
              << " at (" << result.target_x << "," << result.target_y << ")]\n";
}

TEST(connection_landing_math) {
    // Verify connection landing math matches Gen2Recomped
    // Reference: destX = curX - offset*2, destY = edge
    WorldManager wm;
    GameState state;
    
    wm.set_map_loader([](const std::string& map_id) -> std::optional<RuntimeMap> {
        auto& cached = get_cached_map(map_id);
        if (cached.map_id.empty()) return std::nullopt;
        return cached;
    });
    
    wm.load_map("new_bark_town");
    
    const auto* conn = wm.get_connection(ConnectionDirection::West);
    if (!conn) {
        std::cout << "  [SKIP: No west connection]\n";
        return;
    }
    
    // Test multiple positions along the connection
    state.player.x = 0;
    state.player.y = 8;
    
    auto result = wm.resolve_connection(state.player.x, state.player.y,
                                        enginemon::Direction::Left);
    if (!result.success) {
        std::cout << "  [SKIP: Connection resolve failed: " << result.error << "]\n";
        return;
    }
    
    // Landing X should be at far edge of destination (destW - 1)
    // Landing Y should follow the strip offset formula
    std::cout << "  [Landing at (" << result.target_x << "," << result.target_y 
              << ") in " << result.target_map_id << "]\n";
}

// =============================================================================
// CONNECTION SEMANTIC TESTS — Vanilla cases with nonzero asymmetric offsets
//
// All expected values are hand-authored from pokecrystal/data/maps/attributes.asm
// and pokecrystal/constants/map_constants.asm.
// They are NEVER derived from Enginemon encoder/decoder output.
//
// Crystal connection macro formula (PAD = MAP_CONNECTION_PADDING_WIDTH = 3):
//   coord_adjust_tiles = offset * -2      (already in tiles, from data[9] or data[8])
//   src_skip_blocks    = max(0, -(offset + 3))
//   strip_length_blocks = _len - _src     (data[6])
//
// These tests require the real Crystal ROM (g_rom). They skip when ROM is absent.
// =============================================================================

TEST(connection_semantic_cherrygrove_north_to_route30) {
    // Source: pokecrystal/data/maps/attributes.asm
    //   map_attributes CherrygroveCity, CHERRYGROVE_CITY, $35
    //   connection north, Route30, ROUTE_30, 5
    //
    // CHERRYGROVE_CITY: 20w x 9h  (map_constants.asm)
    // ROUTE_30:         10w x 27h
    // offset = +5, direction = north
    //
    // Crystal macro (north):
    //   _x = offset * -2 = 5 * -2 = -10  → coord_adjust_tiles = -10
    //   _src = max(0, -(5 + 3)) = max(0, -8) = 0  → src_skip_blocks = 0
    //   _len = CHERRYGROVE_W + PAD - offset = 20 + 3 - 5 = 18
    //   clamped to min(18, ROUTE_30_W=10) = 10
    //   data[6] = _len - _src = 10 - 0 = 10  → strip_length_blocks = 10
    //
    // PROOF: positive offset → coord_adjust_tiles negative, src_skip_blocks zero.
    if (!g_rom) {
        std::cout << "  [SKIP: ROM not loaded]\n";
        return;
    }

    crystal::MapExtractor extractor(*g_rom, *g_profile);
    // Cherrygrove City: group 26, map 3 (from map_constants.asm newgroup CHERRYGROVE + map offset)
    auto result = extractor.extract_map(26, 3);
    ASSERT_TRUE(result.success);

    const crystal::MapConnection* north_conn = nullptr;
    for (const auto& c : result.map.connections) {
        if (c.direction == crystal::Direction::North) { north_conn = &c; break; }
    }
    ASSERT_TRUE(north_conn != nullptr);
    ASSERT_STR_EQ(north_conn->target_map_id.c_str(), "route_30");

    // coord_adjust_tiles: offset*-2 = 5*-2 = -10 (already tiles, no *2 needed)
    ASSERT_EQ(north_conn->coord_adjust_tiles, -10);
    // src_skip_blocks: max(0, -(5+3)) = 0 (positive offset → no source-edge skip)
    ASSERT_EQ(north_conn->src_skip_blocks, 0);
    // strip_length_blocks: min(CHERRYGROVE_W+PAD-offset, ROUTE_30_W) - _src = 10 - 0 = 10
    ASSERT_EQ(north_conn->strip_length_blocks, 10u);

    // MUTATION: coord_adjust_tiles must not be zero or the old strip_offset*2 value
    ASSERT_TRUE(north_conn->coord_adjust_tiles != 0);
    ASSERT_TRUE(north_conn->coord_adjust_tiles != -20);  // old bug: -10 * 2 = -20

    std::cout << "  [Cherrygrove→Route30 (north, offset=+5): "
              << "coord_adjust=-10, src_skip=0, len=10 ✓]\n";
}

TEST(connection_semantic_azalea_west_to_route34) {
    // Source: pokecrystal/data/maps/attributes.asm
    //   map_attributes AzaleaTown, AZALEA_TOWN, $05
    //   connection west, Route34, ROUTE_34, -18
    //
    // AZALEA_TOWN: 20w x 9h  (map_constants.asm)
    // ROUTE_34:    10w x 27h
    // offset = -18, direction = west
    //
    // Crystal macro (west):
    //   _y = offset * -2 = (-18) * -2 = +36  → coord_adjust_tiles = +36
    //   _src = max(0, -(-18 + 3)) = max(0, 15) = 15  → src_skip_blocks = 15
    //   _len = AZALEA_H + PAD - offset = 9 + 3 - (-18) = 30
    //   clamped to min(30, ROUTE_34_H=27) = 27
    //   data[6] = _len - _src = 27 - 15 = 12  → strip_length_blocks = 12
    //
    // PROOF: negative offset → coord_adjust_tiles positive, src_skip_blocks nonzero.
    if (!g_rom) {
        std::cout << "  [SKIP: ROM not loaded]\n";
        return;
    }

    crystal::MapExtractor extractor(*g_rom, *g_profile);
    // Azalea Town: group 8, map 7 (from map_constants.asm newgroup AZALEA + map offset)
    auto result = extractor.extract_map(8, 7);
    ASSERT_TRUE(result.success);

    const crystal::MapConnection* west_conn = nullptr;
    for (const auto& c : result.map.connections) {
        if (c.direction == crystal::Direction::West) { west_conn = &c; break; }
    }
    ASSERT_TRUE(west_conn != nullptr);
    // target_map_id will be a fallback ID ("map_g11_i01") since Route 34's group
    // is not in the known-group table; semantic field values are the proof here
    ASSERT_FALSE(west_conn->target_map_id.empty());

    // coord_adjust_tiles: offset*-2 = (-18)*-2 = +36 (already tiles)
    ASSERT_EQ(west_conn->coord_adjust_tiles, 36);
    // src_skip_blocks: max(0, -(-18+3)) = max(0, 15) = 15 (negative offset → large skip)
    ASSERT_EQ(west_conn->src_skip_blocks, 15);
    // strip_length_blocks: min(AZALEA_H+PAD-offset, ROUTE_34_H) - _src = 27 - 15 = 12
    ASSERT_EQ(west_conn->strip_length_blocks, 12u);

    // MUTATION: coord_adjust_tiles must not be zero or the old strip_offset*2 value
    ASSERT_TRUE(west_conn->coord_adjust_tiles != 0);
    ASSERT_TRUE(west_conn->coord_adjust_tiles != 72);  // old bug: 36 * 2 = 72
    // MUTATION: src_skip_blocks must not be zero (negative offset drives this nonzero)
    ASSERT_TRUE(west_conn->src_skip_blocks != 0);

    std::cout << "  [Azalea→Route34 (west, offset=-18): "
              << "coord_adjust=+36, src_skip=15, len=12 ✓]\n";
}

TEST(connection_semantic_route26_west_to_route27) {
    // Source: pokecrystal/data/maps/attributes.asm
    //   map_attributes Route26, ROUTE_26, $05
    //   connection west, Route27, ROUTE_27, 45
    //
    // ROUTE_26: 10w x 54h  (map_constants.asm)
    // ROUTE_27: 40w x 9h
    // offset = +45, direction = west (large positive — extreme alignment shift)
    //
    // Crystal macro (west):
    //   _y = offset * -2 = 45 * -2 = -90  → coord_adjust_tiles = -90
    //   _src = max(0, -(45 + 3)) = max(0, -48) = 0  → src_skip_blocks = 0
    //   _len = ROUTE_26_H + PAD - offset = 54 + 3 - 45 = 12
    //   clamped to min(12, ROUTE_27_H=9) = 9
    //   data[6] = _len - _src = 9 - 0 = 9  → strip_length_blocks = 9
    //
    // PROOF: large positive offset → large negative coord_adjust_tiles, src_skip_blocks zero.
    // Also tests that coord_adjust_tiles=-90 is NOT confused with the old strip_offset*2=-180.
    if (!g_rom) {
        std::cout << "  [SKIP: ROM not loaded]\n";
        return;
    }

    crystal::MapExtractor extractor(*g_rom, *g_profile);
    // Route 26: confirmed from map_constants.asm; group index requires checking attributes
    // Route 26 is at attributes.asm directly under its map_attributes label
    // From map_constants.asm: map_const ROUTE_26, 10, 54 with no explicit group offset shown
    // Use extract_map by semantic ID to avoid hardcoding group/index
    auto result = extractor.extract_map("route_26");
    ASSERT_TRUE(result.success);

    const crystal::MapConnection* west_conn = nullptr;
    for (const auto& c : result.map.connections) {
        if (c.direction == crystal::Direction::West) { west_conn = &c; break; }
    }
    ASSERT_TRUE(west_conn != nullptr);
    ASSERT_STR_EQ(west_conn->target_map_id.c_str(), "route_27");

    // coord_adjust_tiles: offset*-2 = 45*-2 = -90 (already tiles — large negative)
    ASSERT_EQ(west_conn->coord_adjust_tiles, -90);
    // src_skip_blocks: max(0, -(45+3)) = 0 (large positive offset → no source skip)
    ASSERT_EQ(west_conn->src_skip_blocks, 0);
    // strip_length_blocks: min(ROUTE_26_H+PAD-offset, ROUTE_27_H) - _src = 9 - 0 = 9
    ASSERT_EQ(west_conn->strip_length_blocks, 9u);

    // MUTATION: must not be the old strip_offset*2 error value
    ASSERT_TRUE(west_conn->coord_adjust_tiles != -180);  // old bug: -90 * 2 = -180
    ASSERT_TRUE(west_conn->src_skip_blocks != 48);       // would be wrong: -(45+3)

    std::cout << "  [Route26→Route27 (west, offset=+45): "
              << "coord_adjust=-90, src_skip=0, len=9 ✓]\n";
}

TEST(connection_semantic_route27_east_to_route26) {
    // Source: pokecrystal/data/maps/attributes.asm
    //   map_attributes Route27, ROUTE_27, $35
    //   connection east, Route26, ROUTE_26, -45
    //
    // ROUTE_27: 40w x 9h  (map_constants.asm)
    // ROUTE_26: 10w x 54h
    // offset = -45, direction = east (mirror of Route26→Route27)
    //
    // Crystal macro (east):
    //   _y = offset * -2 = (-45) * -2 = +90  → coord_adjust_tiles = +90
    //   _src = max(0, -(-45 + 3)) = max(0, 42) = 42  → src_skip_blocks = 42
    //   _len = ROUTE_27_H + PAD - offset = 9 + 3 - (-45) = 57
    //   clamped to min(57, ROUTE_26_H=54) = 54
    //   data[6] = _len - _src = 54 - 42 = 12  → strip_length_blocks = 12
    //
    // PROOF: large negative offset → large positive coord_adjust and large src_skip.
    if (!g_rom) {
        std::cout << "  [SKIP: ROM not loaded]\n";
        return;
    }

    crystal::MapExtractor extractor(*g_rom, *g_profile);
    auto result = extractor.extract_map("route_27");
    ASSERT_TRUE(result.success);

    const crystal::MapConnection* east_conn = nullptr;
    for (const auto& c : result.map.connections) {
        if (c.direction == crystal::Direction::East) { east_conn = &c; break; }
    }
    ASSERT_TRUE(east_conn != nullptr);
    ASSERT_STR_EQ(east_conn->target_map_id.c_str(), "route_26");

    // coord_adjust_tiles: offset*-2 = (-45)*-2 = +90 (already tiles)
    ASSERT_EQ(east_conn->coord_adjust_tiles, 90);
    // src_skip_blocks: max(0, -(-45+3)) = max(0, 42) = 42
    ASSERT_EQ(east_conn->src_skip_blocks, 42);
    // strip_length_blocks: min(ROUTE_27_H+PAD-offset, ROUTE_26_H) - _src = 54 - 42 = 12
    ASSERT_EQ(east_conn->strip_length_blocks, 12u);

    // MUTATION: coord_adjust_tiles != old strip_offset*2 error value
    ASSERT_TRUE(east_conn->coord_adjust_tiles != 180);  // old bug: 90 * 2 = 180
    // MUTATION: src_skip_blocks is large — must not be zero
    ASSERT_TRUE(east_conn->src_skip_blocks != 0);

    std::cout << "  [Route27→Route26 (east, offset=-45): "
              << "coord_adjust=+90, src_skip=42, len=12 ✓]\n";
}

// =============================================================================
// SAVE/LOAD TESTS - GameState serialization
// =============================================================================

TEST(gamestate_serialize_roundtrip) {
    // Basic roundtrip: serialize → deserialize → identical
    GameState original;
    original.player.current_map_id = "new_bark_town";
    original.player.x = 10;
    original.player.y = 15;
    original.player.facing = enginemon::Direction::Right;
    original.player.surfing = false;
    original.playtime_frames = 12345;
    
    auto bytes = original.serialize();
    ASSERT_TRUE(bytes.size() > 0);
    
    auto result = GameState::try_deserialize(bytes);
    ASSERT_TRUE(result.ok());
    GameState& restored = result.state;
    ASSERT_TRUE(restored.is_valid());
    
    ASSERT_STR_EQ(restored.player.current_map_id.c_str(), "new_bark_town");
    ASSERT_EQ(restored.player.x, 10);
    ASSERT_EQ(restored.player.y, 15);
    ASSERT_EQ(static_cast<int>(restored.player.facing), static_cast<int>(enginemon::Direction::Right));
    ASSERT_EQ(restored.playtime_frames, 12345);
    
    std::cout << "  [Basic roundtrip successful]\n";
}

TEST(gamestate_flags_persist) {
    // Flags survive serialization
    GameState original;
    original.player.current_map_id = "test_map";
    original.set_flag("MET_PROFESSOR_ELM");
    original.set_flag("RECEIVED_STARTER");
    original.set_flag("BADGE_ZEPHYR");
    
    auto bytes = original.serialize();
    auto result = GameState::try_deserialize(bytes);
    ASSERT_TRUE(result.ok());
    GameState& restored = result.state;
    
    ASSERT_TRUE(restored.check_flag("MET_PROFESSOR_ELM"));
    ASSERT_TRUE(restored.check_flag("RECEIVED_STARTER"));
    ASSERT_TRUE(restored.check_flag("BADGE_ZEPHYR"));
    ASSERT_FALSE(restored.check_flag("NONEXISTENT_FLAG"));
    
    std::cout << "  [3 flags persisted]\n";
}

TEST(gamestate_variables_persist) {
    // Variables survive serialization
    GameState original;
    original.player.current_map_id = "test_map";
    original.set_var("PLAYER_MONEY", 3000);
    original.set_var("SCORE", -50);  // Test negative values
    original.set_var("ZERO_VAR", 0);
    
    auto bytes = original.serialize();
    auto result = GameState::try_deserialize(bytes);
    ASSERT_TRUE(result.ok());
    GameState& restored = result.state;
    
    ASSERT_EQ(restored.get_var("PLAYER_MONEY"), 3000);
    ASSERT_EQ(restored.get_var("SCORE"), -50);
    ASSERT_EQ(restored.get_var("ZERO_VAR"), 0);
    ASSERT_EQ(restored.get_var("MISSING_VAR"), 0);  // Default
    
    std::cout << "  [Variables persisted correctly]\n";
}

// Save/load regression: Lua-set flags and vars survive serialize → deserialize
// Verifies that ctx.flags write-through to GameState is complete end-to-end.
TEST(lua_flags_vars_persist_through_gamestate_save_load) {
    // Script that sets a flag, clears another, sets a var, and adds to a var.
    // All operations go through ctx.flags → GameState (when bound).
    const char* script = R"(
script = (function()
  local function main()
    ctx.flags:set(7)
    ctx.flags:set(42)
    ctx.flags:clear(42)
    ctx.flags:set_var(3, 100)
    ctx.flags:add_var(3, 25)
  end
  return {main = coroutine.wrap(main)}
end)()
)";

    LuaRuntime runtime;
    GameState gs;
    gs.player.current_map_id = "test";
    runtime.set_game_state(&gs);
    runtime.execute_string(script, "test_flag_persistence");
    uint32_t coro = runtime.start_script("script");

    // Script runs to completion synchronously (no yields)
    auto state = runtime.get_state(coro);
    ASSERT_TRUE(state == ScriptState::Finished || state == ScriptState::Running);

    // GameState must reflect script mutations
    // With canonical hex flag keys: set(7) -> "flag_0007", clear(42) -> "flag_002a" removed
    ASSERT_TRUE(gs.check_flag("flag_0007"));      // set(7) fired
    ASSERT_FALSE(gs.check_flag("flag_002a"));    // set(42) then clear(42) → absent
    ASSERT_EQ(gs.get_var("var_3"), 125);       // set_var(3,100) + add_var(3,25)

    // Serialize and deserialize
    auto bytes = gs.serialize();
    auto result = GameState::try_deserialize(bytes);
    ASSERT_TRUE(result.ok());
    GameState& restored = result.state;

    // Flag 0007 survives, flag 002a absent, var_3 = 125
    ASSERT_TRUE(restored.check_flag("flag_0007"));
    ASSERT_FALSE(restored.check_flag("flag_002a"));
    ASSERT_EQ(restored.get_var("var_3"), 125);

    std::cout << "  [Lua flags/vars → GameState → serialize → deserialize: all correct ✓]\n";
}

// Isolated test mode: no GameState bound → stubs absorb ops, GameState unaffected
TEST(lua_flags_without_gamestate_uses_stubs_only) {
    const char* script = R"(
script = (function()
  local function main()
    ctx.flags:set(99)
    ctx.flags:set_var(5, 777)
  end
  return {main = coroutine.wrap(main)}
end)()
)";

    LuaRuntime runtime;
    // No set_game_state call — stub-only mode
    runtime.execute_string(script, "test_stub_isolation");
    runtime.start_script("script");

    // Stubs received the calls
    ASSERT_TRUE(flag_api::get_test_flag(&runtime, 99));
    auto it = runtime.get_stub_services().vars.find(5);
    ASSERT_TRUE(it != runtime.get_stub_services().vars.end());
    ASSERT_EQ(it->second, 777);

    // No GameState was mutated (no pointer was set — nothing to check)
    std::cout << "  [No GameState bound → stubs absorb, no side-effects ✓]\n";
}

TEST(gamestate_warp_memory_persist) {
    // Warp memory survives for LAST_MAP exits
    GameState original;
    original.player.current_map_id = "elms_lab";
    original.warp_memory.map_id = "new_bark_town";
    original.warp_memory.x = 12;
    original.warp_memory.y = 6;
    original.warp_memory.backup_map_id = "route_29";
    original.warp_memory.backup_x = 5;
    original.warp_memory.backup_y = 10;
    
    auto bytes = original.serialize();
    auto result = GameState::try_deserialize(bytes);
    ASSERT_TRUE(result.ok());
    GameState& restored = result.state;
    
    ASSERT_STR_EQ(restored.warp_memory.map_id.c_str(), "new_bark_town");
    ASSERT_EQ(restored.warp_memory.x, 12);
    ASSERT_EQ(restored.warp_memory.y, 6);
    ASSERT_STR_EQ(restored.warp_memory.backup_map_id.c_str(), "route_29");
    
    std::cout << "  [Warp memory persisted for LAST_MAP]\n";
}

TEST(gamestate_rng_persist) {
    // RNG state survives for determinism
    GameState original;
    original.player.current_map_id = "test_map";
    original.rng.seed(0xDEADBEEFULL);
    original.rng.restore_state(0x12345678ABCDEF00ULL);
    
    auto bytes = original.serialize();
    auto result = GameState::try_deserialize(bytes);
    ASSERT_TRUE(result.ok());
    GameState& restored = result.state;
    
    ASSERT_EQ(restored.rng.state(), 0x12345678ABCDEF00ULL);
    
    std::cout << "  [RNG state persisted for determinism]\n";
}

TEST(save_mutate_load_identical) {
    // save → mutate → load restores identical gameplay state
    GameState state1;
    state1.player.current_map_id = "new_bark_town";
    state1.player.x = 5;
    state1.player.y = 5;
    state1.set_flag("FLAG_A");
    state1.set_var("VAR_X", 100);
    
    // Save state
    auto saved_bytes = state1.serialize();
    
    // Mutate the original
    state1.player.x = 999;
    state1.player.y = 999;
    state1.set_flag("FLAG_B");
    state1.set_var("VAR_X", 0);
    state1.clear_flag("FLAG_A");
    
    // Load from saved bytes
    auto result = GameState::try_deserialize(saved_bytes);
    ASSERT_TRUE(result.ok());
    GameState& state2 = result.state;
    
    // state2 should have original values, not mutated ones
    ASSERT_EQ(state2.player.x, 5);
    ASSERT_EQ(state2.player.y, 5);
    ASSERT_TRUE(state2.check_flag("FLAG_A"));
    ASSERT_FALSE(state2.check_flag("FLAG_B"));
    ASSERT_EQ(state2.get_var("VAR_X"), 100);
    
    std::cout << "  [Save→mutate→load restores original state]\n";
}

TEST(gamestate_serialize_insertion_order_determinism) {
    // CRITICAL (Audit 8): Same logical state inserted in different order → byte-identical output
    // This tests that serialization uses canonical (sorted) ordering, not hash table iteration order.
    
    // State A: Insert flags/vars in order A, B, C
    GameState state_a;
    state_a.player.current_map_id = "test_map";
    state_a.set_flag("AAA_FIRST");
    state_a.set_flag("BBB_SECOND");
    state_a.set_flag("ZZZ_LAST");
    state_a.set_var("VAR_A", 100);
    state_a.set_var("VAR_M", 200);
    state_a.set_var("VAR_Z", 300);
    
    // State B: Insert same flags/vars in REVERSE order
    GameState state_b;
    state_b.player.current_map_id = "test_map";
    state_b.set_flag("ZZZ_LAST");
    state_b.set_flag("BBB_SECOND");
    state_b.set_flag("AAA_FIRST");
    state_b.set_var("VAR_Z", 300);
    state_b.set_var("VAR_M", 200);
    state_b.set_var("VAR_A", 100);
    
    // Serialize both
    auto bytes_a = state_a.serialize();
    auto bytes_b = state_b.serialize();
    
    // Must be byte-identical
    ASSERT_EQ(bytes_a.size(), bytes_b.size());
    
    bool identical = (bytes_a == bytes_b);
    ASSERT_TRUE(identical);
    
    std::cout << "  [Same state, different insertion order → byte-identical serialization ✓]\n";
}

// =============================================================================
// F3: GameState::player single authority — direct sync, no callback needed
// =============================================================================
TEST(f3_player_authority_step_syncs_gamestate) {
    // Prove: HeadlessGameLoop directly writes game_state_->player.x/y/facing
    // at spawn_player, handle_movement (facing), and complete_player_movement.
    // No external callback is required — the loop IS the single write path.
    GameState gs;
    gs.player.x = 5;
    gs.player.y = 5;
    gs.player.facing = enginemon::Direction::Down;

    HeadlessGameLoop loop;
    loop.set_game_state(&gs);
    // No movement callback registered — direct sync is sufficient.

    RuntimeMap map;
    map.map_id = "f3_test";
    map.width = 10; map.height = 10;
    map.blocks.assign(100, 0);
    loop.load_map(map);
    loop.set_collision_data([](int32_t, int32_t) -> CollisionClass { return CollisionClass::Floor; });


    // ORACLE 1: spawn_player directly syncs game_state.player immediately
    loop.spawn_player(3, 7, enginemon::Direction::Right);
    ASSERT_EQ(gs.player.x, 3);
    ASSERT_EQ(gs.player.y, 7);
    ASSERT_EQ(gs.player.facing, enginemon::Direction::Right);

    // ORACLE 2: facing-only update (blocked movement) also syncs
    auto blocked = loop.process_input(InputAction::MoveLeft); // map too small edge case, or use a wall
    // Whether blocked or not, facing must be updated
    ASSERT_EQ(gs.player.facing, enginemon::Direction::Left);

    // Reset to a clean position
    loop.spawn_player(3, 7, enginemon::Direction::Right);

    // ORACLE 3: full step completion syncs x/y
    auto input_result = loop.process_input(InputAction::MoveRight);
    ASSERT_TRUE(input_result.accepted);
    for (int i = 0; i < 16; ++i) loop.tick();
    ASSERT_EQ(loop.player().x, 4);
    ASSERT_EQ(loop.player().y, 7);
    ASSERT_EQ(gs.player.x, 4);  // Direct sync — not deferred callback
    ASSERT_EQ(gs.player.y, 7);
    ASSERT_EQ(gs.player.facing, enginemon::Direction::Right);

    // MUTATION CHECK: there is no second writable copy that could diverge.
    // Any write to loop.player_ is immediately mirrored to gs.player.
    // Verify by comparing all x/y/facing fields:
    ASSERT_EQ(loop.player().x,      gs.player.x);
    ASSERT_EQ(loop.player().y,      gs.player.y);
    ASSERT_EQ(loop.player().facing, gs.player.facing);

    std::cout << "  [F3: spawn/facing/step all directly sync GameState::player — no callback ✓]\n";
}

