// engine/scripting/semantic_ir.cpp
// SemanticScriptIR implementation

#include "engine/scripting/semantic_ir.hpp"
#include <sstream>
#include <vector>

namespace enginemon {

// =============================================================================
// SemanticInstruction — out-of-line special members
//
// Declaring these non-inline in the header and defining them here means the
// compiler only generates the SemanticOp variant's destructor/move/copy
// machinery ONCE, in this TU, rather than in every TU that includes the header.
//
// This is the single most impactful change for reducing per-TU compile memory:
// the 50% backend (c2.dll) cost in each test TU comes entirely from code-
// generating these bodies.
// =============================================================================

SemanticInstruction::SemanticInstruction()                                     = default;
SemanticInstruction::~SemanticInstruction()                                    = default;
SemanticInstruction::SemanticInstruction(const SemanticInstruction&)           = default;
SemanticInstruction::SemanticInstruction(SemanticInstruction&&) noexcept       = default;
SemanticInstruction& SemanticInstruction::operator=(const SemanticInstruction&) = default;
SemanticInstruction& SemanticInstruction::operator=(SemanticInstruction&&) noexcept = default;

SemanticBasicBlock::SemanticBasicBlock()                                       = default;
SemanticBasicBlock::~SemanticBasicBlock()                                      = default;
SemanticBasicBlock::SemanticBasicBlock(const SemanticBasicBlock&)              = default;
SemanticBasicBlock::SemanticBasicBlock(SemanticBasicBlock&&) noexcept          = default;
SemanticBasicBlock& SemanticBasicBlock::operator=(const SemanticBasicBlock&)   = default;
SemanticBasicBlock& SemanticBasicBlock::operator=(SemanticBasicBlock&&) noexcept = default;

SemanticScriptIR::SemanticScriptIR()                                           = default;
SemanticScriptIR::~SemanticScriptIR()                                          = default;
SemanticScriptIR::SemanticScriptIR(const SemanticScriptIR&)                    = default;
SemanticScriptIR::SemanticScriptIR(SemanticScriptIR&&) noexcept                = default;
SemanticScriptIR& SemanticScriptIR::operator=(const SemanticScriptIR&)         = default;
SemanticScriptIR& SemanticScriptIR::operator=(SemanticScriptIR&&) noexcept     = default;

LoweringResult::LoweringResult()                                               = default;
LoweringResult::~LoweringResult()                                              = default;
LoweringResult::LoweringResult(const LoweringResult&)                          = default;
LoweringResult::LoweringResult(LoweringResult&&) noexcept                      = default;
LoweringResult& LoweringResult::operator=(const LoweringResult&)               = default;
LoweringResult& LoweringResult::operator=(LoweringResult&&) noexcept           = default;

// Explicit instantiations moved to file scope (after namespace) — see bottom of file.

// =============================================================================
// SemanticTextSequence
// =============================================================================

std::string SemanticTextSequence::debug_string() const {
    std::ostringstream ss;
    ss << "TextSequence[";
    bool first = true;
    for (const auto& elem : elements) {
        if (!first) ss << ", ";
        first = false;
        
        switch (elem.op) {
            case SemanticTextOp::Text:
                ss << "Text(\"" << elem.text << "\")";
                break;
            case SemanticTextOp::Arg:
                ss << "Arg(" << (int)elem.arg_index << ")";
                break;
            case SemanticTextOp::Line:
                ss << "Line";
                break;
            case SemanticTextOp::Next:
                ss << "Next";
                break;
            case SemanticTextOp::Para:
                ss << "Para";
                break;
            case SemanticTextOp::Cont:
                ss << "Cont";
                break;
            case SemanticTextOp::Scroll:
                ss << "Scroll";
                break;
            case SemanticTextOp::Done:
                ss << "Done";
                break;
            case SemanticTextOp::Prompt:
                ss << "Prompt";
                break;
            case SemanticTextOp::InlinePromptButton:
                ss << "InlinePromptButton";
                break;
            case SemanticTextOp::Pause:
                ss << "Pause(" << (int)elem.pause_frames() << "frames)";
                break;
            case SemanticTextOp::ScriptVarDecimal: {
                const uint8_t bytes  = (elem.param1 >> 4) & 0xF;
                const uint8_t digits =  elem.param1       & 0xF;
                ss << "ScriptVarDecimal(" << (int)bytes << "bytes," << (int)digits << "digits)";
                break;
            }
            case SemanticTextOp::Day:
                ss << "Day";
                break;
            case SemanticTextOp::Sound: {
                const char* kind_name = "?";
                switch (elem.sound_kind()) {
                    case TextSoundKind::ItemJingle:      kind_name = "ItemJingle";      break;
                    case TextSoundKind::CaughtMonJingle: kind_name = "CaughtMonJingle"; break;
                    case TextSoundKind::Fanfare:         kind_name = "Fanfare";         break;
                }
                ss << "Sound(" << kind_name << ")";
                break;
            }
            case SemanticTextOp::RamSource: {
                const char* src_name = "?";
                switch (elem.ram_source()) {
                    case TextRamSource::PreparedString2: src_name = "PreparedString2"; break;
                    case TextRamSource::PreparedString1: src_name = "PreparedString1"; break;
                    case TextRamSource::EnemyNickname:   src_name = "EnemyNickname";   break;
                    case TextRamSource::BattleNickname:  src_name = "BattleNickname";  break;
                }
                ss << "RamSource(" << src_name << ")";
                break;
            }
        }
    }
    ss << "]";
    return ss.str();
}


// =============================================================================
// SemanticScriptIR
// =============================================================================

size_t SemanticScriptIR::total_instructions() const {
    size_t total = 0;
    for (const auto& block : blocks) {
        total += block.instructions.size();
    }
    return total;
}

bool SemanticScriptIR::is_fully_lowered() const {
    // All instructions in SemanticScriptIR are valid semantic ops
    // Fully lowered means no unlowered diagnostics in the LoweringResult
    return true;  // Always true for the IR itself
}


// =============================================================================
// Stage4CorpusStats
// =============================================================================

void Stage4CorpusStats::accumulate(const LoweringResult& result) {
    total_scripts++;
    
    if (result.commands_unlowered == 0) {
        fully_lowered_scripts++;
    } else {
        partially_lowered_scripts++;
    }
    
    total_commands += result.commands_consumed;
    commands_lowered += result.commands_lowered;
    commands_unlowered += result.commands_unlowered;
    commands_absorbed += result.commands_absorbed;
    
    // Accumulate per-opcode stats
    for (const auto& [opcode, count] : result.lowered_by_opcode) {
        lowered_by_opcode[opcode] += count;
    }
    for (const auto& [opcode, count] : result.unlowered_by_opcode) {
        unlowered_by_opcode[opcode] += count;
    }
    for (const auto& [opcode, count] : result.absorbed_by_opcode) {
        absorbed_by_opcode[opcode] += count;
    }
}

} // namespace enginemon

// File-scope explicit instantiations — paired with extern template in semantic_ir.hpp.
// Must be at file scope so enclosing namespace of std::vector is std ([temp.explicit]/7).
template class std::vector<enginemon::SemanticInstruction>;
template class std::vector<enginemon::SemanticBasicBlock>;
