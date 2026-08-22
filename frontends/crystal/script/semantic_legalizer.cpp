// crystal/script/semantic_legalizer.cpp
// Stage 4: Block-local semantic legalization from Crystal CFG to SemanticScriptIR

#include "crystal/script/semantic_legalizer.hpp"
#include "crystal/script/behavior_table.hpp"
#include "crystal/script/decoder.hpp"  // For text decoding
#include "crystal/script/pokemail_registry.hpp"
#include "crystal/script/text_registry.hpp"
#include "crystal/rom/bank_utils.hpp"
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
    
    if (std::holds_alternative<Cmd_End>(cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        r.instructions.push_back(make_inst(enginemon::Sem_End{}));
        return r;
    }
    // endall (0x93): clears entire call stack — distinct from end which pops one frame
    // Source: Script_endall sets wScriptStackSize=0 unconditionally
    if (std::holds_alternative<Cmd_Endall>(cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        r.instructions.push_back(make_inst(enginemon::Sem_EndAll{}));
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
//
// CRITICAL: The pointer in Cmd_Sdefer is bank-relative (16-bit).
// We MUST resolve it to a flat address using the same formula as corpus_discovery:
//   flat = bank * 0x4000 + (ptr - 0x4000) for ptr >= 0x4000
// The bank is inferred from the source script's entry_address.
RuleResult rule_sdefer(LoweringContext& ctx) {
    const auto* cmd = ctx.peek();
    if (!cmd) return {};
    
    if (auto* sdef = std::get_if<Cmd_Sdefer>(&cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        
        // Resolve sdefer pointer to flat address using the calling script's bank.
        // Source-proven: Script_sdefer (pokecrystal scripting.asm:1389) reads
        // wScriptBank (the currently executing script's bank) as the target bank.
        // INVARIANT: discovery target == semantic target == canonical helper result.
        const uint32_t entry = ctx.source_ir ? ctx.source_ir->entry_address : 0;
        uint32_t flat_addr = crystal_local_ptr_to_flat(entry, sdef->pointer);
        
        enginemon::Sem_Sdefer op;
        // Generate script_id based on RESOLVED flat address
        // This matches how corpus_discovery discovers deferred targets
        std::ostringstream ss;
        ss << "deferred_" << std::hex << flat_addr;
        op.target_script_id = ss.str();
        
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
        op.flag = enginemon::FlagRef::event_flag(p->event_flag);
        r.instructions.push_back(make_inst(std::move(op)));
        return r;
    }
    if (auto* p = std::get_if<Cmd_Setflag>(&cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        enginemon::Sem_SetFlag op;
        op.flag = enginemon::FlagRef::engine_flag(p->engine_flag);
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
        op.flag = enginemon::FlagRef::event_flag(p->event_flag);
        r.instructions.push_back(make_inst(std::move(op)));
        return r;
    }
    if (auto* p = std::get_if<Cmd_Clearflag>(&cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        enginemon::Sem_ClearFlag op;
        op.flag = enginemon::FlagRef::engine_flag(p->engine_flag);
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
        op.flag = enginemon::FlagRef::event_flag(p->event_flag);
        r.instructions.push_back(make_inst(std::move(op)));
        return r;
    }
    if (auto* p = std::get_if<Cmd_Checkflag>(&cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        enginemon::Sem_CheckFlag op;
        op.flag = enginemon::FlagRef::engine_flag(p->engine_flag);
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
    
    // writetext (0x4C): shows text from local bank pointer in current textbox
    // farwritetext (0x4B): shows text from explicit bank:pointer in current textbox
    // Both must resolve the text pointer to a stable TextId — not an empty sequence.
    // Source: Script_writetext uses wScriptBank:HL; Script_farwritetext uses explicit bank
    
    if (auto* p = std::get_if<Cmd_Writetext>(&cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        enginemon::Sem_ShowText op;
        // Resolve bank-local text pointer using the COMMAND's own ROM address, not
        // the script entry address.  This is correct when the command lives in a
        // farsjump target block (different bank from the script root).
        // Source: Script_writetext uses wScriptBank which tracks the current
        // executing bank — equivalent to the command's own bank.
        const uint32_t flat_addr = crystal_local_ptr_to_flat(cmd->span.rom_address, p->text_pointer);
        if (ctx.text_registry && flat_addr != 0) {
            auto text_id = ctx.text_registry->extract(flat_addr);
            if (text_id != enginemon::TEXT_NONE) {
                const auto* def = ctx.text_registry->get(text_id);
                if (def) {
                    op.sequence = def->to_semantic_sequence(ctx.text_registry);
                }
            }
        }
        r.instructions.push_back(make_inst(std::move(op)));
        return r;
    }
    
    if (auto* p = std::get_if<Cmd_Farwritetext>(&cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        enginemon::Sem_ShowText op;
        // farwritetext carries explicit bank:pointer — resolve directly
        const uint32_t flat_addr = crystal_bank_to_flat(p->bank, p->pointer);
        if (ctx.text_registry && flat_addr != 0) {
            auto text_id = ctx.text_registry->extract(flat_addr);
            if (text_id != enginemon::TEXT_NONE) {
                const auto* def = ctx.text_registry->get(text_id);
                if (def) {
                    op.sequence = def->to_semantic_sequence(ctx.text_registry);
                }
            }
        }
        r.instructions.push_back(make_inst(std::move(op)));
        return r;
    }
    return {};
}


RuleResult rule_jump_text(LoweringContext& ctx) {
    const auto* cmd = ctx.peek();
    if (!cmd) return {};
    
    // jumptext (0x53): opentext + text + waitbutton + closetext + end, bank-local pointer
    // farjumptext (0x52): same but with explicit bank:pointer
    // Both must resolve the text pointer — see rule_write_text for pattern.
    
    if (auto* p = std::get_if<Cmd_Jumptext>(&cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        enginemon::Sem_ShowTextAndEnd op;
        // Use command's own ROM address for bank derivation — correct across farsjump targets.
        const uint32_t flat_addr = crystal_local_ptr_to_flat(cmd->span.rom_address, p->text_pointer);
        if (ctx.text_registry && flat_addr != 0) {
            auto text_id = ctx.text_registry->extract(flat_addr);
            if (text_id != enginemon::TEXT_NONE) {
                const auto* def = ctx.text_registry->get(text_id);
                if (def) {
                    op.sequence = def->to_semantic_sequence(ctx.text_registry);
                }
            }
        }
        r.instructions.push_back(make_inst(std::move(op)));
        return r;
    }
    
    if (auto* p = std::get_if<Cmd_Farjumptext>(&cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        enginemon::Sem_ShowTextAndEnd op;
        const uint32_t flat_addr = crystal_bank_to_flat(p->bank, p->pointer);
        if (ctx.text_registry && flat_addr != 0) {
            auto text_id = ctx.text_registry->extract(flat_addr);
            if (text_id != enginemon::TEXT_NONE) {
                const auto* def = ctx.text_registry->get(text_id);
                if (def) {
                    op.sequence = def->to_semantic_sequence(ctx.text_registry);
                }
            }
        }
        r.instructions.push_back(make_inst(std::move(op)));
        return r;
    }
    return {};
}

RuleResult rule_jump_text_face_player(LoweringContext& ctx) {
    const auto* cmd = ctx.peek();
    if (!cmd) return {};
    
    // jumptextfaceplayer (0x51): faceplayer + jumptext semantics, bank-local pointer
    
    if (auto* p = std::get_if<Cmd_Jumptextfaceplayer>(&cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        enginemon::Sem_FacePlayerAndShowText op;
        // Use command's own ROM address for bank derivation — correct across farsjump targets.
        const uint32_t flat_addr = crystal_local_ptr_to_flat(cmd->span.rom_address, p->text_pointer);
        if (ctx.text_registry && flat_addr != 0) {
            auto text_id = ctx.text_registry->extract(flat_addr);
            if (text_id != enginemon::TEXT_NONE) {
                const auto* def = ctx.text_registry->get(text_id);
                if (def) {
                    op.sequence = def->to_semantic_sequence(ctx.text_registry);
                }
            }
        }
        r.instructions.push_back(make_inst(std::move(op)));
        return r;
    }
    return {};
}

RuleResult rule_wait_button(LoweringContext& ctx) {
    const auto* cmd = ctx.peek();
    if (!cmd) return {};
    
    // waitbutton (0x54): wait for any button press — jp WaitButton
    if (std::holds_alternative<Cmd_Waitbutton>(cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        r.instructions.push_back(make_inst(enginemon::Sem_WaitButton{}));
        return r;
    }
    
    // promptbutton (0x55): WaitBGMap sync THEN PromptButton — distinct from waitbutton
    // Source: Script_promptbutton calls WaitBGMap then PromptButton (not WaitButton)
    if (std::holds_alternative<Cmd_Promptbutton>(cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        r.instructions.push_back(make_inst(enginemon::Sem_PromptButton{}));
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
        // yesorno writes wScriptVar (TRUE=yes, FALSE=no) — invalidate compile-time fact
        // Source: Script_yesorno (scripting.asm): ld [wScriptVar], a
        ctx.block_ctx.invalidate();
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
        // giveitem writes wScriptVar (TRUE=given, FALSE=bag full)
        // Source: Script_giveitem (scripting.asm): ld [wScriptVar], a
        ctx.block_ctx.invalidate();
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
        // takeitem writes wScriptVar (TRUE=taken, FALSE=not found)
        // Source: Script_takeitem (scripting.asm): ld [wScriptVar], a
        ctx.block_ctx.invalidate();
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
        // checkitem writes wScriptVar (TRUE=has item, FALSE=doesn't)
        // Source: Script_checkitem (scripting.asm): ld [wScriptVar], a
        ctx.block_ctx.invalidate();
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
        // verbosegiveitem writes wScriptVar (TRUE=given, FALSE=bag full)
        // Source: chains to GiveItemScript which calls ReceiveItem and writes result
        ctx.block_ctx.invalidate();
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
        
        // Resolve nickname and OT name if present
        // Source: Script_givepoke reads 4 extra bytes when trainer byte != 0:
        //   - 2 bytes: nickname pointer (bank-local)
        //   - 2 bytes: OT name pointer (bank-local)
        // Both are text resources in the script's bank.
        if (p->has_extra_data) {
            const uint32_t entry = ctx.source_ir ? ctx.source_ir->entry_address : 0;
            
            // Resolve nickname pointer to text content
            if (p->nickname_ptr != 0) {
                const uint32_t nick_flat = crystal_local_ptr_to_flat(entry, p->nickname_ptr);
                if (ctx.text_registry && nick_flat != 0) {
                    auto text_id = ctx.text_registry->extract(nick_flat);
                    if (text_id != enginemon::TEXT_NONE) {
                        const auto* def = ctx.text_registry->get(text_id);
                        if (def) {
                            op.nickname = def->plain_text();
                        }
                    }
                }
            }
            
            // Resolve OT name pointer to text content
            if (p->ot_name_ptr != 0) {
                const uint32_t ot_flat = crystal_local_ptr_to_flat(entry, p->ot_name_ptr);
                if (ctx.text_registry && ot_flat != 0) {
                    auto text_id = ctx.text_registry->extract(ot_flat);
                    if (text_id != enginemon::TEXT_NONE) {
                        const auto* def = ctx.text_registry->get(text_id);
                        if (def) {
                            op.ot_name = def->plain_text();
                        }
                    }
                }
            }
        }
        
        r.instructions.push_back(make_inst(std::move(op)));
        // givepoke writes wScriptVar (result of GivePoke — trainer byte value)
        // Source: Script_givepoke (scripting.asm): farcall GivePoke; ld a, b; ld [wScriptVar], a
        ctx.block_ctx.invalidate();
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
        // giveegg writes wScriptVar (0=no room in party, 2=egg given)
        // Source: Script_giveegg (scripting.asm): xor a; ld [wScriptVar], a; ...; ld a, 2; ld [wScriptVar], a
        ctx.block_ctx.invalidate();
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
        // checkpoke writes wScriptVar (TRUE=species in party, FALSE=not found)
        // Source: Script_checkpoke (scripting.asm): xor a; ld [wScriptVar], a; ...; ld [wScriptVar], a
        ctx.block_ctx.invalidate();
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
    // Source: Script_writecmdqueue reads 2-byte local ptr + wScriptBank, calls WriteCmdQueue
    // The pointer is bank-local to the calling script's bank.
    // Must resolve to flat ROM address using canonical bank helper — no raw pointer in semantic IR.
    if (auto* p = std::get_if<Cmd_Writecmdqueue>(&cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        enginemon::Sem_WriteCmdQueue op;
        // Resolve bank-local queue pointer to flat ROM address
        // Source: Script_writecmdqueue uses wScriptBank (= calling script's bank)
        const uint32_t entry = ctx.source_ir ? ctx.source_ir->entry_address : 0;
        op.queue_flat_address = crystal_local_ptr_to_flat(entry, p->queue_pointer);
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
        // delcmdqueue writes wScriptVar (TRUE=deleted, FALSE=not found)
        // Source: Script_delcmdqueue (scripting.asm): ld [wScriptVar], a
        ctx.block_ctx.invalidate();
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
        // startbattle writes wScriptVar (battle result: wBattleResult & ~BATTLERESULT_BITMASK)
        // Source: Script_startbattle (scripting.asm): ld a, [wBattleResult]; ld [wScriptVar], a
        ctx.block_ctx.invalidate();
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
        op.variant = enginemon::CryVariant::Normal;
        
        // Source-proven: Script_cry (scripting.asm ~line 784):
        //   and a; jr nz, .ok; ld a, [wScriptVar]   ← low byte == 0 → dynamic species
        // The cry opcode has a dw (16-bit) cry_id. Only the low byte is tested.
        uint8_t cry_low = static_cast<uint8_t>(p->cry_id & 0xFF);
        if (cry_low == 0) {
            // Dynamic: species read from wScriptVar at runtime
            op.source = enginemon::SpeciesSource::from_script_var();
        } else {
            // Literal species ID from operand
            op.source = enginemon::SpeciesSource::literal(enginemon::SpeciesId{cry_low});
        }
        // cry does NOT write wScriptVar — block_ctx preserved
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
        // checktime writes wScriptVar (TRUE=time matches flags, FALSE=doesn't)
        // Source: Script_checktime (scripting.asm): ld [wScriptVar], a
        ctx.block_ctx.invalidate();
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
        // addcellnum writes wScriptVar (TRUE=added, FALSE=full)
        // Source: Script_addcellnum (scripting.asm): ld [wScriptVar], a
        ctx.block_ctx.invalidate();
        return r;
    }
    if (auto* p = std::get_if<Cmd_Delcellnum>(&cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        enginemon::Sem_DeletePhoneNumber op;
        op.person = p->person;
        r.instructions.push_back(make_inst(std::move(op)));
        // delcellnum writes wScriptVar (TRUE=deleted, FALSE=not found)
        // Source: Script_delcellnum (scripting.asm): ld [wScriptVar], a
        ctx.block_ctx.invalidate();
        return r;
    }
    if (auto* p = std::get_if<Cmd_Checkcellnum>(&cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        enginemon::Sem_CheckPhoneNumber op;
        op.person = p->person;
        r.instructions.push_back(make_inst(std::move(op)));
        // checkcellnum writes wScriptVar (TRUE=registered, FALSE=not registered)
        // Source: Script_checkcellnum (scripting.asm): ld [wScriptVar], a
        ctx.block_ctx.invalidate();
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
        // checkphonecall writes wScriptVar (TRUE=pending call, FALSE=no pending call)
        // Source: Script_checkphonecall (scripting.asm): ld [wScriptVar], a
        ctx.block_ctx.invalidate();
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
                // checkpokerus writes wScriptVar — invalidate block-local context
                ctx.block_ctx.invalidate();
                return r;
            }
            
            case SPECIAL_GAMEBOY_CHECK: {
                // GameboyCheck — always returns GBCHECK_CGB (2) on Crystal hardware
                // See full documentation above.
                enginemon::Sem_SetVar op;
                op.var = enginemon::VarId{0};  // wScriptVar
                op.source = enginemon::VarValueSource::literal(GBCHECK_CGB);
                r.instructions.push_back(make_inst(std::move(op)));
                // This emits Sem_SetVar — update block_ctx to reflect the known value
                ctx.block_ctx.on_setval(GBCHECK_CGB);
                return r;
            }
            
            case SPECIAL_CHECK_MOBILE_ADAPTER_STATUS: {
                // CheckMobileAdapterStatusSpecial — always returns 0 in international Crystal
                // See full documentation above.
                enginemon::Sem_SetVar op;
                op.var = enginemon::VarId{0};  // wScriptVar
                op.source = enginemon::VarValueSource::literal(0);
                r.instructions.push_back(make_inst(std::move(op)));
                // This emits Sem_SetVar with value 0 — update block_ctx
                ctx.block_ctx.on_setval(0);
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
                op.source = enginemon::SpeciesSource::literal(enginemon::SpeciesId{species});
                op.variant = enginemon::CryVariant::Slow;
                r.instructions.push_back(make_inst(std::move(op)));
                // Context preserved - PlaySlowCry does NOT write wScriptVar
                return r;
            }
            
            case SPECIAL_SET_PLAYER_PALETTE: {
                // SetPlayerPalette - change player sprite visual appearance from wScriptVar
                // Source: pokecrystal/engine/overworld/map_objects.asm _SetPlayerPalette
                //
                // Source-proven semantics from _SetPlayerPalette:
                //   ld a, d
                //   and 1 << 7     ; Check bit 7
                //   ret z          ; No-op if bit 7 not set
                //   ...
                //   swap a         ; Swap nibbles
                //   and OAM_PALETTE; OAM_PALETTE = %00000_111 = 0x07
                //   ...            ; Apply selector to wPlayerStruct.OBJECT_PALETTE
                //
                // Source contract:
                //   - Input with bit 7 clear: no-op (routine returns immediately)
                //   - Input with bit 7 set: extract 3-bit selector (0-7) and apply
                //   - Extraction: (input >> 4) & 0x07 (or equivalently: swap & 0x07)
                //   - ALL selectors 0-7 are source-valid; do not reject merely because
                //     vanilla corpus only uses 0 and 1
                //
                // Vanilla Crystal usage (corpus-observed):
                //   - Selector 0 (0x80): Normal player colors (PAL_NPC_RED << 4)
                //   - Selector 1 (0x90): Team Rocket disguise (PAL_NPC_BLUE << 4)
                //   - Selectors 2-7: Not used in vanilla corpus, but source-valid
                //
                // ROM hacks may use any selector 0-7. The frontend must not reject
                // source-valid values merely because vanilla doesn't use them.
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
                    // The source routine would be a no-op; fall through to Sem_Special
                    break;
                }
                
                // Normalize Crystal encoding to selector (frontend computation)
                // Equivalent to Crystal's `swap a; and OAM_PALETTE` = (raw >> 4) & 0x07
                // ALL selectors 0-7 are source-valid
                uint8_t selector = (raw_palette >> 4) & 0x07;
                
                enginemon::Sem_SetPlayerPalette op;
                op.selector = selector;
                r.instructions.push_back(make_inst(std::move(op)));
                // Context preserved - SetPlayerPalette does NOT write wScriptVar
                return r;
            }
            
            default:
                break;
        }

        // =================================================================
        // GAME-SPECIFIC BEHAVIORS — Sem_GameSpecificEvent
        // =================================================================
        // All remaining unhandled Crystal Specials are lowered to
        // Sem_GameSpecificEvent using the canonical BEHAVIOR_TABLE from
        // crystal/script/behavior_table.hpp.
        //
        // Source authority: pokecrystal/data/events/special_pointers.asm
        // Table is shared with CompiledGameData::behavior_names (built at
        // compile time) and the Stage 5 legality gate (validates names).
        // =================================================================
        {
            for (std::size_t i = 0; i < BEHAVIOR_TABLE_SIZE; ++i) {
                const auto& e = BEHAVIOR_TABLE[i];
                if (p->special_id == e.special_id) {
                    enginemon::Sem_GameSpecificEvent op;
                    op.behavior_name = e.behavior_name;
                    op.writes_script_var = e.writes_script_var;
                    r.instructions.push_back(make_inst(std::move(op)));
                    if (e.writes_script_var) {
                        ctx.block_ctx.invalidate();
                    }
                    return r;
                }
            }
        }
        
        // No lowering rule matched for this Special ID.
        // UnloweredDiagnostic and increment commands_unlowered, which will
        // cause the legality gate to reject this script.
        //
        // Sem_Special is NOT a valid fallback: it carries raw Crystal Special
        // table identity and must not reach the package or runtime.
        //
        // To add support for a new Special, add a named case above with a
        // source-proven semantic operation, then add a corpus test.
        return {};
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
        
        // Source-proven: Script_pokepic (scripting.asm ~line 411):
        //   and a; jr nz, .ok; ld a, [wScriptVar]   ← operand == 0 → dynamic species
        if (p->pokemon == 0) {
            op.source = enginemon::SpeciesSource::from_script_var();
        } else {
            op.source = enginemon::SpeciesSource::literal(enginemon::SpeciesId{p->pokemon});
        }
        // pokepic does NOT write wScriptVar — block_ctx preserved
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
        // checksave writes wScriptVar (save status result)
        // Source: Script_checksave (scripting.asm): ld [wScriptVar], a
        ctx.block_ctx.invalidate();
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
    
    if (auto* p = std::get_if<Cmd_Reanchormap>(&cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        // reanchormap is DISTINCT from refreshmap:
        // - reanchormap (0x48): calls ReanchorMap, consumes dummy byte
        // - refreshmap (0x7C): BGMapMode→0, UpdateSprites, DelayFrame
        // They are NOT interchangeable - must remain distinct
        enginemon::Sem_ReanchorMap op;
        op.dummy = p->dummy;  // Preserve dummy byte for round-trip
        r.instructions.push_back(make_inst(std::move(op)));
        return r;
    }
    return {};
}

RuleResult rule_menu_ops(LoweringContext& ctx) {
    const auto* cmd = ctx.peek();
    if (!cmd) return {};
    
    // loadmenu (0x4F): Loads menu via bank-local menu-header pointer.
    // Source: Script_loadmenu reads 2-byte pointer, calls LoadMenuHeader(wScriptBank:HL).
    // The menu header data is at that pointer and defines items, cursor, layout.
    // Result is stored implicitly in Crystal menu state (wMenuCursorY etc).
    // DISTINCT from verticalmenu/2dmenu: uses LoadMenuHeader, not VerticalMenu/_2DMenu.
    if (auto* p = std::get_if<Cmd_Loadmenu>(&cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        enginemon::Sem_LoadMenu op;
        // Resolve bank-local menu-header pointer to flat ROM address
        const uint32_t entry = ctx.source_ir ? ctx.source_ir->entry_address : 0;
        op.header_pointer = crystal_local_ptr_to_flat(entry, p->menu_header);
        r.instructions.push_back(make_inst(std::move(op)));
        return r;
    }
    
    // verticalmenu (0x59): Runs VerticalMenu, writes wMenuCursorY to wScriptVar.
    // Source: Script_verticalmenu calls VerticalMenu, reads wMenuCursorY (not wMenuCursorPosition).
    // On cancel (carry set), result is 0.
    // DISTINCT from _2dmenu: reads different result register.
    if (std::holds_alternative<Cmd_Verticalmenu>(cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        r.instructions.push_back(make_inst(enginemon::Sem_VerticalMenu{}));
        // verticalmenu writes wScriptVar (wMenuCursorY result)
        // Source: Script_verticalmenu (scripting.asm): ld [wScriptVar], a
        ctx.block_ctx.invalidate();
        return r;
    }
    
    // _2dmenu (0x58): Runs _2DMenu, writes wMenuCursorPosition to wScriptVar.
    // Source: Script__2dmenu calls _2DMenu, reads wMenuCursorPosition (not wMenuCursorY).
    // On cancel (carry set), result is 0.
    // DISTINCT from verticalmenu: reads different result register.
    if (std::holds_alternative<Cmd_2dmenu>(cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        r.instructions.push_back(make_inst(enginemon::Sem_2DMenu{}));
        // _2dmenu writes wScriptVar (wMenuCursorPosition result)
        // Source: Script__2dmenu (scripting.asm): ld [wScriptVar], a
        ctx.block_ctx.invalidate();
        return r;
    }
    return {};
}

RuleResult rule_string_format(LoweringContext& ctx) {
    const auto* cmd = ctx.peek();
    if (!cmd) return {};
    
    // ==========================================================================
    // String formatting commands prepare values for text substitution
    // CRITICAL: Every operand MUST be preserved - dropping operands is corruption
    // ==========================================================================
    
    // getmonname - prepare Pokemon species name
    // Source: Script_getmonname reads pokemon, then strbuf
    // ROM layout: opcode, strbuf, pokemon (decoder verified)
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
    // Source: Script_getitemname reads item, then strbuf
    // ROM layout: opcode, strbuf, item (decoder verified)
    if (auto* p = std::get_if<Cmd_Getitemname>(&cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        auto op = enginemon::Sem_PrepareTextArg::item_name(
            enginemon::ItemId{p->item}, p->strbuf);
        r.instructions.push_back(make_inst(std::move(op)));
        return r;
    }
    
    // gettrainername - prepare trainer name from group+id
    // Source: Script_gettrainername reads trainer_group, trainer_id, then strbuf
    // ROM layout: opcode, trainer_group, trainer_id, strbuf (decoder verified)
    // BOTH group AND id are required for correct trainer lookup
    if (auto* p = std::get_if<Cmd_Gettrainername>(&cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        auto op = enginemon::Sem_PrepareTextArg::trainer_name(
            p->trainer_group, p->trainer_id, p->strbuf);
        r.instructions.push_back(make_inst(std::move(op)));
        return r;
    }
    
    // getstring - prepare string from ROM pointer
    // Source: Script_getstring reads text_pointer (2 bytes), then strbuf
    // ROM layout: opcode, strbuf, pointer_lo, pointer_hi
    // The text_pointer references a text resource - resolve at lowering time
    if (auto* p = std::get_if<Cmd_Getstring>(&cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        
        // Resolve the text pointer to flat address using the COMMAND's own ROM address.
        // Source-proven: Script_getstring (pokecrystal scripting.asm:1688) reads
        // wScriptBank (the currently executing script's bank) — equivalent to the
        // command's own bank, which is correct across farsjump target blocks.
        const uint32_t flat_addr = crystal_local_ptr_to_flat(cmd->span.rom_address, p->text_pointer);
        
        // Extract text content through registry if available
        std::string resolved_text;
        if (ctx.text_registry && flat_addr != 0) {
            auto text_id = ctx.text_registry->extract(flat_addr);
            if (text_id != enginemon::TEXT_NONE) {
                const auto* def = ctx.text_registry->get(text_id);
                if (def) {
                    // Get display text from sequence
                    for (const auto& elem : def->sequence.elements) {
                        if (elem.op == TextOp::Text) {
                            resolved_text += elem.text;
                        }
                    }
                }
            }
        }
        
        auto op = enginemon::Sem_PrepareTextArg::string_from_resolved(
            resolved_text, p->strbuf);
        r.instructions.push_back(make_inst(std::move(op)));
        return r;
    }
    
    // getmoney - prepare money amount (account specifies player vs mom)
    // Source: Script_getmoney reads account, then strbuf
    // ROM layout: opcode, account, strbuf (SWAPPED from macro - decoder verified)
    if (auto* p = std::get_if<Cmd_Getmoney>(&cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        auto op = enginemon::Sem_PrepareTextArg::money(p->account, p->strbuf);
        r.instructions.push_back(make_inst(std::move(op)));
        return r;
    }
    
    // getcoins - prepare coin amount (always player's coins)
    // Source: Script_getcoins reads strbuf only
    // ROM layout: opcode, strbuf
    if (auto* p = std::get_if<Cmd_Getcoins>(&cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        auto op = enginemon::Sem_PrepareTextArg::coins(p->strbuf);
        r.instructions.push_back(make_inst(std::move(op)));
        return r;
    }
    
    // getnum - prepare number from wScriptVar
    // Source: Script_getnum reads strbuf only, formats wScriptVar
    // ROM layout: opcode, strbuf
    if (auto* p = std::get_if<Cmd_Getnum>(&cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        auto op = enginemon::Sem_PrepareTextArg::number_from_var(p->strbuf);
        r.instructions.push_back(make_inst(std::move(op)));
        return r;
    }
    
    // getcurlandmarkname - prepare current map's landmark name
    // Source: Script_getcurlandmarkname reads strbuf, uses current map's landmark
    // ROM layout: opcode, strbuf
    if (auto* p = std::get_if<Cmd_Getcurlandmarkname>(&cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        auto op = enginemon::Sem_PrepareTextArg::current_landmark_name(p->strbuf);
        r.instructions.push_back(make_inst(std::move(op)));
        return r;
    }
    
    // getlandmarkname - prepare landmark name by ID
    // Source: Script_getlandmarkname reads landmark_id, then strbuf
    // ROM layout: opcode, strbuf, landmark_id (decoder verified)
    if (auto* p = std::get_if<Cmd_Getlandmarkname>(&cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        auto op = enginemon::Sem_PrepareTextArg::landmark_name(p->landmark_id, p->strbuf);
        r.instructions.push_back(make_inst(std::move(op)));
        return r;
    }
    
    // gettrainerclassname - prepare trainer class name
    // Source: Script_gettrainerclassname reads trainer_group, then strbuf
    // ROM layout: opcode, strbuf, trainer_group (decoder verified)
    if (auto* p = std::get_if<Cmd_Gettrainerclassname>(&cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        auto op = enginemon::Sem_PrepareTextArg::trainer_class_name(p->trainer_group, p->strbuf);
        r.instructions.push_back(make_inst(std::move(op)));
        return r;
    }
    
    // getname - generic name lookup by type and ID
    // Source: Script_getname reads type, then id, then strbuf
    // ROM layout: opcode, strbuf, type, id (decoder verified)
    // Authoritative type mapping from pokecrystal/constants/text_constants.asm:
    //   1 = MON_NAME, 2 = MOVE_NAME, 3 = DUMMY_NAME, 4 = ITEM_NAME,
    //   5 = PARTY_OT_NAME, 6 = ENEMY_OT_NAME, 7 = TRAINER_NAME,
    //   8 = MOVE_DESC_NAME_BROKEN
    if (auto* p = std::get_if<Cmd_Getname>(&cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        
        // Map Crystal NAME_* constant directly to NameSourceType enum value
        // NameSourceType values are defined to match Crystal constants exactly.
        // Types outside [1,8] or reserved (3=DUMMY, 8=MOVE_DESC_BROKEN) are
        // preserved with their explicit enum values — not mapped to a random domain.
        enginemon::NameSourceType name_type;
        switch (p->type) {
            case 1: name_type = enginemon::NameSourceType::Pokemon;      break; // MON_NAME
            case 2: name_type = enginemon::NameSourceType::Move;         break; // MOVE_NAME
            case 3: name_type = enginemon::NameSourceType::Dummy;        break; // DUMMY_NAME (reserved)
            case 4: name_type = enginemon::NameSourceType::Item;         break; // ITEM_NAME
            case 5: name_type = enginemon::NameSourceType::PartyOT;      break; // PARTY_OT_NAME
            case 6: name_type = enginemon::NameSourceType::EnemyOT;      break; // ENEMY_OT_NAME
            case 7: name_type = enginemon::NameSourceType::Trainer;      break; // TRAINER_NAME
            case 8: name_type = enginemon::NameSourceType::MoveDescBroken; break; // MOVE_DESC_NAME_BROKEN
            default: name_type = enginemon::NameSourceType::Dummy; break; // Unknown → dummy, don't guess
        }
        
        auto op = enginemon::Sem_PrepareTextArg::name_by_type(name_type, p->id, p->strbuf);
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
    
    // pocketisfull - display pocket-is-full notification text
    // Source: pokecrystal/engine/overworld/scripting.asm Script_pocketisfull (opcode 0x46):
    //   call GetPocketName    ; get pocket name from current item context
    //   call CurItemName      ; get current item name
    //   call MapTextbox       ; DISPLAYS "The BAG pocket is full." notification
    //   ret
    //
    // Source-proven: Script_pocketisfull does NOT write wScriptVar.
    // There is NO "ld [wScriptVar], a" in the implementation.
    // Block-local context is PRESERVED across this operation.
    //
    // This is a user-visible text display, NOT an inventory capacity check.
    // The script uses it to notify the player that a pocket is full, then
    // continues without a conditional branch on a wScriptVar result.
    //
    // Vanilla pattern: appears after giveitem/verbosegiveitem in full-pocket paths.
    // The preceding giveitem/verbosegiveitem already writes wScriptVar with the
    // give result (FALSE=bag full), so pocketisfull is purely a presentation follow-up.
    if (std::holds_alternative<Cmd_Pocketisfull>(cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        // Emit as Sem_PocketFullNotify — a distinct, user-visible presentation operation.
        // Block-local context NOT invalidated (confirmed: no wScriptVar write in source).
        r.instructions.push_back(make_inst(enginemon::Sem_PocketFullNotify{}));
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
        // deactivatefacing uses SCRIPT_WAIT/StopScript suspension — distinct from Sem_Pause
        // Source: Script_deactivatefacing sets wScriptMode=SCRIPT_WAIT, calls StopScript
        // If time==0: delay NOT stored (wScriptDelay unchanged), but SCRIPT_WAIT still set
        enginemon::Sem_DeactivateFacing op;
        op.duration = p->time;
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
        // encountermusic plays trainer-class-specific music
        // DISTINCT from playmapmusic which plays the map's background music
        // Source: Script_encountermusic calls PlayTrainerEncounterMusic with wOtherTrainerClass
        r.instructions.push_back(make_inst(enginemon::Sem_PlayEncounterMusic{}));
        return r;
    }
    return {};
}

// --- Additional Misc Rules ---

// verbosegiveitemvar: Give item with message, item/quantity from variables
// Reference: pokecrystal/engine/overworld/scripting.asm Script_verbosegiveitemvar
// Semantics:
//   first byte == ITEM_FROM_MEM (0) → item ID comes from wScriptVar at runtime
//   first byte != 0               → literal item ID
//   second byte = quantity variable index (via GetVarAction table, NOT a literal)
// Writes wScriptVar: TRUE=given, FALSE=bag full
RuleResult rule_verbose_give_item_var(LoweringContext& ctx) {
    const auto* cmd = ctx.peek();
    if (!cmd) return {};
    
    if (auto* p = std::get_if<Cmd_Verbosegiveitemvar>(&cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        enginemon::Sem_GiveItemVerboseVar op;
        // ITEM_FROM_MEM = 0: item ID read from wScriptVar at runtime
        if (p->item == 0) {
            op.item_source = enginemon::ItemSource::FromScriptVar;
            op.item = enginemon::ItemId{0};  // Not used when FromScriptVar
        } else {
            op.item_source = enginemon::ItemSource::Literal;
            op.item = enginemon::ItemId{p->item};
        }
        // Second byte is a variable INDEX, not a literal quantity
        // GetVarAction(var_index) → wram pointer → quantity read at runtime
        op.quantity_var = p->var;
        r.instructions.push_back(make_inst(std::move(op)));
        // verbosegiveitemvar writes wScriptVar (TRUE=given, FALSE=bag full)
        // Source: Script_verbosegiveitemvar (scripting.asm): ld [wScriptVar], a
        // Same result contract as verbosegiveitem — must invalidate block_ctx.
        // Note: the item operand may also read wScriptVar (ITEM_FROM_MEM=0 case),
        // but that is a read before the write, so the write still invalidates.
        ctx.block_ctx.invalidate();
        return r;
    }
    return {};
}

// newloadmap: Load map with specific method (0-3)
// Reference: pokecrystal/engine/overworld/scripting.asm Script_newloadmap
// Semantics: triggers map reload with entry method - MUST preserve method byte
RuleResult rule_new_load_map(LoweringContext& ctx) {
    const auto* cmd = ctx.peek();
    if (!cmd) return {};
    
    if (auto* p = std::get_if<Cmd_Newloadmap>(&cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        // newloadmap entry method affects spawn, fade, music, and other behaviors
        // Method is NOT optional - it must be preserved for correct runtime behavior
        enginemon::Sem_NewLoadMap op;
        op.method = static_cast<enginemon::MapEntryMethod>(p->method);
        r.instructions.push_back(make_inst(std::move(op)));
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
// Semantics: YesNo prompt → conditional add → 3-way result in wScriptVar
//   0 = PHONE_CONTACT_GOT (accepted, added)
//   PHONE_CONTACTS_FULL (accepted, list full)
//   PHONE_CONTACT_REFUSED (declined)
// DISTINCT from Sem_AddPhoneNumber which is unconditional (no prompt, no result)
RuleResult rule_ask_phone_number(LoweringContext& ctx) {
    const auto* cmd = ctx.peek();
    if (!cmd) return {};
    
    if (auto* p = std::get_if<Cmd_Askforphonenumber>(&cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        // Preserve the compound semantics (prompt + conditional add + result)
        enginemon::Sem_AskForPhoneNumber op;
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
        // checkver writes wScriptVar = 1 (GS_VERSION for Crystal) — update block_ctx
        // Source: Script_checkver (scripting.asm): ld [wScriptVar], a (always 1)
        ctx.block_ctx.on_setval(1);
        return r;
    }
    return {};
}

// catchtutorial: Run catch tutorial battle
// Reference: pokecrystal/engine/overworld/scripting.asm Script_catchtutorial
// Semantics: sets wBattleType from byte operand, calls CatchTutorial, then reloads map
// DISTINCT from Sem_StartBattle — uses tutorial entry path, not normal battle entry
// Post-tutorial map reload is implicit in Crystal (jp Script_reloadmap after CatchTutorial)
RuleResult rule_catch_tutorial(LoweringContext& ctx) {
    const auto* cmd = ctx.peek();
    if (!cmd) return {};
    
    if (auto* p = std::get_if<Cmd_Catchtutorial>(&cmd->data)) {
        RuleResult r;
        r.matched = true;
        r.consumed = 1;
        // Preserve tutorial type byte — this is NOT Sem_StartBattle
        // Source: Script_catchtutorial ld [wBattleType], a (byte operand)
        enginemon::Sem_CatchTutorial op;
        op.tutorial_type = p->byte;
        r.instructions.push_back(make_inst(std::move(op)));
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
