// tests/scripting/runtime_test_shared.hpp
// Shared test framework (macros, externs, using-directives, collision test helpers)
// for split runtime_test TUs. Definitions live in runtime_test_main.cpp.
// Each TU includes only the project headers it actually uses.
#pragma once

#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <array>
#include <cstdint>

using namespace crystal;
using namespace enginemon;

// =============================================================================
// TEST FRAMEWORK — definitions in runtime_test_main.cpp
// =============================================================================

extern int  g_tests_passed;
extern int  g_tests_failed;
extern bool g_current_test_failed;

#define TEST(name) void test_##name()
#define RUN_TEST(name) run_test(#name, test_##name)

#define ASSERT_TRUE(cond) \
    if (!(cond)) { \
        std::cerr << "  FAIL: " << #cond << " at line " << __LINE__ << "\n"; \
        g_current_test_failed = true; \
        return; \
    }

#define ASSERT_FALSE(cond) \
    if ((cond)) { \
        std::cerr << "  FAIL: NOT " << #cond << " at line " << __LINE__ << "\n"; \
        g_current_test_failed = true; \
        return; \
    }

#define ASSERT_EQ(a, b) \
    if ((a) != (b)) { \
        std::cerr << "  FAIL: " << #a << " == " << #b << "\n"; \
        std::cerr << "    Expected: " << static_cast<int64_t>(b) << "\n"; \
        std::cerr << "    Actual: " << static_cast<int64_t>(a) << "\n"; \
        std::cerr << "    at line " << __LINE__ << "\n"; \
        g_current_test_failed = true; \
        return; \
    }

#define ASSERT_STR_EQ(a, b) \
    if (std::string(a) != std::string(b)) { \
        std::cerr << "  FAIL: " << #a << " == " << #b << "\n"; \
        std::cerr << "    Expected: \"" << (b) << "\"\n"; \
        std::cerr << "    Actual:   \"" << (a) << "\"\n"; \
        std::cerr << "    at line " << __LINE__ << "\n"; \
        g_current_test_failed = true; \
        return; \
    }

#define ASSERT_STR_CONTAINS(haystack, needle) \
    if (std::string(haystack).find(needle) == std::string::npos) { \
        std::cerr << "  FAIL: expected '" << (needle) << "' in string\n"; \
        std::cerr << "    at line " << __LINE__ << "\n"; \
        g_current_test_failed = true; \
        return; \
    }

void run_test(const char* name, void (*test)());

// ROM globals — set in main(), read by all TUs
extern const crystal::RomData* g_rom;
extern const crystal::ExtractionProfile* g_profile;
extern std::string g_generated_lua;

// =============================================================================
// JOHTO COLLISION HELPERS
// Used by runtime_test_core.cpp and runtime_test_maps.cpp.
// Requires: engine/world/collision.hpp (CollisionClass + collision_is_*)
// =============================================================================

