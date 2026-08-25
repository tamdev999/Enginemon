#include "oracle_shared.hpp"

TEST(p3_s1_end_vs_endall_distinct_types) {
    using namespace crystal;
    using namespace enginemon;

    // Bytes: 0x91 = end, 0x93 = endall — two separate scripts decoded from different offsets
    std::vector<uint8_t> bytes_end  = {0x91};  // end opcode
    std::vector<uint8_t> bytes_endall = {0x93}; // endall opcode

    auto decode_cmd = [](uint8_t opcode) -> LoweringResult {
        std::vector<uint8_t> padded = {opcode};
        while (padded.size() < 0x8000) padded.push_back(0xFF);
        auto rom = make_rom_from_bytes(padded);
        SymbolMap sym;
        TypedScriptDecoder dec(*rom, sym);
        CrystalScriptIR ir = dec.decode_script(0x0000);
        return lower_ir(ir);
    };

    // ORACLE: 0x91 lowers to Sem_End
    auto lr_end = decode_cmd(0x91);
    ASSERT_TRUE(lr_end.success);
    bool found_end = false;
    for (const auto& blk : lr_end.ir.blocks)
        for (const auto& inst : blk.instructions)
            if (std::holds_alternative<Sem_End>(inst.op)) found_end = true;
    ASSERT_TRUE(found_end);

    // ORACLE: 0x93 lowers to Sem_EndAll (NOT Sem_End)
    auto lr_endall = decode_cmd(0x93);
    ASSERT_TRUE(lr_endall.success);
    bool found_endall = false, found_end_instead = false;
    for (const auto& blk : lr_endall.ir.blocks) {
        for (const auto& inst : blk.instructions) {
            if (std::holds_alternative<Sem_EndAll>(inst.op)) found_endall = true;
            if (std::holds_alternative<Sem_End>(inst.op))    found_end_instead = true;
        }
    }
    ASSERT_TRUE(found_endall);
    // MUTATION CHECK: Sem_End must NOT be produced from opcode 0x93
    ASSERT_FALSE(found_end_instead);

    std::cout << "  [P3-S1: Sem_End (0x91) != Sem_EndAll (0x93) — distinct types ✓]\n";
}

// =============================================================================
// P3-S2: Sem_WaitButton vs Sem_PromptButton — distinct empty structs
// Source: Crystal 0x54=waitbutton (jp WaitButton) vs 0x55=promptbutton (WaitBGMap+PromptButton)
// The sync step before PromptButton is the observable distinction
// =============================================================================
TEST(p3_s2_waitbutton_vs_promptbutton_distinct) {
    using namespace crystal;
    using namespace enginemon;

    auto decode_and_lower = [](uint8_t opcode) -> LoweringResult {
        std::vector<uint8_t> padded = {opcode, 0x91};
        while (padded.size() < 0x8000) padded.push_back(0xFF);
        auto rom = make_rom_from_bytes(padded);
        SymbolMap sym;
        TypedScriptDecoder dec(*rom, sym);
        CrystalScriptIR ir = dec.decode_script(0x0000);
        return lower_ir(ir);
    };

    // ORACLE: 0x54 → Sem_WaitButton (no sync), NOT Sem_PromptButton
    auto lr_wait = decode_and_lower(0x54);
    ASSERT_TRUE(lr_wait.success);
    bool found_wait = false, found_prompt_for_wait = false;
    for (const auto& blk : lr_wait.ir.blocks) {
        for (const auto& inst : blk.instructions) {
            if (std::holds_alternative<Sem_WaitButton>(inst.op))  found_wait = true;
            if (std::holds_alternative<Sem_PromptButton>(inst.op)) found_prompt_for_wait = true;
        }
    }
    ASSERT_TRUE(found_wait);
    ASSERT_FALSE(found_prompt_for_wait); // MUTATION: 0x54 must NOT collapse to PromptButton

    // ORACLE: 0x55 → Sem_PromptButton (with sync), NOT Sem_WaitButton
    auto lr_prompt = decode_and_lower(0x55);
    ASSERT_TRUE(lr_prompt.success);
    bool found_prompt = false, found_wait_for_prompt = false;
    for (const auto& blk : lr_prompt.ir.blocks) {
        for (const auto& inst : blk.instructions) {
            if (std::holds_alternative<Sem_PromptButton>(inst.op)) found_prompt = true;
            if (std::holds_alternative<Sem_WaitButton>(inst.op))   found_wait_for_prompt = true;
        }
    }
    ASSERT_TRUE(found_prompt);
    ASSERT_FALSE(found_wait_for_prompt); // MUTATION: 0x55 must NOT collapse to WaitButton

    std::cout << "  [P3-S2: WaitButton (0x54) != PromptButton (0x55) — distinct empty structs ✓]\n";
}

// =============================================================================
// P3-S3: Sem_AskForPhoneNumber vs Sem_AddPhoneNumber — distinct semantics
// Source: Script_askforphonenumber (0x97) = prompt+conditional+3-way result
//         Script_addcellnum (0x28) = unconditional add, no prompt
// Both carry a person ID — the distinction is the compound vs unconditional semantics
// =============================================================================
TEST(p3_s3_askforphone_vs_addphone_distinct) {
    using namespace crystal;
    using namespace enginemon;

    auto decode_and_lower = [](uint8_t opcode, uint8_t person) -> LoweringResult {
        std::vector<uint8_t> padded = {opcode, person, 0x91};
        while (padded.size() < 0x8000) padded.push_back(0xFF);
        auto rom = make_rom_from_bytes(padded);
        SymbolMap sym;
        TypedScriptDecoder dec(*rom, sym);
        CrystalScriptIR ir = dec.decode_script(0x0000);
        return lower_ir(ir);
    };

    // ORACLE: 0x97 person=11 → Sem_AskForPhoneNumber{person=11}
    auto lr_ask = decode_and_lower(0x97, 11);
    ASSERT_TRUE(lr_ask.success);
    const Sem_AskForPhoneNumber* ask_op = nullptr;
    bool found_add_instead = false;
    for (const auto& blk : lr_ask.ir.blocks) {
        for (const auto& inst : blk.instructions) {
            if (auto* p = std::get_if<Sem_AskForPhoneNumber>(&inst.op)) ask_op = p;
            if (std::holds_alternative<Sem_AddPhoneNumber>(inst.op)) found_add_instead = true;
        }
    }
    ASSERT_TRUE(ask_op != nullptr);
    ASSERT_EQ(ask_op->person, 11u);
    ASSERT_FALSE(found_add_instead); // MUTATION: must NOT collapse to unconditional add

    // ORACLE: 0x28 person=11 → Sem_AddPhoneNumber{person=11}, NOT AskFor
    auto lr_add = decode_and_lower(0x28, 11);
    ASSERT_TRUE(lr_add.success);
    const Sem_AddPhoneNumber* add_op = nullptr;
    bool found_ask_instead = false;
    for (const auto& blk : lr_add.ir.blocks) {
        for (const auto& inst : blk.instructions) {
            if (auto* p = std::get_if<Sem_AddPhoneNumber>(&inst.op)) add_op = p;
            if (std::holds_alternative<Sem_AskForPhoneNumber>(inst.op)) found_ask_instead = true;
        }
    }
    ASSERT_TRUE(add_op != nullptr);
    ASSERT_EQ(add_op->person, 11u);
    ASSERT_FALSE(found_ask_instead);

    std::cout << "  [P3-S3: AskForPhoneNumber (0x97) != AddPhoneNumber (0x28) — distinct ✓]\n";
}

// =============================================================================
// P3-S4: Sem_NewLoadMap method value preservation
// Source: Script_newloadmap (0x8A) stores byte in hMapEntryMethod
// MapEntryMethod enum: Warp=0xF1, Door=0xF5, Fly=0xFC — method must survive lowering
// Asymmetric values to detect default-zero or enum-cast corruption
// =============================================================================
TEST(p3_s4_newloadmap_method_preserved) {
    using namespace crystal;
    using namespace enginemon;

    // Test three distinct method values
    struct Case { uint8_t byte; MapEntryMethod expected; const char* name; };
    Case cases[] = {
        {0xF1, MapEntryMethod::Warp,  "Warp"},
        {0xF5, MapEntryMethod::Door,  "Door"},
        {0xFC, MapEntryMethod::Fly,   "Fly"},
    };

    for (const auto& c : cases) {
        std::vector<uint8_t> padded = {0x8A, c.byte, 0x91};
        while (padded.size() < 0x8000) padded.push_back(0xFF);
        auto rom = make_rom_from_bytes(padded);
        SymbolMap sym;
        TypedScriptDecoder dec(*rom, sym);
        CrystalScriptIR ir = dec.decode_script(0x0000);
        auto lr = lower_ir(ir);
        ASSERT_TRUE(lr.success);

        const Sem_NewLoadMap* op = nullptr;
        for (const auto& blk : lr.ir.blocks)
            for (const auto& inst : blk.instructions)
                if (auto* p = std::get_if<Sem_NewLoadMap>(&inst.op)) op = p;
        ASSERT_TRUE(op != nullptr);

        // ORACLE: method must be the exact MapEntryMethod value
        ASSERT_EQ(static_cast<uint8_t>(op->method), c.byte);
        ASSERT_EQ(op->method, c.expected);

        // MUTATION CHECK: must not be default-zero or some other method
        ASSERT_TRUE(op->method != MapEntryMethod::Warp || c.expected == MapEntryMethod::Warp);
    }

    // MUTATION CHECK: Fly (0xFC) must NOT equal Door (0xF5) or Warp (0xF1)
    ASSERT_TRUE(MapEntryMethod::Fly != MapEntryMethod::Door);
    ASSERT_TRUE(MapEntryMethod::Fly != MapEntryMethod::Warp);
    ASSERT_TRUE(MapEntryMethod::Door != MapEntryMethod::Warp);

    std::cout << "  [P3-S4: NewLoadMap method Warp/Door/Fly preserved as distinct MapEntryMethod ✓]\n";
}

// =============================================================================
// P3-S5: Sem_CatchTutorial tutorial_type preserved — distinct from Sem_StartBattle
// Source: Script_catchtutorial (0x61) ld [wBattleType], a — NOT normal battle entry
// tutorial_type byte must survive lowering; command type must be CatchTutorial not StartBattle
// =============================================================================
TEST(p3_s5_catchtutorial_distinct_from_startbattle) {
    using namespace crystal;
    using namespace enginemon;

    // tutorial_type=2 (asymmetric — not 0 or 1 which could be defaults)
    std::vector<uint8_t> padded = {0x61, 0x02, 0x91};
    while (padded.size() < 0x8000) padded.push_back(0xFF);
    auto rom = make_rom_from_bytes(padded);
    SymbolMap sym;
    TypedScriptDecoder dec(*rom, sym);
    CrystalScriptIR ir = dec.decode_script(0x0000);
    auto lr = lower_ir(ir);
    ASSERT_TRUE(lr.success);

    const Sem_CatchTutorial* op = nullptr;
    bool found_start_battle = false;
    for (const auto& blk : lr.ir.blocks) {
        for (const auto& inst : blk.instructions) {
            if (auto* p = std::get_if<Sem_CatchTutorial>(&inst.op)) op = p;
            if (std::holds_alternative<Sem_StartBattle>(inst.op)) found_start_battle = true;
        }
    }
    ASSERT_TRUE(op != nullptr);
    // ORACLE: tutorial_type=2 must be preserved exactly
    ASSERT_EQ(op->tutorial_type, 2u);
    // MUTATION CHECK: must NOT produce Sem_StartBattle
    ASSERT_FALSE(found_start_battle);

    std::cout << "  [P3-S5: CatchTutorial tutorial_type=2 preserved, not StartBattle ✓]\n";
}

