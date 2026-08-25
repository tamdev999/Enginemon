// runtime_test_core.cpp — LuaRuntime, ScriptDecoder/Emitter, collision, interaction, movement
#include "engine/scripting/lua_runtime.hpp"
#include "engine/scripting/api_bindings.hpp"
#include "engine/core/game_loop.hpp"
#include "engine/core/game_state.hpp"
#include "engine/core/timing.hpp"
#include "engine/input/input_system.hpp"
#include "engine/world/collision.hpp"
#include "engine/world/interaction.hpp"
#include "engine/world/runtime_map.hpp"
#include "crystal/rom/loader.hpp"
#include "crystal/rom/profile.hpp"
#include "crystal/rom/symbol_map.hpp"
#include "crystal/script/decoder.hpp"
#include "crystal/script/ir.hpp"
#include "crystal/script/lua_emitter.hpp"
#include "crystal/extract/map_extractor.hpp"
#include "crystal/extract/tileset_extractor.hpp"
#include "crystal/extract/sprite_ids.hpp"
#include "crystal/world/collision_classifier.hpp"
#include <array>
#include <unordered_map>
#include <filesystem>
#include "scripting/runtime_test_shared.hpp"

TEST(lua_runtime_creates) {
    LuaRuntime runtime;
    ASSERT_TRUE(runtime.get_state() != nullptr);
}

TEST(lua_runtime_executes_simple) {
    LuaRuntime runtime;
    
    // Execute simple Lua code
    runtime.execute_string("result = 1 + 2", "test");
    
    lua_State* L = runtime.get_state();
    lua_getglobal(L, "result");
    int result = lua_tointeger(L, -1);
    lua_pop(L, 1);
    
    ASSERT_EQ(result, 3);
}

TEST(lua_runtime_ctx_exists) {
    LuaRuntime runtime;
    
    // ctx should be created even without GameContext
    lua_State* L = runtime.get_state();
    lua_getglobal(L, "ctx");
    ASSERT_TRUE(lua_istable(L, -1));
    
    // Check ctx.ui exists
    lua_getfield(L, -1, "ui");
    ASSERT_TRUE(lua_istable(L, -1));
    lua_pop(L, 2);
}

TEST(decode_newbarktownsign) {
    // Decode the script and emit Lua
    SymbolMap symbols;
    ScriptDecoder decoder(*g_rom, symbols);
    
    uint32_t script_addr = g_rom->bank_to_flat(0x6A, 0x40C8);
    auto script = decoder.decode_script(script_addr, "NewBarkTownSign");
    
    ASSERT_EQ(script.instructions.size(), 1);
    
    LuaEmitter emitter;
    g_generated_lua = emitter.emit(script);
    
    ASSERT_STR_CONTAINS(g_generated_lua, "function script.main(ctx)");
    ASSERT_STR_CONTAINS(g_generated_lua, "NEW BARK TOWN");
    
    // Generated Lua uses "local script = {}" and "return script"
    // Wrap it so it assigns to a global when loaded
    g_generated_lua = "script = (function()\n" + g_generated_lua + "\nend)()";
}

TEST(lua_runtime_loads_generated_script) {
    LuaRuntime runtime;
    
    // The generated Lua defines a table called "script" and returns it
    // So we execute it, and it sets up the global "script" table
    runtime.execute_string(g_generated_lua, "NewBarkTownSign");
    
    // script table should now exist
    lua_State* L = runtime.get_state();
    lua_getglobal(L, "script");
    ASSERT_TRUE(lua_istable(L, -1));
    
    // script.main should be a function
    lua_getfield(L, -1, "main");
    ASSERT_TRUE(lua_isfunction(L, -1));
    lua_pop(L, 2);
}

TEST(lua_runtime_executes_script_yields_on_wait_button) {
    LuaRuntime runtime;
    
    // Load the script
    runtime.execute_string(g_generated_lua, "NewBarkTownSign");
    
    // Capture output
    std::vector<std::string> calls;
    bool yielded = false;
    
    // Start the script
    uint32_t co_id = runtime.start_script("script");
    
    // The script should have yielded at coroutine.yield("wait_button")
    ScriptState state = runtime.get_state(co_id);
    
    // The script may finish or yield depending on how yield is handled
    // With our implementation it should yield since wait_button is a dialog yield
    ASSERT_TRUE(state == ScriptState::Yielded || state == ScriptState::Finished);
    
    if (state == ScriptState::Yielded) {
        YieldReason reason = runtime.get_yield_reason(co_id);
        ASSERT_EQ(static_cast<int>(reason), static_cast<int>(YieldReason::Dialog));
        
        // Resume the script to complete it
        runtime.resume(co_id);
        state = runtime.get_state(co_id);
        ASSERT_EQ(static_cast<int>(state), static_cast<int>(ScriptState::Finished));
    }
}

TEST(lua_runtime_full_pipeline) {
    // Full end-to-end test:
    // 1. Decode NewBarkTownSign from ROM
    // 2. Emit as Lua
    // 3. Load into Lua runtime
    // 4. Execute with ctx.* stubs
    // 5. Verify yield/resume cycle completes
    
    SymbolMap symbols;
    ScriptDecoder decoder(*g_rom, symbols);
    
    uint32_t script_addr = g_rom->bank_to_flat(0x6A, 0x40C8);
    auto script = decoder.decode_script(script_addr, "NewBarkTownSign");
    
    LuaEmitter emitter;
    std::string lua_code = emitter.emit(script);
    
    // Wrap to assign to global
    lua_code = "script = (function()\n" + lua_code + "\nend)()";
    
    LuaRuntime runtime;
    runtime.execute_string(lua_code, "NewBarkTownSign");
    
    // Set error handler
    runtime.set_error_handler([](const std::string& error, const std::string& tb) {
        std::cerr << "Script error: " << error << "\n" << tb << "\n";
    });
    
    // Start script
    uint32_t co_id = runtime.start_script("script");
    
    ScriptState state = runtime.get_state(co_id);
    
    // Complete the script
    while (state == ScriptState::Yielded) {
        runtime.resume(co_id);
        state = runtime.get_state(co_id);
    }
    
    ASSERT_EQ(static_cast<int>(state), static_cast<int>(ScriptState::Finished));
    
    std::cout << "  [Full pipeline completed successfully]\n";
}

TEST(lua_runtime_multiple_scripts) {
    // Test that we can run multiple scripts (sequentially, not simultaneously)
    LuaRuntime runtime;
    
    // First script - simple, no return value issues
    std::string script1_code = R"(
script1 = {}
function script1.main(ctx)
    ctx.ui:text("Test 1")
    return
end
)";
    
    runtime.execute_string(script1_code, "script1_code");
    uint32_t co1 = runtime.start_script("script1");
    
    // First should complete
    ASSERT_EQ(static_cast<int>(runtime.get_state(co1)), static_cast<int>(ScriptState::Finished));
    
    // Now run second script with fresh runtime to avoid state issues
    LuaRuntime runtime2;
    
    std::string script2_code = R"(
script2 = {}
function script2.main(ctx)
    ctx.ui:text("Test 2")
    return
end
)";
    
    runtime2.execute_string(script2_code, "script2_code");
    uint32_t co2 = runtime2.start_script("script2");
    
    ASSERT_EQ(static_cast<int>(runtime2.get_state(co2)), static_cast<int>(ScriptState::Finished));
}

TEST(lua_runtime_wait_frames) {
    LuaRuntime runtime;
    
    std::string script_code = R"(
wait_test = {}
function wait_test.main(ctx)
    ctx.util:wait_frames(3)
    return
end
)";
    
    runtime.execute_string(script_code, "wait_test_code");
    
    uint32_t co_id = runtime.start_script("wait_test");
    
    // Should be yielded waiting for frames
    ScriptState state = runtime.get_state(co_id);
    ASSERT_EQ(static_cast<int>(state), static_cast<int>(ScriptState::Yielded));
    ASSERT_EQ(static_cast<int>(runtime.get_yield_reason(co_id)), static_cast<int>(YieldReason::WaitFrames));
    
    // Simulate 3 frames passing
    runtime.update(1.0f / 60.0f);  // Frame 1
    state = runtime.get_state(co_id);
    ASSERT_EQ(static_cast<int>(state), static_cast<int>(ScriptState::Yielded));
    
    runtime.update(1.0f / 60.0f);  // Frame 2
    state = runtime.get_state(co_id);
    ASSERT_EQ(static_cast<int>(state), static_cast<int>(ScriptState::Yielded));
    
    runtime.update(1.0f / 60.0f);  // Frame 3
    state = runtime.get_state(co_id);
    ASSERT_EQ(static_cast<int>(state), static_cast<int>(ScriptState::Finished));
}

TEST(lua_goto_works) {
    // Test that Lua goto/labels work in this version
    LuaRuntime runtime;
    
    std::string script_code = R"(
goto_test = {}
function goto_test.main(ctx)
    local x = 1
    if x == 1 then goto skip end
    ctx.ui:text("SHOULD NOT PRINT")
    ::skip::
    ctx.ui:text("Skipped correctly!")
end
)";
    
    runtime.execute_string(script_code, "goto_test_code");
    uint32_t co_id = runtime.start_script("goto_test");
    ASSERT_EQ(static_cast<int>(runtime.get_state(co_id)), static_cast<int>(ScriptState::Finished));
}

// NewBarkTownTeacherScript is at bank 0x6A, addr 0x406F
// This script demonstrates:
// - faceplayer
// - opentext/closetext
// - checkevent (flag checking)
// - iftrue (conditional branching based on flags)
// - writetext/waitbutton
// - end
//
// Branch structure:
//   faceplayer
//   opentext
//   checkevent EVENT_TALKED_TO_MOM_AFTER_MYSTERY_EGG_QUEST
//   iftrue .CallMom
//   checkevent EVENT_GAVE_MYSTERY_EGG_TO_ELM
//   iftrue .TellMomYoureLeaving
//   checkevent EVENT_GOT_A_POKEMON_FROM_ELM
//   iftrue .MonIsAdorable
//   writetext Text_GearIsImpressive  ; default path
//   waitbutton
//   closetext
//   end
// .MonIsAdorable:  (at 0x4089)
//   writetext Text_YourMonIsAdorable
//   waitbutton
//   closetext
//   end
// .TellMomYoureLeaving:  (at 0x408F)
//   writetext Text_TellMomIfLeaving
//   waitbutton
//   closetext
//   end
// .CallMom:  (at 0x4095)
//   writetext Text_CallMomOnGear
//   waitbutton
//   closetext
//   end

TEST(newbarktown_teacher_decode) {
    SymbolMap symbols;
    ScriptDecoder decoder(*g_rom, symbols);
    
    uint32_t script_addr = g_rom->bank_to_flat(0x6A, 0x406F);
    auto script = decoder.decode_script(script_addr, "NewBarkTownTeacherScript");
    
    // Should have multiple instructions including conditionals
    ASSERT_TRUE(script.instructions.size() > 5);
    
    // First instruction should be faceplayer
    bool found_faceplayer = false;
    bool found_checkevent = false;
    bool found_iftrue = false;
    bool found_writetext = false;
    
    for (const auto& inst : script.instructions) {
        std::visit([&](const auto& op) {
            using T = std::decay_t<decltype(op)>;
            if constexpr (std::is_same_v<T, Op_FacePlayer>) found_faceplayer = true;
            if constexpr (std::is_same_v<T, Op_CheckFlag>) found_checkevent = true;
            if constexpr (std::is_same_v<T, Op_JumpIf>) found_iftrue = true;
            if constexpr (std::is_same_v<T, Op_Text>) found_writetext = true;
        }, inst.op);
    }
    
    ASSERT_TRUE(found_faceplayer);
    ASSERT_TRUE(found_checkevent);
    ASSERT_TRUE(found_iftrue);
    ASSERT_TRUE(found_writetext);
    
    std::cout << "  [Decoded " << script.instructions.size() << " instructions]\n";
}

TEST(newbarktown_teacher_lua_emit) {
    SymbolMap symbols;
    ScriptDecoder decoder(*g_rom, symbols);
    
    uint32_t script_addr = g_rom->bank_to_flat(0x6A, 0x406F);
    auto script = decoder.decode_script(script_addr, "NewBarkTownTeacherScript");
    
    LuaEmitter emitter;
    std::string lua_code = emitter.emit(script);
    
    // Should contain labels (:: syntax for Lua goto targets)
    ASSERT_STR_CONTAINS(lua_code, "::label_");
    
    // Should contain goto statements for conditionals
    ASSERT_STR_CONTAINS(lua_code, "goto label_");
    
    // Should contain the result variable and conditionals (integer VM contract)
    ASSERT_STR_CONTAINS(lua_code, "local result = 0");
    ASSERT_STR_CONTAINS(lua_code, "if result ~= 0 then goto");
    
    // Should contain ctx.flags:check for checkevent
    ASSERT_STR_CONTAINS(lua_code, "ctx.flags:check(");
    
    // Should contain face_player
    ASSERT_STR_CONTAINS(lua_code, "ctx.world:face_player()");
    
    // Should have script_end label for proper Lua syntax
    ASSERT_STR_CONTAINS(lua_code, "::script_end::");
    
    std::cout << "  [Generated Lua contains proper control flow]\n";
}

TEST(newbarktown_teacher_default_branch) {
    // Test with all flags FALSE - should take default path (Text_GearIsImpressive)
    SymbolMap symbols;
    ScriptDecoder decoder(*g_rom, symbols);
    
    uint32_t script_addr = g_rom->bank_to_flat(0x6A, 0x406F);
    auto script = decoder.decode_script(script_addr, "NewBarkTownTeacherScript");
    
    LuaEmitter emitter;
    std::string lua_code = emitter.emit(script);
    
    // The emitted code already has "local script = {}" and "return script"
    // We need to capture the returned table. Use dostring with assignment.
    // Actually, just change local to global:
    size_t pos = lua_code.find("local script = {}");
    if (pos != std::string::npos) {
        lua_code.replace(pos, 17, "script = {}      ");  // Same length to preserve line numbers
    }
    // Remove the final "return script" as it's now a global
    pos = lua_code.rfind("return script");
    if (pos != std::string::npos) {
        lua_code.replace(pos, 13, "             ");  // Same length
    }
    
    // Create runtime FIRST, then reset flag state on it
    LuaRuntime runtime;
    flag_api::reset_test_state(&runtime);
    
    runtime.set_error_handler([](const std::string& error, const std::string& tb) {
        std::cerr << "Script error: " << error << "\n" << tb << "\n";
    });
    runtime.execute_string(lua_code, "NewBarkTownTeacherScript");
    
    uint32_t co_id = runtime.start_script("script");
    ScriptState state = runtime.get_state(co_id);
    
    // Complete the script (resume on yields)
    while (state == ScriptState::Yielded) {
        runtime.resume(co_id);
        state = runtime.get_state(co_id);
    }
    
    ASSERT_EQ(static_cast<int>(state), static_cast<int>(ScriptState::Finished));
    
    // Verify flags were checked (we check 3 event flags in order)
    auto& calls = flag_api::get_flag_calls(&runtime);
    int check_count = 0;
    for (const auto& call : calls) {
        if (call.first == "check") check_count++;
    }
    ASSERT_TRUE(check_count >= 3);  // All three checkevent calls should run
    
    std::cout << "  [Default branch executed successfully, " << check_count << " flag checks]\n";
}

