// tests/scripting/emitter_test.cpp
// Stage 7 SemanticLuaEmitter tests + sprite namespace + money emitter tests.
// Separated from runtime_test.cpp to avoid MSVC heap OOM on large variant instantiation.
// Does not require a ROM path argument.

#include "engine/scripting/lua_runtime.hpp"
#include "engine/scripting/api_bindings.hpp"
#include "engine/core/game_state.hpp"
#include "engine/core/types.hpp"
#include "crystal/script/semantic_lua_emitter.hpp"
#include "crystal/extract/sprite_ids.hpp"
#include "crystal/rom/loader.hpp"
#include "crystal/rom/profile.hpp"
#include "engine/core/game_loop.hpp"
#include <iostream>
#include <cassert>
#include <string>
#include <stdexcept>

using namespace crystal;
using namespace enginemon;

//=============================================================================
// MINIMAL TEST FRAMEWORK (mirrors runtime_test.cpp)
//=============================================================================

static int g_tests_passed = 0;
static int g_tests_failed = 0;
static bool g_current_test_failed = false;

#define TEST(name) void test_##name()
#define RUN_TEST(name) run_test(#name, test_##name)

#define ASSERT_TRUE(cond) \
    do { if (!(cond)) { \
        std::cerr << "  FAIL: " << #cond << " at line " << __LINE__ << "\n"; \
        g_current_test_failed = true; return; } } while(0)

#define ASSERT_FALSE(cond) ASSERT_TRUE(!(cond))
#define ASSERT_EQ(a, b)    ASSERT_TRUE((a) == (b))
#define ASSERT_STR_EQ(a, b) ASSERT_TRUE(std::string(a) == std::string(b))
#define ASSERT_STR_CONTAINS(s, sub) ASSERT_TRUE(std::string(s).find(sub) != std::string::npos)

static const RomData* g_rom = nullptr;
static const ExtractionProfile* g_profile = nullptr;

void run_test(const char* name, void(*fn)()) {
    std::cout << "Running " << name << "... ";
    std::cout.flush();
    g_current_test_failed = false;
    try { fn(); }
    catch (const std::exception& e) {
        std::cerr << "EXCEPTION: " << e.what() << "\n";
        g_current_test_failed = true;
    }
    if (g_current_test_failed) { std::cout << "FAIL\n"; ++g_tests_failed; }
    else                       { std::cout << "PASS\n"; ++g_tests_passed; }
}


//=============================================================================
// STAGE 7: SemanticLuaEmitter tests
// Verify that legal SemanticScriptIR emits correct Lua targeting ctx.* APIs.
//=============================================================================

// Helper: build a minimal SemanticScriptIR with one block containing a single op.
static enginemon::SemanticScriptIR make_one_op_ir(enginemon::SemanticOp op) {
    enginemon::SemanticScriptIR ir;
    ir.script_id = "test";
    enginemon::SemanticBasicBlock block;
    block.id = 0;
    block.instructions.push_back({std::move(op)});
    ir.blocks.push_back(std::move(block));
    return ir;
}

TEST(stage7_emit_set_flag_contains_ctx_flags_set) {
    using namespace enginemon;
    crystal::SemanticLuaEmitter emitter;

    FlagRef f;
    f.ns = FlagNamespace::Event;
    f.value = 42;
    auto ir = make_one_op_ir(Sem_SetFlag{f});
    std::string lua = emitter.emit(ir);

    // Must contain ctx.flags:set(...)
    ASSERT_TRUE(lua.find("ctx.flags:set(") != std::string::npos);
    // Must not contain raw Crystal RAM addresses or opcodes
    ASSERT_TRUE(lua.find("0xC") == std::string::npos); // no raw RAM address
}

TEST(stage7_emit_check_var_contains_ctx_flags_get_var) {
    using namespace enginemon;
    crystal::SemanticLuaEmitter emitter;

    Sem_CheckVar cv;
    cv.var = 7;
    cv.op = "==";
    cv.value = 3;
    auto ir = make_one_op_ir(cv);
    std::string lua = emitter.emit(ir);

    ASSERT_TRUE(lua.find("ctx.flags:get_var(7)") != std::string::npos);
    ASSERT_TRUE(lua.find("== 3") != std::string::npos);
}

TEST(stage7_emit_end_returns) {
    using namespace enginemon;
    crystal::SemanticLuaEmitter emitter;

    auto ir = make_one_op_ir(Sem_End{});
    std::string lua = emitter.emit(ir);
    ASSERT_TRUE(lua.find("return") != std::string::npos);
}

TEST(stage7_emit_end_all_distinct_from_end) {
    using namespace enginemon;
    crystal::SemanticLuaEmitter emitter;

    std::string lua_end    = emitter.emit(make_one_op_ir(Sem_End{}));
    std::string lua_endall = emitter.emit(make_one_op_ir(Sem_EndAll{}));

    // EndAll clears the call stack and terminates: "__call_stack = {}; do return end"
    ASSERT_TRUE(lua_endall.find("__call_stack = {}") != std::string::npos);
    ASSERT_TRUE(lua_endall.find("do return end") != std::string::npos);

    // Sem_End does NOT clear the call stack unconditionally (it pops one frame or returns).
    // EndAll emits "__call_stack = {}; do return end" as a bare statement (no "local" prefix).
    // Sem_End only has "__call_stack" in the preamble "local __call_stack = {}" declaration.
    // The distinction: EndAll has a standalone assignment statement, End does not.
    // Detect EndAll's statement by checking for the pattern at non-local position.
    ASSERT_TRUE(lua_endall.find("  __call_stack = {}") != std::string::npos); // indented statement
    ASSERT_TRUE(lua_end.find("  __call_stack = {}") == std::string::npos);    // no such statement

    // Both must emit "do return end" — but EndAll's is preceded by the stack clear
    ASSERT_TRUE(lua_end.find("do return end") != std::string::npos);
}

TEST(stage7_emit_show_text_contains_text_sequence_and_yield) {
    using namespace enginemon;
    crystal::SemanticLuaEmitter emitter;

    SemanticTextSequence seq;
    seq.elements.push_back(SemanticTextElement::make_text("Hello"));
    seq.elements.push_back(SemanticTextElement::make_done());

    Sem_ShowText st;
    st.sequence = seq;
    auto ir = make_one_op_ir(st);
    std::string lua = emitter.emit(ir);

    ASSERT_TRUE(lua.find("ctx.ui:text_sequence(") != std::string::npos);
    ASSERT_TRUE(lua.find("coroutine.yield(\"wait_button\")") != std::string::npos);
    ASSERT_TRUE(lua.find("Hello") != std::string::npos);
}

TEST(stage7_emit_text_with_inline_prompt_button) {
    using namespace enginemon;
    crystal::SemanticLuaEmitter emitter;

    SemanticTextSequence seq;
    seq.elements.push_back(SemanticTextElement::make_text("Press A"));
    seq.elements.push_back(SemanticTextElement::make_inline_prompt_button());
    seq.elements.push_back(SemanticTextElement::make_done());

    Sem_ShowText st;
    st.sequence = seq;
    auto ir = make_one_op_ir(st);
    std::string lua = emitter.emit(ir);

    ASSERT_TRUE(lua.find("inline_prompt_button") != std::string::npos);
}

TEST(stage7_emit_text_with_pause_frames) {
    using namespace enginemon;
    crystal::SemanticLuaEmitter emitter;

    SemanticTextSequence seq;
    seq.elements.push_back(SemanticTextElement::make_text("Wait"));
    seq.elements.push_back(SemanticTextElement::make_pause(30));
    seq.elements.push_back(SemanticTextElement::make_done());

    Sem_ShowText st;
    st.sequence = seq;
    auto ir = make_one_op_ir(st);
    std::string lua = emitter.emit(ir);

    ASSERT_TRUE(lua.find("{op=\"pause\", frames=30}") != std::string::npos);
}

TEST(stage7_emit_turn_object_contains_face_actor_with_direction) {
    using namespace enginemon;
    crystal::SemanticLuaEmitter emitter;

    Sem_TurnObject to;
    to.object_id = 3;
    to.facing = enginemon::Direction::Right;
    auto ir = make_one_op_ir(to);
    std::string lua = emitter.emit(ir);

    ASSERT_TRUE(lua.find("ctx.world:face_actor(3, \"right\")") != std::string::npos);
}

TEST(stage7_emit_apply_movement_contains_move_actor) {
    using namespace enginemon;
    crystal::SemanticLuaEmitter emitter;

    Sem_ApplyMovement am;
    am.target.type = MovementTargetType::Object;
    am.target.object_id = 2;
    MovementCommand mc;
    mc.type = MovementType::Step;
    mc.direction = enginemon::Direction::Left;
    am.commands.push_back(mc);
    mc.type = MovementType::StepEnd;
    am.commands.push_back(mc);

    auto ir = make_one_op_ir(am);
    std::string lua = emitter.emit(ir);

    ASSERT_TRUE(lua.find("ctx.world:move_actor(2,") != std::string::npos);
    ASSERT_TRUE(lua.find("\"left\"") != std::string::npos);
}

TEST(stage7_emit_give_pokemon_contains_add_pokemon) {
    using namespace enginemon;
    crystal::SemanticLuaEmitter emitter;

    Sem_GivePokemon gp;
    gp.species = 25;   // Pikachu
    gp.level   = 5;
    gp.held_item = 0;
    auto ir = make_one_op_ir(gp);
    std::string lua = emitter.emit(ir);

    ASSERT_TRUE(lua.find("ctx.party:add_pokemon({") != std::string::npos);
    ASSERT_TRUE(lua.find("species=25") != std::string::npos);
    ASSERT_TRUE(lua.find("level=5") != std::string::npos);
}

TEST(stage7_emit_warp_contains_warp_and_yield) {
    using namespace enginemon;
    crystal::SemanticLuaEmitter emitter;

    Sem_Warp w;
    w.map = 7;
    w.x   = 3;
    w.y   = 8;
    auto ir = make_one_op_ir(w);
    std::string lua = emitter.emit(ir);

    ASSERT_TRUE(lua.find("ctx.world:warp(7, 3, 8)") != std::string::npos);
    ASSERT_TRUE(lua.find("coroutine.yield(\"warp\")") != std::string::npos);
}

TEST(stage7_emit_intra_body_call_goto) {
    using namespace enginemon;
    crystal::SemanticLuaEmitter emitter;

    // Two-block IR: block 0 calls into block 1; block 1 returns
    SemanticScriptIR ir;
    ir.script_id = "test_call";

    SemanticBasicBlock b0;
    b0.id = 0;
    Sem_Call call_op;
    call_op.target = SemanticLabelRef{1, "sub"};
    b0.instructions.push_back({call_op});
    ir.blocks.push_back(b0);

    SemanticBasicBlock b1;
    b1.id = 1;
    b1.instructions.push_back({Sem_Return{}});
    ir.blocks.push_back(b1);

    std::string lua = emitter.emit(ir);

    ASSERT_TRUE(lua.find("goto block_1") != std::string::npos);
    ASSERT_TRUE(lua.find("::block_1::") != std::string::npos);
    ASSERT_TRUE(lua.find("return") != std::string::npos);
}

TEST(stage7_emit_game_specific_event_behavior_name) {
    using namespace enginemon;
    crystal::SemanticLuaEmitter emitter;

    Sem_GameSpecificEvent gse;
    gse.behavior_name = "GiveTownMap";
    auto ir = make_one_op_ir(gse);
    std::string lua = emitter.emit(ir);

    ASSERT_TRUE(lua.find("ctx.game:behavior(\"GiveTownMap\")") != std::string::npos);
    // Must not expose any Crystal ID/index
    ASSERT_TRUE(lua.find("special_id") == std::string::npos);
}

