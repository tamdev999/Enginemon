// legality_gate_test.cpp
// Stage 5 negative tests: prove the legality gate rejects known-bad scripts
//
// Each test creates a deliberately invalid input and verifies the gate
// produces the expected rejection with correct diagnostics.
//
// NOTE: This test uses minimal includes to avoid compiler memory issues
// with the large variant types in the full headers.

#include "crystal/script/legality_gate.hpp"
#include <iostream>
#include <iomanip>
#include <cassert>

using namespace crystal;
using namespace enginemon;

// Test counter
int tests_run = 0;
int tests_passed = 0;

#define RUN_TEST(name) do { \
    std::cout << "Running " << #name << "... "; \
    tests_run++; \
    if (name()) { \
        std::cout << "PASS\n"; \
        tests_passed++; \
    } else { \
        std::cout << "FAIL\n"; \
    } \
} while(0)

// =============================================================================
// HELPER FUNCTIONS
// =============================================================================

// Create a minimal valid CrystalScriptIR for testing
CrystalScriptIR make_minimal_ir(uint32_t address = 0x1000) {
    CrystalScriptIR ir;
    
    // Add a simple "end" command
    CrystalCommand cmd;
    cmd.data = Cmd_End{};
    cmd.span.rom_address = address;
    cmd.span.raw_bytes = {0x91};  // end opcode
    cmd.status = DecodeStatus::Success;
    ir.commands.push_back(cmd);
    
    return ir;
}

// Create a minimal valid CrystalCFG
CrystalCFG make_minimal_cfg(const CrystalScriptIR& ir) {
    CrystalCFG cfg;
    cfg.entry_address = ir.commands.empty() ? 0 : ir.commands[0].span.rom_address;
    cfg.script_name = "test_script";
    cfg.source_ir = &ir;
    
    // Create one block with the commands
    BasicBlock block;
    block.id = 0;
    block.start_address = cfg.entry_address;
    block.end_address = cfg.entry_address + 1;
    block.command_start = 0;
    block.command_count = ir.commands.size();
    block.is_entry = true;
    block.is_reachable = true;
    block.exit.kind = ExitKind::Terminal;
    
    cfg.blocks.push_back(block);
    cfg.address_to_block[cfg.entry_address] = 0;
    
    // Populate command boundaries
    for (const auto& cmd : ir.commands) {
        cfg.command_boundaries.insert(cmd.span.rom_address);
    }
    
    // Validation stats
    cfg.validation.valid = true;
    cfg.validation.commands_covered = ir.commands.size();
    cfg.validation.commands_total = ir.commands.size();
    cfg.validation.terminal_exits = 1;
    
    return cfg;
}

// Create a minimal valid LoweringResult
LoweringResult make_minimal_lowering(const CrystalScriptIR& ir, const CrystalCFG& cfg) {
    LoweringResult result;
    result.ir.script_id = cfg.script_name;
    result.ir.script_name = cfg.script_name;
    result.ir.source_rom_address = cfg.entry_address;
    result.success = true;
    result.commands_consumed = ir.commands.size();
    result.commands_lowered = ir.commands.size();
    result.commands_unlowered = 0;
    result.commands_absorbed = 0;
    
    // Create a semantic block with Sem_End
    SemanticBasicBlock sem_block;
    sem_block.id = 0;
    sem_block.label = "block_0";
    sem_block.is_entry = true;
    
    SemanticInstruction inst;
    inst.op = Sem_End{};
    sem_block.instructions.push_back(inst);
    
    result.ir.blocks.push_back(sem_block);
    
    return result;
}

// =============================================================================
// NEGATIVE TEST 1: Unknown Opcode (Stage 1)
// =============================================================================

