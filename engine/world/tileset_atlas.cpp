// engine/world/tileset_atlas.cpp
// Runtime tileset atlas implementation
// NOTE: This is legacy code for the old baked-32×32 format
// New code should use runtime_tileset.hpp/cpp

#include "engine/world/tileset_atlas.hpp"
#include <cstring>

namespace enginemon {

// Helper to read little-endian values
template<typename T>
static T read_le(const uint8_t*& ptr) {
    T value = 0;
    for (size_t i = 0; i < sizeof(T); ++i) {
        value |= static_cast<T>(*ptr++) << (i * 8);
    }
    return value;
}

RuntimeTilesetAtlas RuntimeTilesetAtlas::from_package_data(
    const std::string& tileset_id,
    const std::vector<uint8_t>& data
) {
    RuntimeTilesetAtlas atlas;
    atlas.tileset_id = tileset_id;
    
    if (data.size() < 12) {
        return atlas;  // Invalid data
    }
    
    const uint8_t* ptr = data.data();
    const uint8_t* end = ptr + data.size();
    
    // Read header
    atlas.width = read_le<uint32_t>(ptr);
    atlas.height = read_le<uint32_t>(ptr);
    uint32_t pixel_count = read_le<uint32_t>(ptr);
    
    // Validate
    if (pixel_count != atlas.width * atlas.height) {
        return atlas;  // Corrupt
    }
    
    // Read pixels
    size_t pixel_bytes = pixel_count * sizeof(uint32_t);
    if (ptr + pixel_bytes > end) {
        return atlas;  // Truncated
    }
    
    atlas.pixels.resize(pixel_count);
    std::memcpy(atlas.pixels.data(), ptr, pixel_bytes);
    ptr += pixel_bytes;
    
    // Read UV count
    if (ptr + 4 > end) return atlas;
    uint32_t uv_count = read_le<uint32_t>(ptr);
    
    // Read UVs
    size_t uv_bytes = uv_count * 4 * sizeof(float);
    if (ptr + uv_bytes > end) return atlas;
    
    atlas.metatile_uvs.reserve(uv_count);
    for (uint32_t i = 0; i < uv_count; ++i) {
        MetatileUV uv;
        uint32_t u0_bits = read_le<uint32_t>(ptr);
        uint32_t v0_bits = read_le<uint32_t>(ptr);
        uint32_t u1_bits = read_le<uint32_t>(ptr);
        uint32_t v1_bits = read_le<uint32_t>(ptr);
        std::memcpy(&uv.u0, &u0_bits, sizeof(float));
        std::memcpy(&uv.v0, &v0_bits, sizeof(float));
        std::memcpy(&uv.u1, &u1_bits, sizeof(float));
        std::memcpy(&uv.v1, &v1_bits, sizeof(float));
        atlas.metatile_uvs.push_back(uv);
    }
    
    // Read collision count
    if (ptr + 4 > end) return atlas;
    uint32_t coll_count = read_le<uint32_t>(ptr);
    
    // Read collision data
    if (ptr + coll_count > end) return atlas;
    
    atlas.collision.reserve(coll_count);
    for (uint32_t i = 0; i < coll_count; ++i) {
        atlas.collision.push_back(*ptr++);
    }
    
    return atlas;
}

} // namespace enginemon