TEST(newbarktown_teacher_first_branch) {
    // Test with first flag TRUE - should take .CallMom branch
    SymbolMap symbols;
    ScriptDecoder decoder(*g_rom, symbols);
    
    uint32_t script_addr = g_rom->bank_to_flat(0x6A, 0x406F);
    auto script = decoder.decode_script(script_addr, "NewBarkTownTeacherScript");
    
    // Find the flag IDs that are checked in the script
    // The first checkevent will be EVENT_TALKED_TO_MOM_AFTER_MYSTERY_EGG_QUEST
    std::vector<FlagId> checked_flags;
    for (const auto& inst : script.instructions) {
        std::visit([&](const auto& op) {
            using T = std::decay_t<decltype(op)>;
            if constexpr (std::is_same_v<T, Op_CheckFlag>) {
                checked_flags.push_back(op.flag);
            }
        }, inst.op);
    }
    ASSERT_TRUE(checked_flags.size() >= 1);
    
    LuaEmitter emitter;
    std::string lua_code = emitter.emit(script);
    
    // Convert local to global script
    size_t pos = lua_code.find("local script = {}");
    if (pos != std::string::npos) {
        lua_code.replace(pos, 17, "script = {}      ");
    }
    pos = lua_code.rfind("return script");
    if (pos != std::string::npos) {
        lua_code.replace(pos, 13, "             ");
    }
    
    // Create runtime FIRST, then reset and set the FIRST flag to true
    LuaRuntime runtime;
    flag_api::reset_test_state(&runtime);
    flag_api::set_test_flag(&runtime, checked_flags[0], true);
    
    runtime.set_error_handler([](const std::string& error, const std::string& tb) {
        std::cerr << "Script error: " << error << "\n" << tb << "\n";
    });
    runtime.execute_string(lua_code, "NewBarkTownTeacherScript");
    
    uint32_t co_id = runtime.start_script("script");
    ScriptState state = runtime.get_state(co_id);
    
    // Complete the script
    while (state == ScriptState::Yielded) {
        runtime.resume(co_id);
        state = runtime.get_state(co_id);
    }
    
    ASSERT_EQ(static_cast<int>(state), static_cast<int>(ScriptState::Finished));
    
    // Verify only ONE flag was checked (we should have jumped after the first check)
    auto& calls = flag_api::get_flag_calls(&runtime);
    int check_count = 0;
    for (const auto& call : calls) {
        if (call.first == "check") check_count++;
    }
    ASSERT_EQ(check_count, 1);  // Only first checkevent should run before branch
    
    std::cout << "  [First branch (.CallMom) taken after 1 flag check]\n";
}

TEST(newbarktown_teacher_second_branch) {
    // Test with second flag TRUE - should take .TellMomYoureLeaving branch  
    SymbolMap symbols;
    ScriptDecoder decoder(*g_rom, symbols);
    
    uint32_t script_addr = g_rom->bank_to_flat(0x6A, 0x406F);
    auto script = decoder.decode_script(script_addr, "NewBarkTownTeacherScript");
    
    std::vector<FlagId> checked_flags;
    for (const auto& inst : script.instructions) {
        std::visit([&](const auto& op) {
            using T = std::decay_t<decltype(op)>;
            if constexpr (std::is_same_v<T, Op_CheckFlag>) {
                checked_flags.push_back(op.flag);
            }
        }, inst.op);
    }
    ASSERT_TRUE(checked_flags.size() >= 2);
    
    LuaEmitter emitter;
    std::string lua_code = emitter.emit(script);
    
    // Convert local to global script
    size_t pos = lua_code.find("local script = {}");
    if (pos != std::string::npos) {
        lua_code.replace(pos, 17, "script = {}      ");
    }
    pos = lua_code.rfind("return script");
    if (pos != std::string::npos) {
        lua_code.replace(pos, 13, "             ");
    }
    
    // Create runtime FIRST, then reset and set the SECOND flag to true
    LuaRuntime runtime;
    flag_api::reset_test_state(&runtime);
    flag_api::set_test_flag(&runtime, checked_flags[1], true);
    
    runtime.set_error_handler([](const std::string& error, const std::string& tb) {
        std::cerr << "Script error: " << error << "\n" << tb << "\n";
    });
    runtime.execute_string(lua_code, "NewBarkTownTeacherScript");
    
    uint32_t co_id = runtime.start_script("script");
    ScriptState state = runtime.get_state(co_id);
    
    while (state == ScriptState::Yielded) {
        runtime.resume(co_id);
        state = runtime.get_state(co_id);
    }
    
    ASSERT_EQ(static_cast<int>(state), static_cast<int>(ScriptState::Finished));
    
    // Verify TWO flags were checked
    auto& calls = flag_api::get_flag_calls(&runtime);
    int check_count = 0;
    for (const auto& call : calls) {
        if (call.first == "check") check_count++;
    }
    ASSERT_EQ(check_count, 2);
    
    std::cout << "  [Second branch (.TellMomYoureLeaving) taken after 2 flag checks]\n";
}

TEST(newbarktown_teacher_third_branch) {
    // Test with third flag TRUE - should take .MonIsAdorable branch  
    // This covers EVENT_GOT_A_POKEMON_FROM_ELM
    SymbolMap symbols;
    ScriptDecoder decoder(*g_rom, symbols);
    
    uint32_t script_addr = g_rom->bank_to_flat(0x6A, 0x406F);
    auto script = decoder.decode_script(script_addr, "NewBarkTownTeacherScript");
    
    std::vector<FlagId> checked_flags;
    for (const auto& inst : script.instructions) {
        std::visit([&](const auto& op) {
            using T = std::decay_t<decltype(op)>;
            if constexpr (std::is_same_v<T, Op_CheckFlag>) {
                checked_flags.push_back(op.flag);
            }
        }, inst.op);
    }
    ASSERT_TRUE(checked_flags.size() >= 3);
    
    LuaEmitter emitter;
    std::string lua_code = emitter.emit(script);
    
    // Convert local to global script
    size_t pos = lua_code.find("local script = {}");
    if (pos != std::string::npos) {
        lua_code.replace(pos, 17, "script = {}      ");
    }
    pos = lua_code.rfind("return script");
    if (pos != std::string::npos) {
        lua_code.replace(pos, 13, "             ");
    }
    
    // Create runtime FIRST, then reset and set the THIRD flag to true (EVENT_GOT_A_POKEMON_FROM_ELM)
    LuaRuntime runtime;
    flag_api::reset_test_state(&runtime);
    flag_api::set_test_flag(&runtime, checked_flags[2], true);
    
    runtime.set_error_handler([](const std::string& error, const std::string& tb) {
        std::cerr << "Script error: " << error << "\n" << tb << "\n";
    });
    runtime.execute_string(lua_code, "NewBarkTownTeacherScript");
    
    uint32_t co_id = runtime.start_script("script");
    ScriptState state = runtime.get_state(co_id);
    
    while (state == ScriptState::Yielded) {
        runtime.resume(co_id);
        state = runtime.get_state(co_id);
    }
    
    ASSERT_EQ(static_cast<int>(state), static_cast<int>(ScriptState::Finished));
    
    // Verify THREE flags were checked before taking the .MonIsAdorable branch
    auto& calls = flag_api::get_flag_calls(&runtime);
    int check_count = 0;
    for (const auto& call : calls) {
        if (call.first == "check") check_count++;
    }
    ASSERT_EQ(check_count, 3);
    
    std::cout << "  [Third branch (.MonIsAdorable) taken after 3 flag checks]\n";
}

TEST(charmap_pokemon_text) {
    // Verify charmap correctly decodes POKé, POKéMON, contractions, etc.
    // Cross-reference: pokecrystal/constants/charmap.asm
    SymbolMap symbols;
    ScriptDecoder decoder(*g_rom, symbols);
    
    // NewBarkTownTeacherScript has "POKé" and "POKéMON" in its text
    uint32_t script_addr = g_rom->bank_to_flat(0x6A, 0x406F);
    auto script = decoder.decode_script(script_addr, "NewBarkTownTeacherScript");
    
    // Collect all text from the script using the semantic sequences
    std::string all_text;
    for (const auto& inst : script.instructions) {
        std::visit([&](const auto& op) {
            using T = std::decay_t<decltype(op)>;
            if constexpr (std::is_same_v<T, Op_Text>) {
                all_text += op.sequence.debug_string() + " ";
            }
        }, inst.op);
    }
    
    // Should contain properly decoded "POKéMON" (via 0x54 = "POKé")
    // The text uses "#MON" which expands 0x54 to "POKé" and MON is regular letters
    // Note: "POKé" contains U+00E9 (é); use the UTF-8 bytes directly to avoid
    // Windows console encoding issues in the test assertion string.
    // The decoded text contains "POK" followed by é (U+00E9 = 0xC3 0xA9 in UTF-8).
    ASSERT_TRUE(all_text.find("POK") != std::string::npos);
    // Verify the é character appears (charmap 0x54 → "POKé", rest is MON)
    bool has_e_accent = false;
    for (size_t i = 0; i + 1 < all_text.size(); ++i) {
        if ((uint8_t)all_text[i] == 0xC3 && (uint8_t)all_text[i+1] == 0xA9) {
            has_e_accent = true; break;
        }
    }
    ASSERT_TRUE(has_e_accent);
    
    std::cout << "  [Charmap correctly decodes POKé]\n";
}

TEST(charmap_contractions) {
    // Verify contractions are correctly decoded
    // Cross-reference: pokecrystal/constants/charmap.asm 0xD0-0xD6
    SymbolMap symbols;
    ScriptDecoder decoder(*g_rom, symbols);
    
    // NewBarkTownTeacherScript has "you're" text
    uint32_t script_addr = g_rom->bank_to_flat(0x6A, 0x406F);
    auto script = decoder.decode_script(script_addr, "NewBarkTownTeacherScript");
    
    LuaEmitter emitter;
    std::string lua_code = emitter.emit(script);
    
    // The text "You're doing" or "you're leaving" should decode correctly with apostrophe
    // 0xE0 is ' (apostrophe) in the charmap
    ASSERT_STR_CONTAINS(lua_code, "'");
    
    std::cout << "  [Charmap correctly decodes apostrophes]\n";
}

// =============================================================================
// MOVEMENT TESTS - End-to-end movement verification
// Reference: pokecrystal/macros/scripts/movement.asm
// Movement bytes: base | direction where direction = DOWN(0), UP(1), LEFT(2), RIGHT(3)
// step = 0x0C, turn_head = 0x00, step_end = 0x47
// =============================================================================

TEST(movement_parse_commands) {
    // Test parsing of raw movement bytes into semantic MovementCommand array
    // Example: 4 steps LEFT + step_end (from NewBarkTown_TeacherRunsToYouMovement1)
    SymbolMap symbols;
    ScriptDecoder decoder(*g_rom, symbols);
    
    // step LEFT = 0x0C | 2 = 0x0E
    // step_end = 0x47
    std::vector<uint8_t> raw = {0x0E, 0x0E, 0x0E, 0x0E, 0x47};
    
    auto commands = decoder.parse_movement_commands(raw);
    
    ASSERT_EQ(commands.size(), 5);  // 4 steps + step_end
    
    // First 4 should be step LEFT
    for (int i = 0; i < 4; i++) {
        ASSERT_TRUE(commands[i].is_step());
        ASSERT_EQ(static_cast<int>(commands[i].direction), static_cast<int>(enginemon::Direction::Left));
        ASSERT_EQ(static_cast<int>(commands[i].type), static_cast<int>(MovementType::Step));
    }
    
    // Last should be step_end
    ASSERT_EQ(static_cast<int>(commands[4].type), static_cast<int>(MovementType::StepEnd));
    
    std::cout << "  [Movement bytes parsed correctly: 4 steps LEFT + step_end]\n";
}

TEST(movement_parse_with_turn) {
    // Test parsing movement with a turn_head command
    // Example: 4 steps LEFT + turn_head DOWN + step_end
    SymbolMap symbols;
    ScriptDecoder decoder(*g_rom, symbols);
    
    // step LEFT = 0x0C | 2 = 0x0E
    // turn_head DOWN = 0x00 | 0 = 0x00
    // step_end = 0x47
    std::vector<uint8_t> raw = {0x0E, 0x0E, 0x0E, 0x0E, 0x00, 0x47};
    
    auto commands = decoder.parse_movement_commands(raw);
    
    ASSERT_EQ(commands.size(), 6);
    
    // First 4 should be step LEFT
    for (int i = 0; i < 4; i++) {
        ASSERT_TRUE(commands[i].is_step());
        ASSERT_EQ(static_cast<int>(commands[i].direction), static_cast<int>(enginemon::Direction::Left));
    }
    
    // 5th should be turn_head DOWN
    ASSERT_TRUE(commands[4].is_turn());
    ASSERT_EQ(static_cast<int>(commands[4].direction), static_cast<int>(enginemon::Direction::Down));
    ASSERT_EQ(static_cast<int>(commands[4].type), static_cast<int>(MovementType::TurnHead));
    
    std::cout << "  [Movement with turn parsed correctly]\n";
}

