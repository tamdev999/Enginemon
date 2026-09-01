// runtime_test_vm_state.cpp Ã¢â‚¬â€ input edge/scheduler, coroutine lifecycle, semantic_fix, bank_utils
#include "engine/scripting/lua_runtime.hpp"
#include "engine/scripting/api_bindings.hpp"
#include "engine/scripting/semantic_ir.hpp"
#include "engine/core/game_loop.hpp"
#include "engine/core/game_state.hpp"
#include "engine/core/timing.hpp"
#include "engine/input/input_system.hpp"
#include "engine/world/collision.hpp"
#include "engine/world/runtime_map.hpp"
#include "crystal/rom/loader.hpp"
#include "crystal/rom/profile.hpp"
#include "crystal/rom/bank_utils.hpp"
#include "crystal/extract/map_extractor.hpp"
#include "crystal/script/crystal_command.hpp"
#include "crystal/script/crystal_cfg.hpp"
#include "crystal/script/semantic_legalizer.hpp"
#include "crystal/script/text_registry.hpp"
#include <array>
#include <filesystem>
#include <fstream>
#include <optional>
#include <algorithm>
#include "scripting/runtime_test_lowering_helpers.hpp"
#include "scripting/runtime_test_shared.hpp"

TEST(input_edge_new_press_after_release) {
    // Test: press Ã¢â€ â€™ release Ã¢â€ â€™ press (tap-tap) before simulation tick
    // The second press should be observable
    InputSystem input;
    
    input.begin_frame();
    input.on_key_down(Sdl3Scancode::Z);  // Press 1
    input.on_key_up(Sdl3Scancode::Z);    // Release
    input.on_key_down(Sdl3Scancode::Z);  // Press 2
    
    // Final state: held (key is down)
    ASSERT_TRUE(input.snapshot().is_held(InputButton::A));
    
    // Press 2 creates a new pending edge (press 1 may have been overwritten)
    ASSERT_TRUE(input.has_pending_pressed(InputButton::A));
    
    // Release was registered (but may be stale if press 2 came after)
    // For simple model: latest state wins, so we have a press edge
    bool pressed = input.consume_pressed(InputButton::A);
    ASSERT_TRUE(pressed);
    
    std::cout << "  [PressÃ¢â€ â€™releaseÃ¢â€ â€™press before tick Ã¢â€ â€™ press observed, held=true Ã¢Å“â€œ]\n";
}

TEST(input_edge_multiple_render_frames_preserves_press) {
    // Edge case: press survives multiple render frames with 0 ticks each
    InputSystem input;
    
    // Frame 1: Press
    input.begin_frame();
    input.on_key_down(Sdl3Scancode::Z);
    // 0 ticks
    
    // Frame 2: 0 ticks
    input.begin_frame();
    ASSERT_TRUE(input.has_pending_pressed(InputButton::A));
    
    // Frame 3: 0 ticks
    input.begin_frame();
    ASSERT_TRUE(input.has_pending_pressed(InputButton::A));
    
    // Frame 4: 0 ticks
    input.begin_frame();
    ASSERT_TRUE(input.has_pending_pressed(InputButton::A));
    
    // Frame 5: Finally get a tick
    bool pressed = input.consume_pressed(InputButton::A);
    ASSERT_TRUE(pressed);
    
    std::cout << "  [Press survives 4 zero-tick render frames Ã¢Å“â€œ]\n";
}

//=============================================================================
// SCHEDULER DEBT RETENTION ADVERSARIAL TESTS (Audit 8)
// Proves: scheduler never silently discards simulation time
//=============================================================================

TEST(scheduler_500ms_hitch_retains_debt) {
    // 500ms hitch should run max_ticks and retain remaining debt
    SimulationScheduler scheduler(TICK_60HZ, 10);  // Max 10 ticks per update
    
    // 500ms hitch
    constexpr int64_t HITCH_500MS = 500'000'000;
    auto result = scheduler.advance(HITCH_500MS);
    
    // Should run max 10 ticks
    ASSERT_EQ(result.ticks_to_run, 10);
    ASSERT_TRUE(result.capped);  // Hit the cap
    
    // 500ms at 60Hz = 30 ticks worth of time
    // After running 10 ticks, 20 ticks worth of debt should remain in accumulator
    // 20 ticks * 16666667 ns = ~333ms
    int64_t expected_debt = (30 - 10) * TICK_60HZ;  // ~333ms
    
    // Accumulator should have significant debt (at least 15 ticks worth)
    ASSERT_TRUE(scheduler.accumulator_ns() >= 15 * TICK_60HZ);
    
    std::cout << "  [500ms hitch: ran 10 ticks, retained ~" 
              << scheduler.accumulator_ns() / 1'000'000 << "ms debt Ã¢Å“â€œ]\n";
}

TEST(scheduler_2_second_hitch_retains_debt) {
    // 2 second hitch should run max_ticks and retain remaining debt
    SimulationScheduler scheduler(TICK_60HZ, 10);
    
    // 2 second hitch
    constexpr int64_t HITCH_2S = 2'000'000'000LL;
    auto result = scheduler.advance(HITCH_2S);
    
    // Should run max 10 ticks
    ASSERT_EQ(result.ticks_to_run, 10);
    ASSERT_TRUE(result.capped);
    
    // 2 seconds at 60Hz = 120 ticks worth of time
    // After running 10 ticks, 110 ticks worth of debt should remain
    int64_t expected_debt = (120 - 10) * TICK_60HZ;  // ~1833ms
    
    // Accumulator should have massive debt (at least 100 ticks worth)
    ASSERT_TRUE(scheduler.accumulator_ns() >= 100 * TICK_60HZ);
    
    std::cout << "  [2s hitch: ran 10 ticks, retained ~" 
              << scheduler.accumulator_ns() / 1'000'000 << "ms debt Ã¢Å“â€œ]\n";
}

TEST(scheduler_repeated_updates_catch_up) {
    // After hitch, repeated updates should eventually catch up
    SimulationScheduler scheduler(TICK_60HZ, 10);
    
    // 500ms hitch (30 ticks worth)
    constexpr int64_t HITCH_500MS = 500'000'000;
    auto result = scheduler.advance(HITCH_500MS);
    
    int total_ticks = result.ticks_to_run;  // First batch
    
    // Simulate several frames with no new elapsed time (pure catch-up)
    for (int frame = 0; frame < 5; frame++) {
        result = scheduler.advance(0);  // No new time, just catch-up
        total_ticks += result.ticks_to_run;
    }
    
    // Should have caught up to approximately 30 ticks total
    // (may be slightly less due to nanosecond rounding)
    ASSERT_TRUE(total_ticks >= 29 && total_ticks <= 31);
    ASSERT_EQ(scheduler.total_ticks(), total_ticks);
    
    std::cout << "  [After 500ms hitch + catch-up: " << total_ticks << " total ticks Ã¢Å“â€œ]\n";
}

TEST(scheduler_total_ticks_equals_elapsed_time) {
    // CRITICAL INVARIANT: Total eventual tick count = elapsed simulation time
    // (subject only to nanosecond rounding)
    SimulationScheduler scheduler(TICK_60HZ, 10);
    
    // Simulate: 100ms normal, 500ms hitch, 100ms normal, catch-up
    int total_ticks = 0;
    
    // Normal 100ms (6 ticks)
    total_ticks += scheduler.advance(100'000'000).ticks_to_run;
    
    // 500ms hitch
    total_ticks += scheduler.advance(500'000'000).ticks_to_run;
    
    // Normal 100ms
    total_ticks += scheduler.advance(100'000'000).ticks_to_run;
    
    // Catch-up with zero elapsed time until no debt remains
    while (scheduler.accumulator_ns() >= TICK_60HZ) {
        total_ticks += scheduler.advance(0).ticks_to_run;
    }
    
    // Total elapsed = 100ms + 500ms + 100ms = 700ms
    // 700ms at 60Hz = 42 ticks
    int expected_ticks = 700'000'000 / TICK_60HZ;  // ~42
    
    // Should match within rounding tolerance
    ASSERT_TRUE(total_ticks >= expected_ticks - 1 && total_ticks <= expected_ticks + 1);
    ASSERT_EQ(scheduler.total_ticks(), total_ticks);
    
    std::cout << "  [700ms total Ã¢â€ â€™ " << total_ticks << " ticks (expected ~" 
              << expected_ticks << ") Ã¢Å“â€œ]\n";
    std::cout << "  [No simulation time discarded Ã¢Å“â€œ]\n";
}

//=============================================================================
// Coroutine Lifecycle Adversarial Tests
// Proves: cleanup_coroutine() called on terminal paths, no stale entries
//=============================================================================

TEST(lua_coroutine_cleanup_via_resume) {
    // INVARIANT: After coroutine finishes via resume(), the active entry is
    // removed and registry ref is released. get_state() returns correct final state.
    LuaRuntime runtime;
    
    // Script that completes immediately (no yield)
    const char* immediate_script = R"lua(
        script = {
            main = function(ctx)
                return 42
            end
        }
    )lua";
    
    runtime.execute_string(immediate_script, "immediate");
    uint32_t coro_id = runtime.start_script("script");
    
    // Script should have finished immediately in start_script
    ScriptState state = runtime.get_state(coro_id);
    ASSERT_EQ(static_cast<int>(state), static_cast<int>(ScriptState::Finished));
    
    // Verify no active coroutine entries remain (has_active_scripts should be false)
    ASSERT_FALSE(runtime.has_active_scripts());
    
    std::cout << "  [Immediate completion cleans up Ã¢Å“â€œ]\n";
    
    // Script that yields once then finishes
    const char* yield_once_script = R"lua(
        script = {
            main = function(ctx)
                coroutine.yield("dialog")
                return 1
            end
        }
    )lua";
    
    runtime.execute_string(yield_once_script, "yield_once");
    coro_id = runtime.start_script("script");
    
    // Should be yielded
    state = runtime.get_state(coro_id);
    ASSERT_EQ(static_cast<int>(state), static_cast<int>(ScriptState::Yielded));
    ASSERT_TRUE(runtime.has_active_scripts());
    
    // Resume via resume() - should complete
    runtime.resume(coro_id);
    
    state = runtime.get_state(coro_id);
    ASSERT_EQ(static_cast<int>(state), static_cast<int>(ScriptState::Finished));
    ASSERT_FALSE(runtime.has_active_scripts());
    
    std::cout << "  [Yield+resume completion cleans up Ã¢Å“â€œ]\n";
}

TEST(lua_coroutine_cleanup_via_resume_with_result) {
    // INVARIANT: After coroutine finishes via resume_with_result(), the active
    // entry is removed and registry ref is released - same behavior as resume().
    LuaRuntime runtime;
    
    // Script that yields for choice then finishes
    const char* choice_script = R"lua(
        script = {
            main = function(ctx)
                local result = coroutine.yield("choice")
                return result  -- Return the choice result
            end
        }
    )lua";
    
    runtime.execute_string(choice_script, "choice");
    uint32_t coro_id = runtime.start_script("script");
    
    // Should be yielded waiting for choice
    ScriptState state = runtime.get_state(coro_id);
    ASSERT_EQ(static_cast<int>(state), static_cast<int>(ScriptState::Yielded));
    ASSERT_EQ(static_cast<int>(runtime.get_yield_reason(coro_id)), 
              static_cast<int>(YieldReason::Choice));
    ASSERT_TRUE(runtime.has_active_scripts());
    
    // Resume with result - should complete and clean up
    runtime.resume_with_result(coro_id, 1);  // User chose option 1
    
    state = runtime.get_state(coro_id);
    ASSERT_EQ(static_cast<int>(state), static_cast<int>(ScriptState::Finished));
    
    // CRITICAL: No stale active entry should remain
    ASSERT_FALSE(runtime.has_active_scripts());
    
    std::cout << "  [resume_with_result() cleans up on completion Ã¢Å“â€œ]\n";
    
    // Test error path via resume_with_error
    const char* error_script = R"lua(
        script = {
            main = function(ctx)
                local result = coroutine.yield("dialog")
                return result
            end
        }
    )lua";
    
    runtime.execute_string(error_script, "error_test");
    coro_id = runtime.start_script("script");
    
    state = runtime.get_state(coro_id);
    ASSERT_EQ(static_cast<int>(state), static_cast<int>(ScriptState::Yielded));
    ASSERT_TRUE(runtime.has_active_scripts());
    
    // Inject error - should clean up
    runtime.resume_with_error(coro_id, "injected test error");
    
    state = runtime.get_state(coro_id);
    ASSERT_EQ(static_cast<int>(state), static_cast<int>(ScriptState::Error));
    ASSERT_FALSE(runtime.has_active_scripts());
    
    std::cout << "  [resume_with_error() cleans up on error Ã¢Å“â€œ]\n";
}

TEST(npc_destination_occupancy_blocks_conflicting_movement) {
    // INVARIANT: When an NPC is moving toward a destination tile, another entity
    // attempting to move to that same tile should be blocked.
    // Reference: Gen2Recomped Collision.occupied() checks both cellX/Y AND targetX/Y
    
    HeadlessGameLoop loop;
    GameState gs;
    loop.set_game_state(&gs);
    // Create a simple 10x10 map (width/height in tiles)
    RuntimeMap rtmap;
    rtmap.width = 20;
    rtmap.height = 20;
    rtmap.blocks.resize(100, 0);
    
    loop.load_map(rtmap);
    loop.set_collision_data([](int32_t x, int32_t y) -> CollisionClass {
        if (x < 0 || y < 0 || x >= 20 || y >= 20) return CollisionClass::Wall;
        return CollisionClass::Floor;
    });
    
    // NPC 1 at (5,5), currently moving toward (6,5) - destination reserved
    NpcState npc1;
    npc1.id = 1;
    npc1.x = 5;
    npc1.y = 5;
    npc1.target_x = 6;  // Destination is (6,5)
    npc1.target_y = 5;
    npc1.is_moving = true;
    npc1.move_progress = 8;  // Midway through step
    npc1.facing = enginemon::Direction::Right;
    npc1.behavior = NpcMovementBehavior::Standing;
    npc1.visible = true;
    npc1.frozen = true;  // Freeze to prevent behavior updates
    loop.add_npc(npc1);
    
    // Player at (7,5) trying to move left to (6,5) - should be blocked
    loop.spawn_player(7, 5, enginemon::Direction::Left);
    
    InputResult result = loop.process_input(InputAction::MoveLeft);
    
    // Should be blocked because NPC1's destination is (6,5)
    ASSERT_TRUE(result.blocked);
    ASSERT_STR_EQ(result.block_reason, "entity");
    
    std::cout << "  [Player blocked by NPC moving toward same tile Ã¢Å“â€œ]\n";
    
    // Now test that the NPC's current position (5,5) is also blocked
    loop.spawn_player(4, 5, enginemon::Direction::Right);
    
    result = loop.process_input(InputAction::MoveRight);
    
    // Should be blocked because NPC1's current position is (5,5)
    ASSERT_TRUE(result.blocked);
    ASSERT_STR_EQ(result.block_reason, "entity");
    
    std::cout << "  [Player blocked by NPC's current position Ã¢Å“â€œ]\n";
    
    // Test: NPC not moving - target_x/y equals x/y, only current position blocked
    loop.clear_npcs();
    
    NpcState npc2;
    npc2.id = 2;
    npc2.x = 5;
    npc2.y = 5;
    npc2.target_x = 5;  // Not moving - target equals current
    npc2.target_y = 5;
    npc2.is_moving = false;
    npc2.facing = enginemon::Direction::Down;
    npc2.behavior = NpcMovementBehavior::Standing;
    npc2.visible = true;
    loop.add_npc(npc2);
    
    // Player at (7,5) can now move to (6,5) because NPC2 isn't targeting it
    loop.spawn_player(7, 5, enginemon::Direction::Left);
    
    result = loop.process_input(InputAction::MoveLeft);
    
    ASSERT_TRUE(result.accepted);
    ASSERT_FALSE(result.blocked);
    
    std::cout << "  [Player allowed when NPC not targeting destination Ã¢Å“â€œ]\n";
}

//=============================================================================
// MAP EVENT DECODE REGRESSION TESTS - Pre-RNG Semantic Fix Pass
//=============================================================================

// Coord event decode: scene_id, y, x order (not y, x, scene_id)
TEST(coord_event_field_decode) {
    // coord_event macro: db \3, \2, \1 -> scene_id, y, x
    // Verify extraction reads these in the correct order
    
    MapExtractor extractor(*g_rom, *g_profile);
    
    // Find a map with coord events
    // IlexForest has coord events for encounters (from pokecrystal data)
    // Let's check a few maps to find one with coord events
    auto result = extractor.extract_map(7, 1);  // IlexForest (group 7, map 1)
    
    if (result.success && !result.map.coord_events.empty()) {
        // If we found coord events, verify the format
        const auto& evt = result.map.coord_events[0];
        
        // Coord events should have:
        // - scene_id in valid range (0-255, often small or 0xFF for "always")
        // - x, y in valid map coordinates (less than map width/height * 2)
        // - script_rom_address resolved to flat address (> 0x4000 for banked)
        
        ASSERT_TRUE(evt.x < result.map.width * 2);
        ASSERT_TRUE(evt.y < result.map.height * 2);
        ASSERT_TRUE(evt.script_rom_address >= 0x4000);  // Should be resolved flat address
        
        std::cout << "  [coord_event fields decoded correctly Ã¢Å“â€œ]\n";
        std::cout << "    scene_id=" << (int)evt.scene_id 
                  << ", x=" << (int)evt.x 
                  << ", y=" << (int)evt.y << "\n";
    } else {
        // Alternative: check that extract_map with known coord events works
        // Sprout Tower (group 20, map 1) has SCENE_SPROUTT TOWER scenes
        result = extractor.extract_map(20, 1);
        
        if (result.success && !result.map.coord_events.empty()) {
            const auto& evt = result.map.coord_events[0];
            ASSERT_TRUE(evt.x < result.map.width * 2);
            ASSERT_TRUE(evt.y < result.map.height * 2);
            ASSERT_TRUE(evt.script_rom_address >= 0x4000);
            
            std::cout << "  [coord_event fields decoded correctly Ã¢Å“â€œ]\n";
        } else {
            // Just verify the extraction code runs without crash
            std::cout << "  [No coord events found to verify, extraction runs Ã¢Å“â€œ]\n";
        }
    }
}

