// crystal/script/elevator_registry.cpp
// Elevator floor-list extraction and registry implementation

#include "crystal/script/elevator_registry.hpp"
#include <stdexcept>

namespace crystal {

using namespace enginemon;

ElevatorRegistry::ElevatorRegistry(const RomData& rom) 
    : rom_(&rom) 
{
}

ElevatorId ElevatorRegistry::register_elevator(uint32_t flat_address, const std::string& name) {
    // Check if already registered
    auto it = address_to_id_.find(flat_address);
    if (it != address_to_id_.end()) {
        return it->second;
    }
    
    if (!rom_) {
        throw std::runtime_error("ElevatorRegistry: ROM not set");
    }
    
    // Extract floor list data
    auto def_opt = extract_floor_list(flat_address);
    if (!def_opt) {
        // Extraction failed - return ELEVATOR_NONE
        return ELEVATOR_NONE;
    }
    
    auto& def = *def_opt;
    
    // Check for duplicate by content (same floor list = same elevator)
    for (size_t i = 0; i < definitions_.size(); ++i) {
        if (definitions_[i] == def) {
            // Same content, reuse existing ID
            address_to_id_[flat_address] = static_cast<ElevatorId>(i);
            return static_cast<ElevatorId>(i);
        }
    }
    
    // Assign new ID
    ElevatorId id = static_cast<ElevatorId>(definitions_.size());
    def.id = id;
    def.name = name.empty() ? ("elevator_" + std::to_string(id)) : name;
    
    address_to_id_[flat_address] = id;
    definitions_.push_back(std::move(def));
    
    return id;
}

ElevatorId ElevatorRegistry::lookup_by_address(uint32_t flat_address) const {
    auto it = address_to_id_.find(flat_address);
    if (it != address_to_id_.end()) {
        return it->second;
    }
    return ELEVATOR_NONE;
}

const ElevatorDefinition* ElevatorRegistry::get_definition(ElevatorId id) const {
    if (id == ELEVATOR_NONE || id >= definitions_.size()) {
        return nullptr;
    }
    return &definitions_[id];
}

std::optional<ElevatorDefinition> ElevatorRegistry::extract_floor_list(uint32_t flat_address) const {
    if (!rom_) return std::nullopt;
    
    // Floor list format from pokecrystal:
    //   db count          ; number of floors
    //   elevfloor x N     ; N floor entries
    //   db -1             ; terminator (0xFF)
    //
    // Each elevfloor entry (4 bytes):
    //   db floor_label    ; FLOOR_* constant (0-15)
    //   db warp_id        ; warp destination index (1-based)
    //   db map_group      ; map group
    //   db map_index      ; map index
    
    // Read floor count
    if (flat_address >= rom_->size()) return std::nullopt;
    uint8_t floor_count = rom_->read_byte(flat_address);
    
    // Sanity check - elevator shouldn't have more than 20 floors
    if (floor_count == 0 || floor_count > 20) return std::nullopt;
    
    ElevatorDefinition def;
    def.floors.reserve(floor_count);
    
    uint32_t ptr = flat_address + 1;  // Skip count byte
    
    for (uint8_t i = 0; i < floor_count; ++i) {
        if (ptr + 4 > rom_->size()) return std::nullopt;
        
        uint8_t label = rom_->read_byte(ptr);
        uint8_t warp_id = rom_->read_byte(ptr + 1);
        uint8_t map_group = rom_->read_byte(ptr + 2);
        uint8_t map_index = rom_->read_byte(ptr + 3);
        
        // Validate floor label (0-15 valid)
        if (label > 15) return std::nullopt;
        
        // Convert to semantic MapId
        MapId target_map = (static_cast<uint16_t>(map_group) << 8) | map_index;
        
        ElevatorFloor floor;
        floor.label = static_cast<FloorLabel>(label);
        floor.warp_id = warp_id;
        floor.target_map = target_map;
        
        def.floors.push_back(floor);
        ptr += 4;
    }
    
    // Verify terminator
    if (ptr >= rom_->size()) return std::nullopt;
    uint8_t terminator = rom_->read_byte(ptr);
    if (terminator != 0xFF) {
        // Not a valid floor list (missing terminator)
        return std::nullopt;
    }
    
    return def;
}

} // namespace crystal