bool test_reject_unknown_opcode() {
    // Create IR with an unknown opcode (>0xA9)
    CrystalScriptIR ir;
    
    CrystalCommand cmd;
    cmd.data = Cmd_Unknown{0xBB};  // Invalid opcode
    cmd.span.rom_address = 0x1000;
    cmd.span.raw_bytes = {0xBB};
    cmd.status = DecodeStatus::Success;
    ir.commands.push_back(cmd);
    
    CrystalCFG cfg = make_minimal_cfg(ir);
    LoweringResult lowering = make_minimal_lowering(ir, cfg);
    
    LegalityInput input;
    input.ir = &ir;
    input.decode_complete = true;
    input.round_trip_failures = 0;
    input.unknown_opcodes = 1;  // Report the unknown opcode
    input.cfg = &cfg;
    input.lowering = &lowering;
    
    LegalityGate gate;
    LegalityResult result = gate.validate(input);
    
    // Should be illegal
    if (result.is_legal) {
        std::cout << "(expected illegal, got legal) ";
        return false;
    }
    
    // Check for correct failure kind
    bool found_unknown_opcode = false;
    for (const auto& diag : result.diagnostics()) {
        if (diag.kind == LegalityFailureKind::UnknownOpcode) {
            found_unknown_opcode = true;
            break;
        }
    }
    
    if (!found_unknown_opcode) {
        std::cout << "(expected UnknownOpcode diagnostic) ";
        return false;
    }
    
    std::cout << "[UnknownOpcode rejected] ";
    return true;
}

// =============================================================================
// NEGATIVE TEST 2: Round-trip Failure (Stage 1)
// =============================================================================

bool test_reject_roundtrip_failure() {
    CrystalScriptIR ir = make_minimal_ir();
    CrystalCFG cfg = make_minimal_cfg(ir);
    LoweringResult lowering = make_minimal_lowering(ir, cfg);
    
    LegalityInput input;
    input.ir = &ir;
    input.decode_complete = true;
    input.round_trip_failures = 3;  // Simulate round-trip failures
    input.unknown_opcodes = 0;
    input.cfg = &cfg;
    input.lowering = &lowering;
    
    LegalityGate gate;
    LegalityResult result = gate.validate(input);
    
    if (result.is_legal) {
        std::cout << "(expected illegal, got legal) ";
        return false;
    }
    
    bool found_roundtrip = false;
    for (const auto& diag : result.diagnostics()) {
        if (diag.kind == LegalityFailureKind::RoundTripFailure) {
            found_roundtrip = true;
            break;
        }
    }
    
    if (!found_roundtrip) {
        std::cout << "(expected RoundTripFailure diagnostic) ";
        return false;
    }
    
    std::cout << "[RoundTripFailure rejected] ";
    return true;
}

// =============================================================================
// NEGATIVE TEST 3: Decode Incomplete (Stage 1)
// =============================================================================

bool test_reject_decode_incomplete() {
    CrystalScriptIR ir = make_minimal_ir();
    CrystalCFG cfg = make_minimal_cfg(ir);
    LoweringResult lowering = make_minimal_lowering(ir, cfg);
    
    LegalityInput input;
    input.ir = &ir;
    input.decode_complete = false;  // Decode failed
    input.round_trip_failures = 0;
    input.unknown_opcodes = 0;
    input.cfg = &cfg;
    input.lowering = &lowering;
    
    LegalityGate gate;
    LegalityResult result = gate.validate(input);
    
    if (result.is_legal) {
        std::cout << "(expected illegal, got legal) ";
        return false;
    }
    
    bool found_incomplete = false;
    for (const auto& diag : result.diagnostics()) {
        if (diag.kind == LegalityFailureKind::DecodeIncomplete) {
            found_incomplete = true;
            break;
        }
    }
    
    if (!found_incomplete) {
        std::cout << "(expected DecodeIncomplete diagnostic) ";
        return false;
    }
    
    std::cout << "[DecodeIncomplete rejected] ";
    return true;
}

// =============================================================================
// NEGATIVE TEST 4: Unclosed CFG (Stage 2)
// =============================================================================

bool test_reject_unclosed_cfg() {
    CrystalScriptIR ir = make_minimal_ir();
    CrystalCFG cfg = make_minimal_cfg(ir);
    
    // Make the CFG unclosed by adding computed exits
    cfg.validation.computed_exits = 2;
    cfg.blocks[0].exit.kind = ExitKind::Computed;
    
    LoweringResult lowering = make_minimal_lowering(ir, cfg);
    
    LegalityInput input;
    input.ir = &ir;
    input.decode_complete = true;
    input.round_trip_failures = 0;
    input.unknown_opcodes = 0;
    input.cfg = &cfg;
    input.lowering = &lowering;
    
    LegalityGate gate;
    LegalityResult result = gate.validate(input);
    
    if (result.is_legal) {
        std::cout << "(expected illegal, got legal) ";
        return false;
    }
    
    bool found_unclosed = false;
    for (const auto& diag : result.diagnostics()) {
        if (diag.kind == LegalityFailureKind::CFGUnclosed) {
            found_unclosed = true;
            break;
        }
    }
    
    if (!found_unclosed) {
        std::cout << "(expected CFGUnclosed diagnostic) ";
        return false;
    }
    
    std::cout << "[CFGUnclosed rejected] ";
    return true;
}