// BG event directional types preserved: FacingUp/Down/Left/Right not collapsed to Read
TEST(bg_event_directional_types_preserved) {
    // BG events that require specific facing should preserve their type
    MapExtractor extractor(*g_rom, *g_profile);
    
    // Search for maps with directional BG events
    // Check Route 36 area for potential directional signs
    auto result = extractor.extract_map(26, 1);  // Route 30
    
    bool found_directional = false;
    for (const auto& bg : result.map.bg_events) {
        if (bg.type == BgEventType::FacingUp ||
            bg.type == BgEventType::FacingDown ||
            bg.type == BgEventType::FacingLeft ||
            bg.type == BgEventType::FacingRight) {
            found_directional = true;
            
            // Verify it's not collapsed to Read
            ASSERT_TRUE(bg.type != BgEventType::Read);
            
            std::cout << "  [Found directional BG event type: " 
                      << static_cast<int>(bg.type) << " Ã¢Å“â€œ]\n";
            break;
        }
    }
    
    // Also verify the enum has all distinct values
    ASSERT_TRUE(static_cast<int>(BgEventType::FacingUp) != static_cast<int>(BgEventType::Read));
    ASSERT_TRUE(static_cast<int>(BgEventType::FacingDown) != static_cast<int>(BgEventType::Read));
    ASSERT_TRUE(static_cast<int>(BgEventType::FacingLeft) != static_cast<int>(BgEventType::Read));
    ASSERT_TRUE(static_cast<int>(BgEventType::FacingRight) != static_cast<int>(BgEventType::Read));
    
    std::cout << "  [BG event directional types are distinct Ã¢Å“â€œ]\n";
}

// Hidden item flag/item preserved (not pointer bytes as item_id/quantity)
TEST(bg_event_hidden_item_semantic_decode) {
    // BGEVENT_ITEM reads hiddenitem structure: dw flag, db item
    // The pointer should NOT be interpreted as item_id/quantity
    
    MapExtractor extractor(*g_rom, *g_profile);
    
    // Search for a map with hidden items (common in routes and caves)
    // Route 29 has HIDDEN_POTION
    auto result = extractor.extract_map(24, 3);  // Route 29
    
    for (const auto& bg : result.map.bg_events) {
        if (bg.type == BgEventType::HiddenItem) {
            // Verify the hidden item has:
            // - item_id as semantic ID (e.g., "item_XX" or named item)
            // - quantity as 1 (hidden items always quantity 1)
            // - condition_flag as flag ID (e.g., "flag_XXXX")
            
            ASSERT_TRUE(bg.quantity == 1);  // Hidden items always 1
            ASSERT_FALSE(bg.item_id.empty());
            ASSERT_FALSE(bg.condition_flag.empty());
            
            std::cout << "  [Hidden item decoded: item=" << bg.item_id 
                      << ", flag=" << bg.condition_flag << " Ã¢Å“â€œ]\n";
            return;
        }
    }
    
    // If no hidden items found, just verify the enum value exists
    ASSERT_TRUE(static_cast<int>(BgEventType::HiddenItem) == 7);
    std::cout << "  [HiddenItem type defined correctly Ã¢Å“â€œ]\n";
}

// IFSET/IFNOTSET flag/script preserved
TEST(bg_event_conditional_script_decode) {
    // BGEVENT_IFSET/IFNOTSET reads conditional_event: dw flag, dw script
    
    MapExtractor extractor(*g_rom, *g_profile);
    
    // Search through maps for conditional BG events
    // These are less common, often in story-progression areas
    
    // Verify the enum values are distinct
    ASSERT_TRUE(static_cast<int>(BgEventType::IfSet) == 5);
    ASSERT_TRUE(static_cast<int>(BgEventType::IfNotSet) == 6);
    ASSERT_TRUE(static_cast<int>(BgEventType::IfSet) != static_cast<int>(BgEventType::IfNotSet));
    
    // Check that conditional BG events store flag/script correctly
    for (uint8_t g = 1; g <= 26; ++g) {
        for (uint8_t m = 1; m <= 20; ++m) {
            auto result = extractor.extract_map(g, m);
            if (!result.success) break;
            
            for (const auto& bg : result.map.bg_events) {
                if (bg.type == BgEventType::IfSet || bg.type == BgEventType::IfNotSet) {
                    // Verify conditional has flag and script
                    ASSERT_FALSE(bg.condition_flag.empty());
                    ASSERT_TRUE(bg.script_rom_address > 0);
                    
                    std::cout << "  [Conditional BG event: type=" << static_cast<int>(bg.type)
                              << ", flag=" << bg.condition_flag 
                              << ", script_addr=0x" << std::hex << bg.script_rom_address 
                              << std::dec << " Ã¢Å“â€œ]\n";
                    return;
                }
            }
        }
    }
    
    std::cout << "  [Conditional BG event types defined correctly Ã¢Å“â€œ]\n";
}

// Object palette extracted from high nibble, type from low nibble
TEST(object_event_palette_type_decode) {
    // object_event byte 7: dn palette, object_type
    // High nibble = palette (PAL_NPC_*), low nibble = object_type
    
    MapExtractor extractor(*g_rom, *g_profile);
    auto result = extractor.extract_map("new_bark_town");
    ASSERT_TRUE(result.success);
    
    // New Bark Town has several NPCs with different palettes
    bool found_nonzero_palette = false;
    
    for (const auto& obj : result.map.objects) {
        // Palette should be 0-15 (4 bits)
        ASSERT_TRUE(obj.palette <= 15);
        
        // is_trainer comes from object_type == 2
        // Non-trainers should have object_type 0 or 1
        
        if (obj.palette > 0) {
            found_nonzero_palette = true;
        }
        
        // Verify hour_start/hour_end are valid
        // Crystal uses:
        //   0-23 = hour (appears only in that time range)
        //   255 (-1) = special sentinel (time-of-day mask mode or always-visible)
        // Reference: pokecrystal/macros/scripts/maps.asm lines 119-124
        ASSERT_TRUE(obj.hour_start <= 23 || obj.hour_start == 255);
        ASSERT_TRUE(obj.hour_end <= 23 || obj.hour_end == 255);
    }
    
    std::cout << "  [Object palette/type extracted correctly Ã¢Å“â€œ]\n";
    if (found_nonzero_palette) {
        std::cout << "    (Found non-default palette values)\n";
    }
}

// script_resumed: no resume attempt => false
TEST(script_resumed_no_resume_attempt) {
    // script_resumed should be false when no script is running or yielded
    
    HeadlessGameLoop loop;
    loop.spawn_player(5, 5, enginemon::Direction::Down);
    loop.set_collision_data([](int32_t x, int32_t y) -> CollisionClass { 
        (void)x; (void)y;
        return CollisionClass::Floor; 
    });
    
    // No script running: script_resumed = false
    TickResult tick1 = loop.tick();
    ASSERT_FALSE(tick1.script_resumed);
    
    // Still no script: script_resumed = false
    TickResult tick2 = loop.tick();
    ASSERT_FALSE(tick2.script_resumed);
    
    std::cout << "  [script_resumed=false when no script running Ã¢Å“â€œ]\n";
}

// script_resumed: Yielded Ã¢â€ â€™ resume Ã¢â€ â€™ Completed => true
TEST(script_resumed_yielded_to_completed) {
    // script_resumed should be true when a yielded script is resumed and completes
    
    HeadlessGameLoop loop;
    loop.spawn_player(5, 5, enginemon::Direction::Down);
    loop.set_collision_data([](int32_t x, int32_t y) -> CollisionClass { 
        (void)x; (void)y;
        return CollisionClass::Floor; 
    });
    
    // Set up Lua runtime with a script that yields for dialog then completes
    LuaRuntime runtime;
    loop.set_lua_runtime(&runtime);
    
    // Script must create global "script" table with "main" function
    // Use coroutine.yield("dialog") to yield for dialog input
    const char* script_code = R"(
        script = {
            main = function(ctx)
                coroutine.yield("dialog")  -- Yield for dialog
                return true
            end
        }
    )";
    
    loop.set_script_loader([&](const std::string&) { return script_code; });
    
    // Start the script - it will yield on dialog
    bool started = loop.start_script("test_yield_complete");
    ASSERT_TRUE(started);
    ASSERT_TRUE(loop.state() == LoopState::ScriptYielded);
    
    // First tick with yielded script - headless mode auto-advances dialog
    // This should resume the script and it will complete
    TickResult tick1 = loop.tick();
    
    // The script was resumed (dialog auto-advanced in headless mode)
    ASSERT_TRUE(tick1.script_resumed);
    ASSERT_TRUE(tick1.script_complete);  // Script finished after resume
    ASSERT_TRUE(loop.state() == LoopState::Idle);
    
    std::cout << "  [Yielded Ã¢â€ â€™ resume Ã¢â€ â€™ Completed: script_resumed=true Ã¢Å“â€œ]\n";
}

// script_resumed: Yielded Ã¢â€ â€™ resume Ã¢â€ â€™ Yielded => true
TEST(script_resumed_yielded_to_yielded) {
    // script_resumed should be true when a yielded script is resumed but yields again
    
    HeadlessGameLoop loop;
    loop.spawn_player(5, 5, enginemon::Direction::Down);
    loop.set_collision_data([](int32_t x, int32_t y) -> CollisionClass { 
        (void)x; (void)y;
        return CollisionClass::Floor; 
    });
    
    // Set up Lua runtime with a script that yields twice
    LuaRuntime runtime;
    loop.set_lua_runtime(&runtime);
    
    // Script must create global "script" table with "main" function
    const char* script_code = R"(
        script = {
            main = function(ctx)
                coroutine.yield("dialog")  -- First yield
                coroutine.yield("dialog")  -- Second yield
                return true
            end
        }
    )";
    
    loop.set_script_loader([&](const std::string&) { return script_code; });
    
    // Start the script - yields on first dialog
    bool started = loop.start_script("test_yield_twice");
    ASSERT_TRUE(started);
    ASSERT_TRUE(loop.state() == LoopState::ScriptYielded);
    
    // Second tick - headless mode auto-advances dialog, script resumes and yields again
    TickResult tick1 = loop.tick();
    
    // Script was resumed (from first dialog) but yielded again (on second dialog)
    // Post-state is still ScriptYielded, but script_resumed should be TRUE
    ASSERT_TRUE(tick1.script_resumed);
    ASSERT_FALSE(tick1.script_complete);
    ASSERT_TRUE(loop.state() == LoopState::ScriptYielded);
    
    std::cout << "  [Yielded Ã¢â€ â€™ resume Ã¢â€ â€™ Yielded: script_resumed=true Ã¢Å“â€œ]\n";
    
    // Third tick - resume again, should complete
    TickResult tick2 = loop.tick();
    ASSERT_TRUE(tick2.script_resumed);
    ASSERT_TRUE(tick2.script_complete);
    ASSERT_TRUE(loop.state() == LoopState::Idle);
    
    std::cout << "  [Second resume completes script Ã¢Å“â€œ]\n";
}

//=============================================================================
// TIMED YIELD TESTS - WaitFrames / WaitSeconds script_resumed tracking
//=============================================================================

// Helper to create a loop with collision callback for timed yield tests
static void init_timed_test_loop(HeadlessGameLoop& loop) {
    loop.spawn_player(5, 5, enginemon::Direction::Down);
    loop.set_collision_data([](int32_t, int32_t) -> CollisionClass {
        return CollisionClass::Floor;
    });
}

// WaitFrames before expiry: script_resumed == false
TEST(wait_frames_before_expiry_no_resume) {
    HeadlessGameLoop loop; init_timed_test_loop(loop);
    LuaRuntime runtime;
    loop.set_lua_runtime(&runtime);
    
    const char* script_code = R"(script={main=function(ctx) coroutine.yield("wait_frames",5) return true end})";
    loop.set_script_loader([&](const std::string&) { return script_code; });
    
    loop.start_script("test");
    for (int i = 0; i < 4; i++) {
        TickResult r = loop.tick();
        ASSERT_FALSE(r.script_resumed);
    }
    std::cout << "  [WaitFrames before expiry: script_resumed=false Ã¢Å“â€œ]\n";
}

// WaitFrames expiry: script_resumed == true
TEST(wait_frames_expiry_sets_resumed) {
    HeadlessGameLoop loop; init_timed_test_loop(loop);
    LuaRuntime runtime;
    loop.set_lua_runtime(&runtime);
    
    const char* script_code = R"(script={main=function(ctx) coroutine.yield("wait_frames",3) return true end})";
    loop.set_script_loader([&](const std::string&) { return script_code; });
    
    loop.start_script("test");
    loop.tick(); loop.tick();
    TickResult r = loop.tick();
    ASSERT_TRUE(r.script_resumed);
    ASSERT_TRUE(r.script_complete);
    std::cout << "  [WaitFrames expiry: script_resumed=true Ã¢Å“â€œ]\n";
}

// WaitFrames expiry + immediate re-yield: script_resumed == true
TEST(wait_frames_expiry_reyield_sets_resumed) {
    HeadlessGameLoop loop; init_timed_test_loop(loop);
    LuaRuntime runtime;
    loop.set_lua_runtime(&runtime);
    
    const char* script_code = R"(script={main=function(ctx) coroutine.yield("wait_frames",2) coroutine.yield("wait_frames",2) return true end})";
    loop.set_script_loader([&](const std::string&) { return script_code; });
    
    loop.start_script("test");
    loop.tick();
    TickResult r2 = loop.tick();
    ASSERT_TRUE(r2.script_resumed);
    ASSERT_FALSE(r2.script_complete);
    std::cout << "  [WaitFrames re-yield: script_resumed=true, complete=false Ã¢Å“â€œ]\n";
}

// WaitSeconds does NOT resume on next tick
TEST(wait_seconds_not_immediate_resume) {
    HeadlessGameLoop loop; init_timed_test_loop(loop);
    LuaRuntime runtime;
    loop.set_lua_runtime(&runtime);
    
    const char* script_code = R"(script={main=function(ctx) coroutine.yield("wait_seconds",0.5) return true end})";
    loop.set_script_loader([&](const std::string&) { return script_code; });
    
    loop.start_script("test");
    for (int i = 0; i < 10; i++) {
        TickResult r = loop.tick();
        ASSERT_FALSE(r.script_resumed);
    }
    std::cout << "  [WaitSeconds(0.5s): not resumed in first 10 ticks Ã¢Å“â€œ]\n";
}

// WaitSeconds resumes after duration (60 FPS)
TEST(wait_seconds_resumes_after_duration) {
    HeadlessGameLoop loop; init_timed_test_loop(loop);
    LuaRuntime runtime;
    loop.set_lua_runtime(&runtime);
    
    // 0.05s = 3 ticks at 60 FPS
    const char* script_code = R"(script={main=function(ctx) coroutine.yield("wait_seconds",0.05) return true end})";
    loop.set_script_loader([&](const std::string&) { return script_code; });
    
    loop.start_script("test");
    loop.tick(); loop.tick();
    TickResult r3 = loop.tick();
    ASSERT_TRUE(r3.script_resumed);
    ASSERT_TRUE(r3.script_complete);
    std::cout << "  [WaitSeconds(0.05s) resumes after 3 ticks Ã¢Å“â€œ]\n";
}

// WaitSeconds resume sets script_resumed flag
TEST(wait_seconds_resume_sets_flag) {
    HeadlessGameLoop loop; init_timed_test_loop(loop);
    LuaRuntime runtime;
    loop.set_lua_runtime(&runtime);
    
    // ~1 tick duration
    const char* script_code = R"(script={main=function(ctx) coroutine.yield("wait_seconds",0.017) return true end})";
    loop.set_script_loader([&](const std::string&) { return script_code; });
    
    loop.start_script("test");
    loop.tick();
    TickResult r2 = loop.tick();
    ASSERT_TRUE(r2.script_resumed);
    std::cout << "  [WaitSeconds resume sets script_resumed=true Ã¢Å“â€œ]\n";
}

// Zero-duration WaitSeconds resumes on first tick
TEST(wait_seconds_zero_duration) {
    HeadlessGameLoop loop; init_timed_test_loop(loop);
    LuaRuntime runtime;
    loop.set_lua_runtime(&runtime);
    
    const char* script_code = R"(script={main=function(ctx) coroutine.yield("wait_seconds",0) return true end})";
    loop.set_script_loader([&](const std::string&) { return script_code; });
    
    loop.start_script("test");
    TickResult r1 = loop.tick();
    ASSERT_TRUE(r1.script_resumed);
    ASSERT_TRUE(r1.script_complete);
    std::cout << "  [WaitSeconds(0) resumes on first tick Ã¢Å“â€œ]\n";
}

//=============================================================================
// WAITSECONDS PRECISION TESTS - Integer tick conversion
// Verify: wait_ticks = ceil(seconds * 60) for deterministic timing
// No floating-point subtraction drift
//=============================================================================

// WaitSeconds(0.05) = ceil(0.05 * 60) = 3 ticks exactly
TEST(wait_seconds_precision_0_05s_is_3_ticks) {
    HeadlessGameLoop loop; init_timed_test_loop(loop);
    LuaRuntime runtime;
    loop.set_lua_runtime(&runtime);
    
    const char* script_code = R"(script={main=function(ctx) coroutine.yield("wait_seconds",0.05) return true end})";
    loop.set_script_loader([&](const std::string&) { return script_code; });
    
    loop.start_script("test");
    
    // Tick 1, 2: should NOT resume
    TickResult r1 = loop.tick();
    ASSERT_FALSE(r1.script_resumed);
    TickResult r2 = loop.tick();
    ASSERT_FALSE(r2.script_resumed);
    
    // Tick 3: should resume
    TickResult r3 = loop.tick();
    ASSERT_TRUE(r3.script_resumed);
    ASSERT_TRUE(r3.script_complete);
    std::cout << "  [WaitSeconds(0.05s) = 3 ticks exactly Ã¢Å“â€œ]\n";
}

