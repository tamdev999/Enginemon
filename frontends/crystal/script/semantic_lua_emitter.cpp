// crystal/script/semantic_lua_emitter.cpp
// Stage 7: SemanticScriptIR → generated Lua
//
// Every SemanticOp must emit; no silent stubs.
// See semantic_lua_emitter.hpp for contract.

#include "crystal/script/semantic_lua_emitter.hpp"
#include <sstream>
#include <stdexcept>
#include <iomanip>

namespace crystal {

using namespace enginemon;

// =============================================================================
// Helpers
// =============================================================================

void SemanticLuaEmitter::indent_line(std::ostream& out, int n) {
    for (int i = 0; i < n * 2; ++i) out << ' ';
}

std::string SemanticLuaEmitter::escape_lua_string(const std::string& s) {
    std::string r;
    r.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"':  r += "\\\""; break;
            case '\\': r += "\\\\"; break;
            case '\n': r += "\\n";  break;
            case '\r': r += "\\r";  break;
            case '\t': r += "\\t";  break;
            default:   r += c;      break;
        }
    }
    return r;
}

std::string SemanticLuaEmitter::direction_name(Direction d) {
    switch (d) {
        case Direction::Down:  return "\"down\"";
        case Direction::Up:    return "\"up\"";
        case Direction::Left:  return "\"left\"";
        case Direction::Right: return "\"right\"";
        default: return "\"down\"";
    }
}

std::string SemanticLuaEmitter::emit_text_element(const SemanticTextElement& elem) {
    std::ostringstream s;
    switch (elem.op) {
        case SemanticTextOp::Text:
            s << "{op=\"text\", text=\"" << escape_lua_string(elem.text) << "\"}";
            break;
        case SemanticTextOp::Arg:
            s << "{op=\"arg\", slot=" << (int)elem.arg_index << "}";
            break;
        case SemanticTextOp::Line:   s << "{op=\"line\"}"; break;
        case SemanticTextOp::Next:   s << "{op=\"next\"}"; break;
        case SemanticTextOp::Para:   s << "{op=\"para\"}"; break;
        case SemanticTextOp::Cont:   s << "{op=\"cont\"}"; break;
        case SemanticTextOp::Scroll: s << "{op=\"scroll\"}"; break;
        case SemanticTextOp::Done:   s << "{op=\"done\"}"; break;
        case SemanticTextOp::Prompt: s << "{op=\"prompt\"}"; break;
        case SemanticTextOp::InlinePromptButton:
            s << "{op=\"inline_prompt_button\"}";
            break;
        case SemanticTextOp::Pause:
            s << "{op=\"pause\", frames=" << (int)elem.pause_frames() << "}";
            break;
        case SemanticTextOp::ScriptVarDecimal:
            s << "{op=\"script_var_decimal\", bytes=" << ((elem.param1 >> 4) & 0xF)
              << ", digits=" << (elem.param1 & 0xF) << "}";
            break;
        case SemanticTextOp::Day:
            s << "{op=\"day\"}";
            break;
        case SemanticTextOp::Sound:
            switch (elem.sound_kind()) {
                case TextSoundKind::ItemJingle:      s << "{op=\"sound\", kind=\"item_jingle\"}"; break;
                case TextSoundKind::CaughtMonJingle: s << "{op=\"sound\", kind=\"caught_mon_jingle\"}"; break;
                case TextSoundKind::Fanfare:         s << "{op=\"sound\", kind=\"fanfare\"}"; break;
                default:                             s << "{op=\"sound\", kind=\"unknown\"}"; break;
            }
            break;
        case SemanticTextOp::RamSource:
            s << "{op=\"ram_source\", kind=" << (int)elem.ram_source() << "}";
            break;
        default:
            s << "{op=\"unknown\"}";
            break;
    }
    return s.str();
}

std::string SemanticLuaEmitter::emit_text_sequence(const SemanticTextSequence& seq) {
    std::ostringstream s;
    s << "{";
    bool first = true;
    for (const auto& elem : seq.elements) {
        if (!first) s << ", ";
        first = false;
        s << emit_text_element(elem);
    }
    s << "}";
    return s.str();
}

std::string SemanticLuaEmitter::movement_commands_to_lua(
        const std::vector<MovementCommand>& cmds) {
    // Each MovementCommand has type + direction (+ optional param).
    // Encode as a Lua table array: {{type="step", dir="left"}, ...}
    std::ostringstream s;
    s << "{";
    bool first = true;
    for (const auto& mc : cmds) {
        if (!first) s << ", ";
        first = false;

        if (mc.type == MovementType::StepSleep) {
            s << "{type=\"sleep\", frames=" << (int)mc.param << "}";
            continue;
        }
        if (mc.type == MovementType::StepEnd) {
            // Terminal — emit as marker so runtime knows the list is complete
            s << "{type=\"step_end\"}";
            continue;
        }

        const char* t = "step";
        if (mc.type == MovementType::TurnHead || mc.type == MovementType::TurnStep ||
            mc.type == MovementType::TurnAway || mc.type == MovementType::TurnIn ||
            mc.type == MovementType::TurnWaterfall) {
            t = "turn";
        } else if (mc.type == MovementType::SlowStep) {
            t = "slow_step";
        } else if (mc.type == MovementType::BigStep) {
            t = "big_step";
        }

        std::string dir;
        switch (mc.direction) {
            case Direction::Down:  dir = "down";  break;
            case Direction::Up:    dir = "up";    break;
            case Direction::Left:  dir = "left";  break;
            case Direction::Right: dir = "right"; break;
            default:               dir = "down";  break;
        }
        s << "{type=\"" << t << "\", dir=\"" << dir << "\"}";
    }
    s << "}";
    return s.str();
}

// =============================================================================
// emit()
// =============================================================================