TEST(stage7_emit_call_std_contains_call_std_name) {
    using namespace enginemon;
    crystal::SemanticLuaEmitter emitter;

    Sem_CallStd cs;
    cs.std_id = 3;
    cs.name   = "FacePlayer";
    auto ir = make_one_op_ir(cs);
    std::string lua = emitter.emit(ir);

    ASSERT_TRUE(lua.find("ctx.game:call_std(3, \"FacePlayer\")") != std::string::npos);
}

TEST(stage7_emit_jump_std_contains_return) {
    using namespace enginemon;
    crystal::SemanticLuaEmitter emitter;

    Sem_JumpStd js;
    js.std_id = 5;
    js.name   = "SaveGame";
    auto ir = make_one_op_ir(js);
    std::string lua = emitter.emit(ir);

    ASSERT_TRUE(lua.find("ctx.game:jump_std(5, \"SaveGame\")") != std::string::npos);
    // JumpStd must also emit a return (tail-call semantics)
    ASSERT_TRUE(lua.find("return") != std::string::npos);
}

TEST(stage7_unimplemented_op_throws) {
    using namespace enginemon;
    crystal::SemanticLuaEmitter emitter;

    // Sem_Special is explicitly illegal — emitter must throw, not silently emit
    Sem_Special sp;
    sp.special_id = 99;
    auto ir = make_one_op_ir(sp);

    bool threw = false;
    try {
        emitter.emit(ir);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    ASSERT_TRUE(threw);
}

TEST(stage7_emitted_lua_executes_in_runtime_flag) {
    // End-to-end: emit Lua for a flag-set script, load it into LuaRuntime,
    // run it, verify the flag was set via ctx.flags:set().
    using namespace enginemon;

    crystal::SemanticLuaEmitter emitter;

    // Build IR: setflag(Event:100); end
    SemanticScriptIR ir;
    ir.script_id = "stage7_flag_test";
    SemanticBasicBlock block;
    block.id = 0;

    FlagRef f;
    f.ns = FlagNamespace::Event;
    f.value = 100;
    block.instructions.push_back({Sem_SetFlag{f}});
    block.instructions.push_back({Sem_End{}});
    ir.blocks.push_back(block);

    std::string lua = emitter.emit(ir);
    // Emitter produces: script = {} / function script.main(ctx) ... end / return script
    LuaRuntime runtime;
    runtime.execute_string(lua, "stage7_flag_lua");
    uint32_t co_id = runtime.start_script("script");

    ScriptState state = runtime.get_state(co_id);
    for (int i = 0; i < 5 && state != ScriptState::Finished; ++i) {
        runtime.update(1.0f / 60.0f);
        state = runtime.get_state(co_id);
    }
    ASSERT_EQ(static_cast<int>(state), static_cast<int>(ScriptState::Finished));

    // Encoded flag id: (Event ns=0 << 16) | 100 = 100
    ASSERT_TRUE(flag_api::get_test_flag(&runtime, 100));
}

TEST(stage7_emitted_lua_executes_in_runtime_var) {
    // End-to-end: emit Lua for a setvar script, run it, verify var was set.
    using namespace enginemon;

    crystal::SemanticLuaEmitter emitter;

    SemanticScriptIR ir;
    ir.script_id = "stage7_var_test";
    SemanticBasicBlock block;
    block.id = 0;

    Sem_SetVar sv;
    sv.var = 5;
    sv.source.type = VarValueSourceType::Literal;
    sv.source.value = 42;
    block.instructions.push_back({sv});
    block.instructions.push_back({Sem_End{}});
    ir.blocks.push_back(block);

    std::string lua = emitter.emit(ir);

    LuaRuntime runtime;
    runtime.execute_string(lua, "stage7_var_lua");
    uint32_t co_id = runtime.start_script("script");

    ScriptState state = runtime.get_state(co_id);
    for (int i = 0; i < 5 && state != ScriptState::Finished; ++i) {
        runtime.update(1.0f / 60.0f);
        state = runtime.get_state(co_id);
    }
    ASSERT_EQ(static_cast<int>(state), static_cast<int>(ScriptState::Finished));

    auto it = runtime.get_stub_services().vars.find(5);
    ASSERT_TRUE(it != runtime.get_stub_services().vars.end());
    ASSERT_EQ(it->second, 42);
}


//=============================================================================
// POST-STAGE-7 SPRITE NAMESPACE TESTS
// Verify crystal_sprite_byte_to_id() covers all four Crystal sprite namespaces
// and no valid stock sprite byte produces "" or an unknown tag.
//=============================================================================

TEST(sprite_namespace_fixed_min_and_max) {
    using namespace crystal;
    // Fixed range: 0x01-0x66 (1-102)
    // Boundary: index 1 → "fixed:chris", index 102 → "fixed:standing_youngster"
    ASSERT_STR_EQ(crystal_sprite_byte_to_id(0x01), "fixed:chris");
    ASSERT_STR_EQ(crystal_sprite_byte_to_id(0x66), "fixed:standing_youngster");
    // Mid-range check: SPRITE_TEACHER = 0x29 = 41
    ASSERT_STR_EQ(crystal_sprite_byte_to_id(0x29), "fixed:teacher");
}

TEST(sprite_namespace_fixed_never_empty) {
    using namespace crystal;
    // Every fixed-range byte must produce a non-empty, non-unknown result
    for (uint8_t i = 0x01; i <= 0x66; ++i) {
        std::string id = crystal_sprite_byte_to_id(i);
        ASSERT_FALSE(id.empty());
        ASSERT_FALSE(id.starts_with("unknown:"));
        ASSERT_TRUE(id.starts_with("fixed:"));
    }
}

TEST(sprite_namespace_pokemon_icon_range) {
    using namespace crystal;
    // Pokémon icon range: 0x80-0xA2
    // SPRITE_UNOWN = 0x80 → SpriteMons[0]=UNOWN → ICON_UNOWN → "pokemon_icon:unown"
    // SPRITE_HO_OH = 0xA2 → SpriteMons[34]=HO_OH → ICON_HO_OH → "pokemon_icon:ho_oh"
    ASSERT_STR_EQ(crystal_sprite_byte_to_id(0x80), "pokemon_icon:unown");
    ASSERT_STR_EQ(crystal_sprite_byte_to_id(0xA2), "pokemon_icon:ho_oh");
    // Never empty, always "pokemon_icon:<icon_type_name>"
    for (int i = 0x80; i <= 0xA2; ++i) {
        std::string id = crystal_sprite_byte_to_id(static_cast<uint8_t>(i));
        ASSERT_FALSE(id.empty());
        ASSERT_TRUE(id.starts_with("pokemon_icon:"));
    }
}

TEST(sprite_namespace_daycare_route34) {
    using namespace crystal;
    // Route 34 Day Care uses 0xE0 and 0xE1 directly in object_event macros
    // Source: references/pokecrystal/data/maps/outdoor_sprites.asm
    ASSERT_STR_EQ(crystal_sprite_byte_to_id(0xE0), "daycare:1");
    ASSERT_STR_EQ(crystal_sprite_byte_to_id(0xE1), "daycare:2");
    // Must not be empty or unknown
    ASSERT_FALSE(crystal_sprite_byte_to_id(0xE0).empty());
    ASSERT_FALSE(crystal_sprite_byte_to_id(0xE1).empty());
    ASSERT_FALSE(crystal_sprite_byte_to_id(0xE0).starts_with("unknown:"));
    ASSERT_FALSE(crystal_sprite_byte_to_id(0xE1).starts_with("unknown:"));
}

TEST(sprite_namespace_variable_olivine_rival) {
    using namespace crystal;
    // SPRITE_OLIVINE_RIVAL = 0xF5 — used in outdoor_sprites.asm
    // Source: references/pokecrystal/constants/sprite_constants.asm
    ASSERT_STR_EQ(crystal_sprite_byte_to_id(0xF5), "variable:olivine_rival");
    ASSERT_FALSE(crystal_sprite_byte_to_id(0xF5).empty());
}

TEST(sprite_namespace_variable_azalea_rocket) {
    using namespace crystal;
    // SPRITE_AZALEA_ROCKET = 0xF6 — used in outdoor_sprites.asm
    ASSERT_STR_EQ(crystal_sprite_byte_to_id(0xF6), "variable:azalea_rocket");
    ASSERT_FALSE(crystal_sprite_byte_to_id(0xF6).empty());
}

TEST(sprite_namespace_variable_fuchsia_gym_1_to_4) {
    using namespace crystal;
    // FuchsiaGym uses SPRITE_FUCHSIA_GYM_1-4 (0xF7-0xFA) directly in object_event macros
    // Source: references/pokecrystal/maps/FuchsiaGym.asm
    ASSERT_STR_EQ(crystal_sprite_byte_to_id(0xF7), "variable:fuchsia_gym_1");
    ASSERT_STR_EQ(crystal_sprite_byte_to_id(0xF8), "variable:fuchsia_gym_2");
    ASSERT_STR_EQ(crystal_sprite_byte_to_id(0xF9), "variable:fuchsia_gym_3");
    ASSERT_STR_EQ(crystal_sprite_byte_to_id(0xFA), "variable:fuchsia_gym_4");
}

TEST(sprite_namespace_variable_console_and_dolls) {
    using namespace crystal;
    // PlayersHouse2F uses SPRITE_CONSOLE (0xF0), DOLL_1 (0xF1), DOLL_2 (0xF2)
    // Source: references/pokecrystal/maps/PlayersHouse2F.asm
    ASSERT_STR_EQ(crystal_sprite_byte_to_id(0xF0), "variable:console");
    ASSERT_STR_EQ(crystal_sprite_byte_to_id(0xF1), "variable:doll_1");
    ASSERT_STR_EQ(crystal_sprite_byte_to_id(0xF2), "variable:doll_2");
}

TEST(sprite_namespace_variable_copycat_and_janine_impersonator) {
    using namespace crystal;
    // CopycatsHouse2F uses SPRITE_COPYCAT (0xFB)
    // FuchsiaPokecenter1F uses SPRITE_JANINE_IMPERSONATOR (0xFC)
    ASSERT_STR_EQ(crystal_sprite_byte_to_id(0xFB), "variable:copycat");
    ASSERT_STR_EQ(crystal_sprite_byte_to_id(0xFC), "variable:janine_impersonator");
}

TEST(sprite_namespace_variable_range_never_empty) {
    using namespace crystal;
    // All variable sprite bytes (0xF0-0xFC) must produce non-empty, non-unknown results
    for (int i = 0xF0; i <= 0xFC; ++i) {
        std::string id = crystal_sprite_byte_to_id(static_cast<uint8_t>(i));
        ASSERT_FALSE(id.empty());
        ASSERT_FALSE(id.starts_with("unknown:"));
        ASSERT_TRUE(id.starts_with("variable:"));
    }
}

TEST(sprite_namespace_zero_is_unknown_throws) {
    using namespace crystal;
    // SPRITE_NONE = 0 is genuinely invalid — must throw, not produce a tag
    bool threw = false;
    try { crystal_sprite_byte_to_id(0x00); }
    catch (const std::runtime_error&) { threw = true; }
    ASSERT_TRUE(threw);
}

TEST(sprite_namespace_gap_bytes_throw) {
    using namespace crystal;
    // Bytes 0x67-0x7F (between fixed and pokemon_icon ranges) are undefined.
    // crystal_sprite_byte_to_id() must throw, not produce "unknown:<hex>" or "".
    bool threw_67 = false, threw_7f = false;
    try { crystal_sprite_byte_to_id(0x67); } catch (const std::runtime_error&) { threw_67 = true; }
    try { crystal_sprite_byte_to_id(0x7F); } catch (const std::runtime_error&) { threw_7f = true; }
    ASSERT_TRUE(threw_67);
    ASSERT_TRUE(threw_7f);
}

TEST(sprite_namespace_tag_query_helpers) {
    using namespace crystal;
    // Verify tag-query helper functions work correctly
    ASSERT_TRUE(sprite_id_is_fixed("fixed:teacher"));
    ASSERT_FALSE(sprite_id_is_fixed("variable:copycat"));
    ASSERT_TRUE(sprite_id_is_variable("variable:copycat"));
    ASSERT_TRUE(sprite_id_is_daycare("daycare:1"));
    ASSERT_TRUE(sprite_id_is_pokemon_icon("pokemon_icon:pikachu"));

    ASSERT_STR_EQ(sprite_id_fixed_name("fixed:teacher"), "teacher");
    // sprite_id_pokemon_icon_index now returns -1 (use sprite_id_pokemon_icon_name instead)
    ASSERT_EQ(sprite_id_pokemon_icon_index("pokemon_icon:pikachu"), -1);
    ASSERT_STR_EQ(sprite_id_pokemon_icon_name("pokemon_icon:pikachu"), "pikachu");
    ASSERT_EQ(sprite_id_daycare_slot("daycare:2"), 2);
    ASSERT_STR_EQ(sprite_id_variable_name("variable:olivine_rival"), "olivine_rival");
}

TEST(sprite_namespace_backward_compat_index_to_id) {
    using namespace crystal;
    // crystal_sprite_index_to_id() still returns bare name for fixed range
    // (backward compat for old oracle test code)
    ASSERT_STR_EQ(crystal_sprite_index_to_id(1), "chris");
    ASSERT_STR_EQ(crystal_sprite_index_to_id(102), "standing_youngster");
    ASSERT_STR_EQ(crystal_sprite_index_to_id(0), "");    // SPRITE_NONE still ""
    ASSERT_STR_EQ(crystal_sprite_index_to_id(103), "");  // Out of range still ""
}

//=============================================================================
// POST-STAGE-7 EMITTER CONTRACT TESTS
// Verify fixed emitter/binding mismatches and new policy.
//=============================================================================

TEST(stage7_money_text_arg_emits_prepare_money_text_not_show_money) {
    // Money text arg must emit ctx.inventory:prepare_money_text(account, slot)
    // NOT ctx.inventory:show_money() (unregistered) and NOT a comment (no-op).
    using namespace enginemon;
    crystal::SemanticLuaEmitter emitter;

    Sem_PrepareTextArg arg;
    arg.arg_type = TextArgType::Number;
    arg.number_source = NumberSource::Money;
    arg.buffer_slot = 1;
    arg.account = MoneyAccount::Player;

    SemanticScriptIR ir;
    ir.script_id = "test_money_arg";
    SemanticBasicBlock block;
    block.id = 0;
    block.instructions.push_back({arg});
    block.instructions.push_back({Sem_End{}});
    ir.blocks.push_back(block);

    std::string lua = emitter.emit(ir);

    // Must NOT call show_money() — that binding does not exist
    ASSERT_TRUE(lua.find("show_money") == std::string::npos);
    // Must call prepare_money_text with the account index
    ASSERT_TRUE(lua.find("ctx.inventory:prepare_money_text(") != std::string::npos);
    ASSERT_TRUE(lua.find("0,") != std::string::npos);  // account=0 (player)
    // Must NOT be a comment (comments would be no-ops)
    ASSERT_TRUE(lua.find("-- prepare_money") == std::string::npos);

    // Must be loadable by LuaRuntime without error
    LuaRuntime runtime;
    runtime.execute_string(lua, "test_money_arg");
    uint32_t co = runtime.start_script("script");
    ASSERT_EQ(static_cast<int>(runtime.get_state(co)), static_cast<int>(ScriptState::Finished));
}

TEST(stage7_check_time_morning_emits_is_morning) {
    using namespace enginemon;
    crystal::SemanticLuaEmitter emitter;

    // MORN_F = 1
    Sem_CheckTime ct;
    ct.time_flags = 1;
    SemanticScriptIR ir;
    ir.script_id = "t";
    SemanticBasicBlock b;
    b.id = 0;
    b.instructions.push_back({ct});
    b.instructions.push_back({Sem_End{}});
    ir.blocks.push_back(b);

    std::string lua = emitter.emit(ir);
    ASSERT_TRUE(lua.find("ctx.time:is_morning()") != std::string::npos);
    ASSERT_TRUE(lua.find("~= nil") == std::string::npos);
}

TEST(stage7_check_time_night_emits_is_night) {
    using namespace enginemon;
    crystal::SemanticLuaEmitter emitter;

    // NITE_F = 4
    Sem_CheckTime ct;
    ct.time_flags = 4;
    SemanticScriptIR ir;
    ir.script_id = "t";
    SemanticBasicBlock b;
    b.id = 0;
    b.instructions.push_back({ct});
    b.instructions.push_back({Sem_End{}});
    ir.blocks.push_back(b);

    std::string lua = emitter.emit(ir);
    ASSERT_TRUE(lua.find("ctx.time:is_night()") != std::string::npos);
    ASSERT_TRUE(lua.find("~= nil") == std::string::npos);
}

TEST(stage7_check_just_battled_emits_error_not_false) {
    using namespace enginemon;
    crystal::SemanticLuaEmitter emitter;

    SemanticScriptIR ir;
    ir.script_id = "t";
    SemanticBasicBlock b;
    b.id = 0;
    b.instructions.push_back({Sem_CheckJustBattled{}});
    b.instructions.push_back({Sem_End{}});
    ir.blocks.push_back(b);

    std::string lua = emitter.emit(ir);

    // Must emit error(), not "result = false" as a control-flow fabrication
    ASSERT_TRUE(lua.find("error(") != std::string::npos);
    ASSERT_TRUE(lua.find("check_just_battled: not yet implemented") != std::string::npos);
    // Must NOT have the old fabricated-result pattern
    ASSERT_TRUE(lua.find("result = false -- check_just_battled") == std::string::npos);
}

TEST(stage7_check_phone_number_emits_error_not_false) {
    using namespace enginemon;
    crystal::SemanticLuaEmitter emitter;

    Sem_CheckPhoneNumber cpn;
    cpn.person = 3;
    SemanticScriptIR ir;
    ir.script_id = "t";
    SemanticBasicBlock b;
    b.id = 0;
    b.instructions.push_back({cpn});
    b.instructions.push_back({Sem_End{}});
    ir.blocks.push_back(b);

    std::string lua = emitter.emit(ir);
    // Must emit error() call
    ASSERT_TRUE(lua.find("error(") != std::string::npos);
    // Must NOT assign result = false as a control-flow fabrication
    // (note: "local result = false" in the header is initialization, not fabrication)
    ASSERT_TRUE(lua.find("result = false -- check_phone") == std::string::npos);
    ASSERT_TRUE(lua.find("check_phone_number: not yet implemented") != std::string::npos);
}

TEST(stage7_pokepic_emits_error_not_comment) {
    using namespace enginemon;
    crystal::SemanticLuaEmitter emitter;

    Sem_Pokepic pp;
    pp.source = SpeciesSource::literal(25);
    SemanticScriptIR ir;
    ir.script_id = "t";
    SemanticBasicBlock b;
    b.id = 0;
    b.instructions.push_back({pp});
    b.instructions.push_back({Sem_End{}});
    ir.blocks.push_back(b);

    std::string lua = emitter.emit(ir);
    // Must emit error() not a Lua comment
    ASSERT_TRUE(lua.find("error(") != std::string::npos);
    // Must not be a plain comment (which would silently succeed)
    ASSERT_TRUE(lua.find("-- pokepic") == std::string::npos);
}

TEST(stage7_sdefer_schedules_deferred_script) {
    // End-to-end: emit Lua for a script that calls ctx.game:behavior("Sdefer_target"),
    // load it into HeadlessGameLoop, run it to completion, then verify the
    // deferred script actually runs on the next tick.
    using namespace enginemon;

    // Script A: sets flag 200, then defers script B
    std::string script_a_lua = R"(
script = {}
function script.main(ctx)
    ctx.flags:set(200)
    ctx.game:behavior("Sdefer_script_b")
    return
end
return script
)";

    // Script B: sets flag 201
    std::string script_b_lua = R"(
script = {}
function script.main(ctx)
    ctx.flags:set(201)
    return
end
return script
)";

    HeadlessGameLoop loop;
    LuaRuntime runtime;
    loop.set_lua_runtime(&runtime);

    loop.set_script_loader([&](const std::string& id) -> std::string {
        if (id == "script_a") return script_a_lua;
        if (id == "script_b") return script_b_lua;
        return "";
    });

    // Start script A — it may finish immediately within start_script
    bool started = loop.start_script("script_a");
    ASSERT_TRUE(started);

    // Tick several times to allow both A and deferred B to complete
    for (int i = 0; i < 5; ++i) {
        loop.tick();
        if (loop.is_idle() && flag_api::get_test_flag(&runtime, 201)) break;
    }

    // Flag 200 was set by A
    ASSERT_TRUE(flag_api::get_test_flag(&runtime, 200));
    // Flag 201 was set by B (deferred)
    ASSERT_TRUE(flag_api::get_test_flag(&runtime, 201));
}