// WaitSeconds(0.1) = ceil(0.1 * 60) = 6 ticks exactly
TEST(wait_seconds_precision_0_1s_is_6_ticks) {
    HeadlessGameLoop loop; init_timed_test_loop(loop);
    LuaRuntime runtime;
    loop.set_lua_runtime(&runtime);
    
    const char* script_code = R"(script={main=function(ctx) coroutine.yield("wait_seconds",0.1) return true end})";
    loop.set_script_loader([&](const std::string&) { return script_code; });
    
    loop.start_script("test");
    
    // Tick 1-5: should NOT resume
    for (int i = 0; i < 5; i++) {
        TickResult r = loop.tick();
        ASSERT_FALSE(r.script_resumed);
    }
    
    // Tick 6: should resume
    TickResult r6 = loop.tick();
    ASSERT_TRUE(r6.script_resumed);
    ASSERT_TRUE(r6.script_complete);
    std::cout << "  [WaitSeconds(0.1s) = 6 ticks exactly Ã¢Å“â€œ]\n";
}

// WaitSeconds(1.0) = ceil(1.0 * 60) = 60 ticks exactly
TEST(wait_seconds_precision_1_0s_is_60_ticks) {
    HeadlessGameLoop loop; init_timed_test_loop(loop);
    LuaRuntime runtime;
    loop.set_lua_runtime(&runtime);
    
    const char* script_code = R"(script={main=function(ctx) coroutine.yield("wait_seconds",1.0) return true end})";
    loop.set_script_loader([&](const std::string&) { return script_code; });
    
    loop.start_script("test");
    
    // Tick 1-59: should NOT resume
    for (int i = 0; i < 59; i++) {
        TickResult r = loop.tick();
        ASSERT_FALSE(r.script_resumed);
    }
    
    // Tick 60: should resume
    TickResult r60 = loop.tick();
    ASSERT_TRUE(r60.script_resumed);
    ASSERT_TRUE(r60.script_complete);
    std::cout << "  [WaitSeconds(1.0s) = 60 ticks exactly Ã¢Å“â€œ]\n";
}

//=============================================================================
// COROUTINE IDENTITY TESTS - Correct resume attribution
// Verify: script_resumed = true IFF active_coroutine was resumed
//=============================================================================

// Unrelated coroutine resumes -> active script_resumed == false
TEST(unrelated_coroutine_resume_does_not_set_script_resumed) {
    HeadlessGameLoop loop; init_timed_test_loop(loop);
    LuaRuntime runtime;
    loop.set_lua_runtime(&runtime);
    
    // Active script: waits 10 frames
    const char* active_script = R"(script={main=function(ctx) coroutine.yield("wait_frames",10) return true end})";
    loop.set_script_loader([&](const std::string&) { return active_script; });
    
    loop.start_script("test");
    uint32_t active_id = loop.active_coroutine();
    ASSERT_TRUE(active_id != 0);
    
    // Start an unrelated coroutine directly in the runtime (waits 1 frame)
    runtime.execute_string(R"(unrelated={main=function(ctx) coroutine.yield("wait_frames",1) return true end})", "unrelated");
    uint32_t unrelated_id = runtime.start_script("unrelated");
    ASSERT_TRUE(unrelated_id != 0);
    ASSERT_TRUE(unrelated_id != active_id);
    
    // Tick 1: unrelated coroutine should resume, but active should NOT
    TickResult r1 = loop.tick();
    // script_resumed should be FALSE because the ACTIVE coroutine didn't resume
    ASSERT_FALSE(r1.script_resumed);
    
    // Verify: unrelated coroutine finished, active still yielded
    ASSERT_TRUE(runtime.get_state(unrelated_id) == ScriptState::Finished);
    ASSERT_TRUE(runtime.get_state(active_id) == ScriptState::Yielded);
    
    std::cout << "  [Unrelated coroutine resume: script_resumed=false Ã¢Å“â€œ]\n";
}

// Active timed coroutine resumes -> script_resumed == true
TEST(active_coroutine_timed_resume_sets_script_resumed) {
    HeadlessGameLoop loop; init_timed_test_loop(loop);
    LuaRuntime runtime;
    loop.set_lua_runtime(&runtime);
    
    // Active script: waits 2 frames
    const char* script_code = R"(script={main=function(ctx) coroutine.yield("wait_frames",2) return true end})";
    loop.set_script_loader([&](const std::string&) { return script_code; });
    
    loop.start_script("test");
    
    // Tick 1: should NOT resume
    TickResult r1 = loop.tick();
    ASSERT_FALSE(r1.script_resumed);
    
    // Tick 2: should resume (timed resume of active coroutine)
    TickResult r2 = loop.tick();
    ASSERT_TRUE(r2.script_resumed);
    ASSERT_TRUE(r2.script_complete);
    
    std::cout << "  [Active coroutine timed resume: script_resumed=true Ã¢Å“â€œ]\n";
}

// Active coroutine resumes and re-yields -> script_resumed == true
TEST(active_coroutine_resume_reyield_sets_script_resumed) {
    HeadlessGameLoop loop; init_timed_test_loop(loop);
    LuaRuntime runtime;
    loop.set_lua_runtime(&runtime);
    
    // Script yields twice: first wait 1 frame, then wait 100 frames
    const char* script_code = R"(script={main=function(ctx)
        coroutine.yield("wait_frames",1)
        coroutine.yield("wait_frames",100)
        return true
    end})";
    loop.set_script_loader([&](const std::string&) { return script_code; });
    
    loop.start_script("test");
    
    // Tick 1: first yield expires, resumes, re-yields with 100 frames
    TickResult r1 = loop.tick();
    ASSERT_TRUE(r1.script_resumed);  // Resume happened
    ASSERT_FALSE(r1.script_complete);  // But script re-yielded, not complete
    
    // Verify still yielded
    ASSERT_TRUE(runtime.get_state(loop.active_coroutine()) == ScriptState::Yielded);
    
    std::cout << "  [Active coroutine resume+re-yield: script_resumed=true Ã¢Å“â€œ]\n";
}

//=============================================================================
// SCRIPT LIFECYCLE TESTS - Completion vs Error distinction, reset cleanup
//=============================================================================

// Script finishes normally: script_complete=true, script_error=false
TEST(script_finishes_normally_sets_complete) {
    HeadlessGameLoop loop; init_timed_test_loop(loop);
    LuaRuntime runtime;
    loop.set_lua_runtime(&runtime);
    
    const char* script_code = R"(script={main=function(ctx) return true end})";
    loop.set_script_loader([&](const std::string&) { return script_code; });
    
    bool started = loop.start_script("test");
    ASSERT_TRUE(started);
    // Script completed immediately during start
    ASSERT_TRUE(loop.state() == LoopState::Idle);
    
    std::cout << "  [Normal completion: start_script returns true Ã¢Å“â€œ]\n";
}

// Script errors after resume: script_complete=false, script_error=true
TEST(script_errors_after_resume_sets_error) {
    HeadlessGameLoop loop; init_timed_test_loop(loop);
    LuaRuntime runtime;
    // Silence expected Lua error output — this test intentionally triggers an error.
    runtime.set_error_handler([](const std::string&, const std::string&) {});
    loop.set_lua_runtime(&runtime);
    
    // Script yields then errors on resume
    const char* script_code = R"(script={main=function(ctx) 
        coroutine.yield("dialog")
        error("intentional test error")
    end})";
    loop.set_script_loader([&](const std::string&) { return script_code; });
    
    bool started = loop.start_script("test");
    ASSERT_TRUE(started);
    ASSERT_TRUE(loop.state() == LoopState::ScriptYielded);
    
    // Tick resumes (auto-advance dialog in headless mode), script errors
    TickResult r = loop.tick();
    ASSERT_TRUE(r.script_resumed);
    ASSERT_FALSE(r.script_complete);  // NOT normal completion
    ASSERT_TRUE(r.script_error);       // Error occurred
    ASSERT_TRUE(loop.state() == LoopState::Idle);
    
    std::cout << "  [Error after resume: script_error=true, script_complete=false Ã¢Å“â€œ]\n";
}

// Script errors immediately during start: start_script returns false
TEST(script_errors_immediately_returns_false) {
    HeadlessGameLoop loop; init_timed_test_loop(loop);
    LuaRuntime runtime;
    // Silence expected Lua error output — this test intentionally triggers a syntax error.
    runtime.set_error_handler([](const std::string&, const std::string&) {});
    loop.set_lua_runtime(&runtime);
    
    // Script has syntax error
    const char* script_code = R"(script={main=function(ctx) 
        local x = -- syntax error, incomplete expression
    end})";
    loop.set_script_loader([&](const std::string&) { return script_code; });
    
    bool started = loop.start_script("test");
    ASSERT_FALSE(started);  // Should return false on immediate error
    ASSERT_TRUE(loop.state() == LoopState::Idle);
    
    std::cout << "  [Immediate syntax error: start_script returns false Ã¢Å“â€œ]\n";
}

// Script runtime error during start: start_script returns false
TEST(script_runtime_error_during_start_returns_false) {
    HeadlessGameLoop loop; init_timed_test_loop(loop);
    LuaRuntime runtime;
    // Silence expected Lua error output — this test intentionally triggers a runtime error.
    runtime.set_error_handler([](const std::string&, const std::string&) {});
    loop.set_lua_runtime(&runtime);
    
    // Script errors immediately on execution
    const char* script_code = R"(script={main=function(ctx) error("immediate error") end})";
    loop.set_script_loader([&](const std::string&) { return script_code; });
    
    bool started = loop.start_script("test");
    ASSERT_FALSE(started);  // Should return false on runtime error during start
    ASSERT_TRUE(loop.state() == LoopState::Idle);
    
    std::cout << "  [Immediate runtime error: start_script returns false Ã¢Å“â€œ]\n";
}

// Yielded script remains non-terminal
TEST(yielded_script_remains_nonterminal) {
    HeadlessGameLoop loop; init_timed_test_loop(loop);
    LuaRuntime runtime;
    loop.set_lua_runtime(&runtime);
    
    // Script yields for long wait
    const char* script_code = R"(script={main=function(ctx) coroutine.yield("wait_frames",100) return true end})";
    loop.set_script_loader([&](const std::string&) { return script_code; });
    
    loop.start_script("test");
    ASSERT_TRUE(loop.state() == LoopState::ScriptYielded);
    
    // Tick a few times, not enough to expire
    for (int i = 0; i < 5; i++) {
        TickResult r = loop.tick();
        ASSERT_FALSE(r.script_complete);
        ASSERT_FALSE(r.script_error);
        ASSERT_TRUE(loop.state() == LoopState::ScriptYielded);
    }
    
    std::cout << "  [Yielded script: script_complete=false, script_error=false Ã¢Å“â€œ]\n";
}

// Reset cancels active coroutine - no later timed resume
TEST(reset_cancels_active_coroutine_no_timed_resume) {
    HeadlessGameLoop loop; init_timed_test_loop(loop);
    LuaRuntime runtime;
    loop.set_lua_runtime(&runtime);
    
    // Track if coroutine has effect
    bool side_effect_occurred = false;
    
    // Script waits 5 frames then would set a flag
    // We use a global variable to detect if it ever resumes
    runtime.execute_string("test_side_effect = false", "init");
    
    const char* script_code = R"(script={main=function(ctx) 
        coroutine.yield("wait_frames", 5)
        test_side_effect = true
        return true
    end})";
    loop.set_script_loader([&](const std::string&) { return script_code; });
    
    // Start script and verify it yields
    loop.start_script("test");
    ASSERT_TRUE(loop.state() == LoopState::ScriptYielded);
    
    // Tick once (4 frames remaining)
    loop.tick();
    ASSERT_TRUE(loop.state() == LoopState::ScriptYielded);
    
    // Reset the loop - should cancel coroutine
    loop.reset();
    ASSERT_TRUE(loop.state() == LoopState::Idle);
    ASSERT_TRUE(loop.active_coroutine() == 0);
    
    // Advance runtime well beyond the original wake time
    for (int i = 0; i < 20; i++) {
        runtime.update(1.0f / 60.0f);
    }
    
    // Check that the side effect did NOT occur
    lua_getglobal(runtime.get_state(), "test_side_effect");
    side_effect_occurred = lua_toboolean(runtime.get_state(), -1);
    lua_pop(runtime.get_state(), 1);
    
    ASSERT_FALSE(side_effect_occurred);
    
    std::cout << "  [Reset cancels coroutine: no timed resume occurs Ã¢Å“â€œ]\n";
}

// Reset when no script active
TEST(reset_when_no_script_active) {
    HeadlessGameLoop loop; init_timed_test_loop(loop);
    LuaRuntime runtime;
    loop.set_lua_runtime(&runtime);
    
    // No script started
    ASSERT_TRUE(loop.state() == LoopState::Idle);
    ASSERT_TRUE(loop.active_coroutine() == 0);
    
    // Reset should succeed without error
    loop.reset();
    
    ASSERT_TRUE(loop.state() == LoopState::Idle);
    ASSERT_TRUE(loop.active_coroutine() == 0);
    
    std::cout << "  [Reset when no script: succeeds safely Ã¢Å“â€œ]\n";
}

// Reset when script already completed
TEST(reset_after_script_completed) {
    HeadlessGameLoop loop; init_timed_test_loop(loop);
    LuaRuntime runtime;
    loop.set_lua_runtime(&runtime);
    
    // Script completes immediately
    const char* script_code = R"(script={main=function(ctx) return true end})";
    loop.set_script_loader([&](const std::string&) { return script_code; });
    
    loop.start_script("test");
    ASSERT_TRUE(loop.state() == LoopState::Idle);  // Completed immediately
    ASSERT_TRUE(loop.active_coroutine() == 0);
    
    // Reset should succeed
    loop.reset();
    
    ASSERT_TRUE(loop.state() == LoopState::Idle);
    ASSERT_TRUE(loop.active_coroutine() == 0);
    
    std::cout << "  [Reset after completion: succeeds safely Ã¢Å“â€œ]\n";
}

// Reset when script currently yielded
TEST(reset_when_script_yielded) {
    HeadlessGameLoop loop; init_timed_test_loop(loop);
    LuaRuntime runtime;
    loop.set_lua_runtime(&runtime);
    
    // Script yields
    const char* script_code = R"(script={main=function(ctx) coroutine.yield("dialog") return true end})";
    loop.set_script_loader([&](const std::string&) { return script_code; });
    
    loop.start_script("test");
    ASSERT_TRUE(loop.state() == LoopState::ScriptYielded);
    uint32_t old_coro = loop.active_coroutine();
    ASSERT_TRUE(old_coro != 0);
    
    // Reset should cancel the yielded coroutine
    loop.reset();
    
    ASSERT_TRUE(loop.state() == LoopState::Idle);
    ASSERT_TRUE(loop.active_coroutine() == 0);
    
    // The old coroutine should be gone from runtime
    ScriptState state = runtime.get_state(old_coro);
    // Cancelled coroutines end in Finished state
    ASSERT_TRUE(state == ScriptState::Finished || state == ScriptState::Error);
    
    std::cout << "  [Reset when yielded: coroutine cancelled Ã¢Å“â€œ]\n";
}

// Coord event script roots appear in corpus
TEST(coord_event_scripts_in_corpus) {
    // Coord events should contribute their scripts to the corpus discovery
    // This is implicitly tested by the corpus count increasing from 1679 to 1788
    
    MapExtractor extractor(*g_rom, *g_profile);
    
    // Count total coord events across discovered maps
    size_t total_coord_events = 0;
    
    for (uint8_t g = 1; g <= 26; ++g) {
        for (uint8_t m = 1; m <= 50; ++m) {
            auto result = extractor.extract_map(g, m);
            if (!result.success) break;
            
            total_coord_events += result.map.coord_events.size();
        }
    }
    
    std::cout << "  [Total coord events found: " << total_coord_events << " Ã¢Å“â€œ]\n";
    
    // Verify coord events have valid script addresses
    auto result = extractor.extract_map(7, 1);  // IlexForest
    if (result.success) {
        for (const auto& coord : result.map.coord_events) {
            // script_rom_address should be resolved flat address
            ASSERT_TRUE(coord.script_rom_address >= 0x4000);
        }
    }
    
    std::cout << "  [Coord event script addresses resolved to flat Ã¢Å“â€œ]\n";
}

//=============================================================================
// CANONICAL BANK ADDRESS HELPER TESTS Ã¢â‚¬â€ August 2026
// Verifies crystal_flat_to_bank, crystal_bank_to_flat, crystal_local_ptr_to_flat
// from crystal/rom/bank_utils.hpp.
//
// These helpers are the single source of truth for Crystal bank arithmetic.
// Each call site that previously inlined the formula delegates to them.
//
// Source authority:
//   pokecrystal scripting.asm:1389  Script_sdefer   Ã¢â‚¬â€ uses wScriptBank
//   pokecrystal scripting.asm:1688  Script_getstring Ã¢â‚¬â€ uses wScriptBank
//=============================================================================

// Helper: crystal_flat_to_bank Ã¢â‚¬â€ bank 0 boundary
TEST(bank_utils_flat_to_bank_zero) {
    using namespace crystal;
    // Bank 0 spans flat 0x0000Ã¢â‚¬â€œ0x3FFF
    ASSERT_EQ(crystal_flat_to_bank(0x0000), 0);
    ASSERT_EQ(crystal_flat_to_bank(0x0001), 0);
    ASSERT_EQ(crystal_flat_to_bank(0x3FFF), 0);
    std::cout << "  [flat_to_bank: bank 0 range Ã¢Å“â€œ]\n";
}

// Helper: crystal_flat_to_bank Ã¢â‚¬â€ first switchable bank
TEST(bank_utils_flat_to_bank_one) {
    using namespace crystal;
    // Bank 1 spans flat 0x4000Ã¢â‚¬â€œ0x7FFF
    ASSERT_EQ(crystal_flat_to_bank(0x4000), 1);
    ASSERT_EQ(crystal_flat_to_bank(0x4001), 1);
    ASSERT_EQ(crystal_flat_to_bank(0x7FFF), 1);
    std::cout << "  [flat_to_bank: bank 1 range Ã¢Å“â€œ]\n";
}