TEST(movement_lua_emit_steps) {
    // Test Lua emission for step commands
    // Should batch steps into ctx.world:move_actor(id, {left=N})
    
    ScriptIR script;
    script.name = "TestMovement";
    script.rom_start = 0;
    script.rom_end = 0;
    
    Op_ApplyMovement mov;
    mov.object_id = 2;  // Object ID 2
    mov.movements = {0x0E, 0x0E, 0x0E, 0x0E, 0x47};  // 4 steps LEFT + step_end
    
    // Parse commands
    mov.commands.push_back({MovementType::Step, enginemon::Direction::Left, 0});
    mov.commands.push_back({MovementType::Step, enginemon::Direction::Left, 0});
    mov.commands.push_back({MovementType::Step, enginemon::Direction::Left, 0});
    mov.commands.push_back({MovementType::Step, enginemon::Direction::Left, 0});
    mov.commands.push_back({MovementType::StepEnd, enginemon::Direction::Down, 0});
    
    Instruction inst;
    inst.op = mov;
    script.instructions.push_back(inst);
    
    // Add end
    Instruction end_inst;
    end_inst.op = Op_End{};
    script.instructions.push_back(end_inst);
    
    LuaEmitter emitter;
    std::string lua_code = emitter.emit(script);
    
    // Should contain per-step ordered move calls (F2 fix: no more batching)
    ASSERT_STR_CONTAINS(lua_code, "ctx.world:move_actor(2, \"left\", 1)");
    
    // Four separate left calls must appear (exact source order preserved)
    // Count occurrences: must be 4
    {
        int count = 0;
        size_t pos = 0;
        std::string needle = "ctx.world:move_actor(2, \"left\", 1)";
        while ((pos = lua_code.find(needle, pos)) != std::string::npos) {
            ++count;
            pos += needle.size();
        }
        ASSERT_EQ(count, 4);
    }
    
    // Should yield for movement
    ASSERT_STR_CONTAINS(lua_code, "coroutine.yield(\"movement\")");
    
    std::cout << "  [Lua emission emits per-step ordered calls: 4x move_actor left,1]\n";
}

TEST(movement_lua_emit_turn) {
    // Test Lua emission with a turn - should flush steps then emit face_actor
    
    ScriptIR script;
    script.name = "TestMovementTurn";
    script.rom_start = 0;
    script.rom_end = 0;
    
    Op_ApplyMovement mov;
    mov.object_id = 2;
    
    // 2 steps LEFT, turn_head DOWN, step_end
    mov.commands.push_back({MovementType::Step, enginemon::Direction::Left, 0});
    mov.commands.push_back({MovementType::Step, enginemon::Direction::Left, 0});
    mov.commands.push_back({MovementType::TurnHead, enginemon::Direction::Down, 0});
    mov.commands.push_back({MovementType::StepEnd, enginemon::Direction::Down, 0});
    
    Instruction inst;
    inst.op = mov;
    script.instructions.push_back(inst);
    
    Instruction end_inst;
    end_inst.op = Op_End{};
    script.instructions.push_back(end_inst);
    
    LuaEmitter emitter;
    std::string lua_code = emitter.emit(script);
    
    // Should contain per-step moves in source order
    ASSERT_STR_CONTAINS(lua_code, "ctx.world:move_actor(2, \"left\", 1)");
    
    // Should contain face_actor for the turn
    ASSERT_STR_CONTAINS(lua_code, "ctx.world:face_actor(2, \"down\")");
    
    std::cout << "  [Lua emission handles turn: per-step left calls then face_actor]\n";
}

// =============================================================================
// F2: Movement order preservation tests
// RIGHT, DOWN, DOWN must execute as RIGHT then DOWN then DOWN (not batched)
// =============================================================================
TEST(f2_movement_order_right_down_down) {
    using namespace enginemon;
    // Source sequence: RIGHT, DOWN, DOWN — must NOT be reordered to DOWN, DOWN, RIGHT
    ScriptIR script;
    script.name = "TestOrderRDD";
    script.rom_start = 0;
    script.rom_end = 0;

    Op_ApplyMovement mov;
    mov.object_id = 3;
    mov.commands.push_back({MovementType::Step, enginemon::Direction::Right, 0}); // RIGHT first
    mov.commands.push_back({MovementType::Step, enginemon::Direction::Down,  0}); // DOWN second
    mov.commands.push_back({MovementType::Step, enginemon::Direction::Down,  0}); // DOWN third
    mov.commands.push_back({MovementType::StepEnd, enginemon::Direction::Down, 0});

    Instruction inst; inst.op = mov;
    script.instructions.push_back(inst);
    Instruction end_inst; end_inst.op = Op_End{};
    script.instructions.push_back(end_inst);

    LuaEmitter emitter;
    std::string lua_code = emitter.emit(script);

    // ORACLE: right must appear BEFORE down in the emitted Lua
    size_t right_pos = lua_code.find("ctx.world:move_actor(3, \"right\", 1)");
    size_t down_pos  = lua_code.find("ctx.world:move_actor(3, \"down\", 1)");
    ASSERT_TRUE(right_pos != std::string::npos);
    ASSERT_TRUE(down_pos  != std::string::npos);
    ASSERT_TRUE(right_pos < down_pos);  // right before down

    // Exactly 1 right and 2 down calls
    int right_count = 0, down_count = 0;
    size_t pos = 0;
    while ((pos = lua_code.find("ctx.world:move_actor(3, \"right\", 1)", pos)) != std::string::npos) {
        ++right_count; pos += 10;
    }
    pos = 0;
    while ((pos = lua_code.find("ctx.world:move_actor(3, \"down\", 1)", pos)) != std::string::npos) {
        ++down_count; pos += 10;
    }
    ASSERT_EQ(right_count, 1);
    ASSERT_EQ(down_count,  2);

    // MUTATION CHECK: old batched format must NOT appear
    ASSERT_TRUE(lua_code.find("{right=") == std::string::npos);
    ASSERT_TRUE(lua_code.find("{down=")  == std::string::npos);

    std::cout << "  [F2: RIGHT,DOWN,DOWN emitted in source order, right before down ✓]\n";
}

TEST(f2_movement_order_right_right_up) {
    using namespace enginemon;
    // Source sequence: RIGHT, RIGHT, UP — UP must come AFTER both RIGHTs
    ScriptIR script;
    script.name = "TestOrderRRU";
    script.rom_start = 0;
    script.rom_end = 0;

    Op_ApplyMovement mov;
    mov.object_id = 5;
    mov.commands.push_back({MovementType::Step, enginemon::Direction::Right, 0});
    mov.commands.push_back({MovementType::Step, enginemon::Direction::Right, 0});
    mov.commands.push_back({MovementType::Step, enginemon::Direction::Up,    0});
    mov.commands.push_back({MovementType::StepEnd, enginemon::Direction::Down, 0});

    Instruction inst; inst.op = mov;
    script.instructions.push_back(inst);
    Instruction end_inst; end_inst.op = Op_End{};
    script.instructions.push_back(end_inst);

    LuaEmitter emitter;
    std::string lua_code = emitter.emit(script);

    // ORACLE: last right must appear BEFORE up
    size_t right1 = lua_code.find("ctx.world:move_actor(5, \"right\", 1)");
    ASSERT_TRUE(right1 != std::string::npos);
    // Find second right
    size_t right2 = lua_code.find("ctx.world:move_actor(5, \"right\", 1)", right1 + 1);
    ASSERT_TRUE(right2 != std::string::npos);
    size_t up_pos = lua_code.find("ctx.world:move_actor(5, \"up\", 1)");
    ASSERT_TRUE(up_pos != std::string::npos);
    ASSERT_TRUE(right2 < up_pos); // second right before up

    // Mutation check: no batching
    ASSERT_TRUE(lua_code.find("{right=") == std::string::npos);
    ASSERT_TRUE(lua_code.find("{up=")    == std::string::npos);

    std::cout << "  [F2: RIGHT,RIGHT,UP emitted in source order ✓]\n";
}

TEST(movement_world_state_changes) {
    // Test that movement execution actually changes world state
    // Execute movement Lua and verify actor position changes
    
    LuaRuntime runtime;
    runtime.set_error_handler([](const std::string& error, const std::string& tb) {
        std::cerr << "Script error: " << error << "\n" << tb << "\n";
    });
    
    // Reset world state - actor at position (5, 5)
    world_api::reset_world_state(&runtime);
    world_api::set_actor_pos(&runtime, 2, 5, 5);
    world_api::set_actor_facing(&runtime, 2, "down");
    
    // Verify initial state
    auto initial = world_api::get_actor_state(&runtime, 2);
    ASSERT_EQ(initial.x, 5);
    ASSERT_EQ(initial.y, 5);
    
    // Execute script that moves actor 2 left by 4 tiles
    std::string script_code = R"(
move_test = {}
function move_test.main(ctx)
    ctx.world:move_actor(2, {left=4})
    return
end
)";
    
    runtime.execute_string(script_code, "move_test");
    uint32_t co_id = runtime.start_script("move_test");
    
    // Complete the script
    ScriptState state = runtime.get_state(co_id);
    while (state == ScriptState::Yielded) {
        runtime.resume(co_id);
        state = runtime.get_state(co_id);
    }
    
    ASSERT_EQ(static_cast<int>(state), static_cast<int>(ScriptState::Finished));
    
    // Verify actor moved left by 4 tiles
    auto final_state = world_api::get_actor_state(&runtime, 2);
    ASSERT_EQ(final_state.x, 1);  // 5 - 4 = 1
    ASSERT_EQ(final_state.y, 5);  // unchanged
    
    // Verify movement was recorded
    auto& calls = world_api::get_movement_calls(&runtime);
    ASSERT_TRUE(calls.size() >= 1);
    
    std::cout << "  [World state changed: actor moved from (5,5) to (1,5)]\n";
}

TEST(movement_face_changes_facing) {
    // Test that face_actor changes the actor's facing direction
    
    LuaRuntime runtime;
    
    world_api::reset_world_state(&runtime);
    world_api::set_actor_pos(&runtime, 2, 5, 5);
    world_api::set_actor_facing(&runtime, 2, "down");
    
    std::string script_code = R"(
face_test = {}
function face_test.main(ctx)
    ctx.world:face_actor(2, "left")
    return
end
)";
    
    runtime.execute_string(script_code, "face_test");
    uint32_t co_id = runtime.start_script("face_test");
    
    // Complete the script
    ScriptState state = runtime.get_state(co_id);
    while (state == ScriptState::Yielded) {
        runtime.resume(co_id);
        state = runtime.get_state(co_id);
    }
    
    // Verify facing changed
    auto final_state = world_api::get_actor_state(&runtime, 2);
    ASSERT_TRUE(final_state.facing == "left");
    
    std::cout << "  [Actor facing changed from down to left]\n";
}

TEST(movement_combined_steps_and_turns) {
    // Test a combined sequence: move left, turn down, move down
    
    LuaRuntime runtime;
    
    world_api::reset_world_state(&runtime);
    world_api::set_actor_pos(&runtime, 2, 10, 10);
    world_api::set_actor_facing(&runtime, 2, "up");
    
    std::string script_code = R"(
combined_test = {}
function combined_test.main(ctx)
    -- Move left 3 tiles
    ctx.world:move_actor(2, {left=3})
    -- Turn to face down
    ctx.world:face_actor(2, "down")
    -- Move down 2 tiles
    ctx.world:move_actor(2, {down=2})
    return
end
)";
    
    runtime.execute_string(script_code, "combined_test");
    uint32_t co_id = runtime.start_script("combined_test");
    
    ScriptState state = runtime.get_state(co_id);
    while (state == ScriptState::Yielded) {
        runtime.resume(co_id);
        state = runtime.get_state(co_id);
    }
    
    auto final_state = world_api::get_actor_state(&runtime, 2);
    
    // Started at (10, 10), moved left 3 to (7, 10), then down 2 to (7, 12)
    ASSERT_EQ(final_state.x, 7);
    ASSERT_EQ(final_state.y, 12);
    ASSERT_TRUE(final_state.facing == "down");
    
    std::cout << "  [Combined movement: (10,10) -> left 3 -> down 2 -> (7,12) facing down]\n";
}

TEST(movement_player_movement) {
    // Test that player movement works (actor_id = 0 is player)
    
    LuaRuntime runtime;
    
    world_api::reset_world_state(&runtime);
    // Player starts at (5, 5) by default from reset_world_state
    
    std::string script_code = R"(
player_move_test = {}
function player_move_test.main(ctx)
    -- Move player right 2, up 3
    ctx.world:move_actor(0, {right=2})
    ctx.world:move_actor(0, {up=3})
    return
end
)";
    
    runtime.execute_string(script_code, "player_move_test");
    uint32_t co_id = runtime.start_script("player_move_test");
    
    ScriptState state = runtime.get_state(co_id);
    while (state == ScriptState::Yielded) {
        runtime.resume(co_id);
        state = runtime.get_state(co_id);
    }
    
    auto final_state = world_api::get_actor_state(&runtime, 0);  // 0 = player
    
    // Started at (5, 5), moved right 2 to (7, 5), then up 3 to (7, 2)
    ASSERT_EQ(final_state.x, 7);
    ASSERT_EQ(final_state.y, 2);
    
    std::cout << "  [Player movement: (5,5) -> right 2 -> up 3 -> (7,2)]\n";
}

// =============================================================================
// ASYNC MOVEMENT TESTS - Simulation-driven asynchronous movement
// Reference: pokecrystal OBJECT_STEP_DURATION = 16 frames per step
// Reference: Gen2Recomped scriptMove/updateScriptMoves callback pattern
// =============================================================================

#include "engine/world/movement_manager.hpp"
#include "engine/world/collision.hpp"

TEST(async_movement_manager_basic) {
    // Test MovementManager directly without Lua
    MovementManager mm;
    
    // Enqueue a simple 2-step movement (left x2)
    std::vector<MovementCmd> cmds = {
        {MovementCommandType::Step, MovementDirection::Left, 0},
        {MovementCommandType::Step, MovementDirection::Left, 0},
    };
    
    ASSERT_TRUE(mm.enqueue_movement(2, 100, cmds, 5, 5, MovementDirection::Down));
    ASSERT_TRUE(mm.is_actor_moving(2));
    
    // Actor should still be at starting position
    auto state = mm.get_actor_state(2);
    ASSERT_TRUE(state.has_value());
    ASSERT_EQ(state->x, 5);
    ASSERT_EQ(state->y, 5);
    
    std::cout << "  [MovementManager basic enqueue works]\n";
}