// =============================================================================
// NEGATIVE TEST 5: Invalid CFG Target (Stage 2)
// =============================================================================

bool test_reject_invalid_cfg_target() {
    CrystalScriptIR ir = make_minimal_ir();
    CrystalCFG cfg = make_minimal_cfg(ir);
    
    // Add invalid target edges
    cfg.validation.invalid_targets = 1;
    cfg.validation.bad_edges.push_back({0x1000, 0xDEAD});
    
    LoweringResult lowering = make_minimal_lowering(ir, cfg);
    
    LegalityInput input;
    input.ir = &ir;
    input.decode_complete = true;
    input.round_trip_failures = 0;
    input.unknown_opcodes = 0;
    input.cfg = &cfg;
    input.lowering = &lowering;
    
    LegalityGate gate;
    LegalityResult result = gate.validate(input);
    
    if (result.is_legal) {
        std::cout << "(expected illegal, got legal) ";
        return false;
    }
    
    bool found_invalid_target = false;
    for (const auto& diag : result.diagnostics()) {
        if (diag.kind == LegalityFailureKind::CFGInvalidTarget) {
            found_invalid_target = true;
            break;
        }
    }
    
    if (!found_invalid_target) {
        std::cout << "(expected CFGInvalidTarget diagnostic) ";
        return false;
    }
    
    std::cout << "[CFGInvalidTarget rejected] ";
    return true;
}

// =============================================================================
// NEGATIVE TEST 6: Orphan Commands (Stage 2)
// =============================================================================

bool test_reject_orphan_commands() {
    CrystalScriptIR ir = make_minimal_ir();
    CrystalCFG cfg = make_minimal_cfg(ir);
    
    // Add orphan commands
    cfg.validation.orphan_commands = 5;
    
    LoweringResult lowering = make_minimal_lowering(ir, cfg);
    
    LegalityInput input;
    input.ir = &ir;
    input.decode_complete = true;
    input.round_trip_failures = 0;
    input.unknown_opcodes = 0;
    input.cfg = &cfg;
    input.lowering = &lowering;
    
    LegalityGate gate;
    LegalityResult result = gate.validate(input);
    
    if (result.is_legal) {
        std::cout << "(expected illegal, got legal) ";
        return false;
    }
    
    bool found_orphan = false;
    for (const auto& diag : result.diagnostics()) {
        if (diag.kind == LegalityFailureKind::CFGOrphanCommands) {
            found_orphan = true;
            break;
        }
    }
    
    if (!found_orphan) {
        std::cout << "(expected CFGOrphanCommands diagnostic) ";
        return false;
    }
    
    std::cout << "[CFGOrphanCommands rejected] ";
    return true;
}

// =============================================================================
// NEGATIVE TEST 6b: Overlapping Blocks (Stage 2)
// =============================================================================

