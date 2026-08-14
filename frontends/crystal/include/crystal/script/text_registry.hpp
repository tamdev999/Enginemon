#pragma once
// crystal/script/text_registry.hpp
// Text extraction and semantic ID assignment
//
// This registry handles text extraction for:
// - Trainer win/loss text (winlosstext command)
// - Other non-script-root text that needs semantic TextId
//
// Architecture:
//   ROM text pointer → decode_text_sequence() → TextDefinition → register → TextId
//   Frontend-only: ROM address → TextId lookup
//   SemanticIR: carries only TextId (no ROM address)
//
// Text identity is based on semantic content (the actual text), not ROM address.
// Two identical text strings at different ROM addresses get the same TextId.

#include "engine/core/types.hpp"
#include "crystal/script/ir.hpp"
#include <unordered_map>
#include <vector>
#include <optional>
#include <string>
#include <functional>

namespace crystal {

// Forward declaration
class ScriptDecoder;

// Text content extracted from ROM
struct TextDefinition {
    enginemon::TextId id = enginemon::TEXT_NONE;
    TextSequence sequence;                          // Semantic text with LINE/CONT/PARA preserved
    uint32_t source_rom_address = 0;                // Debug only, NOT used for identity
    
    // Get flattened text for identity comparison (ignores control codes)
    std::string identity_string() const;
    
    // Identity is based on semantic content, not ROM address
    bool operator==(const TextDefinition& other) const {
        return identity_string() == other.identity_string();
    }
};

// Hash for deduplication by semantic content
struct TextContentHash {
    size_t operator()(const TextDefinition& def) const {
        return std::hash<std::string>{}(def.identity_string());
    }
};

// Registry of all extracted text definitions
// Provides ROM address → TextId lookup for frontend use only
class TextRegistry {
public:
    // Decoder function type for extracting text sequences
    using TextDecoder = std::function<TextSequence(uint32_t)>;
    
    // Constructor with text decoder function
    explicit TextRegistry(TextDecoder decoder);
    
    // Extract text at ROM address and register (or return existing if duplicate)
    // Returns assigned TextId (or TEXT_NONE on failure)
    enginemon::TextId extract(uint32_t rom_address);
    
    // Lookup TextId by ROM address (frontend use only)
    // Returns TEXT_NONE if not registered
    std::optional<enginemon::TextId> lookup(uint32_t rom_address) const;
    
    // Get definition by ID (for linker validation)
    const TextDefinition* get(enginemon::TextId id) const;
    
    // Check if ID exists (for linker validation)
    bool has(enginemon::TextId id) const;
    
    // Get all definitions (for debugging/testing)
    const std::vector<TextDefinition>& all_definitions() const { return definitions_; }
    
    // Statistics
    size_t count() const { return definitions_.size(); }
    size_t deduplicated_count() const { return dedup_count_; }

private:
    TextDecoder decoder_;
    
    // Sequential definitions (ID = index)
    std::vector<TextDefinition> definitions_;
    
    // ROM address → TextId lookup (frontend use only)
    std::unordered_map<uint32_t, enginemon::TextId> address_to_id_;
    
    // Content → TextId for deduplication
    std::unordered_map<TextDefinition, enginemon::TextId, TextContentHash> content_to_id_;
    
    size_t dedup_count_ = 0;
    
    // Internal: register or deduplicate definition
    enginemon::TextId register_definition(TextDefinition def);
};

} // namespace crystal
