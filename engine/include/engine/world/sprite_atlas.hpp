#pragma once
// engine/world/sprite_atlas.hpp
// Runtime sprite data structures - SEMANTIC MODEL
//
// Sprites are compiled from Crystal ROM data into semantic frames.
// The runtime knows nothing about Crystal's VRAM banking, OAM tile addressing,
// or $80 offsets. It only knows about facing directions and animation states.
//
// Reference: pokecrystal data/sprites/facings.asm (traced, not reproduced)

#include <cstdint>
#include <string>
#include <vector>
#include <array>

namespace enginemon {

// Sprite types from Crystal - preserved semantically
enum class SpriteType : uint8_t {
    Walking = 1,    // 6 semantic frames: stand D/U/L, walk D/U/L; right uses flip
    Standing = 2,   // 6 semantic frames: can turn but doesn't walk
    Still = 3       // 1 semantic frame: static object (items, etc.)
};

// OBJ palette indices from Crystal (semantic, not hardware)
enum class SpritePalette : uint8_t {
    Red = 0,
    Blue = 1,
    Green = 2,
    Brown = 3,
    Pink = 4,
    Emote = 5,
    Tree = 6,
    Rock = 7
};

// Semantic facing direction (no Crystal encoding)
enum class SpriteFacing {
    Down = 0,
    Up = 1,
    Left = 2,
    Right = 3  // Uses left frame with horizontal flip
};

// 4-color OBJ palette (color 0 is always transparent)
struct SpriteObjPalette {
    // Colors 0-3, where color 0 is transparent
    // Stored as RGB 5-5-5
    std::array<uint16_t, 4> colors_gbc;
    
    // Convert to RGBA32 (color 0 has alpha=0)
    std::array<uint32_t, 4> to_rgba32() const;
};

// Semantic sprite frame (16x16 pixels, 2bpp palette indices)
struct SpriteFrame {
    std::array<uint8_t, 256> pixels;  // 16x16, row-major, values 0-3
    
    uint8_t get_pixel(int x, int y) const {
        if (x < 0 || x >= 16 || y < 0 || y >= 16) return 0;
        return pixels[y * 16 + x];
    }
};

// Compiled semantic sprite definition
// Walking/Standing: 6 frames (stand D/U/L, walk D/U/L)
// Still: 1 frame
struct RuntimeSprite {
    std::string sprite_id;          // e.g., "chris", "teacher", "fisher"
    SpriteType type;
    SpritePalette default_palette;
    
    // Semantic frames (compiled from ROM)
    // Walking/Standing: frames[0-5] = stand_down, stand_up, stand_left, walk_down, walk_up, walk_left
    // Still: frames[0] only
    std::vector<SpriteFrame> frames;
    
    // Accessors
    bool is_walking() const { return type == SpriteType::Walking; }
    bool is_standing() const { return type == SpriteType::Standing; }
    bool is_still() const { return type == SpriteType::Still; }
    
    int frame_count() const { return static_cast<int>(frames.size()); }
    
    // Get the semantic frame index and flip flag for a facing/walking state
    // This is the ONLY place that encodes the facing→frame mapping
    struct FrameSelection {
        int frame_index;    // Index into frames[]
        bool flip_x;        // Horizontal flip for rendering
    };
    
    FrameSelection get_frame(SpriteFacing facing, bool walking, bool step_flip) const {
        if (type == SpriteType::Still || frames.empty()) {
            return {0, false};
        }
        
        // Base frame: standing (0-2) or walking (3-5)
        int base = (type == SpriteType::Walking && walking) ? 3 : 0;
        
        switch (facing) {
            case SpriteFacing::Down:
                // Walk step 2 for down uses horizontal flip of walk frame
                return {base + 0, walking && step_flip};
            case SpriteFacing::Up:
                // Walk step 2 for up uses horizontal flip of walk frame
                return {base + 1, walking && step_flip};
            case SpriteFacing::Left:
                return {base + 2, false};
            case SpriteFacing::Right:
                // Right always uses left frame with flip
                return {base + 2, true};
        }
        return {0, false};
    }
};

// Time-of-day OBJ palettes (8 palettes × 4 time variants)
struct SpriteObjPalettes {
    // [time_of_day][palette_id] - time: morn(0)/day(1)/nite(2)/dark(3)
    std::array<std::array<SpriteObjPalette, 8>, 4> time_palettes;
    
    const SpriteObjPalette& get(int time_of_day, SpritePalette pal) const {
        return time_palettes[time_of_day][static_cast<int>(pal)];
    }
};

// Pre-rendered sprite atlas for GPU upload
// All sprites rendered to a single texture with UV coordinates per frame
struct RuntimeSpriteAtlas {
    uint32_t atlas_width = 0;
    uint32_t atlas_height = 0;
    std::vector<uint32_t> pixels;   // RGBA32
    
    // UV coordinates for each sprite's frames
    struct SpriteUVs {
        std::string sprite_id;
        int frame_count;
        // UV for each frame (u0, v0, u1, v1) - up to 6 frames
        std::array<std::array<float, 4>, 6> frame_uvs;
    };
    std::vector<SpriteUVs> sprite_uvs;
    
    // Find UVs for a sprite
    const SpriteUVs* find_sprite(const std::string& sprite_id) const {
        for (const auto& s : sprite_uvs) {
            if (s.sprite_id == sprite_id) return &s;
        }
        return nullptr;
    }
};

// Helper to convert facing string to enum
inline SpriteFacing facing_from_string(const std::string& dir) {
    if (dir == "up") return SpriteFacing::Up;
    if (dir == "left") return SpriteFacing::Left;
    if (dir == "right") return SpriteFacing::Right;
    return SpriteFacing::Down;  // Default
}

inline std::string facing_to_string(SpriteFacing facing) {
    switch (facing) {
        case SpriteFacing::Up: return "up";
        case SpriteFacing::Left: return "left";
        case SpriteFacing::Right: return "right";
        default: return "down";
    }
}

//=============================================================================
// ATLAS RENDERING
//=============================================================================

// Render sprites to atlas texture
// time_of_day: 0=morning, 1=day, 2=night, 3=dark
RuntimeSpriteAtlas render_sprite_atlas(
    const std::vector<RuntimeSprite>& sprites,
    const SpriteObjPalettes& palettes,
    int time_of_day = 1  // Default: day
);

} // namespace enginemon
