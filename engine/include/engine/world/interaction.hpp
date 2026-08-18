#pragma once
// engine/world/interaction.hpp
// Native interaction system for Pokémon-style A-button dispatch
//
// Reference: pokecrystal/engine/overworld/events.asm CheckAPressOW
// Reference: Gen2Recomped/src/world/OverworldController.lua interact()
//
// Dispatch order (from pokecrystal):
// 1. TryObjectEvent - NPCs take priority
// 2. TryBGEvent - Signs/hidden items only if no NPC
// 3. TryTileCollisionEvent - Special tiles like PC
//
// Key behaviors:
// - Objects first, BG events second (precedence)
// - Counter tiles double the reach distance
// - Moving NPCs cannot be interacted with
// - Directional BG events require matching facing

#include "engine/core/types.hpp"
#include "engine/world/collision_types.hpp"
#include <cstdint>
#include <string>
#include <optional>
#include <vector>
#include <functional>

namespace enginemon {

//=============================================================================
// INTERACTION RESULT
//=============================================================================

enum class InteractionType {
    None,           // No interaction found
    Object,         // NPC/trainer/itemball
    BgEvent,        // Sign/readable
    HiddenItem,     // Hidden item on ground
    Tile,           // Special tile (PC, bookshelf)
};

struct InteractionResult {
    InteractionType type = InteractionType::None;
    
    // Target cell (after facing calculation)
    int32_t target_x = 0;
    int32_t target_y = 0;
    
    // For Object interactions
    uint16_t object_local_id = 0;   // 1-indexed object ID within map
    std::string object_script_id;   // Semantic script ID
    bool is_trainer = false;
    
    // For BgEvent interactions  
    uint8_t bg_event_type = 0;      // BgEventType (Read, HiddenItem, etc.)
    std::string bg_script_id;       // Semantic script ID
    std::string bg_item_id;         // For hidden items
    uint8_t bg_item_quantity = 0;
    
    // Helper
    bool found() const { return type != InteractionType::None; }
    const std::string& script_id() const {
        return type == InteractionType::Object ? object_script_id : bg_script_id;
    }
};

//=============================================================================
// BG EVENT TYPES (from pokecrystal)
//=============================================================================

// Matches crystal::BgEventType but repeated here to avoid frontend dependency
namespace BgEventTypeId {
    constexpr uint8_t Read = 0;          // BGEVENT_READ - any facing
    constexpr uint8_t Up = 1;            // BGEVENT_UP - requires facing up
    constexpr uint8_t Down = 2;          // BGEVENT_DOWN
    constexpr uint8_t Right = 3;         // BGEVENT_RIGHT
    constexpr uint8_t Left = 4;          // BGEVENT_LEFT
    constexpr uint8_t IfSet = 5;         // BGEVENT_IFSET - flag check
    constexpr uint8_t IfNotSet = 6;      // BGEVENT_IFNOTSET
    constexpr uint8_t ItemIfSet = 7;     // BGEVENT_ITEMIFSET - hidden item
    constexpr uint8_t Copy = 8;          // BGEVENT_COPY
}

//=============================================================================
// ENTITY INTERFACES
// Minimal interfaces to avoid coupling to specific World/Actor classes
//=============================================================================

// Object (NPC/trainer) for interaction checking
struct InteractableObject {
    uint16_t local_id;          // 1-indexed map object ID
    int32_t x;                  // Current cell X
    int32_t y;                  // Current cell Y
    bool is_moving;             // Can't interact if moving
    bool is_trainer;
    std::string script_id;      // Semantic script ID (no ROM addresses)
    std::string visibility_flag; // Empty if always visible
};

// BG event (sign/readable/hidden item)
struct InteractableBgEvent {
    int32_t x;
    int32_t y;
    uint8_t type;               // BgEventTypeId
    std::string script_id;      // For readable events
    std::string item_id;        // For hidden items
    uint8_t quantity;
    std::string condition_flag; // For IFSET/IFNOTSET and hidden items
};

// Map interface for interaction checking
struct InteractionMap {
    int32_t width;              // Map width in tiles
    int32_t height;             // Map height in tiles
    
    // Get collision class at tile (for counter detection)
    std::function<CollisionClass(int32_t x, int32_t y)> get_collision;
};

//=============================================================================
// INTERACTION CHECKER
//=============================================================================

class Interaction {
public:
    Interaction();
    
    // Flag checker function type - returns true if flag is set
    using FlagChecker = std::function<bool(const std::string&)>;
    
    // Main entry point: check what player would interact with
    // Matches pokecrystal CheckAPressOW dispatch order
    // flag_checker: optional callback to evaluate IFSET/IFNOTSET conditions
    InteractionResult check(
        const InteractionMap& map,
        const std::vector<InteractableObject>& objects,
        const std::vector<InteractableBgEvent>& bg_events,
        int32_t player_x,
        int32_t player_y,
        Direction player_facing,
        FlagChecker flag_checker = nullptr
    ) const;
    
    // Get facing cell coordinates
    static void get_facing_cell(int32_t x, int32_t y, Direction dir,
                                int32_t& out_x, int32_t& out_y);
    
    // Check if tile is a counter (doubles interaction reach)
    bool is_counter_tile(CollisionClass coll) const;
    
    // Check if BG event type requires specific facing
    static bool bg_event_requires_facing(uint8_t bg_type);
    
    // Get required facing for directional BG event
    static std::optional<Direction> bg_event_required_facing(uint8_t bg_type);

private:
    // Try to find object at cell (TryObjectEvent equivalent)
    std::optional<InteractionResult> try_object(
        const std::vector<InteractableObject>& objects,
        int32_t x, int32_t y
    ) const;
    
    // Try to find BG event at cell (TryBGEvent equivalent)
    std::optional<InteractionResult> try_bg_event(
        const std::vector<InteractableBgEvent>& bg_events,
        int32_t x, int32_t y,
        Direction player_facing,
        FlagChecker flag_checker
    ) const;
};

//=============================================================================
// UTILITY
//=============================================================================

// Convert Direction to string for Lua/debug
inline const char* direction_name(Direction dir) {
    switch (dir) {
        case Direction::Down: return "down";
        case Direction::Up: return "up";
        case Direction::Left: return "left";
        case Direction::Right: return "right";
    }
    return "down";
}

// Parse direction from string
inline Direction parse_direction(const char* name) {
    if (!name) return Direction::Down;
    switch (name[0]) {
        case 'u': case 'U': return Direction::Up;
        case 'l': case 'L': return Direction::Left;
        case 'r': case 'R': return Direction::Right;
        default: return Direction::Down;
    }
}

} // namespace enginemon
