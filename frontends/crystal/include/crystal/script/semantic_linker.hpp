#pragma once
// crystal/script/semantic_linker.hpp
// Stage 6: Corpus-wide Typed-Reference Linker for SemanticScriptIR
//
// This module validates ALL typed semantic references in the legal script corpus
// against frozen game data BEFORE emission. Requirements:
//
// - NO raw ROM addresses, NO banks/pointers
// - NO Crystal symbol-name runtime lookup
// - NO unresolved textual resource names, NO runtime linker
//
// VALIDATION CLASSIFICATIONS:
// - ExactResolved: Reference validated against compiled resource registry
// - OwnershipValidated: Reference validated against owning scope (e.g., object in map)
// - RangeOnly: Semantic scalar/enumeration domain (not a resource reference)
// - Unresolved: Reference could not be validated - LINK ERROR

#include "engine/scripting/semantic_ir.hpp"
#include "engine/core/types.hpp"
#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <optional>
#include <set>
#include <map>

namespace crystal {

// Forward declarations
class StdScriptsTable;

// =============================================================================
// VALIDATION CLASSIFICATION
// =============================================================================

enum class ValidationClass : uint8_t {
    ExactResolved,      // Validated against compiled resource registry (actual artifact exists)
    OwnershipValidated, // Validated against owning scope (map objects, etc.)
    RangeOnly,          // Semantic scalar/enumeration domain, not resource ref (Flag, Var, etc.)
    PendingDefinition,  // Domain-valid ID but runtime artifact producer not yet built
    Unresolved,         // Could not validate - LINK ERROR
    InvalidOwnership,   // Valid entity but wrong scope - LINK ERROR  
    WrongType,          // Reference type mismatch - LINK ERROR
    InvalidDomain,      // ID outside authoritative domain - LINK ERROR
};

inline const char* validation_class_name(ValidationClass vc) {
    switch (vc) {
        case ValidationClass::ExactResolved: return "ExactResolved";
        case ValidationClass::OwnershipValidated: return "OwnershipValidated";
        case ValidationClass::RangeOnly: return "RangeOnly";
        case ValidationClass::PendingDefinition: return "PendingDefinition";
        case ValidationClass::Unresolved: return "Unresolved";
        case ValidationClass::InvalidOwnership: return "InvalidOwnership";
        case ValidationClass::WrongType: return "WrongType";
        case ValidationClass::InvalidDomain: return "InvalidDomain";
    }
    return "Unknown";
}

// =============================================================================
// REFERENCE TYPES AND THEIR VALIDATION REQUIREMENTS
// =============================================================================
// 
// ExactResolved (actual compiled artifacts exist):
//   Map, ElevatorId, Trainer, StdScript, PokeMailId, TextId
//
// PendingDefinition (authoritative closed-domain membership, artifact producer not yet compiled):
//   Species, Item, Music, Sfx, PhonePerson, TradeId, FruitTreeId, MartId, Emote
//   NOTE: These use Crystal profile table counts as domain authority.
//         The domain is proven contiguous (no holes) for supported ROM profiles.
//         NOT extracted definition membership - that comes when extractors are built.
//
// OwnershipValidated (requires scope context - owner is consulted):
//   Object (map-local: validated against map_object_counts)
//   WarpId (map-local: validated against map_warp_counts)
//
// RangeOnly (semantic scalar domains, not resource references):
//   Flag, Var (script variables, not resource IDs)
//   StateVar (mini-game state indices)
//   Scene (map scene numbers 0-255)
//   TimeFlags (time-of-day bitfield)
//   Label (intra-script CFG target - validated by Stage 2)
//   Block (metatile index 0-255: would need tileset context for true ownership,
//          but Crystal tilesets always define the full 8-bit range)
//
// Map references are ExactResolved against compiled map set

enum class ReferenceType : uint8_t {
    Species,        // PendingDefinition: SpeciesId in domain, awaits registry
    Item,           // PendingDefinition: ItemId in domain, awaits registry
    Map,            // ExactResolved: MapId in compiled map set
    Flag,           // RangeOnly: Event flag index (semantic domain 0..N)
    Var,            // RangeOnly: Script variable index (semantic domain)
    StateVar,       // RangeOnly: Mini-game state index (semantic domain)
    Music,          // PendingDefinition: MusicId in domain, awaits registry
    Sfx,            // PendingDefinition: SfxId in domain, awaits registry
    Trainer,        // ExactResolved: (group,id) in TrainerRegistry
    Object,         // OwnershipValidated: object index in owning map
    Label,          // Stage2Validated: CFG block target (not corpus linking)
    StdScript,      // ExactResolved: std_id against compiled/legalized bodies
    // NOTE: Special (raw Crystal Special ID) is intentionally absent.
    //   Sem_Special is rejected by Stage 5 and never reaches Stage 6.
    //   All Special opcodes lower to dedicated SemanticOps or Sem_GameSpecificEvent.
    Emote,          // PendingDefinition: emote bubble index in domain, awaits registry
    Scene,          // RangeOnly: map scene number (semantic domain 0-255)
    WarpId,         // OwnershipValidated: warp index in owning map
    PhonePerson,    // PendingDefinition: phone contact in domain, awaits registry
    TimeFlags,      // RangeOnly: time-of-day bitfield (semantic domain)
    TradeId,        // PendingDefinition: in-game trade in domain, awaits registry
    ElevatorId,     // ExactResolved: elevator floor list in compiled data
    FruitTreeId,    // PendingDefinition: fruit tree in domain, awaits registry
    MartId,         // PendingDefinition: mart inventory in domain, awaits registry
    Block,          // OwnershipValidated: metatile in owning tileset
    PokeMailId,     // ExactResolved: semantic mail ID from PokeMail registry
    TextId,         // ExactResolved: semantic text ID from Text registry
};

inline const char* reference_type_name(ReferenceType type) {
    switch (type) {
        case ReferenceType::Species: return "Species";
        case ReferenceType::Item: return "Item";
        case ReferenceType::Map: return "Map";
        case ReferenceType::Flag: return "Flag";
        case ReferenceType::Var: return "Var";
        case ReferenceType::StateVar: return "StateVar";
        case ReferenceType::Music: return "Music";
        case ReferenceType::Sfx: return "Sfx";
        case ReferenceType::Trainer: return "Trainer";
        case ReferenceType::Object: return "Object";
        case ReferenceType::Label: return "Label";
        case ReferenceType::StdScript: return "StdScript";
        case ReferenceType::Emote: return "Emote";
        case ReferenceType::Scene: return "Scene";
        case ReferenceType::WarpId: return "WarpId";
        case ReferenceType::PhonePerson: return "PhonePerson";
        case ReferenceType::TimeFlags: return "TimeFlags";
        case ReferenceType::TradeId: return "TradeId";
        case ReferenceType::ElevatorId: return "ElevatorId";
        case ReferenceType::FruitTreeId: return "FruitTreeId";
        case ReferenceType::MartId: return "MartId";
        case ReferenceType::Block: return "Block";
        case ReferenceType::PokeMailId: return "PokeMailId";
        case ReferenceType::TextId: return "TextId";
    }
    return "Unknown";
}

// Get expected validation class for a reference type
inline ValidationClass expected_validation_class(ReferenceType type) {
    switch (type) {
        // ExactResolved - requires actual compiled artifacts
        case ReferenceType::Map:         // Validated against compiled map set
        case ReferenceType::ElevatorId:  // Validated against compiled elevator registry
        case ReferenceType::Trainer:     // Validated against TrainerRegistry (ROM extraction)
        case ReferenceType::StdScript:   // Validated against compiled/legalized bodies
        case ReferenceType::PokeMailId:  // Validated against compiled PokeMail registry
        case ReferenceType::TextId:      // Validated against compiled Text registry
            return ValidationClass::ExactResolved;
            
        // PendingDefinition - authoritative closed-domain membership from Crystal profile
        // Domain is proven contiguous (no holes) for supported ROM profiles.
        // Actual extracted definitions will replace count-derived membership when built.
        case ReferenceType::Species:      // Closed domain [1, num_pokemon]
        case ReferenceType::Item:         // Closed domain [0, num_items)
        case ReferenceType::Music:        // Closed domain [0, num_music)
        case ReferenceType::Sfx:          // Closed domain [0, num_sfx)
        case ReferenceType::PhonePerson:  // Closed domain [0, num_phone_contacts)
        case ReferenceType::TradeId:      // Closed domain [0, num_npc_trades)
        case ReferenceType::FruitTreeId:  // Closed domain [1, num_fruit_trees] (1-indexed)
        case ReferenceType::MartId:       // Closed domain [0, num_marts)
        case ReferenceType::Emote:        // Closed domain [0, num_emotes)
            return ValidationClass::PendingDefinition;
            
        // OwnershipValidated - requires scope context, owner is actually consulted
        case ReferenceType::Object:       // Validated against map_object_counts[owning_map]
        case ReferenceType::WarpId:       // Validated against map_warp_counts[owning_map]
            return ValidationClass::OwnershipValidated;
            
        // RangeOnly - semantic scalar domains, not resource refs
        case ReferenceType::Flag:         // Event flag indices
        case ReferenceType::Var:          // Script variable indices
        case ReferenceType::StateVar:     // Mini-game state indices
        case ReferenceType::Scene:        // Map scene numbers
        case ReferenceType::TimeFlags:    // Time-of-day bitfield
        case ReferenceType::Label:        // CFG targets (validated by Stage 2)
        case ReferenceType::Block:        // Metatile indices [0,255] - would need tileset for ownership
            return ValidationClass::RangeOnly;
    }
    return ValidationClass::Unresolved;
}

// =============================================================================
// VALIDATED REFERENCE
// =============================================================================

struct ValidatedReference {
    ReferenceType type;
    uint32_t value;
    ValidationClass validation;
    std::string script_id;
    size_t block_index;
    size_t instruction_index;
    std::string op_name;
    