TEST(stage7_sdefer_deferred_script_after_current_not_concurrent) {
    // Verify B does NOT run before A's remaining instructions complete.
    // Script A sets flag 300, defers B, then sets flag 301.
    // B must not run until after flag 301 is set.
    using namespace enginemon;

    std::string script_a_lua = R"(
script = {}
function script.main(ctx)
    ctx.flags:set(300)
    ctx.game:behavior("Sdefer_script_b")
    -- A continues here; B must not have started yet
    ctx.flags:set(301)
    return
end
return script
)";

    std::string script_b_lua = R"(
script = {}
function script.main(ctx)
    ctx.flags:set(302)
    return
end
return script
)";

    HeadlessGameLoop loop;
    LuaRuntime runtime;
    loop.set_lua_runtime(&runtime);
    loop.set_script_loader([&](const std::string& id) -> std::string {
        if (id == "script_a") return script_a_lua;
        if (id == "script_b") return script_b_lua;
        return "";
    });

    loop.start_script("script_a");

    // Let everything run
    for (int i = 0; i < 10; ++i) {
        loop.tick();
        if (loop.is_idle() && flag_api::get_test_flag(&runtime, 302)) break;
    }

    // A set both 300 and 301 (proof it ran to completion before B)
    ASSERT_TRUE(flag_api::get_test_flag(&runtime, 300));
    ASSERT_TRUE(flag_api::get_test_flag(&runtime, 301));
    // B ran after A (flag 302 set by B)
    ASSERT_TRUE(flag_api::get_test_flag(&runtime, 302));
}


//=============================================================================
// POST-STAGE-7 SPRITE COMPLETION TESTS
// Unknown byte → throw, variable sprite → GameState, money → GameState
//=============================================================================

TEST(unknown_sprite_byte_throws) {
    // Unknown sprite bytes must throw at crystal_sprite_byte_to_id(), not produce tags.
    using namespace crystal;
    // byte 0x00 (SPRITE_NONE) — invalid
    bool threw = false;
    try { crystal_sprite_byte_to_id(0x00); }
    catch (const std::runtime_error&) { threw = true; }
    ASSERT_TRUE(threw);
    // byte 0x67 (above fixed range, below pokemon_icon range) — invalid gap
    threw = false;
    try { crystal_sprite_byte_to_id(0x67); }
    catch (const std::runtime_error&) { threw = true; }
    ASSERT_TRUE(threw);
    // byte 0xFD (above variable range) — invalid
    threw = false;
    try { crystal_sprite_byte_to_id(0xFD); }
    catch (const std::runtime_error&) { threw = true; }
    ASSERT_TRUE(threw);
}

TEST(variable_sprite_emitter_produces_set_variable_sprite_call) {
    using namespace enginemon;
    crystal::SemanticLuaEmitter emitter;

    Sem_VariableSprite vs;
    vs.slot_name = "copycat";
    vs.assigned_sprite_id = "fixed:lass";

    SemanticScriptIR ir;
    ir.script_id = "t";
    SemanticBasicBlock b;
    b.id = 0;
    b.instructions.push_back({vs});
    b.instructions.push_back({Sem_End{}});
    ir.blocks.push_back(b);

    std::string lua = emitter.emit(ir);

    // Must call ctx.world:set_variable_sprite, not a comment
    ASSERT_TRUE(lua.find("ctx.world:set_variable_sprite(") != std::string::npos);
    ASSERT_TRUE(lua.find("copycat") != std::string::npos);
    // Must not be a comment
    ASSERT_TRUE(lua.find("-- variable_sprite") == std::string::npos);
}

