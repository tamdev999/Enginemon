#pragma once
// crystal/script/lua_emitter.hpp
// Emits Lua code from Script IR
// Generated scripts use the same ctx.* API as mod scripts

#include "crystal/script/ir.hpp"
#include <ostream>
#include <string>
#include <unordered_map>

namespace crystal {

// Lua code emitter configuration
struct EmitterConfig {
    // Include comments with ROM addresses
    bool emit_address_comments = true;
    
    // Include original Crystal opcode names in comments
    bool emit_opcode_comments = true;
    
    // Pretty-print with indentation
    bool pretty_print = true;
    int indent_width = 2;
    
    // Generate local variables for repeated lookups
    bool optimize_locals = true;
    
    // Emit debug hooks for script debugging
    bool emit_debug_hooks = false;
};

// Emits Lua code from script IR
class LuaEmitter {
public:
    explicit LuaEmitter(EmitterConfig config = {});
    
    // Emit single script to string
    std::string emit(const ScriptIR& script);
    
    // Emit single script to stream
    void emit(const ScriptIR& script, std::ostream& out);
    
    // Emit all scripts for a map to single file
    std::string emit_map_file(MapId map, const std::vector<const ScriptIR*>& scripts);
    
    // Emit complete script set to directory structure
    void emit_all(const ScriptIRSet& scripts, const std::filesystem::path& output_dir);

private:
    EmitterConfig config_;
    int current_indent_ = 0;
    
    // State during emission
    struct EmitState {
        const ScriptIR* script = nullptr;
        std::unordered_map<uint32_t, std::string> address_to_label;
        std::unordered_map<std::string, bool> used_labels;
        bool in_conditional = false;
        int conditional_depth = 0;
    };
    EmitState state_;
    
    // Output helpers
    void emit_indent(std::ostream& out);
    void emit_line(std::ostream& out, const std::string& line);
    void emit_comment(std::ostream& out, const std::string& comment);
    
    // Structure emission
    void emit_header(std::ostream& out, const ScriptIR& script);
    void emit_footer(std::ostream& out, const ScriptIR& script);
    void emit_labels_as_functions(std::ostream& out, const ScriptIR& script);
    
    // Instruction emission (visitor pattern)
    void emit_instruction(std::ostream& out, const Instruction& inst);
    
    // Individual operation emitters
    void emit_op(std::ostream& out, const Op_End& op);
    void emit_op(std::ostream& out, const Op_Jump& op);
    void emit_op(std::ostream& out, const Op_JumpIf& op);
    void emit_op(std::ostream& out, const Op_Call& op);
    void emit_op(std::ostream& out, const Op_Return& op);
    void emit_op(std::ostream& out, const Op_JumpStd& op);
    void emit_op(std::ostream& out, const Op_CallStd& op);
    
    void emit_op(std::ostream& out, const Op_SetFlag& op);
    void emit_op(std::ostream& out, const Op_ClearFlag& op);
    void emit_op(std::ostream& out, const Op_CheckFlag& op);
    void emit_op(std::ostream& out, const Op_SetVar& op);
    void emit_op(std::ostream& out, const Op_AddVar& op);
    void emit_op(std::ostream& out, const Op_CheckVar& op);
    
    void emit_op(std::ostream& out, const Op_OpenText& op);
    void emit_op(std::ostream& out, const Op_CloseText& op);
    void emit_op(std::ostream& out, const Op_Text& op);
    void emit_op(std::ostream& out, const Op_JumpText& op);
    void emit_op(std::ostream& out, const Op_JumpTextFacePlayer& op);
    void emit_op(std::ostream& out, const Op_YesNo& op);
    void emit_op(std::ostream& out, const Op_Choice& op);
    
    // Helper to emit semantic text sequence as Lua table
    void emit_text_sequence(std::ostream& out, const TextSequence& seq);
    