    // For compound references
    std::optional<uint16_t> secondary_value;  // trainer_id
    std::optional<enginemon::MapId> owning_map;
    
    // Error info (if validation != ExactResolved/OwnershipValidated/RangeOnly)
    std::string error_reason;
};

// =============================================================================
// LINKER DIAGNOSTIC
// =============================================================================

struct LinkerDiagnostic {
    ValidationClass result;
    ReferenceType ref_type;
    uint32_t ref_value;
    std::string script_id;
    std::string op_name;
    std::string reason;
    size_t block_index;
    size_t instruction_index;
    
    std::string to_string() const;
};

// =============================================================================
// REFERENCE STATISTICS BY VALIDATION CLASS
// =============================================================================

struct ValidationStats {
    std::map<ReferenceType, size_t> exact_resolved;
    std::map<ReferenceType, size_t> ownership_validated;
    std::map<ReferenceType, size_t> range_only;
    std::map<ReferenceType, size_t> pending_definition;
    std::map<ReferenceType, size_t> unresolved;
    std::map<ReferenceType, size_t> invalid_ownership;
    std::map<ReferenceType, size_t> wrong_type;
    std::map<ReferenceType, size_t> invalid_domain;
    
    // Unique values by type
    std::map<ReferenceType, std::set<uint32_t>> unique_by_type;
    
