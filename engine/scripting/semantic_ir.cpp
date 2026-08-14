// engine/scripting/semantic_ir.cpp
// SemanticScriptIR implementation

#include "engine/scripting/semantic_ir.hpp"
#include <sstream>

namespace enginemon {

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