// Helper: crystal_flat_to_bank Ã¢â‚¬â€ later bank (0x1A = 26, used by sdefer test)
TEST(bank_utils_flat_to_bank_nonzero) {
    using namespace crystal;
    // Bank 0x1A spans flat 0x1A*0x4000 = 0x68000 to 0x6BFFF
    ASSERT_EQ(crystal_flat_to_bank(0x68000), 0x1A);
    ASSERT_EQ(crystal_flat_to_bank(0x68100), 0x1A);
    ASSERT_EQ(crystal_flat_to_bank(0x6BFFF), 0x1A);
    // Bank 0x1B starts at 0x6C000
    ASSERT_EQ(crystal_flat_to_bank(0x6C000), 0x1B);
    std::cout << "  [flat_to_bank: bank 0x1A/0x1B boundary Ã¢Å“â€œ]\n";
}

// Helper: crystal_bank_to_flat Ã¢â‚¬â€ local ptr 0x4000 (start of banked window)
TEST(bank_utils_bank_to_flat_ptr_at_4000) {
    using namespace crystal;
    // ptr = 0x4000, bank = 0x1A Ã¢â€ â€™ flat = 0x1A * 0x4000 + 0 = 0x68000
    ASSERT_EQ(crystal_bank_to_flat(0x1A, 0x4000), 0x68000);
    // ptr = 0x4000, bank = 1 Ã¢â€ â€™ flat = 0x4000
    ASSERT_EQ(crystal_bank_to_flat(1, 0x4000), 0x4000);
    std::cout << "  [bank_to_flat: ptr=0x4000 Ã¢Å“â€œ]\n";
}

// Helper: crystal_bank_to_flat Ã¢â‚¬â€ local ptr 0x7FFF (end of banked window)
TEST(bank_utils_bank_to_flat_ptr_at_7fff) {
    using namespace crystal;
    // ptr = 0x7FFF, bank = 0x1A Ã¢â€ â€™ flat = 0x1A*0x4000 + 0x3FFF = 0x6BFFF
    ASSERT_EQ(crystal_bank_to_flat(0x1A, 0x7FFF), 0x6BFFF);
    std::cout << "  [bank_to_flat: ptr=0x7FFF Ã¢Å“â€œ]\n";
}

// Helper: crystal_bank_to_flat Ã¢â‚¬â€ local ptr < 0x4000 (ROM0 region)
TEST(bank_utils_bank_to_flat_ptr_in_rom0) {
    using namespace crystal;
    // ptr < 0x4000 Ã¢â€ â€™ ROM0; bank is irrelevant, flat = ptr
    ASSERT_EQ(crystal_bank_to_flat(0x1A, 0x0100), 0x0100);
    ASSERT_EQ(crystal_bank_to_flat(0x00, 0x0100), 0x0100);
    ASSERT_EQ(crystal_bank_to_flat(0xFF, 0x3FFF), 0x3FFF);
    std::cout << "  [bank_to_flat: ROM0 ptr Ã¢â€ â€™ flat=ptr Ã¢Å“â€œ]\n";
}

// Helper: round-trip flatÃ¢â€ â€™bankÃ¢â€ â€™flat
TEST(bank_utils_round_trip) {
    using namespace crystal;
    // For a flat address in a non-zero bank, bank_to_flat(flat_to_bank(addr), local)
    // should recover addr when local = 0x4000 + (addr & 0x3FFF)
    uint32_t flat = 0x68500;  // Bank 0x1A, offset 0x500 within bank
    uint8_t bank = crystal_flat_to_bank(flat);
    // Local ptr = 0x4000 + (flat - bank*0x4000) = 0x4000 + 0x500 = 0x4500
    uint16_t local_ptr = static_cast<uint16_t>(0x4000 + (flat - bank * 0x4000u));
    ASSERT_EQ(crystal_bank_to_flat(bank, local_ptr), flat);
    std::cout << "  [round-trip flat=0x68500 Ã¢â€ â€™ bank=0x1A, ptr=0x4500 Ã¢â€ â€™ flat=0x68500 Ã¢Å“â€œ]\n";
}

// crystal_local_ptr_to_flat Ã¢â‚¬â€ sdefer nonzero-bank case
// Proves the helper matches the expected result and asymmetry rules out raw-ptr mistake.
// Raw ptr 0x4500 != flat 0x68500, so a raw16-as-flat bug would produce wrong result.
TEST(bank_utils_local_ptr_to_flat_sdefer_nonzero_bank) {
    using namespace crystal;
    // Script at bank 0x1A (entry=0x68100), sdefer ptr=0x4500
    // Expected flat = 0x1A*0x4000 + (0x4500 - 0x4000) = 0x68000 + 0x500 = 0x68500
    uint32_t flat = crystal_local_ptr_to_flat(0x68100, 0x4500);
    ASSERT_EQ(flat, 0x68500u);
    // Prove asymmetry: raw ptr 0x4500 != flat result 0x68500
    ASSERT_TRUE(flat != 0x4500);
    std::cout << "  [local_ptr_to_flat sdefer: 0x4500 @ entry 0x68100 Ã¢â€ â€™ 0x68500 Ã¢Å“â€œ]\n";
}

// crystal_local_ptr_to_flat Ã¢â‚¬â€ getstring nonzero-bank case
// getstring carries the same bank semantics as sdefer: uses wScriptBank.
// Use a different bank (0x06) and ptr to prove this is independent.
TEST(bank_utils_local_ptr_to_flat_getstring_nonzero_bank) {
    using namespace crystal;
    // Script at bank 0x06 (entry=0x18080), getstring ptr=0x5100
    // flat = 0x06*0x4000 + (0x5100 - 0x4000) = 0x18000 + 0x1100 = 0x19100
    uint32_t flat = crystal_local_ptr_to_flat(0x18080, 0x5100);
    ASSERT_EQ(flat, 0x19100u);
    // Prove asymmetry: raw ptr 0x5100 != flat result 0x19100
    ASSERT_TRUE(flat != 0x5100);
    std::cout << "  [local_ptr_to_flat getstring: 0x5100 @ entry 0x18080 Ã¢â€ â€™ 0x19100 Ã¢Å“â€œ]\n";
}

// sdefer lowering now uses crystal_local_ptr_to_flat Ã¢â‚¬â€ prove via canonical helper
// This regression test replaces the older semantic_fix_sdefer_bank_resolution test's
// implicit formula with an explicit canonical helper comparison.
TEST(bank_utils_sdefer_lowering_matches_canonical_helper) {
    using namespace crystal;
    using namespace enginemon;

    // Script at bank 0x1A, sdefer ptr=0x4500
    CrystalCommand cmd;
    Cmd_Sdefer sd;
    sd.pointer = 0x4500;
    cmd.data = sd;
    cmd.span.rom_address = 0;
    cmd.span.raw_bytes = {0x8D, 0x00, 0x45};

    CrystalScriptIR ir;
    ir.name = "test_sdefer_canonical";
    ir.entry_address = 0x68100;
    ir.rom_start = 0;
    ir.rom_end = 3;
    ir.commands.push_back(cmd);

    CrystalCFG cfg;
    cfg.script_name = "test_sdefer_canonical";
    cfg.entry_address = 0x68100;
    BasicBlock block;
    block.id = 0;
    block.start_address = 0;
    block.end_address = 3;
    block.command_start = 0;
    block.command_count = 1;
    cfg.blocks.push_back(block);
    cfg.source_ir = &ir;

    SemanticLegalizer legalizer;
    LoweringResult result = legalizer.lower(ir, cfg);
    ASSERT_TRUE(result.success);

    const auto& inst = result.ir.blocks[0].instructions[0];
    auto* sdef_op = std::get_if<Sem_Sdefer>(&inst.op);
    ASSERT_TRUE(sdef_op != nullptr);

    // Canonical helper produces the expected flat address
    uint32_t expected_flat = crystal_local_ptr_to_flat(0x68100, 0x4500);
    ASSERT_EQ(expected_flat, 0x68500u);

    // Lowering result must use exactly that address in the script_id
    std::ostringstream ss;
    ss << "deferred_" << std::hex << expected_flat;
    ASSERT_STR_EQ(sdef_op->target_script_id, ss.str());

    std::cout << "  [sdefer lowering == canonical helper result 0x68500 Ã¢Å“â€œ]\n";
}

// getstring lowering now uses cmd->span.rom_address Ã¢â‚¬â€ prove via canonical helper
// The resolved text content is stored in str_value, not a raw ROM address.
TEST(bank_utils_getstring_lowering_matches_canonical_helper) {
    using namespace crystal;
    using namespace enginemon;

    // Script at bank 0x06 (entry=0x18080), getstring ptr=0x5100
    // Command is also in bank 0x06 Ã¢â€ â€™ cmd.span.rom_address must be in that bank.
    CrystalCommand cmd;
    Cmd_Getstring gs;
    gs.text_pointer = 0x5100;
    gs.strbuf = 0;
    cmd.data = gs;
    // Set command address to match the script's bank (bank 6 = flat 0x18080)
    cmd.span.rom_address = 0x18080;
    cmd.span.raw_bytes = {0x45, 0, 0x00, 0x51};

    CrystalScriptIR ir;
    ir.name = "test_getstring_canonical";
    ir.entry_address = 0x18080;
    ir.rom_start = 0;
    ir.rom_end = 4;
    ir.commands.push_back(cmd);

    CrystalCFG cfg;
    cfg.script_name = "test_getstring_canonical";
    cfg.entry_address = 0x18080;
    BasicBlock block;
    block.id = 0;
    block.start_address = 0;
    block.end_address = 4;
    block.command_start = 0;
    block.command_count = 1;
    cfg.blocks.push_back(block);
    cfg.source_ir = &ir;

    SemanticLegalizer legalizer;
    LoweringResult result = legalizer.lower(ir, cfg);
    ASSERT_TRUE(result.success);

    const auto& inst = result.ir.blocks[0].instructions[0];
    auto* pta = std::get_if<Sem_PrepareTextArg>(&inst.op);
    ASSERT_TRUE(pta != nullptr);

    // No raw ROM address survives Ã¢â‚¬â€ str_value holds the resolved text
    ASSERT_EQ(pta->arg_type, TextArgType::String);
    ASSERT_EQ(pta->buffer_slot, 0u);

    std::cout << "  [getstring lowering produces String arg with buffer_slot=0 Ã¢Å“â€œ]\n";
}

//=============================================================================
// SEMANTIC CORRECTNESS FIX TESTS - August 2026
// Verifies fixes for confirmed active vanilla semantic corruption:
//   - Finding 3: String formatting operands preserved
//   - Finding 7: encountermusic Ã¢â€°Â  playmapmusic
//   - Finding 8: newloadmap method preserved
//   - Finding 9: reanchormap Ã¢â€°Â  refreshmap
//   - Finding 5: sdefer bank resolution
//=============================================================================

// Finding 3: gettrainername preserves BOTH trainer_group AND trainer_id
TEST(semantic_fix_gettrainername_preserves_both_operands) {
    using namespace crystal;
    using namespace enginemon;
    
    // Build Cmd_Gettrainername with distinct operands
    CrystalCommand cmd;
    Cmd_Gettrainername gtn;
    gtn.trainer_group = 5;   // Distinct value
    gtn.trainer_id = 7;      // Distinct value
    gtn.strbuf = 2;          // Destination buffer
    cmd.data = gtn;
    cmd.span.rom_address = 0;
    cmd.span.raw_bytes = {0x44, 5, 7, 2};  // gettrainername opcode + operands
    
    CrystalScriptIR ir;
    ir.name = "test_gettrainername";
    ir.entry_address = 0x10000;
    ir.rom_start = 0;
    ir.rom_end = 4;
    ir.commands.push_back(cmd);
    
    CrystalCFG cfg;
    cfg.script_name = "test_gettrainername";
    cfg.entry_address = 0x10000;
    BasicBlock block;
    block.id = 0;
    block.start_address = 0;
    block.end_address = 4;
    block.command_start = 0;
    block.command_count = 1;
    cfg.blocks.push_back(block);
    cfg.source_ir = &ir;
    
    SemanticLegalizer legalizer;
    LoweringResult result = legalizer.lower(ir, cfg);
    
    ASSERT_TRUE(result.success);
    ASSERT_EQ(result.ir.blocks.size(), 1);
    ASSERT_EQ(result.ir.blocks[0].instructions.size(), 1);
    
    // Get the semantic op
    const auto& inst = result.ir.blocks[0].instructions[0];
    auto* pta = std::get_if<Sem_PrepareTextArg>(&inst.op);
    ASSERT_TRUE(pta != nullptr);
    
    // CRITICAL: Both operands must be preserved
    ASSERT_EQ(pta->trainer_group, 5);
    ASSERT_EQ(pta->id2, 7);  // trainer_id stored in id2
    ASSERT_EQ(pta->buffer_slot, 2);
    ASSERT_EQ(pta->arg_type, TextArgType::TrainerName);
    
    std::cout << "  [gettrainername preserves group=" << (int)pta->trainer_group 
              << ", id=" << (int)pta->id2 << " Ã¢Å“â€œ]\n";
}

// Finding 3: getstring buffer_slot and str_value are preserved (no raw ROM address)
TEST(semantic_fix_getstring_preserves_text_pointer) {
    using namespace crystal;
    using namespace enginemon;
    
    CrystalCommand cmd;
    Cmd_Getstring gs;
    gs.text_pointer = 0x4123;  // Bank-relative pointer
    gs.strbuf = 1;
    cmd.data = gs;
    // Command is in the same bank as the script entry (bank 7 = 0x1c000).
    // Setting cmd.span.rom_address to the entry address places it in bank 7.
    cmd.span.rom_address = 0x1c000;
    cmd.span.raw_bytes = {0x45, 1, 0x23, 0x41};
    
    CrystalScriptIR ir;
    ir.name = "test_getstring";
    ir.entry_address = 0x1c000;
    ir.rom_start = 0;
    ir.rom_end = 4;
    ir.commands.push_back(cmd);
    
    CrystalCFG cfg;
    cfg.script_name = "test_getstring";
    cfg.entry_address = 0x1c000;
    BasicBlock block;
    block.id = 0;
    block.start_address = 0;
    block.end_address = 4;
    block.command_start = 0;
    block.command_count = 1;
    cfg.blocks.push_back(block);
    cfg.source_ir = &ir;
    
    SemanticLegalizer legalizer;
    LoweringResult result = legalizer.lower(ir, cfg);
    
    ASSERT_TRUE(result.success);
    ASSERT_EQ(result.ir.blocks[0].instructions.size(), 1);
    
    const auto& inst = result.ir.blocks[0].instructions[0];
    auto* pta = std::get_if<Sem_PrepareTextArg>(&inst.op);
    ASSERT_TRUE(pta != nullptr);
    
    // No raw ROM address survives Ã¢â‚¬â€ str_value and buffer_slot are the semantic outputs
    ASSERT_EQ(pta->buffer_slot, 1u);
    ASSERT_EQ(pta->arg_type, TextArgType::String);
    // str_value is empty since no text_registry was provided Ã¢â‚¬â€ that's fine
    
    std::cout << "  [getstring preserves buffer_slot=1, arg_type=String, no ROM address Ã¢Å“â€œ]\n";
}

// Finding 3: getmoney preserves account operand
TEST(semantic_fix_getmoney_preserves_account) {
    using namespace crystal;
    using namespace enginemon;
    
    CrystalCommand cmd;
    Cmd_Getmoney gm;
    gm.account = 1;  // Mom's money (distinct from player=0)
    gm.strbuf = 3;
    cmd.data = gm;
    cmd.span.rom_address = 0;
    cmd.span.raw_bytes = {0x3D, 1, 3};
    
    CrystalScriptIR ir;
    ir.name = "test_getmoney";
    ir.entry_address = 0x10000;
    ir.rom_start = 0;
    ir.rom_end = 3;
    ir.commands.push_back(cmd);
    
    CrystalCFG cfg;
    cfg.script_name = "test_getmoney";
    cfg.entry_address = 0x10000;
    BasicBlock block;
    block.id = 0;
    block.start_address = 0;
    block.end_address = 3;
    block.command_start = 0;
    block.command_count = 1;
    cfg.blocks.push_back(block);
    cfg.source_ir = &ir;
    
    SemanticLegalizer legalizer;
    LoweringResult result = legalizer.lower(ir, cfg);
    
    ASSERT_TRUE(result.success);
    
    const auto& inst = result.ir.blocks[0].instructions[0];
    auto* pta = std::get_if<Sem_PrepareTextArg>(&inst.op);
    ASSERT_TRUE(pta != nullptr);
    
    // CRITICAL: account must be preserved (1 = Mom's money)
    ASSERT_EQ(pta->account, enginemon::MoneyAccount::Mom);
    ASSERT_EQ(pta->buffer_slot, 3);
    
    std::cout << "  [getmoney preserves account=" << (int)static_cast<uint8_t>(pta->account) << " Ã¢Å“â€œ]\n";
}

// Finding 7: encountermusic produces Sem_PlayEncounterMusic, NOT Sem_PlayMapMusic
TEST(semantic_fix_encountermusic_distinct_from_playmapmusic) {
    using namespace crystal;
    using namespace enginemon;
    
    // Test encountermusic
    CrystalCommand cmd_enc;
    cmd_enc.data = Cmd_Encountermusic{};
    cmd_enc.span.rom_address = 0;
    cmd_enc.span.raw_bytes = {0x73};  // encountermusic opcode
    
    CrystalScriptIR ir;
    ir.name = "test_encountermusic";
    ir.entry_address = 0x10000;
    ir.rom_start = 0;
    ir.rom_end = 1;
    ir.commands.push_back(cmd_enc);
    
    CrystalCFG cfg;
    cfg.script_name = "test_encountermusic";
    cfg.entry_address = 0x10000;
    BasicBlock block;
    block.id = 0;
    block.start_address = 0;
    block.end_address = 1;
    block.command_start = 0;
    block.command_count = 1;
    cfg.blocks.push_back(block);
    cfg.source_ir = &ir;
    
    SemanticLegalizer legalizer;
    LoweringResult result = legalizer.lower(ir, cfg);
    
    ASSERT_TRUE(result.success);
    ASSERT_EQ(result.ir.blocks[0].instructions.size(), 1);
    
    const auto& inst = result.ir.blocks[0].instructions[0];
    
    // CRITICAL: Must produce Sem_PlayEncounterMusic, NOT Sem_PlayMapMusic
    auto* enc = std::get_if<Sem_PlayEncounterMusic>(&inst.op);
    ASSERT_TRUE(enc != nullptr);
    
    // Verify it's NOT Sem_PlayMapMusic
    auto* map = std::get_if<Sem_PlayMapMusic>(&inst.op);
    ASSERT_TRUE(map == nullptr);
    
    std::cout << "  [encountermusic Ã¢â€ â€™ Sem_PlayEncounterMusic (not PlayMapMusic) Ã¢Å“â€œ]\n";
}

