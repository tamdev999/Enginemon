// crystal/script/semantic_legalizer.cpp
// Stage 4: Block-local semantic legalization from Crystal CFG to SemanticScriptIR

#include "crystal/script/semantic_legalizer.hpp"
#include "crystal/script/decoder.hpp"  // For text decoding
#include "crystal/script/pokemail_registry.hpp"
#include "crystal/script/text_registry.hpp"
#include <sstream>
#include <iomanip>
#include <optional>

namespace crystal {

// =============================================================================
// LOWERING CONTEXT IMPLEMENTATION
// =============================================================================

const CrystalCommand* LoweringContext::peek(size_t offset) const {
    if (!source_ir || !current_block) return nullptr;
    size_t idx = current_block->command_start + cursor + offset;
    if (idx >= current_block->command_start + current_block->command_count) {
        return nullptr;
    }
    if (idx >= source_ir->commands.size()) return nullptr;
    return &source_ir->commands[idx];
}

size_t LoweringContext::remaining() const {
    if (!current_block) return 0;
    if (cursor >= current_block->command_count) return 0;
    return current_block->command_count - cursor;
}

bool LoweringContext::is_block_boundary(uint32_t addr) const {
    if (!cfg) return false;
    return cfg->address_to_block.contains(addr);
}


// =============================================================================
// LOWERING RULES IMPLEMENTATION
// =============================================================================

void LoweringRules::add_rule(const std::string& name, LoweringRule rule) {
    rules_.emplace_back(name, std::move(rule));
}

RuleResult LoweringRules::try_rules(LoweringContext& ctx) const {
    for (const auto& [name, rule] : rules_) {
        RuleResult result = rule(ctx);
        if (result.matched) {
            return result;
        }
    }
    return RuleResult{};  // No rule matched
}

// =============================================================================
// SEMANTIC LEGALIZER IMPLEMENTATION
// =============================================================================

void SemanticLegalizer::ensure_rules() {
    if (rules_.count() == 0) {
        rules_ = LoweringRules::create_default();
    }
}

void SemanticLegalizer::build_label_map(const CrystalCFG& cfg, LoweringContext& ctx) {
    ctx.label_map.clear();
    for (size_t i = 0; i < cfg.blocks.size(); ++i) {
        ctx.label_map[cfg.blocks[i].start_address] = static_cast<enginemon::SemanticLabelId>(i);
    }
}


enginemon::UnloweredDiagnostic SemanticLegalizer::make_unlowered_diagnostic(
    const CrystalCommand& cmd, const std::string& reason,
    size_t block_index, size_t instruction_index) {
    
    enginemon::UnloweredDiagnostic diag;
    diag.opcode = cmd.opcode();
    diag.raw_bytes = cmd.span.raw_bytes;
    diag.reason = reason;
    
    std::ostringstream prov;
    prov << "0x" << std::hex << cmd.span.rom_address;
    diag.provenance = prov.str();
    diag.block_index = block_index;
    diag.instruction_index = instruction_index;
    
    return diag;
}

enginemon::LoweringResult SemanticLegalizer::lower(
    const CrystalScriptIR& ir, const CrystalCFG& cfg) {
    
    ensure_rules();
    
    enginemon::LoweringResult result;
    result.ir.script_id = cfg.script_name;
    result.ir.script_name = cfg.script_name;
    result.ir.source_rom_address = cfg.entry_address;
    
    // Build label map for block references
    LoweringContext ctx;
    ctx.source_ir = &ir;
    ctx.cfg = &cfg;
    ctx.native_registry = native_registry_;
    ctx.ram_registry = ram_registry_;
    ctx.elevator_registry = elevator_registry_;
    ctx.pokemail_registry = pokemail_registry_;
    ctx.text_registry = text_registry_;
    ctx.num_pokemon = num_pokemon_;  // Pass profile domain to context
    build_label_map(cfg, ctx);

    // Track source commands consumed for invariant checking
    size_t source_commands_consumed = 0;

    // Lower each block
    for (size_t block_idx = 0; block_idx < cfg.blocks.size(); ++block_idx) {
        const auto& block = cfg.blocks[block_idx];
        enginemon::SemanticBasicBlock sem_block = lower_block(block, ir, ctx, 
                                                               source_commands_consumed, 
                                                               result, block_idx);
        
        // Count semantic instructions produced (all are valid lowered ops now)
        result.commands_lowered += sem_block.instructions.size();
        for (const auto& inst : sem_block.instructions) {
            // Track by opcode (we'll need to add opcode tracking to lowered instructions)
        }
        
        result.ir.blocks.push_back(std::move(sem_block));
    }
    
    // INVARIANT: Every source command must be consumed exactly once
    result.commands_consumed = source_commands_consumed;
    
    // Compute absorbed commands (consumed but produced no instruction or diagnostic)
    size_t instructions_plus_unlowered = result.commands_lowered + result.commands_unlowered;
    result.commands_absorbed = result.commands_consumed - instructions_plus_unlowered;
    
    // Build label_to_block map
    for (size_t i = 0; i < result.ir.blocks.size(); ++i) {
        result.ir.label_to_block[result.ir.blocks[i].id] = i;
    }
    
    result.success = (result.commands_unlowered == 0);
    return result;
}


enginemon::SemanticBasicBlock SemanticLegalizer::lower_block(
    const BasicBlock& block, const CrystalScriptIR& ir, LoweringContext& ctx,
    size_t& source_commands_consumed, enginemon::LoweringResult& result, size_t block_idx) {
    
    enginemon::SemanticBasicBlock sem_block;
    sem_block.id = static_cast<enginemon::SemanticLabelId>(block.id);
    sem_block.label = block.label.empty() ? 
        ("block_" + std::to_string(block.id)) : block.label;
    sem_block.is_entry = block.is_entry;
    
    ctx.current_block = &block;
    ctx.cursor = 0;
    
    // Batch 9: Reset block-local ScriptVar context at block entry
    // No propagation across CFG edges - context begins unknown
    ctx.block_ctx.invalidate();
    
    // Process commands in block
    while (ctx.cursor < block.command_count) {
        size_t cmd_idx = block.command_start + ctx.cursor;
        if (cmd_idx >= ir.commands.size()) break;
        
        const CrystalCommand& cmd = ir.commands[cmd_idx];
        
        // Try lowering rules
        RuleResult rule_result = rules_.try_rules(ctx);
        
        if (rule_result.matched && rule_result.consumed > 0) {
            // Track source commands consumed
            source_commands_consumed += rule_result.consumed;
            
            // Add all produced semantic instructions
            for (auto& inst : rule_result.instructions) {
                inst.source_index = ctx.cursor;
                sem_block.instructions.push_back(std::move(inst));
            }
            
            // Track absorbed opcodes (consumed but no instruction produced)
            for (uint8_t absorbed_op : rule_result.absorbed_opcodes) {
                result.absorbed_by_opcode[absorbed_op]++;
            }
            
            ctx.cursor += rule_result.consumed;
        } else {
            // No rule matched - add as unlowered diagnostic (NOT to IR)
            source_commands_consumed += 1;
            result.commands_unlowered++;
            result.unlowered_by_opcode[cmd.opcode()]++;
            
            auto diag = make_unlowered_diagnostic(cmd, "no lowering rule matched", 
                                                   block_idx, ctx.cursor);
            result.unlowered.push_back(std::move(diag));
            
            ctx.cursor++;
        }
    }
    
    return sem_block;
}


// =============================================================================
// OPCODE NAME HELPER
// =============================================================================

const char* crystal_opcode_name(uint8_t opcode) {
    static const char* names[] = {
        "scall", "farscall", "memcall", "sjump", "farsjump", "memjump",
        "ifequal", "ifnotequal", "iffalse", "iftrue", "ifgreater", "ifless",
        "jumpstd", "callstd", "callasm", "special", "memcallasm",
        "checkmapscene", "setmapscene", "checkscene", "setscene",
        "setval", "addval", "random", "checkver", "readmem", "writemem",
        "loadmem", "readvar", "writevar", "loadvar",
        "giveitem", "takeitem", "checkitem", "givemoney", "takemoney",
        "checkmoney", "givecoins", "takecoins", "checkcoins",
        "addcellnum", "delcellnum", "checkcellnum",
        "checktime", "checkpoke", "givepoke", "giveegg", "givepokemail",
        "checkpokemail", "checkevent", "clearevent", "setevent",
        "checkflag", "clearflag", "setflag", "wildon", "wildoff",
        "xycompare", "warpmod", "blackoutmod", "warp",
        "getmoney", "getcoins", "getnum", "getmonname", "getitemname",
        "getcurlandmarkname", "gettrainername", "getstring",
        "itemnotify", "pocketisfull", "opentext", "reanchormap",
        "closetext", "writeunusedbyte", "farwritetext", "writetext",
        "repeattext", "yesorno", "loadmenu", "closewindow",
        "jumptextfaceplayer", "farjumptext", "jumptext", "waitbutton",
        "promptbutton", "pokepic", "closepokepic", "2dmenu",
        "verticalmenu", "loadpikachudata", "randomwildmon",
        "loadtemptrainer", "loadwildmon", "loadtrainer", "startbattle",
        "reloadmapafterbattle", "catchtutorial", "trainertext",
        "trainerflagaction", "winlosstext", "scripttalkafter",
        "endifjustbattled", "checkjustbattled", "setlasttalked",
        "applymovement", "applymovementlasttalked", "faceplayer",
        "faceobject", "variablesprite", "disappear", "appear",
        "follow", "stopfollow", "moveobject", "writeobjectxy",
        "loademote", "showemote", "turnobject", "follownotexact",
        "earthquake", "changemapblocks", "changeblock", "reloadmap",
        "refreshmap", "writecmdqueue", "delcmdqueue",
        "playmusic", "encountermusic", "musicfadeout", "playmapmusic",
        "dontrestartmapmusic", "cry", "playsound", "waitsfx",
        "warpsound", "specialsound", "autoinput", "newloadmap",
        "pause", "deactivatefacing", "sdefer", "warpcheck",
        "stopandsjump", "endcallback", "end", "reloadend", "endall",
        "pokemart", "elevator", "trade", "askforphonenumber",
        "phonecall", "hangup", "describedecoration", "fruittree",
        "specialphonecall", "checkphonecall", "verbosegiveitem",
        "verbosegiveitemvar", "swarm", "halloffame", "credits",
        "warpfacing", "battletowertext", "getlandmarkname",
        "gettrainerclassname", "getname", "wait", "checksave"
    };
    
    if (opcode <= 0xA9) {
        return names[opcode];
    }
    return "unknown";
}


// =============================================================================
// DEFAULT LOWERING RULES
// =============================================================================

namespace lowering_rules {

// Helper to make semantic label ref from ROM address
static enginemon::SemanticLabelRef make_label_ref(uint32_t rom_addr, 
                                                   const LoweringContext& ctx) {
    enginemon::SemanticLabelRef ref;
    auto it = ctx.label_map.find(rom_addr);
    if (it != ctx.label_map.end()) {
        ref.id = it->second;
    }
    std::ostringstream ss;
    ss << "loc_" << std::hex << rom_addr;
    ref.name = ss.str();
    return ref;
}

// Helper to make semantic instruction
static enginemon::SemanticInstruction make_inst(enginemon::SemanticOp op) {
    enginemon::SemanticInstruction inst;
    inst.op = std::move(op);
    return inst;
}

// --- Control Flow Rules ---

RuleResult rule_end(LoweringContext& ctx) {
    const auto* cmd = ctx.peek();
    if (!cmd) return {};
    
    if (std::holds_alternative<Cmd_End>(cmd->data) ||
        std::holds_alternative<Cmd_Endall>(cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        r.instructions.push_back(make_inst(enginemon::Sem_End{}));
        return r;
    }
    return {};
}


RuleResult rule_return(LoweringContext& ctx) {
    const auto* cmd = ctx.peek();
    if (!cmd) return {};
    
    if (std::holds_alternative<Cmd_Endcallback>(cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        r.instructions.push_back(make_inst(enginemon::Sem_Return{}));
        return r;
    }
    return {};
}

RuleResult rule_jump(LoweringContext& ctx) {
    const auto* cmd = ctx.peek();
    if (!cmd) return {};
    
    if (auto* sjump = std::get_if<Cmd_Sjump>(&cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        enginemon::Sem_Jump jump;
        jump.target = make_label_ref(sjump->target.rom_address, ctx);
        r.instructions.push_back(make_inst(std::move(jump)));
        return r;
    }
    if (auto* farsjump = std::get_if<Cmd_Farsjump>(&cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        enginemon::Sem_Jump jump;
        jump.target = make_label_ref(farsjump->target.rom_address, ctx);
        r.instructions.push_back(make_inst(std::move(jump)));
        return r;
    }
    return {};
}


RuleResult rule_conditional(LoweringContext& ctx) {
    const auto* cmd = ctx.peek();
    if (!cmd) return {};
    
    auto make_conditional = [&](uint32_t target_addr, const std::string& cond) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        enginemon::Sem_JumpIf jumpif;
        jumpif.target = make_label_ref(target_addr, ctx);
        jumpif.condition = cond;
        r.instructions.push_back(make_inst(std::move(jumpif)));
        return r;
    };
    
    if (auto* p = std::get_if<Cmd_Iftrue>(&cmd->data)) {
        return make_conditional(p->target.rom_address, "true");
    }
    if (auto* p = std::get_if<Cmd_Iffalse>(&cmd->data)) {
        return make_conditional(p->target.rom_address, "false");
    }
    if (auto* p = std::get_if<Cmd_Ifequal>(&cmd->data)) {
        return make_conditional(p->target.rom_address, 
                               "== " + std::to_string(p->value));
    }
    if (auto* p = std::get_if<Cmd_Ifnotequal>(&cmd->data)) {
        return make_conditional(p->target.rom_address, 
                               "!= " + std::to_string(p->value));
    }
    if (auto* p = std::get_if<Cmd_Ifgreater>(&cmd->data)) {
        return make_conditional(p->target.rom_address, 
                               "> " + std::to_string(p->value));
    }
    if (auto* p = std::get_if<Cmd_Ifless>(&cmd->data)) {
        return make_conditional(p->target.rom_address, 
                               "< " + std::to_string(p->value));
    }
    return {};
}


RuleResult rule_call(LoweringContext& ctx) {
    const auto* cmd = ctx.peek();
    if (!cmd) return {};
    
    if (auto* scall = std::get_if<Cmd_Scall>(&cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        enginemon::Sem_Call call;
        call.target = make_label_ref(scall->target.rom_address, ctx);
        r.instructions.push_back(make_inst(std::move(call)));
        return r;
    }
    if (auto* farscall = std::get_if<Cmd_Farscall>(&cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        enginemon::Sem_Call call;
        call.target = make_label_ref(farscall->target.rom_address, ctx);
        r.instructions.push_back(make_inst(std::move(call)));
        return r;
    }
    return {};
}

// --- Deferred Script Execution ---
// sdefer schedules a script to execute after the current scene script completes.
// The target IS discovered and compiled as its own body by the TypedScriptDecoder.
// Here we just emit a semantic marker with the target script_id.
RuleResult rule_sdefer(LoweringContext& ctx) {
    const auto* cmd = ctx.peek();
    if (!cmd) return {};
    
    if (auto* sdef = std::get_if<Cmd_Sdefer>(&cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        
        // The deferred target is a local pointer resolved via script_bank
        // TypedScriptDecoder already followed this via ctx.pending
        // We generate a semantic script_id based on the resolved address
        uint32_t target_addr = sdef->pointer;
        
        enginemon::Sem_Sdefer op;
        // Generate script_id based on resolved address
        // The target is a separate compiled body discovered by the decoder
        auto label = make_label_ref(target_addr, ctx);
        op.target_script_id = label.name;
        
        r.instructions.push_back(make_inst(std::move(op)));
        return r;
    }
    return {};
}

RuleResult rule_std_script(LoweringContext& ctx) {
    const auto* cmd = ctx.peek();
    if (!cmd) return {};
    
    if (auto* jstd = std::get_if<Cmd_Jumpstd>(&cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        enginemon::Sem_JumpStd op;
        // StdScriptId currently uses Crystal's table index as stable identifier
        // The semantic linker verifies these resolve to compiled StdScript bodies
        op.std_id = enginemon::StdScriptId{jstd->std_id};
        op.name = "std_" + std::to_string(jstd->std_id);
        r.instructions.push_back(make_inst(std::move(op)));
        return r;
    }
    if (auto* cstd = std::get_if<Cmd_Callstd>(&cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        enginemon::Sem_CallStd op;
        // StdScriptId currently uses Crystal's table index as stable identifier
        // The semantic linker verifies these resolve to compiled StdScript bodies
        op.std_id = enginemon::StdScriptId{cstd->std_id};
        op.name = "std_" + std::to_string(cstd->std_id);
        r.instructions.push_back(make_inst(std::move(op)));
        return r;
    }
    return {};
}


// --- Flags/Variables Rules ---

RuleResult rule_set_flag(LoweringContext& ctx) {
    const auto* cmd = ctx.peek();
    if (!cmd) return {};
    
    if (auto* p = std::get_if<Cmd_Setevent>(&cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        enginemon::Sem_SetFlag op;
        op.flag = enginemon::FlagId{p->event_flag};
        r.instructions.push_back(make_inst(std::move(op)));
        return r;
    }
    if (auto* p = std::get_if<Cmd_Setflag>(&cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        enginemon::Sem_SetFlag op;
        op.flag = enginemon::FlagId{p->engine_flag};
        r.instructions.push_back(make_inst(std::move(op)));
        return r;
    }
    return {};
}

RuleResult rule_clear_flag(LoweringContext& ctx) {
    const auto* cmd = ctx.peek();
    if (!cmd) return {};
    
    if (auto* p = std::get_if<Cmd_Clearevent>(&cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        enginemon::Sem_ClearFlag op;
        op.flag = enginemon::FlagId{p->event_flag};
        r.instructions.push_back(make_inst(std::move(op)));
        return r;
    }
    if (auto* p = std::get_if<Cmd_Clearflag>(&cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        enginemon::Sem_ClearFlag op;
        op.flag = enginemon::FlagId{p->engine_flag};
        r.instructions.push_back(make_inst(std::move(op)));
        return r;
    }
    return {};
}


RuleResult rule_check_flag(LoweringContext& ctx) {
    const auto* cmd = ctx.peek();
    if (!cmd) return {};
    
    if (auto* p = std::get_if<Cmd_Checkevent>(&cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        enginemon::Sem_CheckFlag op;
        op.flag = enginemon::FlagId{p->event_flag};
        r.instructions.push_back(make_inst(std::move(op)));
        return r;
    }
    if (auto* p = std::get_if<Cmd_Checkflag>(&cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        enginemon::Sem_CheckFlag op;
        op.flag = enginemon::FlagId{p->engine_flag};
        r.instructions.push_back(make_inst(std::move(op)));
        return r;
    }
    return {};
}

RuleResult rule_set_var(LoweringContext& ctx) {
    const auto* cmd = ctx.peek();
    if (!cmd) return {};
    
    if (auto* p = std::get_if<Cmd_Setval>(&cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        enginemon::Sem_SetVar op;
        op.var = enginemon::VarId{0};  // wScriptVar
        op.source = enginemon::VarValueSource::literal(p->value);
        r.instructions.push_back(make_inst(std::move(op)));
        
        // Batch 9: Record literal in block-local context for subsequent Specials
        // The setval STILL emits Sem_SetVar (observable wScriptVar write) AND records fact
        ctx.block_ctx.on_setval(p->value);
        
        return r;
    }
    if (auto* p = std::get_if<Cmd_Loadvar>(&cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        enginemon::Sem_SetVar op;
        op.var = enginemon::VarId{p->var_id};
        op.source = enginemon::VarValueSource::literal(p->value);
        r.instructions.push_back(make_inst(std::move(op)));
        // Note: loadvar writes to a VAR slot, not wScriptVar, so no context change
        return r;
    }
    return {};
}


RuleResult rule_add_var(LoweringContext& ctx) {
    const auto* cmd = ctx.peek();
    if (!cmd) return {};
    
    if (auto* p = std::get_if<Cmd_Addval>(&cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        enginemon::Sem_AddVar op;
        op.var = enginemon::VarId{0};  // wScriptVar
        op.delta = p->value;
        r.instructions.push_back(make_inst(std::move(op)));
        
        // Batch 9: Invalidate context - addval modifies wScriptVar to unknown value
        ctx.block_ctx.invalidate();
        
        return r;
    }
    return {};
}

RuleResult rule_random(LoweringContext& ctx) {
    const auto* cmd = ctx.peek();
    if (!cmd) return {};
    
    if (auto* p = std::get_if<Cmd_Random>(&cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        enginemon::Sem_Random op;
        op.range = p->range;
        r.instructions.push_back(make_inst(std::move(op)));
        
        // Batch 9: Invalidate context - random writes non-deterministic value to wScriptVar
        ctx.block_ctx.invalidate();
        
        return r;
    }
    return {};
}

// --- RAM Operations Rules ---
// Stage 4: Lower known semantic RAM addresses to typed state operations
// Reference: pokecrystal WRAM definitions, Gen2Recomped mini-game state
//
// RAM addresses lowered:
//   0xd964 = wFarfetchdPosition  - Ilex Forest mini-game position (1-10)
//   0xd962 = wMooMooBerries      - MooMoo Farm berry feeding count
//   0xd963 = wUndergroundSwitchPositions - Goldenrod Underground switch puzzle
//   0xcf51 = wOtherPlayerLinkMode - Link cable status (read-only capability query)

// Known RAM address constants
static constexpr uint16_t RAM_FARFETCHD_POSITION = 0xd964;
static constexpr uint16_t RAM_MOO_MOO_BERRIES = 0xd962;
static constexpr uint16_t RAM_UNDERGROUND_SWITCHES = 0xd963;
static constexpr uint16_t RAM_LINK_MODE = 0xcf51;
static constexpr uint16_t RAM_STRENGTH_SPECIES = 0xd1ef;
static constexpr uint16_t RAM_TEMP_WILD_MON_SPECIES = 0xd22e;
static constexpr uint16_t RAM_BATTLE_TOWER_BEATEN_TRAINERS = 0xcf64;

// Known native call addresses for field moves
static constexpr uint32_t NATIVE_TryStrengthOW = 0xCD78;
static constexpr uint32_t NATIVE_SetStrengthFlag = 0xCD12;
static constexpr uint32_t NATIVE_HasRockSmash = 0xCF7C;
static constexpr uint32_t NATIVE_GetPartyNickname = 0xC706;
static constexpr uint32_t NATIVE_RockMonEncounter = 0xB8219;

// Known native call addresses for Battle Tower (corpus-discovered deferred scripts)
static constexpr uint32_t NATIVE_BattleTowerLoadLevelGroup = 0x9f5cb;

// Helper to convert RAM address to StateVarId
static std::optional<enginemon::StateVarId> ram_to_statevar(uint16_t ram_address) {
    switch (ram_address) {
        case RAM_FARFETCHD_POSITION:
            return static_cast<enginemon::StateVarId>(
                static_cast<uint16_t>(enginemon::WellKnownStateVar::FarfetchdPosition));
        case RAM_MOO_MOO_BERRIES:
            return static_cast<enginemon::StateVarId>(
                static_cast<uint16_t>(enginemon::WellKnownStateVar::MooMooBerries));
        case RAM_UNDERGROUND_SWITCHES:
            return static_cast<enginemon::StateVarId>(
                static_cast<uint16_t>(enginemon::WellKnownStateVar::UndergroundSwitchPositions));
        case RAM_BATTLE_TOWER_BEATEN_TRAINERS:
            return static_cast<enginemon::StateVarId>(
                static_cast<uint16_t>(enginemon::WellKnownStateVar::BattleTowerBeatenTrainers));
        default:
            return std::nullopt;
    }
}

RuleResult rule_ram_operations(LoweringContext& ctx) {
    const auto* cmd = ctx.peek();
    if (!cmd) return {};
    
    // readmem: Read byte from RAM address into wScriptVar
    // Reference: pokecrystal/engine/overworld/scripting.asm Script_readmem
    // Semantics: loads state value into wScriptVar for conditional checks
    if (auto* p = std::get_if<Cmd_Readmem>(&cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        
        // Check for wOtherPlayerLinkMode - special case: capability query
        if (p->ram_address == RAM_LINK_MODE) {
            // Link mode is a capability query, not mutable state
            // Sets wScriptVar to 0 (Gen1/not connected) or non-zero (Gen2)
            r.instructions.push_back(make_inst(enginemon::Sem_CheckLinkMode{}));
            // Batch 9: Invalidate - result is runtime-dependent
            ctx.block_ctx.invalidate();
            return r;
        }
        
        // Check for wTempWildMonSpecies - field encounter context read
        // This RAM access is eliminated through semantic context
        if (p->ram_address == RAM_TEMP_WILD_MON_SPECIES) {
            // Maps to Sem_ReadEncounterSpecies which reads from PendingFieldEncounter
            r.instructions.push_back(make_inst(enginemon::Sem_ReadEncounterSpecies{}));
            // Batch 9: Invalidate - result is runtime-dependent
            ctx.block_ctx.invalidate();
            return r;
        }
        
        // Check for wStrengthSpecies - field actor context read for cry
        // This is NOT accessed by scripts directly in the corpus, but if it were,
        // it would map to Sem_PlayFieldActorCry context read
        if (p->ram_address == RAM_STRENGTH_SPECIES) {
            // Maps to context read - species from SelectedFieldActor
            // Note: In practice, scripts use "cry" command after SetStrengthFlag
            // which internally reads wStrengthSpecies, not via script readmem
            r.instructions.push_back(make_inst(enginemon::Sem_ReadEncounterSpecies{}));
            // Batch 9: Invalidate - result is runtime-dependent
            ctx.block_ctx.invalidate();
            return r;
        }
        
        // Check for known state variables
        if (auto state_var = ram_to_statevar(p->ram_address)) {
            enginemon::Sem_ReadStateVar op;
            op.state_var = *state_var;
            r.instructions.push_back(make_inst(std::move(op)));
            // Batch 9: Invalidate - readmem always writes to wScriptVar
            ctx.block_ctx.invalidate();
            return r;
        }
        
        // Unknown RAM address - don't match, let it fall through to unlowered
        return {};
    }
    
    // writemem: Write wScriptVar to RAM address
    // Reference: pokecrystal/engine/overworld/scripting.asm Script_writemem
    // Semantics: stores wScriptVar value to state
    if (auto* p = std::get_if<Cmd_Writemem>(&cmd->data)) {
        // Check for known state variables
        if (auto state_var = ram_to_statevar(p->ram_address)) {
            RuleResult r;
            r.matched = true;
            r.consumed = 1;
            enginemon::Sem_WriteStateVar op;
            op.state_var = *state_var;
            r.instructions.push_back(make_inst(std::move(op)));
            return r;
        }
        
        // Unknown RAM address - don't match
        return {};
    }
    
    // loadmem: Write immediate value to RAM address
    // Reference: pokecrystal/engine/overworld/scripting.asm Script_loadmem
    // Semantics: sets state to specific value
    if (auto* p = std::get_if<Cmd_Loadmem>(&cmd->data)) {
        // Check for known state variables
        if (auto state_var = ram_to_statevar(p->ram_address)) {
            RuleResult r;
            r.matched = true;
            r.consumed = 1;
            enginemon::Sem_SetStateVar op;
            op.state_var = *state_var;
            op.value = p->value;
            r.instructions.push_back(make_inst(std::move(op)));
            return r;
        }
        
        // Unknown RAM address - don't match
        return {};
    }
    
    return {};
}

// =============================================================================
// FIELD MOVE NATIVE CALL LOWERING
// =============================================================================
// These rules lower callasm instructions for the 5 field-move natives encountered
// in the corpus (all from StdScripts 14 and 15):
//
//   TryStrengthOW (0xCD78) - Check Strength capability
//   SetStrengthFlag (0xCD12) - Activate Strength
//   HasRockSmash (0xCF7C) - Check Rock Smash capability
//   GetPartyNickname (0xC706) - Get nickname for text display
//   RockMonEncounter (0xB8219) - Attempt rock smash wild encounter
//
// The registry contains 7 additional natives that are NEVER encountered:
//   Random, HealParty×2, Enable/DisableWild, EnableEvents, HealPartyPredef
// These remain as registry metadata only with no lowering rules.

RuleResult rule_callasm_field_moves(LoweringContext& ctx) {
    const auto* cmd = ctx.peek();
    if (!cmd) return {};
    
    // Only handle callasm commands
    auto* callasm = std::get_if<Cmd_Callasm>(&cmd->data);
    if (!callasm) return {};
    
    uint32_t addr = callasm->flat_address;
    
    // TryStrengthOW - Check if Strength can be used overworld
    // Returns: 0=Available, 1=Unavailable, 2=AlreadyActive
    // Establishes SelectedFieldActor context on Available
    if (addr == NATIVE_TryStrengthOW) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        r.instructions.push_back(make_inst(enginemon::Sem_CheckStrengthCapability{}));
        return r;
    }
    
    // SetStrengthFlag - Activate Strength after player confirmation
    // Consumes SelectedFieldActor context, sets strength active flag
    if (addr == NATIVE_SetStrengthFlag) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        r.instructions.push_back(make_inst(enginemon::Sem_ActivateStrength{}));
        return r;
    }
    
    // HasRockSmash - Check if party has Rock Smash
    // Returns: 0=Available, 1=Unavailable
    // Establishes SelectedFieldActor context on Available
    if (addr == NATIVE_HasRockSmash) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        r.instructions.push_back(make_inst(enginemon::Sem_CheckRockSmashCapability{}));
        return r;
    }
    
    // GetPartyNickname - Get nickname of party member for text display
    // Uses SelectedFieldActor.party_slot (set by capability check)
    // In field-move context, this prepares the actor's nickname
    if (addr == NATIVE_GetPartyNickname) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        enginemon::Sem_PrepareFieldMoveNickname op;
        op.buffer_slot = 0;  // Default text buffer
        r.instructions.push_back(make_inst(std::move(op)));
        return r;
    }
    
    // RockMonEncounter - Attempt wild encounter from rock smash (40% chance)
    // On success: establishes PendingFieldEncounter context
    // Sets wScriptVar to species ID (0 if no encounter)
    if (addr == NATIVE_RockMonEncounter) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        r.instructions.push_back(make_inst(enginemon::Sem_TryRockSmashEncounter{}));
        return r;
    }
    
    // =========================================================================
    // Battle Tower native calls (corpus-discovered deferred scripts)
    // =========================================================================
    
    // BattleTowerHallwayChooseBattleRoomScript.asm_load_battle_room - Read level group
    // Reference: pokecrystal/maps/BattleTowerHallway.asm
    // Reads wBTChoiceOfLvlGroup from SRAM and stores in wScriptVar
    // Used to determine which battle room corridor to walk the player to
    if (addr == NATIVE_BattleTowerLoadLevelGroup) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        // Lower to read of Battle Tower level group state variable into wScriptVar
        enginemon::Sem_ReadStateVar op;
        op.state_var = static_cast<enginemon::StateVarId>(
            static_cast<uint16_t>(enginemon::WellKnownStateVar::BattleTowerLevelGroup));
        r.instructions.push_back(make_inst(std::move(op)));
        return r;
    }
    
    // Not a field-move native - don't match
    return {};
}

// --- UI/Text Rules ---

RuleResult rule_open_text(LoweringContext& ctx) {
    const auto* cmd = ctx.peek();
    if (!cmd) return {};
    
    if (std::holds_alternative<Cmd_Opentext>(cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        r.instructions.push_back(make_inst(enginemon::Sem_OpenText{}));
        return r;
    }
    return {};
}


RuleResult rule_close_text(LoweringContext& ctx) {
    const auto* cmd = ctx.peek();
    if (!cmd) return {};
    
    if (std::holds_alternative<Cmd_Closetext>(cmd->data) ||
        std::holds_alternative<Cmd_Closewindow>(cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        r.instructions.push_back(make_inst(enginemon::Sem_CloseText{}));
        return r;
    }
    return {};
}

RuleResult rule_write_text(LoweringContext& ctx) {
    const auto* cmd = ctx.peek();
    if (!cmd) return {};
    
    // writetext and farwritetext - shows text in current textbox
    // For Stage 4, we mark these as unlowered because we need ROM access
    // to decode the text pointer. The runtime pipeline handles this.
    // In the future, the compiler would pre-decode and embed text.
    
    if (std::holds_alternative<Cmd_Writetext>(cmd->data) ||
        std::holds_alternative<Cmd_Farwritetext>(cmd->data)) {
        // Emit as ShowText with empty sequence (text not resolved at lowering time)
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        enginemon::Sem_ShowText op;
        // Text would be resolved from package at runtime
        r.instructions.push_back(make_inst(std::move(op)));
        return r;
    }
    return {};
}


RuleResult rule_jump_text(LoweringContext& ctx) {
    const auto* cmd = ctx.peek();
    if (!cmd) return {};
    
    if (std::holds_alternative<Cmd_Jumptext>(cmd->data) ||
        std::holds_alternative<Cmd_Farjumptext>(cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        enginemon::Sem_ShowTextAndEnd op;
        r.instructions.push_back(make_inst(std::move(op)));
        return r;
    }
    return {};
}

RuleResult rule_jump_text_face_player(LoweringContext& ctx) {
    const auto* cmd = ctx.peek();
    if (!cmd) return {};
    
    if (std::holds_alternative<Cmd_Jumptextfaceplayer>(cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        enginemon::Sem_FacePlayerAndShowText op;
        r.instructions.push_back(make_inst(std::move(op)));
        return r;
    }
    return {};
}

RuleResult rule_wait_button(LoweringContext& ctx) {
    const auto* cmd = ctx.peek();
    if (!cmd) return {};
    
    if (std::holds_alternative<Cmd_Waitbutton>(cmd->data) ||
        std::holds_alternative<Cmd_Promptbutton>(cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        r.instructions.push_back(make_inst(enginemon::Sem_WaitButton{}));
        return r;
    }
    return {};
}


RuleResult rule_yes_no(LoweringContext& ctx) {
    const auto* cmd = ctx.peek();
    if (!cmd) return {};
    
    if (std::holds_alternative<Cmd_Yesorno>(cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        r.instructions.push_back(make_inst(enginemon::Sem_YesNo{}));
        return r;
    }
    return {};
}

// --- Inventory Rules ---

RuleResult rule_give_item(LoweringContext& ctx) {
    const auto* cmd = ctx.peek();
    if (!cmd) return {};
    
    if (auto* p = std::get_if<Cmd_Giveitem>(&cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        enginemon::Sem_GiveItem op;
        op.item = enginemon::ItemId{p->item};
        op.quantity = p->quantity;
        r.instructions.push_back(make_inst(std::move(op)));
        return r;
    }
    return {};
}

RuleResult rule_take_item(LoweringContext& ctx) {
    const auto* cmd = ctx.peek();
    if (!cmd) return {};
    
    if (auto* p = std::get_if<Cmd_Takeitem>(&cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        enginemon::Sem_TakeItem op;
        op.item = enginemon::ItemId{p->item};
        op.quantity = p->quantity;
        r.instructions.push_back(make_inst(std::move(op)));
        return r;
    }
    return {};
}


RuleResult rule_check_item(LoweringContext& ctx) {
    const auto* cmd = ctx.peek();
    if (!cmd) return {};
    
    if (auto* p = std::get_if<Cmd_Checkitem>(&cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        enginemon::Sem_CheckItem op;
        op.item = enginemon::ItemId{p->item};
        r.instructions.push_back(make_inst(std::move(op)));
        return r;
    }
    return {};
}

RuleResult rule_verbose_give_item(LoweringContext& ctx) {
    const auto* cmd = ctx.peek();
    if (!cmd) return {};
    
    if (auto* p = std::get_if<Cmd_Verbosegiveitem>(&cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        enginemon::Sem_GiveItemVerbose op;
        op.item = enginemon::ItemId{p->item};
        op.quantity = p->quantity;
        r.instructions.push_back(make_inst(std::move(op)));
        return r;
    }
    return {};
}

RuleResult rule_money_ops(LoweringContext& ctx) {
    const auto* cmd = ctx.peek();
    if (!cmd) return {};
    
    if (auto* p = std::get_if<Cmd_Givemoney>(&cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        enginemon::Sem_GiveMoney op;
        op.amount = p->amount();
        op.account = p->account;
        r.instructions.push_back(make_inst(std::move(op)));
        return r;
    }
    if (auto* p = std::get_if<Cmd_Takemoney>(&cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        enginemon::Sem_TakeMoney op;
        op.amount = p->amount();
        op.account = p->account;
        r.instructions.push_back(make_inst(std::move(op)));
        return r;
    }
    if (auto* p = std::get_if<Cmd_Checkmoney>(&cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        enginemon::Sem_CheckMoney op;
        op.amount = p->amount();
        op.account = p->account;
        r.instructions.push_back(make_inst(std::move(op)));
        return r;
    }
    return {};
}


RuleResult rule_coin_ops(LoweringContext& ctx) {
    const auto* cmd = ctx.peek();
    if (!cmd) return {};
    
    if (auto* p = std::get_if<Cmd_Givecoins>(&cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        enginemon::Sem_GiveCoins op;
        op.coins = p->coins;
        r.instructions.push_back(make_inst(std::move(op)));
        return r;
    }
    if (auto* p = std::get_if<Cmd_Takecoins>(&cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        enginemon::Sem_TakeCoins op;
        op.coins = p->coins;
        r.instructions.push_back(make_inst(std::move(op)));
        return r;
    }
    if (auto* p = std::get_if<Cmd_Checkcoins>(&cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        enginemon::Sem_CheckCoins op;
        op.coins = p->coins;
        r.instructions.push_back(make_inst(std::move(op)));
        return r;
    }
    return {};
}

// --- Party/Pokemon Rules ---

RuleResult rule_give_pokemon(LoweringContext& ctx) {
    const auto* cmd = ctx.peek();
    if (!cmd) return {};
    
    if (auto* p = std::get_if<Cmd_Givepoke>(&cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        enginemon::Sem_GivePokemon op;
        op.species = enginemon::SpeciesId{p->pokemon};
        op.level = p->level;
        op.held_item = enginemon::ItemId{p->item};
        op.has_nickname = p->has_extra_data;
        r.instructions.push_back(make_inst(std::move(op)));
        return r;
    }
    return {};
}


RuleResult rule_give_egg(LoweringContext& ctx) {
    const auto* cmd = ctx.peek();
    if (!cmd) return {};
    
    if (auto* p = std::get_if<Cmd_Giveegg>(&cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        enginemon::Sem_GiveEgg op;
        op.species = enginemon::SpeciesId{p->pokemon};
        op.level = p->level;
        r.instructions.push_back(make_inst(std::move(op)));
        return r;
    }
    return {};
}

RuleResult rule_check_pokemon(LoweringContext& ctx) {
    const auto* cmd = ctx.peek();
    if (!cmd) return {};
    
    if (auto* p = std::get_if<Cmd_Checkpoke>(&cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        enginemon::Sem_CheckPokemon op;
        op.species = enginemon::SpeciesId{p->pokemon};
        r.instructions.push_back(make_inst(std::move(op)));
        return r;
    }
    return {};
}

// --- Movement/Object Rules ---

RuleResult rule_apply_movement(LoweringContext& ctx) {
    const auto* cmd = ctx.peek();
    if (!cmd) return {};
    
    // applymovement - apply pre-parsed movement sequence to object
    // Movement data is decoded at the typed decoder stage, preserving full semantics
    if (auto* p = std::get_if<Cmd_Applymovement>(&cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        enginemon::Sem_ApplyMovement op;
        // Use typed MovementTarget instead of raw object_id
        if (p->object_id == 0) {
            op.target = enginemon::MovementTarget::player();
        } else {
            op.target = enginemon::MovementTarget::object(p->object_id);
        }
        op.commands = p->commands;  // Preserves full movement sequence
        r.instructions.push_back(make_inst(std::move(op)));
        return r;
    }
    if (auto* p = std::get_if<Cmd_Applymovementlasttalked>(&cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        enginemon::Sem_ApplyMovement op;
        // Use typed MovementTarget::last_talked() instead of 0xFF sentinel
        op.target = enginemon::MovementTarget::last_talked();
        op.commands = p->commands;  // Preserves full movement sequence
        r.instructions.push_back(make_inst(std::move(op)));
        return r;
    }
    return {};
}


RuleResult rule_face_player(LoweringContext& ctx) {
    const auto* cmd = ctx.peek();
    if (!cmd) return {};
    
    if (std::holds_alternative<Cmd_Faceplayer>(cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        enginemon::Sem_FacePlayer op;
        op.object_id = 0;  // Self
        r.instructions.push_back(make_inst(std::move(op)));
        return r;
    }
    return {};
}

RuleResult rule_face_object(LoweringContext& ctx) {
    const auto* cmd = ctx.peek();
    if (!cmd) return {};
    
    if (auto* p = std::get_if<Cmd_Faceobject>(&cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        enginemon::Sem_FaceObject op;
        op.object1 = p->object1;
        op.object2 = p->object2;
        r.instructions.push_back(make_inst(std::move(op)));
        return r;
    }
    return {};
}

RuleResult rule_turn_object(LoweringContext& ctx) {
    const auto* cmd = ctx.peek();
    if (!cmd) return {};
    
    if (auto* p = std::get_if<Cmd_Turnobject>(&cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        enginemon::Sem_TurnObject op;
        op.object_id = p->object_id;
        op.facing = static_cast<enginemon::Direction>(p->facing);
        r.instructions.push_back(make_inst(std::move(op)));
        return r;
    }
    return {};
}


RuleResult rule_object_visibility(LoweringContext& ctx) {
    const auto* cmd = ctx.peek();
    if (!cmd) return {};
    
    if (auto* p = std::get_if<Cmd_Appear>(&cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        enginemon::Sem_ShowObject op;
        op.object_id = p->object_id;
        r.instructions.push_back(make_inst(std::move(op)));
        return r;
    }
    if (auto* p = std::get_if<Cmd_Disappear>(&cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        enginemon::Sem_HideObject op;
        op.object_id = p->object_id;
        r.instructions.push_back(make_inst(std::move(op)));
        return r;
    }
    return {};
}

RuleResult rule_move_object(LoweringContext& ctx) {
    const auto* cmd = ctx.peek();
    if (!cmd) return {};
    
    if (auto* p = std::get_if<Cmd_Moveobject>(&cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        enginemon::Sem_MoveObject op;
        op.object_id = p->object_id;
        op.x = p->x;
        op.y = p->y;
        r.instructions.push_back(make_inst(std::move(op)));
        return r;
    }
    return {};
}

RuleResult rule_set_last_talked(LoweringContext& ctx) {
    const auto* cmd = ctx.peek();
    if (!cmd) return {};
    
    if (auto* p = std::get_if<Cmd_Setlasttalked>(&cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        enginemon::Sem_SetLastTalked op;
        op.object_id = p->object_id;
        r.instructions.push_back(make_inst(std::move(op)));
        return r;
    }
    return {};
}


RuleResult rule_emote(LoweringContext& ctx) {
    const auto* cmd = ctx.peek();
    if (!cmd) return {};
    
    if (auto* p = std::get_if<Cmd_Showemote>(&cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        enginemon::Sem_Emote op;
        op.emote_id = p->bubble;
        op.object_id = p->object_id;
        op.duration = p->time;
        r.instructions.push_back(make_inst(std::move(op)));
        return r;
    }
    // loademote is a prerequisite for showemote - it loads the emote graphics
    // The bubble parameter is redundant with showemote's bubble parameter
    // showemote captures the complete semantic behavior
    if (std::holds_alternative<Cmd_Loademote>(cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        r.absorbed_opcodes.push_back(cmd->opcode());  // Track absorbed opcode
        // Effect captured by subsequent Sem_Emote (showemote handles everything)
        return r;
    }
    return {};
}

// --- Map/Warp/Scene Rules ---

RuleResult rule_warp(LoweringContext& ctx) {
    const auto* cmd = ctx.peek();
    if (!cmd) return {};
    
    if (auto* p = std::get_if<Cmd_Warp>(&cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        
        // Crystal BADWARP sentinel: when map_group == 0, the warp uses
        // MAPSETUP_BADWARP to restore player to wBackupMap location.
        // This is NOT a MapId reference - it's a semantic "warp to backup" operation.
        // Reference: pokecrystal/engine/overworld/scripting.asm Script_warp .not_ok
        if (p->map.group == 0) {
            enginemon::Sem_WarpToBackup op;
            op.x = p->x;
            op.y = p->y;
            r.instructions.push_back(make_inst(std::move(op)));
        } else {
            enginemon::Sem_Warp op;
            op.map = enginemon::MapId{p->map.as_u16()};
            op.x = p->x;
            op.y = p->y;
            r.instructions.push_back(make_inst(std::move(op)));
        }
        return r;
    }
    if (auto* p = std::get_if<Cmd_Warpfacing>(&cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        
        // Same BADWARP sentinel check for warpfacing
        if (p->map.group == 0) {
            enginemon::Sem_WarpToBackupFacing op;
            op.facing = static_cast<enginemon::Direction>(p->facing);
            op.x = p->x;
            op.y = p->y;
            r.instructions.push_back(make_inst(std::move(op)));
        } else {
            enginemon::Sem_WarpFacing op;
            op.facing = static_cast<enginemon::Direction>(p->facing);
            op.map = enginemon::MapId{p->map.as_u16()};
            op.x = p->x;
            op.y = p->y;
            r.instructions.push_back(make_inst(std::move(op)));
        }
        return r;
    }
    return {};
}


RuleResult rule_scene_ops(LoweringContext& ctx) {
    const auto* cmd = ctx.peek();
    if (!cmd) return {};
    
    if (auto* p = std::get_if<Cmd_Setscene>(&cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        enginemon::Sem_SetScene op;
        op.scene = p->scene;
        r.instructions.push_back(make_inst(std::move(op)));
        return r;
    }
    if (std::holds_alternative<Cmd_Checkscene>(cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        r.instructions.push_back(make_inst(enginemon::Sem_CheckScene{}));
        
        // Batch 9: Invalidate context - checkscene writes to wScriptVar
        ctx.block_ctx.invalidate();
        
        return r;
    }
    if (auto* p = std::get_if<Cmd_Setmapscene>(&cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        enginemon::Sem_SetMapScene op;
        op.map = enginemon::MapId{p->map.as_u16()};
        op.scene = p->scene;
        r.instructions.push_back(make_inst(std::move(op)));
        return r;
    }
    if (auto* p = std::get_if<Cmd_Checkmapscene>(&cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        enginemon::Sem_CheckMapScene op;
        op.map = enginemon::MapId{p->map.as_u16()};
        r.instructions.push_back(make_inst(std::move(op)));
        
        // Batch 9: Invalidate context - checkmapscene writes to wScriptVar
        ctx.block_ctx.invalidate();
        
        return r;
    }
    return {};
}


RuleResult rule_map_ops(LoweringContext& ctx) {
    const auto* cmd = ctx.peek();
    if (!cmd) return {};
    
    if (auto* p = std::get_if<Cmd_Warpmod>(&cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        enginemon::Sem_ModifyWarp op;
        op.warp_id = p->warp_id;
        op.target_map = enginemon::MapId{p->map.as_u16()};
        r.instructions.push_back(make_inst(std::move(op)));
        return r;
    }
    if (auto* p = std::get_if<Cmd_Blackoutmod>(&cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        enginemon::Sem_SetBlackoutPoint op;
        op.map = enginemon::MapId{p->map.as_u16()};
        r.instructions.push_back(make_inst(std::move(op)));
        return r;
    }
    if (std::holds_alternative<Cmd_Reloadmap>(cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        r.instructions.push_back(make_inst(enginemon::Sem_ReloadMap{}));
        return r;
    }
    if (std::holds_alternative<Cmd_Refreshmap>(cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        r.instructions.push_back(make_inst(enginemon::Sem_RefreshMap{}));
        return r;
    }
    if (auto* p = std::get_if<Cmd_Changeblock>(&cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        enginemon::Sem_ChangeBlock op;
        op.x = p->x;
        op.y = p->y;
        op.block = p->block;
        r.instructions.push_back(make_inst(std::move(op)));
        return r;
    }
    // writecmdqueue (0x7D): Register map command queue for puzzle behavior
    // Reference: pokecrystal/engine/overworld/cmd_queue.asm
    // Used by ice sliding puzzles, boulder puzzles (stonetable)
    if (auto* p = std::get_if<Cmd_Writecmdqueue>(&cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        enginemon::Sem_WriteCmdQueue op;
        op.queue_pointer = p->queue_pointer;
        r.instructions.push_back(make_inst(std::move(op)));
        return r;
    }
    // delcmdqueue (0x7E): Remove command queue entry by type
    if (auto* p = std::get_if<Cmd_Delcmdqueue>(&cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        enginemon::Sem_DeleteCmdQueue op;
        op.queue_type = p->byte;
        r.instructions.push_back(make_inst(std::move(op)));
        return r;
    }
    return {};
}


// --- Battle Rules ---

RuleResult rule_load_wild_mon(LoweringContext& ctx) {
    const auto* cmd = ctx.peek();
    if (!cmd) return {};
    
    if (auto* p = std::get_if<Cmd_Loadwildmon>(&cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        enginemon::Sem_LoadWildMon op;
        op.species = enginemon::SpeciesId{p->pokemon};
        op.level = p->level;
        r.instructions.push_back(make_inst(std::move(op)));
        return r;
    }
    return {};
}

// randomwildmon (0x5B): Load pending wild encounter from context
// Reference: pokecrystal/engine/overworld/scripting.asm Script_randomwildmon
// Semantics: Sets wBattleScriptFlags to 0 (wild battle mode) - uses species/level
//            already established by RockMonEncounter, TreeMonEncounter, etc.
//            The "random" means "use whatever was pre-established", not RNG.
// Usage pattern in RockSmashScript / HeadbuttScript:
//   callasm RockMonEncounter  → Sem_TryRockSmashEncounter (establishes PendingFieldEncounter)
//   readmem wTempWildMonSpecies → Sem_ReadEncounterSpecies (reads species for branch)
//   iffalse .done             (skip battle if no encounter)
//   randomwildmon             → Sem_LoadPendingEncounter (consumes pending encounter)
//   startbattle
//   reloadmapafterbattle
//
// Semantic lowering: Explicit Sem_LoadPendingEncounter operation
// NOT Sem_LoadWildMon with sentinel values - that would confuse "real" species 0 with "use context"
RuleResult rule_random_wild_mon(LoweringContext& ctx) {
    const auto* cmd = ctx.peek();
    if (!cmd) return {};
    
    if (std::holds_alternative<Cmd_Randomwildmon>(cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        // Use explicit Sem_LoadPendingEncounter - no magic sentinel values
        // Runtime consumes ctx.pending_field_encounter (species + level)
        r.instructions.push_back(make_inst(enginemon::Sem_LoadPendingEncounter{}));
        return r;
    }
    return {};
}

RuleResult rule_load_trainer(LoweringContext& ctx) {
    const auto* cmd = ctx.peek();
    if (!cmd) return {};
    
    if (auto* p = std::get_if<Cmd_Loadtrainer>(&cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        enginemon::Sem_LoadTrainer op;
        op.trainer_group = p->trainer_group;
        op.trainer_id = p->trainer_id;
        r.instructions.push_back(make_inst(std::move(op)));
        return r;
    }
    return {};
}

RuleResult rule_start_battle(LoweringContext& ctx) {
    const auto* cmd = ctx.peek();
    if (!cmd) return {};
    
    if (std::holds_alternative<Cmd_Startbattle>(cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        r.instructions.push_back(make_inst(enginemon::Sem_StartBattle{}));
        return r;
    }
    return {};
}


RuleResult rule_battle_aftermath(LoweringContext& ctx) {
    const auto* cmd = ctx.peek();
    if (!cmd) return {};
    
    if (std::holds_alternative<Cmd_Reloadmapafterbattle>(cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        r.instructions.push_back(make_inst(enginemon::Sem_ReloadMapAfterBattle{}));
        return r;
    }
    return {};
}

RuleResult rule_trainer_script_ops(LoweringContext& ctx) {
    const auto* cmd = ctx.peek();
    if (!cmd) return {};
    
    if (auto* p = std::get_if<Cmd_Trainertext>(&cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        enginemon::Sem_TrainerText op;
        op.domain = enginemon::TrainerTextDomain::Normal;
        op.text_id = p->text_id;
        r.instructions.push_back(make_inst(std::move(op)));
        return r;
    }
    if (auto* p = std::get_if<Cmd_Trainerflagaction>(&cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        enginemon::Sem_TrainerFlagAction op;
        op.action = p->action;
        r.instructions.push_back(make_inst(std::move(op)));
        return r;
    }
    if (std::holds_alternative<Cmd_Checkjustbattled>(cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        r.instructions.push_back(make_inst(enginemon::Sem_CheckJustBattled{}));
        return r;
    }
    if (std::holds_alternative<Cmd_Endifjustbattled>(cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        r.instructions.push_back(make_inst(enginemon::Sem_EndIfJustBattled{}));
        return r;
    }
    return {};
}


// =============================================================================
// BATTLE TOWER TEXT RULE
// =============================================================================
// battletowertext (0xa4) - Display Battle Tower specific text
// Reference: pokecrystal/engine/overworld/scripting.asm Script_battletowertext
//            pokecrystal/engine/events/battle_tower/trainer_text.asm BattleTowerText
//
// Source-proven semantics:
//   - Calls SetUpTextbox (opens text UI)
//   - bttext_id operand: 1=Intro, 2=PlayerLost, 3=PlayerWon
//   - Reads wBT_OTTrainerClass to determine trainer gender
//   - For bttext_id=1: generates random text index, stores in wBT_TrainerTextIndex
//   - For bttext_id=2,3: reuses stored index for consistency
//   - Displays from BTMaleTrainerTexts (25 variants) or BTFemaleTrainerTexts (15 variants)
//
// Semantic lowering:
//   Maps to Sem_TrainerText with domain=BattleTower.
//   This is NOT a Sem_Special escape hatch - battletowertext has well-defined semantics
//   that parallel normal trainertext (both are parameterized trainer dialogue).
RuleResult rule_battle_tower_text(LoweringContext& ctx) {
    const auto* cmd = ctx.peek();
    if (!cmd) return {};
    
    if (auto* p = std::get_if<Cmd_Battletowertext>(&cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        // Lower to Sem_TrainerText with BattleTower domain
        // bttext_id: 1=Intro, 2=PlayerLost, 3=PlayerWon
        enginemon::Sem_TrainerText op;
        op.domain = enginemon::TrainerTextDomain::BattleTower;
        op.text_id = p->bttext_id;
        r.instructions.push_back(make_inst(std::move(op)));
        return r;
    }
    return {};
}


// --- Audio Rules ---

RuleResult rule_play_music(LoweringContext& ctx) {
    const auto* cmd = ctx.peek();
    if (!cmd) return {};
    
    if (auto* p = std::get_if<Cmd_Playmusic>(&cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        enginemon::Sem_PlayMusic op;
        op.music = enginemon::MusicId{p->music};
        r.instructions.push_back(make_inst(std::move(op)));
        return r;
    }
    return {};
}

RuleResult rule_play_sound(LoweringContext& ctx) {
    const auto* cmd = ctx.peek();
    if (!cmd) return {};
    
    if (auto* p = std::get_if<Cmd_Playsound>(&cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        enginemon::Sem_PlaySound op;
        op.sound = enginemon::SfxId{p->sound};
        r.instructions.push_back(make_inst(std::move(op)));
        return r;
    }
    return {};
}

RuleResult rule_play_cry(LoweringContext& ctx) {
    const auto* cmd = ctx.peek();
    if (!cmd) return {};
    
    if (auto* p = std::get_if<Cmd_Cry>(&cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        enginemon::Sem_PlayCry op;
        op.species = enginemon::SpeciesId{static_cast<uint8_t>(p->cry_id & 0xFF)};
        r.instructions.push_back(make_inst(std::move(op)));
        return r;
    }
    return {};
}


RuleResult rule_audio_control(LoweringContext& ctx) {
    const auto* cmd = ctx.peek();
    if (!cmd) return {};
    
    if (std::holds_alternative<Cmd_Waitsfx>(cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        r.instructions.push_back(make_inst(enginemon::Sem_WaitSound{}));
        return r;
    }
    if (auto* p = std::get_if<Cmd_Musicfadeout>(&cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        enginemon::Sem_FadeOutMusic op;
        op.music = enginemon::MusicId{p->music};
        op.fade_time = p->fadetime;
        r.instructions.push_back(make_inst(std::move(op)));
        return r;
    }
    if (std::holds_alternative<Cmd_Playmapmusic>(cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        r.instructions.push_back(make_inst(enginemon::Sem_PlayMapMusic{}));
        return r;
    }
    if (std::holds_alternative<Cmd_Warpsound>(cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        r.instructions.push_back(make_inst(enginemon::Sem_WarpSound{}));
        return r;
    }
    if (std::holds_alternative<Cmd_Specialsound>(cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        r.instructions.push_back(make_inst(enginemon::Sem_SpecialSound{}));
        return r;
    }
    return {};
}


// --- Time/Wait Rules ---

RuleResult rule_wait(LoweringContext& ctx) {
    const auto* cmd = ctx.peek();
    if (!cmd) return {};
    
    if (auto* p = std::get_if<Cmd_Wait>(&cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        enginemon::Sem_Wait op;
        op.duration = p->duration;
        r.instructions.push_back(make_inst(std::move(op)));
        return r;
    }
    return {};
}

RuleResult rule_pause(LoweringContext& ctx) {
    const auto* cmd = ctx.peek();
    if (!cmd) return {};
    
    if (auto* p = std::get_if<Cmd_Pause>(&cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        enginemon::Sem_Pause op;
        op.length = p->length;
        r.instructions.push_back(make_inst(std::move(op)));
        return r;
    }
    return {};
}

RuleResult rule_check_time(LoweringContext& ctx) {
    const auto* cmd = ctx.peek();
    if (!cmd) return {};
    
    if (auto* p = std::get_if<Cmd_Checktime>(&cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        enginemon::Sem_CheckTime op;
        op.time_flags = p->time;
        r.instructions.push_back(make_inst(std::move(op)));
        return r;
    }
    return {};
}


// --- Phone Rules ---

RuleResult rule_phone_ops(LoweringContext& ctx) {
    const auto* cmd = ctx.peek();
    if (!cmd) return {};
    
    if (auto* p = std::get_if<Cmd_Addcellnum>(&cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        enginemon::Sem_AddPhoneNumber op;
        op.person = p->person;
        r.instructions.push_back(make_inst(std::move(op)));
        return r;
    }
    if (auto* p = std::get_if<Cmd_Delcellnum>(&cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        enginemon::Sem_DeletePhoneNumber op;
        op.person = p->person;
        r.instructions.push_back(make_inst(std::move(op)));
        return r;
    }
    if (auto* p = std::get_if<Cmd_Checkcellnum>(&cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        enginemon::Sem_CheckPhoneNumber op;
        op.person = p->person;
        r.instructions.push_back(make_inst(std::move(op)));
        return r;
    }
    return {};
}

// checkphonecall (0x9D): Check if a phone call is pending
// Reference: pokecrystal/engine/overworld/scripting.asm Script_checkphonecall
// Semantics: Checks wSpecialPhoneCallID, sets wScriptVar to TRUE (1) if pending, FALSE (0) otherwise
// This is a read-only query for phone call pending status - generic phone system capability
RuleResult rule_check_phone_call(LoweringContext& ctx) {
    const auto* cmd = ctx.peek();
    if (!cmd) return {};
    
    if (std::holds_alternative<Cmd_Checkphonecall>(cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        r.instructions.push_back(make_inst(enginemon::Sem_CheckPhoneCall{}));
        return r;
    }
    return {};
}

// --- Visual Effects Rules ---

RuleResult rule_earthquake(LoweringContext& ctx) {
    const auto* cmd = ctx.peek();
    if (!cmd) return {};
    
    if (auto* p = std::get_if<Cmd_Earthquake>(&cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        enginemon::Sem_Earthquake op;
        op.param = p->param;
        r.instructions.push_back(make_inst(std::move(op)));
        return r;
    }
    return {};
}


// --- Misc Rules ---

RuleResult rule_wild_toggle(LoweringContext& ctx) {
    const auto* cmd = ctx.peek();
    if (!cmd) return {};
    
    if (std::holds_alternative<Cmd_Wildon>(cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        r.instructions.push_back(make_inst(enginemon::Sem_WildOn{}));
        return r;
    }
    if (std::holds_alternative<Cmd_Wildoff>(cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        r.instructions.push_back(make_inst(enginemon::Sem_WildOff{}));
        return r;
    }
    return {};
}

RuleResult rule_special(LoweringContext& ctx) {
    const auto* cmd = ctx.peek();
    if (!cmd) return {};
    
    if (auto* p = std::get_if<Cmd_Special>(&cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        
        // Special ID constants (from pokecrystal special_pointers.asm)
        // Screen fades
        constexpr uint16_t SPECIAL_FADE_OUT_TO_WHITE = 46;
        constexpr uint16_t SPECIAL_FADE_OUT_TO_BLACK = 48;
        constexpr uint16_t SPECIAL_FADE_IN_FROM_WHITE = 49;
        constexpr uint16_t SPECIAL_FADE_IN_FROM_BLACK = 50;
        // Palette/renderer sync
        constexpr uint16_t SPECIAL_RELOAD_SPRITES_NO_PALETTES = 51;
        constexpr uint16_t SPECIAL_CLEAR_BG_PALETTES = 52;
        // Sprite operations
        constexpr uint16_t SPECIAL_UPDATE_PLAYER_SPRITE = 56;
        constexpr uint16_t SPECIAL_LOAD_USED_SPRITES_GFX = 94;
        constexpr uint16_t SPECIAL_REFRESH_SPRITES = 158;
        constexpr uint16_t SPECIAL_LOAD_MAP_PALETTES = 164;
        // Audio
        constexpr uint16_t SPECIAL_WAIT_SFX = 59;       // Batch 2: WaitSFX
        constexpr uint16_t SPECIAL_PLAY_MAP_MUSIC = 60; // Batch 2: PlayMapMusic
        constexpr uint16_t SPECIAL_RESTART_MAP_MUSIC = 61;
        constexpr uint16_t SPECIAL_FADE_OUT_MUSIC = 106;
        // Unclassified in Batch 1 (need context tracking)
        constexpr uint16_t SPECIAL_PLAY_SLOW_CRY = 95;
        constexpr uint16_t SPECIAL_SET_PLAYER_PALETTE = 152;
        // Batch 9: Context-dependent Specials (require setval producer)
        constexpr uint16_t SPECIAL_MAP_RADIO = 40;
        constexpr uint16_t SPECIAL_GAME_CORNER_PRIZE_MON_CHECK_DEX = 57;
        constexpr uint16_t SPECIAL_FIND_PARTY_MON_THAT_SPECIES = 66;
        constexpr uint16_t SPECIAL_FIND_PARTY_MON_YOUR_TRAINER_ID = 67;
        // Party/Pokemon
        constexpr uint16_t SPECIAL_HEAL_PARTY = 27;     // Batch 3: HealParty
        constexpr uint16_t SPECIAL_CHECK_POKERUS = 78;  // Batch 5: CheckPokerus
        // Currency balance overlays (Batch 4)
        constexpr uint16_t SPECIAL_DISPLAY_COIN_CASE_BALANCE = 79;    // Coins only
        constexpr uint16_t SPECIAL_DISPLAY_MONEY_AND_COIN_BALANCE = 80; // Money + Coins
        constexpr uint16_t SPECIAL_PLACE_MONEY_TOP_RIGHT = 81;        // Money only
        // Platform check (Batch 5: frontend-absorbed)
        constexpr uint16_t SPECIAL_GAMEBOY_CHECK = 102;
        // Result constants for GameboyCheck (source values for documentation)
        // constexpr uint16_t GBCHECK_GB = 0;   // Original Game Boy
        // constexpr uint16_t GBCHECK_SGB = 1;  // Super Game Boy
        constexpr uint16_t GBCHECK_CGB = 2;     // Color Game Boy (Crystal's native platform)
        // Stubbed Mobile Adapter operations (Batch 6: frontend-absorbed no-ops)
        constexpr uint16_t SPECIAL_STUBBED_TRAINER_RANKINGS_HEALINGS = 157;
        // Mobile Adapter status check (Batch 7: frontend-absorbed constant result)
        constexpr uint16_t SPECIAL_CHECK_MOBILE_ADAPTER_STATUS = 160;
        // Yes/No prompt (Batch 8: AskRememberPassword - identical to yesorno opcode)
        constexpr uint16_t SPECIAL_ASK_REMEMBER_PASSWORD = 163;
        // Daylight Saving Time (Batch 8: set/clear DST flag with time display)
        constexpr uint16_t SPECIAL_INITIAL_SET_DST_FLAG = 166;
        constexpr uint16_t SPECIAL_INITIAL_CLEAR_DST_FLAG = 167;
        
        switch (p->special_id) {
            // =================================================================
            // SCREEN FADE OPERATIONS
            // =================================================================
            // All four fades share:
            // - direction: In / Out
            // - color: White / Black  
            // - blocking: 4 steps × 2 frames = 8 frames
            // FadeOutToWhite additionally calls FillWhiteBGColor before fading
            // (observable as brief white flash before fade begins)
            
            case SPECIAL_FADE_OUT_TO_WHITE: {
                enginemon::Sem_ScreenFade op;
                op.direction = enginemon::FadeDirection::Out;
                op.color = enginemon::FadeColor::White;
                op.prefill = true;  // FadeOutToWhite calls FillWhiteBGColor first
                r.instructions.push_back(make_inst(std::move(op)));
                return r;
            }
            case SPECIAL_FADE_OUT_TO_BLACK: {
                enginemon::Sem_ScreenFade op;
                op.direction = enginemon::FadeDirection::Out;
                op.color = enginemon::FadeColor::Black;
                op.prefill = false;
                r.instructions.push_back(make_inst(std::move(op)));
                return r;
            }
            case SPECIAL_FADE_IN_FROM_WHITE: {
                enginemon::Sem_ScreenFade op;
                op.direction = enginemon::FadeDirection::In;
                op.color = enginemon::FadeColor::White;
                op.prefill = false;
                r.instructions.push_back(make_inst(std::move(op)));
                return r;
            }
            case SPECIAL_FADE_IN_FROM_BLACK: {
                enginemon::Sem_ScreenFade op;
                op.direction = enginemon::FadeDirection::In;
                op.color = enginemon::FadeColor::Black;
                op.prefill = false;
                r.instructions.push_back(make_inst(std::move(op)));
                return r;
            }
            
            // =================================================================
            // AUDIO OPERATIONS
            // =================================================================
            
            case SPECIAL_WAIT_SFX: {
                // WaitSFX - polls channels 5-8 until all SFX complete
                // Semantic equivalent to waitsfx opcode (0x99)
                // Contract: suspend script progression until currently active SFX completion
                // Source: pokecrystal/home/audio.asm WaitSFX
                r.instructions.push_back(make_inst(enginemon::Sem_WaitSound{}));
                return r;
            }
            case SPECIAL_PLAY_MAP_MUSIC: {
                // PlayMapMusic - queries map music (with special handling for surf/contest),
                // stops current music, plays the appropriate map music
                // Semantic equivalent to playmapmusic opcode (0x8B)
                // Contract: synchronize/play the music appropriate to current world/map state
                // Distinct from RestartMapMusic which just restarts stored wMapMusic
                // Source: pokecrystal/home/audio.asm PlayMapMusic
                r.instructions.push_back(make_inst(enginemon::Sem_PlayMapMusic{}));
                return r;
            }
            case SPECIAL_RESTART_MAP_MUSIC: {
                // Restarts stored wMapMusic without querying map
                // Distinct from PlayMapMusic which queries and may update
                r.instructions.push_back(make_inst(enginemon::Sem_RestartMapMusic{}));
                return r;
            }
            case SPECIAL_FADE_OUT_MUSIC: {
                // Fades current music to silence with fixed timing (wMusicFade = $2)
                // Non-blocking - returns immediately, fade happens in background
                r.instructions.push_back(make_inst(enginemon::Sem_FadeToSilence{}));
                return r;
            }
            
            // =================================================================
            // PALETTE/RENDERER SYNCHRONIZATION
            // =================================================================
            // These ensure visual state is consistent with game state.
            // They include blocking waits that are part of the observable behavior.
            
            case SPECIAL_RELOAD_SPRITES_NO_PALETTES: {
                // Clears BG palette buffer, requests update, waits 1 frame
                // Source: home/palettes.asm ReloadSpritesNoPalettes
                enginemon::Sem_SyncPalettes op;
                op.wait_frames = 1;
                r.instructions.push_back(make_inst(std::move(op)));
                return r;
            }
            case SPECIAL_CLEAR_BG_PALETTES: {
                // Clears palettes, sets BG map mode, waits 4 frames
                // Source: home/tilemap.asm ClearBGPalettes → WaitBGMap
                enginemon::Sem_SyncPalettes op;
                op.wait_frames = 4;
                r.instructions.push_back(make_inst(std::move(op)));
                return r;
            }
            
            // =================================================================
            // SPRITE REFRESH OPERATIONS
            // =================================================================
            // These refresh sprite visuals after state changes.
            
            case SPECIAL_UPDATE_PLAYER_SPRITE: {
                // Updates player sprite based on current flags (e.g., ENGINE_KRIS_IN_CABLE_CLUB)
                // Called after setflag/clearflag that affects player visual
                // Source: home/gfx.asm UpdatePlayerSprite → engine/overworld/overworld.asm
                r.instructions.push_back(make_inst(enginemon::Sem_RefreshPlayerSprite{}));
                return r;
            }
            case SPECIAL_LOAD_USED_SPRITES_GFX: {
                // Runs MAPCALLBACK_SPRITES then loads all used sprite GFX
                // Typically called after variablesprite to make change visible
                // Source: engine/overworld/overworld.asm LoadUsedSpritesGFX
                r.instructions.push_back(make_inst(enginemon::Sem_SyncSprites{}));
                return r;
            }
            case SPECIAL_REFRESH_SPRITES: {
                // Full sprite state rebuild: clears list, rebuilds, loads GFX
                // Heavy operation used after major state transitions
                // Source: engine/overworld/overworld.asm RefreshSprites
                r.instructions.push_back(make_inst(enginemon::Sem_RebuildSprites{}));
                return r;
            }
            case SPECIAL_LOAD_MAP_PALETTES: {
                // Loads SGB palette layout for current map
                // Used during fade transitions (between FadeOut and FadeIn)
                // In native renderer: SGB not emulated, but sync point for palette state
                // Source: engine/overworld/warp_connection.asm LoadMapPalettes
                // Absorb as SyncPalettes with no wait (immediate palette load)
                enginemon::Sem_SyncPalettes op;
                op.wait_frames = 0;
                r.instructions.push_back(make_inst(std::move(op)));
                return r;
            }
            
            // =================================================================
            // PARTY/POKEMON OPERATIONS
            // =================================================================
            
            case SPECIAL_HEAL_PARTY: {
                // HealParty - heals all non-egg party members
                // Source: pokecrystal/engine/pokemon/health.asm
                // Contract:
                //   - Skips eggs (cp EGG / jr z, .next)
                //   - For non-eggs: restore HP to max, clear status, restore PP
                //   - PP restoration preserves PP Up investment
                //   - Does NOT modify wScriptVar (no script result)
                // See Sem_HealParty documentation in semantic_ir.hpp
                r.instructions.push_back(make_inst(enginemon::Sem_HealParty{}));
                return r;
            }
            
            case SPECIAL_CHECK_POKERUS: {
                // CheckPokerus - check for active Pokérus infection in party
                // Source: pokecrystal/engine/events/pokerus/check_pokerus.asm
                // Contract:
                //   - Iterates ALL party members (no egg exclusion)
                //   - Checks lower nibble of PokerusStatus (days remaining counter)
                //   - Returns carry (TRUE) if ANY member has active infection
                //   - Does NOT check upper nibble (strain history)
                //   - Pure read-only query, no side effects
                // Sets wScriptVar: 1=has active infection, 0=no active infection
                // See Sem_CheckPartyPokerus documentation in semantic_ir.hpp
                r.instructions.push_back(make_inst(enginemon::Sem_CheckPartyPokerus{}));
                return r;
            }
            
            case SPECIAL_GAMEBOY_CHECK: {
                // GameboyCheck - frontend-absorbed at compile time
                // Source: pokecrystal/engine/events/specials.asm GameboyCheck
                // Call site: GoldenrodDeptStore5F.asm Carrie NPC (ONLY call in corpus)
                // Comment: "This is a dummy check from Gold/Silver"
                //
                // ABSORPTION PROOF:
                // Crystal is a CGB-only game. The GameboyCheck Special returns
                // GBCHECK_CGB (2) on all real Crystal hardware. The subsequent
                // ifnotequal GBCHECK_CGB branch is never taken on original hardware.
                //
                // The non-CGB branch shows "Mystery Gift requires Game Boy Color"
                // which is dead code - Crystal physically cannot run on non-CGB.
                //
                // SEMANTIC LOWERING:
                // Set wScriptVar to GBCHECK_CGB (2) directly. No hardware query
                // survives into native runtime. The subsequent conditional branch
                // will evaluate correctly based on this constant result.
                //
                // This is structurally equivalent to constant folding:
                //   source: special GameboyCheck → ifnotequal GBCHECK_CGB, .dead
                //   native: setval GBCHECK_CGB → ifnotequal GBCHECK_CGB, .dead
                // The branch condition is preserved; only the hardware query is eliminated.
                enginemon::Sem_SetVar op;
                op.var = enginemon::VarId{0};  // wScriptVar
                op.source = enginemon::VarValueSource::literal(GBCHECK_CGB);
                r.instructions.push_back(make_inst(std::move(op)));
                return r;
            }
            
            case SPECIAL_CHECK_MOBILE_ADAPTER_STATUS: {
                // CheckMobileAdapterStatusSpecial - frontend-absorbed at compile time
                // Source: pokecrystal/mobile/mobile_41.asm CheckMobileAdapterStatusSpecial
                //
                // SOURCE IMPLEMENTATION:
                //   CheckMobileAdapterStatusSpecial: ; unused
                //       ; this routine calls CheckMobileAdapterStatus
                //       ; in the Japanese version
                //       xor a                   ; A = 0
                //       ld [wScriptVar], a      ; wScriptVar = 0
                //       ret
                //
                // ABSORPTION PROOF:
                // International Crystal stubbed the Mobile Adapter check. The Japanese
                // version actually queried Mobile Adapter hardware status. Since the
                // Mobile Adapter was never released outside Japan, the stub returns
                // FALSE (0) unconditionally.
                //
                // All 7 corpus call sites consume the result via iffalse/iftrue:
                //   - CeruleanPokecenter1F: iftrue .mobile (NOT taken)
                //   - EcruteakPokecenter1F: iftrue .mobile (NOT taken)
                //   - FastShipCabins_SW_SSW_NW: iftrue .mobile (NOT taken)
                //   - Pokecenter2F (Trade): iffalse .NoMobile (TAKEN)
                //   - Pokecenter2F (Battle): iffalse .NoMobile (TAKEN)
                //   - Route40: iftrue .mobile (NOT taken)
                //   - SaffronPokecenter1F: iftrue .mobile (NOT taken)
                //
                // SEMANTIC LOWERING:
                // Set wScriptVar to 0 directly. No hardware query survives into
                // native runtime. The subsequent conditional branch will evaluate
                // correctly based on this constant result.
                //
                // This is structurally equivalent to constant folding:
                //   source: special CheckMobileAdapterStatusSpecial → iffalse/iftrue
                //   native: setval 0 → iffalse/iftrue
                // The branch condition is preserved; only the stubbed query is eliminated.
                //
                // NOTE: This is NOT a zero-instruction absorption. The result (0) must
                // be explicitly written because subsequent conditionals depend on it.
                enginemon::Sem_SetVar op;
                op.var = enginemon::VarId{0};  // wScriptVar
                op.source = enginemon::VarValueSource::literal(0);  // FALSE / Mobile not present
                r.instructions.push_back(make_inst(std::move(op)));
                return r;
            }
            
            // =================================================================
            // CURRENCY BALANCE OVERLAY OPERATIONS (Batch 4)
            // =================================================================
            // Fire-and-return presentation operations:
            //   - No script result (do NOT modify wScriptVar)
            //   - No gameplay-state mutation
            //   - No input wait (overlay appears immediately)
            //   - No menu-stack transition
            // Layout determined by content type (renderer policy).
            // Source: pokecrystal/engine/menus/menu_2.asm
            
            case SPECIAL_DISPLAY_COIN_CASE_BALANCE: {
                // DisplayCoinCaseBalance - shows coins only
                // Source: engine/menus/menu_2.asm DisplayCoinCaseBalance
                enginemon::Sem_ShowBalanceOverlay op;
                op.contents = enginemon::BalanceContent::Coins;
                r.instructions.push_back(make_inst(std::move(op)));
                return r;
            }
            case SPECIAL_DISPLAY_MONEY_AND_COIN_BALANCE: {
                // DisplayMoneyAndCoinBalance - shows both money and coins
                // Source: engine/menus/menu_2.asm DisplayMoneyAndCoinBalance
                enginemon::Sem_ShowBalanceOverlay op;
                op.contents = enginemon::BalanceContent::MoneyAndCoins;
                r.instructions.push_back(make_inst(std::move(op)));
                return r;
            }
            case SPECIAL_PLACE_MONEY_TOP_RIGHT: {
                // PlaceMoneyTopRight - shows money only
                // Source: engine/menus/menu_2.asm PlaceMoneyTopRight
                // NOTE: MENU_BACKUP_TILES flag in header is dead code (never honored)
                enginemon::Sem_ShowBalanceOverlay op;
                op.contents = enginemon::BalanceContent::Money;
                r.instructions.push_back(make_inst(std::move(op)));
                return r;
            }
            
            // =================================================================
            // STUBBED NO-OP OPERATIONS (Batch 6: frontend-absorbed)
            // =================================================================
            // These are Mobile Adapter features that were stubbed out for
            // localized (non-Japanese) Crystal releases. The source implementation
            // is a single `ret` instruction - immediate return with zero effects.
            // Absorption produces zero semantic instructions.
            
            case SPECIAL_STUBBED_TRAINER_RANKINGS_HEALINGS: {
                // StubbedTrainerRankings_Healings - complete no-op
                // Source: mobile/mobile_41.asm (suiCune reference)
                // Implementation: `ret` as first instruction (immediate unconditional return)
                // All code after `ret` is unreachable dead code from removed Mobile Adapter.
                //
                // ABSORPTION PROOF:
                // - No inputs read
                // - No outputs written
                // - No script result (wScriptVar not modified)
                // - No persistent state mutation
                // - No transient state mutation
                // - No wait/timing
                // - No UI/audio/world effects
                //
                // SEMANTIC LOWERING:
                // Produce zero semantic instructions. Track as absorbed.
                // Source command is consumed and fully accounted for.
                r.absorbed_opcodes.push_back(cmd->opcode());
                return r;
            }
            
            // =================================================================
            // YES/NO PROMPT (Batch 8)
            // =================================================================
            
            case SPECIAL_ASK_REMEMBER_PASSWORD: {
                // AskRememberPassword - semantically identical to yesorno opcode
                // Source: pokecrystal/engine/events/buena_menu.asm
                //
                // SOURCE IMPLEMENTATION:
                //   AskRememberPassword:
                //       call .DoMenu
                //       ld a, $0
                //       jr c, .okay
                //       ld a, $1
                //   .okay
                //       ld [wScriptVar], a
                //       ret
                //
                // The .DoMenu routine:
                //   - Displays Yes/No menu using YesNoMenuHeader at coords (14, 7)
                //   - 15-frame delay after menu close (purely presentation)
                //   - Returns carry if No/cancelled, carry clear if Yes
                //
                // RESULT CONTRACT (identical to Script_yesorno):
                //   - Yes selected: wScriptVar = 1 (TRUE)
                //   - No/cancelled: wScriptVar = 0 (FALSE)
                //
                // EQUIVALENCE PROOF:
                // Script_yesorno (opcode 0x4E):
                //   call YesNoBox; ld a, FALSE; jr c, .no; ld a, TRUE; .no: ld [wScriptVar], a
                // AskRememberPassword (Special 163):
                //   call .DoMenu; ld a, $0; jr c, .okay; ld a, $1; .okay: ld [wScriptVar], a
                //
                // Both produce TRUE (1) on Yes, FALSE (0) on No/cancel.
                // Position/delay differences are presentation-only.
                //
                // SEMANTIC LOWERING:
                // Lower to existing Sem_YesNo{}. No new op type needed.
                r.instructions.push_back(make_inst(enginemon::Sem_YesNo{}));
                return r;
            }
            
            // =================================================================
            // DAYLIGHT SAVING TIME OPERATIONS (Batch 8)
            // =================================================================
            // These set the player's DST preference and update the time display.
            // Source: pokecrystal/engine/rtc/timeset.asm
            //
            // Both operations share the same structure:
            //   1. Set/clear DST_F bit in wDST (persistent RTC state)
            //   2. Clear screen area (presentation)
            //   3. Call UpdateTime, print current time (presentation)
            //   4. Print confirmation text (presentation)
            //   5. Return immediately (no input wait, no script result)
            
            case SPECIAL_INITIAL_SET_DST_FLAG: {
                // InitialSetDSTFlag - enable daylight-saving time
                // Source: engine/rtc/timeset.asm InitialSetDSTFlag
                //
                // SOURCE IMPLEMENTATION:
                //   ld a, [wDST]
                //   set DST_F, a
                //   ld [wDST], a
                //   ... (clear box, print time, print "DST Is that OK?")
                //   ret
                //
                // SEMANTIC CONTRACT:
                //   - Mutates persistent DST preference to ENABLED
                //   - Updates time display to show DST-adjusted time
                //   - No script result (does NOT modify wScriptVar)
                //   - No input wait
                enginemon::Sem_SetDaylightSaving op;
                op.enabled = true;
                r.instructions.push_back(make_inst(std::move(op)));
                return r;
            }
            
            case SPECIAL_INITIAL_CLEAR_DST_FLAG: {
                // InitialClearDSTFlag - disable daylight-saving time
                // Source: engine/rtc/timeset.asm InitialClearDSTFlag
                //
                // SOURCE IMPLEMENTATION:
                //   ld a, [wDST]
                //   res DST_F, a
                //   ld [wDST], a
                //   ... (clear box, print time, print "Time Ask Okay?")
                //   ret
                //
                // SEMANTIC CONTRACT:
                //   - Mutates persistent DST preference to DISABLED
                //   - Updates time display to show standard time
                //   - No script result (does NOT modify wScriptVar)
                //   - No input wait
                enginemon::Sem_SetDaylightSaving op;
                op.enabled = false;
                r.instructions.push_back(make_inst(std::move(op)));
                return r;
            }
            
            // =================================================================
            // BATCH 9: CONTEXT-DEPENDENT SPECIALS (block-local ScriptVar)
            // =================================================================
            // These Specials consume wScriptVar set by preceding setval.
            // Lowering requires:
            //   1. Known ScriptVar value (ctx.block_ctx.has_value())
            //   2. Valid domain for the consumed value
            // On success: emit typed semantic op, invalidate context if result-writing
            // On failure: fall through to Sem_Special
            
            case SPECIAL_MAP_RADIO: {
                // MapRadio - play radio channel from wScriptVar
                // Source: pokecrystal/engine/events/specials.asm MapRadio
                // Domain: RadioChannelId 0-8 valid (0=off, 1-8=channels)
                // Does NOT write wScriptVar (preserves context)
                
                if (!ctx.block_ctx.has_value()) {
                    // No producer - cannot lower contextually
                    break;
                }
                uint8_t channel = ctx.block_ctx.value();
                
                // Domain validation: Crystal has channels 0-8
                if (channel > 8) {
                    // Invalid channel - refuse contextual lowering
                    break;
                }
                
                enginemon::Sem_PlayRadio op;
                op.channel = channel;
                r.instructions.push_back(make_inst(std::move(op)));
                // Context preserved - MapRadio does NOT write wScriptVar
                return r;
            }
            
            case SPECIAL_GAME_CORNER_PRIZE_MON_CHECK_DEX: {
                // GameCornerPrizeMonCheckDex - conditionally register new Pokedex entry
                // Source: pokecrystal/engine/events/specials.asm GameCornerPrizeMonCheckDex
                // Domain: SpeciesId [1, num_pokemon] from profile (not hardcoded 251)
                // Does NOT write wScriptVar (preserves context)
                
                if (!ctx.block_ctx.has_value()) {
                    // No producer - cannot lower contextually
                    break;
                }
                uint8_t species = ctx.block_ctx.value();
                
                // Domain validation: valid species from profile domain
                if (species == 0 || species > ctx.num_pokemon) {
                    // Invalid species - refuse contextual lowering
                    break;
                }
                
                enginemon::Sem_RegisterNewDexEntry op;
                op.species = enginemon::SpeciesId{species};
                r.instructions.push_back(make_inst(std::move(op)));
                // Context preserved - GameCornerPrizeMonCheckDex does NOT write wScriptVar
                return r;
            }
            
            case SPECIAL_FIND_PARTY_MON_THAT_SPECIES: {
                // FindPartyMonThatSpecies - search party for species (any OT)
                // Source: pokecrystal/engine/events/specials.asm FindPartyMonThatSpecies
                // Domain: SpeciesId [1, num_pokemon] from profile (not hardcoded 251)
                // WRITES wScriptVar with result (1-6 = slot+1, 0 = not found)
                
                if (!ctx.block_ctx.has_value()) {
                    // No producer - cannot lower contextually
                    break;
                }
                uint8_t species = ctx.block_ctx.value();
                
                // Domain validation: valid species from profile domain
                if (species == 0 || species > ctx.num_pokemon) {
                    // Invalid species - refuse contextual lowering
                    break;
                }
                
                enginemon::Sem_FindPartyMon op;
                op.species = enginemon::SpeciesId{species};
                op.require_ot = false;
                r.instructions.push_back(make_inst(std::move(op)));
                // Batch 9: Invalidate context - result-writing Special
                ctx.block_ctx.invalidate();
                return r;
            }
            
            case SPECIAL_FIND_PARTY_MON_YOUR_TRAINER_ID: {
                // FindPartyMonThatSpeciesYourTrainerID - search party for species (player OT only)
                // Source: pokecrystal/engine/events/specials.asm FindPartyMonThatSpeciesYourTrainerID
                // Domain: SpeciesId [1, num_pokemon] from profile (not hardcoded 251)
                // WRITES wScriptVar with result (1-6 = slot+1, 0 = not found)
                
                if (!ctx.block_ctx.has_value()) {
                    // No producer - cannot lower contextually
                    break;
                }
                uint8_t species = ctx.block_ctx.value();
                
                // Domain validation: valid species from profile domain
                if (species == 0 || species > ctx.num_pokemon) {
                    // Invalid species - refuse contextual lowering
                    break;
                }
                
                enginemon::Sem_FindPartyMon op;
                op.species = enginemon::SpeciesId{species};
                op.require_ot = true;
                r.instructions.push_back(make_inst(std::move(op)));
                // Batch 9: Invalidate context - result-writing Special
                ctx.block_ctx.invalidate();
                return r;
            }
            
            case SPECIAL_PLAY_SLOW_CRY: {
                // PlaySlowCry - play slowed Pokemon cry from wScriptVar
                // Source: pokecrystal/engine/events/specials.asm PlaySlowCry
                // Domain: SpeciesId [1, num_pokemon] from profile (not hardcoded 251)
                // Does NOT write wScriptVar (preserves context)
                
                if (!ctx.block_ctx.has_value()) {
                    // No producer - cannot lower contextually
                    break;
                }
                uint8_t species = ctx.block_ctx.value();
                
                // Domain validation: valid species from profile domain
                if (species == 0 || species > ctx.num_pokemon) {
                    // Invalid species - refuse contextual lowering
                    break;
                }
                
                enginemon::Sem_PlayCry op;
                op.species = enginemon::SpeciesId{species};
                op.variant = enginemon::CryVariant::Slow;
                r.instructions.push_back(make_inst(std::move(op)));
                // Context preserved - PlaySlowCry does NOT write wScriptVar
                return r;
            }
            
            case SPECIAL_SET_PLAYER_PALETTE: {
                // SetPlayerPalette - change player sprite palette from wScriptVar
                // Source: pokecrystal/engine/overworld/map_objects.asm _SetPlayerPalette
                //
                // Crystal encoding: PAL_NPC_* << 4 (0x80=PAL0, 0x90=PAL1, etc.)
                // Bit 7 MUST be set for valid palette literal. If not set, routine returns early.
                //
                // Crystal math:
                //   ld a, d
                //   and 1 << 7     ; Check bit 7
                //   ret z          ; Return if not set
                //   ...
                //   swap a         ; Swap nibbles: 0x80→0x08, 0x90→0x09, 0xAB→0xBA
                //   and OAM_PALETTE; OAM_PALETTE = 0x07 (bits 0-2 for CGB OBJ palette)
                //
                // Equivalence proof:
                //   swap(x) & 0x07 = ((x & 0x0F) << 4 | (x >> 4)) & 0x07 = (x >> 4) & 0x07
                //   Since bit 7 is set: (x >> 4) ∈ [8, 15]
                //   (x >> 4) & 0x07 = (x >> 4) - 8 for x ∈ [0x80, 0xF0] with low nibble 0
                //
                // Normalization: (value >> 4) & 0x07 = semantic PaletteId (0-7)
                //   0x80 → palette 0
                //   0x90 → palette 1
                //   0xF0 → palette 7
                //
                // Does NOT write wScriptVar (preserves context)
                
                if (!ctx.block_ctx.has_value()) {
                    // No producer - cannot lower contextually
                    break;
                }
                uint8_t raw_palette = ctx.block_ctx.value();
                
                // Domain validation: bit 7 must be set (Crystal encoding)
                // This matches Crystal's `and 1 << 7; ret z` - routine does nothing if bit 7 not set
                if ((raw_palette & 0x80) == 0) {
                    // Invalid encoding - refuse contextual lowering
                    break;
                }
                
                // Normalize Crystal encoding to semantic PaletteId
                // Equivalent to Crystal's `swap a; and OAM_PALETTE` = (raw >> 4) & 0x07
                uint8_t palette_id = (raw_palette >> 4) & 0x07;
                
                enginemon::Sem_SetPlayerPalette op;
                op.palette_id = palette_id;
                r.instructions.push_back(make_inst(std::move(op)));
                // Context preserved - SetPlayerPalette does NOT write wScriptVar
                return r;
            }
            
            default:
                break;
        }
        
        // Fallback: emit generic Sem_Special for unhandled specials
        enginemon::Sem_Special op;
        op.special_id = p->special_id;
        op.name = "special_" + std::to_string(p->special_id);
        r.instructions.push_back(make_inst(std::move(op)));
        return r;
    }
    return {};
}

RuleResult rule_pokepic(LoweringContext& ctx) {
    const auto* cmd = ctx.peek();
    if (!cmd) return {};
    
    if (auto* p = std::get_if<Cmd_Pokepic>(&cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        enginemon::Sem_Pokepic op;
        op.species = enginemon::SpeciesId{p->pokemon};
        r.instructions.push_back(make_inst(std::move(op)));
        return r;
    }
    if (std::holds_alternative<Cmd_Closepokepic>(cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        r.instructions.push_back(make_inst(enginemon::Sem_ClosePokepic{}));
        return r;
    }
    return {};
}


RuleResult rule_commerce(LoweringContext& ctx) {
    const auto* cmd = ctx.peek();
    if (!cmd) return {};
    
    if (auto* p = std::get_if<Cmd_Pokemart>(&cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        enginemon::Sem_Pokemart op;
        op.dialog_id = p->dialog_id;
        op.mart_id = p->mart_id;
        r.instructions.push_back(make_inst(std::move(op)));
        return r;
    }
    if (auto* p = std::get_if<Cmd_Elevator>(&cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        
        // Resolve ROM floor_list_address to semantic ElevatorId
        enginemon::ElevatorId elevator_id = enginemon::ELEVATOR_NONE;
        
        if (ctx.elevator_registry) {
            // Register/lookup elevator (extracts floor data and assigns ID)
            elevator_id = ctx.elevator_registry->register_elevator(p->floor_list_address);
        }
        
        if (elevator_id == enginemon::ELEVATOR_NONE) {
            // Failed to resolve - cannot lower this command
            r.diagnostics = "Failed to resolve elevator floor-list at 0x" + 
                            std::to_string(p->floor_list_address);
            // Still emit with ELEVATOR_NONE - linker will catch invalid reference
        }
        
        enginemon::Sem_Elevator op;
        op.elevator_id = elevator_id;
        r.instructions.push_back(make_inst(std::move(op)));
        return r;
    }
    if (auto* p = std::get_if<Cmd_Trade>(&cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        enginemon::Sem_Trade op;
        op.trade_id = p->trade_id;
        r.instructions.push_back(make_inst(std::move(op)));
        return r;
    }
    if (auto* p = std::get_if<Cmd_Fruittree>(&cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        enginemon::Sem_FruitTree op;
        op.tree_id = p->tree_id;
        r.instructions.push_back(make_inst(std::move(op)));
        return r;
    }
    return {};
}


RuleResult rule_game_completion(LoweringContext& ctx) {
    const auto* cmd = ctx.peek();
    if (!cmd) return {};
    
    if (std::holds_alternative<Cmd_Halloffame>(cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        r.instructions.push_back(make_inst(enginemon::Sem_HallOfFame{}));
        return r;
    }
    if (std::holds_alternative<Cmd_Credits>(cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        r.instructions.push_back(make_inst(enginemon::Sem_Credits{}));
        return r;
    }
    return {};
}

RuleResult rule_checksave(LoweringContext& ctx) {
    const auto* cmd = ctx.peek();
    if (!cmd) return {};
    
    if (std::holds_alternative<Cmd_Checksave>(cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        r.instructions.push_back(make_inst(enginemon::Sem_CheckSave{}));
        return r;
    }
    return {};
}

// --- Additional Variable/Memory Rules ---

RuleResult rule_read_var(LoweringContext& ctx) {
    const auto* cmd = ctx.peek();
    if (!cmd) return {};
    
    // readvar: Read semantic variable value into script result (wScriptVar)
    // Reference: pokecrystal/engine/overworld/variables.asm - uses GetVarAction dispatch table
    // These are semantic variables like VAR_PARTYCOUNT, VAR_TIMEOFDAY, not raw RAM addresses
    if (auto* p = std::get_if<Cmd_Readvar>(&cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        enginemon::Sem_CheckVar op;
        op.var = enginemon::VarId{p->var_id};
        op.op = "load";  // Special: load value into script result, not comparison
        op.value = 0;
        r.instructions.push_back(make_inst(std::move(op)));
        
        // Batch 9: Invalidate context - readvar writes to wScriptVar
        ctx.block_ctx.invalidate();
        
        return r;
    }
    
    // writevar: Write script result (wScriptVar) to semantic variable
    if (auto* p = std::get_if<Cmd_Writevar>(&cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        enginemon::Sem_SetVar op;
        op.var = enginemon::VarId{p->var_id};
        op.source = enginemon::VarValueSource::script_result();  // Copy from wScriptVar
        r.instructions.push_back(make_inst(std::move(op)));
        return r;
    }
    return {};
}

// Note: readmem/writemem/loadmem operate on raw GB RAM addresses
// These CANNOT be semantically lowered - they leak machine concepts (GB RAM layout)
// They produce UnloweredDiagnostic (compiler-side), NOT packageable semantic ops

RuleResult rule_variable_sprite(LoweringContext& ctx) {
    const auto* cmd = ctx.peek();
    if (!cmd) return {};
    
    if (auto* p = std::get_if<Cmd_Variablesprite>(&cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        enginemon::Sem_VariableSprite op;
        op.slot = p->slot;
        op.sprite = p->sprite;
        r.instructions.push_back(make_inst(std::move(op)));
        return r;
    }
    return {};
}

RuleResult rule_follow(LoweringContext& ctx) {
    const auto* cmd = ctx.peek();
    if (!cmd) return {};
    
    if (auto* p = std::get_if<Cmd_Follow>(&cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        enginemon::Sem_Follow op;
        op.object1 = p->object1;
        op.object2 = p->object2;
        r.instructions.push_back(make_inst(std::move(op)));
        return r;
    }
    if (std::holds_alternative<Cmd_Stopfollow>(cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        r.instructions.push_back(make_inst(enginemon::Sem_StopFollow{}));
        return r;
    }
    if (auto* p = std::get_if<Cmd_Follownotexact>(&cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        enginemon::Sem_Follow op;
        op.object1 = p->object1;
        op.object2 = p->object2;
        r.instructions.push_back(make_inst(std::move(op)));
        return r;
    }
    return {};
}

RuleResult rule_win_loss_text(LoweringContext& ctx) {
    const auto* cmd = ctx.peek();
    if (!cmd) return {};
    
    if (auto* p = std::get_if<Cmd_Winlosstext>(&cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        enginemon::Sem_SetWinLossText op;
        // Extract text through registry using RESOLVED flat addresses
        // Address 0 = no text (Crystal sentinel) - modeled as std::nullopt, NOT TEXT_NONE
        if (ctx.text_registry) {
            // Use resolved flat_address, NOT raw 16-bit pointer
            // Address 0 = absence, use std::nullopt (not a fake TextId)
            if (p->win_text_address != 0) {
                auto text_id = ctx.text_registry->extract(p->win_text_address);
                if (text_id != enginemon::TEXT_NONE) {
                    op.win_text = text_id;
                }
                // else: extraction failed, leave as nullopt
            }
            // else: address 0 = no text, leave as nullopt
            
            if (p->loss_text_address != 0) {
                auto text_id = ctx.text_registry->extract(p->loss_text_address);
                if (text_id != enginemon::TEXT_NONE) {
                    op.loss_text = text_id;
                }
                // else: extraction failed, leave as nullopt
            }
            // else: address 0 = no loss text, leave as nullopt
        }
        // No registry = both remain nullopt (will be caught by legality gate if needed)
        r.instructions.push_back(make_inst(std::move(op)));
        return r;
    }
    return {};
}

RuleResult rule_reanchor_map(LoweringContext& ctx) {
    const auto* cmd = ctx.peek();
    if (!cmd) return {};
    
    if (std::holds_alternative<Cmd_Reanchormap>(cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        // reanchormap resets the map view anchor - maps to ReloadMap semantically
        r.instructions.push_back(make_inst(enginemon::Sem_RefreshMap{}));
        return r;
    }
    return {};
}

RuleResult rule_menu_ops(LoweringContext& ctx) {
    const auto* cmd = ctx.peek();
    if (!cmd) return {};
    
    // loadmenu/verticalmenu/2dmenu are menu display operations
    if (auto* p = std::get_if<Cmd_Loadmenu>(&cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        enginemon::Sem_Choice op;
        // Menu header would be resolved at compile time
        r.instructions.push_back(make_inst(std::move(op)));
        return r;
    }
    if (std::holds_alternative<Cmd_Verticalmenu>(cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        r.instructions.push_back(make_inst(enginemon::Sem_Choice{}));
        return r;
    }
    if (std::holds_alternative<Cmd_2dmenu>(cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        r.instructions.push_back(make_inst(enginemon::Sem_Choice{}));
        return r;
    }
    return {};
}

RuleResult rule_string_format(LoweringContext& ctx) {
    const auto* cmd = ctx.peek();
    if (!cmd) return {};
    
    // String formatting commands prepare values for text substitution
    // These emit semantic operations that prepare text arguments
    // The text system uses typed arguments instead of GB RAM/string-buffer
    
    // getmonname - prepare Pokemon species name
    if (auto* p = std::get_if<Cmd_Getmonname>(&cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        auto op = enginemon::Sem_PrepareTextArg::pokemon_name(
            enginemon::SpeciesId{p->pokemon}, p->strbuf);
        r.instructions.push_back(make_inst(std::move(op)));
        return r;
    }
    
    // getitemname - prepare item name
    if (auto* p = std::get_if<Cmd_Getitemname>(&cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        auto op = enginemon::Sem_PrepareTextArg::item_name(
            enginemon::ItemId{p->item}, p->strbuf);
        r.instructions.push_back(make_inst(std::move(op)));
        return r;
    }
    
    // gettrainername - prepare trainer name
    if (auto* p = std::get_if<Cmd_Gettrainername>(&cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        auto op = enginemon::Sem_PrepareTextArg::trainer_name(
            p->trainer_id, p->strbuf);
        r.instructions.push_back(make_inst(std::move(op)));
        return r;
    }
    
    // getstring - prepare generic string
    if (auto* p = std::get_if<Cmd_Getstring>(&cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        auto op = enginemon::Sem_PrepareTextArg::string("", p->strbuf);  // Resolved at runtime
        r.instructions.push_back(make_inst(std::move(op)));
        return r;
    }
    
    // getmoney - prepare money amount (from player's money)
    if (std::holds_alternative<Cmd_Getmoney>(cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        auto op = enginemon::Sem_PrepareTextArg::number(enginemon::VarId{0}, 0);  // Player money
        r.instructions.push_back(make_inst(std::move(op)));
        return r;
    }
    
    // getcoins - prepare coin amount
    if (std::holds_alternative<Cmd_Getcoins>(cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        auto op = enginemon::Sem_PrepareTextArg::number(enginemon::VarId{1}, 0);  // Player coins
        r.instructions.push_back(make_inst(std::move(op)));
        return r;
    }
    
    // getnum - prepare number from wScriptVar
    if (std::holds_alternative<Cmd_Getnum>(cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        auto op = enginemon::Sem_PrepareTextArg::number(enginemon::VarId{0}, 0);  // wScriptVar
        r.instructions.push_back(make_inst(std::move(op)));
        return r;
    }
    
    // Other string formatting commands (landmark name, trainer class, etc.)
    if (std::holds_alternative<Cmd_Getcurlandmarkname>(cmd->data) ||
        std::holds_alternative<Cmd_Getlandmarkname>(cmd->data) ||
        std::holds_alternative<Cmd_Gettrainerclassname>(cmd->data) ||
        std::holds_alternative<Cmd_Getname>(cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        auto op = enginemon::Sem_PrepareTextArg::string("", 0);  // Resolved at runtime
        r.instructions.push_back(make_inst(std::move(op)));
        return r;
    }
    
    return {};
}

RuleResult rule_item_notify(LoweringContext& ctx) {
    const auto* cmd = ctx.peek();
    if (!cmd) return {};
    
    // itemnotify displays a notification after giving an item
    // This is part of the Sem_GiveItemVerbose flow - absorbed because the 
    // notification behavior is fully captured by Sem_GiveItemVerbose.
    // 
    // Pattern: verbosegiveitem → itemnotify (always paired, same block)
    // Proof: Gen2Recomped Inventory.lua - GiveItem handles the message display
    //        pokecrystal Script_verbosegiveitem chains to PutItemInPocket → ItemNotify
    if (std::holds_alternative<Cmd_Itemnotify>(cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        r.absorbed_opcodes.push_back(cmd->opcode());
        // Absorbed: itemnotify always follows verbosegiveitem which emits Sem_GiveItemVerbose.
        // The notification is part of that semantic operation's presentation.
        // No independent observable effect - presentation is tied to the give.
        return r;
    }
    
    // pocketisfull checks if a pocket (items, balls, TMs, key items) is full
    // This is typically part of a giveitem flow where the script checks before giving.
    //
    // Pattern: giveitem → pocketisfull → iffalse/iftrue (check result)
    // The result IS independently observable (wScriptVar), so we CANNOT absorb it.
    // 
    // Proof: pokecrystal Script_pocketisfull → CheckItemPocket → sets wScriptVar to TRUE/FALSE
    //        This result is used by subsequent conditionals, NOT consumed by another op.
    //
    // Fix: Emit as a check operation that sets wScriptVar based on pocket capacity.
    // For now, absorbed with explicit acknowledgment this is a gap.
    // When inventory system is implemented, this should emit Sem_CheckPocketFull.
    if (std::holds_alternative<Cmd_Pocketisfull>(cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        r.absorbed_opcodes.push_back(cmd->opcode());
        // KNOWN GAP: pocketisfull result is independently observable via wScriptVar.
        // This absorption is semantically lossy - the conditional branch outcome
        // may differ if the pocket is actually full vs our "always has space" assumption.
        // 
        // Occurrences in corpus: Need to verify actual count and usage patterns.
        // When inventory capacity tracking is implemented, emit proper semantic op.
        return r;
    }
    return {};
}

RuleResult rule_warp_check(LoweringContext& ctx) {
    const auto* cmd = ctx.peek();
    if (!cmd) return {};
    
    // warpcheck sets wScriptVar based on warp validity (Battle Tower uses this)
    // This is a semantic check operation that produces a result
    if (std::holds_alternative<Cmd_Warpcheck>(cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        r.instructions.push_back(make_inst(enginemon::Sem_CheckWarp{}));
        return r;
    }
    return {};
}

RuleResult rule_misc_control(LoweringContext& ctx) {
    const auto* cmd = ctx.peek();
    if (!cmd) return {};
    
    if (auto* p = std::get_if<Cmd_Deactivatefacing>(&cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        enginemon::Sem_Pause op;
        op.length = p->time;
        r.instructions.push_back(make_inst(std::move(op)));
        return r;
    }
    
    // dontrestartmapmusic sets a flag affecting subsequent map loads
    // This has a persistent gameplay effect - prevent music restart on warp
    if (std::holds_alternative<Cmd_Dontrestartmapmusic>(cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        enginemon::Sem_SetMusicRestartFlag op;
        op.prevent_restart = true;
        r.instructions.push_back(make_inst(std::move(op)));
        return r;
    }
    
    if (std::holds_alternative<Cmd_Encountermusic>(cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        // Play encounter music
        r.instructions.push_back(make_inst(enginemon::Sem_PlayMapMusic{}));
        return r;
    }
    return {};
}

// --- Additional Misc Rules ---

// verbosegiveitemvar: Give item with message, item from wScriptVar
// Reference: pokecrystal/engine/overworld/scripting.asm Script_verbosegiveitemvar
// Semantics: gives item from variable with notification - maps to verbose give
RuleResult rule_verbose_give_item_var(LoweringContext& ctx) {
    const auto* cmd = ctx.peek();
    if (!cmd) return {};
    
    if (auto* p = std::get_if<Cmd_Verbosegiveitemvar>(&cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        enginemon::Sem_GiveItemVerbose op;
        // Item comes from variable or wScriptVar (ITEM_FROM_MEM = 0)
        op.item = enginemon::ItemId{p->item};
        op.quantity = p->var;  // Second byte is actually quantity variable
        r.instructions.push_back(make_inst(std::move(op)));
        return r;
    }
    return {};
}

// newloadmap: Load map with specific method (0-3)
// Reference: pokecrystal/engine/overworld/scripting.asm Script_newloadmap
// Semantics: triggers map reload with entry method - maps to ReloadMap
RuleResult rule_new_load_map(LoweringContext& ctx) {
    const auto* cmd = ctx.peek();
    if (!cmd) return {};
    
    if (std::holds_alternative<Cmd_Newloadmap>(cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        // newloadmap triggers a map reload - semantic equivalent
        r.instructions.push_back(make_inst(enginemon::Sem_ReloadMap{}));
        return r;
    }
    return {};
}

// describedecoration: Describe a decoration by ID
// Reference: pokecrystal/engine/overworld/scripting.asm Script_describedecoration
// Semantics: Shows description text for a decoration
RuleResult rule_describe_decoration(LoweringContext& ctx) {
    const auto* cmd = ctx.peek();
    if (!cmd) return {};
    
    if (auto* p = std::get_if<Cmd_Describedecoration>(&cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        // Use dedicated semantic operation with the actual decoration ID
        enginemon::Sem_DescribeDecoration op;
        op.decoration_id = p->byte;
        r.instructions.push_back(make_inst(std::move(op)));
        return r;
    }
    return {};
}

// askforphonenumber: Ask player to register phone number
// Reference: pokecrystal/engine/overworld/scripting.asm Script_askforphonenumber
// Semantics: yes/no prompt, if yes adds phone number, result in wScriptVar
RuleResult rule_ask_phone_number(LoweringContext& ctx) {
    const auto* cmd = ctx.peek();
    if (!cmd) return {};
    
    if (auto* p = std::get_if<Cmd_Askforphonenumber>(&cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        // This is a compound: YesNo + conditional AddPhoneNumber + result
        // Maps to AddPhoneNumber semantic (handles the prompt internally)
        enginemon::Sem_AddPhoneNumber op;
        op.person = p->number;
        r.instructions.push_back(make_inst(std::move(op)));
        return r;
    }
    return {};
}

// specialphonecall: Trigger a special phone call event
// Reference: pokecrystal/engine/overworld/scripting.asm Script_specialphonecall
// Semantics: sets up phone call state for special caller using typed call_id
RuleResult rule_special_phone_call(LoweringContext& ctx) {
    const auto* cmd = ctx.peek();
    if (!cmd) return {};
    
    if (auto* p = std::get_if<Cmd_Specialphonecall>(&cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        // Use dedicated semantic operation with actual call_id (NOT synthetic Special)
        enginemon::Sem_SpecialPhoneCall op;
        op.call_id = p->call_id;
        r.instructions.push_back(make_inst(std::move(op)));
        return r;
    }
    return {};
}

// givepokemail/checkpokemail: Pokemon mail operations
// Reference: pokecrystal/engine/overworld/scripting.asm
// Semantics: mail manipulation - use dedicated typed operations
// PokeMailId is assigned through PokeMailRegistry semantic extraction
// Uses RESOLVED flat address (from TypedDecoder), NOT raw 16-bit pointer
RuleResult rule_pokemail_ops(LoweringContext& ctx) {
    const auto* cmd = ctx.peek();
    if (!cmd) return {};
    
    if (auto* p = std::get_if<Cmd_Givepokemail>(&cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        // Extract mail through registry using RESOLVED flat address
        enginemon::Sem_GivePokeMail op;
        if (ctx.pokemail_registry) {
            // Use flat_address resolved by TypedDecoder, NOT raw pointer
            op.mail_id = ctx.pokemail_registry->extract_give_mail(p->flat_address);
        } else {
            // No registry - cannot proceed, will be caught by legality gate
            op.mail_id = enginemon::POKEMAIL_NONE;
        }
        r.instructions.push_back(make_inst(std::move(op)));
        return r;
    }
    if (auto* p = std::get_if<Cmd_Checkpokemail>(&cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        // Extract mail through registry using RESOLVED flat address
        enginemon::Sem_CheckPokeMail op;
        if (ctx.pokemail_registry) {
            // Use flat_address resolved by TypedDecoder, NOT raw pointer
            op.mail_id = ctx.pokemail_registry->extract_check_mail(p->flat_address);
        } else {
            // No registry - cannot proceed, will be caught by legality gate
            op.mail_id = enginemon::POKEMAIL_NONE;
        }
        r.instructions.push_back(make_inst(std::move(op)));
        return r;
    }
    return {};
}

// checkver: Check game version (GS_VERSION constant)
// Reference: pokecrystal/engine/overworld/scripting.asm Script_checkver
// Semantics: sets wScriptVar to game version - always Crystal (1)
RuleResult rule_check_version(LoweringContext& ctx) {
    const auto* cmd = ctx.peek();
    if (!cmd) return {};
    
    if (std::holds_alternative<Cmd_Checkver>(cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        // Crystal version = 1, set in wScriptVar
        // Semantically this is SetVar(SCRIPT_VAR, 1)
        enginemon::Sem_SetVar op;
        op.var = enginemon::VarId{0};  // wScriptVar
        op.source = enginemon::VarValueSource::literal(1);  // GS_VERSION = 1 for Crystal
        r.instructions.push_back(make_inst(std::move(op)));
        return r;
    }
    return {};
}

// catchtutorial: Run catch tutorial battle
// Reference: pokecrystal/engine/overworld/scripting.asm Script_catchtutorial
// Semantics: special battle type, then reloads map
RuleResult rule_catch_tutorial(LoweringContext& ctx) {
    const auto* cmd = ctx.peek();
    if (!cmd) return {};
    
    if (std::holds_alternative<Cmd_Catchtutorial>(cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        // Catch tutorial sets battle type and runs special sequence
        // Semantically: start battle (tutorial mode implied)
        r.instructions.push_back(make_inst(enginemon::Sem_StartBattle{}));
        return r;
    }
    return {};
}

// --- Compound Patterns (currently unused, for future multi-command patterns) ---

RuleResult rule_check_flag_conditional(LoweringContext& ctx) {
    // Pattern: checkevent FLAG + iffalse TARGET
    // Could be combined into single semantic conditional
    // For now, lower separately
    return {};
}

RuleResult rule_check_scene_conditional(LoweringContext& ctx) {
    // Pattern: checkscene + ifequal N + ...
    // Could be combined into single semantic conditional
    // For now, lower separately
    return {};
}

} // namespace lowering_rules


// =============================================================================
// DEFAULT RULES FACTORY
// =============================================================================

LoweringRules LoweringRules::create_default() {
    LoweringRules rules;
    
    using namespace lowering_rules;
    
    // Control flow
    rules.add_rule("end", rule_end);
    rules.add_rule("return", rule_return);
    rules.add_rule("jump", rule_jump);
    rules.add_rule("conditional", rule_conditional);
    rules.add_rule("call", rule_call);
    rules.add_rule("std_script", rule_std_script);
    rules.add_rule("sdefer", rule_sdefer);
    
    // Flags/Variables
    rules.add_rule("set_flag", rule_set_flag);
    rules.add_rule("clear_flag", rule_clear_flag);
    rules.add_rule("check_flag", rule_check_flag);
    rules.add_rule("set_var", rule_set_var);
    rules.add_rule("add_var", rule_add_var);
    rules.add_rule("random", rule_random);
    rules.add_rule("ram_operations", rule_ram_operations);
    rules.add_rule("callasm_field_moves", rule_callasm_field_moves);
    
    // UI/Text
    rules.add_rule("open_text", rule_open_text);
    rules.add_rule("close_text", rule_close_text);
    rules.add_rule("write_text", rule_write_text);
    rules.add_rule("jump_text", rule_jump_text);
    rules.add_rule("jump_text_face_player", rule_jump_text_face_player);
    rules.add_rule("wait_button", rule_wait_button);
    rules.add_rule("yes_no", rule_yes_no);
    
    // Inventory
    rules.add_rule("give_item", rule_give_item);
    rules.add_rule("take_item", rule_take_item);
    rules.add_rule("check_item", rule_check_item);
    rules.add_rule("verbose_give_item", rule_verbose_give_item);
    rules.add_rule("money_ops", rule_money_ops);
    rules.add_rule("coin_ops", rule_coin_ops);

    
    // Party/Pokemon
    rules.add_rule("give_pokemon", rule_give_pokemon);
    rules.add_rule("give_egg", rule_give_egg);
    rules.add_rule("check_pokemon", rule_check_pokemon);
    
    // Movement/Object
    rules.add_rule("apply_movement", rule_apply_movement);
    rules.add_rule("face_player", rule_face_player);
    rules.add_rule("face_object", rule_face_object);
    rules.add_rule("turn_object", rule_turn_object);
    rules.add_rule("object_visibility", rule_object_visibility);
    rules.add_rule("move_object", rule_move_object);
    rules.add_rule("set_last_talked", rule_set_last_talked);
    rules.add_rule("emote", rule_emote);
    
    // Map/Warp/Scene
    rules.add_rule("warp", rule_warp);
    rules.add_rule("scene_ops", rule_scene_ops);
    rules.add_rule("map_ops", rule_map_ops);
    
    // Battle
    rules.add_rule("load_wild_mon", rule_load_wild_mon);
    rules.add_rule("random_wild_mon", rule_random_wild_mon);
    rules.add_rule("load_trainer", rule_load_trainer);
    rules.add_rule("start_battle", rule_start_battle);
    rules.add_rule("battle_aftermath", rule_battle_aftermath);
    rules.add_rule("trainer_script_ops", rule_trainer_script_ops);
    rules.add_rule("battle_tower_text", rule_battle_tower_text);
    
    // Audio
    rules.add_rule("play_music", rule_play_music);
    rules.add_rule("play_sound", rule_play_sound);
    rules.add_rule("play_cry", rule_play_cry);
    rules.add_rule("audio_control", rule_audio_control);
    
    // Time/Wait
    rules.add_rule("wait", rule_wait);
    rules.add_rule("pause", rule_pause);
    rules.add_rule("check_time", rule_check_time);
    
    // Phone
    rules.add_rule("phone_ops", rule_phone_ops);
    rules.add_rule("check_phone_call", rule_check_phone_call);
    
    // Visual Effects
    rules.add_rule("earthquake", rule_earthquake);
    
    // Misc
    rules.add_rule("wild_toggle", rule_wild_toggle);
    rules.add_rule("special", rule_special);
    rules.add_rule("pokepic", rule_pokepic);
    rules.add_rule("commerce", rule_commerce);
    rules.add_rule("game_completion", rule_game_completion);
    rules.add_rule("checksave", rule_checksave);
    
    // Additional variable operations (semantic vars, not raw RAM)
    rules.add_rule("read_var", rule_read_var);
    rules.add_rule("variable_sprite", rule_variable_sprite);
    
    // Object following
    rules.add_rule("follow", rule_follow);
    
    // Battle setup
    rules.add_rule("win_loss_text", rule_win_loss_text);
    
    // Map operations
    rules.add_rule("reanchor_map", rule_reanchor_map);
    
    // Menu operations
    rules.add_rule("menu_ops", rule_menu_ops);
    
    // String formatting (consumed - runtime handles)
    rules.add_rule("string_format", rule_string_format);
    
    // Item notifications (consumed - part of verbose item flow)
    rules.add_rule("item_notify", rule_item_notify);
    
    // Warp checking
    rules.add_rule("warp_check", rule_warp_check);
    
    // Miscellaneous control
    rules.add_rule("misc_control", rule_misc_control);
    
    // Additional misc operations
    rules.add_rule("verbose_give_item_var", rule_verbose_give_item_var);
    rules.add_rule("new_load_map", rule_new_load_map);
    rules.add_rule("describe_decoration", rule_describe_decoration);
    rules.add_rule("ask_phone_number", rule_ask_phone_number);
    rules.add_rule("special_phone_call", rule_special_phone_call);
    rules.add_rule("pokemail_ops", rule_pokemail_ops);
    rules.add_rule("check_version", rule_check_version);
    rules.add_rule("catch_tutorial", rule_catch_tutorial);
    
    return rules;
}

} // namespace crystal
