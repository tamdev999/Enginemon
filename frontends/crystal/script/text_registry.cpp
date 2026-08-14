// crystal/script/text_registry.cpp
// Text extraction and semantic ID assignment implementation

#include "crystal/script/text_registry.hpp"
#include <sstream>

namespace crystal {

std::string TextDefinition::identity_string() const {
    // Flatten text elements to a canonical string for identity comparison
    std::ostringstream ss;
    for (const auto& elem : sequence.elements) {
        switch (elem.op) {
            case TextOp::Text:
                ss << elem.text;
                break;
            case TextOp::Line:
            case TextOp::Next:
            case TextOp::Para:
            case TextOp::Cont:
            case TextOp::Scroll:
                ss << '\n';
                break;
            case TextOp::Done:
            case TextOp::Prompt:
                // Terminal codes don't contribute to identity
                break;
        }
    }
    return ss.str();
}

TextRegistry::TextRegistry(TextDecoder decoder)
    : decoder_(std::move(decoder))
{
}

enginemon::TextId TextRegistry::register_definition(TextDefinition def) {
    // Check for content-based deduplication
    auto dedup_it = content_to_id_.find(def);
    if (dedup_it != content_to_id_.end()) {
        // Already registered with same content - add address mapping
        address_to_id_[def.source_rom_address] = dedup_it->second;
        ++dedup_count_;
        return dedup_it->second;
    }
    
    // New unique content - assign sequential ID
    enginemon::TextId new_id = static_cast<enginemon::TextId>(definitions_.size());
    def.id = new_id;
    
    definitions_.push_back(def);
    address_to_id_[def.source_rom_address] = new_id;
    content_to_id_[def] = new_id;
    
    return new_id;
}

enginemon::TextId TextRegistry::extract(uint32_t rom_address) {
    if (rom_address == 0) {
        return enginemon::TEXT_NONE;
    }
    
    // Check if already extracted
    auto it = address_to_id_.find(rom_address);
    if (it != address_to_id_.end()) {
        return it->second;
    }
    
    // Extract text using provided decoder
    TextDefinition def;
    def.source_rom_address = rom_address;
    def.sequence = decoder_(rom_address);
    
    // Empty sequence = failed extraction
    if (def.sequence.empty()) {
        return enginemon::TEXT_NONE;
    }
    
    return register_definition(std::move(def));
}

std::optional<enginemon::TextId> TextRegistry::lookup(uint32_t rom_address) const {
    auto it = address_to_id_.find(rom_address);
    if (it != address_to_id_.end()) {
        return it->second;
    }
    return std::nullopt;
}

const TextDefinition* TextRegistry::get(enginemon::TextId id) const {
    if (id < definitions_.size()) {
        return &definitions_[id];
    }
    return nullptr;
}

bool TextRegistry::has(enginemon::TextId id) const {
    return id < definitions_.size();
}

} // namespace crystal
