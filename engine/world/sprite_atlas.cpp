// engine/world/sprite_atlas.cpp
// Sprite atlas rendering implementation
//
// Takes semantic RuntimeSprite data and renders to a GPU-uploadable atlas.
// No Crystal/ROM concepts - purely runtime types.

#include "engine/world/sprite_atlas.hpp"

namespace enginemon {

//=============================================================================
// SpriteObjPalette::to_rgba32
//=============================================================================

std::array<uint32_t, 4> SpriteObjPalette::to_rgba32() const {
    std::array<uint32_t, 4> rgba;
    
    for (int c = 0; c < 4; ++c) {
        uint16_t gbc = colors_gbc[c];
        uint8_t r = (gbc & 0x1F) * 255 / 31;
        uint8_t g = ((gbc >> 5) & 0x1F) * 255 / 31;
        uint8_t b = ((gbc >> 10) & 0x1F) * 255 / 31;
        
        // Color 0 is transparent for OBJ sprites
        uint8_t a = (c == 0) ? 0 : 255;
        
        // VK_FORMAT_R8G8B8A8_UNORM on little-endian
        rgba[c] = r | (g << 8) | (b << 16) | (a << 24);
    }
    
    return rgba;
}

//=============================================================================
// ATLAS RENDERING
//=============================================================================

RuntimeSpriteAtlas render_sprite_atlas(
    const std::vector<RuntimeSprite>& sprites,
    const SpriteObjPalettes& palettes,
    int time_of_day)
{
    RuntimeSpriteAtlas atlas;
    
    if (sprites.empty()) {
        atlas.atlas_width = 16;
        atlas.atlas_height = 16;
        atlas.pixels.resize(256, 0);
        return atlas;
    }
    
    // Calculate total frames
    size_t total_frames = 0;
    for (const auto& sprite : sprites) {
        total_frames += sprite.frame_count();
    }
    
    // Atlas layout: 8 frames per row, each frame is 16×16
    constexpr uint32_t FRAMES_PER_ROW = 8;
    constexpr uint32_t FRAME_SIZE = 16;
    
    uint32_t rows = (total_frames + FRAMES_PER_ROW - 1) / FRAMES_PER_ROW;
    atlas.atlas_width = FRAMES_PER_ROW * FRAME_SIZE;
    atlas.atlas_height = rows * FRAME_SIZE;
    atlas.pixels.resize(atlas.atlas_width * atlas.atlas_height, 0);  // Transparent black
    
    // Render each sprite's frames
    size_t frame_idx = 0;
    for (const auto& sprite : sprites) {
        RuntimeSpriteAtlas::SpriteUVs uvs;
        uvs.sprite_id = sprite.sprite_id;
        uvs.frame_count = sprite.frame_count();
        
        // Get palette for this sprite
        const auto& pal = palettes.time_palettes[time_of_day][static_cast<int>(sprite.default_palette)];
        auto rgba_pal = pal.to_rgba32();
        
        for (int f = 0; f < sprite.frame_count(); ++f) {
            uint32_t fx = (frame_idx % FRAMES_PER_ROW) * FRAME_SIZE;
            uint32_t fy = (frame_idx / FRAMES_PER_ROW) * FRAME_SIZE;
            
            const auto& frame = sprite.frames[f];
            
            // Render frame pixels
            for (int py = 0; py < 16; ++py) {
                for (int px = 0; px < 16; ++px) {
                    uint8_t color_idx = frame.get_pixel(px, py);
                    uint32_t ax = fx + px;
                    uint32_t ay = fy + py;
                    atlas.pixels[ay * atlas.atlas_width + ax] = rgba_pal[color_idx];
                }
            }
            
            // Store UV coordinates
            uvs.frame_uvs[f] = {
                static_cast<float>(fx) / atlas.atlas_width,
                static_cast<float>(fy) / atlas.atlas_height,
                static_cast<float>(fx + FRAME_SIZE) / atlas.atlas_width,
                static_cast<float>(fy + FRAME_SIZE) / atlas.atlas_height
            };
            
            frame_idx++;
        }
        
        atlas.sprite_uvs.push_back(uvs);
    }
    
    return atlas;
}

} // namespace enginemon