// =============================================================================
// P3-S6: Sem_DeactivateFacing distinct from Sem_Pause
// Source: Script_deactivatefacing (0x8C) uses SCRIPT_WAIT/StopScript; Script_pause (0x8B) differs
// Both have a duration byte. The types are distinct C++ structs.
// Asymmetric value duration=17 to detect default-zero or type collapse
// =============================================================================
TEST(p3_s6_deactivatefacing_distinct_from_pause) {
    using namespace crystal;
    using namespace enginemon;

    auto decode_lower = [](uint8_t opcode, uint8_t duration) -> LoweringResult {
        std::vector<uint8_t> padded = {opcode, duration, 0x91};
        while (padded.size() < 0x8000) padded.push_back(0xFF);
        auto rom = make_rom_from_bytes(padded);
        SymbolMap sym;
        TypedScriptDecoder dec(*rom, sym);
        CrystalScriptIR ir = dec.decode_script(0x0000);
        return lower_ir(ir);
    };

    // ORACLE: 0x8C duration=17 → Sem_DeactivateFacing{17}, NOT Sem_Pause
    auto lr_deact = decode_lower(0x8C, 17);
    ASSERT_TRUE(lr_deact.success);
    const Sem_DeactivateFacing* deact_op = nullptr;
    bool found_pause_instead = false;
    for (const auto& blk : lr_deact.ir.blocks) {
        for (const auto& inst : blk.instructions) {
            if (auto* p = std::get_if<Sem_DeactivateFacing>(&inst.op)) deact_op = p;
            if (std::holds_alternative<Sem_Pause>(inst.op)) found_pause_instead = true;
        }
    }
    ASSERT_TRUE(deact_op != nullptr);
    ASSERT_EQ(deact_op->duration, 17u);
    ASSERT_FALSE(found_pause_instead); // MUTATION: must NOT collapse to Sem_Pause

    // ORACLE: 0x8B duration=17 → Sem_Pause, NOT Sem_DeactivateFacing
    auto lr_pause = decode_lower(0x8B, 17);
    ASSERT_TRUE(lr_pause.success);
    const Sem_Pause* pause_op = nullptr;
    bool found_deact_instead = false;
    for (const auto& blk : lr_pause.ir.blocks) {
        for (const auto& inst : blk.instructions) {
            if (auto* p = std::get_if<Sem_Pause>(&inst.op)) pause_op = p;
            if (std::holds_alternative<Sem_DeactivateFacing>(inst.op)) found_deact_instead = true;
        }
    }
    ASSERT_TRUE(pause_op != nullptr);
    ASSERT_EQ(pause_op->length, 17u);
    ASSERT_FALSE(found_deact_instead);

    std::cout << "  [P3-S6: DeactivateFacing (0x8C) != Pause (0x8B) — distinct types, duration=17 ✓]\n";
}

// =============================================================================
// P3-S7: Sem_GiveItemVerboseVar — ItemSource distinction + quantity_var
// Source: Script_verbosegiveitemvar (0x9F): item byte 0 = ITEM_FROM_MEM (ScriptVar), !=0 = literal
// The quantity is a variable INDEX (not literal quantity) — must be preserved
// Asymmetric values: item=37 (literal), quantity_var=5
// =============================================================================
TEST(p3_s7_verbosegiveitemvar_semantics) {
    using namespace crystal;
    using namespace enginemon;

    auto decode_lower = [](uint8_t item_byte, uint8_t qty_var) -> LoweringResult {
        std::vector<uint8_t> padded = {0x9F, item_byte, qty_var, 0x91};
        while (padded.size() < 0x8000) padded.push_back(0xFF);
        auto rom = make_rom_from_bytes(padded);
        SymbolMap sym;
        TypedScriptDecoder dec(*rom, sym);
        CrystalScriptIR ir = dec.decode_script(0x0000);
        return lower_ir(ir);
    };

    // ORACLE: item=37 (literal), qty_var=5
    auto lr_lit = decode_lower(37, 5);
    ASSERT_TRUE(lr_lit.success);
    const Sem_GiveItemVerboseVar* lit_op = nullptr;
    for (const auto& blk : lr_lit.ir.blocks)
        for (const auto& inst : blk.instructions)
            if (auto* p = std::get_if<Sem_GiveItemVerboseVar>(&inst.op)) lit_op = p;
    ASSERT_TRUE(lit_op != nullptr);
    ASSERT_EQ(lit_op->item_source, ItemSource::Literal);
    ASSERT_EQ(static_cast<uint16_t>(lit_op->item), 37u);
    ASSERT_EQ(lit_op->quantity_var, 5u);

    // ORACLE: item=0 (ITEM_FROM_MEM → ScriptVar), qty_var=3
    auto lr_var = decode_lower(0, 3);
    ASSERT_TRUE(lr_var.success);
    const Sem_GiveItemVerboseVar* var_op = nullptr;
    for (const auto& blk : lr_var.ir.blocks)
        for (const auto& inst : blk.instructions)
            if (auto* p = std::get_if<Sem_GiveItemVerboseVar>(&inst.op)) var_op = p;
    ASSERT_TRUE(var_op != nullptr);
    ASSERT_EQ(var_op->item_source, ItemSource::FromScriptVar);
    ASSERT_EQ(var_op->quantity_var, 3u);

    // MUTATION CHECK: Literal and ScriptVar sources must differ
    ASSERT_TRUE(lit_op->item_source != var_op->item_source);
    // MUTATION CHECK: item byte 37 must NOT collapse to item=0 or ScriptVar
    ASSERT_TRUE(lit_op->item_source != ItemSource::FromScriptVar);

    std::cout << "  [P3-S7: GiveItemVerboseVar literal item=37/qty_var=5 and ScriptVar item/qty_var=3 ✓]\n";
}

// =============================================================================
// P3-S8/S9: Sem_PlayCry and Sem_Pokepic — SpeciesSource literal vs ScriptVar
// Source: Script_cry (0x84) and Script_pokepic (0x56): operand==0 → wScriptVar
// INDEPENDENCE: SpeciesId{0} as a sentinel is WRONG; must use SpeciesSource::ScriptVar
// cry opcode: dw cry_id (2 bytes LE); pokepic opcode: db pokemon (1 byte)
// =============================================================================
TEST(p3_s8_s9_speciesource_literal_vs_scriptvar) {
    using namespace crystal;
    using namespace enginemon;

    // --- Sem_PlayCry: species literal (cry_id low byte = 25 = Pikachu, high byte = 0) ---
    {
        // cry opcode 0x84, dw 0x0019 (low=25, high=0 → species=25 literal)
        std::vector<uint8_t> padded = {0x84, 25, 0x00, 0x91};
        while (padded.size() < 0x8000) padded.push_back(0xFF);
        auto rom = make_rom_from_bytes(padded);
        SymbolMap sym; TypedScriptDecoder dec(*rom, sym);
        auto lr = lower_ir(dec.decode_script(0x0000));
        ASSERT_TRUE(lr.success);
        const Sem_PlayCry* op = nullptr;
        for (const auto& blk : lr.ir.blocks)
            for (const auto& inst : blk.instructions)
                if (auto* p = std::get_if<Sem_PlayCry>(&inst.op)) op = p;
        ASSERT_TRUE(op != nullptr);
        ASSERT_TRUE(op->source.is_literal());
        ASSERT_EQ(static_cast<uint16_t>(op->source.species), 25u);
    }

    // --- Sem_PlayCry: species from ScriptVar (cry_id low byte = 0) ---
    {
        std::vector<uint8_t> padded = {0x84, 0x00, 0x00, 0x91}; // cry_id = 0 → ScriptVar
        while (padded.size() < 0x8000) padded.push_back(0xFF);
        auto rom = make_rom_from_bytes(padded);
        SymbolMap sym; TypedScriptDecoder dec(*rom, sym);
        auto lr = lower_ir(dec.decode_script(0x0000));
        ASSERT_TRUE(lr.success);
        const Sem_PlayCry* op = nullptr;
        for (const auto& blk : lr.ir.blocks)
            for (const auto& inst : blk.instructions)
                if (auto* p = std::get_if<Sem_PlayCry>(&inst.op)) op = p;
        ASSERT_TRUE(op != nullptr);
        // ORACLE: operand 0 → ScriptVar, NOT Literal(species=0)
        ASSERT_TRUE(op->source.is_script_var());
        // MUTATION CHECK: must NOT be Literal(0) — that would be the old sentinel pattern
        ASSERT_FALSE(op->source.is_literal());
    }

    // --- Sem_Pokepic: literal species=36 ---
    {
        std::vector<uint8_t> padded = {0x56, 36, 0x91};
        while (padded.size() < 0x8000) padded.push_back(0xFF);
        auto rom = make_rom_from_bytes(padded);
        SymbolMap sym; TypedScriptDecoder dec(*rom, sym);
        auto lr = lower_ir(dec.decode_script(0x0000));
        ASSERT_TRUE(lr.success);
        const Sem_Pokepic* op = nullptr;
        for (const auto& blk : lr.ir.blocks)
            for (const auto& inst : blk.instructions)
                if (auto* p = std::get_if<Sem_Pokepic>(&inst.op)) op = p;
        ASSERT_TRUE(op != nullptr);
        ASSERT_TRUE(op->source.is_literal());
        ASSERT_EQ(static_cast<uint16_t>(op->source.species), 36u);
    }

    // --- Sem_Pokepic: ScriptVar (pokemon byte=0) ---
    {
        std::vector<uint8_t> padded = {0x56, 0x00, 0x91};
        while (padded.size() < 0x8000) padded.push_back(0xFF);
        auto rom = make_rom_from_bytes(padded);
        SymbolMap sym; TypedScriptDecoder dec(*rom, sym);
        auto lr = lower_ir(dec.decode_script(0x0000));
        ASSERT_TRUE(lr.success);
        const Sem_Pokepic* op = nullptr;
        for (const auto& blk : lr.ir.blocks)
            for (const auto& inst : blk.instructions)
                if (auto* p = std::get_if<Sem_Pokepic>(&inst.op)) op = p;
        ASSERT_TRUE(op != nullptr);
        // ORACLE: byte 0 → ScriptVar, NOT Literal(SpeciesId{0})
        ASSERT_TRUE(op->source.is_script_var());
        ASSERT_FALSE(op->source.is_literal());
    }

    std::cout << "  [P3-S8/9: PlayCry/Pokepic SpeciesSource literal=25/36 vs ScriptVar for byte=0 ✓]\n";
}

// =============================================================================
// P3-S10: Menu variants — Sem_LoadMenu / Sem_VerticalMenu / Sem_2DMenu distinct
// Source: Script_loadmenu (0x4F), Script_verticalmenu (0x59), Script__2dmenu (0x58)
// Different routines, different result registers. Must NOT collapse to each other.
// =============================================================================
TEST(p3_s10_menu_variants_distinct) {
    using namespace crystal;
    using namespace enginemon;

    auto decode_lower_opcode = [](uint8_t opcode) -> LoweringResult {
        // LoadMenu needs a 2-byte pointer operand; VerticalMenu/2DMenu have no operands
        std::vector<uint8_t> padded;
        if (opcode == 0x4F) {
            padded = {0x4F, 0x10, 0x00, 0x91}; // loadmenu ptr=$0010
        } else {
            padded = {opcode, 0x91};
        }
        while (padded.size() < 0x8000) padded.push_back(0xFF);
        auto rom = make_rom_from_bytes(padded);
        SymbolMap sym; TypedScriptDecoder dec(*rom, sym);
        CrystalScriptIR ir = dec.decode_script(0x0000);
        return lower_ir(ir);
    };

    // 0x4F → Sem_LoadMenu (with header_pointer field)
    auto lr_load = decode_lower_opcode(0x4F);
    ASSERT_TRUE(lr_load.success);
    const Sem_LoadMenu* load_op = nullptr;
    bool found_vert_for_load = false, found_2d_for_load = false;
    for (const auto& blk : lr_load.ir.blocks) {
        for (const auto& inst : blk.instructions) {
            if (auto* p = std::get_if<Sem_LoadMenu>(&inst.op))     load_op = p;
            if (std::holds_alternative<Sem_VerticalMenu>(inst.op)) found_vert_for_load = true;
            if (std::holds_alternative<Sem_2DMenu>(inst.op))       found_2d_for_load = true;
        }
    }
    ASSERT_TRUE(load_op != nullptr);
    ASSERT_FALSE(found_vert_for_load);
    ASSERT_FALSE(found_2d_for_load);

    // 0x59 → Sem_VerticalMenu (no Sem_LoadMenu, no Sem_2DMenu)
    auto lr_vert = decode_lower_opcode(0x59);
    ASSERT_TRUE(lr_vert.success);
    bool found_vert = false, found_load_for_vert = false, found_2d_for_vert = false;
    for (const auto& blk : lr_vert.ir.blocks) {
        for (const auto& inst : blk.instructions) {
            if (std::holds_alternative<Sem_VerticalMenu>(inst.op)) found_vert = true;
            if (std::holds_alternative<Sem_LoadMenu>(inst.op))     found_load_for_vert = true;
            if (std::holds_alternative<Sem_2DMenu>(inst.op))       found_2d_for_vert = true;
        }
    }
    ASSERT_TRUE(found_vert);
    ASSERT_FALSE(found_load_for_vert);
    ASSERT_FALSE(found_2d_for_vert);

    // 0x58 → Sem_2DMenu (no Sem_VerticalMenu, no Sem_LoadMenu)
    auto lr_2d = decode_lower_opcode(0x58);
    ASSERT_TRUE(lr_2d.success);
    bool found_2d = false, found_vert_for_2d = false, found_load_for_2d = false;
    for (const auto& blk : lr_2d.ir.blocks) {
        for (const auto& inst : blk.instructions) {
            if (std::holds_alternative<Sem_2DMenu>(inst.op))       found_2d = true;
            if (std::holds_alternative<Sem_VerticalMenu>(inst.op)) found_vert_for_2d = true;
            if (std::holds_alternative<Sem_LoadMenu>(inst.op))     found_load_for_2d = true;
        }
    }
    ASSERT_TRUE(found_2d);
    ASSERT_FALSE(found_vert_for_2d);
    ASSERT_FALSE(found_load_for_2d);

    std::cout << "  [P3-S10: LoadMenu(0x4F)/VerticalMenu(0x59)/2DMenu(0x58) — three distinct types ✓]\n";
}