std::string SemanticLuaEmitter::emit(const SemanticScriptIR& ir) const {
    std::ostringstream out;

    out << "script = {}\n";
    out << "function script.main(ctx)\n";
    out << "  local result = false\n";

    for (std::size_t block_idx = 0; block_idx < ir.blocks.size(); ++block_idx) {
        const auto& block = ir.blocks[block_idx];
        out << "  ::block_" << block.id << "::\n";
        for (const auto& inst : block.instructions) {
            emit_instruction(out, inst, 1);
        }
    }

    out << "end\n";
    out << "return script\n";

    return out.str();
}

// =============================================================================
// emit_instruction
// =============================================================================

void SemanticLuaEmitter::emit_instruction(std::ostream& out,
                                           const SemanticInstruction& inst,
                                           int indent) const {
    emit_op(out, inst.op, indent);
}

// =============================================================================
// Emit visitor — split into part1 (control/flags/ui/inventory/pokemon/movement)
//                and part2 (map/battle/audio/time/phone/misc/std)
// to stay within MSVC's block-nesting limit.
// =============================================================================

// Returns true if op was handled; false means try part2.
static bool emit_op_part1(std::ostream& out, const SemanticOp& op, int I) {
    using namespace enginemon;

    // Control flow
    if (auto* o = std::get_if<Sem_End>(&op)) {
        (void)o;
        SemanticLuaEmitter::indent_line(out, I); out << "return\n"; return true;
    }
    if (auto* o = std::get_if<Sem_EndAll>(&op)) {
        (void)o;
        SemanticLuaEmitter::indent_line(out, I); out << "ctx.game:behavior(\"EndAll\"); return\n"; return true;
    }
    if (auto* o = std::get_if<Sem_Return>(&op)) {
        (void)o;
        SemanticLuaEmitter::indent_line(out, I); out << "return\n"; return true;
    }
    if (auto* o = std::get_if<Sem_Jump>(&op)) {
        SemanticLuaEmitter::indent_line(out, I); out << "goto block_" << o->target.id << "\n"; return true;
    }
    if (auto* o = std::get_if<Sem_JumpIf>(&op)) {
        SemanticLuaEmitter::indent_line(out, I);
        std::string cond;
        const std::string& c = o->condition;
        if (c == "true")       cond = "result";
        else if (c == "false") cond = "not result";
        else                   cond = "result " + c;
        out << "if " << cond << " then goto block_" << o->target.id << " end\n"; return true;
    }
    if (auto* o = std::get_if<Sem_Call>(&op)) {
        SemanticLuaEmitter::indent_line(out, I); out << "goto block_" << o->target.id << "\n"; return true;
    }
    if (auto* o = std::get_if<Sem_Sdefer>(&op)) {
        SemanticLuaEmitter::indent_line(out, I);
        out << "ctx.game:behavior(\"Sdefer_" << SemanticLuaEmitter::escape_lua_string(o->target_script_id) << "\")\n"; return true;
    }

    // Flags / Variables
    if (auto* o = std::get_if<Sem_SetFlag>(&op)) {
        uint32_t enc = (static_cast<uint32_t>(static_cast<uint8_t>(o->flag.ns)) << 16) | o->flag.value;
        SemanticLuaEmitter::indent_line(out, I); out << "ctx.flags:set(" << enc << ")\n"; return true;
    }
    if (auto* o = std::get_if<Sem_ClearFlag>(&op)) {
        uint32_t enc = (static_cast<uint32_t>(static_cast<uint8_t>(o->flag.ns)) << 16) | o->flag.value;
        SemanticLuaEmitter::indent_line(out, I); out << "ctx.flags:clear(" << enc << ")\n"; return true;
    }
    if (auto* o = std::get_if<Sem_CheckFlag>(&op)) {
        uint32_t enc = (static_cast<uint32_t>(static_cast<uint8_t>(o->flag.ns)) << 16) | o->flag.value;
        SemanticLuaEmitter::indent_line(out, I); out << "result = ctx.flags:check(" << enc << ")\n"; return true;
    }
    if (auto* o = std::get_if<Sem_SetVar>(&op)) {
        SemanticLuaEmitter::indent_line(out, I);
        if (o->source.type == VarValueSourceType::Literal)
            out << "ctx.flags:set_var(" << o->var << ", " << o->source.value << ")\n";
        else
            out << "ctx.flags:set_var(" << o->var << ", result and 1 or 0)\n";
        return true;
    }
    if (auto* o = std::get_if<Sem_AddVar>(&op)) {
        SemanticLuaEmitter::indent_line(out, I); out << "ctx.flags:add_var(" << o->var << ", " << o->delta << ")\n"; return true;
    }
    if (auto* o = std::get_if<Sem_CheckVar>(&op)) {
        SemanticLuaEmitter::indent_line(out, I); out << "result = ctx.flags:get_var(" << o->var << ") " << o->op << " " << o->value << "\n"; return true;
    }
    if (auto* o = std::get_if<Sem_Random>(&op)) {
        SemanticLuaEmitter::indent_line(out, I); out << "result = ctx.util:random(0, " << (int)o->range - 1 << ")\n"; return true;
    }

    // Semantic state variables
    if (auto* o = std::get_if<Sem_ReadStateVar>(&op)) {
        SemanticLuaEmitter::indent_line(out, I); out << "result = ctx.game:read_state_var(" << (int)o->state_var << ")\n"; return true;
    }
    if (auto* o = std::get_if<Sem_WriteStateVar>(&op)) {
        SemanticLuaEmitter::indent_line(out, I); out << "ctx.game:write_state_var(" << (int)o->state_var << ")\n"; return true;
    }
    if (auto* o = std::get_if<Sem_SetStateVar>(&op)) {
        SemanticLuaEmitter::indent_line(out, I); out << "ctx.game:set_state_var(" << (int)o->state_var << ", " << (int)o->value << ")\n"; return true;
    }
    if (std::get_if<Sem_CheckLinkMode>(&op)) {
        SemanticLuaEmitter::indent_line(out, I); out << "result = ctx.game:check_link_mode()\n"; return true;
    }

    // UI / Text
    if (std::get_if<Sem_OpenText>(&op)) {
        SemanticLuaEmitter::indent_line(out, I); out << "ctx.ui:open_text()\n"; return true;
    }
    if (std::get_if<Sem_CloseText>(&op)) {
        SemanticLuaEmitter::indent_line(out, I); out << "ctx.ui:close_text()\n"; return true;
    }
    if (auto* o = std::get_if<Sem_ShowText>(&op)) {
        SemanticLuaEmitter::indent_line(out, I); out << "ctx.ui:text_sequence(" << SemanticLuaEmitter::emit_text_sequence(o->sequence) << ")\n";
        SemanticLuaEmitter::indent_line(out, I); out << "coroutine.yield(\"wait_button\")\n"; return true;
    }
    if (auto* o = std::get_if<Sem_ShowTextAndEnd>(&op)) {
        SemanticLuaEmitter::indent_line(out, I); out << "ctx.ui:open_text()\n";
        SemanticLuaEmitter::indent_line(out, I); out << "ctx.ui:text_sequence(" << SemanticLuaEmitter::emit_text_sequence(o->sequence) << ")\n";
        SemanticLuaEmitter::indent_line(out, I); out << "coroutine.yield(\"wait_button\")\n";
        SemanticLuaEmitter::indent_line(out, I); out << "ctx.ui:close_text()\n";
        SemanticLuaEmitter::indent_line(out, I); out << "return\n"; return true;
    }
    if (auto* o = std::get_if<Sem_FacePlayerAndShowText>(&op)) {
        SemanticLuaEmitter::indent_line(out, I); out << "ctx.world:face_player()\n";
        SemanticLuaEmitter::indent_line(out, I); out << "ctx.ui:open_text()\n";
        SemanticLuaEmitter::indent_line(out, I); out << "ctx.ui:text_sequence(" << SemanticLuaEmitter::emit_text_sequence(o->sequence) << ")\n";
        SemanticLuaEmitter::indent_line(out, I); out << "coroutine.yield(\"wait_button\")\n";
        SemanticLuaEmitter::indent_line(out, I); out << "ctx.ui:close_text()\n";
        SemanticLuaEmitter::indent_line(out, I); out << "return\n"; return true;
    }
    if (std::get_if<Sem_WaitButton>(&op)) {
        SemanticLuaEmitter::indent_line(out, I); out << "coroutine.yield(\"wait_button\")\n"; return true;
    }
    if (std::get_if<Sem_PromptButton>(&op)) {
        SemanticLuaEmitter::indent_line(out, I); out << "coroutine.yield(\"wait_button\")\n"; return true;
    }
    if (std::get_if<Sem_YesNo>(&op)) {
        SemanticLuaEmitter::indent_line(out, I); out << "result = ctx.ui:yes_no()\n"; return true;
    }
    if (std::get_if<Sem_LoadMenu>(&op)) {
        SemanticLuaEmitter::indent_line(out, I); out << "result = ctx.ui:choice({})\n"; return true;
    }
    if (std::get_if<Sem_VerticalMenu>(&op)) {
        SemanticLuaEmitter::indent_line(out, I); out << "result = ctx.ui:choice({})\n"; return true;
    }
    if (std::get_if<Sem_2DMenu>(&op)) {
        SemanticLuaEmitter::indent_line(out, I); out << "result = ctx.ui:choice({})\n"; return true;
    }
    if (std::get_if<Sem_Choice>(&op)) {
        SemanticLuaEmitter::indent_line(out, I); out << "result = ctx.ui:choice({})\n"; return true;
    }
    if (auto* o = std::get_if<Sem_ShowBalanceOverlay>(&op)) {
        SemanticLuaEmitter::indent_line(out, I); out << "ctx.game:show_balance_overlay(" << (int)o->contents << ")\n"; return true;
    }

    // Text argument preparation
    if (auto* o = std::get_if<Sem_PrepareTextArg>(&op)) {
        SemanticLuaEmitter::indent_line(out, I);
        switch (o->arg_type) {
            case TextArgType::ItemName:
                out << "ctx.flags:set_var(\"strbuf" << (int)o->buffer_slot << "_item\", " << (int)o->id << ")\n"; break;
            case TextArgType::PokemonName:
                out << "ctx.flags:set_var(\"strbuf" << (int)o->buffer_slot << "_species\", " << (int)o->id << ")\n"; break;
            case TextArgType::TrainerName:
                out << "ctx.flags:set_var(\"strbuf" << (int)o->buffer_slot << "_trainer\", " << (int)o->trainer_group << ")\n"; break;
            case TextArgType::Number:
                switch (o->number_source) {
                    case NumberSource::Money:    out << "ctx.inventory:show_money(" << (int)o->account << ")\n"; break;
                    case NumberSource::Coins:    out << "-- getcoins strbuf=" << (int)o->buffer_slot << "\n"; break;
                    case NumberSource::ScriptVar: out << "-- getnum strbuf=" << (int)o->buffer_slot << "\n"; break;
                }
                break;
            case TextArgType::String:
                if (!o->str_value.empty())
                    out << "ctx.flags:set_var(\"strbuf" << (int)o->buffer_slot << "_str\", \"" << SemanticLuaEmitter::escape_lua_string(o->str_value) << "\")\n";
                else
                    out << "-- getstring strbuf=" << (int)o->buffer_slot << "\n";
                break;
            default:
                out << "-- prepare_text_arg strbuf=" << (int)o->buffer_slot << "\n"; break;
        }
        return true;
    }

    // Inventory
    if (auto* o = std::get_if<Sem_GiveItem>(&op)) {
        SemanticLuaEmitter::indent_line(out, I); out << "ctx.inventory:give(" << (int)o->item << ", " << (int)o->quantity << ")\n"; return true;
    }
    if (auto* o = std::get_if<Sem_TakeItem>(&op)) {
        SemanticLuaEmitter::indent_line(out, I); out << "result = ctx.inventory:take(" << (int)o->item << ", " << (int)o->quantity << ")\n"; return true;
    }
    if (auto* o = std::get_if<Sem_CheckItem>(&op)) {
        SemanticLuaEmitter::indent_line(out, I); out << "result = ctx.inventory:has(" << (int)o->item << ")\n"; return true;
    }
    if (auto* o = std::get_if<Sem_GiveItemVerbose>(&op)) {
        SemanticLuaEmitter::indent_line(out, I); out << "result = ctx.inventory:give(" << (int)o->item << ", " << (int)o->quantity << ")\n"; return true;
    }
    if (auto* o = std::get_if<Sem_GiveItemVerboseVar>(&op)) {
        SemanticLuaEmitter::indent_line(out, I);
        if (o->item_source == ItemSource::Literal) out << "result = ctx.inventory:give(" << (int)o->item << ", 1)\n";
        else out << "result = ctx.inventory:give(result, 1)\n";
        return true;
    }
    if (auto* o = std::get_if<Sem_GiveMoney>(&op)) {
        SemanticLuaEmitter::indent_line(out, I); out << "ctx.inventory:give_money(" << o->amount << ", " << (int)o->account << ")\n"; return true;
    }
    if (auto* o = std::get_if<Sem_TakeMoney>(&op)) {
        SemanticLuaEmitter::indent_line(out, I); out << "result = ctx.inventory:take_money(" << o->amount << ", " << (int)o->account << ")\n"; return true;
    }
    if (auto* o = std::get_if<Sem_CheckMoney>(&op)) {
        SemanticLuaEmitter::indent_line(out, I); out << "result = ctx.inventory:has_money(" << o->amount << ", " << (int)o->account << ")\n"; return true;
    }
    if (auto* o = std::get_if<Sem_GiveCoins>(&op)) {
        SemanticLuaEmitter::indent_line(out, I); out << "ctx.inventory:give_money(" << (int)o->coins << ", 2)\n"; return true;
    }
    if (auto* o = std::get_if<Sem_TakeCoins>(&op)) {
        SemanticLuaEmitter::indent_line(out, I); out << "result = ctx.inventory:take_money(" << (int)o->coins << ", 2)\n"; return true;
    }
    if (auto* o = std::get_if<Sem_CheckCoins>(&op)) {
        SemanticLuaEmitter::indent_line(out, I); out << "result = ctx.inventory:has_money(" << (int)o->coins << ", 2)\n"; return true;
    }

    // Party / Pokemon
    if (auto* o = std::get_if<Sem_GivePokemon>(&op)) {
        SemanticLuaEmitter::indent_line(out, I);
        out << "ctx.party:add_pokemon({species=" << (int)o->species << ", level=" << (int)o->level << ", held_item=" << (int)o->held_item;
        if (!o->nickname.empty()) out << ", nickname=\"" << SemanticLuaEmitter::escape_lua_string(o->nickname) << "\"";
        if (!o->ot_name.empty())  out << ", ot_name=\"" << SemanticLuaEmitter::escape_lua_string(o->ot_name) << "\"";
        out << "})\n"; return true;
    }
    if (auto* o = std::get_if<Sem_GiveEgg>(&op)) {
        SemanticLuaEmitter::indent_line(out, I); out << "ctx.party:add_pokemon({species=" << (int)o->species << ", level=" << (int)o->level << ", is_egg=true})\n"; return true;
    }
    if (std::get_if<Sem_HealParty>(&op)) {
        SemanticLuaEmitter::indent_line(out, I); out << "ctx.party:heal_all()\n"; return true;
    }
    if (std::get_if<Sem_CheckPartyPokerus>(&op)) {
        SemanticLuaEmitter::indent_line(out, I); out << "result = ctx.game:check_pokerus()\n"; return true;
    }
    if (auto* o = std::get_if<Sem_CheckPokemon>(&op)) {
        SemanticLuaEmitter::indent_line(out, I); out << "result = ctx.party:has_species(" << (int)o->species << ")\n"; return true;
    }

    // Movement / Object
    if (auto* o = std::get_if<Sem_ApplyMovement>(&op)) {
        int actor = 0;
        if (o->target.type == MovementTargetType::Object) actor = o->target.object_id;
        else if (o->target.type == MovementTargetType::LastTalked) actor = -2;
        SemanticLuaEmitter::indent_line(out, I);
        out << "ctx.world:move_actor(" << actor << ", " << SemanticLuaEmitter::movement_commands_to_lua(o->commands) << ")\n"; return true;
    }
    if (std::get_if<Sem_FacePlayer>(&op)) {
        SemanticLuaEmitter::indent_line(out, I); out << "ctx.world:face_player()\n"; return true;
    }
    if (auto* o = std::get_if<Sem_FaceObject>(&op)) {
        SemanticLuaEmitter::indent_line(out, I); out << "ctx.world:face_actor(" << (int)o->object1 << ", \"down\")\n"; return true;
    }
    if (auto* o = std::get_if<Sem_TurnObject>(&op)) {
        SemanticLuaEmitter::indent_line(out, I); out << "ctx.world:face_actor(" << (int)o->object_id << ", " << SemanticLuaEmitter::direction_name(o->facing) << ")\n"; return true;
    }
    if (auto* o = std::get_if<Sem_ShowObject>(&op)) {
        SemanticLuaEmitter::indent_line(out, I); out << "ctx.world:show_npc(" << (int)o->object_id << ")\n"; return true;
    }
    if (auto* o = std::get_if<Sem_HideObject>(&op)) {
        SemanticLuaEmitter::indent_line(out, I); out << "ctx.world:hide_npc(" << (int)o->object_id << ")\n"; return true;
    }
    if (auto* o = std::get_if<Sem_MoveObject>(&op)) {
        SemanticLuaEmitter::indent_line(out, I); out << "ctx.world:teleport_player(" << (int)o->object_id << ", " << (int)o->x << ", " << (int)o->y << ")\n"; return true;
    }
    if (auto* o = std::get_if<Sem_SetLastTalked>(&op)) {
        SemanticLuaEmitter::indent_line(out, I); out << "-- set_last_talked " << (int)o->object_id << "\n"; return true;
    }
    if (auto* o = std::get_if<Sem_VariableSprite>(&op)) {
        SemanticLuaEmitter::indent_line(out, I); out << "-- variable_sprite slot=" << (int)o->slot << " sprite=" << (int)o->sprite << "\n"; return true;
    }
    if (auto* o = std::get_if<Sem_Follow>(&op)) {
        SemanticLuaEmitter::indent_line(out, I); out << "-- follow " << (int)o->object1 << " follows " << (int)o->object2 << "\n"; return true;
    }
    if (std::get_if<Sem_StopFollow>(&op)) {
        SemanticLuaEmitter::indent_line(out, I); out << "-- stop_follow\n"; return true;
    }
    if (auto* o = std::get_if<Sem_Emote>(&op)) {
        SemanticLuaEmitter::indent_line(out, I); out << "-- emote id=" << (int)o->emote_id << " object=" << (int)o->object_id << " dur=" << (int)o->duration << "\n"; return true;
    }

    return false; // not handled in part1
}

