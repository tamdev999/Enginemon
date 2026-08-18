#pragma once
// crystal/compile/corpus_discovery.hpp
// Unified corpus discovery for Crystal script compilation
//
// AUTHORITATIVE: This is the single source of truth for corpus discovery.
// All tools, tests, and the production compiler MUST use this implementation.
//
// Discovery includes:
//   - Object event script roots
//   - BG event script roots
//   - Scene script roots
//   - Callback script roots
//   - StdScript roots
//   - Deferred script roots (sdefer targets, discovered to fixed point)
//
// Deferred Script Discovery:
//   When a script body contains Cmd_Sdefer, the target is a separate executable
//   body that runs AFTER the current script completes. These targets must be
//   discovered as independent roots, not as CFG blocks within the parent.
//
//   The discovery algorithm iterates to fixed point:
//     1. Collect initial roots (object, BG, scene, callback, StdScript)
//     2. Decode each root to get its executable body
//     3. Scan body for Cmd_Sdefer targets
//     4. Add previously unseen targets as new roots
//     5. Repeat until no new roots are discovered

#include "crystal/rom/loader.hpp"
#include "crystal/rom/profile.hpp"
#include "crystal/extract/map_extractor.hpp"
#include "crystal/script/typed_decoder.hpp"
#include "crystal/script/crystal_cfg.hpp"  // For StdScriptsTable
#include <set>
#include <map>
#include <cstdint>
#include <string>

namespace crystal {

//=============================================================================
// ROOT TYPE CLASSIFICATION
//=============================================================================

enum class ScriptRootType : uint8_t {
    Object,         // Object event script
    BgEvent,        // BG/sign event script
    CoordEvent,     // Coordinate-triggered event script
    Scene,          // MapScripts scene script
    Callback,       // MapScripts callback script
    StdScript,      // StdScripts table entry
    Deferred,       // sdefer target (discovered via fixed-point iteration)
};

inline const char* script_root_type_name(ScriptRootType type) {
    switch (type) {
        case ScriptRootType::Object: return "object";
        case ScriptRootType::BgEvent: return "bg_event";
        case ScriptRootType::CoordEvent: return "coord_event";
        case ScriptRootType::Scene: return "scene";
        case ScriptRootType::Callback: return "callback";
        case ScriptRootType::StdScript: return "std_script";
        case ScriptRootType::Deferred: return "deferred";
        default: return "unknown";
    }
}

//=============================================================================
// CORPUS DISCOVERY RESULT
//=============================================================================

struct CorpusDiscoveryStats {
    // Initial root counts (before deferred discovery)
    uint32_t object_roots = 0;
    uint32_t bg_event_roots = 0;
    uint32_t coord_event_roots = 0;
    uint32_t scene_roots = 0;
    uint32_t callback_roots = 0;
    uint32_t std_script_roots = 0;
    
    // Deferred discovery stats
    uint32_t deferred_targets_encountered = 0;  // Total sdefer targets seen
    uint32_t deferred_already_known = 0;         // Targets that were already roots
    uint32_t deferred_new_roots = 0;             // New roots added from sdefer
    uint32_t deferred_iterations = 0;            // Fixed-point iteration count
    
    // Final counts
    uint32_t total_map_roots() const { 
        return object_roots + bg_event_roots + coord_event_roots + scene_roots + callback_roots + deferred_new_roots;
    }
    uint32_t total_unique_bodies() const;
};

struct ScriptRootInfo {
    uint32_t rom_address;
    ScriptRootType root_type;
    enginemon::MapId owning_map;  // MAP_NONE for StdScripts and orphan deferred
    uint8_t local_index;          // Index within category (e.g., object 0, scene 2)
    
    // For deferred roots: which body discovered this target
    uint32_t discovered_from = 0;  // 0 if initial root
};

struct CorpusDiscoveryResult {
    // All discovered script roots (address → info)
    std::map<uint32_t, ScriptRootInfo> map_roots;
    std::set<uint32_t> std_script_addresses;
    
    // Discovery statistics
    CorpusDiscoveryStats stats;
    
    // Combined address set for iteration
    std::set<uint32_t> all_addresses() const {
        std::set<uint32_t> result;
        for (const auto& [addr, info] : map_roots) {
            result.insert(addr);
        }
        for (uint32_t addr : std_script_addresses) {
            result.insert(addr);
        }
        return result;
    }
    
    // Get root type for an address
    ScriptRootType get_root_type(uint32_t addr) const {
        auto it = map_roots.find(addr);
        if (it != map_roots.end()) return it->second.root_type;
        if (std_script_addresses.contains(addr)) return ScriptRootType::StdScript;
        return ScriptRootType::Object; // Default fallback
    }
};

//=============================================================================
// UNIFIED CORPUS DISCOVERY API
//=============================================================================

// Discover complete script corpus with fixed-point deferred discovery.
//
// This is the AUTHORITATIVE corpus discovery implementation.
// DO NOT duplicate this logic elsewhere.
//
// Algorithm:
//   1. Discover reachable maps via discover_reachable_maps()
//   2. Collect initial roots from maps (object, BG, scene, callback)
//   3. Add StdScript roots
//   4. Fixed-point loop:
//      a. For each undecoded root, decode body
//      b. Scan for Cmd_Sdefer targets
//      c. Add new targets as deferred roots
//      d. Repeat until no new roots
//
// Parameters:
//   rom       - ROM data
//   profile   - Extraction profile
//   extractor - Map extractor (for map header parsing)
//   decoder   - TypedScriptDecoder (for body scanning)
//   std_scripts - StdScripts table
//
// Returns:
//   Complete corpus discovery result with all roots and statistics
CorpusDiscoveryResult discover_corpus(
    const RomData& rom,
    const ExtractionProfile& profile,
    MapExtractor& extractor,
    TypedScriptDecoder& decoder,
    const StdScriptsTable& std_scripts);

// Helper: Collect initial roots without deferred discovery
// (Used internally and for testing)
CorpusDiscoveryResult collect_initial_roots(
    const RomData& rom,
    const ExtractionProfile& profile,
    MapExtractor& extractor,
    const StdScriptsTable& std_scripts);

// Helper: Extract sdefer targets from a decoded script IR
// Returns set of flat ROM addresses (resolved from bank-relative pointers)
std::set<uint32_t> extract_sdefer_targets(
    const CrystalScriptIR& ir,
    uint8_t script_bank);

} // namespace crystal