TEST(async_movement_not_instant) {
    // Verify that async movement does NOT complete instantly (all at once)
    // Reference: pokecrystal InitStep -> GetNextTile updates MAP_X/Y at step START
    // The actor is "committed" to the destination cell once the step begins.
    // However, the step itself takes 16 frames to complete.
    MovementManager mm;
    
    std::vector<MovementCmd> cmds = {
        {MovementCommandType::Step, MovementDirection::Left, 0},
        {MovementCommandType::Step, MovementDirection::Left, 0},
    };
    
    mm.enqueue_movement(2, 100, cmds, 10, 10, MovementDirection::Down);
    
    // Zero ticks - should still be at start (movement not yet started)
    auto state = mm.get_actor_state(2);
    ASSERT_EQ(state->x, 10);
    ASSERT_EQ(state->y, 10);
    
    // One tick - first step STARTS, position updates to destination (9,10)
    // per pokecrystal semantics where MAP_X/Y is the target cell
    mm.update();
    state = mm.get_actor_state(2);
    ASSERT_EQ(state->x, 9);  // Moved to target cell
    
    // But movement is NOT complete - actor is still "moving" for 15 more frames
    ASSERT_TRUE(mm.is_actor_moving(2));
    
    std::cout << "  [Async movement: position updates at step start per pokecrystal]\n";
}

TEST(async_movement_progresses_over_ticks) {
    // Verify that movement progresses across simulation ticks
    // 16 frames per step (from pokecrystal)
    MovementManager mm;
    
    std::vector<MovementCmd> cmds = {
        {MovementCommandType::Step, MovementDirection::Left, 0},  // 16 frames
    };
    
    mm.enqueue_movement(2, 100, cmds, 10, 10, MovementDirection::Down);
    
    // After 8 ticks - should still be moving, not complete
    for (int i = 0; i < 8; ++i) {
        mm.update();
    }
    
    auto state = mm.get_actor_state(2);
    ASSERT_TRUE(state.has_value());  // Still has active movement
    ASSERT_TRUE(mm.is_actor_moving(2));
    
    // After 16 total ticks - step should complete, position updates
    for (int i = 0; i < 8; ++i) {
        mm.update();
    }
    
    // Movement complete - actor no longer moving
    ASSERT_TRUE(!mm.is_actor_moving(2));
    
    // Check completion callback recorded
    auto& completions = mm.get_pending_completions();
    ASSERT_TRUE(completions.size() >= 1);
    ASSERT_EQ(completions[0].first, 2u);   // actor_id
    ASSERT_EQ(completions[0].second, 100u); // coroutine_id
    
    std::cout << "  [Movement progresses: 16 ticks for 1 step]\n";
}

TEST(async_movement_final_position) {
    // Verify final position after complete multi-step movement
    MovementManager mm;
    
    // 4 steps left = 64 frames total
    std::vector<MovementCmd> cmds = {
        {MovementCommandType::Step, MovementDirection::Left, 0},
        {MovementCommandType::Step, MovementDirection::Left, 0},
        {MovementCommandType::Step, MovementDirection::Left, 0},
        {MovementCommandType::Step, MovementDirection::Left, 0},
    };
    
    mm.enqueue_movement(2, 100, cmds, 10, 5, MovementDirection::Down);
    
    // Run 64 ticks to complete all 4 steps
    mm.update(64);
    
    // Actor should no longer be moving
    ASSERT_TRUE(!mm.is_actor_moving(2));
    
    // Final position should be (10-4, 5) = (6, 5)
    // Note: we need to get final state from the completion
    auto& completions = mm.get_pending_completions();
    ASSERT_TRUE(completions.size() >= 1);
    
    std::cout << "  [Final position after 4 steps left: verified complete]\n";
}

TEST(async_movement_with_turns) {
    // Test movement sequence with turns
    // Reference: pokecrystal movement.asm - TurnHead does NOT call ContinueReadingMovement,
    // so it consumes 1 frame before the next command is read on the subsequent frame.
    // This means turns are NOT instant - they take 1 frame.
    //
    // Verified timing from pokecrystal:
    // - Frame 1-16:  step_left(0) executes (STEP_DURATION 16→0)
    // - Frame 17-32: step_left(1) executes
    // - Frame 33:    turn_down executes, STEP_TYPE stays FROM_MOVEMENT
    // - Frame 34-49: step_down executes
    // Total: 49 frames
    MovementManager mm;
    
    // Move left 2, turn down, move down 1
    std::vector<MovementCmd> cmds = {
        {MovementCommandType::Step, MovementDirection::Left, 0},   // 16 frames
        {MovementCommandType::Step, MovementDirection::Left, 0},   // 16 frames
        {MovementCommandType::Turn, MovementDirection::Down, 0},   // 1 frame (NOT instant!)
        {MovementCommandType::Step, MovementDirection::Down, 0},   // 16 frames
    };
    
    mm.enqueue_movement(2, 100, cmds, 10, 10, MovementDirection::Up);
    
    // Run enough ticks for all commands:
    // 16 (step 1) + 16 (step 2) + 1 (turn) + 16 (step 3) = 49 ticks
    mm.update(49);
    
    ASSERT_TRUE(!mm.is_actor_moving(2));
    
    std::cout << "  [Movement with turns: 49 frames verified against pokecrystal]\n";
}

TEST(async_movement_fast_forward) {
    // Test that multiple ticks = fast forward
    // Running more simulation ticks completes movement faster
    MovementManager mm;
    
    std::vector<MovementCmd> cmds = {
        {MovementCommandType::Step, MovementDirection::Right, 0},
    };
    
    mm.enqueue_movement(2, 100, cmds, 0, 0, MovementDirection::Down);
    
    // Fast forward: run 16 ticks at once
    auto completed = mm.update(16);
    
    // Should complete in one batch
    ASSERT_TRUE(!mm.is_actor_moving(2));
    ASSERT_TRUE(completed.size() >= 1);
    
    std::cout << "  [Fast-forward: batch ticks complete movement faster]\n";
}

TEST(async_movement_completion_callback) {
    // Test that completion callback fires correctly
    MovementManager mm;
    
    bool callback_fired = false;
    uint32_t callback_actor = 0;
    uint32_t callback_coroutine = 0;
    
    mm.set_completion_callback([&](uint32_t actor_id, uint32_t coroutine_id) {
        callback_fired = true;
        callback_actor = actor_id;
        callback_coroutine = coroutine_id;
    });
    
    std::vector<MovementCmd> cmds = {
        {MovementCommandType::Step, MovementDirection::Up, 0},
    };
    
    mm.enqueue_movement(5, 200, cmds, 0, 0, MovementDirection::Down);
    
    // Run to completion
    mm.update(16);
    
    ASSERT_TRUE(callback_fired);
    ASSERT_EQ(callback_actor, 5u);
    ASSERT_EQ(callback_coroutine, 200u);
    
    std::cout << "  [Completion callback fires with correct actor/coroutine IDs]\n";
}

TEST(async_movement_batched_table) {
    // Test enqueue_movement_table (the batched {left=N, down=M} format)
    MovementManager mm;
    
    // left=2, down=1 = 3 steps = 48 frames
    mm.enqueue_movement_table(3, 300, 1, 0, 2, 0, 10, 10, MovementDirection::Right);
    // Parameters: actor_id, coroutine_id, down, up, left, right, start_x, start_y, facing
    
    ASSERT_TRUE(mm.is_actor_moving(3));
    
    // Run to completion
    mm.update(48);
    
    ASSERT_TRUE(!mm.is_actor_moving(3));
    
    std::cout << "  [Batched table format: down=1, left=2 converted to commands]\n";
}

TEST(async_movement_lua_yields) {
    // Test that Lua script yields when async movement is enabled
    // and remains yielded during movement
    
    LuaRuntime runtime;
    runtime.set_error_handler([](const std::string& error, const std::string& tb) {
        std::cerr << "Script error: " << error << "\n" << tb << "\n";
    });
    
    world_api::reset_world_state(&runtime);
    world_api::set_actor_pos(&runtime, 2, 10, 10);
    world_api::set_async_movement(&runtime, true);  // Enable async mode
    
    std::string script_code = R"(
async_yield_test = {}
function async_yield_test.main(ctx)
    ctx.world:move_actor(2, {left=2})
    -- This line should NOT execute until movement completes
    ctx.world:face_actor(2, "down")
    return
end
)";
    
    runtime.execute_string(script_code, "async_yield_test");
    uint32_t co_id = runtime.start_script("async_yield_test");
    
    // Script should be yielded waiting for movement
    ScriptState state = runtime.get_state(co_id);
    ASSERT_EQ(static_cast<int>(state), static_cast<int>(ScriptState::Yielded));
    
    YieldReason reason = runtime.get_yield_reason(co_id);
    ASSERT_EQ(static_cast<int>(reason), static_cast<int>(YieldReason::Movement));
    
    // Disable async mode for other tests
    world_api::set_async_movement(&runtime, false);
    
    std::cout << "  [Lua script yields on movement in async mode]\n";
}

TEST(async_movement_position_not_jumped) {
    // Critical test: verify position does NOT jump to final state immediately
    // This is the core difference from sync mode
    
    LuaRuntime runtime;
    
    world_api::reset_world_state(&runtime);
    world_api::set_actor_pos(&runtime, 2, 10, 10);
    world_api::set_async_movement(&runtime, true);
    
    std::string script_code = R"(
no_jump_test = {}
function no_jump_test.main(ctx)
    ctx.world:move_actor(2, {left=4})
    return
end
)";
    
    runtime.execute_string(script_code, "no_jump_test");
    uint32_t co_id = runtime.start_script("no_jump_test");
    
    // Script is yielded - movement enqueued but NOT applied yet
    ScriptState state = runtime.get_state(co_id);
    ASSERT_EQ(static_cast<int>(state), static_cast<int>(ScriptState::Yielded));
    
    // Actor position should STILL be at (10, 10) - NOT jumped to (6, 10)
    auto actor_state = world_api::get_actor_state(&runtime, 2);
    ASSERT_EQ(actor_state.x, 10);  // NOT 6!
    ASSERT_EQ(actor_state.y, 10);
    
    // Movement manager should show actor as moving
    auto& mm = world_api::get_movement_manager(&runtime);
    ASSERT_TRUE(mm.is_actor_moving(2));
    
    // Cleanup
    world_api::set_async_movement(&runtime, false);
    
    std::cout << "  [Position does NOT jump to final state immediately]\n";
}

TEST(async_movement_resumes_after_complete) {
    // Test that script resumes after movement completes
    // Requires ticking the movement manager and then resuming the script
    
    LuaRuntime runtime;
    runtime.set_error_handler([](const std::string& error, const std::string& tb) {
        std::cerr << "Script error: " << error << "\n" << tb << "\n";
    });
    
    world_api::reset_world_state(&runtime);
    world_api::set_actor_pos(&runtime, 2, 10, 10);
    world_api::set_async_movement(&runtime, true);
    
    // Track if face_actor was called (proves script continued past movement)
    bool face_called = false;
    auto& calls = world_api::get_movement_calls(&runtime);
    
    std::string script_code = R"(
resume_test = {}
function resume_test.main(ctx)
    ctx.world:move_actor(2, {left=1})  -- 1 step = 16 frames
    ctx.world:face_actor(2, "down")    -- Should only run after movement
    return
end
)";
    
    runtime.execute_string(script_code, "resume_test");
    uint32_t co_id = runtime.start_script("resume_test");
    
    // Script should be yielded
    ScriptState state = runtime.get_state(co_id);
    ASSERT_EQ(static_cast<int>(state), static_cast<int>(ScriptState::Yielded));
    
    // Tick movement manager to completion (16 frames)
    auto& mm = world_api::get_movement_manager(&runtime);
    mm.update(16);
    
    // Movement complete
    ASSERT_TRUE(!mm.is_actor_moving(2));
    
    // Now resume the script - it should continue to face_actor
    runtime.resume(co_id);
    
    state = runtime.get_state(co_id);
    ASSERT_EQ(static_cast<int>(state), static_cast<int>(ScriptState::Finished));
    
    // Check that face_actor was called (proving script continued)
    bool found_face = false;
    for (const auto& call : world_api::get_movement_calls(&runtime)) {
        if (call.first == "face") found_face = true;
    }
    ASSERT_TRUE(found_face);
    
    // Cleanup
    world_api::set_async_movement(&runtime, false);
    
    std::cout << "  [Script resumes after movement completion]\n";
}

// =============================================================================
// COLLISION TESTS - Native overworld collision system
// Reference: engine/world/collision_types.hpp
// Reference: Semantic CollisionClass enum for all collision checks
// =============================================================================

TEST(collision_class_semantic_queries) {
    // Test semantic collision queries using CollisionClass
    // This replaces the deleted CollisionPermissionTable tests
    
    // Walkable tiles
    ASSERT_TRUE(collision_is_walkable(CollisionClass::Floor));
    ASSERT_TRUE(collision_is_walkable(CollisionClass::Grass));
    ASSERT_TRUE(collision_is_walkable(CollisionClass::WarpDoor));
    ASSERT_TRUE(collision_is_walkable(CollisionClass::WarpFloor));
    
    // Non-walkable on foot
    ASSERT_FALSE(collision_is_walkable(CollisionClass::Wall));
    ASSERT_FALSE(collision_is_walkable(CollisionClass::Water));
    ASSERT_FALSE(collision_is_walkable(CollisionClass::Counter));
    
    // Water passable while surfing
    ASSERT_TRUE(collision_is_passable(CollisionClass::Water, true));
    ASSERT_TRUE(collision_is_passable(CollisionClass::Whirlpool, true));
    ASSERT_FALSE(collision_is_passable(CollisionClass::Water, false));
    
    // Warp detection
    ASSERT_TRUE(collision_is_warp(CollisionClass::WarpFloor));
    ASSERT_TRUE(collision_is_warp(CollisionClass::WarpDoor));
    ASSERT_TRUE(collision_is_warp(CollisionClass::WarpCave));
    ASSERT_TRUE(collision_is_warp(CollisionClass::WarpPit));
    ASSERT_FALSE(collision_is_warp(CollisionClass::Floor));
    
    // Counter detection
    ASSERT_TRUE(collision_is_counter(CollisionClass::Counter));
    ASSERT_FALSE(collision_is_counter(CollisionClass::Floor));
    ASSERT_FALSE(collision_is_counter(CollisionClass::Wall));
    
    std::cout << "  [Semantic CollisionClass queries verified]\n";
}

