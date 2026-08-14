#pragma once
// crystal/script/pokemail_registry.hpp
// PokeMail extraction and semantic ID assignment
//
// Crystal PokeMail structure (from pokecrystal):
//   givepokemail: db mail_type, db "message"... '@'
//   checkpokemail: db "message"... '@'
//
// The mail pointer in ROM points to this data. We extract the semantic content
// (type + message) and assign a stable PokeMailId based on content deduplication.
//
// Architecture:
//   ROM pointer → extract_mail() → PokeMailDefinition → register → PokeMailId
//   Frontend-only: ROM address → PokeMailId lookup
//   SemanticIR: carries only PokeMailId (no ROM address)

#include "engine/core/types.hpp"
#include "crystal/rom/loader.hpp"
#include <unordered_map>
#include <vector>
#include <optional>
#include <string>

namespace crystal {

// PokeMail content extracted from ROM
struct PokeMailDefinition {
    enginemon::PokeMailId id = enginemon::POKEMAIL_NONE;
    enginemon::ItemId mail_type = enginemon::ITEM_NONE;  // FLOWER_MAIL, etc.
    std::string message;                                   // Up to 32 chars + terminator
    uint32_t source_rom_address = 0;                       // Debug only, NOT used for identity
    
    // Identity is based on semantic content, not ROM address
    bool operator==(const PokeMailDefinition& other) const {
        return mail_type == other.mail_type && message == other.message;
    }
};

// Hash for deduplication by semantic content
struct PokeMailContentHash {
    size_t operator()(const PokeMailDefinition& def) const {
        size_t h = std::hash<uint16_t>{}(def.mail_type);
        h ^= std::hash<std::string>{}(def.message) + 0x9e3779b9 + (h << 6) + (h >> 2);
        return h;
    }
};

// Registry of all extracted Pokemon mail definitions
// Provides ROM address → PokeMailId lookup for frontend use only
class PokeMailRegistry {
public:
    explicit PokeMailRegistry(const RomData& rom);
    
    // Extract mail at ROM address and register (or return existing if duplicate)
    // For givepokemail: address points to mail_type + message
    // Returns assigned PokeMailId (or POKEMAIL_NONE on failure)
    enginemon::PokeMailId extract_give_mail(uint32_t rom_address);
    
    // Extract mail message for checkpokemail (message only, no type byte)
    // Returns assigned PokeMailId (or POKEMAIL_NONE on failure)
    enginemon::PokeMailId extract_check_mail(uint32_t rom_address);
    
    // Lookup PokeMailId by ROM address (frontend use only)
    // Returns POKEMAIL_NONE if not registered
    std::optional<enginemon::PokeMailId> lookup(uint32_t rom_address) const;
    
    // Get definition by ID (for linker validation)
    const PokeMailDefinition* get(enginemon::PokeMailId id) const;
    
    // Check if ID exists (for linker validation)
    bool has(enginemon::PokeMailId id) const;
    
    // Get all definitions (for debugging/testing)
    const std::vector<PokeMailDefinition>& all_definitions() const { return definitions_; }
    
    // Statistics
    size_t count() const { return definitions_.size(); }
    size_t deduplicated_count() const { return dedup_count_; }

private:
    const RomData& rom_;
    
    // Sequential definitions (ID = index)
    std::vector<PokeMailDefinition> definitions_;
    
    // ROM address → PokeMailId lookup (frontend use only)
    std::unordered_map<uint32_t, enginemon::PokeMailId> address_to_id_;
    
    // Content → PokeMailId for deduplication
    std::unordered_map<PokeMailDefinition, enginemon::PokeMailId, PokeMailContentHash> content_to_id_;
    
    size_t dedup_count_ = 0;
    
    // Internal: extract message text from ROM address
    std::string extract_message(uint32_t address);
    
    // Internal: register or deduplicate definition
    enginemon::PokeMailId register_definition(PokeMailDefinition def);
};

} // namespace crystal
