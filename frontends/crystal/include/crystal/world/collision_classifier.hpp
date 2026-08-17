#pragma once
// frontends/crystal/world/collision_classifier.hpp
// Translates raw Crystal collision bytes to semantic CollisionClass
//
// This is the ONLY place that knows Crystal collision byte values.
// The runtime engine receives semantic CollisionClass, never raw bytes.
//
// Reference: pokecrystal/constants/collision_constants.asm
// Reference: Gen2Recomped Map.lua gen2IsEntrance, cellTile

#include "engine/world/collision_types.hpp"
#include <cstdint>

namespace crystal {

//=============================================================================
// CRYSTAL COLLISION BYTE TRANSLATION
//=============================================================================

// Translate a raw Crystal collision byte to semantic CollisionClass
// This function contains ALL Crystal-specific collision byte interpretation.
inline enginemon::CollisionClass classify_crystal_collision(uint8_t raw) {
    using CC = enginemon::CollisionClass;
    
    // pokecrystal collision_constants.asm reference:
    
    // 0x00 = COLL_FLOOR (walkable)
    if (raw == 0x00) return CC::Floor;
    
    // 0x01-0x06 = various floor types (all walkable)
    if (raw >= 0x01 && raw <= 0x06) return CC::Floor;
    
    // 0x07 = COLL_WALL (solid wall)
    if (raw == 0x07) return CC::Wall;
    
    // 0x08-0x0F = walls and blocked tiles
    if (raw >= 0x08 && raw <= 0x0F) return CC::Wall;
    
    // 0x10-0x1F = water tiles
    if (raw >= 0x10 && raw <= 0x1F) return CC::Water;
    
    // 0x20-0x27 = cuttable trees
    if (raw >= 0x20 && raw <= 0x27) return CC::CuttableTree;
    
    // 0x28-0x2F = rocks (smashable)
    if (raw >= 0x28 && raw <= 0x2F) return CC::SmashableRock;
    
    // 0x30-0x37 = tall grass
    if (raw >= 0x30 && raw <= 0x37) return CC::Grass;
    
    // 0x38-0x3F = whirlpool tiles
    if (raw == 0x38) return CC::Whirlpool;
    if (raw >= 0x39 && raw <= 0x3F) return CC::Water;
    
    // 0x40-0x4F = ledges (directional)
    if (raw >= 0x40 && raw <= 0x4F) return CC::Ledge;
    
    // 0x50-0x5F = ice/spinner tiles
    if (raw >= 0x50 && raw <= 0x5F) return CC::Ice;
    
    // 0x60 = COLL_PIT (fall-through hole)
    if (raw == 0x60) return CC::WarpPit;
    
    // 0x68 = COLL_PIT_68 (another pit variant)
    if (raw == 0x68) return CC::WarpPit;
    
    // 0x61-0x67, 0x69-0x6F = various special tiles
    if (raw >= 0x61 && raw <= 0x6F) return CC::Floor;
    
    // 0x70 = WARP_CARPET_DOWN
    // 0x76 = WARP_CARPET_LEFT
    // 0x78 = WARP_CARPET_RIGHT
    // 0x7E = WARP_CARPET_UP
    if (raw == 0x70 || raw == 0x76 || raw == 0x78 || raw == 0x7E) {
        return CC::WarpCarpet;
    }
    
    // 0x71 = COLL_DOOR (outdoor building door)
    if (raw == 0x71) return CC::WarpDoor;
    
    // 0x72, 0x73 = COLL_STAIRCASE (clears standing-on-warp)
    if (raw == 0x72 || raw == 0x73) return CC::WarpStair;
    
    // 0x7B = COLL_CAVE (cave entrance)
    if (raw == 0x7B) return CC::WarpCave;
    
    // 0x74-0x7A, 0x7C-0x7D, 0x7F = other warp range tiles
    if (raw >= 0x74 && raw <= 0x7F) return CC::WarpFloor;
    
    // 0x80-0x8F = elevated/raised tiles (walkable)
    if (raw >= 0x80 && raw <= 0x8F) return CC::Floor;
    
    // 0x90-0x9F = counter tiles
    if (raw >= 0x90 && raw <= 0x9F) return CC::Counter;
    
    // 0xA0-0xAF = waterfall tiles
    if (raw >= 0xA0 && raw <= 0xA7) return CC::Waterfall;
    if (raw >= 0xA8 && raw <= 0xAF) return CC::Water;
    
    // 0xB0-0xB3 = side walls
    // B0=blocks from west (COLL_RIGHT_WALL), B1=blocks from east (COLL_LEFT_WALL)
    // B2=blocks from south (COLL_UP_WALL), B3=blocks from north (COLL_DOWN_WALL)
    if (raw == 0xB0) return CC::SideWallE;  // blocks from west = east wall
    if (raw == 0xB1) return CC::SideWallW;  // blocks from east = west wall
    if (raw == 0xB2) return CC::SideWallN;  // blocks from south = north wall
    if (raw == 0xB3) return CC::SideWallS;  // blocks from north = south wall
    
    // 0xC0-0xCF = strength boulders
    if (raw >= 0xC0 && raw <= 0xCF) return CC::StrengthBoulder;
    
    // Everything else is a wall
    return CC::Wall;
}

// Check if raw Crystal collision byte is a warp entrance
// This is used during frontend processing before classification is stored
inline bool crystal_is_warp_entrance(uint8_t raw) {
    // 0x60, 0x68 = pits, 0x70-0x7F = carpet/door/stair range
    return raw == 0x60 || raw == 0x68 || (raw >= 0x70 && raw <= 0x7F);
}

} // namespace crystal