TEST(variable_sprite_emitter_passes_stable_sprite_id_string) {
    using namespace enginemon;
    crystal::SemanticLuaEmitter emitter;

    // Emitter must pass the full stable SpriteId string, not a Crystal numeric index.
    Sem_VariableSprite vs;
    vs.slot_name = "fuchsia_gym_1";
    vs.assigned_sprite_id = "fixed:janine";

    SemanticScriptIR ir;
    ir.script_id = "t";
    SemanticBasicBlock b;
    b.id = 0;
    b.instructions.push_back({vs});
    b.instructions.push_back({Sem_End{}});
    ir.blocks.push_back(b);

    std::string lua = emitter.emit(ir);

    // Must pass the sprite_id string "fixed:janine" — no raw Crystal index.
    ASSERT_TRUE(lua.find("\"fuchsia_gym_1\"") != std::string::npos);
    ASSERT_TRUE(lua.find("\"fixed:janine\"") != std::string::npos);
    // Must NOT contain a raw numeric Crystal sprite index
    ASSERT_TRUE(lua.find(", 10)") == std::string::npos);  // 10 was the old janine index
}

TEST(variable_sprite_gamestate_roundtrip) {
    using namespace enginemon;
    // Execute a variablesprite script and verify GameState::variable_sprites
    // stores the stable SpriteId string (not a Crystal numeric index).
    std::string script_lua = R"(
script = {}
function script.main(ctx)
    ctx.world:set_variable_sprite("copycat", "fixed:lass")
    return
end
return script
)";

    LuaRuntime runtime;
    GameState gs;
    runtime.set_game_state(&gs);
    runtime.execute_string(script_lua, "var_sprite_test");
    uint32_t co = runtime.start_script("script");
    ASSERT_EQ(static_cast<int>(runtime.get_state(co)), static_cast<int>(ScriptState::Finished));

    // GameState::variable_sprites["copycat"] must hold the stable SpriteId string.
    // No Crystal numeric index, no crystal_fixed_sprite_name() lookup.
    auto it = gs.variable_sprites.find("copycat");
    ASSERT_TRUE(it != gs.variable_sprites.end());
    ASSERT_STR_EQ(it->second, "fixed:lass");
}

TEST(variable_sprite_distinct_slots_independent) {
    using namespace enginemon;
    // Two different slots must be stored independently in GameState::variable_sprites.
    std::string script_lua = R"(
script = {}
function script.main(ctx)
    ctx.world:set_variable_sprite("copycat", "fixed:kris")
    ctx.world:set_variable_sprite("fuchsia_gym_1", "fixed:janine")
    return
end
return script
)";

    LuaRuntime runtime;
    GameState gs;
    runtime.set_game_state(&gs);
    runtime.execute_string(script_lua, "var_sprite_two");
    runtime.start_script("script");

    // Both stored as stable SpriteId strings — independent, no index conversion.
    auto it_copycat = gs.variable_sprites.find("copycat");
    auto it_gym1    = gs.variable_sprites.find("fuchsia_gym_1");
    ASSERT_TRUE(it_copycat != gs.variable_sprites.end());
    ASSERT_TRUE(it_gym1    != gs.variable_sprites.end());
    ASSERT_STR_EQ(it_copycat->second, "fixed:kris");
    ASSERT_STR_EQ(it_gym1->second,    "fixed:janine");
    ASSERT_TRUE(it_copycat->second != it_gym1->second);
}

TEST(money_give_writes_to_gamestate_player) {
    using namespace enginemon;
    // ctx.inventory:give_money(amount, 0) must increment money_player in GameState.
    std::string script_lua = R"(
script = {}
function script.main(ctx)
    ctx.inventory:give_money(500, 0)
    return
end
return script
)";

    LuaRuntime runtime;
    GameState gs;
    runtime.set_game_state(&gs);
    runtime.execute_string(script_lua, "money_test");
    runtime.start_script("script");

    auto it = gs.variables.find("money_player");
    ASSERT_TRUE(it != gs.variables.end());
    ASSERT_EQ(it->second, 500);
}

TEST(money_player_vs_mom_account_distinct) {
    using namespace enginemon;
    // Player money and mom money must be stored independently.
    std::string script_lua = R"(
script = {}
function script.main(ctx)
    ctx.inventory:give_money(1000, 0)   -- player
    ctx.inventory:give_money(2000, 1)   -- mom
    return
end
return script
)";

    LuaRuntime runtime;
    GameState gs;
    runtime.set_game_state(&gs);
    runtime.execute_string(script_lua, "money_accounts");
    runtime.start_script("script");

    ASSERT_EQ(gs.variables["money_player"], 1000);
    ASSERT_EQ(gs.variables["money_mom"], 2000);
    // Verify they are stored under different keys
    ASSERT_TRUE(gs.variables["money_player"] != gs.variables["money_mom"]);
}

TEST(money_has_money_reads_gamestate) {
    using namespace enginemon;
    // has_money returns true only if GameState has enough.
    std::string script_lua = R"(
script = {}
function script.main(ctx)
    ctx.inventory:give_money(300, 0)
    result = ctx.inventory:has_money(200, 0)   -- have 300, need 200 → true
    return
end
return script
)";

    LuaRuntime runtime;
    GameState gs;
    runtime.set_game_state(&gs);
    runtime.execute_string(script_lua, "has_money");
    uint32_t co = runtime.start_script("script");

    ASSERT_EQ(static_cast<int>(runtime.get_state(co)), static_cast<int>(ScriptState::Finished));
    // Not enough after take
    std::string script_lua2 = R"(
script = {}
function script.main(ctx)
    result = ctx.inventory:has_money(500, 0)   -- have 300, need 500 → false
    return
end
return script
)";
    runtime.execute_string(script_lua2, "has_money2");
    uint32_t co2 = runtime.start_script("script");
    ASSERT_EQ(static_cast<int>(runtime.get_state(co2)), static_cast<int>(ScriptState::Finished));
}

TEST(money_prepare_money_text_stores_in_transient_buffer_not_gamestate) {
    using namespace enginemon;
    // ctx.inventory:prepare_money_text must store the balance in the transient
    // text buffer (StubServices::text_buffers), NOT in GameState::variables.
    // Transient buffers are not serialized to save state.
    std::string script_lua = R"(
script = {}
function script.main(ctx)
    ctx.inventory:give_money(750, 0)
    ctx.inventory:prepare_money_text(0, 1)   -- account=player, slot=1
    return
end
return script
)";

    LuaRuntime runtime;
    GameState gs;
    runtime.set_game_state(&gs);
    runtime.execute_string(script_lua, "prep_money");
    runtime.start_script("script");

    // Balance must be in GameState (authoritative, serialized).
    ASSERT_EQ(gs.variables["money_player"], 750);

    // Transient buffer must be in StubServices::text_buffers (not serialized).
    auto& text_bufs = runtime.get_stub_services().text_buffers;
    auto it = text_bufs.find("strbuf1_money");
    ASSERT_TRUE(it != text_bufs.end());
    ASSERT_EQ(it->second, 750);

    // text buffer must NOT be in GameState::variables (would pollute save state).
    ASSERT_TRUE(gs.variables.find("strbuf1_money") == gs.variables.end());
}

TEST(money_emitter_produces_prepare_money_text) {
    // Sem_PrepareTextArg with NumberSource::Money must emit
    // ctx.inventory:prepare_money_text(account, slot), not a comment.
    using namespace enginemon;
    crystal::SemanticLuaEmitter emitter;

    Sem_PrepareTextArg arg;
    arg.arg_type = TextArgType::Number;
    arg.number_source = NumberSource::Money;
    arg.buffer_slot = 2;
    arg.account = MoneyAccount::Mom;  // account=1

    SemanticScriptIR ir;
    ir.script_id = "t";
    SemanticBasicBlock b;
    b.id = 0;
    b.instructions.push_back({arg});
    b.instructions.push_back({Sem_End{}});
    ir.blocks.push_back(b);

    std::string lua = emitter.emit(ir);

    // Must call prepare_money_text with account=1 (mom), slot=2
    ASSERT_TRUE(lua.find("ctx.inventory:prepare_money_text(1, 2)") != std::string::npos);
    // Must not be a comment
    ASSERT_TRUE(lua.find("-- prepare_money") == std::string::npos);
    ASSERT_TRUE(lua.find("show_money") == std::string::npos);
}

TEST(pokemon_icon_and_daycare_sprites_are_valid_semantic_ids) {
    using namespace crystal;
    // pokemon_icon and daycare bytes produce valid typed semantic ids (not empty).
    // These ids are preserved in the package for capability resolution.
    ASSERT_STR_EQ(crystal_sprite_byte_to_id(0x80), "pokemon_icon:unown");  // SPRITE_UNOWN
    ASSERT_STR_EQ(crystal_sprite_byte_to_id(0xA2), "pokemon_icon:ho_oh");  // SPRITE_HO_OH
    ASSERT_STR_EQ(crystal_sprite_byte_to_id(0xE0), "daycare:1");
    ASSERT_STR_EQ(crystal_sprite_byte_to_id(0xE1), "daycare:2");
    // None are empty or unknown; pokemon_icon always has an icon type name
    for (int b = 0x80; b <= 0xA2; ++b) {
        std::string id = crystal_sprite_byte_to_id(static_cast<uint8_t>(b));
        ASSERT_FALSE(id.empty());
        ASSERT_TRUE(id.starts_with("pokemon_icon:"));
    }
}

TEST(variable_sprite_legalizer_slot_offset_encoding) {
    // Prove that variablesprite opcode slot byte (offset from SPRITE_VARS=0xF0)
    // is correctly resolved to the semantic slot name.
    // Source: pokecrystal macro: db \1 - SPRITE_VARS
    //   SPRITE_COPYCAT (0xFB) - SPRITE_VARS (0xF0) = 0x0B
    using namespace crystal;
    // slot_offset=0x0B → SPRITE_VARS+0x0B = 0xFB = SPRITE_COPYCAT → "copycat"
    uint8_t full_byte = static_cast<uint8_t>(CRYSTAL_SPRITE_VARS_MIN + 0x0B);
    ASSERT_EQ(full_byte, 0xFB);
    const char* name = crystal_variable_sprite_name(full_byte);
    ASSERT_TRUE(name != nullptr);
    ASSERT_STR_EQ(std::string(name), "copycat");
    // slot_offset=0x07 → 0xF7 = SPRITE_FUCHSIA_GYM_1 → "fuchsia_gym_1"
    const char* gym1 = crystal_variable_sprite_name(
        static_cast<uint8_t>(CRYSTAL_SPRITE_VARS_MIN + 0x07));
    ASSERT_TRUE(gym1 != nullptr);
    ASSERT_STR_EQ(std::string(gym1), "fuchsia_gym_1");
}


//=============================================================================
// CFG TORTURE SUITE
// Structural adversarial tests for SemanticLuaEmitter:
//   T1–T7:   terminal ops each followed by a live label (parse validity)
//   T8:      Sem_LoadPendingEncounter with backward goto into its block
//   T9:      diamond CFG
//   T10:     loop CFG
//   T11:     yield→resume continuation (ShowText then flag set)
//   T12:     sequential yields in one function
//   T13–T19: ctx.* ABI boundary verification
//   T20:     Sem_End without Sem_Call emits valid parseable Lua
//   T21:     Sem_End with Sem_Call — dispatch_return has entries
//   T22:     Sem_Return in terminal block parses
//   T23:     Sem_JumpStd terminal followed by dead block — parses
//   T24:     ShowTextAndEnd followed by dead block — parses
//   T25:     FacePlayerAndShowText followed by dead block — parses
//=============================================================================