bool test_reject_overlapping_blocks() {
    // Create IR with two commands
    CrystalScriptIR ir;
    
    CrystalCommand cmd1;
    cmd1.data = Cmd_End{};
    cmd1.span.rom_address = 0x1000;
    cmd1.span.raw_bytes = {0x91};
    cmd1.status = DecodeStatus::Success;
    ir.commands.push_back(cmd1);
    
    CrystalCommand cmd2;
    cmd2.data = Cmd_End{};
    cmd2.span.rom_address = 0x1001;
    cmd2.span.raw_bytes = {0x91};
    cmd2.status = DecodeStatus::Success;
    ir.commands.push_back(cmd2);
    
    // Create CFG with overlapping blocks - both blocks claim command 0
    CrystalCFG cfg;
    cfg.entry_address = 0x1000;
    cfg.script_name = "test_overlapping";
    cfg.source_ir = &ir;
    
    // Block 0 claims commands 0-1
    BasicBlock block0;
    block0.id = 0;
    block0.start_address = 0x1000;
    block0.end_address = 0x1002;
    block0.command_start = 0;
    block0.command_count = 2;  // Commands 0 and 1
    block0.is_entry = true;
    block0.is_reachable = true;
    block0.exit.kind = ExitKind::Terminal;
    cfg.blocks.push_back(block0);
    
    // Block 1 also claims command 0 (overlap!)
    BasicBlock block1;
    block1.id = 1;
    block1.start_address = 0x1000;
    block1.end_address = 0x1001;
    block1.command_start = 0;
    block1.command_count = 1;  // Command 0 again - OVERLAP
    block1.is_entry = false;
    block1.is_reachable = true;
    block1.exit.kind = ExitKind::Terminal;
    cfg.blocks.push_back(block1);
    
    cfg.address_to_block[0x1000] = 0;
    cfg.command_boundaries.insert(0x1000);
    cfg.command_boundaries.insert(0x1001);
    
    // Validation stats with overlapping commands detected
    cfg.validation.valid = true;  // CFG built but has overlap issue
    cfg.validation.commands_covered = 2;
    cfg.validation.commands_total = 2;
    cfg.validation.overlapping_commands = 1;  // Command 0 is in both blocks
    cfg.validation.terminal_exits = 2;
    
    LoweringResult lowering = make_minimal_lowering(ir, cfg);
    lowering.commands_consumed = 2;
    lowering.commands_lowered = 2;
    
    LegalityInput input;
    input.ir = &ir;
    input.decode_complete = true;
    input.round_trip_failures = 0;
    input.unknown_opcodes = 0;
    input.cfg = &cfg;
    input.lowering = &lowering;
    
    LegalityGate gate;
    LegalityResult result = gate.validate(input);
    
    if (result.is_legal) {
        std::cout << "(expected illegal, got legal) ";
        return false;
    }
    
    bool found_overlapping = false;
    for (const auto& diag : result.diagnostics()) {
        if (diag.kind == LegalityFailureKind::CFGOverlappingBlocks) {
            found_overlapping = true;
            break;
        }
    }
    
    if (!found_overlapping) {
        std::cout << "(expected CFGOverlappingBlocks diagnostic) ";
        return false;
    }
    
    std::cout << "[CFGOverlappingBlocks rejected] ";
    return true;
}

// =============================================================================
// NEGATIVE TEST 7: Unresolved Native Target (Stage 3)
// =============================================================================

bool test_reject_unresolved_native() {
    // Create IR with a callasm to unclassified address
    CrystalScriptIR ir;
    
    CrystalCommand cmd;
    Cmd_Callasm callasm;
    callasm.bank = 0x10;
    callasm.pointer = 0x5000;
    callasm.flat_address = 0x45000;  // Unclassified native target
    cmd.data = callasm;
    cmd.span.rom_address = 0x1000;
    cmd.span.raw_bytes = {0x0E, 0x10, 0x00, 0x50};
    cmd.status = DecodeStatus::Success;
    ir.commands.push_back(cmd);
    
    // Add end command
    CrystalCommand end_cmd;
    end_cmd.data = Cmd_End{};
    end_cmd.span.rom_address = 0x1004;
    end_cmd.span.raw_bytes = {0x91};
    end_cmd.status = DecodeStatus::Success;
    ir.commands.push_back(end_cmd);
    
    CrystalCFG cfg = make_minimal_cfg(ir);
    cfg.blocks[0].command_count = 2;
    cfg.command_boundaries.insert(0x1004);
    cfg.validation.commands_total = 2;
    cfg.validation.commands_covered = 2;
    
    LoweringResult lowering = make_minimal_lowering(ir, cfg);
    lowering.commands_consumed = 2;
    lowering.commands_lowered = 2;
    
    // Create native registry WITHOUT the target classified
    NativeCallRegistry native_registry;
    // Don't register 0x45000 - it's unclassified
    
    LegalityInput input;
    input.ir = &ir;
    input.decode_complete = true;
    input.round_trip_failures = 0;
    input.unknown_opcodes = 0;
    input.cfg = &cfg;
    input.native_registry = &native_registry;
    input.lowering = &lowering;
    
    LegalityGate gate;
    LegalityResult result = gate.validate(input);
    
    if (result.is_legal) {
        std::cout << "(expected illegal, got legal) ";
        return false;
    }
    
    bool found_unresolved = false;
    for (const auto& diag : result.diagnostics()) {
        if (diag.kind == LegalityFailureKind::UnresolvedNativeTarget) {
            found_unresolved = true;
            break;
        }
    }
    
    if (!found_unresolved) {
        std::cout << "(expected UnresolvedNativeTarget diagnostic) ";
        return false;
    }
    
    std::cout << "[UnresolvedNativeTarget rejected] ";
    return true;
}