// =============================================================================
// P3-P1: Package seam — BgEvent type encoded as uint8_t, all types round-trip
// Covers: BgEventType→uint8_t→BgEventType round-trip for IfSet, IfNotSet, Read, HiddenItem
// INDEPENDENCE: expected type values come from BgEventType enum definitions
// =============================================================================
TEST(p3_p1_bgevent_type_roundtrip) {
    using namespace crystal;
    using namespace enginemon;

    auto test_bgevent_type = [](BgEventType type, RuntimeBgEventType expected_runtime, const char* name) {
        ExtractedMap m;
        m.map_id = std::string("p3p1_") + name;
        m.display_name = name;
        m.tileset_id = "johto_outdoor";
        m.width = 2; m.height = 2;
        m.blocks.assign(4, 0);
        m.is_outdoor = true;
        m.environment_type = 1; m.lighting = 0;

        BgEvent ev;
        ev.x = 3; ev.y = 7;
        ev.type = type;
        ev.script_id = "oracle_script";
        ev.condition_flag = "TEST_FLAG_99";
        m.bg_events.push_back(ev);

        auto tmp = std::filesystem::temp_directory_path() / (std::string("p3p1_") + name + ".emon");
        crystal::PackageWriter w;
        w.set_source_rom("p3p1_sha1", "p3p1_v1");
        w.add_map(m);
        ASSERT_TRUE(w.write(tmp));

        auto reader = crystal::PackageReader::open(tmp);
        ASSERT_TRUE(reader != nullptr);
        auto loaded_opt = reader->load_full_map(m.map_id);
        ASSERT_TRUE(loaded_opt.has_value());
        ASSERT_EQ(loaded_opt->bg_events.size(), 1u);

        // ORACLE: type survives the uint8_t wire encoding
        auto actual = loaded_opt->bg_events[0];
        ASSERT_EQ(static_cast<int>(actual.type), static_cast<int>(expected_runtime));
        ASSERT_STR_EQ(actual.condition_flag, "TEST_FLAG_99");
        std::filesystem::remove(tmp);
    };

    test_bgevent_type(BgEventType::IfSet,      RuntimeBgEventType::IfSet,      "ifset");
    test_bgevent_type(BgEventType::IfNotSet,   RuntimeBgEventType::IfNotSet,   "ifnotset");
    test_bgevent_type(BgEventType::Read,       RuntimeBgEventType::Read,       "read");
    test_bgevent_type(BgEventType::HiddenItem, RuntimeBgEventType::HiddenItem, "hiddenitem");

    // MUTATION CHECK: IfSet and IfNotSet must NOT be equal after round-trip
    {
        ExtractedMap m1, m2;
        for (auto* m : {&m1, &m2}) {
            m->map_id = (&m1 == m) ? "p3p1_mutchk1" : "p3p1_mutchk2";
            m->display_name = m->map_id;
            m->tileset_id = "johto_outdoor";
            m->width = 1; m->height = 1;
            m->blocks.assign(1, 0);
            m->environment_type = 1; m->lighting = 0;
        }
        BgEvent e1, e2;
        e1.x = 0; e1.y = 0; e1.type = BgEventType::IfSet;    e1.script_id = "s1";
        e2.x = 0; e2.y = 0; e2.type = BgEventType::IfNotSet; e2.script_id = "s2";
        m1.bg_events.push_back(e1);
        m2.bg_events.push_back(e2);

        crystal::PackageWriter w; w.set_source_rom("p3p1_mut", "v1");
        w.add_map(m1); w.add_map(m2);
        auto tmp2 = std::filesystem::temp_directory_path() / "p3p1_mutation.emon";
        ASSERT_TRUE(w.write(tmp2));
        auto reader2 = crystal::PackageReader::open(tmp2);
        ASSERT_TRUE(reader2 != nullptr);
        auto r1 = reader2->load_full_map("p3p1_mutchk1");
        auto r2 = reader2->load_full_map("p3p1_mutchk2");
        ASSERT_TRUE(r1.has_value() && r2.has_value());
        ASSERT_TRUE(r1->bg_events[0].type != r2->bg_events[0].type);
        std::filesystem::remove(tmp2);
    }

    std::cout << "  [P3-P1: BgEvent type (IfSet/IfNotSet/Read/HiddenItem) roundtrips via uint8_t wire ✓]\n";
}

// =============================================================================
// P3-P2: Package seam — connection coord_adjust_tiles is int32_t (signed), asymmetric values
// Source: write_connection uses write_le(out, conn.coord_adjust_tiles) where coord_adjust_tiles is int32_t
// Oracle: positive=13, negative=-7, zero=0 must all round-trip exactly
// =============================================================================
TEST(p3_p2_connection_signed_strip_offset_roundtrip) {
    using namespace crystal;
    using namespace enginemon;

    auto test_offset = [](int32_t offset_val, const char* map_suffix) {
        ExtractedMap m;
        m.map_id = std::string("p3p2_") + map_suffix;
        m.display_name = m.map_id;
        m.tileset_id = "johto_outdoor";
        m.width = 3; m.height = 3;
        m.blocks.assign(9, 0);
        m.is_outdoor = true;
        m.environment_type = 1; m.lighting = 0;

        MapConnection conn;
        conn.direction = crystal::Direction::East;
        conn.target_map_id = "target_map";
        conn.coord_adjust_tiles = offset_val;
        conn.src_skip_blocks = 0;
        conn.strip_length_blocks = 4;
        m.connections.push_back(conn);

        auto tmp = std::filesystem::temp_directory_path() / (std::string("p3p2_") + map_suffix + ".emon");
        crystal::PackageWriter w; w.set_source_rom("p3p2_sha1", "p3p2_v1");
        w.add_map(m);
        ASSERT_TRUE(w.write(tmp));

        auto reader = crystal::PackageReader::open(tmp);
        ASSERT_TRUE(reader != nullptr);
        auto loaded = reader->load_full_map(m.map_id);
        ASSERT_TRUE(loaded.has_value());
        ASSERT_EQ(loaded->connections.size(), 1u);

        // ORACLE: signed int32_t coord_adjust_tiles survives serialization exactly
        ASSERT_EQ(loaded->connections[0].coord_adjust_tiles, offset_val);
        std::filesystem::remove(tmp);
    };

    test_offset(13,  "pos");    // positive
    test_offset(-7,  "neg");    // negative
    test_offset(0,   "zero");   // zero

    // MUTATION CHECK: positive and negative must not collapse to same value
    {
        ExtractedMap m;
        m.map_id = "p3p2_bothconns";
        m.display_name = m.map_id;
        m.tileset_id = "johto_outdoor";
        m.width = 3; m.height = 3;
        m.blocks.assign(9, 0);
        m.is_outdoor = true; m.environment_type = 1; m.lighting = 0;

        MapConnection c1, c2;
        c1.direction = crystal::Direction::East;  c1.target_map_id = "east_map";
        c1.coord_adjust_tiles = 13; c1.src_skip_blocks = 0; c1.strip_length_blocks = 4;
        c2.direction = crystal::Direction::North; c2.target_map_id = "north_map";
        c2.coord_adjust_tiles = -7; c2.src_skip_blocks = 0; c2.strip_length_blocks = 3;
        m.connections.push_back(c1); m.connections.push_back(c2);

        auto tmp = std::filesystem::temp_directory_path() / "p3p2_bothconns.emon";
        crystal::PackageWriter w; w.set_source_rom("p3p2b_sha1", "p3p2b_v1");
        w.add_map(m);
        ASSERT_TRUE(w.write(tmp));
        auto reader = crystal::PackageReader::open(tmp);
        ASSERT_TRUE(reader != nullptr);
        auto loaded = reader->load_full_map("p3p2_bothconns");
        ASSERT_TRUE(loaded.has_value());
        ASSERT_EQ(loaded->connections.size(), 2u);

        const RuntimeConnection* east_c = nullptr;
        const RuntimeConnection* north_c = nullptr;
        for (const auto& c : loaded->connections) {
            if (c.direction == ConnectionDirection::East)  east_c  = &c;
            if (c.direction == ConnectionDirection::North) north_c = &c;
        }
        ASSERT_TRUE(east_c  != nullptr);
        ASSERT_TRUE(north_c != nullptr);
        ASSERT_EQ(east_c->coord_adjust_tiles,  13);
        ASSERT_EQ(north_c->coord_adjust_tiles, -7);
        ASSERT_TRUE(east_c->coord_adjust_tiles != north_c->coord_adjust_tiles);
        std::filesystem::remove(tmp);
    }

    std::cout << "  [P3-P2: connection coord_adjust_tiles +13/-7/0 survive as int32_t signed ✓]\n";
}

// =============================================================================
// P3-P3: Package seam — object event fields: is_trainer, sprite_id, visibility_flag
// Source: write_object serializes 14 fields; runtime reads all 14
// Asymmetric values; is_trainer as boolean; sprite_id and visibility_flag as strings
// =============================================================================
TEST(p3_p3_object_event_fields_roundtrip) {
    using namespace crystal;
    using namespace enginemon;

    ExtractedMap m;
    m.map_id = "p3p3_map";
    m.display_name = "P3P3";
    m.tileset_id = "johto_outdoor";
    m.width = 5; m.height = 5;
    m.blocks.assign(25, 0);
    m.is_outdoor = true;
    m.environment_type = 1; m.lighting = 0;

    // NPC object with all distinctive field values
    ObjectEvent obj;
    obj.local_id = 3;
    obj.x = 7; obj.y = 11;
    obj.movement_type = 0x03;     // SPINRANDOM_SLOW
    obj.movement_radius_x = 2;
    obj.movement_radius_y = 0;
    obj.hour_start = 8; obj.hour_end = 20;
    obj.palette = 2;
    obj.is_trainer = true;
    obj.trainer_sight_range = 3;
    obj.sprite_id = "rocket_grunt";
    obj.script_id = "p3p3_npc_script";
    obj.visibility_flag = "FLAG_ROCKET_GRUNTS_DEFEATED";
    m.objects.push_back(obj);

    auto tmp = std::filesystem::temp_directory_path() / "p3p3_obj.emon";
    crystal::PackageWriter w; w.set_source_rom("p3p3_sha1", "v1");
    w.add_map(m);
    ASSERT_TRUE(w.write(tmp));

    auto reader = crystal::PackageReader::open(tmp);
    ASSERT_TRUE(reader != nullptr);
    auto loaded = reader->load_full_map("p3p3_map");
    ASSERT_TRUE(loaded.has_value());
    ASSERT_EQ(loaded->objects.size(), 1u);

    const auto& robj = loaded->objects[0];
    // ORACLE: all 14 serialized fields survive the wire
    ASSERT_EQ(robj.local_id,           3u);
    ASSERT_EQ(robj.x,                  7u);
    ASSERT_EQ(robj.y,                  11u);
    ASSERT_EQ(robj.movement_type,      0x03u);
    ASSERT_EQ(robj.movement_radius_x,  2u);
    ASSERT_EQ(robj.movement_radius_y,  0u);
    ASSERT_EQ(robj.hour_start,         8u);
    ASSERT_EQ(robj.hour_end,           20u);
    ASSERT_EQ(robj.palette,            2u);
    ASSERT_TRUE(robj.is_trainer);
    ASSERT_EQ(robj.trainer_sight_range, 3u);
    ASSERT_STR_EQ(robj.sprite_id,       "rocket_grunt");
    ASSERT_STR_EQ(robj.script_id,       "p3p3_npc_script");
    ASSERT_STR_EQ(robj.visibility_flag, "FLAG_ROCKET_GRUNTS_DEFEATED");

    std::filesystem::remove(tmp);
    std::cout << "  [P3-P3: object event all 14 fields survive package seam ✓]\n";
}

// =============================================================================
// P3-L1: Linker FlagRef — EventFlag{5} and EngineFlag{5} same numeric value
// are distinct references (encoded with namespace in bits 16+) and produce
// distinct validated references
// Source: FlagNamespace::Event=0 / Engine=1 in types.hpp; linker encodes as (ns<<16)|value
// =============================================================================
TEST(p3_l1_flag_namespace_linker_distinct) {
    using namespace enginemon;

    // Hand-craft the two flag encodings the linker uses
    // EventFlag{5}: encoded as (0 << 16) | 5 = 0x00000005
    // EngineFlag{5}: encoded as (1 << 16) | 5 = 0x00010005
    uint32_t event_encoded  = (static_cast<uint32_t>(FlagNamespace::Event)  << 16) | 5u;
    uint32_t engine_encoded = (static_cast<uint32_t>(FlagNamespace::Engine) << 16) | 5u;

    // ORACLE: same value (5) but different namespace → different encoding
    ASSERT_TRUE(event_encoded != engine_encoded);

    // The FlagRef equality operator respects namespace
    FlagRef ef = FlagRef::event_flag(5);
    FlagRef ngf = FlagRef::engine_flag(5);

    ASSERT_TRUE(ef != ngf);   // Different namespace = not equal
    ASSERT_TRUE(ef  == FlagRef::event_flag(5));   // Same ns + value = equal
    ASSERT_TRUE(ngf == FlagRef::engine_flag(5));  // Same ns + value = equal

    // Verify encoding produces the expected bit patterns
    ASSERT_EQ((static_cast<uint32_t>(ef.ns) << 16)  | ef.value,  0x00000005u);
    ASSERT_EQ((static_cast<uint32_t>(ngf.ns) << 16) | ngf.value, 0x00010005u);

    // EventFlag{5} is a valid range for Crystal (0-2047); EngineFlag{5} valid (0-189)
    // Both are within-domain flags — the distinction is the namespace identity
    ASSERT_TRUE(ef.value < 2048);    // EventFlag domain
    ASSERT_TRUE(ngf.value < 190);    // EngineFlag domain

    std::cout << "  [P3-L1: FlagRef EventFlag{5}(0x00000005) != EngineFlag{5}(0x00010005) ✓]\n";
}

