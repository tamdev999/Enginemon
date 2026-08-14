#pragma once
// engine/world/tileset_atlas.hpp
// Runtime-native tileset atlas structure
//
// Loaded from packages. Contains pre-rendered RGBA pixel data
// for uploading to GPU textures.

#include <cstdint>
#include <string>
#include <vector>

namespace enginemon {

// Metatile UV coordinates (normalized 0-1)
struct MetatileUV {
    float u0, v0;  // Top-left
    float u1, v1;  // Bottom-right
};

// Collision type (matches Crystal)
enum class CollisionType : uint8_t {
    Walkable = 0x00,
    Solid = 0x01,
    Water = 0x02,
    WaterfallTop = 0x03,
    Grass = 0x04,
    TallGrass = 0x05,
    Ice = 0x06,
    Whirlpool = 0x07,
    Ledge = 0x08,
    Counter = 0x60,
    Door = 0x70,
    Warp = 0x80,
};

// Runtime tileset atlas - loaded from package
struct RuntimeTilesetAtlas {
    std::string tileset_id;
    
    // Atlas dimensions in pixels
    uint32_t width = 0;
    uint32_t height = 0;
    
    // RGBA32 pixel data (row-major)
    std::vector<uint32_t> pixels;
    
    // UV coordinates for each metatile
    std::vector<MetatileUV> metatile_uvs;
    
    // Collision data: 4 bytes per metatile (TL, TR, BL, BR quadrants)
    // Index formula: collision[metatile_index * 4 + quadrant]
    // where quadrant = (cell_x % 2) + (cell_y % 2) * 2
    // Reference: pokecrystal data/tilesets/*_collision.asm, Gen2Recomped Map.lua
    std::vector<uint8_t> collision;
    
    // Parse from raw package data
    static RuntimeTilesetAtlas from_package_data(const std::string& tileset_id,
                                                  const std::vector<uint8_t>& data);
};

} // namespace enginemon