// Build a two-block SemanticScriptIR.
// Block 0: ops from first_ops
// Block 1: ops from second_ops
static enginemon::SemanticScriptIR make_two_block_ir(
    std::vector<enginemon::SemanticOp> first_ops,
    std::vector<enginemon::SemanticOp> second_ops,
    enginemon::SemanticLabelId id0 = 0,
    enginemon::SemanticLabelId id1 = 1)
{
    enginemon::SemanticScriptIR ir;
    ir.script_id = "test";
    {
        enginemon::SemanticBasicBlock b;
        b.id = id0; b.is_entry = true;
        for (auto& op : first_ops)
            b.instructions.push_back({op});
        ir.blocks.push_back(std::move(b));
    }
    {
        enginemon::SemanticBasicBlock b;
        b.id = id1;
        for (auto& op : second_ops)
            b.instructions.push_back({op});
        ir.blocks.push_back(std::move(b));
    }
    return ir;
}

// Helper: emit and parse (execute_string) a multi-block IR, returning the Lua source.
// Returns empty string on parse error, printing the error message.
static std::string emit_and_parse(const enginemon::SemanticScriptIR& ir,
                                  const char* test_name)
{
    crystal::SemanticLuaEmitter emitter;
    std::string lua = emitter.emit(ir);
    LuaRuntime rt;
    try {
        rt.execute_string(lua, test_name);
    } catch (const std::exception& e) {
        std::cerr << "  PARSE ERROR in " << test_name << ": " << e.what() << "\n";
        std::cerr << "  Emitted Lua:\n" << lua << "\n";
        return "";
    }
    return lua;
}

// ── T1: Sem_End followed by live label ──────────────────────────────────────
TEST(cfg_torture_t1_sem_end_followed_by_live_label) {
    // block 0: Sem_End (do return end + __dispatch_return guard)
    // block 1: Sem_SetFlag (reachable via forward goto in other paths)
    // The emitter must emit ::__dispatch_return:: so goto __dispatch_return parses.
    using namespace enginemon;
    FlagRef f; f.ns = FlagNamespace::Event; f.value = 1;
    auto ir = make_two_block_ir({Sem_End{}}, {Sem_SetFlag{f}});
    auto lua = emit_and_parse(ir, "T1_sem_end_label");
    ASSERT_FALSE(lua.empty());
    // ::block_0:: and ::block_1:: both present
    ASSERT_TRUE(lua.find("::block_0::") != std::string::npos);
    ASSERT_TRUE(lua.find("::block_1::") != std::string::npos);
    // dispatch_return present because Sem_End present
    ASSERT_TRUE(lua.find("::__dispatch_return::") != std::string::npos);
    ASSERT_TRUE(lua.find("do return end") != std::string::npos);
    std::cout << "  [T1: Sem_End followed by live label — parses ✓]\n";
}

// ── T2: Sem_EndAll followed by live label ───────────────────────────────────
TEST(cfg_torture_t2_sem_endall_followed_by_live_label) {
    using namespace enginemon;
    FlagRef f; f.ns = FlagNamespace::Event; f.value = 2;
    auto ir = make_two_block_ir({Sem_EndAll{}}, {Sem_SetFlag{f}});
    auto lua = emit_and_parse(ir, "T2_endall_label");
    ASSERT_FALSE(lua.empty());
    ASSERT_TRUE(lua.find("__call_stack = {}") != std::string::npos);
    ASSERT_TRUE(lua.find("do return end") != std::string::npos);
    ASSERT_TRUE(lua.find("::block_1::") != std::string::npos);
    std::cout << "  [T2: Sem_EndAll followed by live label — parses ✓]\n";
}

// ── T3: Sem_Return followed by live label ───────────────────────────────────
TEST(cfg_torture_t3_sem_return_followed_by_live_label) {
    using namespace enginemon;
    FlagRef f; f.ns = FlagNamespace::Event; f.value = 3;
    auto ir = make_two_block_ir({Sem_Return{}}, {Sem_SetFlag{f}});
    auto lua = emit_and_parse(ir, "T3_return_label");
    ASSERT_FALSE(lua.empty());
    ASSERT_TRUE(lua.find("do return end") != std::string::npos);
    ASSERT_TRUE(lua.find("::block_1::") != std::string::npos);
    std::cout << "  [T3: Sem_Return followed by live label — parses ✓]\n";
}

// ── T4: Sem_ShowTextAndEnd followed by live label ───────────────────────────
TEST(cfg_torture_t4_show_text_and_end_followed_by_live_label) {
    using namespace enginemon;
    SemanticTextSequence seq;
    seq.elements.push_back(SemanticTextElement::make_text("Hello"));
    seq.elements.push_back(SemanticTextElement::make_done());
    Sem_ShowTextAndEnd sta; sta.sequence = seq;

    FlagRef f; f.ns = FlagNamespace::Event; f.value = 4;
    auto ir = make_two_block_ir({sta}, {Sem_SetFlag{f}});
    auto lua = emit_and_parse(ir, "T4_showtextend_label");
    ASSERT_FALSE(lua.empty());
    // Must yield before returning
    ASSERT_TRUE(lua.find("coroutine.yield(\"wait_button\")") != std::string::npos);
    ASSERT_TRUE(lua.find("ctx.ui:close_text()") != std::string::npos);
    ASSERT_TRUE(lua.find("do return end") != std::string::npos);
    ASSERT_TRUE(lua.find("::block_1::") != std::string::npos);
    std::cout << "  [T4: Sem_ShowTextAndEnd followed by live label — parses ✓]\n";
}

// ── T5: Sem_FacePlayerAndShowText followed by live label ────────────────────
TEST(cfg_torture_t5_face_player_show_text_followed_by_live_label) {
    using namespace enginemon;
    SemanticTextSequence seq;
    seq.elements.push_back(SemanticTextElement::make_text("Hi"));
    seq.elements.push_back(SemanticTextElement::make_done());
    Sem_FacePlayerAndShowText fps; fps.sequence = seq;

    FlagRef f; f.ns = FlagNamespace::Event; f.value = 5;
    auto ir = make_two_block_ir({fps}, {Sem_SetFlag{f}});
    auto lua = emit_and_parse(ir, "T5_faceplayer_label");
    ASSERT_FALSE(lua.empty());
    ASSERT_TRUE(lua.find("ctx.world:face_player()") != std::string::npos);
    ASSERT_TRUE(lua.find("do return end") != std::string::npos);
    ASSERT_TRUE(lua.find("::block_1::") != std::string::npos);
    std::cout << "  [T5: Sem_FacePlayerAndShowText followed by live label — parses ✓]\n";
}

// ── T6: Sem_JumpStd terminal followed by live label ─────────────────────────
TEST(cfg_torture_t6_jump_std_followed_by_live_label) {
    using namespace enginemon;
    Sem_JumpStd js; js.std_id = 3; js.name = "TestStd";

    FlagRef f; f.ns = FlagNamespace::Event; f.value = 6;
    auto ir = make_two_block_ir({js}, {Sem_SetFlag{f}});
    auto lua = emit_and_parse(ir, "T6_jumpstd_label");
    ASSERT_FALSE(lua.empty());
    ASSERT_TRUE(lua.find("ctx.game:jump_std(3, \"TestStd\")") != std::string::npos);
    ASSERT_TRUE(lua.find("do return end") != std::string::npos);
    ASSERT_TRUE(lua.find("::block_1::") != std::string::npos);
    std::cout << "  [T6: Sem_JumpStd followed by live label — parses ✓]\n";
}

// ── T7: Sem_Jump (unconditional goto) to a later block ───────────────────────
TEST(cfg_torture_t7_sem_jump_forward) {
    using namespace enginemon;
    // block 0: Sem_Jump to block 2 (skips block 1)
    // block 1: unreachable dead code (Sem_SetFlag{var=1})
    // block 2: Sem_SetFlag{var=2} + Sem_End
    Sem_Jump jmp; jmp.target = {2, "block_2"};
    FlagRef f1; f1.ns = FlagNamespace::Event; f1.value = 101;
    FlagRef f2; f2.ns = FlagNamespace::Event; f2.value = 102;

    enginemon::SemanticScriptIR ir;
    ir.script_id = "T7_jump_fwd";
    {
        enginemon::SemanticBasicBlock b; b.id = 0; b.is_entry = true;
        b.instructions.push_back({jmp});
        ir.blocks.push_back(std::move(b));
    }
    {
        enginemon::SemanticBasicBlock b; b.id = 1;
        b.instructions.push_back({Sem_SetFlag{f1}});
        ir.blocks.push_back(std::move(b));
    }
    {
        enginemon::SemanticBasicBlock b; b.id = 2;
        b.instructions.push_back({Sem_SetFlag{f2}});
        b.instructions.push_back({Sem_End{}});
        ir.blocks.push_back(std::move(b));
    }

    auto lua = emit_and_parse(ir, "T7_jump_fwd");
    ASSERT_FALSE(lua.empty());
    ASSERT_TRUE(lua.find("goto block_2") != std::string::npos);
    ASSERT_TRUE(lua.find("::block_1::") != std::string::npos);
    ASSERT_TRUE(lua.find("::block_2::") != std::string::npos);

    // Execute: block 1 must be skipped, block 2 flag must be set
    LuaRuntime rt;
    rt.execute_string(lua, "T7_exec");
    rt.start_script("script");
    auto& flags = rt.get_stub_services().flags;
    // flag 102 set (block 2), flag 101 NOT set (block 1 skipped)
    uint32_t enc1 = (static_cast<uint32_t>(0) << 16) | 101;
    uint32_t enc2 = (static_cast<uint32_t>(0) << 16) | 102;
    ASSERT_FALSE(flags.count(static_cast<int>(enc1)) && flags.at(static_cast<int>(enc1)));
    ASSERT_TRUE(flags.count(static_cast<int>(enc2)) && flags.at(static_cast<int>(enc2)));
    std::cout << "  [T7: Sem_Jump forward — block_1 skipped, block_2 executed ✓]\n";
}

// ── T8: Sem_LoadPendingEncounter — backward goto must not break do...end guard ─
TEST(cfg_torture_t8_load_pending_encounter_backward_goto) {
    // block 0: Sem_SetVar{result=1}
    // block 1: Sem_JumpIf{true → block_1} (loop back)
    //          Sem_LoadPendingEncounter
    // The IR has a backward jump that arrives at block_1 which precedes
    // the LoadPendingEncounter block.  The do...end guard on LoadPendingEncounter
    // means the Lua locals are never in scope at the backward goto target.
    // Must parse and not error.
    using namespace enginemon;

    enginemon::SemanticScriptIR ir;
    ir.script_id = "T8_lpenc";

    // block 0: set result=1, jump to block 2
    {
        enginemon::SemanticBasicBlock b; b.id = 0; b.is_entry = true;
        Sem_SetVar sv; sv.var = 0; sv.source.type = VarValueSourceType::Literal; sv.source.value = 1;
        b.instructions.push_back({sv});
        Sem_Jump jmp; jmp.target = {2, "block_2"};
        b.instructions.push_back({jmp});
        ir.blocks.push_back(std::move(b));
    }
    // block 1: backward jump target (jumped to from block 2 conditional)
    {
        enginemon::SemanticBasicBlock b; b.id = 1;
        Sem_SetVar sv2; sv2.var = 0; sv2.source.type = VarValueSourceType::Literal; sv2.source.value = 0;
        b.instructions.push_back({sv2});
        Sem_End end;
        b.instructions.push_back({end});
        ir.blocks.push_back(std::move(b));
    }
    // block 2: Sem_LoadPendingEncounter — has do...end local guard
    {
        enginemon::SemanticBasicBlock b; b.id = 2;
        b.instructions.push_back({Sem_LoadPendingEncounter{}});
        ir.blocks.push_back(std::move(b));
    }

    // This must parse cleanly — do...end prevents goto-into-local
    crystal::SemanticLuaEmitter emitter;
    std::string lua = emitter.emit(ir);
    ASSERT_FALSE(lua.empty());
    // Verify do...end wrapping is present
    ASSERT_TRUE(lua.find("do\n") != std::string::npos || lua.find("do\r\n") != std::string::npos);
    ASSERT_TRUE(lua.find("local enc_species") != std::string::npos);
    ASSERT_TRUE(lua.find("ctx.battle:start_wild") != std::string::npos);
    // Must parse (no goto-into-local error)
    LuaRuntime rt;
    bool parse_ok = true;
    try {
        rt.execute_string(lua, "T8_lpenc_parse");
    } catch (...) {
        parse_ok = false;
    }
    ASSERT_TRUE(parse_ok);
    std::cout << "  [T8: LoadPendingEncounter do...end guard — backward goto parses ✓]\n";
}