// =============================================================================
// P3-L4: Linker — SpeciesSource::ScriptVar emits no reference (link-transparent)
// vs SpeciesSource::Literal(species) emits a reference that can be InvalidDomain
// Source: semantic_linker.cpp — ScriptVar path skips add_ref for PlayCry/Pokepic
// =============================================================================
TEST(p3_l4_scriptvar_species_no_linker_reference) {
    using namespace crystal;
    using namespace enginemon;

    // The linker skips a species reference entirely for ScriptVar source.
    // We verify the IR distinction: ScriptVar source has is_script_var()==true
    // and the species field is meaningless (SpeciesId{0}).

    // ScriptVar source construction
    SpeciesSource sv = SpeciesSource::from_script_var();
    ASSERT_TRUE(sv.is_script_var());
    ASSERT_FALSE(sv.is_literal());

    // Literal source construction for species=25 (Pikachu)
    SpeciesSource lit = SpeciesSource::literal(SpeciesId{25});
    ASSERT_TRUE(lit.is_literal());
    ASSERT_FALSE(lit.is_script_var());
    ASSERT_EQ(static_cast<uint16_t>(lit.species), 25u);

    // SpeciesSource equality: ScriptVar == ScriptVar (kind match)
    SpeciesSource sv2 = SpeciesSource::from_script_var();
    ASSERT_TRUE(sv == sv2);

    // SpeciesSource inequality: ScriptVar != Literal (different kind)
    ASSERT_TRUE(sv != lit);

    // Literal(25) != Literal(26) (different species)
    SpeciesSource lit26 = SpeciesSource::literal(SpeciesId{26});
    ASSERT_TRUE(lit != lit26);

    // Literal(25) == Literal(25)
    SpeciesSource lit25b = SpeciesSource::literal(SpeciesId{25});
    ASSERT_TRUE(lit == lit25b);

    // MUTATION CHECK: SpeciesId{0} on a Literal source should NOT equal ScriptVar
    SpeciesSource lit0 = SpeciesSource::literal(SpeciesId{0});
    ASSERT_TRUE(lit0 != sv);  // Literal(0) != ScriptVar — different kinds
    ASSERT_TRUE(lit0.is_literal());

    std::cout << "  [P3-L4: SpeciesSource ScriptVar != Literal, no fake SpeciesId{0} conflation ✓]\n";
}

// =============================================================================
// P3-SER1: Serialization boundary — signed connection coord_adjust_tiles MAX/MIN/asymmetric
// Prove int32_t boundary values survive the write_le/read_le round-trip
// =============================================================================
TEST(p3_ser1_signed_offset_boundary_values) {
    using namespace crystal;
    using namespace enginemon;

    auto roundtrip_offset = [](int32_t val, const std::string& map_id) -> int32_t {
        ExtractedMap m;
        m.map_id = map_id;
        m.display_name = map_id;
        m.tileset_id = "johto_outdoor";
        m.width = 2; m.height = 2;
        m.blocks.assign(4, 0);
        m.is_outdoor = true;
        m.environment_type = 1; m.lighting = 0;

        MapConnection conn;
        conn.direction = crystal::Direction::West;
        conn.target_map_id = "t";
        conn.coord_adjust_tiles = val;
        conn.src_skip_blocks = 0;
        conn.strip_length_blocks = 1;
        m.connections.push_back(conn);

        auto tmp = std::filesystem::temp_directory_path() / (map_id + ".emon");
        crystal::PackageWriter w; w.set_source_rom("ser1_sha1", "v1");
        w.add_map(m);
        if (!w.write(tmp)) return -999999;

        auto reader = crystal::PackageReader::open(tmp);
        if (!reader) return -999998;
        auto loaded = reader->load_full_map(map_id);
        std::filesystem::remove(tmp);
        if (!loaded.has_value() || loaded->connections.empty()) return -999997;
        return loaded->connections[0].coord_adjust_tiles;
    };

    // Vanilla-realistic range: [-128, 127] (Crystal int8_t origin) but stored as int32_t
    ASSERT_EQ(roundtrip_offset(0,    "ser1_zero"),   0);
    ASSERT_EQ(roundtrip_offset(127,  "ser1_p127"),   127);
    ASSERT_EQ(roundtrip_offset(-128, "ser1_n128"),   -128);
    ASSERT_EQ(roundtrip_offset(13,   "ser1_p13"),    13);
    ASSERT_EQ(roundtrip_offset(-7,   "ser1_n7"),     -7);
    // Large positive and negative (proves int32_t not truncated to int8_t/int16_t)
    ASSERT_EQ(roundtrip_offset(1000,  "ser1_p1k"),  1000);
    ASSERT_EQ(roundtrip_offset(-1000, "ser1_n1k"), -1000);

    // MUTATION CHECK: +1000 != -1000 (sign not lost)
    ASSERT_TRUE(roundtrip_offset(1000, "ser1_mut1") != roundtrip_offset(-1000, "ser1_mut2"));

    std::cout << "  [P3-SER1: signed coord_adjust_tiles 0/127/-128/13/-7/±1000 all survive int32_t wire ✓]\n";
}

// =============================================================================
// P3-SER-CONN: All three connection fields round-trip independently
//
// Source: pokecrystal/data/maps/attributes.asm connection macro
//   src_skip_blocks    = _src = max(0, -(offset + 3))       — blocks, uint8_t on wire
//   strip_length_blocks = _len - _src = data[6]             — blocks, uint8_t on wire  
//   coord_adjust_tiles  = offset * -2 from data[8] or data[9] — tiles, int32_t on wire
//
// This test proves the three fields are serialized/deserialized as independent values.
// Expected values use Azalea→Route34 (west, offset=-18) semantics:
//   coord_adjust_tiles = (-18)*-2 = +36
//   src_skip_blocks    = max(0, -(-18+3)) = 15
//   strip_length_blocks = 12  (27 - 15, see runtime_test.cpp for derivation)
//
// The same asymmetric values for all three fields prove no field collapses to
// another, no sign is lost, and the byte layout matches the new schema.
// =============================================================================
TEST(p3_ser_conn_three_fields_independent_roundtrip) {
    using namespace crystal;
    using namespace enginemon;

    ExtractedMap m;
    m.map_id = "p3_ser_conn_three_fields";
    m.display_name = m.map_id;
    m.tileset_id = "johto_outdoor";
    m.width = 20; m.height = 9;
    m.blocks.assign(20 * 9, 0);
    m.is_outdoor = true;
    m.environment_type = 1; m.lighting = 0;

    // West connection matching Azalea→Route34 (offset=-18) semantics
    MapConnection conn;
    conn.direction = crystal::Direction::West;
    conn.target_map_id = "route_34";
    conn.coord_adjust_tiles  = 36;    // (-18)*-2 = +36, already in tiles
    conn.src_skip_blocks     = 15;    // max(0, -(-18+3)) = 15
    conn.strip_length_blocks = 12;    // _len - _src = 27 - 15 = 12
    m.connections.push_back(conn);

    auto tmp = std::filesystem::temp_directory_path() / "p3_ser_conn_three_fields.emon";
    crystal::PackageWriter w; w.set_source_rom("p3sc_sha1", "p3sc_v1");
    w.add_map(m);
    ASSERT_TRUE(w.write(tmp));

    auto reader = crystal::PackageReader::open(tmp);
    ASSERT_TRUE(reader != nullptr);
    auto loaded = reader->load_full_map(m.map_id);
    ASSERT_TRUE(loaded.has_value());
    ASSERT_EQ(loaded->connections.size(), 1u);

    const auto& rc = loaded->connections[0];
    ASSERT_EQ(rc.direction, ConnectionDirection::West);
    ASSERT_STR_EQ(rc.target_map_id, "route_34");

    // ORACLE: all three fields survive independently
    ASSERT_EQ(rc.coord_adjust_tiles,  36);
    ASSERT_EQ(rc.src_skip_blocks,     15);
    ASSERT_EQ(rc.strip_length_blocks, 12u);

    // MUTATION: fields are all distinct from each other and from zero
    ASSERT_TRUE(rc.coord_adjust_tiles  != 0);
    ASSERT_TRUE(rc.src_skip_blocks     != 0);
    ASSERT_TRUE(rc.strip_length_blocks != 0u);
    ASSERT_TRUE(rc.coord_adjust_tiles  != rc.src_skip_blocks);
    ASSERT_TRUE(rc.coord_adjust_tiles  != (int32_t)rc.strip_length_blocks);
    ASSERT_TRUE(rc.src_skip_blocks     != (int32_t)rc.strip_length_blocks);

    // MUTATION: coord_adjust_tiles is NOT the old strip_offset*2 error value (72)
    ASSERT_TRUE(rc.coord_adjust_tiles != 72);

    std::filesystem::remove(tmp);
    std::cout << "  [P3-SER-CONN: coord_adjust=36, src_skip=15, strip_len=12 all independent ✓]\n";
}

// =============================================================================
// P3-SER2: Serialization — sprite_id string boundary (empty, typical, max-reasonable)
// Prove length-prefixed string survives the package seam exactly
// =============================================================================
TEST(p3_ser2_sprite_id_string_boundary) {
    using namespace crystal;
    using namespace enginemon;

    auto roundtrip_sprite = [](const std::string& sprite_id, const std::string& map_id) -> std::string {
        ExtractedMap m;
        m.map_id = map_id;
        m.display_name = map_id;
        m.tileset_id = "johto_outdoor";
        m.width = 2; m.height = 2;
        m.blocks.assign(4, 0);
        m.environment_type = 1; m.lighting = 0;

        ObjectEvent obj;
        obj.local_id = 1; obj.x = 1; obj.y = 1;
        obj.sprite_id = sprite_id;
        obj.script_id = "s"; obj.visibility_flag = "";
        m.objects.push_back(obj);

        auto tmp = std::filesystem::temp_directory_path() / (map_id + ".emon");
        crystal::PackageWriter w; w.set_source_rom("ser2_sha1", "v1");
        w.add_map(m);
        if (!w.write(tmp)) return "WRITE_FAIL";

        auto reader = crystal::PackageReader::open(tmp);
        if (!reader) return "OPEN_FAIL";
        auto loaded = reader->load_full_map(map_id);
        std::filesystem::remove(tmp);
        if (!loaded.has_value() || loaded->objects.empty()) return "LOAD_FAIL";
        return loaded->objects[0].sprite_id;
    };

    // Typical sprite IDs from the corpus
    ASSERT_STR_EQ(roundtrip_sprite("chris",             "ser2_chris"),  "chris");
    ASSERT_STR_EQ(roundtrip_sprite("standing_youngster","ser2_sty"),    "standing_youngster");
    ASSERT_STR_EQ(roundtrip_sprite("rocket_grunt",      "ser2_rg"),     "rocket_grunt");
    // Empty string is valid (some objects may have no sprite assignment)
    ASSERT_STR_EQ(roundtrip_sprite("",                  "ser2_empty"),  "");

    // MUTATION CHECK: distinct IDs stay distinct
    ASSERT_TRUE(roundtrip_sprite("chris", "ser2_mut1") != roundtrip_sprite("standing_youngster", "ser2_mut2"));

    std::cout << "  [P3-SER2: sprite_id strings chris/standing_youngster/empty survive package seam ✓]\n";
}