    size_t total_exact_resolved() const;
    size_t total_ownership_validated() const;
    size_t total_range_only() const;
    size_t total_pending_definition() const;
    size_t total_unresolved() const;
    size_t total_invalid_ownership() const;
    size_t total_wrong_type() const;
    size_t total_invalid_domain() const;
    size_t total_errors() const;
    
    void add(const ValidatedReference& ref);
    void print_summary() const;
};

// =============================================================================
// COMPILED GAME DATA (Built from actual compiler output)
// =============================================================================

// Represents the actual compiled game data for link-time validation
// NOT Crystal numeric ranges - actual compiled resources
struct CompiledGameData {
    // Species: exact compiled set from ROM extraction
    std::unordered_set<enginemon::SpeciesId> species;
    
    // Items: exact compiled set from ROM extraction
    std::unordered_set<enginemon::ItemId> items;
    
    // Maps: exact compiled map set (discovered via fixed-point)
    std::unordered_set<enginemon::MapId> maps;
    
    // Map object counts: how many objects each compiled map has
    std::unordered_map<enginemon::MapId, uint8_t> map_object_counts;
    
    // Map warp counts: how many warps each compiled map has
    std::unordered_map<enginemon::MapId, uint8_t> map_warp_counts;
    
    // Music: exact compiled music set
    std::unordered_set<enginemon::MusicId> music;
    
    // Sound effects: exact compiled sfx set
    std::unordered_set<enginemon::SfxId> sfx;
    
    // Trainers: exact compiled (group, id) pairs
    std::set<std::pair<uint8_t, uint8_t>> trainers;
    
    // StdScripts: exact std_id values with valid targets
    // Key = std_id, Value = ROM address
    // StdScripts: std_id -> ROM address (from ROM table)
    // Key = std_id, Value = ROM address
    std::unordered_map<uint16_t, uint32_t> std_scripts;
    
    // StdScripts that were successfully compiled through Stages 1-5
    // Only these should validate as ExactResolved
    std::unordered_set<uint16_t> compiled_std_scripts;
    
    // Specials: intentionally removed.
    // Sem_Special is rejected at Stage 5; no Special references reach Stage 6.
    // has_special() is gone — use has_behavior_name() for Sem_GameSpecificEvent validation.
    
    // Phone contacts: exact compiled phone person IDs
    std::unordered_set<uint8_t> phone_persons;
    
    // Trades: exact compiled trade IDs
    std::unordered_set<uint8_t> trades;
    
    // Fruit trees: exact compiled fruit tree IDs  
    std::unordered_set<uint8_t> fruit_trees;
    
    // Elevators: exact compiled elevator floor list IDs
    std::unordered_set<uint16_t> elevators;
    
    // Marts: exact compiled mart inventory IDs
    std::unordered_set<uint16_t> marts;
    
    // Emotes: valid emote bubble indices
    std::unordered_set<uint8_t> emotes;
    
