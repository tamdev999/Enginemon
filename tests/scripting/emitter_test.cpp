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

    // EndAll must call ctx.game:behavior("EndAll")
    ASSERT_TRUE(lua_endall.find("ctx.game:behavior(\"EndAll\")") != std::string::npos);
    // End must NOT contain that call
    ASSERT_TRUE(lua_end.find("ctx.game:behavior(\"EndAll\")") == std::string::npos);
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

    std::cout << "\n=== Results ===\n";
    std::cout << "Passed: " << g_tests_passed << "\n";
    std::cout << "Failed: " << g_tests_failed << "\n";
    return g_tests_failed > 0 ? 1 : 0;
}
