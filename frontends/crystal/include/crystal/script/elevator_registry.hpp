#pragma once
// crystal/script/elevator_registry.hpp
// Registry for compiled elevator floor-list definitions
//
// Extracts elevator floor data from ROM and assigns semantic IDs.
// Used during semantic lowering to resolve ROM pointers to stable IDs.

#include "crystal/rom/loader.hpp"
#include "engine/core/types.hpp"
#include <unordered_map>
#include <vector>
#include <optional>
#include <string>

namespace crystal {

// Registry that extracts and compiles elevator definitions from ROM
class ElevatorRegistry {
public:
    ElevatorRegistry() = default;
    explicit ElevatorRegistry(const RomData& rom);
    
    // Set ROM reference for lazy extraction
    void set_rom(const RomData& rom) { rom_ = &rom; }
    
    // Register an elevator from a flat ROM address
    // Returns the semantic ElevatorId assigned
    // If the address was already registered, returns the existing ID
    enginemon::ElevatorId register_elevator(uint32_t flat_address, const std::string& name = "");
    
    // Look up elevator by flat ROM address
    // Returns ELEVATOR_NONE if not found
    enginemon::ElevatorId lookup_by_address(uint32_t flat_address) const;
    
    // Look up elevator by semantic ID
    const enginemon::ElevatorDefinition* get_definition(enginemon::ElevatorId id) const;
    
    // Get all compiled elevator definitions
    const std::vector<enginemon::ElevatorDefinition>& all_definitions() const { return definitions_; }
    
    // Get count of registered elevators
    size_t size() const { return definitions_.size(); }
    
    // Check if an ID is valid
    bool contains(enginemon::ElevatorId id) const { 
        return id != enginemon::ELEVATOR_NONE && id < definitions_.size(); 
    }
    
    // Clear all registered elevators
    void clear() { 
        definitions_.clear(); 
        address_to_id_.clear(); 
    }
    
private:
    const RomData* rom_ = nullptr;
    std::vector<enginemon::ElevatorDefinition> definitions_;
    std::unordered_map<uint32_t, enginemon::ElevatorId> address_to_id_;
    
    // Extract floor list data from ROM address
    // Returns nullopt if extraction fails
    std::optional<enginemon::ElevatorDefinition> extract_floor_list(uint32_t flat_address) const;
};

} // namespace crystal
