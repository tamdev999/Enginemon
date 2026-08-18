#pragma once
// frontends/crystal/world/collision_classifier.hpp
// Translates raw Crystal collision bytes to semantic CollisionClass
//
// This is the ONLY place that knows Crystal collision byte values.
// The runtime engine receives semantic CollisionClass, never raw bytes.
//
// SOURCE-TRACED from:
//   pokecrystal/constants/collision_constants.asm
//   pokecrystal/data/collision/collision_permissions.asm
//
// CRITICAL: Crystal collision constants are SPARSE, not ranges.
// Each constant has explicit semantics from the source.
// Do NOT infer meaning from numeric proximity.

#include "engine/world/collision_types.hpp"
#include <cstdint>
#include <array>

namespace crystal {

//=============================================================================
// CRYSTAL PERMISSION TABLE (from collision_permissions.asm)
// Values: LAND_TILE=0x00, WATER_TILE=0x01, WALL_TILE=0x0F, TALK=0x10
//=============================================================================
constexpr std::array<uint8_t, 256> CRYSTAL_PERMISSION = {{
    // 0x00-0x0F
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x0F, 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x0F,
    // 0x10-0x1F (grass/trees)
    0x00,0x00,0x1F,0x00,0x00,0x1F,0x00,0x00, 0x00,0x00,0x1F,0x00,0x00,0x1F,0x00,0x00,
    // 0x20-0x2F (water/ice)
    0x01,0x01,0x11,0x00,0x11,0x01,0x01,0x0F, 0x01,0x01,0x11,0x00,0x11,0x01,0x01,0x0F,
    // 0x30-0x3F (waterfall/current)
    0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01, 0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,
    // 0x40-0x4F (brake/walk)
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    // 0x50-0x5F (walk alt)
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    // 0x60-0x6F (pits)
    0x00,0x00,0x0F,0x00,0x00,0x00,0x00,0x00, 0x00,0x00,0x0F,0x00,0x00,0x00,0x00,0x00,
    // 0x70-0x7F (warps)
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    // 0x80-0x8F
    0x0F,0x0F,0x0F,0x0F,0x0F,0x00,0x00,0x00, 0x0F,0x0F,0x0F,0x0F,0x0F,0x00,0x00,0x00,
    // 0x90-0x9F (counters)
    0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F, 0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,
    // 0xA0-0xAF (ledges)
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    // 0xB0-0xBF (side walls)
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    // 0xC0-0xCF (side buoys - water)
    0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01, 0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,
    // 0xD0-0xDF
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    // 0xE0-0xEF
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    // 0xF0-0xFF
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x0F,
}};

//=============================================================================
// EXPLICIT COLLISION CLASSIFICATION
// Source-traced from pokecrystal constants - NO RANGE-BASED INFERENCE
//=============================================================================

inline enginemon::CollisionClass classify_crystal_collision(uint8_t raw) {
    using CC = enginemon::CollisionClass;
    
    // Switch on explicit Crystal collision constants
    // CRITICAL: This is an exhaustive switch on known constants, not ranges
    switch (raw) {
        //=====================================================================
        // GRASS TILES - walkable land that triggers encounters
        // Source: collision_constants.asm COLL_TALL_GRASS, COLL_LONG_GRASS
        //=====================================================================
        case 0x10:  // COLL_TALL_GRASS_10 (unused)
        case 0x14:  // COLL_LONG_GRASS
        case 0x18:  // COLL_TALL_GRASS
        case 0x1C:  // COLL_LONG_GRASS_1C (unused)
            return CC::Grass;
            
        //=====================================================================
        // WATER TILES - requires Surf
        // Source: collision_permissions.asm WATER_TILE entries
        //=====================================================================
        case 0x20:  // water
        case 0x21:  // COLL_WATER_21 (unused)
        case 0x25:  // water
        case 0x26:  // water
        case 0x28:  // COLL_CUT_28 (garbage, water)
        case 0x29:  // COLL_WATER (standard water)
        case 0x2D:  // water
        case 0x2E:  // water
        case 0x3C:  // water
        case 0x3D:  // water
        case 0x3E:  // water
        case 0x3F:  // water
        case 0xC8:  // water (side buoy filler)
        case 0xC9:
        case 0xCA:
        case 0xCB:
        case 0xCC:
        case 0xCD:
        case 0xCE:
        case 0xCF:
            return CC::Water;

        //=====================================================================
        // WATERFALL TILES - requires Waterfall
        // Source: COLL_WATERFALL = 0x33, COLL_WATERFALL_RIGHT/LEFT/UP
        //=====================================================================
        case 0x30:  // COLL_WATERFALL_RIGHT (unused)
        case 0x31:  // COLL_WATERFALL_LEFT (unused)
        case 0x32:  // COLL_WATERFALL_UP (unused)
        case 0x33:  // COLL_WATERFALL
        case 0x34:  // water (waterfall filler)
        case 0x35:
        case 0x36:
        case 0x37:
            return CC::Waterfall;
            
        //=====================================================================
        // CURRENT TILES - water with forced movement
        // Source: COLL_CURRENT_RIGHT/LEFT/UP/DOWN
        // Treated as water (behavior handled separately)
        //=====================================================================
        case 0x38:  // COLL_CURRENT_RIGHT (unused)
        case 0x39:  // COLL_CURRENT_LEFT (unused)
        case 0x3A:  // COLL_CURRENT_UP (unused)
        case 0x3B:  // COLL_CURRENT_DOWN (unused)
            return CC::Water;  // Current = water with movement override
            
        //=====================================================================
        // ICE TILES - sliding
        // Source: COLL_ICE = 0x23, COLL_ICE_2B = 0x2B
        //=====================================================================
        case 0x23:  // COLL_ICE
        case 0x2B:  // COLL_ICE_2B (unused)
            return CC::Ice;
            
        //=====================================================================
        // WHIRLPOOL - requires Whirlpool
        // Source: COLL_WHIRLPOOL = 0x24, COLL_WHIRLPOOL_2C = 0x2C
        //=====================================================================
        case 0x24:  // COLL_WHIRLPOOL
        case 0x2C:  // COLL_WHIRLPOOL_2C (unused)
            return CC::Whirlpool;

        //=====================================================================
        // CUTTABLE TREES - wall, interactable with Cut
        // Source: COLL_CUT_TREE = 0x12, COLL_CUT_TREE_1A = 0x1A
        //=====================================================================
        case 0x12:  // COLL_CUT_TREE
        case 0x1A:  // COLL_CUT_TREE_1A (unused)
            return CC::CuttableTree;
            
        //=====================================================================
        // HEADBUTT TREES - wall, interactable with Headbutt
        // Source: COLL_HEADBUTT_TREE = 0x15, 0x1D
        // (Treated as Wall since no separate HeadbuttTree class)
        //=====================================================================
        case 0x15:  // COLL_HEADBUTT_TREE
        case 0x1D:  // COLL_HEADBUTT_TREE_1D (unused)
            return CC::Wall;  // Wall + interactable
            
        //=====================================================================
        // BUOY - wall in water context
        // Source: COLL_BUOY = 0x27, 0x2F
        //=====================================================================
        case 0x27:  // COLL_BUOY
        case 0x2F:  // wall (0x2F)
            return CC::Wall;
            
        //=====================================================================
        // PIT TILES - fall-through warp
        // Source: COLL_PIT = 0x60, COLL_PIT_68 = 0x68
        //=====================================================================
        case 0x60:  // COLL_PIT
        case 0x68:  // COLL_PIT_68 (unused)
            return CC::WarpPit;
            
        //=====================================================================
        // WARP CARPET - requires directional input
        // Source: COLL_WARP_CARPET_DOWN/LEFT/UP/RIGHT
        //=====================================================================
        case 0x70:  // COLL_WARP_CARPET_DOWN
        case 0x76:  // COLL_WARP_CARPET_LEFT
        case 0x78:  // COLL_WARP_CARPET_UP
        case 0x7E:  // COLL_WARP_CARPET_RIGHT
            return CC::WarpCarpet;

        //=====================================================================
        // DOOR WARPS - outdoor building doors
        // Source: COLL_DOOR = 0x71, 0x75, 0x79, 0x7D
        //=====================================================================
        case 0x71:  // COLL_DOOR
        case 0x75:  // COLL_DOOR_75 (unused)
        case 0x79:  // COLL_DOOR_79 (unused)
        case 0x7D:  // COLL_DOOR_7D (unused)
            return CC::WarpDoor;
            
        //=====================================================================
        // CAVE WARPS - cave entrance
        // Source: COLL_CAVE = 0x7B, COLL_CAVE_74 = 0x74
        //=====================================================================
        case 0x74:  // COLL_CAVE_74 (unused)
        case 0x7B:  // COLL_CAVE
            return CC::WarpCave;
            
        //=====================================================================
        // STAIR/LADDER WARPS
        // Source: COLL_LADDER = 0x72, COLL_STAIRCASE = 0x7A, 0x73
        //=====================================================================
        case 0x72:  // COLL_LADDER
        case 0x73:  // COLL_STAIRCASE_73 (unused)
        case 0x7A:  // COLL_STAIRCASE
            return CC::WarpStair;
            
        //=====================================================================
        // OTHER WARP TILES
        // Source: COLL_WARP_77, COLL_WARP_PANEL, COLL_WARP_7F
        //=====================================================================
        case 0x77:  // COLL_WARP_77 (unused)
        case 0x7C:  // COLL_WARP_PANEL
        case 0x7F:  // COLL_WARP_7F (unused)
            return CC::WarpFloor;

        //=====================================================================
        // LEDGE/HOP TILES - one-way hop down
        // Source: COLL_HOP_RIGHT/LEFT/UP/DOWN/etc = 0xA0-0xA7
        //=====================================================================
        case 0xA0:  // COLL_HOP_RIGHT
        case 0xA1:  // COLL_HOP_LEFT
        case 0xA2:  // COLL_HOP_UP (unused)
        case 0xA3:  // COLL_HOP_DOWN
        case 0xA4:  // COLL_HOP_DOWN_RIGHT
        case 0xA5:  // COLL_HOP_DOWN_LEFT
        case 0xA6:  // COLL_HOP_UP_RIGHT (unused)
        case 0xA7:  // COLL_HOP_UP_LEFT (unused)
            return CC::Ledge;
            
        //=====================================================================
        // SIDE WALLS - directional blocking on land
        // Source: COLL_RIGHT_WALL = 0xB0, etc.
        //=====================================================================
        case 0xB0:  // COLL_RIGHT_WALL (blocks from west)
            return CC::SideWallE;
        case 0xB1:  // COLL_LEFT_WALL (blocks from east)
            return CC::SideWallW;
        case 0xB2:  // COLL_UP_WALL (blocks from south)
            return CC::SideWallN;
        case 0xB3:  // COLL_DOWN_WALL (blocks from north, unused)
            return CC::SideWallS;
        case 0xB4:  // COLL_DOWN_RIGHT_WALL (unused)
            return CC::SideWallE;
        case 0xB5:  // COLL_DOWN_LEFT_WALL (unused)
            return CC::SideWallW;
        case 0xB6:  // COLL_UP_RIGHT_WALL (unused)
            return CC::SideWallE;
        case 0xB7:  // COLL_UP_LEFT_WALL (unused)
            return CC::SideWallW;
            
        //=====================================================================
        // SIDE BUOYS - directional blocking in water
        // Source: COLL_RIGHT_BUOY = 0xC0, etc. (all unused in vanilla)
        // Treated as Water (directional blocking checked at runtime)
        //=====================================================================
        case 0xC0:  // COLL_RIGHT_BUOY
        case 0xC1:  // COLL_LEFT_BUOY
        case 0xC2:  // COLL_UP_BUOY
        case 0xC3:  // COLL_DOWN_BUOY
        case 0xC4:  // COLL_DOWN_RIGHT_BUOY
        case 0xC5:  // COLL_DOWN_LEFT_BUOY
        case 0xC6:  // COLL_UP_RIGHT_BUOY
        case 0xC7:  // COLL_UP_LEFT_BUOY
            return CC::Water;  // Water with directional blocking

        //=====================================================================
        // COUNTER TILES - extends interaction reach
        // Source: COLL_COUNTER = 0x90, COLL_BOOKSHELF = 0x91, etc.
        // All 0x90-0x9F are WALL_TILE with counter-like behavior
        //=====================================================================
        case 0x90:  // COLL_COUNTER
        case 0x91:  // COLL_BOOKSHELF
        case 0x92:  // wall (counter)
        case 0x93:  // COLL_PC
        case 0x94:  // COLL_RADIO
        case 0x95:  // COLL_TOWN_MAP
        case 0x96:  // COLL_MART_SHELF
        case 0x97:  // COLL_TV
        case 0x98:  // COLL_COUNTER_98 (unused)
        case 0x99:  // wall
        case 0x9A:  // wall
        case 0x9B:  // wall
        case 0x9C:  // COLL_9C (garbage)
        case 0x9D:  // COLL_WINDOW
        case 0x9E:  // wall
        case 0x9F:  // COLL_INCENSE_BURNER
            return CC::Counter;
            
        //=====================================================================
        // EXPLICIT WALL TILES
        // Source: collision_permissions.asm WALL_TILE entries
        //=====================================================================
        case 0x07:  // COLL_WALL
        case 0x0F:  // wall
        case 0x62:  // wall
        case 0x6A:  // wall
        case 0x80:  // wall
        case 0x81:  // wall
        case 0x82:  // wall
        case 0x83:  // wall
        case 0x84:  // wall
        case 0x88:  // wall
        case 0x89:  // wall
        case 0x8A:  // wall
        case 0x8B:  // wall
        case 0x8C:  // wall
        case 0xFF:  // COLL_FF (garbage, wall)
            return CC::Wall;
            
        //=====================================================================
        // INTERACTABLE WATER TILES (water + talk)
        // Source: collision_permissions.asm WATER_TILE | TALK entries
        //=====================================================================
        case 0x22:  // water+talk
        case 0x2A:  // water+talk
            return CC::Water;  // Water, interactable

        //=====================================================================
        // FLOOR TILES - walkable land (default case)
        // Explicit list of all LAND_TILE entries from collision_permissions.asm
        //=====================================================================
        case 0x00:  // COLL_FLOOR
        case 0x01:  // COLL_01 (garbage)
        case 0x02:  // land
        case 0x03:  // COLL_03 (garbage)
        case 0x04:  // COLL_04 (garbage)
        case 0x05:  // land
        case 0x06:  // land
        case 0x08:  // COLL_CUT_08 (unused)
        case 0x09:  // land
        case 0x0A:  // land
        case 0x0B:  // land
        case 0x0C:  // land
        case 0x0D:  // land
        case 0x0E:  // land
        case 0x11:  // land
        case 0x13:  // land
        case 0x16:  // land
        case 0x17:  // land
        case 0x19:  // land
        case 0x1B:  // land
        case 0x1E:  // land
        case 0x1F:  // land
        // 0x40-0x5F: brake/walk tiles (all LAND_TILE)
        case 0x40:  // COLL_BRAKE (unused)
        case 0x41:  // COLL_WALK_RIGHT (unused)
        case 0x42:  // COLL_WALK_LEFT (unused)
        case 0x43:  // COLL_WALK_UP (unused)
        case 0x44:  // COLL_WALK_DOWN (unused)
        case 0x45:  // COLL_BRAKE_45 (garbage)
        case 0x46:  // COLL_BRAKE_46 (unused)
        case 0x47:  // COLL_BRAKE_47 (unused)
        case 0x48:  // COLL_GRASS_48 (unused)
        case 0x49:  // COLL_GRASS_49 (unused)
        case 0x4A:  // COLL_GRASS_4A (garbage)
        case 0x4B:  // COLL_GRASS_4B (garbage)
        case 0x4C:  // COLL_GRASS_4C (unused)
        case 0x4D:  // land
        case 0x4E:  // land
        case 0x4F:  // land
        case 0x50:  // COLL_WALK_RIGHT_ALT (unused)
        case 0x51:  // COLL_WALK_LEFT_ALT (unused)
        case 0x52:  // COLL_WALK_UP_ALT (unused)
        case 0x53:  // COLL_WALK_DOWN_ALT (unused)
        case 0x54:  // COLL_BRAKE_ALT (unused)
        case 0x55:  // COLL_BRAKE_55 (unused)
        case 0x56:  // COLL_BRAKE_56 (unused)
        case 0x57:  // COLL_BRAKE_57 (unused)
        case 0x58:  // land
        case 0x59:  // land
        case 0x5A:  // land
        case 0x5B:  // COLL_5B (garbage)
        case 0x5C:  // land
        case 0x5D:  // land
        case 0x5E:  // land
        case 0x5F:  // land
        // 0x60-0x6F (excluding pits 0x60, 0x68 and walls 0x62, 0x6A)
        case 0x61:  // COLL_VIRTUAL_BOY (garbage)
        case 0x63:  // land
        case 0x64:  // COLL_64 (garbage)
        case 0x65:  // COLL_65 (garbage)
        case 0x66:  // land
        case 0x67:  // land
        case 0x69:  // land
        case 0x6B:  // land
        case 0x6C:  // land
        case 0x6D:  // land
        case 0x6E:  // land
        case 0x6F:  // land
        // 0x85-0x87, 0x8D-0x8F: land in wall range
        case 0x85:  // land
        case 0x86:  // land
        case 0x87:  // land
        case 0x8D:  // land
        case 0x8E:  // land
        case 0x8F:  // land
        // 0xA8-0xAF: land after ledges
        case 0xA8:  // land
        case 0xA9:  // land
        case 0xAA:  // land
        case 0xAB:  // land
        case 0xAC:  // land
        case 0xAD:  // land
        case 0xAE:  // land
        case 0xAF:  // land
        // 0xB8-0xBF: land after side walls
        case 0xB8:  // land
        case 0xB9:  // land
        case 0xBA:  // land
        case 0xBB:  // land
        case 0xBC:  // land
        case 0xBD:  // land
        case 0xBE:  // land
        case 0xBF:  // land
        // 0xD0-0xFE: all land
        case 0xD0: case 0xD1: case 0xD2: case 0xD3:
        case 0xD4: case 0xD5: case 0xD6: case 0xD7:
        case 0xD8: case 0xD9: case 0xDA: case 0xDB:
        case 0xDC: case 0xDD: case 0xDE: case 0xDF:
        case 0xE0: case 0xE1: case 0xE2: case 0xE3:
        case 0xE4: case 0xE5: case 0xE6: case 0xE7:
        case 0xE8: case 0xE9: case 0xEA: case 0xEB:
        case 0xEC: case 0xED: case 0xEE: case 0xEF:
        case 0xF0: case 0xF1: case 0xF2: case 0xF3:
        case 0xF4: case 0xF5: case 0xF6: case 0xF7:
        case 0xF8: case 0xF9: case 0xFA: case 0xFB:
        case 0xFC: case 0xFD: case 0xFE:
            return CC::Floor;
            
        default:
            // Unknown - treat as wall (safe default)
            return CC::Unknown;
    }
}

//=============================================================================
// WARP ENTRANCE HELPER
// Used by runtime to check if stepping on a tile triggers a warp
//=============================================================================

inline bool crystal_is_warp_entrance(uint8_t raw) {
    // Crystal collision IDs that trigger warps when stepped on
    switch (raw) {
        // Pit tiles
        case 0x60:  // COLL_PIT
        case 0x68:  // COLL_PIT_68
        // Warp carpet (requires direction input)
        case 0x70:  // COLL_WARP_CARPET_DOWN
        case 0x76:  // COLL_WARP_CARPET_LEFT
        case 0x78:  // COLL_WARP_CARPET_UP
        case 0x7E:  // COLL_WARP_CARPET_RIGHT
        // Door warps
        case 0x71:  // COLL_DOOR
        case 0x75:  // COLL_DOOR_75
        case 0x79:  // COLL_DOOR_79
        case 0x7D:  // COLL_DOOR_7D
        // Cave warps
        case 0x74:  // COLL_CAVE_74
        case 0x7B:  // COLL_CAVE
        // Stair/ladder warps
        case 0x72:  // COLL_LADDER
        case 0x73:  // COLL_STAIRCASE_73
        case 0x7A:  // COLL_STAIRCASE
        // Other warps
        case 0x77:  // COLL_WARP_77
        case 0x7C:  // COLL_WARP_PANEL
        case 0x7F:  // COLL_WARP_7F
            return true;
        default:
            return false;
    }
}

} // namespace crystal
