// crystal/script/native_registry.cpp
// Stage 3: NativeCallRegistry + RamAddressRegistry implementation
//
// Classifies non-script machine-facing references so semantic lowering
// never needs raw ASM targets or GB RAM addresses.
//
// All ROM-version-specific addresses are now loaded from the ExtractionProfile
// (profile.offsets.native_calls / profile.offsets.ram_addresses) rather than
// being hardcoded in initialize(). This makes the registry profile-driven.
//
// The parameterless initialize() is kept for backward-compatible call sites
// that construct the registry independently of the compiler (e.g. linker tests
// that load the default Crystal v1.1 profile themselves).

#include "crystal/script/native_registry.hpp"
#include "crystal/rom/profile.hpp"

namespace crystal {

// =============================================================================
// NATIVE CALL REGISTRY IMPLEMENTATION
// =============================================================================

void NativeCallRegistry::add_known(uint32_t address, const char* symbol, const char* semantic,
                                    NativeClassification cls, NativeControlFlow cf,
                                    const char* source, const char* notes) {
    NativeCallEntry entry;
    entry.address = address;
    entry.symbol_name = symbol;
    entry.semantic_name = semantic;
    entry.classification = cls;
    entry.control_flow = cf;
    entry.confidence = Confidence::Verified;
    entry.source_reference = source;
    entry.notes = notes;
    entries_[address] = entry;
}

void NativeCallRegistry::initialize() {
    // Fall back to the default Crystal v1.1 profile from the singleton registry.
    // This path is used by test scaffolding and standalone tools that create
    // NativeCallRegistry without passing the profile directly.
    const auto* profile =
        ProfileRegistry::instance().get_profile(RomVersion::Crystal_USA_v1_1);
    if (profile) {
        initialize_from_profile(*profile);
    }
    // If profile is null (unlikely in practice), the registry stays empty.
}

void NativeCallRegistry::initialize_from_profile(const ExtractionProfile& profile) {
    const auto& o = profile.offsets;
    for (uint8_t i = 0; i < o.native_call_count; ++i) {
        const auto& spec = o.native_calls[i];
        if (spec.flat_address == 0) continue;
        add_known(
            spec.flat_address,
            spec.symbol_name   ? spec.symbol_name   : "",
            spec.semantic_name ? spec.semantic_name : "",
            static_cast<NativeClassification>(static_cast<uint8_t>(spec.classification)),
            static_cast<NativeControlFlow>(static_cast<uint8_t>(spec.control_flow)),
            spec.source_ref ? spec.source_ref : "",
            spec.notes      ? spec.notes      : "");
    }
}
void NativeCallRegistry::register_target(uint32_t address) {
    if (entries_.contains(address)) {
        // Already registered, do nothing
        return;
    }
    
    // Create opaque entry for unknown target
    NativeCallEntry entry;
    entry.address = address;
    entry.symbol_name = "";  // Unknown
    entry.semantic_name = "";
    entry.classification = NativeClassification::Opaque;
    entry.control_flow = NativeControlFlow::Unknown;
    entry.confidence = Confidence::Unverified;
    entry.source_reference = "corpus_scan";
    entry.notes = "Encountered during corpus scan";
    entries_[address] = entry;
}

const NativeCallEntry* NativeCallRegistry::get(uint32_t address) const {
    auto it = entries_.find(address);
    if (it != entries_.end()) {
        return &it->second;
    }
    return nullptr;
}

bool NativeCallRegistry::is_classified(uint32_t address) const {
    auto entry = get(address);
    return entry && entry->is_classified();
}

size_t NativeCallRegistry::classified_count() const {
    size_t count = 0;
    for (const auto& [addr, entry] : entries_) {
        if (entry.is_classified()) {
            count++;
        }
    }
    return count;
}

size_t NativeCallRegistry::opaque_count() const {
    size_t count = 0;
    for (const auto& [addr, entry] : entries_) {
        if (!entry.is_classified()) {
            count++;
        }
    }
    return count;
}

size_t NativeCallRegistry::count_by_control_flow(NativeControlFlow cf) const {
    size_t count = 0;
    for (const auto& [addr, entry] : entries_) {
        if (entry.control_flow == cf) {
            count++;
        }
    }
    return count;
}

// =============================================================================
// RAM ADDRESS REGISTRY IMPLEMENTATION
// =============================================================================

void RamAddressRegistry::add_known(uint16_t address, const char* symbol, const char* semantic,
                                    RamClassification cls, const char* source, const char* notes) {
    RamAddressEntry entry;
    entry.address = address;
    entry.symbol_name = symbol;
    entry.semantic_meaning = semantic;
    entry.classification = cls;
    entry.confidence = Confidence::Verified;
    entry.source_reference = source;
    entry.notes = notes;
    entries_[address] = entry;
}

void RamAddressRegistry::initialize() {
    // Fall back to the default Crystal v1.1 profile from the singleton registry.
    const auto* profile =
        ProfileRegistry::instance().get_profile(RomVersion::Crystal_USA_v1_1);
    if (profile) {
        initialize_from_profile(*profile);
    }
}

void RamAddressRegistry::initialize_from_profile(const ExtractionProfile& profile) {
    const auto& o = profile.offsets;
    for (uint8_t i = 0; i < o.ram_address_count; ++i) {
        const auto& spec = o.ram_addresses[i];
        if (spec.address == 0) continue;
        add_known(
            spec.address,
            spec.symbol_name   ? spec.symbol_name   : "",
            spec.semantic_name ? spec.semantic_name : "",
            static_cast<RamClassification>(spec.classification),
            spec.source_ref ? spec.source_ref : "",
            spec.notes      ? spec.notes      : "");
    }
}
void RamAddressRegistry::register_access(uint16_t address, RamAccessKind kind) {
    auto it = entries_.find(address);
    if (it != entries_.end()) {
        // Already registered, just add access kind
        it->second.add_access(kind);
        return;
    }
    
    // Create opaque entry for unknown address
    RamAddressEntry entry;
    entry.address = address;
    entry.symbol_name = "";  // Unknown
    entry.semantic_meaning = "";
    entry.classification = RamClassification::OpaqueRam;
    entry.confidence = Confidence::Unverified;
    entry.source_reference = "corpus_scan";
    entry.notes = "Encountered during corpus scan";
    entry.add_access(kind);
    entries_[address] = entry;
}

const RamAddressEntry* RamAddressRegistry::get(uint16_t address) const {
    auto it = entries_.find(address);
    if (it != entries_.end()) {
        return &it->second;
    }
    return nullptr;
}

bool RamAddressRegistry::is_classified(uint16_t address) const {
    auto entry = get(address);
    return entry && entry->is_classified();
}

size_t RamAddressRegistry::classified_count() const {
    size_t count = 0;
    for (const auto& [addr, entry] : entries_) {
        if (entry.is_classified()) {
            count++;
        }
    }
    return count;
}

size_t RamAddressRegistry::opaque_count() const {
    size_t count = 0;
    for (const auto& [addr, entry] : entries_) {
        if (!entry.is_classified()) {
            count++;
        }
    }
    return count;
}

} // namespace crystal