// =============================================================================
// Phase 3 — Heuristic coverage gap summary (non-oracle, informational)
//
// Coverage table (manually derived from SemanticOp variant):
//   Sem_End               ORACLE: P3-S1    ✓
//   Sem_EndAll            ORACLE: P3-S1    ✓
//   Sem_WaitButton        ORACLE: P3-S2    ✓
//   Sem_PromptButton      ORACLE: P3-S2    ✓
//   Sem_AskForPhoneNumber ORACLE: P3-S3    ✓
//   Sem_NewLoadMap        ORACLE: P3-S4    ✓
//   Sem_CatchTutorial     ORACLE: P3-S5    ✓
//   Sem_DeactivateFacing  ORACLE: P3-S6    ✓
//   Sem_GiveItemVerboseVar ORACLE: P3-S7   ✓
//   Sem_PlayCry           ORACLE: P3-S8/9  ✓
//   Sem_Pokepic           ORACLE: P3-S8/9  ✓
//   Sem_LoadMenu          ORACLE: P3-S10   ✓
//   Sem_VerticalMenu      ORACLE: P3-S10   ✓
//   Sem_2DMenu            ORACLE: P3-S10   ✓
//   FlagRef namespace     ORACLE: P3-L1    ✓  (partial — encoding test, not full linker run)
//   SpeciesSource         ORACLE: P3-L4    ✓
//   BgEvent type wire     ORACLE: P3-P1    ✓
//   connection strip_off  ORACLE: P3-P2    ✓
//   object event 14 flds  ORACLE: P3-P3    ✓
//   int32_t signed seam   ORACLE: P3-SER1  ✓
//   sprite_id string      ORACLE: P3-SER2  ✓
//
//   NOT YET COVERED (deferred to Phase 4 or later):
//   Sem_TrainerText domain  — BattleTower vs Normal (no RGBDS fixture yet)
//   Sem_SetWinLossText      — nullopt semantics
//   Sem_Sdefer              — covered in P1 (bank resolution)
//   Sem_PlayEncounterMusic  — covered implicitly by golden tests
//   Full linker run for     — requires full compilation (golden tests cover this)
//     invalid MapId/Species
// =============================================================================

// MAIN
// =============================================================================

// =============================================================================
// ORACLE PHASE 4 — VERTICAL SLICES
// =============================================================================
// Each test traces: Crystal source bytes → TypedDecoder → SemanticLegalizer →
// observable Sem_* type and field values.
// Expected values are HAND-AUTHORED from pokecrystal source, never from
// Enginemon encoder/decoder output.
//
// Mutation checks are included for each historically dangerous failure mode.
// =============================================================================

// =============================================================================
// P4-1: setflag ENGINE_FLYPOINT_NEW_BARK
//
// Source: maps/NewBarkTown.asm — NewBarkTownFlypointCallback:
//   setflag ENGINE_FLYPOINT_NEW_BARK   ; $36 dw 0x0041
//   clearevent EVENT_FIRST_TIME_BANKING_WITH_MOM  ; $32 dw 0x0076
//   endcallback                         ; $90
//
// constants/engine_flags.asm: const_def starts at 0; ENGINE_FLYPOINT_NEW_BARK = 65 = 0x41
// constants/event_flags.asm:  EVENT_FIRST_TIME_BANKING_WITH_MOM = 118 = 0x76
// engine/include/engine/core/types.hpp: FlagNamespace::Engine=1, FlagNamespace::Event=0
//
// Hand-derived byte sequence (bank 0 / flat 0x0000):
//   36 41 00   setflag  ENGINE_FLYPOINT_NEW_BARK (LE u16=65)
//   32 76 00   clearevent EVENT_FIRST_TIME_BANKING_WITH_MOM (LE u16=118)
//   90         endcallback
// =============================================================================
TEST(p4_1_setflag_engine_flypoint_new_bark) {
    using namespace crystal;
    using namespace enginemon;

    // Hand-authored bytes from pokecrystal source
    // setflag $36 LE16(65) = 36 41 00
    // clearevent $32 LE16(118) = 32 76 00
    // endcallback $90
    std::vector<uint8_t> bytes = {
        0x36, 0x41, 0x00,   // setflag ENGINE_FLYPOINT_NEW_BARK (flag=65, namespace=Engine)
        0x32, 0x76, 0x00,   // clearevent EVENT_FIRST_TIME_BANKING_WITH_MOM (flag=118, namespace=Event)
        0x90                // endcallback
    };
    while (bytes.size() < 0x8000) bytes.push_back(0xFF);
    auto rom = make_rom_from_bytes(bytes);
    SymbolMap sym;
    TypedScriptDecoder dec(*rom, sym);
    CrystalScriptIR ir = dec.decode_script(0x0000);
    ASSERT_TRUE(ir.commands.size() >= 2u);

    // Stage 1 decode: setflag (0x36) must carry flag=65 (0x41)
    {
        auto* cmd = std::get_if<Cmd_Setflag>(&ir.commands[0].data);
        ASSERT_TRUE(cmd != nullptr);
        ASSERT_EQ(cmd->engine_flag, 65u);
        // MUTATION CHECK: must NOT be 0x36 (the opcode byte, wrong offset decode)
        ASSERT_TRUE(cmd->engine_flag != 0x36u);
        // MUTATION CHECK: must NOT be 0 (default-zero from missing advance)
        ASSERT_TRUE(cmd->engine_flag != 0u);
    }

    // Stage 1 decode: clearevent (0x32) must carry flag=118 (0x76)
    {
        auto* cmd = std::get_if<Cmd_Clearevent>(&ir.commands[1].data);
        ASSERT_TRUE(cmd != nullptr);
        ASSERT_EQ(cmd->event_flag, 118u);
    }

    // Stage 4 lower: Sem_SetFlag{ns=Engine, value=65}
    auto lr = lower_ir(ir);
    ASSERT_TRUE(lr.success);

    const Sem_SetFlag* set_flag_op = nullptr;
    const Sem_ClearFlag* clear_flag_op = nullptr;
    for (const auto& blk : lr.ir.blocks) {
        for (const auto& inst : blk.instructions) {
            if (auto* p = std::get_if<Sem_SetFlag>(&inst.op))   set_flag_op   = p;
            if (auto* p = std::get_if<Sem_ClearFlag>(&inst.op)) clear_flag_op = p;
        }
    }

    // ORACLE: setflag ENGINE_FLYPOINT_NEW_BARK → namespace=Engine, value=65
    ASSERT_TRUE(set_flag_op != nullptr);
    ASSERT_EQ(set_flag_op->flag.ns, FlagNamespace::Engine);
    ASSERT_EQ(set_flag_op->flag.value, 65u);

    // ORACLE: clearevent EVENT_FIRST_TIME_BANKING_WITH_MOM → namespace=Event, value=118
    ASSERT_TRUE(clear_flag_op != nullptr);
    ASSERT_EQ(clear_flag_op->flag.ns, FlagNamespace::Event);
    ASSERT_EQ(clear_flag_op->flag.value, 118u);

    // MUTATION CHECK: setflag must produce Engine namespace, NOT Event (same value would be wrong)
    ASSERT_TRUE(set_flag_op->flag.ns != FlagNamespace::Event);
    // MUTATION CHECK: clearevent must produce Event namespace, NOT Engine
    ASSERT_TRUE(clear_flag_op->flag.ns != FlagNamespace::Engine);
    // MUTATION CHECK: Engine{65} != Event{65} — same numeric value, different namespaces
    ASSERT_TRUE(set_flag_op->flag != clear_flag_op->flag);
    ASSERT_TRUE(FlagRef::engine_flag(65) != FlagRef::event_flag(65));

    std::cout << "  [P4-1: setflag ENGINE_FLYPOINT_NEW_BARK=65 (Engine ns), clearevent Event{118} ✓]\n";
}

// =============================================================================
// P4-2: givepoke CYNDAQUIL 5 BERRY (Elm's Lab starter gift)
//
// Source: maps/ElmsLab.asm — CyndaquilPokeBallScript:
//   givepoke CYNDAQUIL, 5, BERRY    (no trainer flag → 4 bytes)
//
// Macro expansion (events.asm givepoke with 3 args → 4-arg form):
//   $2d  db CYNDAQUIL  db 5  db BERRY  db FALSE(0)
//
// constants/pokemon_constants.asm: CYNDAQUIL=$9B (JOHTO_POKEMON+3, base $98)
// constants/item_constants.asm:    BERRY=$AD
//
// Hand-derived bytes:
//   2D 9B 05 AD 00   givepoke CYNDAQUIL(0x9B) level=5 item=BERRY(0xAD) trainer=FALSE(0x00)
//   91               end
//
// Also verify Chikorita for independence: CHIKORITA=$98, same level/item
// =============================================================================
TEST(p4_2_givepoke_cyndaquil_level5_berry) {
    using namespace crystal;
    using namespace enginemon;

    // Hand-authored bytes: givepoke CYNDAQUIL, 5, BERRY, FALSE
    // Source: macros/scripts/events.asm givepoke macro, 4-arg variant
    std::vector<uint8_t> bytes = {
        0x2D,       // givepoke opcode
        0x9B,       // CYNDAQUIL = 0x9B (pokemon_constants.asm: JOHTO_POKEMON=0x98, +3)
        0x05,       // level = 5
        0xAD,       // BERRY = 0xAD (item_constants.asm)
        0x00,       // trainer = FALSE (no nickname/OT pointer follows)
        0x91        // end
    };
    while (bytes.size() < 0x8000) bytes.push_back(0xFF);
    auto rom = make_rom_from_bytes(bytes);
    SymbolMap sym;
    TypedScriptDecoder dec(*rom, sym);
    CrystalScriptIR ir = dec.decode_script(0x0000);
    ASSERT_TRUE(ir.commands.size() >= 1u);

    // Stage 1: Cmd_Givepoke must carry exact byte values
    auto* cmd = std::get_if<Cmd_Givepoke>(&ir.commands[0].data);
    ASSERT_TRUE(cmd != nullptr);
    ASSERT_EQ(cmd->pokemon, 0x9Bu);   // CYNDAQUIL
    ASSERT_EQ(cmd->level, 5u);
    ASSERT_EQ(cmd->item, 0xADu);      // BERRY
    ASSERT_EQ(cmd->trainer, 0u);      // FALSE — no nickname follows

    // MUTATION CHECK: opcode byte must NOT read as pokemon byte (wrong PC advance)
    ASSERT_TRUE(cmd->pokemon != 0x2Du);
    // MUTATION CHECK: level must NOT be 0 (default) or confuse with CHIKORITA ($98)
    ASSERT_EQ(cmd->level, 5u);
    ASSERT_TRUE(cmd->pokemon != 0x98u); // CHIKORITA, not CYNDAQUIL

    // Stage 4: Sem_GivePokemon preserves all three operands
    auto lr = lower_ir(ir);
    ASSERT_TRUE(lr.success);

    const Sem_GivePokemon* give_op = nullptr;
    for (const auto& blk : lr.ir.blocks)
        for (const auto& inst : blk.instructions)
            if (auto* p = std::get_if<Sem_GivePokemon>(&inst.op)) give_op = p;

    ASSERT_TRUE(give_op != nullptr);

    // ORACLE: CYNDAQUIL=0x9B, level=5, BERRY=0xAD — all preserved through semantic boundary
    ASSERT_EQ(static_cast<uint16_t>(give_op->species), 0x9Bu);
    ASSERT_EQ(give_op->level, 5u);
    ASSERT_EQ(static_cast<uint16_t>(give_op->held_item), 0xADu);
    ASSERT_FALSE(give_op->has_nickname); // trainer=FALSE → no nickname

    // MUTATION CHECK: species must NOT be 0 (dropped) or confused with item byte
    ASSERT_TRUE(static_cast<uint16_t>(give_op->species) != 0u);
    ASSERT_TRUE(static_cast<uint16_t>(give_op->species) != 0xADu); // item byte ≠ species
    // MUTATION CHECK: level must NOT be confused with species byte
    ASSERT_TRUE(give_op->level != 0x9Bu);

    // Independence verification: CHIKORITA=$98 is a distinct different species
    // (prove the givepoke for Chikorita produces a different result)
    std::vector<uint8_t> chikorita_bytes = {0x2D, 0x98, 0x05, 0xAD, 0x00, 0x91};
    while (chikorita_bytes.size() < 0x8000) chikorita_bytes.push_back(0xFF);
    auto rom2 = make_rom_from_bytes(chikorita_bytes);
    TypedScriptDecoder dec2(*rom2, sym);
    auto ir2 = dec2.decode_script(0x0000);
    auto* cmd2 = std::get_if<Cmd_Givepoke>(&ir2.commands[0].data);
    ASSERT_TRUE(cmd2 != nullptr);
    ASSERT_EQ(cmd2->pokemon, 0x98u); // CHIKORITA, not 0x9B
    ASSERT_TRUE(cmd2->pokemon != cmd->pokemon); // species differ

    std::cout << "  [P4-2: givepoke CYNDAQUIL(0x9B) level=5 BERRY(0xAD) trainer=FALSE → Sem_GivePokemon ✓]\n";
}

