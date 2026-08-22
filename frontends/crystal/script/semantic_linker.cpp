// crystal/script/semantic_linker.cpp
// Stage 6: Corpus-wide Typed-Reference Linker implementation

#include "crystal/script/semantic_linker.hpp"
#include "crystal/script/crystal_cfg.hpp"
#include <iostream>
#include <iomanip>
#include <sstream>

namespace crystal {

// =============================================================================
// LINKER DIAGNOSTIC
// =============================================================================

std::string LinkerDiagnostic::to_string() const {
    std::ostringstream ss;
    ss << "[" << validation_class_name(result) << "] "
       << reference_type_name(ref_type) << " " << ref_value
       << " in " << script_id << "::" << op_name
       << " (block " << block_index << ", inst " << instruction_index << ")"
       << " - " << reason;
    return ss.str();
}

// =============================================================================
// VALIDATION STATS
// =============================================================================

size_t ValidationStats::total_exact_resolved() const {
    size_t total = 0;
    for (const auto& [type, count] : exact_resolved) total += count;
    return total;
}

size_t ValidationStats::total_ownership_validated() const {
    size_t total = 0;
    for (const auto& [type, count] : ownership_validated) total += count;
    return total;
}

size_t ValidationStats::total_range_only() const {
    size_t total = 0;
    for (const auto& [type, count] : range_only) total += count;
    return total;
}

size_t ValidationStats::total_pending_definition() const {
    size_t total = 0;
    for (const auto& [type, count] : pending_definition) total += count;
    return total;
}

size_t ValidationStats::total_unresolved() const {
    size_t total = 0;
    for (const auto& [type, count] : unresolved) total += count;
    return total;
}

size_t ValidationStats::total_invalid_ownership() const {
    size_t total = 0;
    for (const auto& [type, count] : invalid_ownership) total += count;
    return total;
}

size_t ValidationStats::total_wrong_type() const {
    size_t total = 0;
    for (const auto& [type, count] : wrong_type) total += count;
    return total;
}

size_t ValidationStats::total_invalid_domain() const {
    size_t total = 0;
    for (const auto& [type, count] : invalid_domain) total += count;
    return total;
}

size_t ValidationStats::total_errors() const {
    return total_unresolved() + total_invalid_ownership() + total_wrong_type() + total_invalid_domain();
}

void ValidationStats::add(const ValidatedReference& ref) {
    unique_by_type[ref.type].insert(ref.value);
    
    switch (ref.validation) {
        case ValidationClass::ExactResolved:
            exact_resolved[ref.type]++;
            break;
        case ValidationClass::OwnershipValidated:
            ownership_validated[ref.type]++;
            break;
        case ValidationClass::RangeOnly:
            range_only[ref.type]++;
            break;
        case ValidationClass::PendingDefinition:
            pending_definition[ref.type]++;
            break;
        case ValidationClass::Unresolved:
            unresolved[ref.type]++;
            break;
        case ValidationClass::InvalidOwnership:
            invalid_ownership[ref.type]++;
            break;
        case ValidationClass::WrongType:
            wrong_type[ref.type]++;
            break;
        case ValidationClass::InvalidDomain:
            invalid_domain[ref.type]++;
            break;
    }
}

void ValidationStats::print_summary() const {
    std::cout << "\n=== Reference Validation by Type ===\n\n";
    
    std::cout << std::left << std::setw(14) << "Type"
              << std::right << std::setw(8) << "Exact"
              << std::setw(10) << "Ownership"
              << std::setw(10) << "RangeOnly"
              << std::setw(10) << "Pending"
              << std::setw(10) << "InvDomain"
              << std::setw(8) << "Unique"
              << "\n";
    std::cout << std::string(80, '-') << "\n";
    
    // Collect all types that have any references
    std::set<ReferenceType> all_types;
    for (const auto& [t, _] : exact_resolved) all_types.insert(t);
    for (const auto& [t, _] : ownership_validated) all_types.insert(t);
    for (const auto& [t, _] : range_only) all_types.insert(t);
    for (const auto& [t, _] : pending_definition) all_types.insert(t);
    for (const auto& [t, _] : unresolved) all_types.insert(t);
    for (const auto& [t, _] : invalid_ownership) all_types.insert(t);
    for (const auto& [t, _] : invalid_domain) all_types.insert(t);
    
    for (ReferenceType type : all_types) {
        auto get_count = [](const std::map<ReferenceType, size_t>& m, ReferenceType t) {
            auto it = m.find(t);
            return it != m.end() ? it->second : 0;
        };
        
        size_t exact = get_count(exact_resolved, type);
        size_t owner = get_count(ownership_validated, type);
        size_t range = get_count(range_only, type);
        size_t pending = get_count(pending_definition, type);
        size_t inv_domain = get_count(invalid_domain, type);
        
        auto unique_it = unique_by_type.find(type);
        size_t unique = unique_it != unique_by_type.end() ? unique_it->second.size() : 0;
        
        std::cout << std::left << std::setw(14) << reference_type_name(type)
                  << std::right << std::setw(8) << exact
                  << std::setw(10) << owner
                  << std::setw(10) << range
                  << std::setw(10) << pending
                  << std::setw(10) << inv_domain
                  << std::setw(8) << unique
                  << "\n";
    }
    
    std::cout << std::string(80, '-') << "\n";
    std::cout << std::left << std::setw(14) << "TOTAL"
              << std::right << std::setw(8) << total_exact_resolved()
              << std::setw(10) << total_ownership_validated()
              << std::setw(10) << total_range_only()
              << std::setw(10) << total_pending_definition()
              << std::setw(10) << total_invalid_domain()
              << "\n";
    
    if (total_errors() > 0) {
        std::cout << "\n=== Errors ===\n";
        std::cout << "Unresolved:       " << total_unresolved() << "\n";
        std::cout << "InvalidOwnership: " << total_invalid_ownership() << "\n";
        std::cout << "WrongType:        " << total_wrong_type() << "\n";
        std::cout << "InvalidDomain:    " << total_invalid_domain() << "\n";
    }
}

// =============================================================================
// COMPILED GAME DATA
// =============================================================================

void CompiledGameData::load_std_scripts(const StdScriptsTable& table) {
    std_scripts.clear();
    for (size_t i = 0; i < table.size(); ++i) {
        const auto* entry = table.get(static_cast<uint16_t>(i));
        if (entry && entry->flat_address != 0) {
            std_scripts[entry->std_id] = entry->flat_address;
        }
    }
}

bool CompiledGameData::has_trainer(uint8_t group, uint8_t id) const {
    return trainers.contains({group, id});
}

bool CompiledGameData::is_valid_object(enginemon::MapId map, uint8_t object_id) const {
    // Crystal object ID semantics (from pokecrystal/constants/map_object_constants.asm):
    //   PLAYER_OBJECT EQU 0  - Player is always object 0
    //   Map objects use IDs 2..N+1 (object_index + 2)
    //   LAST_TALKED EQU -2 = 0xFE - Reference to last interacted object
    //   Legacy LAST_TALKED = 0xFF - Backward compatibility
    
    // Object 0 = player (PLAYER_OBJECT EQU 0)
    if (object_id == 0) return true;
    
    // Object 0xFE = "last talked" (LAST_TALKED EQU -2)
    if (object_id == 0xFE) return true;
    
    // Object 0xFF = legacy "last talked" compatibility
    if (object_id == 0xFF) return true;
    
    // Object 1 is never valid - Crystal skips ID 1
    // Map objects start at ID 2 (first object = index 0, ID = index + 2 = 2)
    if (object_id == 1) return false;
    
    auto it = map_object_counts.find(map);
    if (it == map_object_counts.end()) {
        // Map not in compiled set - cannot validate ownership
        return false;
    }
    
    // Map objects are indexed 0..N-1 but use IDs 2..N+1
    // So if a map has N objects, valid object IDs are 2..N+1
    // Example: 3 objects → IDs 2, 3, 4 (indices 0, 1, 2)
    uint8_t object_count = it->second;
    return object_id >= 2 && object_id <= (object_count + 1);
}

bool CompiledGameData::is_valid_warp(enginemon::MapId map, uint8_t warp_id) const {
    auto it = map_warp_counts.find(map);
    if (it == map_warp_counts.end()) {
        return false;
    }
    return warp_id < it->second;
}

// =============================================================================
// LINKED CORPUS REPORT
// =============================================================================

void LinkedCorpus::print_report() const {
    std::cout << "\n=== Stage 6: Linker Report ===\n";
    std::cout << "Map-root bodies:     " << map_root_bodies << "\n";
    std::cout << "StdScript bodies:    " << std_script_bodies << "\n";
    std::cout << "Overlap:             " << overlap << "\n";
    std::cout << "Total unique bodies: " << total_bodies << "\n";
    std::cout << "Total blocks:        " << total_blocks << "\n";
    std::cout << "Total instructions:  " << total_instructions << "\n";
    
    stats.print_summary();
    
    std::cout << "\n=== Link Results ===\n";
    std::cout << "Scripts linked OK:   " << scripts_linked_ok << "\n";
    std::cout << "Scripts with errors: " << scripts_with_errors << "\n";
    std::cout << "Total errors:        " << stats.total_errors() << "\n";
    
    std::cout << "\n=== Validation Summary ===\n";
    std::cout << "ExactResolved:       " << stats.total_exact_resolved() << "\n";
    std::cout << "OwnershipValidated:  " << stats.total_ownership_validated() << "\n";
    std::cout << "RangeOnly:           " << stats.total_range_only() << "\n";
    std::cout << "Unresolved:          " << stats.total_unresolved() << "\n";
    std::cout << "InvalidOwnership:    " << stats.total_invalid_ownership() << "\n";
    std::cout << "WrongType:           " << stats.total_wrong_type() << "\n";
    
    if (!diagnostics.empty()) {
        std::cout << "\n=== Diagnostics (first 20) ===\n";
        size_t shown = 0;
        for (const auto& diag : diagnostics) {
            if (shown++ >= 20) break;
            std::cout << "  " << diag.to_string() << "\n";
        }
        if (diagnostics.size() > 20) {
            std::cout << "  ... and " << (diagnostics.size() - 20) << " more\n";
        }
    }
    
    if (all_linked()) {
        std::cout << "\n*** ALL " << total_bodies << " BODIES LINKED SUCCESSFULLY ***\n";
    } else {
        std::cout << "\n*** LINK FAILURES: " << stats.total_errors() << " errors ***\n";
    }
}

// =============================================================================
// SEMANTIC LINKER - CONTEXT
// =============================================================================

void SemanticLinker::set_script_context(const std::string& script_id, 
                                         enginemon::MapId owning_map) {
    script_context_[script_id] = owning_map;
}

// =============================================================================
// REFERENCE VALIDATION
// =============================================================================

ValidatedReference SemanticLinker::validate_reference(
    ReferenceType type, uint32_t value,
    const std::string& script_id, const std::string& op_name,
    size_t block_idx, size_t inst_idx,
    std::optional<uint16_t> secondary) {
    
    ValidatedReference ref;
    ref.type = type;
    ref.value = value;
    ref.script_id = script_id;
    ref.block_index = block_idx;
    ref.instruction_index = inst_idx;
    ref.op_name = op_name;
    ref.secondary_value = secondary;
    ref.owning_map = current_map_;
    
    if (!game_data_) {
        ref.validation = ValidationClass::Unresolved;
        ref.error_reason = "No compiled game data provided";
        return ref;
    }
    
    ValidationClass expected = expected_validation_class(type);
    
    switch (type) {
        // =================================================================
        // ExactResolved - Actual compiled artifacts exist
        // =================================================================
        
        case ReferenceType::Map:
            // Map references are ExactResolved against discovered/compiled map set
            if (value == enginemon::MAP_NONE) {
                ref.validation = ValidationClass::Unresolved;
                ref.error_reason = "MAP_NONE is not a valid target";
            } else if (game_data_->has_map(static_cast<enginemon::MapId>(value))) {
                ref.validation = ValidationClass::ExactResolved;
            } else {
                ref.validation = ValidationClass::InvalidDomain;
                ref.error_reason = "Map not in compiled map set";
            }
            break;
            
        case ReferenceType::ElevatorId:
            // ElevatorId is a semantic ID from compiled elevator floor data
            if (game_data_->has_elevator(static_cast<uint16_t>(value))) {
                ref.validation = ValidationClass::ExactResolved;
            } else {
                ref.validation = ValidationClass::InvalidDomain;
                ref.error_reason = "Elevator not in compiled registry";
            }
            break;
            
        case ReferenceType::Trainer:
            // Trainers are ExactResolved against TrainerRegistry (real ROM extraction)
            if (secondary.has_value()) {
                if (game_data_->has_trainer(static_cast<uint8_t>(value), 
                                            static_cast<uint8_t>(*secondary))) {
                    ref.validation = ValidationClass::ExactResolved;
                } else {
                    ref.validation = ValidationClass::InvalidDomain;
                    ref.error_reason = "Trainer (group,id) not in TrainerRegistry";
                }
            } else {
                ref.validation = ValidationClass::Unresolved;
                ref.error_reason = "Trainer missing trainer_id";
            }
            break;
            
        case ReferenceType::StdScript:
            // StdScript references must resolve against COMPILED bodies, not just ROM table
            if (game_data_->has_std_script(static_cast<uint16_t>(value))) {
                ref.validation = ValidationClass::ExactResolved;
            } else {
                ref.validation = ValidationClass::Unresolved;
                ref.error_reason = "StdScript not in compiled/legalized body set";
            }
            break;
            
        case ReferenceType::PokeMailId:
            // PokeMailId references must resolve against compiled PokeMail registry
            // POKEMAIL_NONE is a failure sentinel from extraction - InvalidDomain
            if (value == enginemon::POKEMAIL_NONE) {
                // POKEMAIL_NONE should never be in valid SemanticIR - extraction failure
                ref.validation = ValidationClass::InvalidDomain;
                ref.error_reason = "POKEMAIL_NONE sentinel leaked into SemanticIR (extraction failure)";
            } else if (game_data_->has_pokemail(static_cast<enginemon::PokeMailId>(value))) {
                ref.validation = ValidationClass::ExactResolved;
            } else {
                ref.validation = ValidationClass::InvalidDomain;
                ref.error_reason = "PokeMailId not in compiled PokeMail registry";
            }
            break;
            
        case ReferenceType::TextId:
            // TextId references must resolve against compiled Text registry
            // Absence is modeled as std::nullopt in Sem_SetWinLossText (no ref emitted)
            // TEXT_NONE reaching validation is a semantic leak - InvalidDomain
            if (value == enginemon::TEXT_NONE) {
                // TEXT_NONE should never be emitted as a ref - absence uses nullopt
                ref.validation = ValidationClass::InvalidDomain;
                ref.error_reason = "TEXT_NONE sentinel leaked into SemanticIR (use nullopt for absence)";
            } else if (game_data_->has_text(static_cast<enginemon::TextId>(value))) {
                ref.validation = ValidationClass::ExactResolved;
            } else {
                ref.validation = ValidationClass::InvalidDomain;
                ref.error_reason = "TextId not in compiled Text registry";
            }
            break;
            
        // =================================================================
        // PendingDefinition - Domain-valid ID, but no artifact producer yet
        // These use profile.counts for domain validation only
        // =================================================================
        
        case ReferenceType::Species:
            // Species domain: 0 = SPECIES_NONE sentinel, 1-251 valid
            if (value == 0) {
                ref.validation = ValidationClass::PendingDefinition;  // Sentinel
            } else if (game_data_->has_species(static_cast<enginemon::SpeciesId>(value))) {
                ref.validation = ValidationClass::PendingDefinition;
            } else {
                ref.validation = ValidationClass::InvalidDomain;
                ref.error_reason = "Species ID outside authoritative domain";
            }
            break;
            
        case ReferenceType::Item:
            // Item domain: 0-255, 0 = NO_ITEM sentinel
            if (game_data_->has_item(static_cast<enginemon::ItemId>(value))) {
                ref.validation = ValidationClass::PendingDefinition;
            } else {
                ref.validation = ValidationClass::InvalidDomain;
                ref.error_reason = "Item ID outside authoritative domain";
            }
            break;
            
        case ReferenceType::Music:
            // Music domain: 0-102, 0 = MUSIC_NONE
            if (game_data_->has_music(static_cast<enginemon::MusicId>(value))) {
                ref.validation = ValidationClass::PendingDefinition;
            } else {
                ref.validation = ValidationClass::InvalidDomain;
                ref.error_reason = "Music ID outside authoritative domain";
            }
            break;
            
        case ReferenceType::Sfx:
            // SFX domain: 0-206
            if (game_data_->has_sfx(static_cast<enginemon::SfxId>(value))) {
                ref.validation = ValidationClass::PendingDefinition;
            } else {
                ref.validation = ValidationClass::InvalidDomain;
                ref.error_reason = "SFX ID outside authoritative domain";
            }
            break;
            
        case ReferenceType::Special:
            // Special domain: 0-255 (indices into SpecialsPointers)
            // NOTE: Sem_Special is a pass-through that needs semantic classification
            if (game_data_->has_special(static_cast<uint16_t>(value))) {
                ref.validation = ValidationClass::PendingDefinition;
            } else {
                ref.validation = ValidationClass::InvalidDomain;
                ref.error_reason = "Special ID outside authoritative domain";
            }
            break;
            
        case ReferenceType::PhonePerson:
            // Phone contacts domain: 0-37
            if (game_data_->has_phone_person(static_cast<uint8_t>(value))) {
                ref.validation = ValidationClass::PendingDefinition;
            } else {
                ref.validation = ValidationClass::InvalidDomain;
                ref.error_reason = "Phone person ID outside authoritative domain";
            }
            break;
            
        case ReferenceType::TradeId:
            // Trades domain: 0-6
            if (game_data_->has_trade(static_cast<uint8_t>(value))) {
                ref.validation = ValidationClass::PendingDefinition;
            } else {
                ref.validation = ValidationClass::InvalidDomain;
                ref.error_reason = "Trade ID outside authoritative domain";
            }
            break;
            
        case ReferenceType::FruitTreeId:
            // Fruit trees domain: 1-30 (1-indexed)
            if (game_data_->has_fruit_tree(static_cast<uint8_t>(value))) {
                ref.validation = ValidationClass::PendingDefinition;
            } else {
                ref.validation = ValidationClass::InvalidDomain;
                ref.error_reason = "Fruit tree ID outside authoritative domain";
            }
            break;
            
        case ReferenceType::MartId:
            // Marts domain: 0-33
            if (game_data_->has_mart(static_cast<uint16_t>(value))) {
                ref.validation = ValidationClass::PendingDefinition;
            } else {
                ref.validation = ValidationClass::InvalidDomain;
                ref.error_reason = "Mart ID outside authoritative domain";
            }
            break;
            
        case ReferenceType::Emote:
            // Emotes domain: 0-11
            if (game_data_->has_emote(static_cast<uint8_t>(value))) {
                ref.validation = ValidationClass::PendingDefinition;
            } else {
                ref.validation = ValidationClass::InvalidDomain;
                ref.error_reason = "Emote ID outside authoritative domain";
            }
            break;
            
        // =================================================================
        // OwnershipValidated - requires scope context
        // =================================================================
        
        case ReferenceType::Object:
            // Object 0 = player (PLAYER EQU 0)
            // Object 0xFE (254) = last talked (LAST_TALKED EQU -2)
            // Object 0xFF = legacy "last talked" compatibility
            if (value == 0 || value == 0xFE || value == 0xFF) {
                ref.validation = ValidationClass::OwnershipValidated;
            } else if (current_map_ == enginemon::MAP_NONE) {
                // No map context - cannot validate ownership
                ref.validation = ValidationClass::InvalidOwnership;
                ref.error_reason = "No map context for object validation";
            } else if (game_data_->is_valid_object(current_map_, 
                                                    static_cast<uint8_t>(value))) {
                ref.validation = ValidationClass::OwnershipValidated;
            } else {
                ref.validation = ValidationClass::InvalidOwnership;
                ref.error_reason = "Object ID exceeds map object count";
            }
            break;
            
        case ReferenceType::WarpId:
            if (current_map_ == enginemon::MAP_NONE) {
                ref.validation = ValidationClass::InvalidOwnership;
                ref.error_reason = "No map context for warp validation";
            } else if (game_data_->is_valid_warp(current_map_, 
                                                  static_cast<uint8_t>(value))) {
                ref.validation = ValidationClass::OwnershipValidated;
            } else {
                ref.validation = ValidationClass::InvalidOwnership;
                ref.error_reason = "Warp ID exceeds map warp count";
            }
            break;
            
        case ReferenceType::Block:
            // Block/metatile IDs are indices into the current map's tileset.
            // True ownership validation would require:
            //   1. Map -> tileset association
            //   2. Tileset -> block count extraction
            // Stage 6 currently lacks tileset context for scripts.
            //
            // Crystal metatiles are 8-bit indices [0, 255].
            // All Crystal tilesets define 256 metatiles (the full 8-bit range).
            // Therefore, any uint8_t value is domain-valid.
            //
            // Classification: RangeOnly (semantic scalar domain)
            // NOT OwnershipValidated - we don't actually consult owning tileset
            if (value < 256) {
                ref.validation = ValidationClass::RangeOnly;
            } else {
                ref.validation = ValidationClass::InvalidDomain;
                ref.error_reason = "Block ID exceeds 8-bit metatile range";
            }
            break;
            
        // === RangeOnly types - semantic scalar domains ===
        // These are NOT resource references - they are domain values
        
        case ReferenceType::Flag: {
            // Flag values are encoded: (namespace << 16) | value
            // EventFlags (ns=0): 2048 flags (0-2047) - see pokecrystal/constants/event_flags.asm
            // EngineFlags (ns=1): 190 flags (0-189) - see pokecrystal/constants/engine_flags.asm
            uint8_t ns = static_cast<uint8_t>((value >> 16) & 0xFF);
            uint16_t flag_value = static_cast<uint16_t>(value & 0xFFFF);
            
            if (ns == 0) {  // EventFlag
                if (flag_value < 2048) {
                    ref.validation = ValidationClass::RangeOnly;
                } else {
                    ref.validation = ValidationClass::InvalidDomain;
                    ref.error_reason = "EventFlag " + std::to_string(flag_value) + " out of range (0-2047)";
                }
            } else if (ns == 1) {  // EngineFlag
                if (flag_value < 190) {
                    ref.validation = ValidationClass::RangeOnly;
                } else {
                    ref.validation = ValidationClass::InvalidDomain;
                    ref.error_reason = "EngineFlag " + std::to_string(flag_value) + " out of range (0-189)";
                }
            } else {
                ref.validation = ValidationClass::InvalidDomain;
                ref.error_reason = "Unknown flag namespace: " + std::to_string(ns);
            }
            break;
        }
            
        case ReferenceType::Var:
            // Script variables are a semantic domain
            ref.validation = ValidationClass::RangeOnly;
            break;
            
        case ReferenceType::StateVar:
            // Mini-game state indices are a semantic domain
            ref.validation = ValidationClass::RangeOnly;
            break;
            
        case ReferenceType::Scene:
            // Map scene numbers (0-255) are a semantic domain
            ref.validation = ValidationClass::RangeOnly;
            break;
            
        case ReferenceType::TimeFlags:
            // Time-of-day bitfield is a semantic domain
            ref.validation = ValidationClass::RangeOnly;
            break;
            
        // NOTE: Emote is handled in PendingDefinition section above
            
        case ReferenceType::Label:
            // Labels are CFG targets validated by Stage 2, not corpus linking
            // They are intra-script and already proven valid
            ref.validation = ValidationClass::RangeOnly;
            break;
    }
    
    return ref;
}

// =============================================================================
// REFERENCE EXTRACTION FROM SEMANTIC OP
// =============================================================================

void SemanticLinker::extract_and_validate_from_op(
    const enginemon::SemanticOp& op, 
    const std::string& script_id,
    size_t block_idx, size_t inst_idx,
    std::vector<ValidatedReference>& refs) {
    
    using namespace enginemon;
    
    auto add_ref = [&](ReferenceType type, uint32_t value, const std::string& op_name) {
        refs.push_back(validate_reference(type, value, script_id, op_name,
                                          block_idx, inst_idx));
    };
    
    // Overload for FlagRef - encodes namespace in high bits
    auto add_flag_ref = [&](const FlagRef& flag, const std::string& op_name) {
        // Encode namespace in bits 16-17, value in bits 0-15
        uint32_t encoded = (static_cast<uint32_t>(flag.ns) << 16) | flag.value;
        refs.push_back(validate_reference(ReferenceType::Flag, encoded, script_id, op_name,
                                          block_idx, inst_idx));
    };
    
    auto add_ref_with_secondary = [&](ReferenceType type, uint32_t value, 
                                       uint16_t secondary, const std::string& op_name) {
        refs.push_back(validate_reference(type, value, script_id, op_name,
                                          block_idx, inst_idx, secondary));
    };
    
    std::visit([&](const auto& sem_op) {
        using T = std::decay_t<decltype(sem_op)>;
        
        // === Species References ===
        if constexpr (std::is_same_v<T, Sem_LoadWildMon>) {
            add_ref(ReferenceType::Species, sem_op.species, "LoadWildMon");
        } else if constexpr (std::is_same_v<T, Sem_GivePokemon>) {
            add_ref(ReferenceType::Species, sem_op.species, "GivePokemon");
            if (sem_op.held_item != ITEM_NONE) {
                add_ref(ReferenceType::Item, sem_op.held_item, "GivePokemon.held_item");
            }
        } else if constexpr (std::is_same_v<T, Sem_GiveEgg>) {
            add_ref(ReferenceType::Species, sem_op.species, "GiveEgg");
        } else if constexpr (std::is_same_v<T, Sem_CheckPokemon>) {
            add_ref(ReferenceType::Species, sem_op.species, "CheckPokemon");
        } else if constexpr (std::is_same_v<T, Sem_PlayCry>) {
            // Only emit a species reference for literal species; ScriptVar source has no static ID
            if (sem_op.source.is_literal()) {
                add_ref(ReferenceType::Species, sem_op.source.species, "PlayCry");
            }
            // ScriptVar: species unknown at compile time — no reference to validate
        } else if constexpr (std::is_same_v<T, Sem_Pokepic>) {
            // Only emit a species reference for literal species; ScriptVar source has no static ID
            if (sem_op.source.is_literal()) {
                add_ref(ReferenceType::Species, sem_op.source.species, "Pokepic");
            }
            // ScriptVar: species unknown at compile time — no reference to validate
        }
        
        // === Item References ===
        else if constexpr (std::is_same_v<T, Sem_GiveItem>) {
            add_ref(ReferenceType::Item, sem_op.item, "GiveItem");
        } else if constexpr (std::is_same_v<T, Sem_TakeItem>) {
            add_ref(ReferenceType::Item, sem_op.item, "TakeItem");
        } else if constexpr (std::is_same_v<T, Sem_CheckItem>) {
            add_ref(ReferenceType::Item, sem_op.item, "CheckItem");
        } else if constexpr (std::is_same_v<T, Sem_GiveItemVerbose>) {
            add_ref(ReferenceType::Item, sem_op.item, "GiveItemVerbose");
        }
        
        // === Map References ===
        else if constexpr (std::is_same_v<T, Sem_Warp>) {
            add_ref(ReferenceType::Map, sem_op.map, "Warp");
        } else if constexpr (std::is_same_v<T, Sem_WarpFacing>) {
            add_ref(ReferenceType::Map, sem_op.map, "WarpFacing");
        } else if constexpr (std::is_same_v<T, Sem_SetMapScene>) {
            add_ref(ReferenceType::Map, sem_op.map, "SetMapScene");
            add_ref(ReferenceType::Scene, sem_op.scene, "SetMapScene.scene");
        } else if constexpr (std::is_same_v<T, Sem_CheckMapScene>) {
            add_ref(ReferenceType::Map, sem_op.map, "CheckMapScene");
        } else if constexpr (std::is_same_v<T, Sem_ModifyWarp>) {
            add_ref(ReferenceType::WarpId, sem_op.warp_id, "ModifyWarp");
            add_ref(ReferenceType::Map, sem_op.target_map, "ModifyWarp.target");
        } else if constexpr (std::is_same_v<T, Sem_SetBlackoutPoint>) {
            add_ref(ReferenceType::Map, sem_op.map, "SetBlackoutPoint");
        }
        
        // === Flag References ===
        else if constexpr (std::is_same_v<T, Sem_SetFlag>) {
            add_flag_ref(sem_op.flag, "SetFlag");
        } else if constexpr (std::is_same_v<T, Sem_ClearFlag>) {
            add_flag_ref(sem_op.flag, "ClearFlag");
        } else if constexpr (std::is_same_v<T, Sem_CheckFlag>) {
            add_flag_ref(sem_op.flag, "CheckFlag");
        }
        
        // === Variable References ===
        else if constexpr (std::is_same_v<T, Sem_SetVar>) {
            add_ref(ReferenceType::Var, sem_op.var, "SetVar");
        } else if constexpr (std::is_same_v<T, Sem_AddVar>) {
            add_ref(ReferenceType::Var, sem_op.var, "AddVar");
        } else if constexpr (std::is_same_v<T, Sem_CheckVar>) {
            add_ref(ReferenceType::Var, sem_op.var, "CheckVar");
        }
        
        // === State Variable References ===
        else if constexpr (std::is_same_v<T, Sem_ReadStateVar>) {
            add_ref(ReferenceType::StateVar, sem_op.state_var, "ReadStateVar");
        } else if constexpr (std::is_same_v<T, Sem_WriteStateVar>) {
            add_ref(ReferenceType::StateVar, sem_op.state_var, "WriteStateVar");
        } else if constexpr (std::is_same_v<T, Sem_SetStateVar>) {
            add_ref(ReferenceType::StateVar, sem_op.state_var, "SetStateVar");
        }
        
        // === Music/Audio References ===
        else if constexpr (std::is_same_v<T, Sem_PlayMusic>) {
            add_ref(ReferenceType::Music, sem_op.music, "PlayMusic");
        } else if constexpr (std::is_same_v<T, Sem_FadeOutMusic>) {
            add_ref(ReferenceType::Music, sem_op.music, "FadeOutMusic");
        } else if constexpr (std::is_same_v<T, Sem_PlaySound>) {
            add_ref(ReferenceType::Sfx, sem_op.sound, "PlaySound");
        }
        
        // === Trainer References ===
        else if constexpr (std::is_same_v<T, Sem_LoadTrainer>) {
            add_ref_with_secondary(ReferenceType::Trainer, sem_op.trainer_group, 
                                   sem_op.trainer_id, "LoadTrainer");
        }
        
        // === Object References (map-local) ===
        else if constexpr (std::is_same_v<T, Sem_ApplyMovement>) {
            // Only add object reference if target is an Object (not Player or LastTalked)
            if (sem_op.target.type == enginemon::MovementTargetType::Object) {
                add_ref(ReferenceType::Object, sem_op.target.object_id, "ApplyMovement");
            }
            // Player (0) and LastTalked don't need object ID validation
        } else if constexpr (std::is_same_v<T, Sem_FacePlayer>) {
            add_ref(ReferenceType::Object, sem_op.object_id, "FacePlayer");
        } else if constexpr (std::is_same_v<T, Sem_FaceObject>) {
            add_ref(ReferenceType::Object, sem_op.object1, "FaceObject.obj1");
            add_ref(ReferenceType::Object, sem_op.object2, "FaceObject.obj2");
        } else if constexpr (std::is_same_v<T, Sem_TurnObject>) {
            add_ref(ReferenceType::Object, sem_op.object_id, "TurnObject");
        } else if constexpr (std::is_same_v<T, Sem_ShowObject>) {
            add_ref(ReferenceType::Object, sem_op.object_id, "ShowObject");
        } else if constexpr (std::is_same_v<T, Sem_HideObject>) {
            add_ref(ReferenceType::Object, sem_op.object_id, "HideObject");
        } else if constexpr (std::is_same_v<T, Sem_MoveObject>) {
            add_ref(ReferenceType::Object, sem_op.object_id, "MoveObject");
        } else if constexpr (std::is_same_v<T, Sem_SetLastTalked>) {
            add_ref(ReferenceType::Object, sem_op.object_id, "SetLastTalked");
        } else if constexpr (std::is_same_v<T, Sem_Follow>) {
            add_ref(ReferenceType::Object, sem_op.object1, "Follow.obj1");
            add_ref(ReferenceType::Object, sem_op.object2, "Follow.obj2");
        }
        
        // === Emote References ===
        else if constexpr (std::is_same_v<T, Sem_Emote>) {
            add_ref(ReferenceType::Emote, sem_op.emote_id, "Emote");
            add_ref(ReferenceType::Object, sem_op.object_id, "Emote.object");
        }
        
        // === Scene References ===
        else if constexpr (std::is_same_v<T, Sem_SetScene>) {
            add_ref(ReferenceType::Scene, sem_op.scene, "SetScene");
        }
        
        // === Label References (CFG targets - validated by Stage 2) ===
        else if constexpr (std::is_same_v<T, Sem_Jump>) {
            add_ref(ReferenceType::Label, sem_op.target.id, "Jump");
        } else if constexpr (std::is_same_v<T, Sem_JumpIf>) {
            add_ref(ReferenceType::Label, sem_op.target.id, "JumpIf");
        } else if constexpr (std::is_same_v<T, Sem_Call>) {
            add_ref(ReferenceType::Label, sem_op.target.id, "Call");
        }
        
        // === StdScript References ===
        else if constexpr (std::is_same_v<T, Sem_CallStd>) {
            add_ref(ReferenceType::StdScript, sem_op.std_id, "CallStd");
        } else if constexpr (std::is_same_v<T, Sem_JumpStd>) {
            add_ref(ReferenceType::StdScript, sem_op.std_id, "JumpStd");
        }
        
        // === Special References ===
        else if constexpr (std::is_same_v<T, Sem_Special>) {
            add_ref(ReferenceType::Special, sem_op.special_id, "Special");
        }
        
        // === Phone References ===
        else if constexpr (std::is_same_v<T, Sem_AddPhoneNumber>) {
            add_ref(ReferenceType::PhonePerson, sem_op.person, "AddPhoneNumber");
        } else if constexpr (std::is_same_v<T, Sem_DeletePhoneNumber>) {
            add_ref(ReferenceType::PhonePerson, sem_op.person, "DeletePhoneNumber");
        } else if constexpr (std::is_same_v<T, Sem_CheckPhoneNumber>) {
            add_ref(ReferenceType::PhonePerson, sem_op.person, "CheckPhoneNumber");
        }
        
        // === Time References ===
        else if constexpr (std::is_same_v<T, Sem_CheckTime>) {
            add_ref(ReferenceType::TimeFlags, sem_op.time_flags, "CheckTime");
        }
        
        // === Commerce References ===
        else if constexpr (std::is_same_v<T, Sem_Pokemart>) {
            add_ref(ReferenceType::MartId, sem_op.mart_id, "Pokemart");
        } else if constexpr (std::is_same_v<T, Sem_Elevator>) {
            add_ref(ReferenceType::ElevatorId, sem_op.elevator_id, "Elevator");
        } else if constexpr (std::is_same_v<T, Sem_Trade>) {
            add_ref(ReferenceType::TradeId, sem_op.trade_id, "Trade");
        } else if constexpr (std::is_same_v<T, Sem_FruitTree>) {
            add_ref(ReferenceType::FruitTreeId, sem_op.tree_id, "FruitTree");
        }
        
        // === Block Change References ===
        else if constexpr (std::is_same_v<T, Sem_ChangeBlock>) {
            add_ref(ReferenceType::Block, sem_op.block, "ChangeBlock");
        }
        
        // === PokeMail References ===
        else if constexpr (std::is_same_v<T, Sem_GivePokeMail>) {
            add_ref(ReferenceType::PokeMailId, sem_op.mail_id, "GivePokeMail");
        } else if constexpr (std::is_same_v<T, Sem_CheckPokeMail>) {
            add_ref(ReferenceType::PokeMailId, sem_op.mail_id, "CheckPokeMail");
        }
        
        // === Win/Loss Text References ===
        // Only emit references for present TextIds (std::optional has value)
        // Absence (nullopt) does NOT emit a reference - it's not a resource
        else if constexpr (std::is_same_v<T, Sem_SetWinLossText>) {
            if (sem_op.win_text.has_value()) {
                add_ref(ReferenceType::TextId, sem_op.win_text.value(), "SetWinLossText.win");
            }
            if (sem_op.loss_text.has_value()) {
                add_ref(ReferenceType::TextId, sem_op.loss_text.value(), "SetWinLossText.loss");
            }
        }
        
        // === Text argument references ===
        // Note: TrainerName text args are NOT validated here because they
        // don't contain full (group, id) trainer references - they use a
        // different encoding for display name lookup
        else if constexpr (std::is_same_v<T, Sem_ShowText>) {
            for (const auto& arg : sem_op.sequence.args) {
                if (arg.type == TextArgType::ItemName) {
                    add_ref(ReferenceType::Item, arg.id, "ShowText.ItemArg");
                } else if (arg.type == TextArgType::PokemonName) {
                    add_ref(ReferenceType::Species, arg.id, "ShowText.PokemonArg");
                }
                // TrainerName text args skipped - different encoding
            }
        } else if constexpr (std::is_same_v<T, Sem_ShowTextAndEnd>) {
            for (const auto& arg : sem_op.sequence.args) {
                if (arg.type == TextArgType::ItemName) {
                    add_ref(ReferenceType::Item, arg.id, "ShowTextAndEnd.ItemArg");
                } else if (arg.type == TextArgType::PokemonName) {
                    add_ref(ReferenceType::Species, arg.id, "ShowTextAndEnd.PokemonArg");
                }
                // TrainerName text args skipped - different encoding
            }
        } else if constexpr (std::is_same_v<T, Sem_FacePlayerAndShowText>) {
            for (const auto& arg : sem_op.sequence.args) {
                if (arg.type == TextArgType::ItemName) {
                    add_ref(ReferenceType::Item, arg.id, "FacePlayerShowText.ItemArg");
                } else if (arg.type == TextArgType::PokemonName) {
                    add_ref(ReferenceType::Species, arg.id, "FacePlayerShowText.PokemonArg");
                }
                // TrainerName text args skipped - different encoding
            }
        }
        
        // === Text argument preparation ===
        // Text arg ops have complete operand preservation (Finding 3 fix)
        // - id: ItemId, SpeciesId, LandmarkId depending on arg_type
        // - trainer_group + id2: for trainer_name
        // - str_value: resolved text content for getstring
        // - account: money source (player=0, mom=1, coins=2, var=3)
        else if constexpr (std::is_same_v<T, Sem_PrepareTextArg>) {
            if (sem_op.arg_type == TextArgType::ItemName) {
                add_ref(ReferenceType::Item, sem_op.id, "PrepareTextArg.Item");
            } else if (sem_op.arg_type == TextArgType::PokemonName) {
                add_ref(ReferenceType::Species, sem_op.id, "PrepareTextArg.Pokemon");
            }
            // Note: Number args (money/coins/var) don't reference game data IDs
            // TrainerName uses trainer_group + id2 - not a simple trainer ID reference
            // Landmark uses id with NameSourceType::Location - landmark table reference
        }
        
    }, op);
}

// =============================================================================
// LINK SCRIPT
// =============================================================================

std::vector<ValidatedReference> SemanticLinker::link_script(
    const enginemon::SemanticScriptIR& ir) {
    
    current_script_id_ = ir.script_id;
    
    // Get map context for this script
    auto ctx_it = script_context_.find(ir.script_id);
    if (ctx_it != script_context_.end()) {
        current_map_ = ctx_it->second;
    } else {
        current_map_ = enginemon::MAP_NONE;
    }
    
    std::vector<ValidatedReference> refs;
    
    for (size_t block_idx = 0; block_idx < ir.blocks.size(); ++block_idx) {
        const auto& block = ir.blocks[block_idx];
        for (size_t inst_idx = 0; inst_idx < block.instructions.size(); ++inst_idx) {
            extract_and_validate_from_op(block.instructions[inst_idx].op,
                                         ir.script_id, block_idx, inst_idx, refs);
        }
    }
    
    return refs;
}

// =============================================================================
// LINK FULL CORPUS (1297 map roots + 52 StdScripts)
// =============================================================================

LinkedCorpus SemanticLinker::link_full_corpus(
    const std::vector<enginemon::SemanticScriptIR>& map_root_scripts,
    const std::vector<enginemon::SemanticScriptIR>& std_script_bodies) {
    
    LinkedCorpus result;
    result.map_root_bodies = map_root_scripts.size();
    result.std_script_bodies = std_script_bodies.size();
    
    // Calculate overlap (scripts that appear in both sets)
    std::set<std::string> map_root_ids;
    for (const auto& ir : map_root_scripts) {
        map_root_ids.insert(ir.script_id);
    }
    
    size_t overlap_count = 0;
    for (const auto& ir : std_script_bodies) {
        if (map_root_ids.contains(ir.script_id)) {
            overlap_count++;
        }
    }
    result.overlap = overlap_count;
    result.total_bodies = result.map_root_bodies + result.std_script_bodies - result.overlap;
    
    // Link all map-root scripts
    for (const auto& ir : map_root_scripts) {
        result.total_blocks += ir.blocks.size();
        for (const auto& block : ir.blocks) {
            result.total_instructions += block.instructions.size();
        }
        
        auto refs = link_script(ir);
        bool has_error = false;
        
        for (const auto& ref : refs) {
            result.stats.add(ref);
            
            if (ref.validation == ValidationClass::Unresolved ||
                ref.validation == ValidationClass::InvalidOwnership ||
                ref.validation == ValidationClass::WrongType ||
                ref.validation == ValidationClass::InvalidDomain) {
                has_error = true;
                
                LinkerDiagnostic diag;
                diag.result = ref.validation;
                diag.ref_type = ref.type;
                diag.ref_value = ref.value;
                diag.script_id = ref.script_id;
                diag.op_name = ref.op_name;
                diag.reason = ref.error_reason;
                diag.block_index = ref.block_index;
                diag.instruction_index = ref.instruction_index;
                result.diagnostics.push_back(diag);
            }
        }
        
        if (has_error) {
            result.scripts_with_errors++;
            result.script_link_status[ir.script_id] = false;
        } else {
            result.scripts_linked_ok++;
            result.script_link_status[ir.script_id] = true;
        }
    }
    
    // Link StdScript bodies (skip duplicates)
    for (const auto& ir : std_script_bodies) {
        // Skip if already processed as map root
        if (map_root_ids.contains(ir.script_id)) {
            continue;
        }
        
        result.total_blocks += ir.blocks.size();
        for (const auto& block : ir.blocks) {
            result.total_instructions += block.instructions.size();
        }
        
        auto refs = link_script(ir);
        bool has_error = false;
        
        for (const auto& ref : refs) {
            result.stats.add(ref);
            
            if (ref.validation == ValidationClass::Unresolved ||
                ref.validation == ValidationClass::InvalidOwnership ||
                ref.validation == ValidationClass::WrongType ||
                ref.validation == ValidationClass::InvalidDomain) {
                has_error = true;
                
                LinkerDiagnostic diag;
                diag.result = ref.validation;
                diag.ref_type = ref.type;
                diag.ref_value = ref.value;
                diag.script_id = ref.script_id;
                diag.op_name = ref.op_name;
                diag.reason = ref.error_reason;
                diag.block_index = ref.block_index;
                diag.instruction_index = ref.instruction_index;
                result.diagnostics.push_back(diag);
            }
        }
        
        if (has_error) {
            result.scripts_with_errors++;
            result.script_link_status[ir.script_id] = false;
        } else {
            result.scripts_linked_ok++;
            result.script_link_status[ir.script_id] = true;
        }
    }
    
    return result;
}

} // namespace crystal