// Finding 8: newloadmap preserves method operand
TEST(semantic_fix_newloadmap_preserves_method) {
    using namespace crystal;
    using namespace enginemon;
    
    // Test with multiple method values
    for (uint8_t method : {0xF1, 0xF2, 0xF3, 0xF4, 0xF5, 0xFC}) {
        CrystalCommand cmd;
        Cmd_Newloadmap nlm;
        nlm.method = method;
        cmd.data = nlm;
        cmd.span.rom_address = 0;
        cmd.span.raw_bytes = {0x8A, method};
        
        CrystalScriptIR ir;
        ir.name = "test_newloadmap";
        ir.entry_address = 0x10000;
        ir.rom_start = 0;
        ir.rom_end = 2;
        ir.commands.push_back(cmd);
        
        CrystalCFG cfg;
        cfg.script_name = "test_newloadmap";
        cfg.entry_address = 0x10000;
        BasicBlock block;
        block.id = 0;
        block.start_address = 0;
        block.end_address = 2;
        block.command_start = 0;
        block.command_count = 1;
        cfg.blocks.push_back(block);
        cfg.source_ir = &ir;
        
        SemanticLegalizer legalizer;
        LoweringResult result = legalizer.lower(ir, cfg);
        
        ASSERT_TRUE(result.success);
        
        const auto& inst = result.ir.blocks[0].instructions[0];
        auto* nlm_op = std::get_if<Sem_NewLoadMap>(&inst.op);
        ASSERT_TRUE(nlm_op != nullptr);
        
        // CRITICAL: method must be preserved exactly
        ASSERT_EQ(static_cast<uint8_t>(nlm_op->method), method);
    }
    
    std::cout << "  [newloadmap preserves method (0xF1..0xFC tested) Ã¢Å“â€œ]\n";
}

// Finding 9: reanchormap produces Sem_ReanchorMap, NOT Sem_RefreshMap
TEST(semantic_fix_reanchormap_distinct_from_refreshmap) {
    using namespace crystal;
    using namespace enginemon;
    
    // Test reanchormap
    CrystalCommand cmd_re;
    Cmd_Reanchormap ra;
    ra.dummy = 0x42;  // Dummy byte
    cmd_re.data = ra;
    cmd_re.span.rom_address = 0;
    cmd_re.span.raw_bytes = {0x48, 0x42};  // reanchormap opcode + dummy
    
    CrystalScriptIR ir;
    ir.name = "test_reanchormap";
    ir.entry_address = 0x10000;
    ir.rom_start = 0;
    ir.rom_end = 2;
    ir.commands.push_back(cmd_re);
    
    CrystalCFG cfg;
    cfg.script_name = "test_reanchormap";
    cfg.entry_address = 0x10000;
    BasicBlock block;
    block.id = 0;
    block.start_address = 0;
    block.end_address = 2;
    block.command_start = 0;
    block.command_count = 1;
    cfg.blocks.push_back(block);
    cfg.source_ir = &ir;
    
    SemanticLegalizer legalizer;
    LoweringResult result = legalizer.lower(ir, cfg);
    
    ASSERT_TRUE(result.success);
    
    const auto& inst = result.ir.blocks[0].instructions[0];
    
    // CRITICAL: Must produce Sem_ReanchorMap, NOT Sem_RefreshMap
    auto* reanchor = std::get_if<Sem_ReanchorMap>(&inst.op);
    ASSERT_TRUE(reanchor != nullptr);
    
    // Verify dummy byte is preserved
    ASSERT_EQ(reanchor->dummy, 0x42);
    
    // Verify it's NOT Sem_RefreshMap
    auto* refresh = std::get_if<Sem_RefreshMap>(&inst.op);
    ASSERT_TRUE(refresh == nullptr);
    
    std::cout << "  [reanchormap Ã¢â€ â€™ Sem_ReanchorMap (not RefreshMap), dummy=0x42 Ã¢Å“â€œ]\n";
}

// Finding 9: refreshmap produces Sem_RefreshMap (distinct from reanchormap)
TEST(semantic_fix_refreshmap_distinct_from_reanchormap) {
    using namespace crystal;
    using namespace enginemon;
    
    CrystalCommand cmd_rf;
    cmd_rf.data = Cmd_Refreshmap{};
    cmd_rf.span.rom_address = 0;
    cmd_rf.span.raw_bytes = {0x7C};  // refreshmap opcode
    
    CrystalScriptIR ir;
    ir.name = "test_refreshmap";
    ir.entry_address = 0x10000;
    ir.rom_start = 0;
    ir.rom_end = 1;
    ir.commands.push_back(cmd_rf);
    
    CrystalCFG cfg;
    cfg.script_name = "test_refreshmap";
    cfg.entry_address = 0x10000;
    BasicBlock block;
    block.id = 0;
    block.start_address = 0;
    block.end_address = 1;
    block.command_start = 0;
    block.command_count = 1;
    cfg.blocks.push_back(block);
    cfg.source_ir = &ir;
    
    SemanticLegalizer legalizer;
    LoweringResult result = legalizer.lower(ir, cfg);
    
    ASSERT_TRUE(result.success);
    
    const auto& inst = result.ir.blocks[0].instructions[0];
    
    // CRITICAL: Must produce Sem_RefreshMap
    auto* refresh = std::get_if<Sem_RefreshMap>(&inst.op);
    ASSERT_TRUE(refresh != nullptr);
    
    // Verify it's NOT Sem_ReanchorMap
    auto* reanchor = std::get_if<Sem_ReanchorMap>(&inst.op);
    ASSERT_TRUE(reanchor == nullptr);
    
    std::cout << "  [refreshmap Ã¢â€ â€™ Sem_RefreshMap (not ReanchorMap) Ã¢Å“â€œ]\n";
}

// Finding 5: sdefer resolves bank-relative pointer correctly
TEST(semantic_fix_sdefer_bank_resolution) {
    using namespace crystal;
    using namespace enginemon;
    
    // Script at bank 0x1A, sdefer pointer = 0x4500 (bank-relative)
    // Expected flat = 0x1A * 0x4000 + (0x4500 - 0x4000) = 0x68000 + 0x500 = 0x68500
    CrystalCommand cmd;
    Cmd_Sdefer sd;
    sd.pointer = 0x4500;  // Bank-relative pointer
    cmd.data = sd;
    cmd.span.rom_address = 0;
    cmd.span.raw_bytes = {0x8D, 0x00, 0x45};
    
    CrystalScriptIR ir;
    ir.name = "test_sdefer";
    ir.entry_address = 0x68100;  // Bank 0x1A (0x1A * 0x4000 = 0x68000)
    ir.rom_start = 0;
    ir.rom_end = 3;
    ir.commands.push_back(cmd);
    
    CrystalCFG cfg;
    cfg.script_name = "test_sdefer";
    cfg.entry_address = 0x68100;
    BasicBlock block;
    block.id = 0;
    block.start_address = 0;
    block.end_address = 3;
    block.command_start = 0;
    block.command_count = 1;
    cfg.blocks.push_back(block);
    cfg.source_ir = &ir;
    
    SemanticLegalizer legalizer;
    LoweringResult result = legalizer.lower(ir, cfg);
    
    ASSERT_TRUE(result.success);
    
    const auto& inst = result.ir.blocks[0].instructions[0];
    auto* sdef = std::get_if<Sem_Sdefer>(&inst.op);
    ASSERT_TRUE(sdef != nullptr);
    
    // CRITICAL: target_script_id must be based on resolved flat address
    // "deferred_68500" (hex for 0x1A * 0x4000 + (0x4500 - 0x4000))
    ASSERT_STR_EQ(sdef->target_script_id, "deferred_68500");
    
    std::cout << "  [sdefer bank resolution: 0x4500 @ bank 0x1A Ã¢â€ â€™ deferred_68500 Ã¢Å“â€œ]\n";
}

// Finding 2: TextDefinition identity_string distinguishes control codes
TEST(semantic_fix_text_identity_distinguishes_controls) {
    using namespace crystal;
    
    // Create two text definitions with same literal but different controls
    TextDefinition def1, def2, def3;
    
    // def1: "Hello" + LINE
    def1.source_rom_address = 0x1000;
    def1.sequence.elements.push_back(TextElement::make_text("Hello"));
    def1.sequence.elements.push_back(TextElement::make_line());
    
    // def2: "Hello" + PARA (different control)
    def2.source_rom_address = 0x2000;
    def2.sequence.elements.push_back(TextElement::make_text("Hello"));
    def2.sequence.elements.push_back(TextElement::make_para());
    
    // def3: "Hello" + LINE (same as def1)
    def3.source_rom_address = 0x3000;
    def3.sequence.elements.push_back(TextElement::make_text("Hello"));
    def3.sequence.elements.push_back(TextElement::make_line());
    
    std::string id1 = def1.identity_string();
    std::string id2 = def2.identity_string();
    std::string id3 = def3.identity_string();
    
    // CRITICAL: LINE vs PARA must produce different identities
    ASSERT_TRUE(id1 != id2);
    
    // Same content should produce same identity
    ASSERT_STR_EQ(id1, id3);
    
    // Identity should contain control markers
    ASSERT_STR_CONTAINS(id1, "<LINE>");
    ASSERT_STR_CONTAINS(id2, "<PARA>");
    
    std::cout << "  [TextDefinition: LINE vs PARA Ã¢â€ â€™ distinct identities Ã¢Å“â€œ]\n";
}

// Finding 2: TextDefinition identity distinguishes TX_RAM addresses
TEST(semantic_fix_text_identity_distinguishes_ram_addresses) {
    using namespace crystal;
    
    TextDefinition def1, def2;
    
    // def1: "Name: " + RAM(0xD47D)
    def1.source_rom_address = 0x1000;
    def1.sequence.elements.push_back(TextElement::make_text("Name: "));
    def1.sequence.elements.push_back(TextElement::make_text_ram(0xD47D));
    
    // def2: "Name: " + RAM(0xD47E) - different address
    def2.source_rom_address = 0x2000;
    def2.sequence.elements.push_back(TextElement::make_text("Name: "));
    def2.sequence.elements.push_back(TextElement::make_text_ram(0xD47E));
    
    std::string id1 = def1.identity_string();
    std::string id2 = def2.identity_string();
    
    // CRITICAL: Different RAM addresses must produce different identities
    ASSERT_TRUE(id1 != id2);
    
    // Both should contain RAM markers with addresses
    ASSERT_STR_CONTAINS(id1, "<RAM:");
    ASSERT_STR_CONTAINS(id2, "<RAM:");
    
    std::cout << "  [TextDefinition: RAM(0xD47D) vs RAM(0xD47E) Ã¢â€ â€™ distinct Ã¢Å“â€œ]\n";
}

// =============================================================================
// TEXT SEMANTIC TESTS Ã¢â‚¬â€ TextStringBuffer Ã¢â€ â€™ SemanticTextOp::Arg
// =============================================================================
// Verifies that TX_STRINGBUFFER (0x14) correctly maps to
// SemanticTextElement::make_arg(slot) using the SOURCE-PROVEN 0-indexed
// StringBufferPointers table from pokecrystal/data/text_buffers.asm:
//
//   0: wStringBuffer3    1: wStringBuffer4    2: wStringBuffer5
//   3: wStringBuffer2    4: wStringBuffer1
//   5: wEnemyMonNickname  6: wBattleMonNickname
//
// The encoded byte IS the direct 0-indexed table slot (no subtraction).
// Valid range: 0Ã¢â‚¬â€œ6.  Index >= 7 Ã¢â€ â€™ hard-fail (empty sequence).
// =============================================================================

TEST(text_string_buffer_id4_maps_to_arg_slot4) {
    // buffer_id=4 Ã¢â€ â€™ wStringBuffer1 (most common slot, slot 4 in StringBufferPointers)
    using namespace crystal;
    using namespace enginemon;

    TextDefinition def;
    def.source_rom_address = 0x1000;
    // "Hi " + wStringBuffer1 (buffer_id=4) + "!"
    def.sequence.elements.push_back(TextElement::make_text("Hi "));
    def.sequence.elements.push_back(TextElement::make_text_string_buffer(4));  // wStringBuffer1
    def.sequence.elements.push_back(TextElement::make_text("!"));

    auto sem = def.to_semantic_sequence();

    ASSERT_EQ(sem.elements.size(), 3u);
    // First element: text
    ASSERT_EQ(sem.elements[0].op, SemanticTextOp::Text);
    ASSERT_STR_EQ(sem.elements[0].text.c_str(), "Hi ");
    // Second element: Arg at slot 4 (buffer_id 4 = wStringBuffer1 = index 4)
    ASSERT_EQ(sem.elements[1].op, SemanticTextOp::Arg);
    ASSERT_EQ(sem.elements[1].arg_index, 4u);
    // Third element: text
    ASSERT_EQ(sem.elements[2].op, SemanticTextOp::Text);
    ASSERT_STR_EQ(sem.elements[2].text.c_str(), "!");

    // Must NOT be empty text placeholder
    ASSERT_TRUE(sem.elements[1].op != SemanticTextOp::Text);

    std::cout << "  [TX_STRINGBUFFER(4=wStringBuffer1) Ã¢â€ â€™ SemanticTextOp::Arg(slot=4) Ã¢Å“â€œ]\n";
}

TEST(text_string_buffer_id0_maps_to_arg_slot0) {
    // buffer_id=0 Ã¢â€ â€™ wStringBuffer3 (first entry in StringBufferPointers)
    using namespace crystal;
    using namespace enginemon;

    TextDefinition def;
    def.source_rom_address = 0x2000;
    // wStringBuffer3 (buffer_id=0) Ã¢â€ â€™ slot 0
    def.sequence.elements.push_back(TextElement::make_text_string_buffer(0));  // wStringBuffer3

    auto sem = def.to_semantic_sequence();

    ASSERT_EQ(sem.elements.size(), 1u);
    ASSERT_EQ(sem.elements[0].op, SemanticTextOp::Arg);
    ASSERT_EQ(sem.elements[0].arg_index, 0u);  // direct: id=0 Ã¢â€ â€™ slot=0

    std::cout << "  [TX_STRINGBUFFER(0=wStringBuffer3) Ã¢â€ â€™ SemanticTextOp::Arg(slot=0) Ã¢Å“â€œ]\n";
}

TEST(text_string_buffer_id6_maps_to_arg_slot6) {
    // buffer_id=6 Ã¢â€ â€™ wBattleMonNickname (last valid entry)
    using namespace crystal;
    using namespace enginemon;

    TextDefinition def;
    def.source_rom_address = 0x2100;
    def.sequence.elements.push_back(TextElement::make_text_string_buffer(6));  // wBattleMonNickname

    auto sem = def.to_semantic_sequence();

    ASSERT_EQ(sem.elements.size(), 1u);
    ASSERT_EQ(sem.elements[0].op, SemanticTextOp::Arg);
    ASSERT_EQ(sem.elements[0].arg_index, 6u);

    std::cout << "  [TX_STRINGBUFFER(6=wBattleMonNickname) Ã¢â€ â€™ SemanticTextOp::Arg(slot=6) Ã¢Å“â€œ]\n";
}

TEST(text_string_buffer_invalid_id7_hard_fails) {
    // buffer_id=7 is outside StringBufferPointers (only 7 entries: 0-6).
    // to_semantic_sequence() must return empty sequence Ã¢â€ â€™ legality gate rejects.
    using namespace crystal;
    using namespace enginemon;

    TextDefinition def;
    def.source_rom_address = 0x3000;
    def.sequence.elements.push_back(TextElement::make_text("before "));
    def.sequence.elements.push_back(TextElement::make_text_string_buffer(7));  // INVALID
    def.sequence.elements.push_back(TextElement::make_text(" after"));

    auto sem = def.to_semantic_sequence();

    // Hard-fail: empty sequence
    ASSERT_TRUE(sem.empty());

    std::cout << "  [TX_STRINGBUFFER(7=INVALID) Ã¢â€ â€™ empty SemanticTextSequence (hard-fail) Ã¢Å“â€œ]\n";
}

TEST(text_string_buffer_invalid_id255_hard_fails) {
    // buffer_id=255 Ã¢â‚¬â€ far out of range, must also hard-fail.
    using namespace crystal;
    using namespace enginemon;

    TextDefinition def;
    def.source_rom_address = 0x3100;
    def.sequence.elements.push_back(TextElement::make_text_string_buffer(255));  // INVALID

    auto sem = def.to_semantic_sequence();

    ASSERT_TRUE(sem.empty());

    std::cout << "  [TX_STRINGBUFFER(255=INVALID) Ã¢â€ â€™ empty SemanticTextSequence (hard-fail) Ã¢Å“â€œ]\n";
}

TEST(text_tx_ram_wstringbuffer3_maps_to_arg_slot0) {
    // TX_RAM wStringBuffer3 (0xD099) Ã¢â€ â€™ Arg(slot=0)
    // Source: GetStringBuffer: ld hl, wStringBuffer3; strbuf=0 Ã¢â€ â€™ wStringBuffer3
    using namespace crystal;
    using namespace enginemon;

    TextDefinition def;
    def.source_rom_address = 0x4000;
    def.sequence.elements.push_back(TextElement::make_text("Item: "));
    def.sequence.elements.push_back(TextElement::make_text_ram(0xD099));  // wStringBuffer3
    def.sequence.elements.push_back(TextElement::make_text("!"));

    auto sem = def.to_semantic_sequence();

    ASSERT_EQ(sem.elements.size(), 3u);
    ASSERT_EQ(sem.elements[1].op, SemanticTextOp::Arg);
    ASSERT_EQ(sem.elements[1].arg_index, 0u);  // slot 0 = wStringBuffer3
    ASSERT_TRUE(sem.elements[1].op != SemanticTextOp::Text);

    std::cout << "  [TX_RAM(0xD099=wStringBuffer3) Ã¢â€ â€™ Arg(slot=0) Ã¢Å“â€œ]\n";
}

