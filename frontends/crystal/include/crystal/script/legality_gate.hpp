#pragma once
// crystal/script/legality_gate.hpp
// Stage 5: Hard per-script legality gate for semantic script pipeline
//
// This module validates that a script is safe for production emission.
// Scripts that fail legality CANNOT enter the new semantic pipeline.
//
// FAILURE MEANS:
// - NO partial SemanticScriptIR
// - NO placeholder Lua
// - NO comments/stubs
// - NO silent command dropping
//
// The legality gate is a hard compiler error, not a warning.

#include "crystal/script/crystal_command.hpp"
#include "crystal/script/crystal_cfg.hpp"
#include "crystal/script/native_registry.hpp"
#include "crystal/script/semantic_linker.hpp"
#include "engine/scripting/semantic_ir.hpp"
#include <cstdint>
#include <string>
#include <vector>
#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace crystal {

// =============================================================================
// LEGALITY FAILURE REASONS
// =============================================================================

// Categories of legality failure
enum class LegalityFailureKind : uint8_t {
    // Stage 1: Decode
    DecodeIncomplete,           // Script could not be fully decoded
    RoundTripFailure,           // Decoded command doesn't round-trip
    UnknownOpcode,              // Opcode outside 0x00-0xA9 range
    
    // Stage 2: CFG
    CFGUnclosed,                // CFG has computed/unresolved exits
    CFGInvalidTarget,           // Static edge lands on non-boundary
    CFGOrphanCommands,          // Commands not in any block
    CFGOverlappingBlocks,       // Commands in multiple blocks
    
    // Stage 3: Registry
    UnresolvedNativeTarget,     // Native call to unknown routine
    UnresolvedRAMAddress,       // RAM access to unclassified address
    
    // Stage 4: Lowering
    UnloweredCommand,           // Command has no lowering rule
    AccountingMismatch,         // consumed != lowered + unlowered + absorbed
    CompilerDiagnosticResidue,  // UnloweredDiagnostic in result
    
    // Stage 5: Semantic IR Validation
    InvalidSemanticReference,   // Reference to invalid semantic ID
};

inline const char* legality_failure_kind_name(LegalityFailureKind kind) {
    switch (kind) {
        case LegalityFailureKind::DecodeIncomplete: return "DecodeIncomplete";
        case LegalityFailureKind::RoundTripFailure: return "RoundTripFailure";
        case LegalityFailureKind::UnknownOpcode: return "UnknownOpcode";
        case LegalityFailureKind::CFGUnclosed: return "CFGUnclosed";
        case LegalityFailureKind::CFGInvalidTarget: return "CFGInvalidTarget";
        case LegalityFailureKind::CFGOrphanCommands: return "CFGOrphanCommands";
        case LegalityFailureKind::CFGOverlappingBlocks: return "CFGOverlappingBlocks";
        case LegalityFailureKind::UnresolvedNativeTarget: return "UnresolvedNativeTarget";
        case LegalityFailureKind::UnresolvedRAMAddress: return "UnresolvedRAMAddress";
        case LegalityFailureKind::UnloweredCommand: return "UnloweredCommand";
        case LegalityFailureKind::AccountingMismatch: return "AccountingMismatch";
        case LegalityFailureKind::CompilerDiagnosticResidue: return "CompilerDiagnosticResidue";
        case LegalityFailureKind::InvalidSemanticReference: return "InvalidSemanticReference";
    }
    return "Unknown";
}

// =============================================================================
// LEGALITY DIAGNOSTIC
// =============================================================================

struct LegalityDiagnostic {
    LegalityFailureKind kind;
    std::string script_id;          // Semantic script ID
    std::string provenance;         // Source ROM address or location
    std::string failing_stage;      // "Stage1", "Stage2", etc.
    std::string offending_element;  // What specifically failed
    std::string reason;             // Human-readable explanation
    
    // For detailed diagnostics
    std::optional<uint8_t> opcode;
    std::optional<uint32_t> address;
    std::optional<size_t> block_index;
    std::optional<size_t> instruction_index;
};

// =============================================================================
// LEGALITY RESULT
// =============================================================================

// Result type: either LegalScript or IllegalScript
struct LegalScript {
    std::string script_id;
    enginemon::SemanticScriptIR ir;  // Only valid for legal scripts
    
    // Statistics for legal scripts
    size_t commands_lowered = 0;
    size_t commands_absorbed = 0;
    size_t blocks = 0;
    size_t instructions = 0;
};

