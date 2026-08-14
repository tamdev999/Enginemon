#pragma once
// crystal/script/trainer_registry.hpp
// Registry for compiled trainer definitions from ROM
//
// Extracts the actual (group, id) trainer pairs from the ROM's TrainerGroups
// pointer table. Used during Stage 6 linking to validate trainer references
// against real existing trainers, not fabricated conservative ranges.
//
// ROM Structure (from pokecrystal/data/trainers/party_pointers.asm):
//   TrainerGroups: table of 2-byte pointers, one per trainer class (1-67)
//   Each group points to trainer party data (data/trainers/parties.asm):
//     - Multiple trainers per group, each terminated by db -1 (0xFF)
//     - Trainer entry: "NAME@" (8+ bytes), TRAINERTYPE_* (1 byte), party data
//
// This registry parses each group to count actual trainers and registers
// only (group, id) pairs that actually exist in the ROM.

#include "crystal/rom/loader.hpp"
#include "engine/core/types.hpp"
#include <unordered_map>
#include <vector>
#include <set>
#include <optional>
#include <string>
#include <cstdint>

namespace crystal {

// Minimal trainer definition (enough for Stage 6 membership validation)
// Full party data extraction is deferred until battle subsystem is built
struct TrainerEntry {
    uint8_t group;      // Trainer class (1-67)
    uint8_t id;         // Trainer index within class (1-based)
    std::string name;   // Trainer name (decoded from ROM)
    uint8_t type;       // TRAINERTYPE_* constant
    uint32_t rom_address; // Flat ROM address for debugging/future extraction
};

// Registry that extracts and validates trainer definitions from ROM
class TrainerRegistry {
public:
    TrainerRegistry() = default;
    explicit TrainerRegistry(const RomData& rom, uint32_t trainer_groups_offset, uint8_t num_classes);
    
    // Set ROM reference for lazy extraction
    void set_rom(const RomData& rom) { rom_ = &rom; }
    
    // Initialize from ROM - parses all trainer groups
    // Returns false if extraction fails
    bool initialize(uint32_t trainer_groups_offset, uint8_t num_classes);
    
    // Check if a (group, id) pair exists
    // This is the primary validation method for Stage 6 linking
    bool has_trainer(uint8_t group, uint8_t id) const;
    
    // Get trainer entry (nullptr if not found)
    const TrainerEntry* get_trainer(uint8_t group, uint8_t id) const;
    
    // Get all (group, id) pairs for CompiledGameData population
    const std::set<std::pair<uint8_t, uint8_t>>& all_pairs() const { return pairs_; }
    
    // Get all entries for a specific group
    std::vector<const TrainerEntry*> get_group(uint8_t group) const;
    
    // Get count of trainers in a group
    size_t group_count(uint8_t group) const;
    
    // Total number of trainer entries
    size_t total_count() const { return entries_.size(); }
    
    // Number of trainer groups (classes)
    size_t group_count_total() const { return group_counts_.size(); }
    
    // Print registry summary for debugging
    void print_summary() const;
    
    // Clear all entries
    void clear() {
        entries_.clear();
        pairs_.clear();
        group_counts_.clear();
        lookup_.clear();
    }
    
private:
    const RomData* rom_ = nullptr;
    
    // All trainer entries
    std::vector<TrainerEntry> entries_;
    
    // Fast lookup: (group, id) -> entry index
    std::unordered_map<uint32_t, size_t> lookup_;  // Key = (group << 8) | id
    
    // Set of all valid (group, id) pairs
    std::set<std::pair<uint8_t, uint8_t>> pairs_;
    
    // Count per group for debugging
    std::unordered_map<uint8_t, size_t> group_counts_;
    
    // Parse a single trainer group from ROM
    // group_end is the start address of the next group (for boundary checking)
    // Returns count of trainers found in this group
    size_t parse_group(uint8_t group, uint32_t group_address, uint32_t group_end);
    
    // Skip a trainer name (find @ terminator)
    // Returns position after the @ byte
    std::optional<uint32_t> skip_trainer_name(uint32_t address) const;
    
    // Skip trainer party data based on TRAINERTYPE_*
    // Returns position after party data and db -1 terminator
    std::optional<uint32_t> skip_trainer_party(uint32_t address, uint8_t trainer_type) const;
    
    // Read null-terminated Crystal string (@ = terminator)
    std::string read_trainer_name(uint32_t address) const;
};

} // namespace crystal