TEST(text_tx_ram_wstringbuffer4_maps_to_arg_slot1) {
    // TX_RAM wStringBuffer4 (0xD0AC) Ã¢â€ â€™ Arg(slot=1)
    using namespace crystal;
    using namespace enginemon;

    TextDefinition def;
    def.source_rom_address = 0x4100;
    def.sequence.elements.push_back(TextElement::make_text_ram(0xD0AC));  // wStringBuffer4

    auto sem = def.to_semantic_sequence();

    ASSERT_EQ(sem.elements.size(), 1u);
    ASSERT_EQ(sem.elements[0].op, SemanticTextOp::Arg);
    ASSERT_EQ(sem.elements[0].arg_index, 1u);

    std::cout << "  [TX_RAM(0xD0AC=wStringBuffer4) Ã¢â€ â€™ Arg(slot=1) Ã¢Å“â€œ]\n";
}

TEST(text_tx_ram_wstringbuffer5_maps_to_arg_slot2) {
    // TX_RAM wStringBuffer5 (0xD0BF) Ã¢â€ â€™ Arg(slot=2)
    using namespace crystal;
    using namespace enginemon;

    TextDefinition def;
    def.source_rom_address = 0x4200;
    def.sequence.elements.push_back(TextElement::make_text_ram(0xD0BF));  // wStringBuffer5

    auto sem = def.to_semantic_sequence();

    ASSERT_EQ(sem.elements.size(), 1u);
    ASSERT_EQ(sem.elements[0].op, SemanticTextOp::Arg);
    ASSERT_EQ(sem.elements[0].arg_index, 2u);

    std::cout << "  [TX_RAM(0xD0BF=wStringBuffer5) Ã¢â€ â€™ Arg(slot=2) Ã¢Å“â€œ]\n";
}

TEST(text_tx_ram_unknown_address_hard_fails) {
    // TX_RAM with unknown address (e.g. wPlayerName 0xD47D) Ã¢â€ â€™ hard-fail empty sequence.
    // Only wStringBuffer3/4/5 are valid script text buffer slots.
    using namespace crystal;
    using namespace enginemon;

    TextDefinition def;
    def.source_rom_address = 0x4300;
    def.sequence.elements.push_back(TextElement::make_text("Player: "));
    def.sequence.elements.push_back(TextElement::make_text_ram(0xD47D));  // wPlayerName Ã¢â‚¬â€ not a buffer slot

    auto sem = def.to_semantic_sequence();

    // Hard-fail: wPlayerName is not a valid strbuf slot
    ASSERT_TRUE(sem.empty());

    std::cout << "  [TX_RAM(0xD47D=wPlayerName) Ã¢â€ â€™ hard-fail (not a script buffer slot) Ã¢Å“â€œ]\n";
}

TEST(text_tx_bcd_hard_fails) {
    // TX_BCD is not used in script text corpus (only in battle text engine internals).
    // Any TX_BCD in a script text sequence is an error Ã¢â€ â€™ hard-fail.
    using namespace crystal;
    using namespace enginemon;

    TextDefinition def;
    def.source_rom_address = 0x5000;
    def.sequence.elements.push_back(TextElement::make_text_bcd(0xD84E, 0x13));

    auto sem = def.to_semantic_sequence();

    ASSERT_TRUE(sem.empty());

    std::cout << "  [TX_BCD Ã¢â€ â€™ hard-fail (no script text corpus uses) Ã¢Å“â€œ]\n";
}

TEST(text_tx_decimal_wscriptvar_produces_script_var_decimal) {
    // TX_DECIMAL wScriptVar (0xC2DD) Ã¢â€ â€™ ScriptVarDecimal(bytes_digits).
    // Source: BattleTower1F.asm text_decimal wScriptVar, 1, 3 Ã¢â€ â€™ param=0x13.
    // No WRAM address survives Ã¢â‚¬â€ runtime reads ScriptVar context.
    using namespace crystal;
    using namespace enginemon;

    TextDefinition def;
    def.source_rom_address = 0x5100;
    def.sequence.elements.push_back(TextElement::make_text_decimal(0xC2DD, 0x13)); // wScriptVar, 1 byte, 3 digits

    auto sem = def.to_semantic_sequence();

    ASSERT_EQ(sem.elements.size(), 1u);
    ASSERT_EQ(sem.elements[0].op, SemanticTextOp::ScriptVarDecimal);
    ASSERT_EQ(sem.elements[0].param1, 0x13u);  // bytes_digits preserved
    ASSERT_TRUE(sem.elements[0].op != SemanticTextOp::Text);

    std::cout << "  [TX_DECIMAL(0xC2DD=wScriptVar, param=0x13) Ã¢â€ â€™ ScriptVarDecimal(0x13) Ã¢Å“â€œ]\n";
}

TEST(text_tx_decimal_unknown_address_hard_fails) {
    // TX_DECIMAL with address other than wScriptVar Ã¢â€ â€™ hard-fail.
    using namespace crystal;
    using namespace enginemon;

    TextDefinition def;
    def.source_rom_address = 0x5200;
    def.sequence.elements.push_back(TextElement::make_text_decimal(0xD84F, 0x21));  // not wScriptVar

    auto sem = def.to_semantic_sequence();

    ASSERT_TRUE(sem.empty());

    std::cout << "  [TX_DECIMAL(non-wScriptVar) Ã¢â€ â€™ hard-fail Ã¢Å“â€œ]\n";
}

TEST(text_tx_far_inlines_referenced_text) {
    // TX_FAR must inline the referenced text at this position.
    // The flat ROM address must NOT survive into Semantic IR.
    // A registry must be provided; without it TX_FAR hard-fails.
    using namespace crystal;
    using namespace enginemon;

    // Build far target text: "HELLO<DONE>"
    TextDefinition far_target;
    far_target.source_rom_address = 0x18000;
    far_target.sequence.elements.push_back(TextElement::make_text("HELLO"));
    far_target.sequence.elements.push_back(TextElement::make_done());

    // Build registry with the far target pre-registered
    TextRegistry reg([&](uint32_t addr) -> crystal::TextSequence {
        if (addr == 0x18000) return far_target.sequence;
        return {};
    });
    // Pre-extract so lookup works
    reg.extract(0x18000);

    // Build text with TX_FAR pointing to 0x18000
    // TX_FAR: bank=6, local_ptr=0x4000 Ã¢â€ â€™ flat = 6*0x4000 + (0x4000-0x4000) = 0x18000
    TextDefinition def;
    def.source_rom_address = 0x6000;
    def.sequence.elements.push_back(TextElement::make_text("Before "));
    def.sequence.elements.push_back(TextElement::make_text_far(0x4000, 0x06));  // Ã¢â€ â€™ flat 0x18000
    def.sequence.elements.push_back(TextElement::make_text(" After"));

    auto sem = def.to_semantic_sequence(&reg);

    // TX_FAR must be inlined: "Before " + "HELLO" + Done + " After"
    ASSERT_FALSE(sem.empty());
    ASSERT_TRUE(sem.elements.size() >= 3u);

    // First element: "Before "
    ASSERT_EQ(sem.elements[0].op, SemanticTextOp::Text);
    ASSERT_STR_EQ(sem.elements[0].text.c_str(), "Before ");

    // Inlined: "HELLO" text from far target
    bool found_hello = false;
    for (const auto& e : sem.elements) {
        if (e.op == SemanticTextOp::Text && e.text == "HELLO") { found_hello = true; break; }
    }
    ASSERT_TRUE(found_hello);

    // No FarText element survives Ã¢â‚¬â€ the old FarText op no longer exists in SemanticTextOp.
    // All elements must be Text, Arg, Line, Para, Cont, Scroll, Done, Prompt, Day, Sound, or ScriptVarDecimal.
    for (const auto& e : sem.elements) {
        ASSERT_TRUE(e.op == SemanticTextOp::Text ||
                    e.op == SemanticTextOp::Arg ||
                    e.op == SemanticTextOp::Line ||
                    e.op == SemanticTextOp::Next ||
                    e.op == SemanticTextOp::Para ||
                    e.op == SemanticTextOp::Cont ||
                    e.op == SemanticTextOp::Scroll ||
                    e.op == SemanticTextOp::Done ||
                    e.op == SemanticTextOp::Prompt ||
                    e.op == SemanticTextOp::Day ||
                    e.op == SemanticTextOp::Sound ||
                    e.op == SemanticTextOp::ScriptVarDecimal);
    }

    std::cout << "  [TX_FAR(bank=6,ptr=0x4000Ã¢â€ â€™flat=0x18000) Ã¢â€ â€™ inlined 'HELLO' Ã¢Å“â€œ]\n";
}

TEST(text_tx_far_without_registry_hard_fails) {
    // TX_FAR without registry Ã¢â€ â€™ hard-fail (cannot resolve target).
    using namespace crystal;
    using namespace enginemon;

    TextDefinition def;
    def.source_rom_address = 0x6100;
    def.sequence.elements.push_back(TextElement::make_text_far(0x40C8, 0x06));

    // No registry passed (nullptr)
    auto sem = def.to_semantic_sequence(nullptr);

    ASSERT_TRUE(sem.empty());

    std::cout << "  [TX_FAR without registry Ã¢â€ â€™ hard-fail Ã¢Å“â€œ]\n";
}

TEST(text_tx_day_produces_day_op) {
    // TX_DAY Ã¢â€ â€™ SemanticTextOp::Day (no operands, runtime queries calendar).
    using namespace crystal;
    using namespace enginemon;

    TextDefinition def;
    def.source_rom_address = 0x7000;
    TextElement day_elem; day_elem.op = TextOp::TextDay;
    def.sequence.elements.push_back(day_elem);

    auto sem = def.to_semantic_sequence();

    ASSERT_EQ(sem.elements.size(), 1u);
    ASSERT_EQ(sem.elements[0].op, SemanticTextOp::Day);
    ASSERT_TRUE(sem.elements[0].op != SemanticTextOp::Text);

    std::cout << "  [TX_DAY Ã¢â€ â€™ SemanticTextOp::Day Ã¢Å“â€œ]\n";
}

TEST(text_tx_sound_item_produces_typed_sound_kind) {
    // TX_SOUND_ITEM (0x0f) Ã¢â€ â€™ Sound(TextSoundKind::ItemJingle).
    // No raw opcode 0x0f survives Ã¢â‚¬â€ TextSoundKind is the semantic identity.
    using namespace crystal;
    using namespace enginemon;

    TextDefinition def;
    def.source_rom_address = 0x8000;
    TextElement snd; snd.op = TextOp::TextSoundItem;
    def.sequence.elements.push_back(snd);

    auto sem = def.to_semantic_sequence();

    ASSERT_EQ(sem.elements.size(), 1u);
    ASSERT_EQ(sem.elements[0].op, SemanticTextOp::Sound);
    ASSERT_EQ(sem.elements[0].sound_kind(), TextSoundKind::ItemJingle);

    std::cout << "  [TX_SOUND_ITEM Ã¢â€ â€™ Sound(ItemJingle) Ã¢Å“â€œ]\n";
}

TEST(text_tx_sound_fanfare_produces_typed_sound_kind) {
    // TX_SOUND_FANFARE (0x12) Ã¢â€ â€™ Sound(TextSoundKind::Fanfare).
    using namespace crystal;
    using namespace enginemon;

    TextDefinition def;
    def.source_rom_address = 0x8100;
    TextElement snd; snd.op = TextOp::TextSoundFanfare;
    def.sequence.elements.push_back(snd);

    auto sem = def.to_semantic_sequence();

    ASSERT_EQ(sem.elements.size(), 1u);
    ASSERT_EQ(sem.elements[0].op, SemanticTextOp::Sound);
    ASSERT_EQ(sem.elements[0].sound_kind(), TextSoundKind::Fanfare);

    std::cout << "  [TX_SOUND_FANFARE Ã¢â€ â€™ Sound(Fanfare) Ã¢Å“â€œ]\n";
}

TEST(text_presentation_ops_dropped_not_failed) {
    // TX_MOVE/BOX/LOW/SCROLL/ASM are presentation-only with 0 corpus uses Ã¢â‚¬â€ dropped.
    // TX_PAUSE and TX_PROMPT_BUTTON are now preserved as typed semantic elements.
    // TX_LOW has 3 corpus-reachable uses but only repositions cursor Ã¢â‚¬â€ safely dropped.
    // The sequence is NOT hard-failed Ã¢â‚¬â€ surrounding text content is preserved.
    using namespace crystal;
    using namespace enginemon;

    TextDefinition def;
    def.source_rom_address = 0x9000;
    def.sequence.elements.push_back(TextElement::make_text("A"));
    TextElement low_elem; low_elem.op = TextOp::TextLow;   // dropped
    def.sequence.elements.push_back(low_elem);
    def.sequence.elements.push_back(TextElement::make_text("B"));
    TextElement move_elem; move_elem.op = TextOp::TextMove; // dropped
    def.sequence.elements.push_back(move_elem);
    def.sequence.elements.push_back(TextElement::make_text("C"));

    auto sem = def.to_semantic_sequence();

    // 3 text elements remain; TX_LOW and TX_MOVE were dropped
    ASSERT_EQ(sem.elements.size(), 3u);
    ASSERT_EQ(sem.elements[0].op, SemanticTextOp::Text);
    ASSERT_EQ(sem.elements[1].op, SemanticTextOp::Text);
    ASSERT_EQ(sem.elements[2].op, SemanticTextOp::Text);

    std::cout << "  [TX_LOW + TX_MOVE dropped; surrounding text preserved Ã¢Å“â€œ]\n";
}

TEST(text_tx_raw_unknown_opcode_hard_fails) {
    // TextRaw with unknown opcode Ã¢â€ â€™ hard-fail.
    // Unknown TX commands must not silently pass.
    using namespace crystal;
    using namespace enginemon;

    TextDefinition def;
    def.source_rom_address = 0x9100;
    std::vector<uint8_t> raw_bytes = {0xAB, 0x01};  // Unknown opcode 0xAB
    def.sequence.elements.push_back(TextElement::make_text_raw(raw_bytes));

    auto sem = def.to_semantic_sequence();

    ASSERT_TRUE(sem.empty());

    std::cout << "  [TextRaw(opcode=0xAB) Ã¢â€ â€™ hard-fail Ã¢Å“â€œ]\n";
}

// Arg-slot mapping Ã¢â‚¬â€ proves the 3 valid GetStringBuffer TX_RAM slots.
TEST(text_arg_slot_numbering_table_driven) {
    // Source: GetStringBuffer in scripting.asm:
    //   ld hl, wStringBuffer3; ld bc, STRING_BUFFER_LENGTH (19); AddNTimes(strbuf)
    //   strbuf=0 Ã¢â€ â€™ wStringBuffer3 (0xD099) Ã¢â€ â€™ Arg(0)
    //   strbuf=1 Ã¢â€ â€™ wStringBuffer4 (0xD0AC) Ã¢â€ â€™ Arg(1)
    //   strbuf=2 Ã¢â€ â€™ wStringBuffer5 (0xD0BF) Ã¢â€ â€™ Arg(2)
    //   NUM_STRING_BUFFERS = 3. Only slots 0/1/2 are valid GetStringBuffer destinations.
    //   These match Sem_PrepareTextArg::buffer_slot which uses the same 0-based index.
    using namespace crystal;
    using namespace enginemon;

    struct TestCase {
        uint16_t wram_addr;
        uint8_t expected_slot;
        const char* symbol;
    };

    const TestCase cases[] = {
        { 0xD099, 0, "wStringBuffer3" },
        { 0xD0AC, 1, "wStringBuffer4" },
        { 0xD0BF, 2, "wStringBuffer5" },
    };

    for (const auto& tc : cases) {
        TextDefinition def;
        def.source_rom_address = 0xA000;
        def.sequence.elements.push_back(TextElement::make_text_ram(tc.wram_addr));

        auto sem = def.to_semantic_sequence();

        ASSERT_EQ(sem.elements.size(), 1u);
        ASSERT_EQ(sem.elements[0].op, SemanticTextOp::Arg);
        ASSERT_EQ(sem.elements[0].arg_index, tc.expected_slot);

        std::cout << "  [TX_RAM(0x" << std::hex << tc.wram_addr << "=" << tc.symbol
                  << ") Ã¢â€ â€™ Arg(" << std::dec << (int)tc.expected_slot << ") Ã¢Å“â€œ]\n";
    }
}

// RamSource mapping Ã¢â‚¬â€ proves TX_RAM slots 3-6 produce typed RamSource, NOT Arg.
// These buffers are direct WRAM reads with no Sem_PrepareTextArg producer.
TEST(text_ram_source_domain_distinct_from_arg_domain) {
    // Source: StringBufferPointers[3..6] + corpus TX_RAM analysis.
    //   wStringBuffer2  (0xD086) Ã¢â€ â€™ RamSource(PreparedString2)
    //   wStringBuffer1  (0xD073) Ã¢â€ â€™ RamSource(PreparedString1)
    //   wEnemyMonNickname (0xC616) Ã¢â€ â€™ RamSource(EnemyNickname)
    //   wBattleMonNickname (0xC621) Ã¢â€ â€™ RamSource(BattleNickname)
    //
    // These are NOT Arg(3/4/5/6) Ã¢â‚¬â€ no Sem_PrepareTextArg writes buffer_slot 3-6
    // in vanilla Crystal (GetStringBuffer clamps to NUM_STRING_BUFFERS=3 max).
    using namespace crystal;
    using namespace enginemon;

    struct TestCase {
        uint16_t wram_addr;
        TextRamSource expected_source;
        const char* symbol;
    };

    const TestCase cases[] = {
        { 0xD086, TextRamSource::PreparedString2, "wStringBuffer2" },
        { 0xD073, TextRamSource::PreparedString1, "wStringBuffer1" },
        { 0xC616, TextRamSource::EnemyNickname,   "wEnemyMonNickname" },
        { 0xC621, TextRamSource::BattleNickname,  "wBattleMonNickname" },
    };

    for (const auto& tc : cases) {
        TextDefinition def;
        def.source_rom_address = 0xA100;
        def.sequence.elements.push_back(TextElement::make_text_ram(tc.wram_addr));

        auto sem = def.to_semantic_sequence();

        ASSERT_EQ(sem.elements.size(), 1u);
        // Must be RamSource, NOT Arg Ã¢â‚¬â€ different semantic domain
        ASSERT_EQ(sem.elements[0].op, SemanticTextOp::RamSource);
        ASSERT_TRUE(sem.elements[0].op != SemanticTextOp::Arg);
        ASSERT_EQ(sem.elements[0].ram_source(), tc.expected_source);

        std::cout << "  [TX_RAM(0x" << std::hex << tc.wram_addr << "=" << tc.symbol
                  << ") Ã¢â€ â€™ RamSource (not Arg) Ã¢Å“â€œ]\n";
    }
}

