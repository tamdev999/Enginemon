// tests/crystal/test_semantic_helpers.hpp
//
// Semantic-IR construction helpers — Stage 4-5.
//
// Provides:
//   make_minimal_lowering()  — LoweringResult with a single Sem_End
//   make_minimal_input()     — fully populated LegalityInput (all stages)
//
// Both functions take const CrystalScriptIR& and const CrystalCFG& and
// dereference their members, so this header requires both complete types.
//
// Use test_crystal_ir_helpers.hpp for make_minimal_ir / make_minimal_cfg.
// Include legality_test_helpers.hpp if you need all four helpers at once.
#pragma once
#include "crystal/script/crystal_command.hpp"   // CrystalScriptIR (complete type)
#include "crystal/script/crystal_cfg.hpp"        // CrystalCFG (complete type)
#include "crystal/script/legality_gate.hpp"      // LegalityInput
#include "engine/scripting/semantic_ir.hpp"
#include <string>

namespace legality_test_helpers {

// ---------------------------------------------------------------------------
// make_minimal_lowering
// Returns a LoweringResult that passes Stage 4 accounting with a single
// Sem_End instruction.  Caller may replace lowering.ir.blocks to inject a
// different semantic op for Stage 5 testing.
// ---------------------------------------------------------------------------
inline enginemon::LoweringResult make_minimal_lowering(const crystal::CrystalScriptIR& ir,
                                                        const crystal::CrystalCFG& cfg) {
    enginemon::LoweringResult result;
    result.ir.script_id   = cfg.script_name;
    result.ir.script_name = cfg.script_name;
    result.ir.source_rom_address = cfg.entry_address;
    result.success = true;
    result.commands_consumed  = ir.commands.size();
    result.commands_lowered   = ir.commands.size();
    result.commands_unlowered = 0;
    result.commands_absorbed  = 0;

    enginemon::SemanticBasicBlock sem_block;
    sem_block.id       = 0;
    sem_block.label    = "block_0";
    sem_block.is_entry = true;

    enginemon::SemanticInstruction inst;
    inst.op = enginemon::Sem_End{};
    sem_block.instructions.push_back(inst);
    result.ir.blocks.push_back(sem_block);
    return result;
}

// ---------------------------------------------------------------------------
// make_minimal_input
// Convenience: returns a fully populated LegalityInput with all Stage 1-4
// fields set to pass cleanly.  The caller must keep ir, cfg, and lowering
// alive for the duration of the validate() call.
// ---------------------------------------------------------------------------
inline crystal::LegalityInput make_minimal_input(const crystal::CrystalScriptIR& ir,
                                                   const crystal::CrystalCFG& cfg,
                                                   enginemon::LoweringResult& lowering) {
    crystal::LegalityInput input;
    input.ir     = &ir;
    input.cfg    = &cfg;
    input.lowering           = &lowering;
    input.decode_complete    = true;
    input.round_trip_failures = 0;
    input.unknown_opcodes    = 0;
    return input;
}

} // namespace legality_test_helpers
