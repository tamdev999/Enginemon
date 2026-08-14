// crystal/script/trainer_registry.cpp
// Trainer registry implementation - extracts real (group, id) pairs from ROM

#include "crystal/script/trainer_registry.hpp"
#include <iostream>
#include <iomanip>
#include <stdexcept>

namespace crystal {

// TRAINERTYPE_* constants from pokecrystal/constants/trainer_data_constants.asm
// These determine how much data follows the name for each Pokemon in the party
constexpr uint8_t TRAINERTYPE_NORMAL = 0;      // db level, species
constexpr uint8_t TRAINERTYPE_MOVES = 1;       // db level, species, 4 moves
constexpr uint8_t TRAINERTYPE_ITEM = 2;        // db level, species, item
constexpr uint8_t TRAINERTYPE_ITEM_MOVES = 3;  // db level, species, item, 4 moves

// Bytes per Pokemon entry by trainer type
constexpr size_t pokemon_entry_size(uint8_t type) {
    switch (type) {
        case TRAINERTYPE_NORMAL:     return 2;  // level, species
        case TRAINERTYPE_MOVES:      return 6;  // level, species, move×4
        case TRAINERTYPE_ITEM:       return 3;  // level, species, item
        case TRAINERTYPE_ITEM_MOVES: return 7;  // level, species, item, move×4
        default: return 2;  // Fallback to NORMAL
    }
}

TrainerRegistry::TrainerRegistry(const RomData& rom, uint32_t trainer_groups_offset, uint8_t num_classes)
    : rom_(&rom) 
{
    initialize(trainer_groups_offset, num_classes);
}

bool TrainerRegistry::initialize(uint32_t trainer_groups_offset, uint8_t num_classes) {
    if (!rom_) {
        return false;
    }
    
    clear();
    
    // TrainerGroups is a table of 2-byte pointers
    // Each pointer is a bank-local address in bank 0x0E (trainers live in 0x0E/0x0F)
    // But trainer_groups_offset is already a flat address
    
    // Crystal trainer classes are 1-indexed (FALKNER = 1, WHITNEY = 2, ...)
    // num_classes is typically 67 (NUM_TRAINER_CLASSES)
    
    // Calculate bank from the table's own location
    uint8_t table_bank = static_cast<uint8_t>(trainer_groups_offset / 0x4000);
    
    // First pass: collect all group start addresses for boundary checking
    std::vector<uint32_t> group_starts;
    group_starts.reserve(num_classes);
    
    for (uint8_t group = 1; group <= num_classes; ++group) {
        uint32_t ptr_offset = trainer_groups_offset + (static_cast<size_t>(group - 1) * 2);
        
        if (ptr_offset + 2 > rom_->size()) {
            group_starts.push_back(0);  // Invalid
            continue;
        }
        
        uint16_t group_ptr = rom_->read_word(ptr_offset);
        
        // Validate pointer range
        if (group_ptr < 0x4000 || group_ptr >= 0x8000) {
            group_starts.push_back(0);  // Invalid
            continue;
        }
        
        // Convert to flat address
        uint32_t group_flat = (static_cast<uint32_t>(table_bank) * 0x4000) + (group_ptr - 0x4000);
        group_starts.push_back(group_flat);
    }
    
    // Second pass: parse each group with known boundaries
    for (uint8_t group = 1; group <= num_classes; ++group) {
        uint32_t group_start = group_starts[group - 1];
        if (group_start == 0) {
            // Invalid or empty group
            group_counts_[group] = 0;
            continue;
        }
        
        // Check if this group has the same start address as the next group
        // This indicates an empty group (e.g., PokemonProfGroup)
        if (group < num_classes) {
            uint32_t next_start = group_starts[group];  // group is 1-indexed, so group_starts[group] is next
            if (next_start != 0 && next_start == group_start) {
                // Empty group - same pointer as next group
                group_counts_[group] = 0;
                continue;
            }
        }
        
        // Determine the end boundary (next group's start, or end of ROM region)
        uint32_t group_end = (static_cast<uint32_t>(table_bank + 1) * 0x4000);  // End of bank as default
        
        // Find the next group that has a valid (higher) address
        for (uint8_t next = group + 1; next <= num_classes; ++next) {
            if (group_starts[next - 1] != 0 && group_starts[next - 1] > group_start) {
                group_end = group_starts[next - 1];
                break;
            }
        }
        
        // Parse trainers in this group with boundary checking
        size_t count = parse_group(group, group_start, group_end);
        group_counts_[group] = count;
    }
    
    return !entries_.empty();
}

size_t TrainerRegistry::parse_group(uint8_t group, uint32_t group_address, uint32_t group_end) {
    if (!rom_) return 0;
    
    size_t count = 0;
    uint32_t ptr = group_address;
    uint8_t trainer_id = 1;  // Trainer IDs are 1-based within each group
    
    // Parse trainers until we reach the next group's start address or hit safety limit
    // Each trainer ends with db -1 (0xFF)
    // A group can have multiple trainers back-to-back
    
    constexpr size_t MAX_TRAINERS_PER_GROUP = 100;  // Safety limit
    
    while (count < MAX_TRAINERS_PER_GROUP && ptr < rom_->size() && ptr < group_end) {
        // Check for end-of-group (empty name = first byte is @ or terminator)
        uint8_t first_byte = rom_->read_byte(ptr);
        
        // An empty group or end of groups would have invalid data
        // Valid trainer names start with printable characters (0x50-0xFE in Crystal encoding)
        // '@' = 0x50 terminates names, but wouldn't be the first byte of a valid trainer
        if (first_byte == 0x50 || first_byte == 0xFF || first_byte == 0x00) {
            // End of group or invalid data
            break;
        }
        
        // Read trainer name
        std::string name = read_trainer_name(ptr);
        
        // Skip to after name (find @ terminator)
        auto after_name = skip_trainer_name(ptr);
        if (!after_name || *after_name >= group_end) {
            // Couldn't find name terminator or would cross boundary - stop
            break;
        }
        
        uint32_t type_addr = *after_name;
        if (type_addr >= rom_->size() || type_addr >= group_end) break;
        
        // Read trainer type byte
        uint8_t trainer_type = rom_->read_byte(type_addr);
        
        // Validate trainer type (0-3 are valid)
        if (trainer_type > TRAINERTYPE_ITEM_MOVES) {
            // Invalid trainer type - stop parsing this group
            break;
        }
        
        // Skip party data
        auto after_party = skip_trainer_party(type_addr + 1, trainer_type);
        if (!after_party || *after_party > group_end) {
            // Couldn't parse party or would cross boundary - stop
            break;
        }
        
        // Successfully parsed a trainer entry
        TrainerEntry entry;
        entry.group = group;
        entry.id = trainer_id;
        entry.name = name;
        entry.type = trainer_type;
        entry.rom_address = ptr;
        
        // Register the entry
        uint32_t key = (static_cast<uint32_t>(group) << 8) | trainer_id;
        lookup_[key] = entries_.size();
        entries_.push_back(std::move(entry));
        pairs_.insert({group, trainer_id});
        
        ++count;
        ++trainer_id;
        ptr = *after_party;
    }
    
    return count;
}

std::optional<uint32_t> TrainerRegistry::skip_trainer_name(uint32_t address) const {
    if (!rom_) return std::nullopt;
    
    // Names are terminated by @ (0x50 in Crystal encoding)
    // Max reasonable name length is 10 characters
    constexpr size_t MAX_NAME_LEN = 20;
    
    for (size_t i = 0; i < MAX_NAME_LEN; ++i) {
        if (address + i >= rom_->size()) return std::nullopt;
        
        uint8_t byte = rom_->read_byte(address + i);
        if (byte == 0x50) {  // '@' terminator
            return static_cast<uint32_t>(address + i + 1);  // Return position after @
        }
    }
    
    return std::nullopt;  // Couldn't find terminator
}

std::optional<uint32_t> TrainerRegistry::skip_trainer_party(uint32_t address, uint8_t trainer_type) const {
    if (!rom_) return std::nullopt;
    
    // Party data: 1-6 Pokemon entries followed by db -1 (0xFF)
    // Each Pokemon entry has size based on trainer_type
    
    size_t entry_size = pokemon_entry_size(trainer_type);
    uint32_t ptr = address;
    
    constexpr size_t MAX_POKEMON = 6;
    
    for (size_t i = 0; i < MAX_POKEMON + 1; ++i) {
        if (ptr >= rom_->size()) return std::nullopt;
        
        uint8_t first_byte = rom_->read_byte(ptr);
        
        // Check for end marker (db -1 = 0xFF)
        if (first_byte == 0xFF) {
            return ptr + 1;  // Return position after -1 terminator
        }
        
        // Skip this Pokemon entry
        ptr += entry_size;
    }
    
    return std::nullopt;  // Didn't find terminator
}

std::string TrainerRegistry::read_trainer_name(uint32_t address) const {
    if (!rom_) return "";
    
    // Crystal character encoding: 0x50 = '@', 0x80-0xFF = letters
    // For now, just extract bytes until @ and convert to ASCII approximation
    
    std::string result;
    constexpr size_t MAX_LEN = 20;
    
    for (size_t i = 0; i < MAX_LEN && address + i < rom_->size(); ++i) {
        uint8_t byte = rom_->read_byte(address + i);
        
        if (byte == 0x50) break;  // '@' terminator
        
        // Crystal encoding: 0x80 = 'A', 0x99 = 'Z', etc.
        // Simplified conversion
        if (byte >= 0x80 && byte <= 0x99) {
            result += static_cast<char>('A' + (byte - 0x80));
        } else if (byte >= 0x9A && byte <= 0xB3) {
            result += static_cast<char>('a' + (byte - 0x9A));
        } else if (byte == 0xE3) {
            result += '?';  // Rival name placeholder
        } else {
            result += '?';
        }
    }
    
    return result;
}

bool TrainerRegistry::has_trainer(uint8_t group, uint8_t id) const {
    return pairs_.contains({group, id});
}

const TrainerEntry* TrainerRegistry::get_trainer(uint8_t group, uint8_t id) const {
    uint32_t key = (static_cast<uint32_t>(group) << 8) | id;
    auto it = lookup_.find(key);
    if (it != lookup_.end() && it->second < entries_.size()) {
        return &entries_[it->second];
    }
    return nullptr;
}

std::vector<const TrainerEntry*> TrainerRegistry::get_group(uint8_t group) const {
    std::vector<const TrainerEntry*> result;
    for (const auto& entry : entries_) {
        if (entry.group == group) {
            result.push_back(&entry);
        }
    }
    return result;
}

size_t TrainerRegistry::group_count(uint8_t group) const {
    auto it = group_counts_.find(group);
    if (it != group_counts_.end()) {
        return it->second;
    }
    return 0;
}

void TrainerRegistry::print_summary() const {
    std::cout << "\n=== TrainerRegistry Summary ===\n";
    std::cout << "Total trainers: " << entries_.size() << "\n";
    std::cout << "Groups with trainers: " << group_counts_.size() << "\n\n";
    
    std::cout << "Trainers per group:\n";
    for (const auto& [group, count] : group_counts_) {
        if (count > 0) {
            std::cout << "  Group " << std::setw(2) << (int)group << ": " << count << " trainers\n";
        }
    }
}

} // namespace crystal