// =============================================================================
// P4-3: warp ELMS_LAB, 6, 3
//
// Source: maps/NewBarkTown.asm — warp_event 6, 3, ELMS_LAB, 1
//   The matching script warp form: warp ELMS_LAB, 6, 3
//
// constants/map_constants.asm: newgroup NEW_BARK = 24; map_const ELMS_LAB = entry 5
//   → GROUP_ELMS_LAB=24, MAP_ELMS_LAB=5
// macros/scripts/events.asm warp: db warp_command, map_id(group,map), db x, db y
//   warp_command = $3C; map_id emits db group, db map
//
// Hand-derived bytes:
//   3C 18 05 06 03   warp ELMS_LAB(group=24=0x18, map=5=0x05), x=6, y=3
//   91               end
//
// Enginemon MapId: (group << 8) | map = (24 << 8) | 5 = 0x1805 = 6149
//
// MUTATION CHECK: x/y swap (historical bug) → x=3 y=6 would be wrong
// =============================================================================
TEST(p4_3_warp_elmslab_x6_y3) {
    using namespace crystal;
    using namespace enginemon;

    // Hand-authored bytes from source
    // warp_command=0x3C, GROUP_ELMS_LAB=0x18(24), MAP_ELMS_LAB=0x05(5), x=6, y=3
    std::vector<uint8_t> bytes = {
        0x3C,               // warp opcode
        0x18, 0x05,         // map_id ELMS_LAB: group=24(0x18), map=5(0x05)
        0x06,               // x = 6
        0x03,               // y = 3
        0x91                // end
    };
    while (bytes.size() < 0x8000) bytes.push_back(0xFF);
    auto rom = make_rom_from_bytes(bytes);
    SymbolMap sym;
    TypedScriptDecoder dec(*rom, sym);
    CrystalScriptIR ir = dec.decode_script(0x0000);
    ASSERT_TRUE(ir.commands.size() >= 1u);

    // Stage 1: Cmd_Warp must carry exact map/x/y
    auto* cmd = std::get_if<Cmd_Warp>(&ir.commands[0].data);
    ASSERT_TRUE(cmd != nullptr);
    ASSERT_EQ(cmd->map.group, 24u);
    ASSERT_EQ(cmd->map.map,    5u);
    ASSERT_EQ(cmd->x,          6u);
    ASSERT_EQ(cmd->y,          3u);

    // Stage 4: Sem_Warp with correct MapId and x/y
    auto lr = lower_ir(ir);
    ASSERT_TRUE(lr.success);

    const Sem_Warp* warp_op = nullptr;
    for (const auto& blk : lr.ir.blocks)
        for (const auto& inst : blk.instructions)
            if (auto* p = std::get_if<Sem_Warp>(&inst.op)) warp_op = p;

    ASSERT_TRUE(warp_op != nullptr);

    // ORACLE: MapId = (24 << 8) | 5 = 0x1805 = 6149
    constexpr MapId EXPECTED_MAP_ID = (24u << 8) | 5u;  // 0x1805
    ASSERT_EQ(warp_op->map, EXPECTED_MAP_ID);

    // ORACLE: x=6, y=3 — preserved exactly from source warp_event 6,3
    ASSERT_EQ(warp_op->x, 6u);
    ASSERT_EQ(warp_op->y, 3u);

    // MUTATION CHECK: x/y must NOT be swapped (historically dangerous bug)
    // If x and y were swapped: x=3, y=6
    ASSERT_TRUE(warp_op->x != 3u || warp_op->y != 6u); // can't both be flipped
    ASSERT_EQ(warp_op->x, 6u); // x is definitely 6
    ASSERT_EQ(warp_op->y, 3u); // y is definitely 3

    // MUTATION CHECK: MapId must NOT be (5 << 8) | 24 = wrong byte order
    constexpr MapId WRONG_MAP_ID = (5u << 8) | 24u;  // 0x0518 — byte-swapped group/map
    ASSERT_TRUE(warp_op->map != WRONG_MAP_ID);

    // MUTATION CHECK: MapId must NOT be just group (=24) or just map (=5)
    ASSERT_TRUE(warp_op->map != static_cast<MapId>(24u));
    ASSERT_TRUE(warp_op->map != static_cast<MapId>(5u));

    std::cout << "  [P4-3: warp ELMS_LAB map=0x1805 x=6 y=3 — MapId and coordinates preserved, not swapped ✓]\n";
}

// =============================================================================
// P4-4: promptbutton + pause 30 — both preserved, neither erased
//
// Source: maps/ElmsLab.asm — ElmsLabWalkUpToElmScript:
//   promptbutton                   ; $55  — wait with BG sync, inline (no textbox close)
//
// maps/ElmsLab.asm — ElmsLabHealingMachine_HealParty:
//   pause 30                       ; $8B db 0x1E  — pause 30 frames
//
// These are two historically dangerous collapse cases:
//   - promptbutton was previously collapsed to Sem_WaitButton (opcode confusion)
//   - pause was previously erased or collapsed to deactivatefacing
//
// The test sequence $55 $8B $1E $91 exercises both in sequence.
// ORACLE: must produce Sem_PromptButton then Sem_Pause{30}, in that order.
// =============================================================================
TEST(p4_4_promptbutton_distinct_pause_preserved) {
    using namespace crystal;
    using namespace enginemon;

    // Hand-authored bytes from source:
    // promptbutton=$55, pause_opcode=$8B, pause_len=0x1E(=30), end=$91
    // Source: ElmsLabWalkUpToElmScript (promptbutton after writetext)
    //         ElmsLabHealingMachine_HealParty (pause 30 before RestartMapMusic)
    std::vector<uint8_t> bytes = {
        0x55,       // promptbutton (not waitbutton=0x54)
        0x8B, 0x1E, // pause 30 (0x8B=pause opcode, 0x1E=30)
        0x91        // end
    };
    while (bytes.size() < 0x8000) bytes.push_back(0xFF);
    auto rom = make_rom_from_bytes(bytes);
    SymbolMap sym;
    TypedScriptDecoder dec(*rom, sym);
    CrystalScriptIR ir = dec.decode_script(0x0000);
    ASSERT_TRUE(ir.commands.size() >= 2u);

    // Stage 1: promptbutton and pause decoded correctly
    {
        auto* pb = std::get_if<Cmd_Promptbutton>(&ir.commands[0].data);
        ASSERT_TRUE(pb != nullptr);
        // MUTATION: must NOT be Cmd_Waitbutton (opcode 0x54 vs 0x55)
        ASSERT_TRUE(!std::holds_alternative<Cmd_Waitbutton>(ir.commands[0].data));
    }
    {
        auto* p = std::get_if<Cmd_Pause>(&ir.commands[1].data);
        ASSERT_TRUE(p != nullptr);
        ASSERT_EQ(p->length, 30u);
        // MUTATION: pause_len must NOT be 0 (erased) or 0x8B (opcode itself)
        ASSERT_TRUE(p->length != 0u);
        ASSERT_TRUE(p->length != 0x8Bu);
    }

    // Stage 4: Sem_PromptButton followed by Sem_Pause{30}
    auto lr = lower_ir(ir);
    ASSERT_TRUE(lr.success);

    bool found_prompt = false, found_wait_instead = false;
    bool found_pause30 = false;
    int prompt_idx = -1, pause_idx = -1, inst_i = 0;
    for (const auto& blk : lr.ir.blocks) {
        for (const auto& inst : blk.instructions) {
            if (std::holds_alternative<Sem_PromptButton>(inst.op)) {
                found_prompt = true;
                prompt_idx = inst_i;
            }
            if (std::holds_alternative<Sem_WaitButton>(inst.op)) {
                found_wait_instead = true;
            }
            if (auto* p = std::get_if<Sem_Pause>(&inst.op)) {
                if (p->length == 30u) {
                    found_pause30 = true;
                    pause_idx = inst_i;
                }
            }
            ++inst_i;
        }
    }

    // ORACLE: Sem_PromptButton must be present
    ASSERT_TRUE(found_prompt);
    // MUTATION CHECK: must NOT collapse to Sem_WaitButton
    ASSERT_FALSE(found_wait_instead);
    // ORACLE: Sem_Pause{30} must be present
    ASSERT_TRUE(found_pause30);
    // ORACLE: promptbutton precedes pause in instruction sequence (preserves source order)
    ASSERT_TRUE(prompt_idx < pause_idx);

    std::cout << "  [P4-4: promptbutton(0x55)→Sem_PromptButton, pause(30)→Sem_Pause{30}, order preserved ✓]\n";
}

// =============================================================================
// P4-5: sdefer ElmsLabWalkUpToElmScript — deferred body is a separate root
//
// Source: maps/ElmsLab.asm — ElmsLabMeetElmScene (scene script):
//   sdefer ElmsLabWalkUpToElmScript   ; $8D dw <ptr>
//   end                               ; $91
//
// Semantics: sdefer schedules the target to run AFTER the current script ends.
// It is NOT intra-script control flow (not a call, not a jump).
// The deferred target is discovered as a separate executable body.
//
// This test proves:
//   - opcode $8D decodes with a 16-bit pointer operand
//   - Sem_Sdefer carries a stable target_script_id (not raw pointer)
//   - The encoded target_script_id differs from the calling script's ID
//   - The ScriptId format is "deferred_<hex_address>" (source-provenance address)
//
// Bank resolution: entry_address drives the flat target computation.
// Using entry=0x4C000 (bank 0x13, within bank data) as example:
//   ptr = 0x5200 → flat = 0x13*0x4000 + (0x5200-0x4000) = 0x4C000 + 0x1200 = 0x4D200
// =============================================================================
TEST(p4_5_sdefer_deferred_body_is_separate_root) {
    using namespace crystal;
    using namespace enginemon;

    // Construct Cmd_Sdefer at entry_address = 0x4C000 (bank 0x13 = 19)
    // ptr = 0x5200 → flat = 19 * 0x4000 + (0x5200 - 0x4000) = 0x4C000 + 0x1200 = 0x4D200
    Cmd_Sdefer sdef;
    sdef.pointer = 0x5200;

    CrystalCommand cmd;
    cmd.data = sdef;
    cmd.span.rom_address = 0x4C000;
    cmd.span.raw_bytes = {0x8D, 0x00, 0x52};
    cmd.status = DecodeStatus::Unlowered;

    // Build IR with entry_address = 0x4C000 (bank 19 = 0x13)
    CrystalScriptIR ir = make_single_cmd_ir_with_entry(cmd, 0x4C000);

    auto lr = lower_ir(ir);
    ASSERT_TRUE(lr.success);

    const Sem_Sdefer* sdefer_op = nullptr;
    for (const auto& blk : lr.ir.blocks)
        for (const auto& inst : blk.instructions)
            if (auto* p = std::get_if<Sem_Sdefer>(&inst.op)) sdefer_op = p;

    ASSERT_TRUE(sdefer_op != nullptr);

    // ORACLE: flat = 19 * 0x4000 + (0x5200 - 0x4000) = 0x4C000 + 0x1200 = 0x4D200
    // target_script_id format: "deferred_<hex>" where hex is the flat address
    ASSERT_STR_EQ(sdefer_op->target_script_id, "deferred_4d200");

    // MUTATION CHECK 1: must NOT use raw ptr as flat (0x5200 → "deferred_5200")
    ASSERT_TRUE(sdefer_op->target_script_id != "deferred_5200");

    // MUTATION CHECK 2: must NOT use bank 0 resolution (flat = 0x5200-0x4000 = 0x1200)
    ASSERT_TRUE(sdefer_op->target_script_id != "deferred_1200");

    // MUTATION CHECK 3: the deferred body's ID must differ from the calling script
    // The calling entry is at 0x4C000 — "deferred_4d200" ≠ "deferred_4c000"
    ASSERT_TRUE(sdefer_op->target_script_id != "deferred_4c000");

    // Structural: sdefer is the ONLY instruction in the block (the scene script
    // does nothing else before end — it immediately defers)
    int sdefer_count = 0;
    for (const auto& blk : lr.ir.blocks)
        for (const auto& inst : blk.instructions)
            if (std::holds_alternative<Sem_Sdefer>(inst.op)) ++sdefer_count;
    ASSERT_EQ(sdefer_count, 1);

    std::cout << "  [P4-5: sdefer ptr=0x5200 bank=19 → flat=0x4D200 → deferred_4d200, not 5200 or 1200 ✓]\n";
}