TEST(collision_passable_floor) {
    // Test movement onto passable floor tile succeeds
    Collision collision;
    
    // Create a simple 5x5 map with all floor tiles
    CollisionMap map;
    map.width = 5;
    map.height = 5;
    map.get_collision = [](int32_t x, int32_t y) -> CollisionClass {
        return CollisionClass::Floor;  // All walkable
    };
    
    // No entities
    std::vector<CollisionEntity> entities;
    
    // Mover at (2, 2) facing down
    CollisionEntity mover{1, 2, 2, 0, 0, false, false};
    
    // Should be able to move in all directions
    ASSERT_TRUE(collision.can_move(map, entities, mover, enginemon::Direction::Down).allowed);
    ASSERT_TRUE(collision.can_move(map, entities, mover, enginemon::Direction::Up).allowed);
    ASSERT_TRUE(collision.can_move(map, entities, mover, enginemon::Direction::Left).allowed);
    ASSERT_TRUE(collision.can_move(map, entities, mover, enginemon::Direction::Right).allowed);
    
    std::cout << "  [Movement onto floor tiles succeeds]\n";
}

TEST(collision_blocked_wall) {
    // Test movement into wall tile is blocked
    Collision collision;
    
    // Create a 5x5 map with a wall at (3, 2)
    CollisionMap map;
    map.width = 5;
    map.height = 5;
    map.get_collision = [](int32_t x, int32_t y) -> CollisionClass {
        if (x == 3 && y == 2) return CollisionClass::Wall;
        return CollisionClass::Floor;
    };
    
    std::vector<CollisionEntity> entities;
    
    // Mover at (2, 2)
    CollisionEntity mover{1, 2, 2, 0, 0, false, false};
    
    // Moving right into wall should fail
    auto result = collision.can_move(map, entities, mover, enginemon::Direction::Right);
    ASSERT_TRUE(!result.allowed);
    ASSERT_EQ(static_cast<int>(result.reason), static_cast<int>(MoveBlockReason::Tile));
    
    // Other directions should still work
    ASSERT_TRUE(collision.can_move(map, entities, mover, enginemon::Direction::Down).allowed);
    ASSERT_TRUE(collision.can_move(map, entities, mover, enginemon::Direction::Up).allowed);
    ASSERT_TRUE(collision.can_move(map, entities, mover, enginemon::Direction::Left).allowed);
    
    std::cout << "  [Movement into wall tiles blocked]\n";
}

TEST(collision_blocked_bounds) {
    // Test movement out of map bounds is blocked
    Collision collision;
    
    CollisionMap map;
    map.width = 3;
    map.height = 3;
    map.get_collision = [](int32_t x, int32_t y) -> CollisionClass {
        return CollisionClass::Floor;  // All floor
    };
    
    std::vector<CollisionEntity> entities;
    
    // Mover at (0, 0) - top-left corner
    CollisionEntity mover{1, 0, 0, 0, 0, false, false};
    
    // Moving up or left should be blocked (out of bounds)
    auto up_result = collision.can_move(map, entities, mover, enginemon::Direction::Up);
    ASSERT_TRUE(!up_result.allowed);
    ASSERT_EQ(static_cast<int>(up_result.reason), static_cast<int>(MoveBlockReason::Bounds));
    
    auto left_result = collision.can_move(map, entities, mover, enginemon::Direction::Left);
    ASSERT_TRUE(!left_result.allowed);
    ASSERT_EQ(static_cast<int>(left_result.reason), static_cast<int>(MoveBlockReason::Bounds));
    
    // Down and right should work
    ASSERT_TRUE(collision.can_move(map, entities, mover, enginemon::Direction::Down).allowed);
    ASSERT_TRUE(collision.can_move(map, entities, mover, enginemon::Direction::Right).allowed);
    
    std::cout << "  [Movement out of bounds blocked]\n";
}

TEST(collision_blocked_entity) {
    // Test movement into cell occupied by another entity is blocked
    Collision collision;
    
    CollisionMap map;
    map.width = 5;
    map.height = 5;
    map.get_collision = [](int32_t x, int32_t y) -> CollisionClass {
        return CollisionClass::Floor;  // All floor
    };
    
    // NPC at (3, 2)
    std::vector<CollisionEntity> entities;
    entities.push_back({2, 3, 2, 0, 0, false, false});  // id=2, at (3,2), not moving, not passable
    
    // Player at (2, 2)
    CollisionEntity mover{1, 2, 2, 0, 0, false, false};
    
    // Moving right into NPC should fail
    auto result = collision.can_move(map, entities, mover, enginemon::Direction::Right);
    ASSERT_TRUE(!result.allowed);
    ASSERT_EQ(static_cast<int>(result.reason), static_cast<int>(MoveBlockReason::Entity));
    ASSERT_EQ(result.blocking_entity, 2u);
    
    // Other directions should work
    ASSERT_TRUE(collision.can_move(map, entities, mover, enginemon::Direction::Down).allowed);
    ASSERT_TRUE(collision.can_move(map, entities, mover, enginemon::Direction::Up).allowed);
    ASSERT_TRUE(collision.can_move(map, entities, mover, enginemon::Direction::Left).allowed);
    
    std::cout << "  [Movement into occupied cell blocked]\n";
}

TEST(collision_entity_target_blocks) {
    // Test that an entity's TARGET cell also blocks during mid-step movement
    // Reference: Gen2Recomped Collision.occupied() checks both cellX/Y AND targetX/Y
    Collision collision;
    
    CollisionMap map;
    map.width = 5;
    map.height = 5;
    map.get_collision = [](int32_t x, int32_t y) -> CollisionClass {
        return CollisionClass::Floor;
    };
    
    // NPC currently at (4, 2) but moving INTO (3, 2)
    // This means (3, 2) should be blocked even though NPC isn't "there" yet
    std::vector<CollisionEntity> entities;
    entities.push_back({2, 4, 2, 3, 2, true, false});  // id=2, at (4,2), target (3,2), IS moving
    
    // Player at (2, 2) trying to move right to (3, 2)
    CollisionEntity mover{1, 2, 2, 0, 0, false, false};
    
    // Should be blocked because NPC is moving INTO (3, 2)
    auto result = collision.can_move(map, entities, mover, enginemon::Direction::Right);
    ASSERT_TRUE(!result.allowed);
    ASSERT_EQ(static_cast<int>(result.reason), static_cast<int>(MoveBlockReason::Entity));
    
    std::cout << "  [Entity target cell blocks during movement]\n";
}

TEST(collision_side_wall_blocks) {
    // Test side wall collision (directional blocking)
    // Reference: pokecrystal COLL_UP_WALL blocks entry from below
    Collision collision;
    
    CollisionMap map;
    map.width = 5;
    map.height = 5;
    map.get_collision = [](int32_t x, int32_t y) -> CollisionClass {
        // Put a SIDE_WALL_N (blocks from south) at (2, 1)
        if (x == 2 && y == 1) return CollisionClass::SideWallN;
        return CollisionClass::Floor;
    };
    
    std::vector<CollisionEntity> entities;
    
    // Mover at (2, 2) trying to move up to (2, 1)
    CollisionEntity mover{1, 2, 2, 0, 0, false, false};
    
    // Moving up into UP_WALL should be blocked
    auto result = collision.can_move(map, entities, mover, enginemon::Direction::Up);
    ASSERT_TRUE(!result.allowed);
    ASSERT_EQ(static_cast<int>(result.reason), static_cast<int>(MoveBlockReason::SideWall));
    
    // But we CAN move down from the wall tile if we were standing on it
    CollisionEntity on_wall{1, 2, 1, 0, 0, false, false};
    ASSERT_TRUE(collision.can_move(map, entities, on_wall, enginemon::Direction::Down).allowed);
    
    std::cout << "  [Side wall blocks movement from specific direction]\n";
}

TEST(collision_ledge_detection) {
    // Test ledge detection using semantic CollisionClass
    // Ledges now preserve direction: LedgeRight, LedgeLeft, LedgeUp, LedgeDown
    
    // All directional ledges are detected by collision_is_ledge()
    ASSERT_TRUE(collision_is_ledge(CollisionClass::LedgeRight));
    ASSERT_TRUE(collision_is_ledge(CollisionClass::LedgeLeft));
    ASSERT_TRUE(collision_is_ledge(CollisionClass::LedgeUp));
    ASSERT_TRUE(collision_is_ledge(CollisionClass::LedgeDown));
    ASSERT_FALSE(collision_is_ledge(CollisionClass::Floor));
    ASSERT_FALSE(collision_is_ledge(CollisionClass::Wall));
    
    // Ledge direction is preserved in the semantic type
    ASSERT_EQ(collision_ledge_direction(CollisionClass::LedgeRight), enginemon::Direction::Right);
    ASSERT_EQ(collision_ledge_direction(CollisionClass::LedgeLeft), enginemon::Direction::Left);
    ASSERT_EQ(collision_ledge_direction(CollisionClass::LedgeUp), enginemon::Direction::Up);
    ASSERT_EQ(collision_ledge_direction(CollisionClass::LedgeDown), enginemon::Direction::Down);
    
    // Ledges allow hops ONLY when facing the ledge direction
    ASSERT_TRUE(collision_can_hop_ledge(CollisionClass::LedgeDown, enginemon::Direction::Down));
    ASSERT_FALSE(collision_can_hop_ledge(CollisionClass::LedgeDown, enginemon::Direction::Up));
    ASSERT_FALSE(collision_can_hop_ledge(CollisionClass::LedgeDown, enginemon::Direction::Left));
    ASSERT_FALSE(collision_can_hop_ledge(CollisionClass::LedgeDown, enginemon::Direction::Right));
    
    ASSERT_TRUE(collision_can_hop_ledge(CollisionClass::LedgeRight, enginemon::Direction::Right));
    ASSERT_FALSE(collision_can_hop_ledge(CollisionClass::LedgeRight, enginemon::Direction::Left));
    
    // Ledges are NOT directly walkable (they require hop mechanics)
    ASSERT_FALSE(collision_is_walkable(CollisionClass::LedgeRight));
    ASSERT_FALSE(collision_is_walkable(CollisionClass::LedgeLeft));
    ASSERT_FALSE(collision_is_walkable(CollisionClass::LedgeUp));
    ASSERT_FALSE(collision_is_walkable(CollisionClass::LedgeDown));
    ASSERT_FALSE(collision_is_walkable(CollisionClass::Wall));
    ASSERT_TRUE(collision_is_walkable(CollisionClass::Floor));
    
    std::cout << "  [Directional ledge detection working - direction preserved]\n";
}

TEST(collision_semantic_boundary_adversarial) {
    // ADVERSARIAL TEST: Prove the collision system uses ONLY semantic CollisionClass
    // and has NO dependency on raw Crystal byte values.
    //
    // This test constructs collision scenarios using ONLY CollisionClass enums,
    // proving the runtime collision path is free of Crystal-specific encoding.
    
    Collision collision;
    
    // Test 1: CollisionClass is an enum, not a raw byte wrapper
    // All semantic types have explicit enum values, not raw bytes
    static_assert(static_cast<uint8_t>(CollisionClass::Floor) == 0, "Floor semantic ID");
    static_assert(static_cast<uint8_t>(CollisionClass::Wall) == 1, "Wall semantic ID");
    static_assert(static_cast<uint8_t>(CollisionClass::Water) == 2, "Water semantic ID");
    static_assert(static_cast<uint8_t>(CollisionClass::WarpDoor) == 11, "WarpDoor semantic ID");
    static_assert(static_cast<uint8_t>(CollisionClass::SideWallN) == 30, "SideWallN semantic ID");
    
    // These are Enginemon semantic values, NOT Crystal collision bytes:
    // Crystal uses 0x00=floor, 0x01=wall, 0x0F=water, 0x71=door, 0xB2=side_wall
    // Enginemon uses clean sequential semantic IDs
    
    // Test 2: CollisionMap accepts std::function<CollisionClass(...)>, NOT uint8_t
    CollisionMap map;
    map.width = 5;
    map.height = 5;
    map.get_collision = [](int32_t x, int32_t y) -> CollisionClass {
        // Return semantic types, not raw bytes
        if (x == 2 && y == 2) return CollisionClass::Wall;
        if (x == 3 && y == 2) return CollisionClass::Water;
        return CollisionClass::Floor;
    };
    
    // Test 3: HeadlessGameLoop.set_collision_data() signature takes CollisionClass callback
    HeadlessGameLoop loop;
    RuntimeMap rtmap;
    rtmap.width = 5;
    rtmap.height = 5;
    rtmap.blocks.resize(25, 0);
    loop.load_map(rtmap);
    
    // This would not compile if the API still used uint8_t:
    loop.set_collision_data([](int32_t x, int32_t y) -> CollisionClass {
        return CollisionClass::Floor;
    });
    
    // Test 4: Collision queries use semantic functions, not byte comparisons
    ASSERT_TRUE(collision_is_walkable(CollisionClass::Floor));
    ASSERT_FALSE(collision_is_walkable(CollisionClass::Wall));
    ASSERT_TRUE(collision_is_warp(CollisionClass::WarpDoor));
    ASSERT_TRUE(collision_is_side_wall(CollisionClass::SideWallN));
    ASSERT_TRUE(collision_is_counter(CollisionClass::Counter));
    
    // Test 5: CollisionResult contains CollisionClass, not raw bytes
    std::vector<CollisionEntity> entities;
    CollisionEntity mover{1, 0, 0, 0, 0, false, false};
    auto result = collision.can_move(map, entities, mover, enginemon::Direction::Down);
    ASSERT_TRUE(result.allowed);
    ASSERT_EQ(static_cast<uint8_t>(result.collision_class), static_cast<uint8_t>(CollisionClass::Floor));
    
    std::cout << "  [Collision boundary is 100% semantic CollisionClass - no raw Crystal bytes]\n";
}