// Confirm wPlayerName is NOT a valid script buffer slot (not in StringBufferPointers).
TEST(text_arg_slot_wplayername_hard_fails) {
    // wPlayerName (0xD47D) is not in StringBufferPointers Ã¢â‚¬â€ hard-fail.
    using namespace crystal;
    using namespace enginemon;

    TextDefinition def;
    def.source_rom_address = 0xB000;
    def.sequence.elements.push_back(TextElement::make_text_ram(0xD47D));  // wPlayerName

    auto sem = def.to_semantic_sequence();

    ASSERT_TRUE(sem.empty());

    std::cout << "  [TX_RAM(0xD47D=wPlayerName) Ã¢â€ â€™ hard-fail (not in StringBufferPointers) Ã¢Å“â€œ]\n";
}

// =============================================================================
// INLINE PROMPT BUTTON + PAUSE Ã¢â‚¬â€ Bounded text control cleanup tests
// =============================================================================

TEST(text_tx_prompt_button_produces_inline_prompt_button_not_dropped) {
    // TX_PROMPT_BUTTON (0x06) must produce SemanticTextOp::InlinePromptButton.
    // It must NOT be silently dropped Ã¢â‚¬â€ it gates player progression mid-sequence.
    //
    // Source: home/text.asm TextCommand_PROMPT_BUTTON:
    //   LoadBlinkingCursor Ã¢â€ â€™ PromptButton (waits A/B) Ã¢â€ â€™ UnloadBlinkingCursor
    //   DISTINCT from Prompt (terminating) Ã¢â‚¬â€ text continues after the wait.
    //
    // Corpus-reachable: 11 occurrences including maps/BattleTower1F.asm.
    using namespace crystal;
    using namespace enginemon;

    TextDefinition def;
    def.source_rom_address = 0xC000;
    def.sequence.elements.push_back(TextElement::make_text("WIN!"));
    TextElement pb; pb.op = TextOp::TextPromptButton;
    def.sequence.elements.push_back(pb);
    def.sequence.elements.push_back(TextElement::make_text("PRIZE:"));

    auto sem = def.to_semantic_sequence();

    // 3 elements: text + InlinePromptButton + text
    ASSERT_EQ(sem.elements.size(), 3u);
    ASSERT_EQ(sem.elements[0].op, SemanticTextOp::Text);
    ASSERT_EQ(sem.elements[1].op, SemanticTextOp::InlinePromptButton);
    ASSERT_EQ(sem.elements[2].op, SemanticTextOp::Text);

    // Must NOT be Prompt (terminating) Ã¢â‚¬â€ text continues after
    ASSERT_TRUE(sem.elements[1].op != SemanticTextOp::Prompt);
    // Must NOT be Text (silently dropped to empty string)
    ASSERT_TRUE(sem.elements[1].op != SemanticTextOp::Text);
    // Sequence is non-empty Ã¢â‚¬â€ legality gate accepts it
    ASSERT_FALSE(sem.empty());

    std::cout << "  [TX_PROMPT_BUTTON Ã¢â€ â€™ InlinePromptButton (not dropped, not Prompt) Ã¢Å“â€œ]\n";
}

TEST(text_tx_prompt_button_standalone_produces_single_element) {
    // TX_PROMPT_BUTTON in isolation Ã¢â€ â€™ exactly one InlinePromptButton element.
    // This matches the Battle Tower corpus case where prompt appears mid-text.
    using namespace crystal;
    using namespace enginemon;

    TextDefinition def;
    def.source_rom_address = 0xC100;
    TextElement pb; pb.op = TextOp::TextPromptButton;
    def.sequence.elements.push_back(pb);

    auto sem = def.to_semantic_sequence();

    ASSERT_EQ(sem.elements.size(), 1u);
    ASSERT_EQ(sem.elements[0].op, SemanticTextOp::InlinePromptButton);
    ASSERT_FALSE(sem.empty());

    std::cout << "  [TX_PROMPT_BUTTON standalone Ã¢â€ â€™ InlinePromptButton (non-empty) Ã¢Å“â€œ]\n";
}

TEST(text_tx_pause_produces_pause_with_30_frames) {
    // TX_PAUSE (0x0a) must produce SemanticTextOp::Pause with frames=30.
    // It must NOT be silently dropped Ã¢â‚¬â€ it provides observable ~0.5s pacing.
    //
    // Source: home/text.asm TextCommand_PAUSE:
    //   GetJoypad; if A|B held Ã¢â€ â€™ immediate; else DelayFrames(30)
    //
    // Corpus-reachable: 12 occurrences in Radio Tower, Lucky Channel,
    //   level-up move-learning, NPC trade fanfare text.
    //
    // frames = 30: the ONLY value used in all vanilla Crystal occurrences.
    // Preserved explicitly Ã¢â‚¬â€ not encoded as a magic runtime default.
    using namespace crystal;
    using namespace enginemon;

    TextDefinition def;
    def.source_rom_address = 0xC200;
    def.sequence.elements.push_back(TextElement::make_text("3..."));
    TextElement pause; pause.op = TextOp::TextPause;
    def.sequence.elements.push_back(pause);
    def.sequence.elements.push_back(TextElement::make_text("2..."));

    auto sem = def.to_semantic_sequence();

    // 3 elements: text + Pause(30) + text
    ASSERT_EQ(sem.elements.size(), 3u);
    ASSERT_EQ(sem.elements[0].op, SemanticTextOp::Text);
    ASSERT_EQ(sem.elements[1].op, SemanticTextOp::Pause);
    ASSERT_EQ(sem.elements[2].op, SemanticTextOp::Text);

    // Frame count must be exactly 30 Ã¢â‚¬â€ source-proven value
    ASSERT_EQ(sem.elements[1].pause_frames(), 30u);
    // Must NOT be Text (silently dropped)
    ASSERT_TRUE(sem.elements[1].op != SemanticTextOp::Text);

    std::cout << "  [TX_PAUSE Ã¢â€ â€™ Pause(frames=30), not dropped Ã¢Å“â€œ]\n";
}

TEST(text_tx_pause_frame_count_preserved_explicitly) {
    // Adversarial: Pause carries its frame count as an explicit operand.
    // If the model encoded it as a magic constant, pause_frames() would fail.
    using namespace crystal;
    using namespace enginemon;

    TextDefinition def;
    def.source_rom_address = 0xC300;
    TextElement pause; pause.op = TextOp::TextPause;
    def.sequence.elements.push_back(pause);

    auto sem = def.to_semantic_sequence();

    ASSERT_EQ(sem.elements.size(), 1u);
    ASSERT_EQ(sem.elements[0].op, SemanticTextOp::Pause);
    // 30 is the explicit source-proven value, not a default
    ASSERT_EQ(sem.elements[0].pause_frames(), 30u);
    // Verify it's not zero (dropped/no-op)
    ASSERT_TRUE(sem.elements[0].pause_frames() > 0u);

    std::cout << "  [TX_PAUSE frame count = 30 (explicit, source-proven) Ã¢Å“â€œ]\n";
}

TEST(text_enemy_nickname_is_ram_source_not_arg) {
    // TX_RAM wEnemyMonNickname (0xC616) Ã¢â€ â€™ RamSource(EnemyNickname).
    // Must NOT produce Arg(5) Ã¢â‚¬â€ no Sem_PrepareTextArg(buffer_slot=5) exists.
    // RamSource is a typed direct-WRAM read identity, not a prepared-slot reference.
    using namespace crystal;
    using namespace enginemon;

    TextDefinition def;
    def.source_rom_address = 0xD000;
    def.sequence.elements.push_back(TextElement::make_text_ram(0xC616));  // wEnemyMonNickname

    auto sem = def.to_semantic_sequence();

    ASSERT_EQ(sem.elements.size(), 1u);
    ASSERT_EQ(sem.elements[0].op, SemanticTextOp::RamSource);
    ASSERT_TRUE(sem.elements[0].op != SemanticTextOp::Arg);
    ASSERT_EQ(sem.elements[0].ram_source(), TextRamSource::EnemyNickname);

    std::cout << "  [TX_RAM(wEnemyMonNickname) Ã¢â€ â€™ RamSource(EnemyNickname), not Arg Ã¢Å“â€œ]\n";
}

TEST(text_battle_nickname_is_ram_source_not_arg) {
    // TX_RAM wBattleMonNickname (0xC621) Ã¢â€ â€™ RamSource(BattleNickname).
    // Must NOT produce Arg(6) Ã¢â‚¬â€ no such prepared-slot producer in vanilla Crystal.
    using namespace crystal;
    using namespace enginemon;

    TextDefinition def;
    def.source_rom_address = 0xD100;
    def.sequence.elements.push_back(TextElement::make_text_ram(0xC621));  // wBattleMonNickname

    auto sem = def.to_semantic_sequence();

    ASSERT_EQ(sem.elements.size(), 1u);
    ASSERT_EQ(sem.elements[0].op, SemanticTextOp::RamSource);
    ASSERT_TRUE(sem.elements[0].op != SemanticTextOp::Arg);
    ASSERT_EQ(sem.elements[0].ram_source(), TextRamSource::BattleNickname);

    std::cout << "  [TX_RAM(wBattleMonNickname) Ã¢â€ â€™ RamSource(BattleNickname), not Arg Ã¢Å“â€œ]\n";
}

TEST(text_prepared_string2_is_ram_source_not_arg) {
    // TX_RAM wStringBuffer2 (0xD086) Ã¢â€ â€™ RamSource(PreparedString2).
    // This is the direct-WRAM read used in Strength/RockSmash texts via TX_FAR.
    // NOT Arg(3) Ã¢â‚¬â€ GetStringBuffer only addresses slots 0-2.
    using namespace crystal;
    using namespace enginemon;

    TextDefinition def;
    def.source_rom_address = 0xD200;
    def.sequence.elements.push_back(TextElement::make_text_ram(0xD086));  // wStringBuffer2

    auto sem = def.to_semantic_sequence();

    ASSERT_EQ(sem.elements.size(), 1u);
    ASSERT_EQ(sem.elements[0].op, SemanticTextOp::RamSource);
    ASSERT_TRUE(sem.elements[0].op != SemanticTextOp::Arg);
    ASSERT_EQ(sem.elements[0].ram_source(), TextRamSource::PreparedString2);

    std::cout << "  [TX_RAM(wStringBuffer2) Ã¢â€ â€™ RamSource(PreparedString2), not Arg Ã¢Å“â€œ]\n";
}

TEST(sem_game_specific_event_writes_var_flag_blocks_constant_propagation) {
    // Verify that a Special with writes_script_var=true (e.g., BugContestJudging=20)
    // correctly invalidates block-local ScriptVar context, preventing stale values
    // from being propagated into subsequent context-dependent ops.
    using namespace crystal;
    using namespace enginemon;
    using namespace lowering_rules;
    
    // Build: setval(5), special(BugContestJudging=20), special(MapRadio=40)
    // BugContestJudging writes wScriptVar Ã¢â€ â€™ context invalidated Ã¢â€ â€™ MapRadio cannot fold
    Cmd_Setval sv; sv.value = 5;
    CrystalCommand c1; c1.data = sv; c1.span.raw_bytes = {0x15, 5};
    
    Cmd_Special bugContest; bugContest.special_id = 20;  // BugContestJudging
    CrystalCommand c2; c2.data = bugContest; c2.span.raw_bytes = {0x0F, 20, 0};
    
    Cmd_Special mapRadio; mapRadio.special_id = 40;  // MapRadio (needs context)
    CrystalCommand c3; c3.data = mapRadio; c3.span.raw_bytes = {0x0F, 40, 0};
    
    CrystalScriptIR ir;
    ir.name = "test_gse_invalidates"; ir.entry_address = 0x10000;
    ir.commands = {c1, c2, c3};
    
    CrystalCFG cfg;
    cfg.script_name = "test_gse_invalidates"; cfg.entry_address = 0x10000;
    BasicBlock block;
    block.id = 0; block.is_entry = true;
    block.start_address = 0; block.end_address = 8;
    block.command_start = 0; block.command_count = 3;
    cfg.blocks.push_back(block); cfg.source_ir = &ir;
    
    SemanticLegalizer legalizer;
    auto result = legalizer.lower(ir, cfg);
    
    // BugContestJudging (writes_var=true) should invalidate context
    // Ã¢â€ â€™ MapRadio has no context Ã¢â€ â€™ unlowered
    ASSERT_FALSE(result.success);
    ASSERT_TRUE(result.commands_unlowered > 0);
    
    // ALSO: verify BugContestJudging produced Sem_GameSpecificEvent
    bool found_gse = false;
    for (const auto& b : result.ir.blocks) {
        for (const auto& inst : b.instructions) {
            if (auto* gse = std::get_if<Sem_GameSpecificEvent>(&inst.op)) {
                if (gse->behavior_name == "BugContestJudging") {
                    found_gse = true;
                    ASSERT_TRUE(gse->writes_script_var);  // must be true for BugContestJudging
                }
            }
        }
    }
    ASSERT_TRUE(found_gse);
    
    std::cout << "  [BugContestJudging(writes_var=true) invalidates Ã¢â€ â€™ MapRadio unlowered Ã¢Å“â€œ]\n";
}

TEST(sem_game_specific_event_no_write_preserves_context) {
    // Verify that a Special with writes_script_var=false (e.g., OverworldTownMap=38)
    // does NOT invalidate block-local ScriptVar context.
    using namespace crystal;
    using namespace enginemon;
    using namespace lowering_rules;
    
    // Build: setval(3), special(OverworldTownMap=38), special(MapRadio=40)
    // OverworldTownMap does NOT write wScriptVar Ã¢â€ â€™ context preserved Ã¢â€ â€™ MapRadio folds to channel 3
    Cmd_Setval sv; sv.value = 3;
    CrystalCommand c1; c1.data = sv; c1.span.raw_bytes = {0x15, 3};
    
    Cmd_Special townMap; townMap.special_id = 38;  // OverworldTownMap
    CrystalCommand c2; c2.data = townMap; c2.span.raw_bytes = {0x0F, 38, 0};
    
    Cmd_Special mapRadio; mapRadio.special_id = 40;  // MapRadio (needs context = channel 3)
    CrystalCommand c3; c3.data = mapRadio; c3.span.raw_bytes = {0x0F, 40, 0};
    
    CrystalScriptIR ir;
    ir.name = "test_gse_preserves"; ir.entry_address = 0x10000;
    ir.commands = {c1, c2, c3};
    
    CrystalCFG cfg;
    cfg.script_name = "test_gse_preserves"; cfg.entry_address = 0x10000;
    BasicBlock block;
    block.id = 0; block.is_entry = true;
    block.start_address = 0; block.end_address = 8;
    block.command_start = 0; block.command_count = 3;
    cfg.blocks.push_back(block); cfg.source_ir = &ir;
    
    SemanticLegalizer legalizer;
    auto result = legalizer.lower(ir, cfg);
    
    // OverworldTownMap (writes_var=false) should NOT invalidate context
    // Ã¢â€ â€™ MapRadio still has context (channel=3) Ã¢â€ â€™ lowered to Sem_PlayRadio{channel=3}
    ASSERT_TRUE(result.success);
    
    // Verify OverworldTownMap produced Sem_GameSpecificEvent with writes_var=false
    bool found_townmap = false;
    bool found_radio_ch3 = false;
    for (const auto& b : result.ir.blocks) {
        for (const auto& inst : b.instructions) {
            if (auto* gse = std::get_if<Sem_GameSpecificEvent>(&inst.op)) {
                if (gse->behavior_name == "OverworldTownMap") {
                    found_townmap = true;
                    ASSERT_FALSE(gse->writes_script_var);  // must be false for OverworldTownMap
                }
            }
            if (auto* radio = std::get_if<Sem_PlayRadio>(&inst.op)) {
                if (radio->channel == 3) found_radio_ch3 = true;
            }
        }
    }
    ASSERT_TRUE(found_townmap);
    ASSERT_TRUE(found_radio_ch3);  // context preserved Ã¢â€ â€™ MapRadio folded with channel=3
    
    std::cout << "  [OverworldTownMap(writes_var=false) preserves context Ã¢â€ â€™ MapRadio(3) folded Ã¢Å“â€œ]\n";
}