// Johto tileset collision table (128 metatiles, 4 collision bytes each = 512 bytes)
// From pokecrystal/data/tilesets/johto_collision.asm
// Each metatile has 4 collision values: TL, TR, BL, BR (for 2x2 tiles)
static const std::array<std::array<uint8_t, 4>, 128> JOHTO_COLLISION_TABLE = {{
    // 0x00-0x0F
    {{0x01, 0x01, 0x01, 0x01}}, // 00
    {{0x00, 0x00, 0x00, 0x00}}, // 01 FLOOR
    {{0x00, 0x00, 0x00, 0x00}}, // 02 FLOOR
    {{0x18, 0x18, 0x18, 0x18}}, // 03 TALL_GRASS
    {{0x00, 0x00, 0x00, 0x00}}, // 04 FLOOR
    {{0x07, 0x07, 0x07, 0x07}}, // 05 WALL
    {{0x72, 0x72, 0x72, 0x72}}, // 06 LADDER
    {{0x24, 0x27, 0x29, 0x27}}, // 07 WHIRLPOOL, BUOY, WATER, BUOY
    {{0x07, 0x07, 0x07, 0x07}}, // 08 WALL
    {{0x07, 0x07, 0x07, 0x07}}, // 09 WALL
    {{0x07, 0x07, 0x07, 0x07}}, // 0A WALL
    {{0x76, 0x00, 0x76, 0x00}}, // 0B WARP_CARPET_LEFT, FLOOR...
    {{0x00, 0x00, 0x07, 0x70}}, // 0C FLOOR, FLOOR, WALL, WARP_CARPET_DOWN
    {{0x00, 0x00, 0x70, 0x07}}, // 0D FLOOR, FLOOR, WARP_CARPET_DOWN, WALL
    {{0x00, 0x7E, 0x00, 0x7E}}, // 0E FLOOR, WARP_CARPET_RIGHT...
    {{0x07, 0x07, 0x07, 0x07}}, // 0F WALL
    // 0x10-0x1F (building roofs, walls)
    {{0x07, 0x07, 0x07, 0x07}}, // 10 WALL
    {{0x07, 0x07, 0x07, 0x07}}, // 11 WALL
    {{0x07, 0x07, 0x07, 0x07}}, // 12 WALL
    {{0x07, 0x07, 0x07, 0x07}}, // 13 WALL
    {{0x07, 0x07, 0x07, 0x71}}, // 14 WALL, WALL, WALL, DOOR
    {{0x07, 0x07, 0x07, 0x07}}, // 15 WALL
    {{0x07, 0x07, 0x07, 0x71}}, // 16 WALL, WALL, WALL, DOOR
    {{0x07, 0x07, 0x07, 0x07}}, // 17 WALL
    {{0x07, 0x07, 0x07, 0x07}}, // 18 WALL
    {{0x07, 0x07, 0x07, 0x07}}, // 19 WALL
    {{0x07, 0x07, 0x07, 0x71}}, // 1A WALL, WALL, WALL, DOOR
    {{0x07, 0x07, 0x07, 0x07}}, // 1B WALL
    {{0x07, 0x07, 0x07, 0x07}}, // 1C WALL
    {{0x07, 0x07, 0x71, 0x07}}, // 1D WALL, WALL, DOOR, WALL
    {{0x07, 0x07, 0x07, 0x07}}, // 1E WALL
    {{0x07, 0x07, 0x07, 0x07}}, // 1F WALL
    // 0x20-0x2F
    {{0x07, 0x07, 0x07, 0x07}}, // 20 WALL
    {{0x07, 0x07, 0x07, 0x07}}, // 21 WALL
    {{0x07, 0x07, 0x07, 0x07}}, // 22 WALL
    {{0x07, 0x07, 0x07, 0x07}}, // 23 WALL
    {{0x07, 0x07, 0x07, 0x07}}, // 24 WALL
    {{0x07, 0x07, 0x07, 0x07}}, // 25 WALL
    {{0x07, 0x07, 0x07, 0x07}}, // 26 WALL
    {{0x07, 0x07, 0x71, 0x07}}, // 27 WALL, WALL, DOOR, WALL
    {{0x07, 0x07, 0x07, 0x71}}, // 28 WALL, WALL, WALL, DOOR
    {{0x07, 0x07, 0x07, 0x07}}, // 29 WALL
    {{0x15, 0x15, 0x07, 0x07}}, // 2A HEADBUTT_TREE...
    {{0x07, 0x07, 0x07, 0x07}}, // 2B WALL
    {{0x15, 0x15, 0x07, 0x07}}, // 2C HEADBUTT_TREE...
    {{0x15, 0x15, 0x07, 0x07}}, // 2D HEADBUTT_TREE...
    {{0x07, 0x07, 0x07, 0x71}}, // 2E WALL, WALL, WALL, DOOR
    {{0x07, 0x07, 0x07, 0x07}}, // 2F WALL
    // 0x30-0x3F (water/buoy tiles)
    {{0x27, 0x27, 0x27, 0x29}}, // 30 BUOY, BUOY, BUOY, WATER
    {{0x27, 0x27, 0x29, 0x29}}, // 31 BUOY, BUOY, WATER, WATER
    {{0x27, 0x27, 0x29, 0x27}}, // 32 BUOY, BUOY, WATER, BUOY
    {{0x00, 0x00, 0x07, 0x07}}, // 33 FLOOR, FLOOR, WALL, WALL
    {{0x27, 0x29, 0x27, 0x29}}, // 34 BUOY, WATER, BUOY, WATER
    {{0x29, 0x29, 0x29, 0x29}}, // 35 WATER
    {{0x29, 0x27, 0x29, 0x27}}, // 36 WATER, BUOY, WATER, BUOY
    {{0x07, 0x07, 0x07, 0x71}}, // 37 WALL, WALL, WALL, DOOR
    {{0x27, 0x29, 0x27, 0x27}}, // 38 BUOY, WATER...
    {{0x29, 0x29, 0x27, 0x27}}, // 39 WATER, WATER, BUOY, BUOY
    {{0x29, 0x27, 0x27, 0x27}}, // 3A WATER, BUOY...
    {{0x07, 0x07, 0x07, 0x07}}, // 3B WALL
    {{0x15, 0x00, 0x00, 0x00}}, // 3C HEADBUTT_TREE, FLOOR...
    {{0x00, 0x15, 0x00, 0x00}}, // 3D FLOOR, HEADBUTT_TREE...
    {{0x00, 0x00, 0x15, 0x00}}, // 3E FLOOR, FLOOR, HEADBUTT_TREE, FLOOR
    {{0x00, 0x00, 0x00, 0x15}}, // 3F FLOOR, FLOOR, FLOOR, HEADBUTT_TREE
    // 0x40-0x4F
    {{0x07, 0x07, 0x07, 0x00}}, // 40 WALL, WALL, WALL, FLOOR
    {{0x07, 0x07, 0x00, 0x00}}, // 41 WALL, WALL, FLOOR, FLOOR
    {{0x07, 0x07, 0x00, 0x07}}, // 42 WALL, WALL, FLOOR, WALL
    {{0x29, 0x29, 0x29, 0x29}}, // 43 WATER
    {{0x07, 0x00, 0x07, 0x00}}, // 44 WALL, FLOOR, WALL, FLOOR
    {{0x07, 0x00, 0x00, 0x00}}, // 45 WALL, FLOOR, FLOOR, FLOOR
    {{0x00, 0x07, 0x00, 0x07}}, // 46 FLOOR, WALL, FLOOR, WALL
    {{0x00, 0x00, 0x00, 0x07}}, // 47 FLOOR, FLOOR, FLOOR, WALL
    {{0x07, 0x00, 0x07, 0x07}}, // 48 WALL, FLOOR, WALL, WALL
    {{0x00, 0x00, 0x07, 0x07}}, // 49 FLOOR, FLOOR, WALL, WALL
    {{0x00, 0x07, 0x07, 0x07}}, // 4A FLOOR, WALL, WALL, WALL
    {{0xA3, 0x00, 0x07, 0x00}}, // 4B HOP_DOWN, FLOOR, WALL, FLOOR
    {{0x07, 0xA1, 0x07, 0xA1}}, // 4C WALL, HOP_LEFT...
    {{0xA0, 0x07, 0xA0, 0x07}}, // 4D HOP_RIGHT, WALL...
    {{0x07, 0xA1, 0x07, 0xA1}}, // 4E WALL, HOP_LEFT...
    {{0xA0, 0x07, 0xA0, 0x07}}, // 4F HOP_RIGHT, WALL...
    // 0x50-0x5F
    {{0x07, 0xA5, 0x07, 0x07}}, // 50 WALL, HOP_DOWN_LEFT...
    {{0xA4, 0x07, 0x07, 0x07}}, // 51 HOP_DOWN_RIGHT...
    {{0x07, 0xA5, 0x07, 0x07}}, // 52 WALL, HOP_DOWN_LEFT...
    {{0xA4, 0x07, 0x07, 0x07}}, // 53 HOP_DOWN_RIGHT...
    {{0x29, 0x29, 0x29, 0x29}}, // 54 WATER
    {{0x29, 0x29, 0x29, 0x29}}, // 55 WATER
    {{0xA3, 0xA3, 0x07, 0x07}}, // 56 HOP_DOWN, HOP_DOWN, WALL, WALL
    {{0xA3, 0xA3, 0x07, 0x07}}, // 57 HOP_DOWN, HOP_DOWN, WALL, WALL
    {{0x29, 0x29, 0x29, 0x29}}, // 58 WATER
    {{0x29, 0x29, 0x29, 0x29}}, // 59 WATER
    {{0xA3, 0x00, 0x07, 0x00}}, // 5A HOP_DOWN, FLOOR, WALL, FLOOR
    {{0x15, 0x12, 0x00, 0x00}}, // 5B HEADBUTT_TREE, CUT_TREE...
    {{0x15, 0x15, 0x15, 0x00}}, // 5C HEADBUTT_TREE...
    {{0x15, 0x15, 0x00, 0x00}}, // 5D HEADBUTT_TREE...
    {{0x15, 0x15, 0x00, 0x15}}, // 5E HEADBUTT_TREE...
    {{0x00, 0x15, 0x00, 0x12}}, // 5F FLOOR, HEADBUTT_TREE, FLOOR, CUT_TREE
    // 0x60-0x6F
    {{0x15, 0x00, 0x15, 0x00}}, // 60 HEADBUTT_TREE, FLOOR...
    {{0x15, 0x15, 0x15, 0x15}}, // 61 HEADBUTT_TREE all
    {{0x00, 0x15, 0x00, 0x15}}, // 62 FLOOR, HEADBUTT_TREE...
    {{0x00, 0x00, 0x12, 0x15}}, // 63 FLOOR, FLOOR, CUT_TREE, HEADBUTT_TREE
    {{0x15, 0x00, 0x15, 0x15}}, // 64 HEADBUTT_TREE...
    {{0x00, 0x00, 0x15, 0x15}}, // 65 FLOOR, FLOOR, HEADBUTT_TREE...
    {{0x00, 0x15, 0x15, 0x15}}, // 66 FLOOR, HEADBUTT_TREE...
    {{0x12, 0x00, 0x15, 0x00}}, // 67 CUT_TREE, FLOOR, HEADBUTT_TREE, FLOOR
    {{0x07, 0x00, 0x07, 0x00}}, // 68 WALL, FLOOR, WALL, FLOOR
    {{0x00, 0x07, 0x00, 0x07}}, // 69 FLOOR, WALL, FLOOR, WALL
    {{0x07, 0xB2, 0x07, 0x00}}, // 6A WALL, UP_WALL, WALL, FLOOR
    {{0xB2, 0x07, 0x00, 0x07}}, // 6B UP_WALL, WALL, FLOOR, WALL
    {{0x07, 0x00, 0x07, 0x07}}, // 6C WALL, FLOOR, WALL, WALL
    {{0x00, 0x07, 0x07, 0x07}}, // 6D FLOOR, WALL, WALL, WALL
    {{0x00, 0x00, 0x07, 0x00}}, // 6E FLOOR, FLOOR, WALL, FLOOR
    {{0x00, 0x00, 0x00, 0x07}}, // 6F FLOOR, FLOOR, FLOOR, WALL
    // 0x70-0x7F
    {{0xB2, 0xB2, 0x00, 0x00}}, // 70 UP_WALL, UP_WALL, FLOOR, FLOOR
    {{0x00, 0x00, 0x00, 0x00}}, // 71 FLOOR (grass/path)
    {{0x00, 0x00, 0x07, 0x07}}, // 72 FLOOR, FLOOR, WALL, WALL
    {{0x00, 0x00, 0x7B, 0x07}}, // 73 FLOOR, FLOOR, CAVE, WALL
    {{0x07, 0x00, 0x00, 0x00}}, // 74 WALL, FLOOR, FLOOR, FLOOR
    {{0x07, 0x07, 0x00, 0x00}}, // 75 WALL, WALL, FLOOR, FLOOR
    {{0x29, 0x29, 0x29, 0x29}}, // 76 WATER
    {{0x07, 0x07, 0x71, 0x07}}, // 77 WALL, WALL, DOOR, WALL
    {{0x00, 0x00, 0x00, 0x07}}, // 78 FLOOR, FLOOR, FLOOR, WALL
    {{0x29, 0x29, 0x29, 0x29}}, // 79 WATER
    {{0x29, 0x29, 0x29, 0x29}}, // 7A WATER
    {{0x07, 0x07, 0x07, 0x07}}, // 7B WALL
    {{0x07, 0x07, 0x07, 0x07}}, // 7C WALL
    {{0x07, 0x07, 0x07, 0x07}}, // 7D WALL
    {{0x07, 0x07, 0x07, 0x07}}, // 7E WALL
    {{0x07, 0x07, 0x07, 0x07}}, // 7F WALL
}};