    void emit_op(std::ostream& out, const Op_GiveItem& op);
    void emit_op(std::ostream& out, const Op_TakeItem& op);
    void emit_op(std::ostream& out, const Op_CheckItem& op);
    void emit_op(std::ostream& out, const Op_GiveMoney& op);
    void emit_op(std::ostream& out, const Op_TakeMoney& op);
    void emit_op(std::ostream& out, const Op_CheckMoney& op);
    
    void emit_op(std::ostream& out, const Op_GivePokemon& op);
    void emit_op(std::ostream& out, const Op_HealParty& op);
    void emit_op(std::ostream& out, const Op_CheckPartyCount& op);
    void emit_op(std::ostream& out, const Op_CheckPartySpecies& op);
    
    void emit_op(std::ostream& out, const Op_ApplyMovement& op);
    void emit_op(std::ostream& out, const Op_FacePlayer& op);
    void emit_op(std::ostream& out, const Op_FaceObject& op);
    void emit_op(std::ostream& out, const Op_ShowSprite& op);
    void emit_op(std::ostream& out, const Op_HideSprite& op);
    
    void emit_op(std::ostream& out, const Op_Warp& op);
    void emit_op(std::ostream& out, const Op_WarpToSpawn& op);
    
    void emit_op(std::ostream& out, const Op_WildBattle& op);
    void emit_op(std::ostream& out, const Op_TrainerBattle& op);
    void emit_op(std::ostream& out, const Op_CheckBattleResult& op);
    
    void emit_op(std::ostream& out, const Op_PlayMusic& op);
    void emit_op(std::ostream& out, const Op_PlaySfx& op);
    void emit_op(std::ostream& out, const Op_PlayCry& op);
    void emit_op(std::ostream& out, const Op_WaitSfx& op);
    void emit_op(std::ostream& out, const Op_StopMusic& op);
    
    void emit_op(std::ostream& out, const Op_FadeOut& op);
    void emit_op(std::ostream& out, const Op_FadeIn& op);
    void emit_op(std::ostream& out, const Op_ShowMapName& op);
    void emit_op(std::ostream& out, const Op_Earthquake& op);
    
    void emit_op(std::ostream& out, const Op_CheckTime& op);
    void emit_op(std::ostream& out, const Op_CheckDay& op);
    
    void emit_op(std::ostream& out, const Op_Wait& op);
    void emit_op(std::ostream& out, const Op_WaitButton& op);
    void emit_op(std::ostream& out, const Op_Special& op);
    void emit_op(std::ostream& out, const Op_Raw& op);
    
    // Helpers
    std::string escape_string(const std::string& s);
    std::string make_label_function_name(const LabelRef& ref);
    std::string make_item_id_string(ItemId id);
    std::string make_species_id_string(SpeciesId id);
    std::string make_move_id_string(MoveId id);
    std::string make_flag_id_string(FlagId id);
    std::string make_map_id_string(MapId id);
    std::string make_music_id_string(MusicId id);
    std::string make_direction_string(Direction dir);
    std::string make_time_string(TimeOfDay time);
    std::string make_day_string(DayOfWeek day);
};

// Example output for a simple Crystal script:
//
// -- Script: NewBarkTown_MomsHouse_OakScript
// -- ROM: 0x1A3F00
// -- Generated by Enginemon Crystal Frontend
//
// local script = {}
//
// function script.main(ctx)
//     -- 0x1A3F00: opentext
//     ctx.ui:open_text()
//     
//     -- 0x1A3F01: writetext OakText1
//     ctx.ui:text("Hello there!")
//     
//     -- 0x1A3F04: giveitem POTION, 1
//     ctx.inventory:give(Items.POTION, 1)
//     ctx.ui:text("Received POTION!")
//     
//     -- 0x1A3F08: closetext
//     ctx.ui:close_text()
//     
//     -- 0x1A3F09: end
// end
//
// return script

} // namespace crystal