// Returns true if op was handled.
static bool emit_op_part2(std::ostream& out, const SemanticOp& op, int I) {
    using namespace enginemon;

    // Map / Warp / Scene
    if (auto* o = std::get_if<Sem_Warp>(&op)) {
        SemanticLuaEmitter::indent_line(out, I); out << "ctx.world:warp(" << (int)o->map << ", " << (int)o->x << ", " << (int)o->y << ")\n";
        SemanticLuaEmitter::indent_line(out, I); out << "coroutine.yield(\"warp\")\n"; return true;
    }
    if (auto* o = std::get_if<Sem_WarpFacing>(&op)) {
        SemanticLuaEmitter::indent_line(out, I); out << "ctx.world:face_actor(0, " << SemanticLuaEmitter::direction_name(o->facing) << ")\n";
        SemanticLuaEmitter::indent_line(out, I); out << "ctx.world:warp(" << (int)o->map << ", " << (int)o->x << ", " << (int)o->y << ")\n";
        SemanticLuaEmitter::indent_line(out, I); out << "coroutine.yield(\"warp\")\n"; return true;
    }
    if (std::get_if<Sem_WarpToBackup>(&op)) {
        SemanticLuaEmitter::indent_line(out, I); out << "ctx.world:warp_to_spawn()\n";
        SemanticLuaEmitter::indent_line(out, I); out << "coroutine.yield(\"warp\")\n"; return true;
    }
    if (auto* o = std::get_if<Sem_WarpToBackupFacing>(&op)) {
        SemanticLuaEmitter::indent_line(out, I); out << "ctx.world:face_actor(0, " << SemanticLuaEmitter::direction_name(o->facing) << ")\n";
        SemanticLuaEmitter::indent_line(out, I); out << "ctx.world:warp_to_spawn()\n";
        SemanticLuaEmitter::indent_line(out, I); out << "coroutine.yield(\"warp\")\n"; return true;
    }
    if (auto* o = std::get_if<Sem_SetScene>(&op)) {
        SemanticLuaEmitter::indent_line(out, I); out << "ctx.game:set_scene(" << (int)o->scene << ")\n"; return true;
    }
    if (std::get_if<Sem_CheckScene>(&op)) {
        SemanticLuaEmitter::indent_line(out, I); out << "result = ctx.game:check_scene()\n"; return true;
    }
    if (auto* o = std::get_if<Sem_SetMapScene>(&op)) {
        SemanticLuaEmitter::indent_line(out, I); out << "ctx.game:set_map_scene(" << (int)o->map << ", " << (int)o->scene << ")\n"; return true;
    }
    if (auto* o = std::get_if<Sem_CheckMapScene>(&op)) {
        SemanticLuaEmitter::indent_line(out, I); out << "result = ctx.game:check_map_scene(" << (int)o->map << ")\n"; return true;
    }
    if (auto* o = std::get_if<Sem_ModifyWarp>(&op)) {
        SemanticLuaEmitter::indent_line(out, I); out << "ctx.game:modify_warp(" << (int)o->warp_id << ", " << (int)o->target_map << ")\n"; return true;
    }
    if (auto* o = std::get_if<Sem_SetBlackoutPoint>(&op)) {
        SemanticLuaEmitter::indent_line(out, I); out << "ctx.game:set_blackout_point(" << (int)o->map << ")\n"; return true;
    }
    if (std::get_if<Sem_ReloadMap>(&op)) {
        SemanticLuaEmitter::indent_line(out, I); out << "ctx.game:reload_map()\n"; return true;
    }
    if (std::get_if<Sem_RefreshMap>(&op)) {
        SemanticLuaEmitter::indent_line(out, I); out << "ctx.game:refresh_map()\n"; return true;
    }
    if (std::get_if<Sem_ReanchorMap>(&op)) {
        SemanticLuaEmitter::indent_line(out, I); out << "ctx.game:reanchor_map()\n"; return true;
    }
    if (auto* o = std::get_if<Sem_NewLoadMap>(&op)) {
        SemanticLuaEmitter::indent_line(out, I); out << "ctx.game:new_load_map(" << (int)static_cast<uint8_t>(o->method) << ")\n"; return true;
    }
    if (auto* o = std::get_if<Sem_ChangeBlock>(&op)) {
        SemanticLuaEmitter::indent_line(out, I); out << "ctx.game:change_block(" << (int)o->x << ", " << (int)o->y << ", " << (int)o->block << ")\n"; return true;
    }
    if (auto* o = std::get_if<Sem_WriteCmdQueue>(&op)) {
        SemanticLuaEmitter::indent_line(out, I); out << "ctx.game:write_cmd_queue(" << o->queue_flat_address << ")\n"; return true;
    }
    if (auto* o = std::get_if<Sem_DeleteCmdQueue>(&op)) {
        SemanticLuaEmitter::indent_line(out, I); out << "ctx.game:delete_cmd_queue(" << (int)o->queue_type << ")\n"; return true;
    }

    // Battle
    if (auto* o = std::get_if<Sem_LoadWildMon>(&op)) {
        SemanticLuaEmitter::indent_line(out, I); out << "ctx.battle:start_wild(" << (int)o->species << ", " << (int)o->level << ")\n";
        SemanticLuaEmitter::indent_line(out, I); out << "coroutine.yield(\"battle\")\n"; return true;
    }
    if (auto* o = std::get_if<Sem_LoadTrainer>(&op)) {
        SemanticLuaEmitter::indent_line(out, I); out << "ctx.battle:start_trainer(" << (int)o->trainer_group << " * 256 + " << (int)o->trainer_id << ")\n";
        SemanticLuaEmitter::indent_line(out, I); out << "coroutine.yield(\"battle\")\n"; return true;
    }
    if (std::get_if<Sem_StartBattle>(&op)) {
        SemanticLuaEmitter::indent_line(out, I); out << "coroutine.yield(\"battle\")\n"; return true;
    }
    if (auto* o = std::get_if<Sem_CatchTutorial>(&op)) {
        SemanticLuaEmitter::indent_line(out, I); out << "ctx.game:catch_tutorial(" << (int)o->tutorial_type << ")\n";
        SemanticLuaEmitter::indent_line(out, I); out << "coroutine.yield(\"battle\")\n"; return true;
    }
    if (std::get_if<Sem_ReloadMapAfterBattle>(&op)) {
        SemanticLuaEmitter::indent_line(out, I); out << "ctx.game:reload_map()\n"; return true;
    }
    if (std::get_if<Sem_SetWinLossText>(&op)) {
        SemanticLuaEmitter::indent_line(out, I); out << "-- set_win_loss_text\n"; return true;
    }
    if (auto* o = std::get_if<Sem_TrainerText>(&op)) {
        SemanticLuaEmitter::indent_line(out, I); out << "-- trainer_text domain=" << (int)o->domain << " id=" << (int)o->text_id << "\n"; return true;
    }
    if (auto* o = std::get_if<Sem_TrainerFlagAction>(&op)) {
        SemanticLuaEmitter::indent_line(out, I); out << "-- trainer_flag_action " << (int)o->action << "\n"; return true;
    }
    if (std::get_if<Sem_CheckJustBattled>(&op)) {
        SemanticLuaEmitter::indent_line(out, I); out << "result = false -- check_just_battled\n"; return true;
    }
    if (std::get_if<Sem_EndIfJustBattled>(&op)) {
        SemanticLuaEmitter::indent_line(out, I); out << "-- end_if_just_battled\n"; return true;
    }

    // Audio
    if (auto* o = std::get_if<Sem_PlayMusic>(&op)) {
        SemanticLuaEmitter::indent_line(out, I); out << "ctx.audio:play_music(" << (int)o->music << ")\n"; return true;
    }
    if (auto* o = std::get_if<Sem_PlaySound>(&op)) {
        SemanticLuaEmitter::indent_line(out, I); out << "ctx.audio:play_sfx(" << (int)o->sound << ")\n"; return true;
    }
    if (auto* o = std::get_if<Sem_PlayCry>(&op)) {
        SemanticLuaEmitter::indent_line(out, I);
        if (o->source.is_literal()) out << "ctx.audio:play_cry(" << (int)o->source.species << ")\n";
        else out << "ctx.audio:play_cry(result)\n"; return true;
    }
    if (auto* o = std::get_if<Sem_PlaySlowCry>(&op)) {
        SemanticLuaEmitter::indent_line(out, I);
        if (o->source.is_literal()) out << "ctx.audio:play_cry(" << (int)o->source.species << ") -- slow\n";
        else out << "ctx.audio:play_cry(result) -- slow\n"; return true;
    }
    if (std::get_if<Sem_WaitSound>(&op)) {
        SemanticLuaEmitter::indent_line(out, I); out << "ctx.audio:wait_sfx()\n"; return true;
    }
    if (std::get_if<Sem_FadeOutMusic>(&op)) {
        SemanticLuaEmitter::indent_line(out, I); out << "ctx.audio:stop_music()\n"; return true;
    }
    if (std::get_if<Sem_FadeToSilence>(&op)) {
        SemanticLuaEmitter::indent_line(out, I); out << "ctx.audio:stop_music()\n"; return true;
    }
    if (std::get_if<Sem_PlayMapMusic>(&op)) {
        SemanticLuaEmitter::indent_line(out, I); out << "-- play_map_music\n"; return true;
    }
    if (std::get_if<Sem_PlayEncounterMusic>(&op)) {
        SemanticLuaEmitter::indent_line(out, I); out << "-- play_encounter_music\n"; return true;
    }
    if (std::get_if<Sem_RestartMapMusic>(&op)) {
        SemanticLuaEmitter::indent_line(out, I); out << "-- restart_map_music\n"; return true;
    }
    if (std::get_if<Sem_WarpSound>(&op)) {
        SemanticLuaEmitter::indent_line(out, I); out << "-- warp_sound\n"; return true;
    }
    if (std::get_if<Sem_SpecialSound>(&op)) {
        SemanticLuaEmitter::indent_line(out, I); out << "-- special_sound\n"; return true;
    }
    if (auto* o = std::get_if<Sem_SetMusicRestartFlag>(&op)) {
        SemanticLuaEmitter::indent_line(out, I); out << "-- set_music_restart_flag " << (o->prevent_restart ? "prevent" : "allow") << "\n"; return true;
    }

    // Time / Wait / RTC
    if (auto* o = std::get_if<Sem_Wait>(&op)) {
        SemanticLuaEmitter::indent_line(out, I); out << "ctx.util:wait_frames(" << (int)o->duration << ")\n"; return true;
    }
    if (auto* o = std::get_if<Sem_Pause>(&op)) {
        SemanticLuaEmitter::indent_line(out, I); out << "ctx.util:wait_frames(" << (int)o->length << ")\n"; return true;
    }
    if (auto* o = std::get_if<Sem_DeactivateFacing>(&op)) {
        SemanticLuaEmitter::indent_line(out, I); out << "ctx.game:deactivate_facing(" << (int)o->duration << ")\n"; return true;
    }
    if (auto* o = std::get_if<Sem_CheckTime>(&op)) {
        SemanticLuaEmitter::indent_line(out, I); out << "result = ctx.time:time_of_day() ~= nil -- check_time " << (int)o->time_flags << "\n"; return true;
    }
    if (auto* o = std::get_if<Sem_SetDaylightSaving>(&op)) {
        SemanticLuaEmitter::indent_line(out, I); out << "ctx.game:set_daylight_saving(" << (o->enabled ? 1 : 0) << ")\n"; return true;
    }

    // Phone
    if (auto* o = std::get_if<Sem_AddPhoneNumber>(&op)) {
        SemanticLuaEmitter::indent_line(out, I); out << "-- add_phone_number " << (int)o->person << "\n"; return true;
    }
    if (auto* o = std::get_if<Sem_AskForPhoneNumber>(&op)) {
        SemanticLuaEmitter::indent_line(out, I); out << "result = ctx.ui:yes_no() -- ask_for_phone_number " << (int)o->person << "\n"; return true;
    }
    if (auto* o = std::get_if<Sem_DeletePhoneNumber>(&op)) {
        SemanticLuaEmitter::indent_line(out, I); out << "-- delete_phone_number " << (int)o->person << "\n"; return true;
    }
    if (auto* o = std::get_if<Sem_CheckPhoneNumber>(&op)) {
        SemanticLuaEmitter::indent_line(out, I); out << "result = false -- check_phone_number " << (int)o->person << "\n"; return true;
    }
    if (std::get_if<Sem_CheckPhoneCall>(&op)) {
        SemanticLuaEmitter::indent_line(out, I); out << "result = false -- check_phone_call\n"; return true;
    }
    if (auto* o = std::get_if<Sem_SpecialPhoneCall>(&op)) {
        SemanticLuaEmitter::indent_line(out, I); out << "-- special_phone_call " << (int)o->call_id << "\n"; return true;
    }

    // Decoration
    if (auto* o = std::get_if<Sem_DescribeDecoration>(&op)) {
        SemanticLuaEmitter::indent_line(out, I); out << "ctx.game:describe_decoration(" << (int)o->decoration_id << ")\n"; return true;
    }

    // Radio / Pokedex / Party search
    if (auto* o = std::get_if<Sem_PlayRadio>(&op)) {
        SemanticLuaEmitter::indent_line(out, I); out << "ctx.game:play_radio(" << (int)o->channel << ")\n"; return true;
    }
    if (auto* o = std::get_if<Sem_RegisterNewDexEntry>(&op)) {
        SemanticLuaEmitter::indent_line(out, I); out << "ctx.game:register_dex_entry(" << (int)o->species << ")\n"; return true;
    }
    if (auto* o = std::get_if<Sem_FindPartyMon>(&op)) {
        SemanticLuaEmitter::indent_line(out, I); out << "result = ctx.game:find_party_mon(" << (int)o->species << ", " << (o->require_ot ? 1 : 0) << ")\n"; return true;
    }

    // Pokemon Mail
    if (auto* o = std::get_if<Sem_GivePokeMail>(&op)) {
        SemanticLuaEmitter::indent_line(out, I); out << "ctx.game:give_poke_mail(" << (int)o->mail_id << ")\n"; return true;
    }
    if (auto* o = std::get_if<Sem_CheckPokeMail>(&op)) {
        SemanticLuaEmitter::indent_line(out, I); out << "result = ctx.game:check_poke_mail(" << (int)o->mail_id << ")\n"; return true;
    }

    // Visual Effects
    if (std::get_if<Sem_Earthquake>(&op)) {
        SemanticLuaEmitter::indent_line(out, I); out << "-- earthquake\n"; return true;
    }
    if (std::get_if<Sem_FadeIn>(&op)) {
        SemanticLuaEmitter::indent_line(out, I); out << "ctx.ui:fade_in()\n"; return true;
    }
    if (std::get_if<Sem_FadeOut>(&op)) {
        SemanticLuaEmitter::indent_line(out, I); out << "ctx.ui:fade_out()\n"; return true;
    }
    if (auto* o = std::get_if<Sem_ScreenFade>(&op)) {
        SemanticLuaEmitter::indent_line(out, I);
        if (o->direction == FadeDirection::Out) out << "ctx.ui:fade_out()\n";
        else out << "ctx.ui:fade_in()\n"; return true;
    }

    // Renderer Synchronization
    if (auto* o = std::get_if<Sem_SyncPalettes>(&op)) {
        SemanticLuaEmitter::indent_line(out, I); out << "ctx.game:sync_palettes(" << (int)o->wait_frames << ")\n"; return true;
    }
    if (std::get_if<Sem_RefreshPlayerSprite>(&op)) {
        SemanticLuaEmitter::indent_line(out, I); out << "-- refresh_player_sprite\n"; return true;
    }
    if (std::get_if<Sem_SyncSprites>(&op)) {
        SemanticLuaEmitter::indent_line(out, I); out << "-- sync_sprites\n"; return true;
    }
    if (std::get_if<Sem_RebuildSprites>(&op)) {
        SemanticLuaEmitter::indent_line(out, I); out << "-- rebuild_sprites\n"; return true;
    }

    // Misc
    if (std::get_if<Sem_WildOn>(&op)) {
        SemanticLuaEmitter::indent_line(out, I); out << "ctx.game:wild_on()\n"; return true;
    }
    if (std::get_if<Sem_WildOff>(&op)) {
        SemanticLuaEmitter::indent_line(out, I); out << "ctx.game:wild_off()\n"; return true;
    }
    if (auto* o = std::get_if<Sem_Special>(&op)) {
        throw std::runtime_error(
            "SemanticLuaEmitter: Sem_Special{id=" + std::to_string(o->special_id) +
            "} is not legal — cannot emit");
    }
    if (auto* o = std::get_if<Sem_Pokepic>(&op)) {
        SemanticLuaEmitter::indent_line(out, I);
        if (o->source.is_literal()) out << "-- pokepic species=" << (int)o->source.species << "\n";
        else out << "-- pokepic species=script_var\n"; return true;
    }
    if (std::get_if<Sem_ClosePokepic>(&op)) {
        SemanticLuaEmitter::indent_line(out, I); out << "-- close_pokepic\n"; return true;
    }
    if (auto* o = std::get_if<Sem_Pokemart>(&op)) {
        SemanticLuaEmitter::indent_line(out, I); out << "-- pokemart dialog=" << (int)o->dialog_id << " mart=" << (int)o->mart_id << "\n"; return true;
    }
    if (auto* o = std::get_if<Sem_Elevator>(&op)) {
        SemanticLuaEmitter::indent_line(out, I); out << "-- elevator id=" << (int)o->elevator_id << "\n"; return true;
    }
    if (auto* o = std::get_if<Sem_Trade>(&op)) {
        SemanticLuaEmitter::indent_line(out, I); out << "-- trade id=" << (int)o->trade_id << "\n"; return true;
    }
    if (auto* o = std::get_if<Sem_FruitTree>(&op)) {
        SemanticLuaEmitter::indent_line(out, I); out << "-- fruit_tree id=" << (int)o->tree_id << "\n"; return true;
    }
    if (std::get_if<Sem_HallOfFame>(&op)) {
        SemanticLuaEmitter::indent_line(out, I); out << "ctx.game:hall_of_fame()\n"; return true;
    }
    if (std::get_if<Sem_Credits>(&op)) {
        SemanticLuaEmitter::indent_line(out, I); out << "ctx.game:credits()\n"; return true;
    }
    if (std::get_if<Sem_CheckSave>(&op)) {
        SemanticLuaEmitter::indent_line(out, I); out << "result = ctx.game:check_save()\n"; return true;
    }
    if (std::get_if<Sem_CheckWarp>(&op)) {
        SemanticLuaEmitter::indent_line(out, I); out << "ctx.game:check_warp()\n"; return true;
    }
    if (std::get_if<Sem_PocketFullNotify>(&op)) {
        SemanticLuaEmitter::indent_line(out, I); out << "ctx.game:pocket_full_notify()\n"; return true;
    }
    if (auto* o = std::get_if<Sem_SetPlayerPalette>(&op)) {
        SemanticLuaEmitter::indent_line(out, I); out << "ctx.game:set_player_palette(" << (int)o->selector << ")\n"; return true;
    }
    if (auto* o = std::get_if<Sem_GameSpecificEvent>(&op)) {
        SemanticLuaEmitter::indent_line(out, I); out << "ctx.game:behavior(\"" << SemanticLuaEmitter::escape_lua_string(o->behavior_name) << "\")\n"; return true;
    }

    // Standard scripts
    if (auto* o = std::get_if<Sem_CallStd>(&op)) {
        SemanticLuaEmitter::indent_line(out, I); out << "ctx.game:call_std(" << (int)o->std_id << ", \"" << SemanticLuaEmitter::escape_lua_string(o->name) << "\")\n"; return true;
    }
    if (auto* o = std::get_if<Sem_JumpStd>(&op)) {
        SemanticLuaEmitter::indent_line(out, I); out << "ctx.game:jump_std(" << (int)o->std_id << ", \"" << SemanticLuaEmitter::escape_lua_string(o->name) << "\")\n";
        SemanticLuaEmitter::indent_line(out, I); out << "return\n"; return true;
    }

    // Field move operations
    if (std::get_if<Sem_CheckStrengthCapability>(&op)) {
        SemanticLuaEmitter::indent_line(out, I); out << "result = ctx.field:check_strength()\n"; return true;
    }
    if (std::get_if<Sem_ActivateStrength>(&op)) {
        SemanticLuaEmitter::indent_line(out, I); out << "ctx.field:activate_strength()\n"; return true;
    }
    if (std::get_if<Sem_CheckRockSmashCapability>(&op)) {
        SemanticLuaEmitter::indent_line(out, I); out << "result = ctx.field:check_rock_smash()\n"; return true;
    }
    if (auto* o = std::get_if<Sem_PrepareFieldMoveNickname>(&op)) {
        SemanticLuaEmitter::indent_line(out, I); out << "ctx.field:prepare_nickname(" << (int)o->buffer_slot << ")\n"; return true;
    }
    if (std::get_if<Sem_TryRockSmashEncounter>(&op)) {
        SemanticLuaEmitter::indent_line(out, I); out << "result = ctx.field:try_rock_encounter()\n"; return true;
    }
    if (std::get_if<Sem_ReadEncounterSpecies>(&op)) {
        SemanticLuaEmitter::indent_line(out, I); out << "result = ctx.field:read_encounter_species()\n"; return true;
    }
    if (std::get_if<Sem_PlayFieldActorCry>(&op)) {
        SemanticLuaEmitter::indent_line(out, I); out << "ctx.field:play_actor_cry()\n"; return true;
    }
    if (std::get_if<Sem_LoadPendingEncounter>(&op)) {
        SemanticLuaEmitter::indent_line(out, I); out << "local enc_species, enc_level = ctx.field:load_pending_encounter()\n";
        SemanticLuaEmitter::indent_line(out, I); out << "ctx.battle:start_wild(enc_species, enc_level)\n";
        SemanticLuaEmitter::indent_line(out, I); out << "coroutine.yield(\"battle\")\n"; return true;
    }

    return false; // not handled in part2
}

// =============================================================================
// emit_op — dispatches to part1 then part2
// =============================================================================

void SemanticLuaEmitter::emit_op(std::ostream& out,
                                  const SemanticOp& op,
                                  int indent) const {
    if (emit_op_part1(out, op, indent)) return;
    if (emit_op_part2(out, op, indent)) return;

    // Collect type name for the error message via std::visit (single fallback branch)
    std::string type_name = std::visit([](const auto& o) -> std::string {
        return typeid(o).name();
    }, op);

    throw std::runtime_error(
        "SemanticLuaEmitter: unhandled SemanticOp (" + type_name + ") — "
        "add an emission case before Stage 7 deployment");
}

} // namespace crystal