// =============================================================================
// NEGATIVE TEST 7b: Unresolved RAM Address (Stage 3)
// =============================================================================

bool test_reject_unresolved_ram() {
    // Create IR with a memcallasm reading from unclassified RAM address
    CrystalScriptIR ir;
    
    CrystalCommand cmd;
    Cmd_Memcallasm memcallasm;
    memcallasm.ram_address = 0xD123;  // Unclassified RAM address
    cmd.data = memcallasm;
    cmd.span.rom_address = 0x1000;
    cmd.span.raw_bytes = {0x10, 0x23, 0xD1};  // memcallasm opcode + RAM addr
    cmd.status = DecodeStatus::Success;
    ir.commands.push_back(cmd);
    
    // Add end command
    CrystalCommand end_cmd;
    end_cmd.data = Cmd_End{};
    end_cmd.span.rom_address = 0x1003;
    end_cmd.span.raw_bytes = {0x91};
    end_cmd.status = DecodeStatus::Success;
    ir.commands.push_back(end_cmd);
    
    CrystalCFG cfg = make_minimal_cfg(ir);
    cfg.blocks[0].command_count = 2;
    cfg.command_boundaries.insert(0x1003);
    cfg.validation.commands_total = 2;
    cfg.validation.commands_covered = 2;
    
    LoweringResult lowering = make_minimal_lowering(ir, cfg);
    lowering.commands_consumed = 2;
    lowering.commands_lowered = 2;
    
    // Create RAM registry WITHOUT the address classified
    RamAddressRegistry ram_registry;
    // Don't register 0xD123 - it's unclassified
    
    LegalityInput input;
    input.ir = &ir;
    input.decode_complete = true;
    input.round_trip_failures = 0;
    input.unknown_opcodes = 0;
    input.cfg = &cfg;
    input.ram_registry = &ram_registry;
    input.lowering = &lowering;
    
    LegalityGate gate;
    LegalityResult result = gate.validate(input);
    
    if (result.is_legal) {
        std::cout << "(expected illegal, got legal) ";
        return false;
    }
    
    bool found_unresolved_ram = false;
    for (const auto& diag : result.diagnostics()) {
        if (diag.kind == LegalityFailureKind::UnresolvedRAMAddress) {
            found_unresolved_ram = true;
            break;
        }
    }
    
    if (!found_unresolved_ram) {
        std::cout << "(expected UnresolvedRAMAddress diagnostic) ";
        return false;
    }
    
    std::cout << "[UnresolvedRAMAddress rejected] ";
    return true;
}

// =============================================================================
// NEGATIVE TEST 8: Unlowered Command (Stage 4)
// =============================================================================

bool test_reject_unlowered_command() {
    CrystalScriptIR ir = make_minimal_ir();
    CrystalCFG cfg = make_minimal_cfg(ir);
    
    // Create lowering result with unlowered commands
    LoweringResult lowering;
    lowering.ir.script_id = "test_script";
    lowering.ir.script_name = "test_script";
    lowering.success = false;
    lowering.commands_consumed = 5;
    lowering.commands_lowered = 3;
    lowering.commands_unlowered = 2;  // 2 unlowered commands
    lowering.commands_absorbed = 0;
    lowering.unlowered_by_opcode[0x99] = 2;  // Some unlowered opcode
    
    // Add unlowered diagnostic
    UnloweredDiagnostic diag;
    diag.opcode = 0x99;
    diag.reason = "test unlowered";
    lowering.unlowered.push_back(diag);
    
    LegalityInput input;
    input.ir = &ir;
    input.decode_complete = true;
    input.round_trip_failures = 0;
    input.unknown_opcodes = 0;
    input.cfg = &cfg;
    input.lowering = &lowering;
    
    LegalityGate gate;
    LegalityResult result = gate.validate(input);
    
    if (result.is_legal) {
        std::cout << "(expected illegal, got legal) ";
        return false;
    }
    
    bool found_unlowered = false;
    for (const auto& diag : result.diagnostics()) {
        if (diag.kind == LegalityFailureKind::UnloweredCommand) {
            found_unlowered = true;
            break;
        }
    }
    
    if (!found_unlowered) {
        std::cout << "(expected UnloweredCommand diagnostic) ";
        return false;
    }
    
    std::cout << "[UnloweredCommand rejected] ";
    return true;
}

