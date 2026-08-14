#pragma once
// crystal/script/crystal_cfg.hpp
// Stage 2: Control-Flow Graph over typed CrystalCommand IR
//
// This module constructs CFGs from already-decoded CrystalScriptIR.
// It does NOT perform decoding - that is Stage 1's responsibility.
//
// Key invariants:
// - Every static edge lands on a valid decoded command boundary
// - Every reachable typed command belongs to exactly one basic block
// - No overlapping blocks
// - No implicit/guessed control-flow edges
// - All non-static transfers are explicitly marked (Computed/Unresolved)
// - Native calls (callasm/memcallasm) are NOT assumed to return

#include "crystal/script/crystal_command.hpp"
#include "crystal/script/native_registry.hpp"
#include <cstdint>
#include <string>
#include <vector>
#include <variant>
#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace crystal {

// Forward declarations
class RomData;
struct ExtractionProfile;

// =============================================================================
// EXIT KINDS
// =============================================================================

// Exit kind for basic block termination
enum class ExitKind : uint8_t {
    Fallthrough,    // Sequential flow to next block (explicit, not implicit adjacency)
    StaticJump,     // Unconditional jump to known static target (sjump, farsjump, jumptext, etc.)
    Conditional,    // Conditional branch (ifequal, iftrue, etc.) - has taken + fallthrough
    StaticCall,     // Call to static script address with return (scall, farscall, callstd)
    Return,         // Return from subroutine (endcallback)
    Terminal,       // Script termination (end, endall, etc.)
    Computed,       // Jump/call via RAM address (memjump, memcall)
    NativeCall,     // Call to native ASM code - control flow unproven (callasm, memcallasm)
    Unresolved,     // Target cannot be statically determined
};

inline const char* exit_kind_name(ExitKind kind) {
    switch (kind) {
        case ExitKind::Fallthrough: return "Fallthrough";
        case ExitKind::StaticJump:  return "StaticJump";
        case ExitKind::Conditional: return "Conditional";
        case ExitKind::StaticCall:  return "StaticCall";
        case ExitKind::Return:      return "Return";
        case ExitKind::Terminal:    return "Terminal";
        case ExitKind::Computed:    return "Computed";
        case ExitKind::NativeCall:  return "NativeCall";
        case ExitKind::Unresolved:  return "Unresolved";
    }
    return "Unknown";
}

// =============================================================================
// BLOCK EXIT INFORMATION
// =============================================================================

// Target of a control-flow edge
struct CFGTarget {
    uint32_t address = 0;           // ROM address of target
    size_t block_id = SIZE_MAX;     // Block ID if resolved (SIZE_MAX = unresolved)
    std::string symbol;             // Optional symbol name
    
    bool is_resolved() const { return block_id != SIZE_MAX; }
};

// Exit information for a basic block
struct BlockExit {
    ExitKind kind = ExitKind::Terminal;
    
    // For StaticJump, Conditional (taken branch), StaticCall (call target), Fallthrough
    std::optional<CFGTarget> primary_target;
    
    // For Conditional (fallthrough after not-taken), StaticCall (return point)
    std::optional<CFGTarget> fallthrough_target;
    
    // For Computed/NativeCall exits: the RAM address or native address
    std::optional<uint32_t> indirect_address;
    
    // Provenance: which command caused this exit
    uint32_t exit_command_address = 0;
    uint8_t exit_opcode = 0;
};

// =============================================================================
// BASIC BLOCK
// =============================================================================

struct BasicBlock {
    // Block identity
    size_t id = 0;                      // Unique block ID within CFG
    uint32_t start_address = 0;         // ROM address of first command
    uint32_t end_address = 0;           // ROM address after last command byte
    
    // Commands in this block (indices into CrystalScriptIR::commands)
    size_t command_start = 0;           // First command index
    size_t command_count = 0;           // Number of commands in block
    
    // Exit information
    BlockExit exit;
    
    // Predecessors (block IDs that can transfer to this block)
    std::vector<size_t> predecessors;
    
    // Provenance for diagnostics
    std::string label;                  // Optional label/symbol
    bool is_entry = false;              // True if this is script entry point
    
    // Computed properties
    bool is_reachable = false;          // Reachable from entry
    bool is_closed() const {
        return exit.kind == ExitKind::StaticJump ||
               exit.kind == ExitKind::Conditional ||
               exit.kind == ExitKind::StaticCall ||
               exit.kind == ExitKind::Return ||
               exit.kind == ExitKind::Terminal ||
               exit.kind == ExitKind::Fallthrough;
    }
    
    bool has_computed_exit() const {
        return exit.kind == ExitKind::Computed;
    }
    
