#pragma once
// crystal/script/ir.hpp
// Intermediate representation for Crystal scripts
// Simple, mostly lossless - purpose is validation, normalization, code generation
// NOT reconstructing pretty source code

#include "engine/core/types.hpp"
#include <variant>
#include <vector>
#include <string>
#include <cstdint>
#include <memory>
#include <unordered_map>

namespace crystal {

using namespace enginemon;

// Forward declaration
struct ScriptIR;

// Label reference (for jumps)
struct LabelRef {
    std::string name;
    uint32_t rom_address;   // For debugging/cross-reference
};

// ============================================================================
// TEXT SEQUENCE - Semantic representation of Crystal text
// ============================================================================

// Text control codes from Crystal (preserved semantically, never flattened to whitespace)
// Reference: pokecrystal/macros/scripts/text.asm, home/text.asm
enum class TextOp : uint8_t {
    // Printable text run - the actual characters to display
    Text,
    
    // Flow control codes
    Line,       // <LINE> (0x4F): Move cursor to line 2, no wait
    Next,       // <NEXT> (0x4E): Clear box and continue (like LINE but clears)
    Para,       // <PARA> (0x51): Wait for button, clear box, continue
    Cont,       // <CONT> (0x55): Wait for button, scroll up one line, continue
    Scroll,     // <SCROLL> (0x4B): Scroll without wait (internal use)
    
    // Terminators
    Done,       // <DONE> (0x57): End text processing, box stays open
    Prompt,     // <PROMPT> (0x58): Show cursor, wait for button, close
};

// A single element in a text sequence
struct TextElement {
    TextOp op;
    std::string text;   // For TextOp::Text, the actual character data (UTF-8)
    
    // Convenience constructors
    static TextElement make_text(const std::string& s) { return {TextOp::Text, s}; }
    static TextElement make_line() { return {TextOp::Line, ""}; }
    static TextElement make_next() { return {TextOp::Next, ""}; }
    static TextElement make_para() { return {TextOp::Para, ""}; }
    static TextElement make_cont() { return {TextOp::Cont, ""}; }
    static TextElement make_scroll() { return {TextOp::Scroll, ""}; }
    static TextElement make_done() { return {TextOp::Done, ""}; }
    static TextElement make_prompt() { return {TextOp::Prompt, ""}; }
};

// A complete text sequence from Crystal ROM
// Example for NewBarkTownSignText:
//   Text("NEW BARK TOWN"), Para, Text("The Town Where the"), Line, 
//   Text("Winds of a New"), Cont, Text("Beginning Blow"), Done
struct TextSequence {
    std::vector<TextElement> elements;
    uint32_t rom_address = 0;   // For debugging
    
    // Check if sequence is empty
    bool empty() const { return elements.empty(); }
    