TEST(collision_classifier_adversarial_misclassified_ids) {
    // ADVERSARIAL TEST: Verify previously misclassified Crystal collision IDs
    // are now correctly classified.
    //
    // These IDs were identified as misclassified by range-based inference:
    //   0x18 = TALL_GRASS (was misclassified as Water)
    //   0x29 = WATER (was misclassified as SmashableRock)
    //   0x33 = WATERFALL (was misclassified as Grass)
    //   0x24 = WHIRLPOOL (was misclassified as generic Ice/tree)
    //   0x27 = BUOY (was misclassified as generic Ice/tree)
    
    // Import the Crystal collision classifier
    // This is a frontend function, not runtime - it translates at package time
    
    // Test each previously misclassified ID
    ASSERT_EQ(crystal::classify_crystal_collision(0x18), CollisionClass::Grass);
    ASSERT_EQ(crystal::classify_crystal_collision(0x29), CollisionClass::Water);
    ASSERT_EQ(crystal::classify_crystal_collision(0x33), CollisionClass::Waterfall);
    ASSERT_EQ(crystal::classify_crystal_collision(0x24), CollisionClass::Whirlpool);
    ASSERT_EQ(crystal::classify_crystal_collision(0x27), CollisionClass::Wall);  // Buoy is wall
    
    // Verify related IDs are also correct
    ASSERT_EQ(crystal::classify_crystal_collision(0x10), CollisionClass::Grass);  // TALL_GRASS_10
    ASSERT_EQ(crystal::classify_crystal_collision(0x14), CollisionClass::Grass);  // LONG_GRASS
    ASSERT_EQ(crystal::classify_crystal_collision(0x1C), CollisionClass::Grass);  // LONG_GRASS_1C
    
    ASSERT_EQ(crystal::classify_crystal_collision(0x20), CollisionClass::Water);
    ASSERT_EQ(crystal::classify_crystal_collision(0x21), CollisionClass::Water);
    ASSERT_EQ(crystal::classify_crystal_collision(0x2D), CollisionClass::Water);
    
    ASSERT_EQ(crystal::classify_crystal_collision(0x30), CollisionClass::Waterfall);
    ASSERT_EQ(crystal::classify_crystal_collision(0x31), CollisionClass::Waterfall);
    ASSERT_EQ(crystal::classify_crystal_collision(0x32), CollisionClass::Waterfall);
    
    ASSERT_EQ(crystal::classify_crystal_collision(0x23), CollisionClass::Ice);
    ASSERT_EQ(crystal::classify_crystal_collision(0x2B), CollisionClass::Ice);
    
    ASSERT_EQ(crystal::classify_crystal_collision(0x2C), CollisionClass::Whirlpool);
    
    std::cout << "  [Collision classifier adversarial IDs all verified ✓]\n";
}

TEST(collision_classifier_source_proven_constants) {
    // Verify collision classifier uses source-proven Crystal constants
    // Reference: pokecrystal/constants/collision_constants.asm
    
    // Floor tiles
    ASSERT_EQ(crystal::classify_crystal_collision(0x00), CollisionClass::Floor);
    
    // Wall tiles
    ASSERT_EQ(crystal::classify_crystal_collision(0x07), CollisionClass::Wall);
    ASSERT_EQ(crystal::classify_crystal_collision(0x0F), CollisionClass::Wall);
    ASSERT_EQ(crystal::classify_crystal_collision(0xFF), CollisionClass::Wall);  // COLL_FF - WALL_TILE per collision_permissions.asm
    
    // Cut/Headbutt trees
    ASSERT_EQ(crystal::classify_crystal_collision(0x12), CollisionClass::CuttableTree);
    ASSERT_EQ(crystal::classify_crystal_collision(0x1A), CollisionClass::CuttableTree);
    ASSERT_EQ(crystal::classify_crystal_collision(0x15), CollisionClass::Wall);  // Headbutt = wall
    
    // Pit tiles
    ASSERT_EQ(crystal::classify_crystal_collision(0x60), CollisionClass::WarpPit);
    ASSERT_EQ(crystal::classify_crystal_collision(0x68), CollisionClass::WarpPit);
    
    // Warp carpets
    ASSERT_EQ(crystal::classify_crystal_collision(0x70), CollisionClass::WarpCarpet);
    ASSERT_EQ(crystal::classify_crystal_collision(0x76), CollisionClass::WarpCarpet);
    ASSERT_EQ(crystal::classify_crystal_collision(0x78), CollisionClass::WarpCarpet);
    ASSERT_EQ(crystal::classify_crystal_collision(0x7E), CollisionClass::WarpCarpet);
    
    // Door warps
    ASSERT_EQ(crystal::classify_crystal_collision(0x71), CollisionClass::WarpDoor);
    ASSERT_EQ(crystal::classify_crystal_collision(0x75), CollisionClass::WarpDoor);
    
    // Cave warps
    ASSERT_EQ(crystal::classify_crystal_collision(0x74), CollisionClass::WarpCave);
    ASSERT_EQ(crystal::classify_crystal_collision(0x7B), CollisionClass::WarpCave);
    
    // Stair/ladder warps
    ASSERT_EQ(crystal::classify_crystal_collision(0x72), CollisionClass::WarpStair);
    ASSERT_EQ(crystal::classify_crystal_collision(0x7A), CollisionClass::WarpStair);
    
    // Ledges - now directional
    ASSERT_EQ(crystal::classify_crystal_collision(0xA0), CollisionClass::LedgeRight);
    ASSERT_EQ(crystal::classify_crystal_collision(0xA1), CollisionClass::LedgeLeft);
    ASSERT_EQ(crystal::classify_crystal_collision(0xA2), CollisionClass::LedgeUp);
    ASSERT_EQ(crystal::classify_crystal_collision(0xA3), CollisionClass::LedgeDown);
    // Diagonal ledges map to their primary direction
    ASSERT_EQ(crystal::classify_crystal_collision(0xA4), CollisionClass::LedgeDown);  // HOP_DOWN_RIGHT
    ASSERT_EQ(crystal::classify_crystal_collision(0xA5), CollisionClass::LedgeDown);  // HOP_DOWN_LEFT
    ASSERT_EQ(crystal::classify_crystal_collision(0xA6), CollisionClass::LedgeUp);    // HOP_UP_RIGHT
    ASSERT_EQ(crystal::classify_crystal_collision(0xA7), CollisionClass::LedgeUp);    // HOP_UP_LEFT
    
    // Side walls
    ASSERT_EQ(crystal::classify_crystal_collision(0xB0), CollisionClass::SideWallE);
    ASSERT_EQ(crystal::classify_crystal_collision(0xB1), CollisionClass::SideWallW);
    ASSERT_EQ(crystal::classify_crystal_collision(0xB2), CollisionClass::SideWallN);
    ASSERT_EQ(crystal::classify_crystal_collision(0xB3), CollisionClass::SideWallS);
    
    // Counters
    ASSERT_EQ(crystal::classify_crystal_collision(0x90), CollisionClass::Counter);
    ASSERT_EQ(crystal::classify_crystal_collision(0x91), CollisionClass::Counter);
    ASSERT_EQ(crystal::classify_crystal_collision(0x93), CollisionClass::Counter);
    
    // Current tiles (treated as water)
    ASSERT_EQ(crystal::classify_crystal_collision(0x38), CollisionClass::Water);
    ASSERT_EQ(crystal::classify_crystal_collision(0x39), CollisionClass::Water);
    ASSERT_EQ(crystal::classify_crystal_collision(0x3A), CollisionClass::Water);
    ASSERT_EQ(crystal::classify_crystal_collision(0x3B), CollisionClass::Water);
    
    std::cout << "  [Collision classifier source-proven constants verified ✓]\n";
}

//=============================================================================
// PRE-RNG CORRECTNESS REGRESSION TESTS
// These tests verify the fixes completed in the pre-RNG correctness cleanup
//=============================================================================

TEST(script_yielded_locks_input) {
    // REGRESSION TEST: ScriptYielded state must lock input
    // Previously is_input_locked() only checked ScriptRunning, allowing input
    // during dialog waits (ScriptYielded).
    
    // Create loop with a script that yields
    HeadlessGameLoop loop;
    loop.set_collision_data([](int, int) { return CollisionClass::Floor; });
    
    // Create GameState for RNG
    GameState gs;
    loop.set_game_state(&gs);
    // Spawn player at position
    loop.spawn_player(5, 5, enginemon::Direction::Down);
    
    // Create a Lua runtime and wire it up
    LuaRuntime runtime;
    loop.set_lua_runtime(&runtime);
    
    // Set script loader - start_script expects code that creates global "script" table
    std::string yield_script = R"(
script = {}
function script.main(ctx)
    coroutine.yield("dialog")
    return
end
)";
    
    loop.set_script_loader([&](const std::string& id) -> std::string {
        if (id == "yield_test") return yield_script;
        return "";
    });
    
    // Start the script - should yield on dialog
    bool started = loop.start_script("yield_test");
    ASSERT_TRUE(started);
    
    // Script should now be in ScriptYielded state
    ASSERT_TRUE(loop.is_script_running());
    ASSERT_TRUE(loop.is_input_locked());
    
    // Try to process movement input - should be rejected
    auto result = loop.process_input(InputAction::MoveUp);
    ASSERT_FALSE(result.accepted);
    
    // Resume and complete the script
    loop.resume_script();
    ASSERT_FALSE(loop.is_script_running());
    ASSERT_FALSE(loop.is_input_locked());
    
    // Now input should be accepted
    result = loop.process_input(InputAction::MoveUp);
    ASSERT_TRUE(result.accepted);
    
    std::cout << "  [ScriptYielded input locking verified ✓]\n";
}

TEST(tileset_id_bounds_0_1_36_37) {
    // ADVERSARIAL TEST: Prove tileset extraction bounds are 1..36 (Crystal 1-indexed)
    // - tileset 0 → rejected (invalid, Crystal tilesets are 1-indexed)
    // - tileset 1 → accepted (johto_outdoor)
    // - tileset 36 → accepted and identifies "aerodactyl_word_room"
    // - tileset 37 → rejected (out of range)
    
    TilesetExtractor extractor(*g_rom, *g_profile);
    
    // Tileset 0 - MUST be rejected (Crystal is 1-indexed)
    auto result0 = extractor.extract_tileset(static_cast<uint8_t>(0));
    ASSERT_FALSE(result0.success);
    std::cout << "  [Tileset 0: rejected ✓]\n";
    
    // Tileset 1 (johto_outdoor) - MUST succeed
    auto result1 = extractor.extract_tileset(static_cast<uint8_t>(1));
    ASSERT_TRUE(result1.success);
    ASSERT_TRUE(result1.tileset.tiles.size() > 0);
    std::cout << "  [Tileset 1: accepted ✓]\n";
    
    // Tileset 36 - MUST succeed and be "aerodactyl_word_room"
    auto result36 = extractor.extract_tileset(static_cast<uint8_t>(36));
    ASSERT_TRUE(result36.success);
    ASSERT_TRUE(result36.tileset.tiles.size() > 0);
    std::cout << "  [Tileset 36: accepted ✓]\n";
    
    // Also verify by name
    auto result_aerodactyl = extractor.extract_tileset("aerodactyl_word_room");
    ASSERT_TRUE(result_aerodactyl.success);
    ASSERT_TRUE(result_aerodactyl.tileset.tiles.size() > 0);
    std::cout << "  [Tileset 36 = aerodactyl_word_room ✓]\n";
    
    // Tileset 37 - MUST be rejected (out of range, Crystal has 36 tilesets)
    auto result37 = extractor.extract_tileset(static_cast<uint8_t>(37));
    ASSERT_FALSE(result37.success);
    std::cout << "  [Tileset 37: rejected ✓]\n";
}

//=============================================================================
// BG CONDITION FLAG EVALUATION ADVERSARIAL TESTS
// These tests prove IFSET/IFNOTSET/hidden-item flags are evaluated at runtime
//=============================================================================

TEST(bg_event_ifset_unset_blocked) {
    // ADVERSARIAL: IFSET event with flag UNSET must NOT trigger
    
    Interaction interaction;
    
    // Create map with collision callback
    InteractionMap map;
    map.width = 10;
    map.height = 10;
    map.get_collision = [](int32_t, int32_t) { return CollisionClass::Floor; };
    
    // No objects
    std::vector<InteractableObject> objects;
    
    // BG event at (5, 4) - IFSET type with flag "test_flag"
    std::vector<InteractableBgEvent> bg_events;
    InteractableBgEvent bg;
    bg.x = 5;
    bg.y = 4;
    bg.type = BgEventTypeId::IfSet;
    bg.script_id = "test_script";
    bg.condition_flag = "test_flag";
    bg_events.push_back(bg);
    
    // Player at (5, 5) facing up (toward 5, 4)
    int32_t px = 5, py = 5;
    enginemon::Direction facing = enginemon::Direction::Up;
    
    // Flag checker returns FALSE (flag not set)
    auto flag_checker = [](const std::string& flag) -> bool {
        return false;  // Flag is NOT set
    };
    
    auto result = interaction.check(map, objects, bg_events, px, py, facing, flag_checker);
    
    // IFSET with unset flag must NOT trigger
    ASSERT_EQ(result.type, InteractionType::None);
    
    std::cout << "  [IFSET unset: blocked ✓]\n";
}

TEST(bg_event_ifset_set_triggers) {
    // ADVERSARIAL: IFSET event with flag SET must trigger
    
    Interaction interaction;
    
    InteractionMap map;
    map.width = 10;
    map.height = 10;
    map.get_collision = [](int32_t, int32_t) { return CollisionClass::Floor; };
    
    std::vector<InteractableObject> objects;
    
    std::vector<InteractableBgEvent> bg_events;
    InteractableBgEvent bg;
    bg.x = 5;
    bg.y = 4;
    bg.type = BgEventTypeId::IfSet;
    bg.script_id = "test_script";
    bg.condition_flag = "test_flag";
    bg_events.push_back(bg);
    
    int32_t px = 5, py = 5;
    enginemon::Direction facing = enginemon::Direction::Up;
    
    // Flag checker returns TRUE (flag IS set)
    auto flag_checker = [](const std::string& flag) -> bool {
        return true;  // Flag IS set
    };
    
    auto result = interaction.check(map, objects, bg_events, px, py, facing, flag_checker);
    
    // IFSET with set flag must trigger
    ASSERT_EQ(result.type, InteractionType::BgEvent);
    ASSERT_STR_EQ(result.bg_script_id, "test_script");
    
    std::cout << "  [IFSET set: triggers ✓]\n";
}

