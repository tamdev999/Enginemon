#pragma once
// crystal/script/native_registry.hpp
// Stage 3: NativeCallRegistry + RamAddressRegistry
//
// Classifies non-script machine-facing references so semantic lowering
// never needs raw ASM targets or GB RAM addresses.
//
// RULES:
// - No ASM interpreter or SM83 fallback
// - Unknown targets remain explicitly Opaque/Unknown
// - No raw native/RAM dependency leaks to runtime
// - Identify semantic behavior names but do not lower yet

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <optional>

namespace crystal {

// =============================================================================
// NATIVE CALL REGISTRY
// =============================================================================

// Classification of native routine semantic behavior
enum class NativeClassification : uint8_t {
    PureSemantic,       // Maps directly to a semantic operation (e.g., HealParty)
    HostCapability,     // Host/engine capability (e.g., StartMenu, SaveGame)
    Trivial,            // Trivial helper (e.g., GetPartyNickname, EnableWildEncounters)
    Opaque              // Unknown/unanalyzed - control flow unproven
};

inline const char* native_classification_name(NativeClassification c) {
    switch (c) {
        case NativeClassification::PureSemantic:   return "PureSemantic";
        case NativeClassification::HostCapability: return "HostCapability";
        case NativeClassification::Trivial:        return "Trivial";
        case NativeClassification::Opaque:         return "Opaque";
    }
    return "Unknown";
}

// Control-flow behavior of native routine
enum class NativeControlFlow : uint8_t {
    Returns,            // Returns to caller (proven)
    Terminal,           // Does not return (proven terminal)
    ComputedTransfer,   // Transfers control elsewhere (computed/indirect)
    Unknown             // Control flow is unproven
};

inline const char* native_control_flow_name(NativeControlFlow cf) {
    switch (cf) {
        case NativeControlFlow::Returns:          return "Returns";
        case NativeControlFlow::Terminal:         return "Terminal";
        case NativeControlFlow::ComputedTransfer: return "ComputedTransfer";
        case NativeControlFlow::Unknown:          return "Unknown";
    }
    return "Unknown";
}

// Confidence level for classification
enum class Confidence : uint8_t {
    Verified,           // Manually verified from source analysis
    Inferred,           // Inferred from usage patterns
    Unverified          // Not verified - default for unknown
};

inline const char* confidence_name(Confidence c) {
    switch (c) {
        case Confidence::Verified:   return "Verified";
        case Confidence::Inferred:   return "Inferred";
        case Confidence::Unverified: return "Unverified";
    }
    return "Unknown";
}

// Entry in NativeCallRegistry
struct NativeCallEntry {
    uint32_t address;                       // Flat ROM address
    std::string symbol_name;                // Symbol name (e.g., "GetPartyNickname")
    std::string semantic_name;              // Intended semantic behavior name
    NativeClassification classification;
    NativeControlFlow control_flow;
    Confidence confidence;
    std::string source_reference;           // Source/reference for this classification
    std::string notes;                      // Additional notes
    
    bool is_classified() const {
        return classification != NativeClassification::Opaque ||
               control_flow != NativeControlFlow::Unknown;
    }
};

// Forward declaration
struct ExtractionProfile;

// Registry of native call targets
class NativeCallRegistry {
public:
    // Initialize with known native routines from the default Crystal v1.1 profile.
    // Calls initialize_from_profile(ProfileRegistry::instance().Crystal_USA_v1_1).
    void initialize();

    // Initialize from a specific profile (for non-default ROMs / Gold / Silver).
    void initialize_from_profile(const ExtractionProfile& profile);
    
    // Register a native target (from callasm/memcallasm)
    void register_target(uint32_t address);
    
    // Get entry for address
    const NativeCallEntry* get(uint32_t address) const;
    
    // Check if address is classified (not fully opaque)
    bool is_classified(uint32_t address) const;
    
    // Get all registered entries
    const std::unordered_map<uint32_t, NativeCallEntry>& entries() const { return entries_; }
    
    // Statistics
    size_t total_count() const { return entries_.size(); }
    size_t classified_count() const;
    size_t opaque_count() const;
    
    // Count by control flow
    size_t count_by_control_flow(NativeControlFlow cf) const;
    
private:
    std::unordered_map<uint32_t, NativeCallEntry> entries_;
    