struct IllegalScript {
    std::string script_id;
    std::vector<LegalityDiagnostic> diagnostics;
    
    // Which stage failed first
    std::string first_failure_stage;
    LegalityFailureKind first_failure_kind;
    
    // Summary for corpus reporting
    std::string summary() const;
};

// Discriminated union for legality result
struct LegalityResult {
    bool is_legal = false;
    
    // Only one of these is valid based on is_legal
    std::optional<LegalScript> legal;
    std::optional<IllegalScript> illegal;
    
    // Factory methods
    static LegalityResult make_legal(LegalScript script);
    static LegalityResult make_illegal(IllegalScript script);
    
    // Accessors
    const std::string& script_id() const;
    const std::vector<LegalityDiagnostic>& diagnostics() const;
};

// =============================================================================
// LEGALITY GATE
// =============================================================================

// Input for legality validation
struct LegalityInput {
    // Stage 1 output
    const CrystalScriptIR* ir = nullptr;
    bool decode_complete = false;
    size_t round_trip_failures = 0;
    size_t unknown_opcodes = 0;
    
    // Stage 2 output
    const CrystalCFG* cfg = nullptr;
    
    // Stage 3 context
    const NativeCallRegistry* native_registry = nullptr;
    const RamAddressRegistry* ram_registry = nullptr;
    
    // Stage 4 output
    const enginemon::LoweringResult* lowering = nullptr;

    // Stage 5 context: compiled game data for behavior_name validation
    const CompiledGameData* game_data = nullptr;
};

class LegalityGate {
public:
    // Main validation entry point
    // Returns LegalScript if all checks pass, IllegalScript with diagnostics otherwise
    LegalityResult validate(const LegalityInput& input);
    
    // Individual stage checks (for testing)
    std::vector<LegalityDiagnostic> check_stage1(const LegalityInput& input);
    std::vector<LegalityDiagnostic> check_stage2(const LegalityInput& input);
    std::vector<LegalityDiagnostic> check_stage3(const LegalityInput& input);
    std::vector<LegalityDiagnostic> check_stage4(const LegalityInput& input);
    std::vector<LegalityDiagnostic> check_stage5_ir(const LegalityInput& input);
    
private:
    // Helper to create diagnostic
    LegalityDiagnostic make_diagnostic(
        LegalityFailureKind kind,
        const std::string& script_id,
        const std::string& stage,
        const std::string& offending,
        const std::string& reason);
};

// =============================================================================
// CORPUS LEGALITY STATISTICS
// =============================================================================

struct CorpusLegalityStats {
    // Total counts
    size_t total_scripts = 0;
    size_t legal_scripts = 0;
    size_t illegal_scripts = 0;
    
    // Failure breakdown by kind
    std::unordered_map<LegalityFailureKind, size_t> failures_by_kind;
    
    // Failure breakdown by stage
    std::unordered_map<std::string, size_t> failures_by_stage;
    
    // Sample diagnostics for reporting
    std::vector<LegalityDiagnostic> sample_failures;
    static constexpr size_t MAX_SAMPLES = 10;
    
    // Scripts eligible for new pipeline (legal scripts)
    std::vector<std::string> eligible_scripts;
    
    // Helper methods
    void accumulate(const LegalityResult& result);
    double legal_percentage() const;
    void print_summary() const;
};

// =============================================================================
// SCRIPT COUNT RECONCILIATION (1297 vs ~1339 analysis)
// =============================================================================

struct ScriptCountReconciliation {
    // Scripts in corpus (Stage 1-5)
    size_t corpus_unique_addresses = 0;
    std::unordered_set<uint32_t> corpus_addresses;
    
    // Scripts reported by FullGameCompiler
    size_t compiler_total_events = 0;     // Total BG + object events with scripts
    size_t compiler_unique_addresses = 0; // After deduplication
    size_t compiler_deduplicated = 0;     // scripts_compiled before dedup
    
    // Breakdown of difference
    size_t standard_scripts = 0;          // StdScripts table (called but not compiled)
    size_t generated_wrappers = 0;        // Wrapper scripts (not from ROM)
    size_t text_only_scripts = 0;         // jumptext-style (no execution, just text)
    size_t duplicate_addresses = 0;       // Same ROM address, different events
    
    // Analysis results
    std::vector<std::string> categories;
    std::string explanation;
    
    bool is_reconciled() const;
    void print_report() const;
};

} // namespace crystal