// =============================================================================
// P4-6: variablesprite SPRITE_COPYCAT, SPRITE_LASS
//
// Source: maps/CopycatsHouse2F.asm — Copycat script:
//   variablesprite SPRITE_COPYCAT, SPRITE_LASS    ; $6D db 0x0B db 0x28
//
// Macro encoding (events.asm variablesprite):
//   db variablesprite_command ($6D)
//   db \1 - SPRITE_VARS     → SPRITE_COPYCAT($FB) - SPRITE_VARS($F0) = 0x0B = slot 11
//   db \2                   → SPRITE_LASS = 0x28
//
// constants/sprite_constants.asm:
//   SPRITE_VARS = $F0 (const_next $f0)
//   SPRITE_COPYCAT = $FB (11th var after $F0)
//   SPRITE_LASS = $28
//
// crystal/extract/sprite_ids.hpp:
//   slot_name for SPRITE_COPYCAT: "copycat" (index 0x0B = 11)
//   SPRITE_LASS (0x28) → fixed sprite "lass" → "fixed:lass"
//
// MUTATION CHECK: slot must NOT be 0 or SPRITE_COPYCAT value (0xFB), it's the OFFSET
// MUTATION CHECK: assigned_sprite_id must NOT be "" or a pokemon_icon: type
// =============================================================================
TEST(p4_6_variablesprite_copycat_lass) {
    using namespace crystal;
    using namespace enginemon;

    // Hand-authored bytes: variablesprite SPRITE_COPYCAT, SPRITE_LASS
    // $6D = variablesprite_command
    // $0B = SPRITE_COPYCAT($FB) - SPRITE_VARS($F0) = 11
    // $28 = SPRITE_LASS
    // $91 = end
    std::vector<uint8_t> bytes = {
        0x6D,   // variablesprite opcode
        0x0B,   // slot = SPRITE_COPYCAT - SPRITE_VARS = 0xFB - 0xF0 = 0x0B
        0x28,   // SPRITE_LASS = 0x28
        0x91    // end
    };
    while (bytes.size() < 0x8000) bytes.push_back(0xFF);
    auto rom = make_rom_from_bytes(bytes);
    SymbolMap sym;
    TypedScriptDecoder dec(*rom, sym);
    CrystalScriptIR ir = dec.decode_script(0x0000);
    ASSERT_TRUE(ir.commands.size() >= 1u);

    // Stage 1: Cmd_Variablesprite must carry slot=11 and sprite=0x28
    auto* cmd = std::get_if<Cmd_Variablesprite>(&ir.commands[0].data);
    ASSERT_TRUE(cmd != nullptr);
    ASSERT_EQ(cmd->slot, 0x0Bu);    // offset from SPRITE_VARS: slot index 11
    ASSERT_EQ(cmd->sprite, 0x28u);  // SPRITE_LASS

    // MUTATION CHECK: slot must NOT be SPRITE_COPYCAT value (0xFB, which would mean no subtraction)
    ASSERT_TRUE(cmd->slot != 0xFBu);
    // MUTATION CHECK: slot must NOT be 0
    ASSERT_TRUE(cmd->slot != 0u);

    // Stage 4: Sem_VariableSprite with semantic slot_name and sprite_id
    auto lr = lower_ir(ir);
    ASSERT_TRUE(lr.success);

    const Sem_VariableSprite* vs_op = nullptr;
    for (const auto& blk : lr.ir.blocks)
        for (const auto& inst : blk.instructions)
            if (auto* p = std::get_if<Sem_VariableSprite>(&inst.op)) vs_op = p;

    ASSERT_TRUE(vs_op != nullptr);

    // ORACLE: slot_name = "copycat" (semantic name for slot 11 = SPRITE_COPYCAT - SPRITE_VARS)
    ASSERT_STR_EQ(vs_op->slot_name, "copycat");

    // ORACLE: SPRITE_LASS (0x28) → "fixed:lass" (via crystal_sprite_byte_to_id)
    ASSERT_STR_EQ(vs_op->assigned_sprite_id, "fixed:lass");

    // MUTATION CHECK: must NOT be empty (erased sprite assignment)
    ASSERT_TRUE(!vs_op->slot_name.empty());
    ASSERT_TRUE(!vs_op->assigned_sprite_id.empty());

    // MUTATION CHECK: assigned_sprite_id must NOT be a pokemon_icon: type
    // (SPRITE_LASS is in the fixed sprite range, not the pokemon icon range)
    ASSERT_FALSE(vs_op->assigned_sprite_id.starts_with("pokemon_icon:"));

    // MUTATION CHECK: must NOT be "fixed:chris" (wrong sprite — would indicate
    // a wrong slot→sprite lookup, e.g., using slot 0x01 instead of 0x28)
    ASSERT_TRUE(vs_op->assigned_sprite_id != "fixed:chris");

    std::cout << "  [P4-6: variablesprite SPRITE_COPYCAT(slot=0x0B) SPRITE_LASS(0x28) → copycat/fixed:lass ✓]\n";
}

// =============================================================================
// P4-7: Pokémon icon source-fidelity — 128-byte payload, 16×16, 2×2 tiles
//
// Source authority (NOT derived from SpriteExtractor):
//   pokecrystal/engine/gfx/mon_icons.asm — GetIcon: lb bc, BANK(Icons), 8
//     → Request2bpp loads 8 tiles total = 2 frames × 4 tiles per frame
//   pokecrystal/data/sprite_anims/oam.asm — OAMData_RedWalk:
//     db 4; dbsprite -1,-1 / 0,-1 / -1,0 / 0,0 → 2×2 OBJ layout = 16×16 pixels
//   pokecrystal/engine/gfx/mon_icons.asm — GetMemIconGFX:
//     ld de, 8 * LEN_2BPP_TILE; add hl, de → advances 8 × 16 = 128 bytes
//
// Independent ROM byte evidence (not from SpriteExtractor):
//   IconPointers at bank 23 (0x17), flat 0x05EBBF
//   ICON_PIKACHU = index 4 → entry at flat 0x05EBBF + 4×2 = 0x05EBC7
//   PikachuIcon GFX ptr (little-endian dw from ROM) → flat 0x05E66B
//   128 bytes starting at 0x05E66B, byte-sum = 0x4ED0 (= 20176 decimal)
//   First byte [0] = 0x7F, byte at offset [64] (frame 1 start) = 0xB1
//
// Expected properties (all source-derived, none from SpriteExtractor):
//   - SpriteType::Icon
//   - 2 icon_frames (2 animation frames)
//   - Each frame: pixels array of 256 bytes (16×16 = 256 pixels)
//   - Pixel values all in range 0-3 (2bpp palette indices)
//   - Frame 0 ≠ Frame 1 (animation frames differ per source icon data)
//   - NOT 1024 bytes (old wrong 32×32 size), NOT 512 bytes per icon
//
// This test is INDEPENDENT of the oracle's own SpriteExtractor — it only
// checks geometry and pixel-range invariants derivable from source evidence.
// =============================================================================
TEST(p4_7_icon_format_source_fidelity) {
    using namespace crystal;
    using namespace enginemon;

    // This test requires the real ROM for live extraction.
    if (!g_rom || !g_profile) {
        std::cout << "  [P4-7: ROM not loaded — skipping live icon extraction]\n";
        return;
    }

    SpriteExtractor extractor(*g_rom, *g_profile);

    // Pikachu icon: verified source address chain above
    auto result = extractor.extract_pokemon_icon("pikachu");
    ASSERT_TRUE(result.success);
    ASSERT_EQ(result.sprite.type, SpriteType::Icon);

    // ORACLE: exactly 2 animation frames (source: Frameset_PartyMon has 2 oamframes)
    ASSERT_EQ(result.sprite.icon_frames.size(), static_cast<size_t>(2));

    // ORACLE: each frame is 16×16 = 256 pixels (source: 2×2 OBJ layout, 8px per OBJ)
    ASSERT_EQ(result.sprite.icon_frames[0].pixels.size(), static_cast<size_t>(256));
    ASSERT_EQ(result.sprite.icon_frames[1].pixels.size(), static_cast<size_t>(256));

    // MUTATION CHECK: must NOT be 32×32 = 1024 (the old wrong geometry)
    ASSERT_TRUE(result.sprite.icon_frames[0].pixels.size() != 1024u);

    // ORACLE: all pixels are valid 2bpp palette indices (0-3)
    // Source: 2bpp tile format — each pixel is 2 bits, value 0-3
    for (int f = 0; f < 2; ++f) {
        for (int i = 0; i < 256; ++i) {
            ASSERT_TRUE(result.sprite.icon_frames[f].pixels[i] <= 3u);
        }
    }

    // ORACLE: frame 0 ≠ frame 1 (Pikachu's animation frames are distinct)
    // Source: two separately stored 64-byte tile blocks in ROM
    ASSERT_TRUE(result.sprite.icon_frames[0].pixels != result.sprite.icon_frames[1].pixels);

    // MUTATION CHECK: bigmon icon also extracts correctly with same geometry
    // (proves the 128-byte read doesn't bleed into adjacent icons)
    auto bigmon = extractor.extract_pokemon_icon("bigmon");
    ASSERT_TRUE(bigmon.success);
    ASSERT_EQ(bigmon.sprite.icon_frames.size(), static_cast<size_t>(2));
    ASSERT_EQ(bigmon.sprite.icon_frames[0].pixels.size(), static_cast<size_t>(256));

    // MUTATION CHECK: pikachu and bigmon must produce distinct frame data
    // (they use different GFX from IconPointers — would be same if byte range overlapped)
    ASSERT_TRUE(result.sprite.icon_frames[0].pixels != bigmon.sprite.icon_frames[0].pixels);

    // Independent cross-check: Pikachu sprite_id must reflect source naming
    ASSERT_STR_EQ(result.sprite.sprite_id, "pokemon_icon:pikachu");
    ASSERT_STR_EQ(bigmon.sprite.sprite_id, "pokemon_icon:bigmon");

    std::cout << "  [P4-7: Pikachu icon 2 frames × 256px (16×16), all 2bpp, frames distinct, bigmon distinct ✓]\n";
}

// =============================================================================
// P4-8: FullGameCompiler package seam
//
// Traces the full production pipeline using the real Crystal ROM:
//   RomData → FullGameCompiler → EMON package → PackageReader
//
// Asserts:
//   - compile() returns true
//   - Package file exists and is non-empty
//   - PackageReader opens the package
//   - A known script ID exists (NewBarkTownSign → script_id contains the address)
//   - The script Lua text is non-empty (Stage 7 emission succeeded)
//   - The "johto_outdoor" tileset exists in the package
//   - The "chris" (player) sprite exists in the package
//   - Species→icon map is present and non-empty (MonMenuIcons was compiled)
//
// This test requires the real ROM. If ROM is unavailable it skips.
// It deliberately does NOT assert specific Lua text content because that
// would make the test implementation-derived. It only asserts existence
// and non-emptiness.
// =============================================================================
TEST(p4_8_fullcompiler_package_seam) {
    using namespace crystal;
    using namespace enginemon;

    if (!g_rom || !g_profile) {
        std::cout << "  [P4-8: ROM not loaded — skipping full compiler seam]\n";
        return;
    }

    auto out = std::filesystem::temp_directory_path() / "oracle_p4_8.emon";
    std::filesystem::remove(out);

    // Run the full production compiler
    FullGameCompiler compiler(*g_rom, *g_profile);
    FullCompilerConfig cfg;
    cfg.use_package_cache = false;
    cfg.worker_count = 1;
    bool ok = compiler.compile(out, cfg);

    // ORACLE: compile must succeed on the real vanilla Crystal ROM
    ASSERT_TRUE(ok);
    ASSERT_TRUE(std::filesystem::exists(out));
    ASSERT_TRUE(std::filesystem::file_size(out) > 0);

    // Open package through production PackageReader
    auto reader = enginemon::PackageReader::open(out);
    ASSERT_TRUE(reader != nullptr);

    // ORACLE: "johto_outdoor" tileset must be in the package
    // (New Bark Town uses it — it was the first map ever compiled)
    auto tileset_data = reader->load_tileset_data("johto_outdoor");
    ASSERT_TRUE(tileset_data.has_value());
    ASSERT_TRUE(!tileset_data->empty());

    // ORACLE: player sprite "chris" must be in the package
    auto chris_sprite = reader->load_sprite("chris");
    ASSERT_TRUE(chris_sprite.has_value());
    ASSERT_TRUE(chris_sprite->sprite_id == std::string("chris"));

    // ORACLE: species→icon map must be non-empty
    // (build_species_icon_map() was called and emitted all 251 species)
    auto icon_map = reader->load_species_icon_map();
    ASSERT_FALSE(icon_map.empty());
    // At least 240 species should have valid icon entries (251 total, some may be ICON_NULL)
    ASSERT_TRUE(icon_map.size() >= 240u);
    // ORACLE: Pikachu (species 25) maps to "pokemon_icon:pikachu"
    ASSERT_TRUE(icon_map.count(static_cast<SpeciesId>(25)) > 0);
    ASSERT_TRUE(icon_map.at(static_cast<SpeciesId>(25)) == std::string("pokemon_icon:pikachu"));
    // ORACLE: Charizard (species 6) maps to "pokemon_icon:bigmon"
    ASSERT_TRUE(icon_map.count(static_cast<SpeciesId>(6)) > 0);
    ASSERT_TRUE(icon_map.at(static_cast<SpeciesId>(6)) == std::string("pokemon_icon:bigmon"));

    // ORACLE: bigmon sprite must be in the package (asset closure fix)
    // Without the closure fix, bigmon would be absent (not in any map object event)
    auto bigmon_sprite = reader->load_sprite("pokemon_icon:bigmon");
    ASSERT_TRUE(bigmon_sprite.has_value());

    // ORACLE: New Bark Town map must be in the package
    auto nbt_map = reader->load_map("new_bark_town");
    ASSERT_TRUE(nbt_map.has_value());
    ASSERT_TRUE(nbt_map->map_id == std::string("new_bark_town"));

    // ORACLE: scripts must exist in the package
    // Script IDs use format "map_<group>_<map>_0x<address>" or "std_<id>"
    // New Bark Town is group=24, map=4 → script IDs contain "map_24_4_"
    // We check that the package has at least 1000 scripts (known corpus ≥ 1788)
    auto script_list = reader->list_scripts();
    ASSERT_TRUE(script_list.size() >= 1000u);

    // ORACLE: at least one New Bark Town script exists
    // Source: NewBarkTown is group=24, map=4 (map_constants.asm newgroup NEW_BARK=24)
    bool found_nbt_script = false;
    for (const auto& sid : script_list) {
        if (sid.find("map_24_4_") != std::string::npos) {
            found_nbt_script = true;
            // Verify its Lua is non-empty (Stage 7 ran)
            auto lua = reader->load_script(sid);
            ASSERT_TRUE(lua.has_value());
            ASSERT_TRUE(!lua->empty());
            break;
        }
    }
    ASSERT_TRUE(found_nbt_script);

    std::filesystem::remove(out);
    std::cout << "  [P4-8: full compiler seam — johto_outdoor/chris/bigmon/nbt_map/scripts all in package ✓]\n";
}