    // Add a known native routine
    void add_known(uint32_t address, const char* symbol, const char* semantic,
                   NativeClassification cls, NativeControlFlow cf,
                   const char* source, const char* notes = "");
};

// =============================================================================
// RAM ADDRESS REGISTRY
// =============================================================================

// Classification of RAM address semantic meaning
enum class RamClassification : uint8_t {
    KnownSemanticState,     // Maps to known game state (e.g., wScriptVar)
    KnownCapabilitySlot,    // Known capability/feature slot
    ControlFlowPointer,     // Stores a script/code pointer
    OpaqueRam               // Unknown/unanalyzed RAM
};

inline const char* ram_classification_name(RamClassification c) {
    switch (c) {
        case RamClassification::KnownSemanticState:   return "KnownSemanticState";
        case RamClassification::KnownCapabilitySlot:  return "KnownCapabilitySlot";
        case RamClassification::ControlFlowPointer:   return "ControlFlowPointer";
        case RamClassification::OpaqueRam:            return "OpaqueRam";
    }
    return "Unknown";
}

// Access kind for RAM operation
enum class RamAccessKind : uint8_t {
    Read,               // readmem
    Write,              // writemem  
    Load,               // loadmem (immediate write)
    Jump,               // memjump (read and jump)
    Call,               // memcall (read and call script)
    CallAsm             // memcallasm (read and call native)
};

inline const char* ram_access_kind_name(RamAccessKind k) {
    switch (k) {
        case RamAccessKind::Read:    return "Read";
        case RamAccessKind::Write:   return "Write";
        case RamAccessKind::Load:    return "Load";
        case RamAccessKind::Jump:    return "Jump";
        case RamAccessKind::Call:    return "Call";
        case RamAccessKind::CallAsm: return "CallAsm";
    }
    return "Unknown";
}

// Entry in RamAddressRegistry
struct RamAddressEntry {
    uint16_t address;                       // GB RAM address (0x0000-0xFFFF)
    std::string symbol_name;                // Symbol name (e.g., "wScriptVar")
    std::string semantic_meaning;           // Semantic meaning
    RamClassification classification;
    std::vector<RamAccessKind> observed_accesses;  // How this address was accessed
    Confidence confidence;
    std::string source_reference;
    std::string notes;
    
    bool is_classified() const {
        return classification != RamClassification::OpaqueRam;
    }
    
    void add_access(RamAccessKind kind) {
        for (auto k : observed_accesses) {
            if (k == kind) return;
        }
        observed_accesses.push_back(kind);
    }
};

// Registry of RAM addresses
class RamAddressRegistry {
public:
    // Initialize with known RAM addresses from the default Crystal v1.1 profile.
    void initialize();

    // Initialize from a specific profile (for non-default ROMs / Gold / Silver).
    void initialize_from_profile(const ExtractionProfile& profile);
    
    // Register a RAM address access
    void register_access(uint16_t address, RamAccessKind kind);
    
    // Get entry for address
    const RamAddressEntry* get(uint16_t address) const;
    
    // Check if address is classified (not opaque)
    bool is_classified(uint16_t address) const;
    
    // Get all registered entries
    const std::unordered_map<uint16_t, RamAddressEntry>& entries() const { return entries_; }
    
    // Statistics
    size_t total_count() const { return entries_.size(); }
    size_t classified_count() const;
    size_t opaque_count() const;
    
private:
    std::unordered_map<uint16_t, RamAddressEntry> entries_;
    
    // Add a known RAM address
    void add_known(uint16_t address, const char* symbol, const char* semantic,
                   RamClassification cls, const char* source, const char* notes = "");
};

// =============================================================================
// CORPUS STATISTICS
// =============================================================================

// Statistics for Stage 3 corpus analysis
struct Stage3CorpusStats {
    // Native targets
    size_t unique_native_targets = 0;
    size_t native_targets_classified = 0;
    size_t native_targets_opaque = 0;
    size_t scripts_with_native_refs = 0;
    
    // RAM addresses
    size_t unique_ram_addresses = 0;
    size_t ram_addresses_classified = 0;
    size_t ram_addresses_opaque = 0;
    size_t scripts_with_ram_refs = 0;
    
    // Native control flow breakdown
    size_t native_returns = 0;
    size_t native_terminal = 0;
    size_t native_computed_transfer = 0;
    size_t native_unknown_cf = 0;
};

} // namespace crystal