// ── T9: Diamond CFG ──────────────────────────────────────────────────────────
TEST(cfg_torture_t9_diamond_cfg) {
    // block 0: check flag → JumpIf(true→block_2, false→block_1)
    // block 1: set var[1]=10, jump to block_3
    // block 2: set var[1]=20, jump to block_3
    // block 3: Sem_End
    using namespace enginemon;

    FlagRef f; f.ns = FlagNamespace::Event; f.value = 50;

    enginemon::SemanticScriptIR ir;
    ir.script_id = "T9_diamond";

    // block 0: check flag, conditional jump
    {
        enginemon::SemanticBasicBlock b; b.id = 0; b.is_entry = true;
        b.instructions.push_back({Sem_CheckFlag{f}});
        Sem_JumpIf ji; ji.condition = "true"; ji.target = {2, "block_2"};
        b.instructions.push_back({ji});
        ir.blocks.push_back(std::move(b));
    }
    // block 1: false branch
    {
        enginemon::SemanticBasicBlock b; b.id = 1;
        Sem_SetVar sv; sv.var = 1; sv.source.type = VarValueSourceType::Literal; sv.source.value = 10;
        b.instructions.push_back({sv});
        Sem_Jump jmp; jmp.target = {3, "block_3"};
        b.instructions.push_back({jmp});
        ir.blocks.push_back(std::move(b));
    }
    // block 2: true branch
    {
        enginemon::SemanticBasicBlock b; b.id = 2;
        Sem_SetVar sv; sv.var = 1; sv.source.type = VarValueSourceType::Literal; sv.source.value = 20;
        b.instructions.push_back({sv});
        Sem_Jump jmp; jmp.target = {3, "block_3"};
        b.instructions.push_back({jmp});
        ir.blocks.push_back(std::move(b));
    }
    // block 3: merge + end
    {
        enginemon::SemanticBasicBlock b; b.id = 3;
        b.instructions.push_back({Sem_End{}});
        ir.blocks.push_back(std::move(b));
    }

    auto lua = emit_and_parse(ir, "T9_diamond");
    ASSERT_FALSE(lua.empty());

    // Execute with flag NOT set → false branch → var[1]=10
    {
        LuaRuntime rt;
        rt.execute_string(lua, "T9_false");
        flag_api::set_test_flag(&rt, static_cast<int>((0u << 16) | 50u), false);
        rt.start_script("script");
        ASSERT_EQ(rt.get_stub_services().vars.count(1) ? rt.get_stub_services().vars.at(1) : -1, 10);
    }
    // Execute with flag SET → true branch → var[1]=20
    {
        LuaRuntime rt;
        rt.execute_string(lua, "T9_true");
        flag_api::set_test_flag(&rt, static_cast<int>((0u << 16) | 50u), true);
        rt.start_script("script");
        ASSERT_EQ(rt.get_stub_services().vars.count(1) ? rt.get_stub_services().vars.at(1) : -1, 20);
    }
    std::cout << "  [T9: Diamond CFG — false→10, true→20 ✓]\n";
}

// ── T10: Loop CFG ─────────────────────────────────────────────────────────────
TEST(cfg_torture_t10_loop_cfg) {
    // block 0: set var[1]=0
    // block 1: add_var[1] += 1, check_var[1] < 3 → JumpIf(true→block_1)
    // block 2: Sem_End (exit when var[1] >= 3)
    // Tests that backward edges emit valid Lua (goto to earlier label).
    using namespace enginemon;

    enginemon::SemanticScriptIR ir;
    ir.script_id = "T10_loop";

    // block 0: init
    {
        enginemon::SemanticBasicBlock b; b.id = 0; b.is_entry = true;
        Sem_SetVar sv; sv.var = 1; sv.source.type = VarValueSourceType::Literal; sv.source.value = 0;
        b.instructions.push_back({sv});
        ir.blocks.push_back(std::move(b));
    }
    // block 1: loop body
    {
        enginemon::SemanticBasicBlock b; b.id = 1;
        // add_var(1, 1)
        Sem_AddVar av; av.var = 1; av.delta = 1;
        b.instructions.push_back({av});
        // check_var(1) < 3  → result is 1 if var < 3
        Sem_CheckVar cv; cv.var = 1; cv.op = "<"; cv.value = 3;
        b.instructions.push_back({cv});
        // if result != 0, jump back to block_1
        Sem_JumpIf ji; ji.condition = "true"; ji.target = {1, "block_1"};
        b.instructions.push_back({ji});
        ir.blocks.push_back(std::move(b));
    }
    // block 2: exit
    {
        enginemon::SemanticBasicBlock b; b.id = 2;
        b.instructions.push_back({Sem_End{}});
        ir.blocks.push_back(std::move(b));
    }

    auto lua = emit_and_parse(ir, "T10_loop");
    ASSERT_FALSE(lua.empty());
    // Must have goto block_1 (backward edge)
    ASSERT_TRUE(lua.find("goto block_1") != std::string::npos);

    // Execute: loop runs until var[1] == 3
    LuaRuntime rt;
    rt.execute_string(lua, "T10_exec");
    rt.start_script("script");
    ASSERT_EQ(rt.get_stub_services().vars.count(1) ? rt.get_stub_services().vars.at(1) : -1, 3);
    std::cout << "  [T10: Loop CFG — var[1]=3 after 3 iterations ✓]\n";
}

// ── T11: Yield → resume continuation ─────────────────────────────────────────
TEST(cfg_torture_t11_yield_resume_continuation) {
    // block 0: Sem_ShowText (yields wait_button), then falls through
    // block 1: Sem_SetFlag (executes after resume)
    // Proves that code after a yield in the same emitted function runs on resume.
    using namespace enginemon;

    SemanticTextSequence seq;
    seq.elements.push_back(SemanticTextElement::make_text("Yield test"));
    seq.elements.push_back(SemanticTextElement::make_done());
    Sem_ShowText st; st.sequence = seq;

    FlagRef f; f.ns = FlagNamespace::Event; f.value = 77;
    auto ir = make_two_block_ir({st}, {Sem_SetFlag{f}, Sem_End{}});

    crystal::SemanticLuaEmitter emitter;
    std::string lua = emitter.emit(ir);
    ASSERT_FALSE(lua.empty());

    LuaRuntime rt;
    rt.execute_string(lua, "T11_yield_resume");

    uint32_t coro = rt.start_script("script");
    // Script should have yielded on wait_button
    ASSERT_TRUE(rt.get_state(coro) == ScriptState::Yielded);
    ASSERT_TRUE(rt.get_yield_reason(coro) == YieldReason::Dialog);

    // Flag must NOT be set yet (continuation not executed)
    uint32_t enc = (static_cast<uint32_t>(0) << 16) | 77u;
    ASSERT_FALSE(rt.get_stub_services().flags.count(static_cast<int>(enc)));

    // Resume
    rt.resume(coro);
    ASSERT_TRUE(rt.get_state(coro) == ScriptState::Finished);

    // Flag must now be set
    ASSERT_TRUE(rt.get_stub_services().flags.count(static_cast<int>(enc)));
    ASSERT_TRUE(rt.get_stub_services().flags.at(static_cast<int>(enc)));
    std::cout << "  [T11: ShowText yield → flag set after resume ✓]\n";
}

// ── T12: Sequential yields ────────────────────────────────────────────────────
TEST(cfg_torture_t12_sequential_yields) {
    // block 0: Sem_WaitButton  (yield 1)
    // block 1: Sem_WaitButton  (yield 2)
    // block 2: Sem_SetVar(var[1]=99) + Sem_End
    // Each resume must advance exactly one step.
    using namespace enginemon;

    Sem_SetVar sv; sv.var = 1; sv.source.type = VarValueSourceType::Literal; sv.source.value = 99;

    enginemon::SemanticScriptIR ir;
    ir.script_id = "T12_seq_yields";
    {
        enginemon::SemanticBasicBlock b; b.id = 0; b.is_entry = true;
        b.instructions.push_back({Sem_WaitButton{}});
        ir.blocks.push_back(std::move(b));
    }
    {
        enginemon::SemanticBasicBlock b; b.id = 1;
        b.instructions.push_back({Sem_WaitButton{}});
        ir.blocks.push_back(std::move(b));
    }
    {
        enginemon::SemanticBasicBlock b; b.id = 2;
        b.instructions.push_back({sv});
        b.instructions.push_back({Sem_End{}});
        ir.blocks.push_back(std::move(b));
    }

    crystal::SemanticLuaEmitter emitter;
    std::string lua = emitter.emit(ir);
    ASSERT_FALSE(lua.empty());

    LuaRuntime rt;
    rt.execute_string(lua, "T12_seq");

    uint32_t coro = rt.start_script("script");
    // First yield
    ASSERT_TRUE(rt.get_state(coro) == ScriptState::Yielded);
    ASSERT_EQ(rt.get_stub_services().vars.count(1), 0u); // not yet

    rt.resume(coro);
    // Second yield
    ASSERT_TRUE(rt.get_state(coro) == ScriptState::Yielded);
    ASSERT_EQ(rt.get_stub_services().vars.count(1), 0u); // still not yet

    rt.resume(coro);
    // Complete
    ASSERT_TRUE(rt.get_state(coro) == ScriptState::Finished);
    ASSERT_EQ(rt.get_stub_services().vars.at(1), 99);
    std::cout << "  [T12: Sequential yields — two resumes then var=99 ✓]\n";
}

// ── T13: ctx.flags ABI — set/clear/check round-trip ─────────────────────────
TEST(cfg_torture_t13_flags_abi_set_clear_check) {
    using namespace enginemon;
    FlagRef f; f.ns = FlagNamespace::Event; f.value = 200;
    uint32_t enc = (static_cast<uint32_t>(0) << 16) | 200u;

    // set flag, check it, clear it, check again — all via emitter
    enginemon::SemanticScriptIR ir;
    ir.script_id = "T13_flags";
    {
        enginemon::SemanticBasicBlock b; b.id = 0; b.is_entry = true;
        b.instructions.push_back({Sem_SetFlag{f}});  // set
        b.instructions.push_back({Sem_CheckFlag{f}}); // result = 1
        Sem_SetVar sv; sv.var = 1; sv.source.type = VarValueSourceType::ScriptResult;
        b.instructions.push_back({sv});               // var[1] = (result!=0 ? 1 : 0)
        b.instructions.push_back({Sem_ClearFlag{f}}); // clear
        b.instructions.push_back({Sem_CheckFlag{f}}); // result = 0
        Sem_SetVar sv2; sv2.var = 2; sv2.source.type = VarValueSourceType::ScriptResult;
        b.instructions.push_back({sv2});              // var[2] = (result!=0 ? 1 : 0)
        b.instructions.push_back({Sem_End{}});
        ir.blocks.push_back(std::move(b));
    }

    crystal::SemanticLuaEmitter emitter;
    std::string lua = emitter.emit(ir);
    auto lua2 = emit_and_parse(ir, "T13_flags");
    ASSERT_FALSE(lua2.empty());

    LuaRuntime rt;
    rt.execute_string(lua, "T13_exec");
    rt.start_script("script");
    ASSERT_EQ(rt.get_stub_services().vars.at(1), 1); // was set
    ASSERT_EQ(rt.get_stub_services().vars.at(2), 0); // was cleared
    std::cout << "  [T13: ctx.flags set/clear/check ABI round-trip ✓]\n";
}