    // PokeMail: exact compiled mail IDs from PokeMail registry
    std::unordered_set<enginemon::PokeMailId> pokemail_ids;
    
    // Text: exact compiled text IDs from Text registry (win/loss text, etc.)
    std::unordered_set<enginemon::TextId> text_ids;

    // Behavior names: all valid Sem_GameSpecificEvent behavior_name values.
    // Populated from the canonical special_pointers table in the legalizer.
    // Stage 5 legality gate validates every Sem_GameSpecificEvent.behavior_name
    // is present here. Unknown behavior names hard-fail.
    std::unordered_set<std::string> behavior_names;
    
    // Helper to load StdScripts from table
    void load_std_scripts(const StdScriptsTable& table);
    
    // Validation methods (return true if entity exists in compiled data)
    bool has_species(enginemon::SpeciesId id) const { return species.contains(id); }
    bool has_item(enginemon::ItemId id) const { return items.contains(id); }
    bool has_map(enginemon::MapId id) const { return maps.contains(id); }
    bool has_music(enginemon::MusicId id) const { return music.contains(id); }
    bool has_sfx(enginemon::SfxId id) const { return sfx.contains(id); }
    bool has_trainer(uint8_t group, uint8_t id) const;
    bool has_std_script(uint16_t id) const { return compiled_std_scripts.contains(id); }
    // has_special() removed — Sem_Special never reaches Stage 6 (rejected at Stage 5)
    bool has_phone_person(uint8_t id) const { return phone_persons.contains(id); }
    bool has_trade(uint8_t id) const { return trades.contains(id); }
    bool has_fruit_tree(uint8_t id) const { return fruit_trees.contains(id); }
    bool has_elevator(uint16_t id) const { return elevators.contains(id); }
    bool has_mart(uint16_t id) const { return marts.contains(id); }
    bool has_emote(uint8_t id) const { return emotes.contains(id); }
    bool has_pokemail(enginemon::PokeMailId id) const { return pokemail_ids.contains(id); }
    bool has_text(enginemon::TextId id) const { return text_ids.contains(id); }
    
    // Ownership validation
    bool is_valid_object(enginemon::MapId map, uint8_t object_id) const;
    bool is_valid_warp(enginemon::MapId map, uint8_t warp_id) const;
};

// =============================================================================
// LINKED CORPUS RESULT
// =============================================================================

struct LinkedCorpus {
    // Input statistics
    size_t map_root_bodies = 0;
    size_t std_script_bodies = 0;
    size_t total_bodies = 0;
    size_t overlap = 0;
    
    size_t total_blocks = 0;
    size_t total_instructions = 0;
    
    // Validation statistics
    ValidationStats stats;
    
    // Diagnostics (errors only)
    std::vector<LinkerDiagnostic> diagnostics;
    
    // Per-script link status
    std::unordered_map<std::string, bool> script_link_status;
    size_t scripts_linked_ok = 0;
    size_t scripts_with_errors = 0;
    
    bool all_linked() const { 
        return stats.total_errors() == 0 && scripts_with_errors == 0; 
    }
    void print_report() const;
};

// =============================================================================
// SEMANTIC LINKER
// =============================================================================

class SemanticLinker {
public:
    // Set compiled game data for validation
    void set_game_data(const CompiledGameData* data) { game_data_ = data; }
    
    // Set script owning map context (for object reference validation)
    void set_script_context(const std::string& script_id, enginemon::MapId owning_map);
    
    // Link a single script (validates all references)
    std::vector<ValidatedReference> link_script(
        const enginemon::SemanticScriptIR& ir);
    
    // Link the full 1349-body corpus:
    // map_root_scripts: 1297 map event scripts
    // std_script_bodies: 52 StdScript bodies
    LinkedCorpus link_full_corpus(
        const std::vector<enginemon::SemanticScriptIR>& map_root_scripts,
        const std::vector<enginemon::SemanticScriptIR>& std_script_bodies);
    
private:
    const CompiledGameData* game_data_ = nullptr;
    
    // Script context: script_id -> owning map
    std::unordered_map<std::string, enginemon::MapId> script_context_;
    
    // Current script being processed
    std::string current_script_id_;
    enginemon::MapId current_map_ = enginemon::MAP_NONE;
    
    // Reference extraction and validation
    void extract_and_validate_from_op(
        const enginemon::SemanticOp& op, 
        const std::string& script_id,
        size_t block_idx, size_t inst_idx,
        std::vector<ValidatedReference>& refs);
    
    ValidatedReference validate_reference(
        ReferenceType type, uint32_t value,
        const std::string& script_id, const std::string& op_name,
        size_t block_idx, size_t inst_idx,
        std::optional<uint16_t> secondary = std::nullopt);
};

} // namespace crystal