// =============================================================================
// NEGATIVE TEST 9: Accounting Mismatch (Stage 4)
// =============================================================================

bool test_reject_accounting_mismatch() {
    CrystalScriptIR ir = make_minimal_ir();
    CrystalCFG cfg = make_minimal_cfg(ir);
    
    // Create lowering result with accounting mismatch
    LoweringResult lowering;
    lowering.ir.script_id = "test_script";
    lowering.ir.script_name = "test_script";
    lowering.success = true;
    lowering.commands_consumed = 10;
    lowering.commands_lowered = 5;
    lowering.commands_unlowered = 2;
    lowering.commands_absorbed = 1;
    // Mismatch: 5 + 2 + 1 = 8 != 10
    
    LegalityInput input;
    input.ir = &ir;
    input.decode_complete = true;
    input.round_trip_failures = 0;
    input.unknown_opcodes = 0;
    input.cfg = &cfg;
    input.lowering = &lowering;
    
    LegalityGate gate;
    LegalityResult result = gate.validate(input);
    
    if (result.is_legal) {
        std::cout << "(expected illegal, got legal) ";
        return false;
    }
    
    bool found_mismatch = false;
    for (const auto& diag : result.diagnostics()) {
        if (diag.kind == LegalityFailureKind::AccountingMismatch) {
            found_mismatch = true;
            break;
        }
    }
    
    if (!found_mismatch) {
        std::cout << "(expected AccountingMismatch diagnostic) ";
        return false;
    }
    
    std::cout << "[AccountingMismatch rejected] ";
    return true;
}

// =============================================================================
// NEGATIVE TEST 10: Compiler Diagnostic Residue (Stage 4)
// =============================================================================

bool test_reject_diagnostic_residue() {
    CrystalScriptIR ir = make_minimal_ir();
    CrystalCFG cfg = make_minimal_cfg(ir);
    
    // Create lowering result with diagnostic residue
    LoweringResult lowering;
    lowering.ir.script_id = "test_script";
    lowering.ir.script_name = "test_script";
    lowering.success = true;
    lowering.commands_consumed = 5;
    lowering.commands_lowered = 5;
    lowering.commands_unlowered = 0;
    lowering.commands_absorbed = 0;
    
    // But leave diagnostics (should have been cleaned up)
    UnloweredDiagnostic diag;
    diag.opcode = 0x55;
    diag.reason = "leftover diagnostic";
    lowering.unlowered.push_back(diag);  // Residue!
    
    LegalityInput input;
    input.ir = &ir;
    input.decode_complete = true;
    input.round_trip_failures = 0;
    input.unknown_opcodes = 0;
    input.cfg = &cfg;
    input.lowering = &lowering;
    
    LegalityGate gate;
    LegalityResult result = gate.validate(input);
    
    if (result.is_legal) {
        std::cout << "(expected illegal, got legal) ";
        return false;
    }
    
    bool found_residue = false;
    for (const auto& diag : result.diagnostics()) {
        if (diag.kind == LegalityFailureKind::CompilerDiagnosticResidue) {
            found_residue = true;
            break;
        }
    }
    
    if (!found_residue) {
        std::cout << "(expected CompilerDiagnosticResidue diagnostic) ";
        return false;
    }
    
    std::cout << "[CompilerDiagnosticResidue rejected] ";
    return true;
}

// =============================================================================
// NEGATIVE TEST 11: Invalid Semantic Reference (Stage 5)
// =============================================================================

