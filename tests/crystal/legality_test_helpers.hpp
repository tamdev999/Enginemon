// tests/crystal/legality_test_helpers.hpp
//
// Shared helpers for legality gate tests.
//
// Both legality_gate_test.cpp (crystal/ test suite) and runtime_test.cpp
// (scripting/ test suite) need a legitimately constructed minimal
// CrystalScriptIR + CrystalCFG + LoweringResult to satisfy the full
// Stage 1-5 pipeline before exercising Stage 5 IR invariants.
//
// These helpers are the canonical construction path used by the existing
// legality_gate_test.cpp tests, factored here so they can be shared without
// duplication.  Do NOT manually forge cfg.validation fields inline in tests;
// use make_minimal_cfg() instead.

#pragma once

#include "crystal/script/legality_gate.hpp"    // pulls in crystal_cfg, crystal_command, semantic_ir
#include "crystal/script/crystal_cfg.hpp"
#include "crystal/script/crystal_command.hpp"
#include "engine/scripting/semantic_ir.hpp"

namespace legality_test_helpers {

// ---------------------------------------------------------------------------
// make_minimal_ir
// Returns a CrystalScriptIR containing a single valid Cmd_End at `address`.
// ---------------------------------------------------------------------------
inline crystal::CrystalScriptIR make_minimal_ir(uint32_t address = 0x1000) {
    crystal::CrystalScriptIR ir;

    crystal::CrystalCommand cmd;
    cmd.data = crystal::Cmd_End{};
    cmd.span.rom_address = address;
    cmd.span.raw_bytes = {0x91};   // end opcode
    cmd.status = crystal::DecodeStatus::Success;
    ir.commands.push_back(cmd);

    return ir;
}

// ---------------------------------------------------------------------------
// make_minimal_cfg
// Builds a closed, valid CrystalCFG from `ir` using the same construction
// path as the original legality_gate_test.cpp helpers.  Validation stats are
// derived from the IR — no fields are forged in isolation.
// ---------------------------------------------------------------------------
inline crystal::CrystalCFG make_minimal_cfg(const crystal::CrystalScriptIR& ir,
                                             const std::string& name = "test_script") {
    crystal::CrystalCFG cfg;
    cfg.entry_address = ir.commands.empty() ? 0 : ir.commands[0].span.rom_address;
    cfg.script_name = name;
    cfg.source_ir = &ir;

    crystal::BasicBlock block;
    block.id = 0;
    block.start_address = cfg.entry_address;
    block.end_address = cfg.entry_address + 1;
    block.command_start = 0;
    block.command_count = ir.commands.size();
    block.is_entry = true;
    block.is_reachable = true;
    block.exit.kind = crystal::ExitKind::Terminal;

    cfg.blocks.push_back(block);
    cfg.address_to_block[cfg.entry_address] = 0;

    for (const auto& cmd : ir.commands) {
        cfg.command_boundaries.insert(cmd.span.rom_address);
    }

    cfg.validation.valid = true;
    cfg.validation.commands_covered = ir.commands.size();
    cfg.validation.commands_total = ir.commands.size();
    cfg.validation.terminal_exits = 1;

    return cfg;
}

// ---------------------------------------------------------------------------
// make_minimal_lowering
// Returns a LoweringResult that passes Stage 4 accounting with a single
// Sem_End instruction.  Caller may replace lowering.ir.blocks to inject a
// different semantic op for Stage 5 testing.
// ---------------------------------------------------------------------------
inline enginemon::LoweringResult make_minimal_lowering(const crystal::CrystalScriptIR& ir,
                                                        const crystal::CrystalCFG& cfg) {
    enginemon::LoweringResult result;
    result.ir.script_id = cfg.script_name;
    result.ir.script_name = cfg.script_name;
    result.ir.source_rom_address = cfg.entry_address;
    result.success = true;
    result.commands_consumed = ir.commands.size();
    result.commands_lowered = ir.commands.size();
    result.commands_unlowered = 0;
    result.commands_absorbed = 0;

    enginemon::SemanticBasicBlock sem_block;
    sem_block.id = 0;
    sem_block.label = "block_0";
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
    input.ir = &ir;
    input.cfg = &cfg;
    input.lowering = &lowering;
    input.decode_complete = true;
    input.round_trip_failures = 0;
    input.unknown_opcodes = 0;
    return input;
}

} // namespace legality_test_helpers