    bool has_native_call_exit() const {
        return exit.kind == ExitKind::NativeCall;
    }
    
    bool has_unresolved_exit() const {
        return exit.kind == ExitKind::Unresolved;
    }
};

// =============================================================================
// STD SCRIPTS TABLE
// =============================================================================

// Resolved StdScripts entry
struct StdScriptEntry {
    uint16_t std_id;            // Index (0-based)
    uint8_t bank;               // Script bank
    uint16_t address;           // Script address in bank
    uint32_t flat_address;      // Resolved flat ROM address
    std::string name;           // Optional name (e.g., "PokecenterNurseScript")
};

// StdScripts table reader
class StdScriptsTable {
public:
    // Load StdScripts table from ROM
    // Returns false if table cannot be loaded
    bool load(const RomData& rom, uint32_t table_address, size_t count);
    
    // Resolve std_id to flat ROM address
    // Returns 0 if invalid std_id
    uint32_t resolve(uint16_t std_id) const;
    
    // Get entry for std_id
    const StdScriptEntry* get(uint16_t std_id) const;
    
    // Number of entries
    size_t size() const { return entries_.size(); }
    
    bool is_loaded() const { return loaded_; }
    
private:
    std::vector<StdScriptEntry> entries_;
    bool loaded_ = false;
};

// =============================================================================
// CRYSTAL CFG
// =============================================================================

// Validation result for CFG construction
struct CFGValidation {
    bool valid = true;
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    
    // Statistics
    size_t commands_covered = 0;
    size_t commands_total = 0;
    size_t orphan_commands = 0;         // Commands not in any block
    size_t overlapping_commands = 0;    // Commands in multiple blocks
    
    // Edge statistics
    size_t fallthrough_edges = 0;       // Explicit fallthrough edges
    size_t static_jump_edges = 0;
    size_t conditional_edges = 0;
    size_t static_call_edges = 0;
    size_t return_edges = 0;
    size_t terminal_exits = 0;
    size_t computed_exits = 0;
    size_t native_call_exits = 0;       // callasm/memcallasm - unproven control flow
    size_t unresolved_exits = 0;
    
    // Target validation
    size_t invalid_targets = 0;         // Edges to non-command-boundary addresses
    std::vector<std::pair<uint32_t, uint32_t>> bad_edges;  // (source_addr, target_addr)
};

// Full CFG for a single script
struct CrystalCFG {
    // Source identity
    uint32_t entry_address = 0;         // Script entry point
    std::string script_name;            // Optional name
    
    // Blocks (indexed by block_id)
    std::vector<BasicBlock> blocks;
    
    // Address-to-block mapping for fast lookup
    std::unordered_map<uint32_t, size_t> address_to_block;
    
    // Command address set for boundary validation
    std::unordered_set<uint32_t> command_boundaries;
    
    // Reference to source IR (not owned)
    const CrystalScriptIR* source_ir = nullptr;
    
    // Validation result
    CFGValidation validation;
    
    // Accessors
    const BasicBlock* entry_block() const {
        if (blocks.empty()) return nullptr;
        return &blocks[0];  // Entry is always block 0
    }
    
    const BasicBlock* block_at_address(uint32_t addr) const {
        auto it = address_to_block.find(addr);
        if (it != address_to_block.end() && it->second < blocks.size()) {
            return &blocks[it->second];
        }
        return nullptr;
    }
    
    bool is_command_boundary(uint32_t addr) const {
        return command_boundaries.contains(addr);
    }
    
    // Statistics
    size_t block_count() const { return blocks.size(); }
    
    bool is_closed() const {
        return validation.computed_exits == 0 && 
               validation.native_call_exits == 0 &&
               validation.unresolved_exits == 0;
    }
    
    bool has_computed_exits() const {
        return validation.computed_exits > 0;
    }
    
    bool has_native_call_exits() const {
        return validation.native_call_exits > 0;
    }
    
    bool has_unresolved_exits() const {
        return validation.unresolved_exits > 0;
    }
};

// =============================================================================
// CFG BUILDER
// =============================================================================

// CFG construction from typed CrystalScriptIR
class CFGBuilder {
public:
    // Set StdScripts table for resolving jumpstd/callstd
    void set_std_scripts(const StdScriptsTable* table) { std_scripts_ = table; }
    
    // Set NativeCallRegistry for checking callasm control flow
    void set_native_registry(const NativeCallRegistry* registry) { native_registry_ = registry; }
    
    // Build CFG from already-decoded IR
    // Does NOT perform decoding - consumes Stage 1 output
    CrystalCFG build(const CrystalScriptIR& ir);
    