bool test_reject_invalid_semantic_id() {
    CrystalScriptIR ir = make_minimal_ir();
    CrystalCFG cfg = make_minimal_cfg(ir);
    
    // Create lowering result with invalid semantic ID in IR
    LoweringResult lowering;
    lowering.ir.script_id = "test_script";
    lowering.ir.script_name = "test_script";
    lowering.ir.source_rom_address = 0x1000;
    lowering.success = true;
    lowering.commands_consumed = 1;
    lowering.commands_lowered = 1;
    lowering.commands_unlowered = 0;
    lowering.commands_absorbed = 0;
    
    // Create a semantic block with invalid flag ID
    SemanticBasicBlock sem_block;
    sem_block.id = 0;
    sem_block.label = "block_0";
    sem_block.is_entry = true;
    
    SemanticInstruction inst;
    Sem_SetFlag set_flag;
    set_flag.flag = 0xFFFF;  // Invalid flag ID
    inst.op = set_flag;
    sem_block.instructions.push_back(inst);
    
    lowering.ir.blocks.push_back(sem_block);
    
    LegalityInput input;
    input.ir = &ir;
    input.decode_complete = true;
    input.round_trip_failures = 0;
    input.unknown_opcodes = 0;
    input.cfg = &cfg;
    input.lowering = &lowering;
    
    LegalityGate gate;
    LegalityResult result = gate.validate(input);
    
    if (result.is_legal) {
        std::cout << "(expected illegal, got legal) ";
        return false;
    }
    
    bool found_invalid = false;
    for (const auto& diag : result.diagnostics()) {
        if (diag.kind == LegalityFailureKind::InvalidSemanticReference) {
            found_invalid = true;
            break;
        }
    }
    
    if (!found_invalid) {
        std::cout << "(expected InvalidSemanticReference diagnostic) ";
        return false;
    }
    
    std::cout << "[InvalidSemanticReference rejected] ";
    return true;
}

// =============================================================================
// POSITIVE TEST: Valid Script Passes
// =============================================================================

bool test_valid_script_passes() {
    CrystalScriptIR ir = make_minimal_ir();
    CrystalCFG cfg = make_minimal_cfg(ir);
    LoweringResult lowering = make_minimal_lowering(ir, cfg);
    
    LegalityInput input;
    input.ir = &ir;
    input.decode_complete = true;
    input.round_trip_failures = 0;
    input.unknown_opcodes = 0;
    input.cfg = &cfg;
    input.lowering = &lowering;
    
    LegalityGate gate;
    LegalityResult result = gate.validate(input);
    
    if (!result.is_legal) {
        std::cout << "(expected legal, got illegal: ";
        if (!result.diagnostics().empty()) {
            std::cout << result.diagnostics()[0].reason;
        }
        std::cout << ") ";
        return false;
    }
    
    std::cout << "[valid script accepted] ";
    return true;
}

// =============================================================================
// MAIN
// =============================================================================

int main() {
    std::cout << "\n=== Stage 5: Legality Gate Negative Tests ===\n\n";
    
    // Positive test first - verify valid scripts pass
    RUN_TEST(test_valid_script_passes);
    
    std::cout << "\n--- Stage 1 Rejections ---\n";
    RUN_TEST(test_reject_unknown_opcode);
    RUN_TEST(test_reject_roundtrip_failure);
    RUN_TEST(test_reject_decode_incomplete);
    
    std::cout << "\n--- Stage 2 Rejections ---\n";
    RUN_TEST(test_reject_unclosed_cfg);
    RUN_TEST(test_reject_invalid_cfg_target);
    RUN_TEST(test_reject_orphan_commands);
    RUN_TEST(test_reject_overlapping_blocks);
    
    std::cout << "\n--- Stage 3 Rejections ---\n";
    RUN_TEST(test_reject_unresolved_native);
    RUN_TEST(test_reject_unresolved_ram);
    
    std::cout << "\n--- Stage 4 Rejections ---\n";
    RUN_TEST(test_reject_unlowered_command);
    RUN_TEST(test_reject_accounting_mismatch);
    RUN_TEST(test_reject_diagnostic_residue);
    
    std::cout << "\n--- Stage 5 Rejections ---\n";
    RUN_TEST(test_reject_invalid_semantic_id);
    
    std::cout << "\n=== Results ===\n";
    std::cout << "Passed: " << tests_passed << "\n";
    std::cout << "Failed: " << (tests_run - tests_passed) << "\n";
    
    if (tests_passed == tests_run) {
        std::cout << "\n*** ALL NEGATIVE TESTS PASS ***\n";
        std::cout << "Legality gate demonstrably rejects known-bad scripts.\n";
        return 0;
    } else {
        return 1;
    }
}