// Flatten JOHTO_COLLISION_TABLE into per-quadrant vector format
inline std::vector<uint8_t> make_flat_collision_table() {
    std::vector<uint8_t> flat;
    flat.reserve(128 * 4);
    for (size_t i = 0; i < 128; ++i)
        for (int q = 0; q < 4; ++q)
            flat.push_back(JOHTO_COLLISION_TABLE[i][q]);
    return flat;
}

// Get raw collision byte at tile position (per-tileset collision data, 4 bytes per metatile)
inline uint8_t get_collision_from_blocks(
    const std::vector<uint8_t>& blocks,
    const std::vector<uint8_t>& collision,
    int map_width_blocks,
    int tile_x, int tile_y
) {
    int block_x = tile_x / 2;
    int block_y = tile_y / 2;
    int quad_idx = (tile_x % 2) + (tile_y % 2) * 2;
    if (block_x < 0 || block_y < 0 ||
        block_x >= map_width_blocks ||
        block_y >= static_cast<int>(blocks.size()) / map_width_blocks)
        return 0xFF;
    uint8_t metatile = blocks[block_y * map_width_blocks + block_x];
    if (metatile == 0) return 0xFF;
    size_t coll_idx = static_cast<size_t>(metatile) * 4 + quad_idx;
    if (coll_idx >= collision.size()) return 0xFF;
    return collision[coll_idx];
}