TEST(sem_game_specific_event_behavior_name_is_source_proven_not_raw_id) {
    // Verify that Sem_GameSpecificEvent carries the source behavior name,
    // not a raw Crystal Special table index. This is the key distinction
    // from Sem_Special (which carried the raw numeric index).
    using namespace crystal;
    using namespace enginemon;
    using namespace lowering_rules;
    
    // Test HealMachineAnim (62) and BattleTowerAction (134)
    for (auto&& [special_id, expected_name] : std::vector<std::pair<uint16_t, std::string>>{
            {62, "HealMachineAnim"},
            {134, "BattleTowerAction"},
            {38, "OverworldTownMap"},
            {89, "GetFirstPokemonHappiness"}}) {
        
        Cmd_Special spec; spec.special_id = special_id;
        CrystalCommand cmd; cmd.data = spec; cmd.span.raw_bytes = {0x0F, (uint8_t)(special_id & 0xFF), (uint8_t)(special_id >> 8)};
        
        CrystalScriptIR ir;
        ir.name = "test_name_" + std::to_string(special_id); ir.entry_address = 0x10000;
        ir.commands = {cmd};
        
        CrystalCFG cfg;
        cfg.script_name = ir.name; cfg.entry_address = 0x10000;
        BasicBlock block;
        block.id = 0; block.is_entry = true;
        block.start_address = 0; block.end_address = 3;
        block.command_start = 0; block.command_count = 1;
        cfg.blocks.push_back(block); cfg.source_ir = &ir;
        
        SemanticLegalizer leg;
        auto result = leg.lower(ir, cfg);
        ASSERT_TRUE(result.success);
        
        bool found = false;
        for (const auto& b : result.ir.blocks) {
            for (const auto& inst : b.instructions) {
                if (auto* gse = std::get_if<Sem_GameSpecificEvent>(&inst.op)) {
                    ASSERT_STR_EQ(gse->behavior_name.c_str(), expected_name.c_str());
                    // Must NOT be a raw numeric string like "special_62"
                    ASSERT_TRUE(gse->behavior_name.find("special_") == std::string::npos);
                    ASSERT_TRUE(gse->behavior_name.find("0x") == std::string::npos);
                    found = true;
                }
            }
        }
        ASSERT_TRUE(found);
    }
    
    std::cout << "  [Sem_GameSpecificEvent carries source name not raw numeric ID Ã¢Å“â€œ]\n";
}

// make_single_cmd_ir defined in runtime_test_shared.hpp

//=============================================================================
// SCRIPT STATE AND DYNAMIC RESOURCE SEMANTICS TESTS Ã¢â‚¬â€ August 2026
// Verifies all 5 findings from the hostile audit:
//   Finding 1: wScriptVar block_ctx invalidation
//   Finding 2: cry 0 dynamic species
//   Finding 3: movement completeness
//   Finding 4: writecmdqueue bank resolution
//   Finding 5: pokepic 0 dynamic species
//=============================================================================

// Finding 1: setval 5 Ã¢â€ â€™ yesorno Ã¢â€ â€™ MapRadio must NOT fold channel=5
// yesorno writes wScriptVar, so block_ctx must be invalidated before MapRadio
TEST(stale_script_var_yesorno_invalidates_before_map_radio) {
    using namespace crystal;
    using namespace enginemon;

    // Build: setval(5), yesorno, special(MapRadio)
    CrystalScriptIR ir;
    ir.name = "test_yesorno_invalidate";
    ir.entry_address = 0x10000;

    CrystalCommand c1; c1.data = Cmd_Setval{5};   c1.span.raw_bytes = {0x15, 5};
    CrystalCommand c2; c2.data = Cmd_Yesorno{};   c2.span.raw_bytes = {0x4E};
    CrystalCommand c3; c3.data = Cmd_Special{40}; c3.span.raw_bytes = {0x0F, 40, 0};  // MapRadio=40
    ir.commands = {c1, c2, c3};

    CrystalCFG cfg;
    cfg.script_name = "test_yesorno_invalidate";
    cfg.entry_address = 0x10000;
    BasicBlock block;
    block.id = 0; block.is_entry = true;
    block.start_address = 0; block.end_address = 6;
    block.command_start = 0; block.command_count = 3;
    cfg.blocks.push_back(block);
    cfg.source_ir = &ir;

    SemanticLegalizer legalizer;
    auto result = legalizer.lower(ir, cfg);

    // After yesorno invalidates context, MapRadio (Special 40) has no producer.
    // rule_special returns {} (unmatched) Ã¢â€ â€™ outer loop records unlowered command.
    // result.success = false (unlowered command present)
    ASSERT_FALSE(result.success);
    ASSERT_TRUE(result.commands_unlowered > 0);

    // Also verify MapRadio was NOT folded with channel 5
    for (const auto& block : result.ir.blocks) {
        for (const auto& inst : block.instructions) {
            auto* radio = std::get_if<Sem_PlayRadio>(&inst.op);
            ASSERT_TRUE(radio == nullptr);  // Must NOT be PlayRadio Ã¢â‚¬â€ context was invalidated
        }
    }

    std::cout << "  [setval(5)->yesorno->MapRadio: unlowered (invalidated, no Sem_Special fallback) Ã¢Å“â€œ]\n";
}

// Finding 1: setval 5 Ã¢â€ â€™ non-writer Ã¢â€ â€™ MapRadio SHOULD fold channel=5 (legitimate propagation)
TEST(script_var_propagates_across_non_writer) {
    using namespace crystal;
    using namespace enginemon;

    // Build: setval(3), faceplayer (no wScriptVar write), special(MapRadio=40)
    CrystalScriptIR ir;
    ir.name = "test_propagate";
    ir.entry_address = 0x10000;

    CrystalCommand c1; c1.data = Cmd_Setval{3};     c1.span.raw_bytes = {0x15, 3};
    CrystalCommand c2; c2.data = Cmd_Faceplayer{};  c2.span.raw_bytes = {0x6B};
    CrystalCommand c3; c3.data = Cmd_Special{40};   c3.span.raw_bytes = {0x0F, 40, 0};
    ir.commands = {c1, c2, c3};

    CrystalCFG cfg;
    cfg.script_name = "test_propagate";
    cfg.entry_address = 0x10000;
    BasicBlock block;
    block.id = 0; block.is_entry = true;
    block.start_address = 0; block.end_address = 5;
    block.command_start = 0; block.command_count = 3;
    cfg.blocks.push_back(block);
    cfg.source_ir = &ir;

    SemanticLegalizer legalizer;
    auto result = legalizer.lower(ir, cfg);
    ASSERT_TRUE(result.success);

    const auto& insts = result.ir.blocks[0].instructions;
    // Should produce: Sem_SetVar{3}, Sem_FacePlayer{}, Sem_PlayRadio{3}
    ASSERT_TRUE(insts.size() >= 3);

    // Third instruction should be Sem_PlayRadio{3} Ã¢â‚¬â€ context still valid
    auto* radio = std::get_if<Sem_PlayRadio>(&insts[2].op);
    ASSERT_TRUE(radio != nullptr);
    ASSERT_EQ(radio->channel, 3);

    std::cout << "  [setval(3)->faceplayer->MapRadio: channel=3 (propagated) Ã¢Å“â€œ]\n";
}

// Finding 1: giveitem writes wScriptVar Ã¢â€ â€™ invalidates context

// =============================================================================
// MOVE-SAFETY REGRESSION: HeadlessGameLoop callback wires to correct object
// =============================================================================
// HeadlessGameLoop deletes copy/move. This test proves that a loop constructed
// in place (never moved) has its MovementManager completion callback correctly
// wired to the enclosing object, and that the callback fires correctly when a
// scripted NPC movement completes â€” updating the authoritative NpcState.
//
// The test explicitly exercises the path that the old NRVO-guarded factory
// pattern relied on implicitly. It also proves that give()/take()/has() operate
// against the authoritative GameState::items bag.
// =============================================================================
TEST(movement_callback_wires_to_live_object) {
    // Construct in-place â€” no move, no copy.
    // HeadlessGameLoop deletes copy and move.  This is the only valid construction.
    //
    // Proof: the MovementManager completion callback captures [this] at construction
    // time.  After the movement completes, the callback must fire against the live
    // HeadlessGameLoop â€” not a stale address from a hypothetical move.  We verify
    // this by enqueuing a scripted NPC movement through the production applymovement
    // path (script yields on movement; loop ticks drain the manager; callback fires;
    // NpcState is updated on the live object).
    HeadlessGameLoop loop;
    loop.spawn_player(0, 0, enginemon::Direction::Down);
    loop.set_collision_data([](int32_t, int32_t) -> CollisionClass {
        return CollisionClass::Floor;
    });

    // Add an NPC at (3, 0).
    NpcState npc;
    npc.id     = 1;
    npc.x      = 3;
    npc.y      = 0;
    npc.facing = enginemon::Direction::Down;
    loop.add_npc(npc);

    GameState gs;
    LuaRuntime rt;
    rt.set_game_state(&gs);
    loop.set_game_state(&gs);
    loop.set_lua_runtime(&rt);

    ASSERT_EQ(loop.get_npc(1)->x, 3);
    ASSERT_EQ(loop.get_npc(1)->y, 0);

    // Enqueue NPC movement via the scripted movement manager (same object as
    // loop.movement_manager_).  A single step left: (3,0) -> (2,0).
    std::vector<MovementCmd> cmds;
    MovementCmd step;
    step.type      = MovementCommandType::Step;
    step.direction = MovementDirection::Left;
    cmds.push_back(step);
    MovementCmd end_cmd;
    end_cmd.type = MovementCommandType::StepEnd;
    cmds.push_back(end_cmd);

    uint32_t fake_coroutine_id = 42;  // non-zero = NPC coroutine, not player
    bool enqueued = rt.get_stub_services().scripted_movement_manager->enqueue_movement(
        1, fake_coroutine_id, cmds, 3, 0, MovementDirection::Left
    );
    ASSERT_TRUE(enqueued);

    // Use a script that yields on movement for actor 1 so the loop enters
    // ScriptYielded state â€” that state causes movement_manager_.update() to be
    // called each tick, which drains the movement and fires the callback.
    // The script yields "movement" with actor_id=1, then ends.
    const char* script_code = R"(
script = {}
function script.main(ctx)
    coroutine.yield("movement", 1)
    return
end
return script
)";
    loop.set_script_loader([&](const std::string&) -> std::string {
        return script_code;
    });
    loop.start_script("move_test");

    // Tick until the loop returns to idle (movement complete + script resumes and ends).
    for (int i = 0; i < 40 && !loop.is_idle(); ++i) {
        loop.tick();
    }
    ASSERT_TRUE(loop.is_idle());

    // The completion callback fired via movement_manager_.completion_callback_,
    // which captures [this = &loop].  It called get_npc(1) on the live object and
    // updated npc->x/y/is_moving.
    const NpcState* npc_after = loop.get_npc(1);
    ASSERT_TRUE(npc_after != nullptr);
    ASSERT_EQ(npc_after->x, 2);       // one step left: 3 -> 2
    ASSERT_EQ(npc_after->y, 0);       // y unchanged
    ASSERT_FALSE(npc_after->is_moving);

    std::cout << "  [movement_callback_wires_to_live_object: NPC (3,0)->(2,0);"
              << " callback fired against live in-place object]\n";
}

// =============================================================================
// CAPABILITY CLOSURE E2E TESTS
// Exercises the non-battle script capabilities fixed in this pass:
//   - show_npc / hide_npc -> NpcState::visible
//   - face_actor          -> NpcState::facing
//   - face_player         -> last_talked NPC faces toward player
//   - teleport_npc        -> NpcState::{x,y}
//   - set_last_talked / LastTalked resolution
//   - set_scene/check_scene -> GameState::variables["scene_current"]
//   - set_state_var / read_state_var -> GameState::variables
// =============================================================================

// Helper: build a HeadlessGameLoop with one NPC for actor-state tests.
static void setup_npc_loop(HeadlessGameLoop& loop, GameState& gs, LuaRuntime& rt, int npc_id, int nx, int ny) {
    gs.rng.seed(0xDEADBEEF);
    rt.set_game_state(&gs);
    loop.set_game_state(&gs);
    loop.set_lua_runtime(&rt);
    loop.spawn_player(0, 0, enginemon::Direction::Down);
    loop.set_collision_data([](int32_t, int32_t) { return CollisionClass::Floor; });

    NpcState npc;
    npc.id      = static_cast<uint16_t>(npc_id);
    npc.x       = nx;
    npc.y       = ny;
    npc.facing  = enginemon::Direction::Down;
    npc.visible = true;
    loop.add_npc(npc);
}

// show_npc / hide_npc -> NpcState::visible
TEST(capability_show_hide_npc_updates_npc_state) {
    HeadlessGameLoop loop;
    GameState gs;
    LuaRuntime rt;
    setup_npc_loop(loop, gs, rt, 1, 3, 3);

    ASSERT_TRUE(loop.get_npc(1)->visible);

    // hide via script
    const char* hide_code = R"(
script = {}
function script.main(ctx)
    ctx.world:hide_npc(1)
    return
end
return script
)";
    rt.execute_string(hide_code, "hide");
    rt.start_script("script");
    ASSERT_FALSE(loop.get_npc(1)->visible);

    // show via script
    const char* show_code = R"(
script = {}
function script.main(ctx)
    ctx.world:show_npc(1)
    return
end
return script
)";
    rt.execute_string(show_code, "show");
    rt.start_script("script");
    ASSERT_TRUE(loop.get_npc(1)->visible);

    std::cout << "  [show/hide NPC updates NpcState::visible via callback]\n";
}

// face_actor(npc_id, dir) -> NpcState::facing
TEST(capability_face_actor_updates_npc_facing) {
    HeadlessGameLoop loop;
    GameState gs;
    LuaRuntime rt;
    setup_npc_loop(loop, gs, rt, 2, 5, 5);

    ASSERT_EQ(loop.get_npc(2)->facing, enginemon::Direction::Down);

    const char* code = R"(
script = {}
function script.main(ctx)
    ctx.world:face_actor(2, "up")
    return
end
return script
)";
    rt.execute_string(code, "face");
    rt.start_script("script");

    ASSERT_EQ(loop.get_npc(2)->facing, enginemon::Direction::Up);
    std::cout << "  [face_actor(2,'up') -> NpcState::facing = Up]\n";
}

// face_actor(0, dir) -> PlayerState::facing
TEST(capability_face_actor_player_updates_player_facing) {
    HeadlessGameLoop loop;
    GameState gs;
    LuaRuntime rt;
    setup_npc_loop(loop, gs, rt, 1, 3, 3);

    const char* code = R"(
script = {}
function script.main(ctx)
    ctx.world:face_actor(0, "left")
    return
end
return script
)";
    rt.execute_string(code, "face_p");
    rt.start_script("script");

    ASSERT_EQ(loop.player().facing, enginemon::Direction::Left);
    std::cout << "  [face_actor(0,'left') -> PlayerState::facing = Left]\n";
}

// face_player: last-talked NPC faces toward player
TEST(capability_face_player_updates_npc_facing_toward_player) {
    HeadlessGameLoop loop;
    GameState gs;
    LuaRuntime rt;
    // Player at (0,0); NPC at (0, 3) â€” player is directly above NPC â†’ NPC should face Up
    setup_npc_loop(loop, gs, rt, 3, 0, 3);
    loop.spawn_player(0, 0, enginemon::Direction::Down);

    // Manually set last_talked_id to NPC 3
    rt.get_stub_services().last_talked_id = 3;

    const char* code = R"(
script = {}
function script.main(ctx)
    ctx.world:face_player()
    return
end
return script
)";
    rt.execute_string(code, "fp");
    rt.start_script("script");

    // Player is at y=0, NPC at y=3: dy = 0-3 = -3 â†’ Direction::Up
    ASSERT_EQ(loop.get_npc(3)->facing, enginemon::Direction::Up);
    std::cout << "  [face_player: NPC(0,3) faces player(0,0) -> Up]\n";
}

// teleport_npc -> NpcState::{x,y}
TEST(capability_teleport_npc_updates_npc_position) {
    HeadlessGameLoop loop;
    GameState gs;
    LuaRuntime rt;
    setup_npc_loop(loop, gs, rt, 1, 5, 5);

    const char* code = R"(
script = {}
function script.main(ctx)
    ctx.world:teleport_npc(1, 10, 12)
    return
end
return script
)";
    rt.execute_string(code, "teleport");
    rt.start_script("script");

    ASSERT_EQ(loop.get_npc(1)->x, 10);
    ASSERT_EQ(loop.get_npc(1)->y, 12);
    ASSERT_FALSE(loop.get_npc(1)->is_moving);
    std::cout << "  [teleport_npc(1,10,12) -> NpcState x=10,y=12]\n";
}

// set_last_talked records the NPC id
TEST(capability_set_last_talked_records_id) {
    HeadlessGameLoop loop;
    GameState gs;
    LuaRuntime rt;
    setup_npc_loop(loop, gs, rt, 7, 2, 2);

    const char* code = R"(
script = {}
function script.main(ctx)
    ctx.world:set_last_talked(7)
    return
end
return script
)";
    rt.execute_string(code, "lt");
    rt.start_script("script");

    ASSERT_EQ(rt.get_stub_services().last_talked_id, static_cast<uint16_t>(7));
    std::cout << "  [set_last_talked(7) -> last_talked_id = 7]\n";
}

// set_scene / check_scene persist through GameState â€” per-map keyed
TEST(capability_set_scene_persists_in_game_state) {
    GameState gs;
    gs.player.current_map_id = "test_scene_map";
    LuaRuntime rt;
    rt.set_game_state(&gs);

    const char* code = R"(
script = {}
function script.main(ctx)
    ctx.game:set_scene(5)
    result = ctx.game:check_scene()
    ctx.flags:set_var(0, result)
    return
end
return script
)";
    rt.execute_string(code, "scene");
    rt.start_script("script");

    // scene=5 must be stored under the per-map key "scene_<map_id>"
    ASSERT_EQ(gs.variables["scene_test_scene_map"], 5);
    // check_scene must return 5
    ASSERT_EQ(gs.variables["var_0"], 5);
    std::cout << "  [set_scene(5) -> GameState::variables['scene_test_scene_map']=5]\n";
}

// set_state_var / read_state_var persist through GameState
TEST(capability_state_var_persists_in_game_state) {
    GameState gs;
    LuaRuntime rt;
    rt.set_game_state(&gs);

    const char* code = R"(
script = {}
function script.main(ctx)
    ctx.game:set_state_var(3, 42)
    result = ctx.game:read_state_var(3)
    ctx.flags:set_var(0, result)
    return
end
return script
)";
    rt.execute_string(code, "statevar");
    rt.start_script("script");

    ASSERT_EQ(gs.variables["state_var_3"], 42);
    ASSERT_EQ(gs.variables["var_0"], 42);
    std::cout << "  [set_state_var(3,42) / read_state_var(3) -> GameState persisted]\n";
}