    // Validate CFG structural invariants
    static CFGValidation validate(const CrystalCFG& cfg);
    
private:
    const StdScriptsTable* std_scripts_ = nullptr;
    const NativeCallRegistry* native_registry_ = nullptr;
    
    // Phase 1: Identify block boundaries (leaders)
    void identify_leaders(const CrystalScriptIR& ir,
                         std::unordered_set<uint32_t>& leaders);
    
    // Phase 2: Build blocks from command ranges
    void build_blocks(const CrystalScriptIR& ir,
                     const std::unordered_set<uint32_t>& leaders,
                     CrystalCFG& cfg);
    
    // Phase 3: Resolve edges and link blocks
    void link_blocks(CrystalCFG& cfg);
    
    // Phase 4: Compute reachability from entry
    void compute_reachability(CrystalCFG& cfg);
    
    // Helper: Classify command exit behavior
    struct ExitClassification {
        ExitKind kind;
        std::optional<uint32_t> primary_target;     // For jumps/branches/calls
        std::optional<uint32_t> indirect_address;   // For computed/native exits
        bool has_fallthrough;                       // Does execution continue?
        bool ends_block;                            // Does this command end a basic block?
    };
    
    ExitClassification classify_exit(const CrystalCommand& cmd);
    
    // Helper: Get static target address from command
    std::optional<uint32_t> get_static_target(const CrystalCommand& cmd);
    
    // Helper: Resolve StdScripts target
    std::optional<uint32_t> resolve_std_script(uint16_t std_id);
};

// =============================================================================
// CORPUS CFG STATISTICS
// =============================================================================

// Aggregate statistics for CFG validation across corpus
struct CorpusCFGStats {
    size_t total_scripts = 0;
    size_t closed_cfgs = 0;             // Scripts with fully static CFGs
    size_t computed_exit_scripts = 0;   // Scripts with computed exits
    size_t native_call_scripts = 0;     // Scripts with native call exits
    size_t unresolved_exit_scripts = 0; // Scripts with other unresolved exits
    
    size_t total_blocks = 0;
    size_t total_commands = 0;
    
    // Edge counts by kind
    size_t fallthrough_edges = 0;
    size_t static_jump_edges = 0;
    size_t conditional_edges = 0;
    size_t static_call_edges = 0;
    size_t return_edges = 0;
    size_t terminal_exits = 0;
    size_t computed_exits = 0;
    size_t native_call_exits = 0;
    size_t unresolved_exits = 0;
    
    // Validation errors
    size_t invalid_target_edges = 0;    // Edges to non-boundary addresses
    std::vector<std::pair<uint32_t, uint32_t>> sample_bad_edges;  // (script_root, target_addr)
    
    // Per-script failures
    size_t orphan_command_scripts = 0;
    size_t overlapping_block_scripts = 0;
    
    void accumulate(const CrystalCFG& cfg) {
        total_scripts++;
        total_blocks += cfg.blocks.size();
        total_commands += cfg.source_ir ? cfg.source_ir->commands.size() : 0;
        
        if (cfg.is_closed()) {
            closed_cfgs++;
        }
        if (cfg.has_computed_exits()) {
            computed_exit_scripts++;
        }
        if (cfg.has_native_call_exits()) {
            native_call_scripts++;
        }
        if (cfg.has_unresolved_exits()) {
            unresolved_exit_scripts++;
        }
        
        fallthrough_edges += cfg.validation.fallthrough_edges;
        static_jump_edges += cfg.validation.static_jump_edges;
        conditional_edges += cfg.validation.conditional_edges;
        static_call_edges += cfg.validation.static_call_edges;
        return_edges += cfg.validation.return_edges;
        terminal_exits += cfg.validation.terminal_exits;
        computed_exits += cfg.validation.computed_exits;
        native_call_exits += cfg.validation.native_call_exits;
        unresolved_exits += cfg.validation.unresolved_exits;
        
        invalid_target_edges += cfg.validation.invalid_targets;
        
        if (cfg.validation.orphan_commands > 0) {
            orphan_command_scripts++;
        }
        if (cfg.validation.overlapping_commands > 0) {
            overlapping_block_scripts++;
        }
        
        // Sample bad edges
        for (const auto& edge : cfg.validation.bad_edges) {
            if (sample_bad_edges.size() < 10) {
                sample_bad_edges.push_back({cfg.entry_address, edge.second});
            }
        }
    }
    
    bool all_invariants_hold() const {
        return invalid_target_edges == 0 &&
               orphan_command_scripts == 0 &&
               overlapping_block_scripts == 0;
    }
};

} // namespace crystal
