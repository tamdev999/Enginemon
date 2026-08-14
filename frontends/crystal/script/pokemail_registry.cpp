// crystal/script/pokemail_registry.cpp
// PokeMail extraction and semantic ID assignment implementation

#include "crystal/script/pokemail_registry.hpp"
#include <iostream>

namespace crystal {

// Crystal mail constants (from pokecrystal/constants/item_data_constants.asm)
static constexpr size_t MAIL_MSG_LENGTH = 32;  // 2 lines × 16 chars

PokeMailRegistry::PokeMailRegistry(const RomData& rom)
    : rom_(rom)
{
}

std::string PokeMailRegistry::extract_message(uint32_t address) {
    std::string result;
    result.reserve(MAIL_MSG_LENGTH);
    
    for (size_t i = 0; i < MAIL_MSG_LENGTH && address + i < rom_.size(); ++i) {
        uint8_t ch = rom_.read_byte(address + i);
        
        // Check for terminator
        if (ch == 0x50 || ch == 0x00 || ch == '@') {  // Crystal text terminator
            break;
        }
        
        // Convert Crystal character to ASCII (simplified - just for identity)
        // Crystal uses custom encoding: 0x80-0x99 = A-Z, 0xA0-0xB9 = a-z, etc.
        if (ch >= 0x80 && ch <= 0x99) {
            result += static_cast<char>('A' + (ch - 0x80));
        } else if (ch >= 0xA0 && ch <= 0xB9) {
            result += static_cast<char>('a' + (ch - 0xA0));
        } else if (ch >= 0xF6 && ch <= 0xFF) {
            result += static_cast<char>('0' + (ch - 0xF6));
        } else if (ch == 0x7F) {  // space
            result += ' ';
        } else if (ch == 0x4F) {  // LINE
            result += '\n';
        } else if (ch == 0x4E) {  // NEXT  
            result += '\n';
        } else {
            // Keep raw byte for other characters
            result += static_cast<char>(ch);
        }
    }
    
    return result;
}

enginemon::PokeMailId PokeMailRegistry::register_definition(PokeMailDefinition def) {
    // Check for content-based deduplication
    auto dedup_it = content_to_id_.find(def);
    if (dedup_it != content_to_id_.end()) {
        // Already registered with same content - add address mapping
        address_to_id_[def.source_rom_address] = dedup_it->second;
        ++dedup_count_;
        return dedup_it->second;
    }
    
    // New unique content - assign sequential ID
    enginemon::PokeMailId new_id = static_cast<enginemon::PokeMailId>(definitions_.size());
    def.id = new_id;
    
    definitions_.push_back(def);
    address_to_id_[def.source_rom_address] = new_id;
    content_to_id_[def] = new_id;
    
    return new_id;
}

enginemon::PokeMailId PokeMailRegistry::extract_give_mail(uint32_t rom_address) {
    if (rom_address == 0 || rom_address >= rom_.size()) {
        return enginemon::POKEMAIL_NONE;
    }
    
    // Check if already extracted
    auto it = address_to_id_.find(rom_address);
    if (it != address_to_id_.end()) {
        return it->second;
    }
    
    // givepokemail format: db mail_type, db "message"... '@'
    PokeMailDefinition def;
    def.source_rom_address = rom_address;
    def.mail_type = static_cast<enginemon::ItemId>(rom_.read_byte(rom_address));
    def.message = extract_message(rom_address + 1);
    
    return register_definition(std::move(def));
}

enginemon::PokeMailId PokeMailRegistry::extract_check_mail(uint32_t rom_address) {
    if (rom_address == 0 || rom_address >= rom_.size()) {
        return enginemon::POKEMAIL_NONE;
    }
    
    // Check if already extracted
    auto it = address_to_id_.find(rom_address);
    if (it != address_to_id_.end()) {
        return it->second;
    }
    
    // checkpokemail format: db "message"... '@' (no type byte)
    PokeMailDefinition def;
    def.source_rom_address = rom_address;
    def.mail_type = enginemon::ITEM_NONE;  // checkpokemail doesn't have type
    def.message = extract_message(rom_address);
    
    return register_definition(std::move(def));
}

std::optional<enginemon::PokeMailId> PokeMailRegistry::lookup(uint32_t rom_address) const {
    auto it = address_to_id_.find(rom_address);
    if (it != address_to_id_.end()) {
        return it->second;
    }
    return std::nullopt;
}

const PokeMailDefinition* PokeMailRegistry::get(enginemon::PokeMailId id) const {
    if (id < definitions_.size()) {
        return &definitions_[id];
    }
    return nullptr;
}

bool PokeMailRegistry::has(enginemon::PokeMailId id) const {
    return id < definitions_.size();
}

} // namespace crystal
