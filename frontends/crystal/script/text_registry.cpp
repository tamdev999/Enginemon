// crystal/script/text_registry.cpp
// Text extraction and semantic ID assignment implementation

#include "crystal/script/text_registry.hpp"
#include <sstream>
#include <iomanip>

namespace crystal {

std::string TextDefinition::identity_string() const {
    // Canonical structural identity from typed text operation sequence
    // CRITICAL: Must distinguish all source-semantic differences:
    // - Different control codes (Line, Next, Para, Cont, Scroll, Done, Prompt)
    // - Different TX_RAM/TX_DECIMAL/TX_BCD addresses
    // - Different TX_STRINGBUFFER IDs
    // - Different TX_FAR pointers
    // - Different TX_BOX dimensions
    //
    // DO NOT normalize semantically distinct controls to '\n'
    
    std::ostringstream ss;
    for (const auto& elem : sequence.elements) {
        switch (elem.op) {
            case TextOp::Text:
                ss << "T[" << elem.text << "]";
                break;
            case TextOp::Line:
                ss << "<LINE>";
                break;
            case TextOp::Next:
                ss << "<NEXT>";
                break;
            case TextOp::Para:
                ss << "<PARA>";
                break;
            case TextOp::Cont:
                ss << "<CONT>";
                break;
            case TextOp::Scroll:
                ss << "<SCROLL>";
                break;
            case TextOp::Done:
                ss << "<DONE>";
                break;
            case TextOp::Prompt:
                ss << "<PROMPT>";
                break;
            case TextOp::TextRam:
                ss << "<RAM:" << std::hex << elem.addr << ">";
                break;
            case TextOp::TextBcd:
                ss << "<BCD:" << std::hex << elem.addr << "," << (int)elem.param1 << ">";
                break;
            case TextOp::TextDecimal:
                ss << "<DEC:" << std::hex << elem.addr << "," << (int)elem.param1 << ">";
                break;
            case TextOp::TextStringBuffer:
                ss << "<BUF:" << (int)elem.param1 << ">";
                break;
            case TextOp::TextFar:
                ss << "<FAR:" << std::hex << elem.addr << "," << (int)elem.param1 << ">";
                break;
            case TextOp::TextBox:
                ss << "<BOX:" << std::hex << elem.addr << "," 
                   << (int)elem.param1 << "x" << (int)elem.param2 << ">";
                break;
            case TextOp::TextMove:
                ss << "<MOVE:" << std::hex << elem.addr << ">";
                break;
            case TextOp::TextLow:
                ss << "<LOW>";
                break;
            case TextOp::TextPause:
                ss << "<PAUSE>";
                break;
            case TextOp::TextPromptButton:
                ss << "<WAITBTN>";
                break;
            case TextOp::TextDay:
                ss << "<DAY>";
                break;
            case TextOp::TextAsm:
                ss << "<ASM>";
                break;
            case TextOp::TextSoundItem:
                ss << "<SND_ITEM>";
                break;
            case TextOp::TextSoundCaught:
                ss << "<SND_CAUGHT>";
                break;
            case TextOp::TextSoundFanfare:
                ss << "<SND_FANFARE>";
                break;
            case TextOp::TextRaw:
                ss << "<RAW:" << elem.raw_bytes.size() << ">";
                break;
            default:
                ss << "<UNK>";
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
