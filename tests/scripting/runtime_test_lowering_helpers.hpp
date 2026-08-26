// tests/scripting/runtime_test_lowering_helpers.hpp
// Helpers for building minimal CrystalScriptIR/CrystalCFG for SemanticLegalizer tests.
// Include this AFTER crystal/script/crystal_command.hpp and crystal/script/crystal_cfg.hpp.
#pragma once

#include <utility>
#include <string>
#include <vector>

static std::pair<crystal::CrystalScriptIR, crystal::CrystalCFG>
make_single_cmd_ir(crystal::CrystalCommandData data, uint32_t entry_address,
                   const std::string& name, std::vector<uint8_t> raw_bytes) {
    using namespace crystal;
    CrystalCommand cmd;
    cmd.data = std::move(data);
    cmd.span.rom_address = 0;
    cmd.span.raw_bytes = raw_bytes;
    CrystalScriptIR ir;
    ir.name = name;
    ir.entry_address = entry_address;
    ir.rom_start = 0;
    ir.rom_end = (uint32_t)raw_bytes.size();
    ir.commands.push_back(cmd);
    CrystalCFG cfg;
    cfg.script_name = name;
    cfg.entry_address = entry_address;
    BasicBlock block;
    block.id = 0;
    block.start_address = 0;
    block.end_address = (uint32_t)raw_bytes.size();
    block.command_start = 0;
    block.command_count = 1;
    cfg.blocks.push_back(block);
    cfg.source_ir = &ir;
    return {ir, cfg};
}

static std::pair<crystal::CrystalScriptIR, crystal::CrystalCFG>
make_three_cmd_ir(crystal::CrystalCommandData d1, crystal::CrystalCommandData d2,
                  crystal::CrystalCommandData d3, uint32_t entry_address,
                  const std::string& name) {
    using namespace crystal;
    CrystalScriptIR ir;
    ir.name = name;
    ir.entry_address = entry_address;
    CrystalCommand c1; c1.data = std::move(d1);
    CrystalCommand c2; c2.data = std::move(d2);
    CrystalCommand c3; c3.data = std::move(d3);
    ir.commands = {c1, c2, c3};
    CrystalCFG cfg;
    cfg.script_name = name;
    cfg.entry_address = entry_address;
    BasicBlock block;
    block.id = 0; block.is_entry = true;
    block.start_address = 0; block.end_address = 10;
    block.command_start = 0; block.command_count = 3;
    cfg.blocks.push_back(block);
    cfg.source_ir = &ir;
    return {ir, cfg};
}