    // Get flattened display text (for debugging/logging only, NOT for rendering)
    std::string debug_string() const;
};

// ============================================================================
// IR Operations
// Closely mirrors Crystal's event script commands
// ============================================================================

// Control flow
struct Op_End {};
struct Op_Jump { LabelRef target; };
struct Op_JumpIf { LabelRef target; std::string condition; };
struct Op_Call { LabelRef target; };
struct Op_Return {};

// Flag/variable operations
struct Op_SetFlag { FlagId flag; };
struct Op_ClearFlag { FlagId flag; };
struct Op_CheckFlag { FlagId flag; };
struct Op_SetVar { VarId var; int16_t value; };
struct Op_AddVar { VarId var; int16_t delta; };
struct Op_CheckVar { VarId var; std::string op; int16_t value; }; // op: "==", "<", ">", etc.

// Dialog - uses semantic TextSequence to preserve LINE/CONT/PARA distinctions
struct Op_Text { TextSequence sequence; bool scroll; };
struct Op_YesNo {};
struct Op_Choice { std::vector<std::string> options; };

// Inventory
struct Op_GiveItem { ItemId item; uint8_t count; };
struct Op_TakeItem { ItemId item; uint8_t count; };
struct Op_CheckItem { ItemId item; };
struct Op_GiveMoney { uint32_t amount; };
struct Op_TakeMoney { uint32_t amount; };
struct Op_CheckMoney { uint32_t amount; };

// Party/Pokemon
struct Op_GivePokemon { SpeciesId species; uint8_t level; ItemId held_item; };
struct Op_HealParty {};
struct Op_CheckPartyCount {};
struct Op_CheckPartySpecies { SpeciesId species; };

// Movement/World
// Movement types and commands are defined in engine/core/types.hpp
// They are available here via 'using namespace enginemon'

struct Op_ApplyMovement { 
    uint8_t object_id; 
    std::vector<uint8_t> movements;     // Raw bytes for debugging
    std::vector<MovementCommand> commands;  // Parsed semantic commands
};
struct Op_FacePlayer { uint8_t object_id; };
struct Op_FaceObject { uint8_t object_id; Direction direction; };
struct Op_ShowSprite { uint8_t object_id; };
struct Op_HideSprite { uint8_t object_id; };
struct Op_Warp { MapId map; uint8_t x; uint8_t y; };
struct Op_WarpToSpawn {};

// Battle
struct Op_WildBattle { SpeciesId species; uint8_t level; };
struct Op_TrainerBattle { TrainerId trainer; };
struct Op_CheckBattleResult {};

// Audio
struct Op_PlayMusic { MusicId music; };
struct Op_PlaySfx { SfxId sfx; };
struct Op_PlayCry { SpeciesId species; };
struct Op_WaitSfx {};
struct Op_StopMusic {};

// Visual
struct Op_FadeOut { uint8_t speed; };
struct Op_FadeIn { uint8_t speed; };
struct Op_ShowMapName {};
struct Op_Earthquake { uint8_t intensity; };

// Time checks
struct Op_CheckTime { TimeOfDay time; };
struct Op_CheckDay { DayOfWeek day; };

// Wait
struct Op_Wait { uint8_t frames; };

// Explicit wait for button press (distinct from Op_Wait which is frames-based)
struct Op_WaitButton {};

// Special/misc (game-specific behaviors)
struct Op_Special { uint16_t special_id; std::string name; };

// Open/close text box explicitly
struct Op_OpenText {};
struct Op_CloseText {};

// Jump to text and end (compound: opentext + writetext + waitbutton + closetext + end)
struct Op_JumpText { 
    TextSequence sequence;      // Semantic text sequence 
    uint32_t text_address;      // For debugging 
};

// Jump to text with faceplayer (compound: faceplayer + jumptext)
struct Op_JumpTextFacePlayer { 
    TextSequence sequence;      // Semantic text sequence
    uint32_t text_address; 
};

// Std script references (built-in Crystal scripts)
struct Op_JumpStd { uint16_t std_id; std::string name; };
struct Op_CallStd { uint16_t std_id; std::string name; };

// Generic command for unrecognized/complex opcodes
struct Op_Raw { 
    uint8_t opcode; 
    std::vector<uint8_t> params;
    std::string comment;
};

// Union of all operations
using Operation = std::variant<
    Op_End,
    Op_Jump,
    Op_JumpIf,
    Op_Call,
    Op_Return,
    Op_JumpStd,
    Op_CallStd,
    Op_SetFlag,
    Op_ClearFlag,
    Op_CheckFlag,
    Op_SetVar,
    Op_AddVar,
    Op_CheckVar,
    Op_OpenText,
    Op_CloseText,
    Op_Text,
    Op_JumpText,
    Op_JumpTextFacePlayer,
    Op_YesNo,
    Op_Choice,
    Op_GiveItem,
    Op_TakeItem,
    Op_CheckItem,
    Op_GiveMoney,
    Op_TakeMoney,
    Op_CheckMoney,
    Op_GivePokemon,
    Op_HealParty,
    Op_CheckPartyCount,
    Op_CheckPartySpecies,
    Op_ApplyMovement,
    Op_FacePlayer,
    Op_FaceObject,
    Op_ShowSprite,
    Op_HideSprite,
    Op_Warp,
    Op_WarpToSpawn,
    Op_WildBattle,
    Op_TrainerBattle,
    Op_CheckBattleResult,
    Op_PlayMusic,
    Op_PlaySfx,
    Op_PlayCry,
    Op_WaitSfx,
    Op_StopMusic,
    Op_FadeOut,
    Op_FadeIn,
    Op_ShowMapName,
    Op_Earthquake,
    Op_CheckTime,
    Op_CheckDay,
    Op_Wait,
    Op_WaitButton,
    Op_Special,
    Op_Raw
>;

// Single IR instruction with metadata
struct Instruction {
    Operation op;
    
    // Source location (for debugging)
    uint32_t rom_address = 0;
    std::string source_label;   // If at a labeled address
    
    // Line in generated Lua (filled by emitter)
    int lua_line = 0;
};

// Label definition
struct Label {
    std::string name;
    uint32_t rom_address;
    size_t instruction_index;   // Index into instructions vector
};

// Complete script IR
struct ScriptIR {
    ScriptId id;
    std::string name;           // Symbolic name from ROM (e.g. "NewBarkTown_PlayersHouse1F_OakScript")
    MapId associated_map;       // MAP_NONE if global
    
    std::vector<Instruction> instructions;
    std::vector<Label> labels;
    
    // Entry points (scripts can have multiple)
    std::vector<size_t> entry_points;   // Instruction indices
    
    // Metadata
    uint32_t rom_start;
    uint32_t rom_end;
    
    // Find label by name
    const Label* find_label(const std::string& name) const;
    
    // Find label by address
    const Label* find_label_at(uint32_t address) const;
};

// Collection of all scripts from ROM
struct ScriptIRSet {
    std::vector<ScriptIR> scripts;
    
    // Global map script lookup
    std::unordered_map<MapId, std::vector<size_t>> map_scripts;  // Map -> script indices
    
    const ScriptIR* find_by_id(ScriptId id) const;
    const ScriptIR* find_by_name(const std::string& name) const;
};

} // namespace crystal