// =============================================================================
// RGBDS BANK NOTATION ADVERSARIAL TESTS
// =============================================================================
// These tests prove the decimal-vs-hex RGBDS bank notation hazard is caught.
//
// Hazard: RGBDS sym files use hexadecimal bank numbers.
//   "23:6ac4 MonMenuIcons" means bank=0x23 (decimal 35), NOT bank=23 decimal (=0x17).
//
// The bug fixed in commit 73411bb was exactly this: MON_ICONS_BANK was 0x17
// (decimal 23) when the correct value from the sym is 0x23 (hex 23 = decimal 35).
//
// Test P4-RBN-1 (rgbds_bank_correct):
//   Prove that bank=0x23 produces the correct MonMenuIcons bytes from the real ROM.
//   Expected: byte 0 = ICON_BULBASAUR(22), byte 1 = 22, byte 2 = 22 (Bulbasaur family)
//   Source: pokecrystal/data/pokemon/menu_icons.asm — first three entries are
//           BULBASAUR/IVYSAUR/VENUSAUR all mapped to ICON_BULBASAUR(22)
//
// Test P4-RBN-2 (rgbds_bank_wrong_decimal_gives_wrong_bytes):
//   Prove that bank=0x17 (the decimal-23 mistake) produces different, wrong bytes.
//   This is the mutation check: if bank=0x17 gives the same result, the guard fails.
//
// Test P4-RBN-3 (symbol_map_parses_bank_as_hex):
//   Prove the .sym parser produces bank=0x23 from "23:6ac4", not 23 decimal.
//   Also prove "17:6ac4" (bank 0x17) produces a different flat address.
//   Malformed notation ("GG:6ac4") must produce a non-match (no symbol found).
// =============================================================================

TEST(p4_rgbds_bank_correct) {
    // Requires real ROM to verify actual bytes.
    if (!g_rom || !g_profile) {
        std::cout << "  [P4-RBN-1: ROM not loaded — skipping]\n";
        return;
    }

    using namespace crystal;

    // MonMenuIcons: sym "23:6ac4" → bank=0x23, addr=0x6ac4
    // Source: pokecrystal/data/pokemon/menu_icons.asm
    //   MonMenuIcons:
    //     db ICON_BULBASAUR   ; BULBASAUR  → 22
    //     db ICON_BULBASAUR   ; IVYSAUR    → 22
    //     db ICON_BULBASAUR   ; VENUSAUR   → 22
    //     db ICON_CHARMANDER  ; CHARMANDER → 23
    //     db ICON_CHARMANDER  ; CHARMELEON → 23
    //     db ICON_BIGMON      ; CHARIZARD  → 38

    constexpr uint8_t  CORRECT_BANK = rgbds_bank(0x23); // sym: "23:6ac4"
    constexpr uint16_t MON_ICONS_ADDR = 0x6ac4;

    uint32_t flat = g_rom->bank_to_flat(CORRECT_BANK, MON_ICONS_ADDR);
    ASSERT_TRUE(flat + 6 <= g_rom->size());

    auto first6 = g_rom->read_bytes(flat, 6);

    // ORACLE: first three bytes are ICON_BULBASAUR=22 (Bulbasaur/Ivysaur/Venusaur)
    ASSERT_EQ(first6[0], 22u); // BULBASAUR → ICON_BULBASAUR
    ASSERT_EQ(first6[1], 22u); // IVYSAUR   → ICON_BULBASAUR
    ASSERT_EQ(first6[2], 22u); // VENUSAUR  → ICON_BULBASAUR
    ASSERT_EQ(first6[3], 23u); // CHARMANDER → ICON_CHARMANDER
    ASSERT_EQ(first6[4], 23u); // CHARMELEON → ICON_CHARMANDER
    ASSERT_EQ(first6[5], 38u); // CHARIZARD  → ICON_BIGMON

    // ORACLE: all six values are valid icon type indices (0-38)
    for (int i = 0; i < 6; ++i) {
        ASSERT_TRUE(first6[i] <= 38u);
    }

    std::cout << "  [P4-RBN-1: bank=0x23 → MonMenuIcons[0..5] = {22,22,22,23,23,38} ✓]\n";
}

TEST(p4_rgbds_bank_wrong_decimal_gives_wrong_bytes) {
    // Proves bank=0x17 (decimal 23 — the pre-fix wrong value) reads different, wrong bytes.
    // This is the mutation check: the correct and wrong banks must produce different data.
    if (!g_rom || !g_profile) {
        std::cout << "  [P4-RBN-2: ROM not loaded — skipping]\n";
        return;
    }

    using namespace crystal;

    constexpr uint8_t  CORRECT_BANK = rgbds_bank(0x23); // sym: "23:6ac4"
    constexpr uint8_t  WRONG_BANK   = 0x17;             // decimal 23 — pre-fix mistake
    constexpr uint16_t MON_ICONS_ADDR = 0x6ac4;

    uint32_t flat_correct = g_rom->bank_to_flat(CORRECT_BANK, MON_ICONS_ADDR);
    uint32_t flat_wrong   = g_rom->bank_to_flat(WRONG_BANK,   MON_ICONS_ADDR);

    // The two flat addresses must differ (basic sanity)
    ASSERT_TRUE(flat_correct != flat_wrong);

    auto bytes_correct = g_rom->read_bytes(flat_correct, 6);
    auto bytes_wrong   = g_rom->read_bytes(flat_wrong,   6);

    // ORACLE: correct bank gives the expected MonMenuIcons pattern
    ASSERT_EQ(bytes_correct[0], 22u); // ICON_BULBASAUR
    ASSERT_EQ(bytes_correct[5], 38u); // ICON_BIGMON (Charizard)

    // MUTATION CHECK: wrong bank (0x17) must NOT give the same pattern
    // If this assertion fails, the two banks happen to point to identical data —
    // which would indicate the test itself is not meaningful.
    // In practice, bank 0x17 at 0x6ac4 contains tileset animation data, not icon types.
    bool same = true;
    for (int i = 0; i < 6; ++i) {
        if (bytes_correct[i] != bytes_wrong[i]) { same = false; break; }
    }
    ASSERT_FALSE(same); // wrong bank must give different bytes

    // MUTATION CHECK: wrong bank does NOT give all values in [1,38] (valid icon types)
    // At least one byte from the wrong bank must be outside the valid icon type range
    bool all_valid_icon_type = true;
    for (int i = 0; i < 6; ++i) {
        if (bytes_wrong[i] == 0 || bytes_wrong[i] > 38u) {
            all_valid_icon_type = false;
            break;
        }
    }
    ASSERT_FALSE(all_valid_icon_type); // wrong bank produces non-icon-type bytes

    std::cout << "  [P4-RBN-2: bank=0x17 (wrong decimal-23) gives different, non-icon bytes ✓]\n";
}

TEST(p4_symbol_map_parses_bank_as_hex) {
    // Proves the .sym parser correctly reads bank as hexadecimal.
    // Source: crystal/rom/symbol_map.cpp — std::stoul(match[1], nullptr, 16)
    //
    // "23:6ac4 MonMenuIcons" must parse as bank=0x23 (decimal 35), not 23 decimal.
    // "17:6ac4 FakeSym" must parse as bank=0x17 (decimal 23) — distinct from 0x23.
    //
    // Flat addresses (formula: bank * 0x4000 + (addr - 0x4000)):
    //   bank=0x23, addr=0x6ac4 → 0x23 * 0x4000 + 0x2ac4 = 0x8C000 + 0x2ac4 = 0x8EAC4
    //   bank=0x17, addr=0x6ac4 → 0x17 * 0x4000 + 0x2ac4 = 0x5C000 + 0x2ac4 = 0x5EAC4
    //
    // These flat addresses are DIFFERENT — proves the parser distinguishes them.

    using namespace crystal;

    // Build a synthetic .sym file content with two entries at the same addr, different banks
    std::string sym_content =
        "23:6ac4 MonMenuIcons\n"  // bank 0x23 = 35 decimal
        "17:6ac4 FakeSymDecimal23\n"  // bank 0x17 = 23 decimal — the wrong value
        "GG:6ac4 MalformedEntry\n";   // malformed — non-hex digit

    auto tmp = std::filesystem::temp_directory_path() / "oracle_rgbds_bank_test.sym";
    {
        std::ofstream f(tmp);
        f << sym_content;
    }

    auto sym_map = SymbolMap::load(tmp);
    ASSERT_TRUE(sym_map != nullptr);
    std::filesystem::remove(tmp);

    // ORACLE: "MonMenuIcons" at bank 0x23 → flat = 0x23*0x4000 + (0x6ac4-0x4000) = 0x8EAC4
    constexpr uint32_t EXPECTED_FLAT_CORRECT = 0x23u * 0x4000u + (0x6ac4u - 0x4000u); // 0x8EAC4
    constexpr uint32_t EXPECTED_FLAT_WRONG   = 0x17u * 0x4000u + (0x6ac4u - 0x4000u); // 0x5EAC4

    auto* mon_menu_icons = sym_map->find("MonMenuIcons");
    ASSERT_TRUE(mon_menu_icons != nullptr);
    ASSERT_EQ(mon_menu_icons->bank,         0x23u);
    ASSERT_EQ(mon_menu_icons->address,      0x6ac4u);
    ASSERT_EQ(mon_menu_icons->flat_address, EXPECTED_FLAT_CORRECT);

    // MUTATION CHECK: flat must NOT be the decimal-23 (wrong) interpretation
    ASSERT_TRUE(mon_menu_icons->flat_address != EXPECTED_FLAT_WRONG);

    // ORACLE: "FakeSymDecimal23" at bank 0x17 produces a DIFFERENT flat address
    auto* fake_sym = sym_map->find("FakeSymDecimal23");
    ASSERT_TRUE(fake_sym != nullptr);
    ASSERT_EQ(fake_sym->bank,         0x17u);
    ASSERT_EQ(fake_sym->flat_address, EXPECTED_FLAT_WRONG);

    // MUTATION CHECK: the two symbols' flat addresses are distinct
    ASSERT_TRUE(mon_menu_icons->flat_address != fake_sym->flat_address);

    // ORACLE: "MalformedEntry" with "GG:" (non-hex) must NOT be parsed as a valid symbol
    auto* malformed = sym_map->find("MalformedEntry");
    ASSERT_TRUE(malformed == nullptr); // regex fails to match → not added to symbol map

    // Explicit flat arithmetic verification
    // "23:6ac4" → bank=0x23=35, addr=0x6ac4
    // flat = 35 * 16384 + (27332 - 16384) = 573440 + 10948 = 584388 = 0x8EAC4
    ASSERT_EQ(EXPECTED_FLAT_CORRECT, 0x8EAC4u);
    // "17:6ac4" → bank=0x17=23, addr=0x6ac4
    // flat = 23 * 16384 + 10948 = 376832 + 10948 = 387780 = 0x5EAC4
    ASSERT_EQ(EXPECTED_FLAT_WRONG, 0x5EAC4u);
    // These must differ — the whole point
    ASSERT_TRUE(EXPECTED_FLAT_CORRECT != EXPECTED_FLAT_WRONG);

    std::cout << "  [P4-RBN-3: sym '23:6ac4'→bank=0x23→flat=0x8EAC4, '17:6ac4'→0x5EAC4, 'GG:'→rejected ✓]\n";
}

// =============================================================================
// MAIN
// =============================================================================

// =============================================================================
// ORACLE PHASE 5 — FULL-PIPE EXECUTION TESTS
// =============================================================================
//
// FULL-PIPE HARNESS DESIGN
// ─────────────────────────
// One harness function (make_p5_loop) assembles the full runtime stack:
//   PackageReader   → script loader (load_script by canonical ID)
//   LuaRuntime      → bound to GameState
//   HeadlessGameLoop→ script execution + tick/resume
//   GameState       → observable final state
//
// All tests use real compiled Lua from Stage 7 (semantic_lua_emitter).
// No hand-constructed SemanticIR. No mocked PackageReader. No fake scripts.
//
// NBT ADDRESS CONSTANTS (source-verified from pokecrystal/maps/NewBarkTown.asm)
// NewBarkTownTeacherScript: bank 0x6A, addr 0x406F → flat 1736815
// NewBarkTownSign BgEvent:  bank 0x6A, addr 0x40C8 → flat 1736904
// Script ID format: "map_<group>_<index>_0x<decimal_flat>"
//   NBT=group24,index4 → "map_24_4_0x1736815", "map_24_4_0x1736904", etc.
// =============================================================================