// Backward-compatible version using hardcoded JOHTO_COLLISION_TABLE
inline uint8_t get_collision_from_blocks_johto_raw(
    const std::vector<uint8_t>& blocks,
    int map_width_blocks,
    int tile_x, int tile_y
) {
    static const std::vector<uint8_t> flat_johto = make_flat_collision_table();
    return get_collision_from_blocks(blocks, flat_johto, map_width_blocks, tile_x, tile_y);
}

// Maps raw Johto collision bytes to semantic CollisionClass (test-only)
inline CollisionClass classify_raw_johto_collision(uint8_t raw_byte) {
    if (raw_byte == 0xFF || raw_byte == 0x07) return CollisionClass::Wall;
    uint8_t hi = raw_byte & 0xF0;
    if (hi == 0x20 || hi == 0x30) {
        if (raw_byte == 0x23 || raw_byte == 0x2B) return CollisionClass::Ice;
        if (raw_byte == 0x24 || raw_byte == 0x2C) return CollisionClass::Whirlpool;
        if (raw_byte == 0x33) return CollisionClass::Waterfall;
        return CollisionClass::Water;
    }
    if (hi == 0x70) {
        if (raw_byte == 0x71 || raw_byte == 0x75 || raw_byte == 0x79 || raw_byte == 0x7D)
            return CollisionClass::WarpDoor;
        if (raw_byte == 0x7B || raw_byte == 0x74) return CollisionClass::WarpCave;
        if (raw_byte == 0x72 || raw_byte == 0x7A) return CollisionClass::WarpStair;
        if (raw_byte == 0x70 || raw_byte == 0x76 || raw_byte == 0x78 || raw_byte == 0x7E)
            return CollisionClass::WarpCarpet;
        return CollisionClass::WarpFloor;
    }
    if (raw_byte == 0x60 || raw_byte == 0x68) return CollisionClass::WarpPit;
    if (hi == 0x90) return CollisionClass::Counter;
    if (raw_byte == 0x18 || raw_byte == 0x14) return CollisionClass::Grass;
    if (hi == 0xB0) {
        switch (raw_byte & 0x07) {
            case 2: return CollisionClass::SideWallN;
            case 3: return CollisionClass::SideWallS;
            case 1: return CollisionClass::SideWallE;
            case 0: return CollisionClass::SideWallW;
        }
    }
    return CollisionClass::Floor;
}

// Semantic collision lookup using Johto table (test-only)
inline CollisionClass get_collision_from_blocks_johto(
    const std::vector<uint8_t>& blocks,
    int map_width_blocks,
    int tile_x, int tile_y
) {
    return classify_raw_johto_collision(
        get_collision_from_blocks_johto_raw(blocks, map_width_blocks, tile_x, tile_y));
}


// =============================================================================
