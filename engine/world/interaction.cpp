// engine/world/interaction.cpp
// Native interaction system implementation
//
// Reference: pokecrystal/engine/overworld/events.asm CheckAPressOW
// Reference: pokecrystal/engine/overworld/npc_movement.asm CheckFacingObject
// Reference: pokecrystal/home/map.asm CheckFacingBGEvent

#include "engine/world/interaction.hpp"

namespace enginemon {

//=============================================================================
// CONSTRUCTION
//=============================================================================

Interaction::Interaction() = default;

//=============================================================================
// FACING CALCULATION
// Reference: pokecrystal/home/map.asm GetFacingTileCoord
//=============================================================================

void Interaction::get_facing_cell(int32_t x, int32_t y, Direction dir,
                                   int32_t& out_x, int32_t& out_y) {
    out_x = x;
    out_y = y;
    
    switch (dir) {
        case Direction::Down:  out_y += 1; break;
        case Direction::Up:    out_y -= 1; break;
        case Direction::Left:  out_x -= 1; break;
        case Direction::Right: out_x += 1; break;
    }
}

//=============================================================================
// COUNTER DETECTION
// Now uses semantic CollisionClass instead of raw Crystal bytes
//=============================================================================

bool Interaction::is_counter_tile(CollisionClass coll) const {
    return collision_is_counter(coll);
}

//=============================================================================
// BG EVENT FACING
// Reference: pokecrystal/engine/overworld/events.asm BGEventJumptable
//=============================================================================

bool Interaction::bg_event_requires_facing(uint8_t bg_type) {
    switch (bg_type) {
        case BgEventTypeId::Up:
        case BgEventTypeId::Down:
        case BgEventTypeId::Left:
        case BgEventTypeId::Right:
            return true;
        default:
            return false;
    }
}

std::optional<Direction> Interaction::bg_event_required_facing(uint8_t bg_type) {
    switch (bg_type) {
        case BgEventTypeId::Up:    return Direction::Up;
        case BgEventTypeId::Down:  return Direction::Down;
        case BgEventTypeId::Left:  return Direction::Left;
        case BgEventTypeId::Right: return Direction::Right;
        default:                   return std::nullopt;
    }
}

//=============================================================================
// OBJECT LOOKUP
// Reference: pokecrystal/engine/overworld/npc_movement.asm IsNPCAtCoord
// Reference: pokecrystal/engine/overworld/events.asm TryObjectEvent
//=============================================================================

std::optional<InteractionResult> Interaction::try_object(
    const std::vector<InteractableObject>& objects,
    int32_t x, int32_t y
) const {
    for (const auto& obj : objects) {
        // Check coordinates match
        if (obj.x != x || obj.y != y) continue;
        
        // Can't interact with moving objects
        // Reference: CheckFacingObject checks OBJECT_WALKING == STANDING
        if (obj.is_moving) continue;
        
        // Found a valid object
        InteractionResult result;
        result.type = InteractionType::Object;
        result.target_x = x;
        result.target_y = y;
        result.object_local_id = obj.local_id;
        result.object_script_id = obj.script_id;
        result.is_trainer = obj.is_trainer;
        return result;
    }
    
    return std::nullopt;
}

//=============================================================================
// BG EVENT LOOKUP
// Reference: pokecrystal/home/map.asm CheckFacingBGEvent
// Reference: pokecrystal/engine/overworld/events.asm TryBGEvent
//=============================================================================

std::optional<InteractionResult> Interaction::try_bg_event(
    const std::vector<InteractableBgEvent>& bg_events,
    int32_t x, int32_t y,
    Direction player_facing,
    FlagChecker flag_checker
) const {
    for (const auto& evt : bg_events) {
        // Check coordinates match
        if (evt.x != x || evt.y != y) continue;
        
        // Check facing requirement for directional events
        auto required = bg_event_required_facing(evt.type);
        if (required.has_value() && *required != player_facing) {
            continue;
        }
        
        // Evaluate condition flag for IFSET/IFNOTSET types
        // Reference: pokecrystal/engine/overworld/events.asm BGEventJumptable
        if (flag_checker && !evt.condition_flag.empty()) {
            bool flag_is_set = flag_checker(evt.condition_flag);
            
            if (evt.type == BgEventTypeId::IfSet) {
                // BGEVENT_IFSET: trigger only when flag IS set
                if (!flag_is_set) continue;
            }
            else if (evt.type == BgEventTypeId::IfNotSet) {
                // BGEVENT_IFNOTSET: trigger only when flag is NOT set
                if (flag_is_set) continue;
            }
            else if (evt.type == BgEventTypeId::ItemIfSet && !evt.item_id.empty()) {
                // BGEVENT_ITEMIFSET: hidden item - suppress if already collected
                // The condition_flag represents the "item collected" flag
                if (flag_is_set) continue;
            }
        }
        
        // Found a valid BG event
        InteractionResult result;
        result.target_x = x;
        result.target_y = y;
        result.bg_event_type = evt.type;
        result.bg_script_id = evt.script_id;
        result.bg_item_id = evt.item_id;
        result.bg_item_quantity = evt.quantity;
        
        // Determine interaction type based on BG event type
        if (evt.type == BgEventTypeId::ItemIfSet || !evt.item_id.empty()) {
            result.type = InteractionType::HiddenItem;
        } else {
            result.type = InteractionType::BgEvent;
        }
        
        return result;
    }
    
    return std::nullopt;
}

//=============================================================================
// MAIN INTERACTION CHECK
// Reference: pokecrystal/engine/overworld/events.asm CheckAPressOW
//
// Dispatch order:
// 1. TryObjectEvent (NPCs/trainers)
// 2. TryBGEvent (signs/hidden items)
// 3. TryTileCollisionEvent (not implemented - PC, bookshelf)
//=============================================================================

InteractionResult Interaction::check(
    const InteractionMap& map,
    const std::vector<InteractableObject>& objects,
    const std::vector<InteractableBgEvent>& bg_events,
    int32_t player_x,
    int32_t player_y,
    Direction player_facing,
    FlagChecker flag_checker
) const {
    // Calculate facing cell
    int32_t fx, fy;
    get_facing_cell(player_x, player_y, player_facing, fx, fy);
    
    // Bounds check
    if (fx < 0 || fy < 0 || fx >= map.width || fy >= map.height) {
        return InteractionResult{};
    }
    
    // 1. TryObjectEvent - objects have priority
    auto obj_result = try_object(objects, fx, fy);
    if (obj_result) {
        return *obj_result;
    }
    
    // Check for counter tile - doubles reach
    // Reference: pokecrystal/engine/overworld/npc_movement.asm CheckFacingObject
    if (map.get_collision) {
        CollisionClass coll = map.get_collision(fx, fy);
        if (is_counter_tile(coll)) {
            // Double the facing distance for counters
            int32_t fx2, fy2;
            get_facing_cell(fx, fy, player_facing, fx2, fy2);
            
            // Bounds check extended position
            if (fx2 >= 0 && fy2 >= 0 && fx2 < map.width && fy2 < map.height) {
                auto obj_across_counter = try_object(objects, fx2, fy2);
                if (obj_across_counter) {
                    return *obj_across_counter;
                }
            }
        }
    }
    
    // 2. TryBGEvent - signs/hidden items
    auto bg_result = try_bg_event(bg_events, fx, fy, player_facing, flag_checker);
    if (bg_result) {
        return *bg_result;
    }
    
    // No interaction found
    return InteractionResult{};
}

} // namespace enginemon