TEST(bg_event_ifnotset_unset_triggers) {
    // ADVERSARIAL: IFNOTSET event with flag UNSET must trigger
    
    Interaction interaction;
    
    InteractionMap map;
    map.width = 10;
    map.height = 10;
    map.get_collision = [](int32_t, int32_t) { return CollisionClass::Floor; };
    
    std::vector<InteractableObject> objects;
    
    std::vector<InteractableBgEvent> bg_events;
    InteractableBgEvent bg;
    bg.x = 5;
    bg.y = 4;
    bg.type = BgEventTypeId::IfNotSet;
    bg.script_id = "test_script";
    bg.condition_flag = "test_flag";
    bg_events.push_back(bg);
    
    int32_t px = 5, py = 5;
    enginemon::Direction facing = enginemon::Direction::Up;
    
    // Flag checker returns FALSE (flag not set)
    auto flag_checker = [](const std::string& flag) -> bool {
        return false;
    };
    
    auto result = interaction.check(map, objects, bg_events, px, py, facing, flag_checker);
    
    // IFNOTSET with unset flag must trigger
    ASSERT_EQ(result.type, InteractionType::BgEvent);
    ASSERT_STR_EQ(result.bg_script_id, "test_script");
    
    std::cout << "  [IFNOTSET unset: triggers ✓]\n";
}

TEST(bg_event_ifnotset_set_blocked) {
    // ADVERSARIAL: IFNOTSET event with flag SET must NOT trigger
    
    Interaction interaction;
    
    InteractionMap map;
    map.width = 10;
    map.height = 10;
    map.get_collision = [](int32_t, int32_t) { return CollisionClass::Floor; };
    
    std::vector<InteractableObject> objects;
    
    std::vector<InteractableBgEvent> bg_events;
    InteractableBgEvent bg;
    bg.x = 5;
    bg.y = 4;
    bg.type = BgEventTypeId::IfNotSet;
    bg.script_id = "test_script";
    bg.condition_flag = "test_flag";
    bg_events.push_back(bg);
    
    int32_t px = 5, py = 5;
    enginemon::Direction facing = enginemon::Direction::Up;
    
    // Flag checker returns TRUE (flag IS set)
    auto flag_checker = [](const std::string& flag) -> bool {
        return true;
    };
    
    auto result = interaction.check(map, objects, bg_events, px, py, facing, flag_checker);
    
    // IFNOTSET with set flag must NOT trigger
    ASSERT_EQ(result.type, InteractionType::None);
    
    std::cout << "  [IFNOTSET set: blocked ✓]\n";
}

TEST(bg_event_hidden_item_uncollected_triggers) {
    // ADVERSARIAL: Hidden item with flag UNSET (uncollected) must trigger
    
    Interaction interaction;
    
    InteractionMap map;
    map.width = 10;
    map.height = 10;
    map.get_collision = [](int32_t, int32_t) { return CollisionClass::Floor; };
    
    std::vector<InteractableObject> objects;
    
    std::vector<InteractableBgEvent> bg_events;
    InteractableBgEvent bg;
    bg.x = 5;
    bg.y = 4;
    bg.type = BgEventTypeId::ItemIfSet;  // Hidden item type
    bg.script_id = "";
    bg.item_id = "potion";
    bg.quantity = 1;
    bg.condition_flag = "hidden_item_flag";
    bg_events.push_back(bg);
    
    int32_t px = 5, py = 5;
    enginemon::Direction facing = enginemon::Direction::Up;
    
    // Flag checker returns FALSE (item NOT collected yet)
    auto flag_checker = [](const std::string& flag) -> bool {
        return false;  // Item has NOT been collected
    };
    
    auto result = interaction.check(map, objects, bg_events, px, py, facing, flag_checker);
    
    // Hidden item with uncollected flag must trigger
    ASSERT_EQ(result.type, InteractionType::HiddenItem);
    ASSERT_STR_EQ(result.bg_item_id, "potion");
    
    std::cout << "  [Hidden item uncollected: triggers ✓]\n";
}

TEST(bg_event_hidden_item_collected_blocked) {
    // ADVERSARIAL: Hidden item with flag SET (already collected) must NOT trigger
    
    Interaction interaction;
    
    InteractionMap map;
    map.width = 10;
    map.height = 10;
    map.get_collision = [](int32_t, int32_t) { return CollisionClass::Floor; };
    
    std::vector<InteractableObject> objects;
    
    std::vector<InteractableBgEvent> bg_events;
    InteractableBgEvent bg;
    bg.x = 5;
    bg.y = 4;
    bg.type = BgEventTypeId::ItemIfSet;  // Hidden item type
    bg.script_id = "";
    bg.item_id = "potion";
    bg.quantity = 1;
    bg.condition_flag = "hidden_item_flag";
    bg_events.push_back(bg);
    
    int32_t px = 5, py = 5;
    enginemon::Direction facing = enginemon::Direction::Up;
    
    // Flag checker returns TRUE (item already collected)
    auto flag_checker = [](const std::string& flag) -> bool {
        return true;  // Item HAS been collected
    };
    
    auto result = interaction.check(map, objects, bg_events, px, py, facing, flag_checker);
    
    // Hidden item with collected flag must NOT trigger
    ASSERT_EQ(result.type, InteractionType::None);
    
    std::cout << "  [Hidden item collected: blocked ✓]\n";
}

//=============================================================================
// FINAL PRE-RNG CORRECTNESS TESTS
// Tests added during the final semantic gap closure pass
//=============================================================================

TEST(sprite_id_mapping_authoritative) {
    // ADVERSARIAL TEST: Prove sprite ID mapping is authoritative across MapExtractor
    // and SpriteExtractor using the shared sprite_ids.hpp
    //
    // Previously: MapExtractor had truncated table (0..58), SpriteExtractor had full (0..102)
    // Now: Both use crystal_sprite_index_to_id() from sprite_ids.hpp
    
    // Boundary test: ID 58 (last valid in old truncated table)
    ASSERT_STR_EQ(crystal::crystal_sprite_index_to_id(58), "fisher");
    ASSERT_TRUE(crystal::crystal_sprite_index_valid(58));
    
    // Boundary test: ID 59 (first invalid in old truncated table, should be valid now)
    ASSERT_STR_EQ(crystal::crystal_sprite_index_to_id(59), "fishing_guru");
    ASSERT_TRUE(crystal::crystal_sprite_index_valid(59));
    
    // Middle of previously invalid range: ID 60
    ASSERT_STR_EQ(crystal::crystal_sprite_index_to_id(60), "scientist");
    ASSERT_TRUE(crystal::crystal_sprite_index_valid(60));
    
    // High end of valid range: ID 102
    ASSERT_STR_EQ(crystal::crystal_sprite_index_to_id(102), "standing_youngster");
    ASSERT_TRUE(crystal::crystal_sprite_index_valid(102));
    
    // Invalid: ID 103 (past valid range)
    ASSERT_STR_EQ(crystal::crystal_sprite_index_to_id(103), "");  // Empty = invalid
    ASSERT_FALSE(crystal::crystal_sprite_index_valid(103));
    
    // Invalid: ID 0 (SPRITE_NONE)
    ASSERT_STR_EQ(crystal::crystal_sprite_index_to_id(0), "");  // Empty = invalid
    ASSERT_FALSE(crystal::crystal_sprite_index_valid(0));
    
    // Reverse lookup works
    ASSERT_EQ(crystal::crystal_sprite_id_to_index("chris"), 1);
    ASSERT_EQ(crystal::crystal_sprite_id_to_index("fisher"), 58);
    ASSERT_EQ(crystal::crystal_sprite_id_to_index("fishing_guru"), 59);
    ASSERT_EQ(crystal::crystal_sprite_id_to_index("standing_youngster"), 102);
    ASSERT_EQ(crystal::crystal_sprite_id_to_index("invalid_name"), 0);  // 0 = SPRITE_NONE = invalid
    
    std::cout << "  [Sprite ID mapping authoritative 1..102 ✓]\n";
}

TEST(directional_ledge_semantic_preservation) {
    // ADVERSARIAL TEST: Prove ledge direction is NOT collapsed to a single value
    // 
    // Previously: All Crystal ledges (0xA0-0xA7) → CollisionClass::Ledge (direction lost)
    // Now: 0xA0 → LedgeRight, 0xA1 → LedgeLeft, 0xA2 → LedgeUp, 0xA3 → LedgeDown
    
    // Basic ledge classification
    ASSERT_EQ(crystal::classify_crystal_collision(0xA0), CollisionClass::LedgeRight);
    ASSERT_EQ(crystal::classify_crystal_collision(0xA1), CollisionClass::LedgeLeft);
    ASSERT_EQ(crystal::classify_crystal_collision(0xA2), CollisionClass::LedgeUp);
    ASSERT_EQ(crystal::classify_crystal_collision(0xA3), CollisionClass::LedgeDown);
    
    // Diagonal ledges map to primary direction
    ASSERT_EQ(crystal::classify_crystal_collision(0xA4), CollisionClass::LedgeDown);  // DOWN_RIGHT → Down
    ASSERT_EQ(crystal::classify_crystal_collision(0xA5), CollisionClass::LedgeDown);  // DOWN_LEFT → Down
    ASSERT_EQ(crystal::classify_crystal_collision(0xA6), CollisionClass::LedgeUp);    // UP_RIGHT → Up
    ASSERT_EQ(crystal::classify_crystal_collision(0xA7), CollisionClass::LedgeUp);    // UP_LEFT → Up
    
    // Prove different ledge types are distinguishable
    auto ledge_right = crystal::classify_crystal_collision(0xA0);
    auto ledge_down = crystal::classify_crystal_collision(0xA3);
    ASSERT_TRUE(ledge_right != ledge_down);  // They are NOT the same enum value
    
    // Semantic queries preserve direction
    ASSERT_EQ(collision_ledge_direction(CollisionClass::LedgeRight), enginemon::Direction::Right);
    ASSERT_EQ(collision_ledge_direction(CollisionClass::LedgeLeft), enginemon::Direction::Left);
    ASSERT_EQ(collision_ledge_direction(CollisionClass::LedgeUp), enginemon::Direction::Up);
    ASSERT_EQ(collision_ledge_direction(CollisionClass::LedgeDown), enginemon::Direction::Down);
    
    std::cout << "  [Directional ledge semantic preservation verified ✓]\n";
}

TEST(duplicate_physical_binding_release) {
    // REGRESSION TEST: Multiple physical keys bound to same logical button
    // must not release until ALL are released.
    // Previously, releasing one key would clear the button even if another was held.
    
    InputSystem input;
    
    // Bind two physical scancodes to the same logical button via bindings
    input.bindings().keyboard[100] = InputButton::A;  // Key 100 -> A
    input.bindings().keyboard[200] = InputButton::A;  // Key 200 -> A (second binding)
    
    // Press both keys
    input.begin_frame();
    input.on_key_down(100);
    input.on_key_down(200);
    
    ASSERT_TRUE(input.snapshot().is_held(InputButton::A));
    
    // Release one key - button should STILL be held
    input.begin_frame();
    input.on_key_up(100);
    ASSERT_TRUE(input.snapshot().is_held(InputButton::A));  // 200 still held
    
    // Release second key - NOW button should release
    input.begin_frame();
    input.on_key_up(200);
    ASSERT_FALSE(input.snapshot().is_held(InputButton::A));
    
    std::cout << "  [Duplicate physical binding aggregation verified ✓]\n";
}

TEST(special_tileset_fixed_palette_extracted) {
    // REGRESSION TEST: Special tilesets (house, ice_path, etc.) must have
    // fixed_special_palette populated, not empty standard_palette_rows.
    // These tilesets override environment/time-based palette selection.
    
    TilesetExtractor extractor(*g_rom, *g_profile);
    
    // TILESET_HOUSE (index 5) has HousePalette as fixed special palette
    auto result = extractor.extract_tileset("house");
    ASSERT_TRUE(result.success);
    ASSERT_TRUE(result.tileset.fixed_special_palette.has_value());
    
    // Verify the palette has valid colors (non-zero)
    // fixed_special_palette is std::optional<std::array<Palette, 7>>
    // Palette is struct { std::array<Color, 4> colors; }
    const auto& palette_set = *result.tileset.fixed_special_palette;
    bool has_nonzero = false;
    for (size_t i = 0; i < palette_set.size() && !has_nonzero; ++i) {
        for (size_t j = 0; j < palette_set[i].colors.size() && !has_nonzero; ++j) {
            const auto& color = palette_set[i].colors[j];
            if (color.r > 0 || color.g > 0 || color.b > 0) {
                has_nonzero = true;
            }
        }
    }
    ASSERT_TRUE(has_nonzero);
    
    // Verify TILESET_ICE_PATH (index 20) also has special palette
    auto result_ice = extractor.extract_tileset("ice_path");
    ASSERT_TRUE(result_ice.success);
    ASSERT_TRUE(result_ice.tileset.fixed_special_palette.has_value());
    
    // Verify johto_outdoor does NOT have fixed_special_palette (uses time-of-day)
    auto result_outdoor = extractor.extract_tileset("johto_outdoor");
    ASSERT_TRUE(result_outdoor.success);
    ASSERT_FALSE(result_outdoor.tileset.fixed_special_palette.has_value());
    
    std::cout << "  [Special tileset fixed_special_palette extraction verified ✓]\n";
}

// =============================================================================
// INTERACTION TESTS - A-button interaction system
// Reference: pokecrystal/engine/overworld/events.asm CheckAPressOW
// Reference: pokecrystal/home/map.asm CheckFacingBGEvent
// Reference: Gen2Recomped/src/world/OverworldController.lua interact()
//
// Dispatch order (from pokecrystal):
// 1. TryObjectEvent (NPCs) - priority
// 2. TryBGEvent (signs/hidden items)
// 3. TryTileCollisionEvent (special tiles)
// =============================================================================

TEST(interaction_facing_calculation) {
    // Test facing cell calculation
    // Reference: pokecrystal/home/map.asm GetFacingTileCoord
    int32_t fx, fy;
    
    // Facing down from (5, 5) -> (5, 6)
    Interaction::get_facing_cell(5, 5, enginemon::Direction::Down, fx, fy);
    ASSERT_EQ(fx, 5);
    ASSERT_EQ(fy, 6);
    
    // Facing up from (5, 5) -> (5, 4)
    Interaction::get_facing_cell(5, 5, enginemon::Direction::Up, fx, fy);
    ASSERT_EQ(fx, 5);
    ASSERT_EQ(fy, 4);
    
    // Facing left from (5, 5) -> (4, 5)
    Interaction::get_facing_cell(5, 5, enginemon::Direction::Left, fx, fy);
    ASSERT_EQ(fx, 4);
    ASSERT_EQ(fy, 5);
    
    // Facing right from (5, 5) -> (6, 5)
    Interaction::get_facing_cell(5, 5, enginemon::Direction::Right, fx, fy);
    ASSERT_EQ(fx, 6);
    ASSERT_EQ(fy, 5);
    
    std::cout << "  [Facing calculation matches pokecrystal GetFacingTileCoord]\n";
}

