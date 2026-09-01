// tests/crystal/test_crystal_ir_helpers.hpp
//
// Narrow Crystal-IR construction helpers — Stage 1+2 only.
//
// Provides:
//   make_minimal_ir()   — CrystalScriptIR with a single Cmd_End
//   make_minimal_cfg()  — closed CrystalCFG over that IR
//
// Dependencies:
//   crystal/script/crystal_command.hpp  (CrystalScriptIR, CrystalCommandData)
//   crystal/script/crystal_cfg.hpp      (CrystalCFG, BasicBlock, ExitKind)
//
// Do NOT include semantic_ir.hpp or legality_gate.hpp from this header.
// TUs that need only Stage 1-2 IR scaffolding can include this alone and
// avoid instantiating SemanticOp (154-alt variant).
#pragma once
#include "crystal/script/crystal_command.hpp"
#include "crystal/script/crystal_cfg.hpp"
#include <string>

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
// Builds a closed, valid CrystalCFG from `ir`.  Validation stats are
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

} // namespace legality_test_helpers