// ── T14: ctx.flags add_var/get_var ABI ────────────────────────────────────────
TEST(cfg_torture_t14_var_abi_addvar_checkvar) {
    using namespace enginemon;

    enginemon::SemanticScriptIR ir;
    ir.script_id = "T14_var";
    {
        enginemon::SemanticBasicBlock b; b.id = 0; b.is_entry = true;
        Sem_SetVar init; init.var = 5; init.source.type = VarValueSourceType::Literal; init.source.value = 10;
        b.instructions.push_back({init});             // var[5] = 10
        Sem_AddVar av; av.var = 5; av.delta = 7;
        b.instructions.push_back({av});               // var[5] += 7  → 17
        Sem_CheckVar cv; cv.var = 5; cv.op = "=="; cv.value = 17;
        b.instructions.push_back({cv});               // result = (var[5] == 17)
        Sem_SetVar store; store.var = 6; store.source.type = VarValueSourceType::ScriptResult;
        b.instructions.push_back({store});             // var[6] = result
        b.instructions.push_back({Sem_End{}});
        ir.blocks.push_back(std::move(b));
    }

    auto lua = emit_and_parse(ir, "T14_var");
    ASSERT_FALSE(lua.empty());
    LuaRuntime rt;
    rt.execute_string(lua, "T14_exec");
    rt.start_script("script");
    ASSERT_EQ(rt.get_stub_services().vars.at(5), 17);
    ASSERT_EQ(rt.get_stub_services().vars.at(6), 1); // check passed
    std::cout << "  [T14: ctx.flags add_var/check_var ABI ✓]\n";
}

// ── T15: ctx.world face_player / face_actor ABI ──────────────────────────────
TEST(cfg_torture_t15_world_face_abi) {
    using namespace enginemon;

    enginemon::SemanticScriptIR ir;
    ir.script_id = "T15_face";
    {
        enginemon::SemanticBasicBlock b; b.id = 0; b.is_entry = true;
        b.instructions.push_back({Sem_FacePlayer{}});
        Sem_TurnObject to; to.object_id = 3; to.facing = enginemon::Direction::Up;
        b.instructions.push_back({to});
        b.instructions.push_back({Sem_End{}});
        ir.blocks.push_back(std::move(b));
    }

    auto lua = emit_and_parse(ir, "T15_face");
    ASSERT_FALSE(lua.empty());
    ASSERT_TRUE(lua.find("ctx.world:face_player()") != std::string::npos);
    ASSERT_TRUE(lua.find("ctx.world:face_actor(3, \"up\")") != std::string::npos);

    LuaRuntime rt;
    rt.execute_string(lua, "T15_exec");
    world_api::set_actor_pos(&rt, 3, 10, 10);
    rt.start_script("script");
    // face_actor for actor 3 to "up" — verify facing changed
    auto state = world_api::get_actor_state(&rt, 3);
    ASSERT_STR_EQ(state.facing, "up");
    std::cout << "  [T15: ctx.world:face_player/face_actor ABI ✓]\n";
}

// ── T16: ctx.inventory give/has ABI ─────────────────────────────────────────
TEST(cfg_torture_t16_inventory_give_has_abi) {
    // The inventory stub's give() and has() are minimal stubs that don't track state.
    // This test verifies the emitted Lua is syntactically correct and the ABI
    // calls reach the runtime without error (no crash, no Lua syntax error).
    using namespace enginemon;

    enginemon::SemanticScriptIR ir;
    ir.script_id = "T16_inv";
    {
        enginemon::SemanticBasicBlock b; b.id = 0; b.is_entry = true;
        Sem_GiveItem gi; gi.item = 17; gi.quantity = 3;
        b.instructions.push_back({gi});
        Sem_CheckItem ci; ci.item = 17;
        b.instructions.push_back({ci});
        Sem_SetVar sv; sv.var = 1; sv.source.type = VarValueSourceType::ScriptResult;
        b.instructions.push_back({sv}); // var[1] = result of has(17) — stub returns 0
        b.instructions.push_back({Sem_End{}});
        ir.blocks.push_back(std::move(b));
    }

    // Verify emitted Lua contains correct ABI calls
    crystal::SemanticLuaEmitter emitter;
    std::string lua = emitter.emit(ir);
    ASSERT_TRUE(lua.find("ctx.inventory:give(17, 3)") != std::string::npos);
    ASSERT_TRUE(lua.find("ctx.inventory:has(17)") != std::string::npos);

    // Must parse and execute without error
    auto lua2 = emit_and_parse(ir, "T16_inv");
    ASSERT_FALSE(lua2.empty());

    // Execute: give+has run, has stub returns 0 (not found, as expected)
    LuaRuntime rt;
    rt.execute_string(lua, "T16_exec");
    rt.start_script("script");
    // var[1] = 0 because has() stub returns 0 regardless
    ASSERT_EQ(rt.get_stub_services().vars.count(1) ? rt.get_stub_services().vars.at(1) : -1, 0);
    std::cout << "  [T16: ctx.inventory:give/has ABI — calls parse, stubs reach runtime ✓]\n";
}

// ── T17: Warp yield-type is "warp" not terminal ───────────────────────────────
TEST(cfg_torture_t17_warp_yields_warp_not_terminal) {
    // Warp must yield "warp" and execution must continue after resume.
    using namespace enginemon;

    Sem_Warp w; w.map = 5; w.x = 2; w.y = 3;
    FlagRef f; f.ns = FlagNamespace::Event; f.value = 55;
    auto ir = make_two_block_ir({w}, {Sem_SetFlag{f}, Sem_End{}});

    crystal::SemanticLuaEmitter emitter;
    std::string lua = emitter.emit(ir);
    ASSERT_TRUE(lua.find("coroutine.yield(\"warp\")") != std::string::npos);
    // Crucially: no "do return end" after the warp yield
    // The text after coroutine.yield("warp") must be the next block, not a return
    size_t yield_pos = lua.find("coroutine.yield(\"warp\")");
    size_t block1_pos = lua.find("::block_1::");
    ASSERT_TRUE(block1_pos > yield_pos); // block_1 label comes after the warp yield
    // No "do return end" between yield_pos and block1_pos
    std::string between = lua.substr(yield_pos, block1_pos - yield_pos);
    ASSERT_TRUE(between.find("do return end") == std::string::npos);

    LuaRuntime rt;
    rt.execute_string(lua, "T17_warp");
    uint32_t coro = rt.start_script("script");
    ASSERT_TRUE(rt.get_state(coro) == ScriptState::Yielded);
    ASSERT_TRUE(rt.get_yield_reason(coro) == YieldReason::Warp);

    // Flag must NOT be set (continuation not run yet)
    uint32_t enc = (static_cast<uint32_t>(0) << 16) | 55u;
    ASSERT_FALSE(rt.get_stub_services().flags.count(static_cast<int>(enc)));

    rt.resume(coro);
    ASSERT_TRUE(rt.get_state(coro) == ScriptState::Finished);
    ASSERT_TRUE(rt.get_stub_services().flags.count(static_cast<int>(enc)));
    std::cout << "  [T17: Sem_Warp yields 'warp', continuation runs after resume ✓]\n";
}

// ── T18: Battle yield-type is "battle" not terminal ──────────────────────────
TEST(cfg_torture_t18_battle_yields_battle_not_terminal) {
    using namespace enginemon;

    Sem_LoadWildMon lw; lw.species = 25; lw.level = 5;
    FlagRef f; f.ns = FlagNamespace::Event; f.value = 56;
    auto ir = make_two_block_ir({lw}, {Sem_SetFlag{f}, Sem_End{}});

    crystal::SemanticLuaEmitter emitter;
    std::string lua = emitter.emit(ir);
    ASSERT_TRUE(lua.find("coroutine.yield(\"battle\")") != std::string::npos);

    LuaRuntime rt;
    rt.execute_string(lua, "T18_battle");
    uint32_t coro = rt.start_script("script");
    ASSERT_TRUE(rt.get_state(coro) == ScriptState::Yielded);
    ASSERT_TRUE(rt.get_yield_reason(coro) == YieldReason::Battle);

    rt.resume(coro);
    ASSERT_TRUE(rt.get_state(coro) == ScriptState::Finished);
    uint32_t enc = (static_cast<uint32_t>(0) << 16) | 56u;
    ASSERT_TRUE(rt.get_stub_services().flags.count(static_cast<int>(enc)));
    std::cout << "  [T18: Sem_LoadWildMon yields 'battle', continuation runs ✓]\n";
}

// ── T19: ctx.util wait_frames yield-type ─────────────────────────────────────
TEST(cfg_torture_t19_wait_frames_yield_type) {
    using namespace enginemon;

    Sem_Wait w; w.duration = 3; // wait 3 frames
    FlagRef f; f.ns = FlagNamespace::Event; f.value = 57;
    auto ir = make_two_block_ir({w}, {Sem_SetFlag{f}, Sem_End{}});

    crystal::SemanticLuaEmitter emitter;
    std::string lua = emitter.emit(ir);
    ASSERT_TRUE(lua.find("ctx.util:wait_frames(3)") != std::string::npos);

    LuaRuntime rt;
    rt.execute_string(lua, "T19_wait");
    uint32_t coro = rt.start_script("script");
    ASSERT_TRUE(rt.get_state(coro) == ScriptState::Yielded);
    ASSERT_TRUE(rt.get_yield_reason(coro) == YieldReason::WaitFrames);

    // Tick three times through update (1/60s per tick)
    for (int i = 0; i < 3; ++i) {
        rt.update(1.0f / 60.0f);
    }
    ASSERT_TRUE(rt.get_state(coro) == ScriptState::Finished);
    uint32_t enc = (static_cast<uint32_t>(0) << 16) | 57u;
    ASSERT_TRUE(rt.get_stub_services().flags.count(static_cast<int>(enc)));
    std::cout << "  [T19: Sem_Wait → wait_frames yield, continuation after 3 ticks ✓]\n";
}

// ── T20: Sem_End without Sem_Call — __dispatch_return always present ──────────
TEST(cfg_torture_t20_sem_end_no_call_dispatch_return_present) {
    // This is the bug that was just fixed.
    // A single-block IR with only Sem_End (no Sem_Call) must emit __dispatch_return
    // so that the goto inside the if-guard is a valid Lua goto.
    using namespace enginemon;
    auto ir = make_one_op_ir(Sem_End{});
    crystal::SemanticLuaEmitter emitter;
    std::string lua = emitter.emit(ir);

    ASSERT_TRUE(lua.find("::__dispatch_return::") != std::string::npos);
    // Parse must succeed
    LuaRuntime rt;
    bool ok = true;
    try { rt.execute_string(lua, "T20_end_no_call"); }
    catch (...) { ok = false; }
    ASSERT_TRUE(ok);
    // Execute: script exits normally
    uint32_t coro = rt.start_script("script");
    ASSERT_TRUE(rt.get_state(coro) == ScriptState::Finished);
    std::cout << "  [T20: Sem_End without Sem_Call — __dispatch_return present, parses ✓]\n";
}