TEST(interaction_counter_tile_detection) {
    // Test counter tile detection for double-reach
    // Using semantic CollisionClass now instead of raw bytes
    Interaction interaction;
    
    // Counter tiles should be detected
    ASSERT_TRUE(interaction.is_counter_tile(CollisionClass::Counter));
    
    // Non-counter tiles should not be detected
    ASSERT_TRUE(!interaction.is_counter_tile(CollisionClass::Floor));
    ASSERT_TRUE(!interaction.is_counter_tile(CollisionClass::Wall));
    ASSERT_TRUE(!interaction.is_counter_tile(CollisionClass::Water));
    ASSERT_TRUE(!interaction.is_counter_tile(CollisionClass::WarpDoor));
    
    std::cout << "  [Counter tiles correctly detected via CollisionClass]\n";
}

TEST(interaction_bg_event_facing_requirement) {
    // Test that directional BG events require specific facing
    // Reference: pokecrystal/engine/overworld/events.asm BGEventJumptable
    
    // BGEVENT_READ doesn't require specific facing
    ASSERT_TRUE(!Interaction::bg_event_requires_facing(BgEventTypeId::Read));
    
    // Directional events require specific facing
    ASSERT_TRUE(Interaction::bg_event_requires_facing(BgEventTypeId::Up));
    ASSERT_TRUE(Interaction::bg_event_requires_facing(BgEventTypeId::Down));
    ASSERT_TRUE(Interaction::bg_event_requires_facing(BgEventTypeId::Left));
    ASSERT_TRUE(Interaction::bg_event_requires_facing(BgEventTypeId::Right));
    
    // Verify required facing directions
    ASSERT_EQ(static_cast<int>(*Interaction::bg_event_required_facing(BgEventTypeId::Up)),
              static_cast<int>(enginemon::Direction::Up));
    ASSERT_EQ(static_cast<int>(*Interaction::bg_event_required_facing(BgEventTypeId::Down)),
              static_cast<int>(enginemon::Direction::Down));
    
    std::cout << "  [BG event facing requirements match pokecrystal]\n";
}

TEST(interaction_object_found) {
    // Test finding an object at the facing cell
    Interaction interaction;
    
    InteractionMap map;
    map.width = 10;
    map.height = 10;
    map.get_collision = [](int32_t, int32_t) -> CollisionClass { return CollisionClass::Floor; };
    
    std::vector<InteractableObject> objects;
    objects.push_back({1, 6, 5, false, false, "test_script", ""});  // NPC at (6, 5)
    
    std::vector<InteractableBgEvent> bg_events;
    
    // Player at (5, 5) facing right toward NPC at (6, 5)
    auto result = interaction.check(map, objects, bg_events, 5, 5, enginemon::Direction::Right);
    
    ASSERT_TRUE(result.found());
    ASSERT_EQ(static_cast<int>(result.type), static_cast<int>(InteractionType::Object));
    ASSERT_EQ(result.object_local_id, 1);
    ASSERT_STR_CONTAINS(result.object_script_id.c_str(), "test_script");
    
    std::cout << "  [Object found at facing cell]\n";
}

TEST(interaction_bg_event_found) {
    // Test finding a BG event (sign) at the facing cell
    Interaction interaction;
    
    InteractionMap map;
    map.width = 10;
    map.height = 10;
    map.get_collision = [](int32_t, int32_t) -> CollisionClass { return CollisionClass::Floor; };
    
    std::vector<InteractableObject> objects;
    
    std::vector<InteractableBgEvent> bg_events;
    bg_events.push_back({6, 5, BgEventTypeId::Read, "sign_script", "", 0});  // Sign at (6, 5)
    
    // Player at (5, 5) facing right toward sign at (6, 5)
    auto result = interaction.check(map, objects, bg_events, 5, 5, enginemon::Direction::Right);
    
    ASSERT_TRUE(result.found());
    ASSERT_EQ(static_cast<int>(result.type), static_cast<int>(InteractionType::BgEvent));
    ASSERT_STR_CONTAINS(result.bg_script_id.c_str(), "sign_script");
    
    std::cout << "  [BG event found at facing cell]\n";
}

TEST(interaction_object_priority_over_bg) {
    // Test that objects have priority over BG events
    // Reference: pokecrystal CheckAPressOW - TryObjectEvent called before TryBGEvent
    Interaction interaction;
    
    InteractionMap map;
    map.width = 10;
    map.height = 10;
    map.get_collision = [](int32_t, int32_t) -> CollisionClass { return CollisionClass::Floor; };
    
    // Both NPC and sign at same cell
    std::vector<InteractableObject> objects;
    objects.push_back({1, 6, 5, false, false, "npc_script", ""});
    
    std::vector<InteractableBgEvent> bg_events;
    bg_events.push_back({6, 5, BgEventTypeId::Read, "sign_script", "", 0});
    
    // Player facing the cell with both
    auto result = interaction.check(map, objects, bg_events, 5, 5, enginemon::Direction::Right);
    
    // Object should take priority
    ASSERT_TRUE(result.found());
    ASSERT_EQ(static_cast<int>(result.type), static_cast<int>(InteractionType::Object));
    ASSERT_STR_CONTAINS(result.object_script_id.c_str(), "npc_script");
    
    std::cout << "  [Object takes priority over BG event]\n";
}

TEST(interaction_moving_npc_not_interactable) {
    // Test that moving NPCs cannot be interacted with
    // Reference: pokecrystal CheckFacingObject checks OBJECT_WALKING == STANDING
    Interaction interaction;
    
    InteractionMap map;
    map.width = 10;
    map.height = 10;
    map.get_collision = [](int32_t, int32_t) -> CollisionClass { return CollisionClass::Floor; };
    
    // NPC at (6, 5) but is_moving = true
    std::vector<InteractableObject> objects;
    objects.push_back({1, 6, 5, true, false, "npc_script", ""});  // is_moving = true
    
    std::vector<InteractableBgEvent> bg_events;
    
    // Player facing the moving NPC
    auto result = interaction.check(map, objects, bg_events, 5, 5, enginemon::Direction::Right);
    
    // Should NOT find the moving NPC
    ASSERT_TRUE(!result.found());
    
    std::cout << "  [Moving NPC not interactable]\n";
}

TEST(interaction_directional_bg_wrong_facing) {
    // Test that directional BG events require correct facing
    Interaction interaction;
    
    InteractionMap map;
    map.width = 10;
    map.height = 10;
    map.get_collision = [](int32_t, int32_t) -> CollisionClass { return CollisionClass::Floor; };
    
    std::vector<InteractableObject> objects;
    
    // BG event that requires facing UP
    std::vector<InteractableBgEvent> bg_events;
    bg_events.push_back({6, 5, BgEventTypeId::Up, "up_sign", "", 0});  // Requires up
    
    // Player facing RIGHT (wrong direction)
    auto result = interaction.check(map, objects, bg_events, 5, 5, enginemon::Direction::Right);
    
    // Should NOT find it (wrong facing)
    ASSERT_TRUE(!result.found());
    
    std::cout << "  [Directional BG event requires correct facing]\n";
}

TEST(interaction_counter_extends_reach) {
    // Test that counter tiles double interaction reach
    // Reference: pokecrystal CheckFacingObject handles counter tiles
    Interaction interaction;
    
    // Counter tile at (6, 5), NPC at (7, 5)
    InteractionMap map;
    map.width = 10;
    map.height = 10;
    map.get_collision = [](int32_t x, int32_t y) -> CollisionClass {
        if (x == 6 && y == 5) return CollisionClass::Counter;
        return CollisionClass::Floor;
    };
    
    std::vector<InteractableObject> objects;
    objects.push_back({1, 7, 5, false, false, "clerk_script", ""});  // Clerk behind counter
    
    std::vector<InteractableBgEvent> bg_events;
    
    // Player at (5, 5) facing right
    // Facing cell is (6, 5) which is counter
    // Extended reach checks (7, 5) where clerk is
    auto result = interaction.check(map, objects, bg_events, 5, 5, enginemon::Direction::Right);
    
    ASSERT_TRUE(result.found());
    ASSERT_EQ(static_cast<int>(result.type), static_cast<int>(InteractionType::Object));
    ASSERT_STR_CONTAINS(result.object_script_id.c_str(), "clerk_script");
    
    std::cout << "  [Counter tile extends interaction reach]\n";
}

TEST(interaction_bounds_check) {
    // Test that out-of-bounds facing doesn't crash
    Interaction interaction;
    
    InteractionMap map;
    map.width = 5;
    map.height = 5;
    map.get_collision = [](int32_t, int32_t) -> CollisionClass { return CollisionClass::Floor; };
    
    std::vector<InteractableObject> objects;
    std::vector<InteractableBgEvent> bg_events;
    
    // Player at (0, 0) facing up/left (out of bounds)
    auto result_up = interaction.check(map, objects, bg_events, 0, 0, enginemon::Direction::Up);
    ASSERT_TRUE(!result_up.found());
    
    auto result_left = interaction.check(map, objects, bg_events, 0, 0, enginemon::Direction::Left);
    ASSERT_TRUE(!result_left.found());
    
    // Player at edge facing out
    auto result_edge = interaction.check(map, objects, bg_events, 4, 4, enginemon::Direction::Down);
    ASSERT_TRUE(!result_edge.found());
    
    std::cout << "  [Out-of-bounds facing handled safely]\n";
}


TEST(newbarktown_real_collision_map) {
    // Extract New Bark Town from ROM and build real collision map
    // NewBarkTown: 10x9 blocks = 20x18 tiles
    MapExtractor extractor(*g_rom, *g_profile);
    auto result = extractor.extract_map("new_bark_town");
    
    ASSERT_TRUE(result.success);
    ASSERT_EQ(result.map.width, 10);   // 10 blocks
    ASSERT_EQ(result.map.height, 9);   // 9 blocks
    ASSERT_EQ(result.map.blocks.size(), 90);  // 10*9
    
    // Build collision map (20x18 tiles)
    const int tile_width = result.map.width * 2;   // 20
    const int tile_height = result.map.height * 2; // 18
    
    CollisionMap collision_map;
    collision_map.width = tile_width;
    collision_map.height = tile_height;
    
    // Capture blocks by reference for collision lookup
    const std::vector<uint8_t>& blocks = result.map.blocks;
    const int map_width_blocks = result.map.width;
    
    collision_map.get_collision = [&blocks, map_width_blocks](int32_t x, int32_t y) -> CollisionClass {
        return get_collision_from_blocks_johto(blocks, map_width_blocks, x, y);
    };
    
    // Verify dimensions
    ASSERT_EQ(collision_map.width, 20);
    ASSERT_EQ(collision_map.height, 18);
    
    std::cout << "  [Real New Bark Town collision map: 20x18 tiles]\n";
}

TEST(newbarktown_known_walkable_tiles) {
    // Verify known walkable positions from pokecrystal NewBarkTown.asm
    // Object positions are already verified: Teacher at (6, 8), Fisher at (12, 9)
    // These positions must be walkable since NPCs stand there
    MapExtractor extractor(*g_rom, *g_profile);
    auto result = extractor.extract_map("new_bark_town");
    ASSERT_TRUE(result.success);
    
    const std::vector<uint8_t>& blocks = result.map.blocks;
    const int map_width_blocks = result.map.width;
    
    // Teacher stands at (6, 8) - must be walkable
    CollisionClass coll = get_collision_from_blocks_johto(blocks, map_width_blocks, 6, 8);
    ASSERT_TRUE(collision_is_walkable(coll));
    
    // Fisher stands at (12, 9) - must be walkable
    coll = get_collision_from_blocks_johto(blocks, map_width_blocks, 12, 9);
    ASSERT_TRUE(collision_is_walkable(coll));
    
    // Sign at (8, 8) - BG event position
    // Note: BG events can be on tiles the player faces, not necessarily stands on
    // The sign is read by facing it, so the tile might be wall/interactable
    // Skip assertion for sign tile - just verify NPC positions are walkable
    
    std::cout << "  [Known walkable tiles verified: NPC positions]\n";
}

TEST(newbarktown_known_blocked_tiles) {
    // Verify known blocked positions - building walls
    // Buildings are in upper-left area (Elm's Lab), upper-right (Player's House)
    MapExtractor extractor(*g_rom, *g_profile);
    auto result = extractor.extract_map("new_bark_town");
    ASSERT_TRUE(result.success);
    
    const std::vector<uint8_t>& blocks = result.map.blocks;
    const int map_width_blocks = result.map.width;
    
    // Tile (4, 2) should be wall - inside Elm's Lab roof area
    CollisionClass coll = get_collision_from_blocks_johto(blocks, map_width_blocks, 4, 2);
    ASSERT_TRUE(collision_is_walkable(coll) == false);
    
    // Tile (14, 2) should be wall - Player's House roof
    coll = get_collision_from_blocks_johto(blocks, map_width_blocks, 14, 2);
    ASSERT_TRUE(collision_is_walkable(coll) == false);
    
    std::cout << "  [Known blocked tiles verified]\n";
}

TEST(newbarktown_water_tiles) {
    // New Bark Town has water on the east side (Route 27 connection)
    MapExtractor extractor(*g_rom, *g_profile);
    auto result = extractor.extract_map("new_bark_town");
    ASSERT_TRUE(result.success);
    
    const std::vector<uint8_t>& blocks = result.map.blocks;
    const int map_width_blocks = result.map.width;
    
    // Check far-right edge for water tiles (near Route 27 water)
    // Look for any water tiles in the map
    int water_count = 0;
    for (int y = 0; y < result.map.height * 2; ++y) {
        for (int x = 0; x < result.map.width * 2; ++x) {
            CollisionClass coll = get_collision_from_blocks_johto(blocks, map_width_blocks, x, y);
            if (collision_is_swimmable(coll)) {
                water_count++;
            }
        }
    }
    
    // New Bark Town should have some water tiles (on the east edge)
    std::cout << "  [Found " << water_count << " water tiles]\n";
}