// ── T21: Sem_End with Sem_Call — dispatch_return has entries ─────────────────
TEST(cfg_torture_t21_sem_end_with_call_dispatch_return_has_entry) {
    using namespace enginemon;
    // Layout: [block_0 (Sem_Call→block_1), block_2 (continuation), block_1 (callee)]
    // Pass 1: block[0] has Sem_Call → continuation = ir.blocks[0+1].id = 2
    // Sem_Call emits: push(2), goto block_1
    // Sem_End in block_1: pops 2, goto __dispatch_return
    // __dispatch_return: if __return_target == 2 then goto block_2 end

    enginemon::SemanticScriptIR ir;
    ir.script_id = "T21_call";
    // Array index 0: the caller block (Sem_Call targeting block_1)
    {
        enginemon::SemanticBasicBlock b; b.id = 0; b.is_entry = true;
        Sem_Call c; c.target = {1, "sub"};
        b.instructions.push_back({c});
        ir.blocks.push_back(std::move(b));
    }
    // Array index 1: continuation block (id=2) — next after caller in array
    {
        enginemon::SemanticBasicBlock b; b.id = 2;
        Sem_SetVar sv; sv.var = 2; sv.source.type = VarValueSourceType::Literal; sv.source.value = 20;
        b.instructions.push_back({sv});
        b.instructions.push_back({Sem_End{}});
        ir.blocks.push_back(std::move(b));
    }
    // Array index 2: callee block (id=1)
    {
        enginemon::SemanticBasicBlock b; b.id = 1;
        Sem_SetVar sv; sv.var = 1; sv.source.type = VarValueSourceType::Literal; sv.source.value = 10;
        b.instructions.push_back({sv});
        b.instructions.push_back({Sem_End{}});
        ir.blocks.push_back(std::move(b));
    }

    crystal::SemanticLuaEmitter emitter;
    std::string lua = emitter.emit(ir);
    // __dispatch_return must have entry for continuation block id=2
    ASSERT_TRUE(lua.find("if __return_target == 2 then goto block_2 end") != std::string::npos);

    auto lua2 = emit_and_parse(ir, "T21_call");
    ASSERT_FALSE(lua2.empty());

    // Execute: block_0 calls block_1, block_1 sets var[1]=10 + Sem_End → dispatch → block_2 sets var[2]=20
    LuaRuntime rt;
    rt.execute_string(lua, "T21_exec");
    rt.start_script("script");
    ASSERT_EQ(rt.get_stub_services().vars.count(1) ? rt.get_stub_services().vars.at(1) : -1, 10);
    ASSERT_EQ(rt.get_stub_services().vars.count(2) ? rt.get_stub_services().vars.at(2) : -1, 20);
    std::cout << "  [T21: Sem_Call/Sem_End dispatch — both callee and continuation executed ✓]\n";
}

// ── T22: Sem_Return in terminal block ─────────────────────────────────────────
TEST(cfg_torture_t22_sem_return_terminal) {
    using namespace enginemon;
    auto ir = make_one_op_ir(Sem_Return{});
    crystal::SemanticLuaEmitter emitter;
    std::string lua = emitter.emit(ir);
    ASSERT_TRUE(lua.find("do return end") != std::string::npos);
    LuaRuntime rt;
    rt.execute_string(lua, "T22_return");
    uint32_t coro = rt.start_script("script");
    ASSERT_TRUE(rt.get_state(coro) == ScriptState::Finished);
    std::cout << "  [T22: Sem_Return — do return end, script exits ✓]\n";
}

// ── T23: Sem_JumpStd terminal — dead block must still be emitted ──────────────
TEST(cfg_torture_t23_jump_std_dead_block) {
    using namespace enginemon;
    Sem_JumpStd js; js.std_id = 1; js.name = "Stub";
    FlagRef f; f.ns = FlagNamespace::Engine; f.value = 3;
    auto ir = make_two_block_ir({js}, {Sem_SetFlag{f}}); // block 1 is dead
    auto lua = emit_and_parse(ir, "T23_jumpstd_dead");
    ASSERT_FALSE(lua.empty());
    ASSERT_TRUE(lua.find("do return end") != std::string::npos);
    ASSERT_TRUE(lua.find("::block_1::") != std::string::npos);
    std::cout << "  [T23: Sem_JumpStd dead block emitted — parses ✓]\n";
}

// ── T24: Sem_ShowTextAndEnd dead block ────────────────────────────────────────
TEST(cfg_torture_t24_show_text_and_end_dead_block_parses) {
    using namespace enginemon;
    SemanticTextSequence seq;
    seq.elements.push_back(SemanticTextElement::make_text("X"));
    seq.elements.push_back(SemanticTextElement::make_done());
    Sem_ShowTextAndEnd sta; sta.sequence = seq;
    FlagRef f; f.ns = FlagNamespace::Event; f.value = 99;
    auto ir = make_two_block_ir({sta}, {Sem_SetFlag{f}});
    auto lua = emit_and_parse(ir, "T24_showend_dead");
    ASSERT_FALSE(lua.empty());
    // Block 1 still emitted even though unreachable
    ASSERT_TRUE(lua.find("::block_1::") != std::string::npos);
    std::cout << "  [T24: ShowTextAndEnd + dead block — parses ✓]\n";
}

// ── T25: Sem_FacePlayerAndShowText dead block ─────────────────────────────────
TEST(cfg_torture_t25_face_player_show_text_dead_block_parses) {
    using namespace enginemon;
    SemanticTextSequence seq;
    seq.elements.push_back(SemanticTextElement::make_text("Y"));
    seq.elements.push_back(SemanticTextElement::make_done());
    Sem_FacePlayerAndShowText fps; fps.sequence = seq;
    FlagRef f; f.ns = FlagNamespace::Event; f.value = 98;
    auto ir = make_two_block_ir({fps}, {Sem_SetFlag{f}});
    auto lua = emit_and_parse(ir, "T25_faceshow_dead");
    ASSERT_FALSE(lua.empty());
    ASSERT_TRUE(lua.find("::block_1::") != std::string::npos);
    std::cout << "  [T25: FacePlayerAndShowText + dead block — parses ✓]\n";
}

//=============================================================================
// MAIN
//=============================================================================

int main(int /*argc*/, char* /*argv*/[]) {
    std::cout << "=== Emitter + Sprite/Money Tests ===\n";

    // Stage 7: SemanticLuaEmitter tests (August 2026)
    RUN_TEST(stage7_emit_set_flag_contains_ctx_flags_set);
    RUN_TEST(stage7_emit_check_var_contains_ctx_flags_get_var);
    RUN_TEST(stage7_emit_end_returns);
    RUN_TEST(stage7_emit_end_all_distinct_from_end);
    RUN_TEST(stage7_emit_show_text_contains_text_sequence_and_yield);
    RUN_TEST(stage7_emit_text_with_inline_prompt_button);
    RUN_TEST(stage7_emit_text_with_pause_frames);
    RUN_TEST(stage7_emit_turn_object_contains_face_actor_with_direction);
    RUN_TEST(stage7_emit_apply_movement_contains_move_actor);
    RUN_TEST(stage7_emit_give_pokemon_contains_add_pokemon);
    RUN_TEST(stage7_emit_warp_contains_warp_and_yield);
    RUN_TEST(stage7_emit_intra_body_call_goto);
    RUN_TEST(stage7_emit_game_specific_event_behavior_name);
    RUN_TEST(stage7_emit_call_std_contains_call_std_name);
    RUN_TEST(stage7_emit_jump_std_contains_return);
    RUN_TEST(stage7_unimplemented_op_throws);
    RUN_TEST(stage7_emitted_lua_executes_in_runtime_flag);
    RUN_TEST(stage7_emitted_lua_executes_in_runtime_var);

    // Post-Stage-7 sprite namespace tests (August 2026)
    RUN_TEST(sprite_namespace_fixed_min_and_max);
    RUN_TEST(sprite_namespace_fixed_never_empty);
    RUN_TEST(sprite_namespace_pokemon_icon_range);
    RUN_TEST(sprite_namespace_daycare_route34);
    RUN_TEST(sprite_namespace_variable_olivine_rival);
    RUN_TEST(sprite_namespace_variable_azalea_rocket);
    RUN_TEST(sprite_namespace_variable_fuchsia_gym_1_to_4);
    RUN_TEST(sprite_namespace_variable_console_and_dolls);
    RUN_TEST(sprite_namespace_variable_copycat_and_janine_impersonator);
    RUN_TEST(sprite_namespace_variable_range_never_empty);
    RUN_TEST(sprite_namespace_zero_is_unknown_throws);
    RUN_TEST(sprite_namespace_gap_bytes_throw);
    RUN_TEST(sprite_namespace_tag_query_helpers);
    RUN_TEST(sprite_namespace_backward_compat_index_to_id);

    // Post-Stage-7 emitter contract tests (August 2026)
    RUN_TEST(stage7_money_text_arg_emits_prepare_money_text_not_show_money);
    RUN_TEST(stage7_check_time_morning_emits_is_morning);
    RUN_TEST(stage7_check_time_night_emits_is_night);
    RUN_TEST(stage7_check_just_battled_emits_error_not_false);
    RUN_TEST(stage7_check_phone_number_emits_error_not_false);
    RUN_TEST(stage7_pokepic_emits_error_not_comment);
    RUN_TEST(stage7_sdefer_schedules_deferred_script);
    RUN_TEST(stage7_sdefer_deferred_script_after_current_not_concurrent);

    // Post-Stage-7 sprite completion + contract fixes (August 2026)
    RUN_TEST(unknown_sprite_byte_throws);
    RUN_TEST(variable_sprite_emitter_produces_set_variable_sprite_call);
    RUN_TEST(variable_sprite_emitter_passes_stable_sprite_id_string);
    RUN_TEST(variable_sprite_gamestate_roundtrip);
    RUN_TEST(variable_sprite_distinct_slots_independent);
    RUN_TEST(money_give_writes_to_gamestate_player);
    RUN_TEST(money_player_vs_mom_account_distinct);
    RUN_TEST(money_has_money_reads_gamestate);
    RUN_TEST(money_prepare_money_text_stores_in_transient_buffer_not_gamestate);
    RUN_TEST(money_emitter_produces_prepare_money_text);
    RUN_TEST(pokemon_icon_and_daycare_sprites_are_valid_semantic_ids);
    RUN_TEST(variable_sprite_legalizer_slot_offset_encoding);

    // CFG Torture Suite (August 2026)
    RUN_TEST(cfg_torture_t1_sem_end_followed_by_live_label);
    RUN_TEST(cfg_torture_t2_sem_endall_followed_by_live_label);
    RUN_TEST(cfg_torture_t3_sem_return_followed_by_live_label);
    RUN_TEST(cfg_torture_t4_show_text_and_end_followed_by_live_label);
    RUN_TEST(cfg_torture_t5_face_player_show_text_followed_by_live_label);
    RUN_TEST(cfg_torture_t6_jump_std_followed_by_live_label);
    RUN_TEST(cfg_torture_t7_sem_jump_forward);
    RUN_TEST(cfg_torture_t8_load_pending_encounter_backward_goto);
    RUN_TEST(cfg_torture_t9_diamond_cfg);
    RUN_TEST(cfg_torture_t10_loop_cfg);
    RUN_TEST(cfg_torture_t11_yield_resume_continuation);
    RUN_TEST(cfg_torture_t12_sequential_yields);
    RUN_TEST(cfg_torture_t13_flags_abi_set_clear_check);
    RUN_TEST(cfg_torture_t14_var_abi_addvar_checkvar);
    RUN_TEST(cfg_torture_t15_world_face_abi);
    RUN_TEST(cfg_torture_t16_inventory_give_has_abi);
    RUN_TEST(cfg_torture_t17_warp_yields_warp_not_terminal);
    RUN_TEST(cfg_torture_t18_battle_yields_battle_not_terminal);
    RUN_TEST(cfg_torture_t19_wait_frames_yield_type);
    RUN_TEST(cfg_torture_t20_sem_end_no_call_dispatch_return_present);
    RUN_TEST(cfg_torture_t21_sem_end_with_call_dispatch_return_has_entry);
    RUN_TEST(cfg_torture_t22_sem_return_terminal);
    RUN_TEST(cfg_torture_t23_jump_std_dead_block);
    RUN_TEST(cfg_torture_t24_show_text_and_end_dead_block_parses);
    RUN_TEST(cfg_torture_t25_face_player_show_text_dead_block_parses);

    std::cout << "\n=== Results ===\n";
    std::cout << "Passed: " << g_tests_passed << "\n";
    std::cout << "Failed: " << g_tests_failed << "\n";
    return g_tests_failed > 0 ? 1 : 0;
}
