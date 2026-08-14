// crystal/extract/sprites.cpp
// Sprite extraction from Crystal ROM - compiles to semantic format
//
// Traced from pokecrystal:
// - OverworldSprites table at 14:401A (6 bytes per entry)
// - Walking sprites: 24 tiles (384 bytes) = 6 frames × 4 tiles each
// - Standing sprites: 24 tiles (same layout, walk frames unused by engine)
// - Still sprites: 4 tiles (64 bytes) = 1 frame × 4 tiles
//
// Output is semantic: the runtime knows nothing about Crystal VRAM or OAM.

#include "crystal/extract/sprite_extractor.hpp"
#include <format>

namespace crystal {

//=============================================================================
// SPRITE ID MAPPING
//=============================================================================

// Maps sprite index to semantic ID
// Matches SPRITE_* constants from sprite_constants.asm
static const char* SPRITE_NAMES[] = {
    nullptr,            // 0 = SPRITE_NONE
    "chris",            // 1
    "chris_bike",       // 2
    "gameboy_kid",      // 3
    "rival",            // 4
    "oak",              // 5
    "red",              // 6
    "blue",             // 7
    "bill",             // 8
    "elder",            // 9
    "janine",           // 10
    "kurt",             // 11
    "mom",              // 12
    "blaine",           // 13
    "reds_mom",         // 14
    "daisy",            // 15
    "elm",              // 16
    "will",             // 17
    "falkner",          // 18
    "whitney",          // 19
    "bugsy",            // 20
    "morty",            // 21
    "chuck",            // 22
    "jasmine",          // 23
    "pryce",            // 24
    "clair",            // 25
    "brock",            // 26
    "karen",            // 27
    "bruno",            // 28
    "misty",            // 29
    "lance",            // 30
    "surge",            // 31
    "erika",            // 32
    "koga",             // 33
    "sabrina",          // 34
    "cooltrainer_m",    // 35
    "cooltrainer_f",    // 36
    "bug_catcher",      // 37
    "twin",             // 38
    "youngster",        // 39
    "lass",             // 40
    "teacher",          // 41
    "beauty",           // 42
    "super_nerd",       // 43
    "rocker",           // 44
    "pokefan_m",        // 45
    "pokefan_f",        // 46
    "gramps",           // 47
    "granny",           // 48
    "swimmer_guy",      // 49
    "swimmer_girl",     // 50
    "big_snorlax",      // 51
    "surfing_pikachu",  // 52
    "rocket",           // 53
    "rocket_girl",      // 54
    "nurse",            // 55
    "link_receptionist", // 56
    "clerk",            // 57
    "fisher",           // 58
    "fishing_guru",     // 59
    "scientist",        // 60
    "kimono_girl",      // 61
    "sage",             // 62
    "unused_guy",       // 63
    "gentleman",        // 64
    "black_belt",       // 65
    "receptionist",     // 66
    "officer",          // 67
    "cal",              // 68
    "slowpoke",         // 69
    "captain",          // 70
    "big_lapras",       // 71
    "gym_guide",        // 72
    "sailor",           // 73
    "biker",            // 74
    "pharmacist",       // 75
    "monster",          // 76
    "fairy",            // 77
    "bird",             // 78
    "dragon",           // 79
    "big_onix",         // 80
    "n64",              // 81
    "sudowoodo",        // 82
    "surf",             // 83
    "poke_ball",        // 84
    "pokedex",          // 85
    "paper",            // 86
    "virtual_boy",      // 87
    "old_link_receptionist", // 88
    "rock",             // 89
    "boulder",          // 90
    "snes",             // 91
    "famicom",          // 92
    "fruit_tree",       // 93
    "gold_trophy",      // 94
    "silver_trophy",    // 95
    "kris",             // 96
    "kris_bike",        // 97
    "kurt_outside",     // 98
    "suicune",          // 99
    "entei",            // 100
    "raikou",           // 101
    "standing_youngster", // 102
};
constexpr size_t NUM_SPRITE_NAMES = sizeof(SPRITE_NAMES) / sizeof(SPRITE_NAMES[0]);

std::string SpriteExtractor::make_sprite_id(uint8_t index) {
    if (index < NUM_SPRITE_NAMES && SPRITE_NAMES[index]) {
        return SPRITE_NAMES[index];
    }
    return std::format("sprite_{:02d}", index);
}

uint8_t SpriteExtractor::sprite_id_to_index(const std::string& id) {
    for (size_t i = 0; i < NUM_SPRITE_NAMES; ++i) {
        if (SPRITE_NAMES[i] && id == SPRITE_NAMES[i]) {
            return static_cast<uint8_t>(i);
        }
    }
    return 0;
}

//=============================================================================
// CONSTRUCTION
//=============================================================================

SpriteExtractor::SpriteExtractor(const RomData& rom, const ExtractionProfile& profile)
    : rom_(rom), profile_(profile) {}

//=============================================================================
// 2BPP TILE DECODING
//=============================================================================

void SpriteExtractor::decode_tile(const uint8_t* data, uint8_t* pixels) const {
    // 2bpp: 16 bytes per 8x8 tile
    // Each row: 2 bytes (low plane, high plane)
    for (int row = 0; row < 8; ++row) {
        uint8_t low = data[row * 2];
        uint8_t high = data[row * 2 + 1];
        
        for (int col = 0; col < 8; ++col) {
            int bit = 7 - col;
            uint8_t pixel = ((low >> bit) & 1) | (((high >> bit) & 1) << 1);
            pixels[row * 8 + col] = pixel;
        }
    }
}

//=============================================================================
// FRAME COMPILATION
//=============================================================================

SpriteFrame SpriteExtractor::compile_frame(const uint8_t* tile_data) const {
    SpriteFrame frame;
    
    // 4 tiles arranged as 2x2:
    //   [0][1]  <- top row (y=0-7)
    //   [2][3]  <- bottom row (y=8-15)
    
    uint8_t tile_pixels[4][64];  // 4 tiles, 64 pixels each
    
    for (int t = 0; t < 4; ++t) {
        decode_tile(&tile_data[t * 16], tile_pixels[t]);
    }
    
    // Assemble into 16x16 frame
    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) {
            int tile_idx;
            int local_x, local_y;
            
            if (y < 8) {
                // Top row: tiles 0 and 1
                tile_idx = (x < 8) ? 0 : 1;
                local_y = y;
            } else {
                // Bottom row: tiles 2 and 3
                tile_idx = (x < 8) ? 2 : 3;
                local_y = y - 8;
            }
            local_x = x % 8;
            
            frame.pixels[y * 16 + x] = tile_pixels[tile_idx][local_y * 8 + local_x];
        }
    }
    
    return frame;
}

//=============================================================================
// SPRITE EXTRACTION
//=============================================================================

SpriteExtractionResult SpriteExtractor::extract_sprite(uint8_t sprite_index) const {
    SpriteExtractionResult result;
    
    if (sprite_index == 0 || sprite_index > 102) {
        result.error = std::format("Invalid sprite index: {}", sprite_index);
        return result;
    }
    
    // OverworldSprites table structure (from sprites.asm):
    // dw pointer  (2 bytes) - GFX address
    // db tiles    (1 byte)  - tiles per VRAM bank (12 for walking, 4 for still)
    // db bank     (1 byte)  - ROM bank
    // db type     (1 byte)  - WALKING_SPRITE/STANDING_SPRITE/STILL_SPRITE
    // db palette  (1 byte)  - PAL_OW_*
    // Total: 6 bytes per entry (NUM_SPRITEDATA_FIELDS)
    
    // From pokecrystal symbols: 05:4736 OverworldSprites
    constexpr uint8_t SPRITES_BANK = 0x05;
    constexpr uint16_t SPRITES_ADDR = 0x4736;
    constexpr size_t ENTRY_SIZE = 6;
    
    uint32_t table_addr = rom_.bank_to_flat(SPRITES_BANK, SPRITES_ADDR);
    uint32_t entry_addr = table_addr + (sprite_index - 1) * ENTRY_SIZE;
    
    if (entry_addr + ENTRY_SIZE > rom_.size()) {
        result.error = "Sprite entry out of bounds";
        stats_.bounds_check_failures++;
        return result;
    }
    
    auto entry = rom_.read_bytes(entry_addr, ENTRY_SIZE);
    
    uint16_t gfx_ptr = entry[0] | (entry[1] << 8);
    uint8_t vram_tiles = entry[2];  // Tiles per VRAM bank (12 for walking, 4 for still)
    uint8_t gfx_bank = entry[3];
    uint8_t type_byte = entry[4];
    uint8_t palette = entry[5];
    
    RuntimeSprite& sprite = result.sprite;
    sprite.sprite_id = make_sprite_id(sprite_index);
    sprite.type = static_cast<SpriteType>(type_byte);
    sprite.default_palette = static_cast<SpritePalette>(palette & 0x07);
    
    // Determine actual ROM data size based on sprite type
    // Traced from Gen2Recomped: walking sprites have 24 tiles (384 bytes) in ROM
    // The vram_tiles field (12) is tiles per VRAM bank, not total tiles
    size_t total_tiles;
    size_t semantic_frames;
    
    switch (sprite.type) {
        case SpriteType::Walking:
        case SpriteType::Standing:
            // 24 tiles = 6 frames × 4 tiles each
            // Frame layout in ROM:
            //   Tiles 0-3:   Stand Down
            //   Tiles 4-7:   Stand Up
            //   Tiles 8-11:  Stand Left
            //   Tiles 12-15: Walk Down
            //   Tiles 16-19: Walk Up
            //   Tiles 20-23: Walk Left
            total_tiles = 24;
            semantic_frames = 6;
            break;
            
        case SpriteType::Still:
            // 4 tiles = 1 frame × 4 tiles
            total_tiles = 4;
            semantic_frames = 1;
            break;
            
        default:
            result.error = std::format("Unknown sprite type: {}", type_byte);
            return result;
    }
    
    // Read sprite graphics from ROM (uncompressed 2bpp)
    uint32_t gfx_addr = rom_.bank_to_flat(gfx_bank, gfx_ptr);
    size_t gfx_size = total_tiles * 16;  // 16 bytes per 8x8 tile
    
    if (gfx_addr + gfx_size > rom_.size()) {
        result.error = std::format("Sprite graphics out of bounds (addr={:06X}, size={})", 
                                   gfx_addr, gfx_size);
        stats_.bounds_check_failures++;
        return result;
    }
    
    auto tile_data = rom_.read_bytes(gfx_addr, gfx_size);
    
    // Compile semantic frames
    sprite.frames.resize(semantic_frames);
    
    for (size_t f = 0; f < semantic_frames; ++f) {
        // Each frame is 4 consecutive tiles (64 bytes)
        sprite.frames[f] = compile_frame(&tile_data[f * 64]);
        stats_.frames_compiled++;
    }
    
    stats_.sprites_extracted++;
    result.success = true;
    return result;
}

SpriteExtractionResult SpriteExtractor::extract_sprite(const std::string& sprite_id) const {
    uint8_t index = sprite_id_to_index(sprite_id);
    if (index == 0) {
        SpriteExtractionResult result;
        result.error = "Unknown sprite ID: " + sprite_id;
        return result;
    }
    return extract_sprite(index);
}

SpriteExtractionResult SpriteExtractor::extract_player_sprite(bool is_female) const {
    // SPRITE_CHRIS = 1, SPRITE_KRIS = 96
    return extract_sprite(is_female ? 96 : 1);
}

std::vector<RuntimeSprite> SpriteExtractor::extract_sprites_for_map(
    const std::vector<std::string>& sprite_ids) const 
{
    std::vector<RuntimeSprite> sprites;
    
    for (const auto& id : sprite_ids) {
        auto result = extract_sprite(id);
        if (result.success) {
            sprites.push_back(std::move(result.sprite));
        }
    }
    
    return sprites;
}

//=============================================================================
// OBJ PALETTE EXTRACTION
//=============================================================================

SpritePaletteExtractionResult SpriteExtractor::extract_obj_palettes() const {
    SpritePaletteExtractionResult result;
    
    // From pokecrystal symbols: 02:7469 MapObjectPals
    // Format: 4 time-of-day sets (morn, day, nite, dark)
    // Each set: 8 palettes × 4 colors × 2 bytes = 64 bytes
    constexpr uint8_t OBJ_PAL_BANK = 0x02;
    constexpr uint16_t OBJ_PAL_ADDR = 0x7469;
    constexpr size_t COLORS_PER_PAL = 4;
    constexpr size_t BYTES_PER_COLOR = 2;
    constexpr size_t PALS_PER_SET = 8;
    constexpr size_t SET_SIZE = PALS_PER_SET * COLORS_PER_PAL * BYTES_PER_COLOR;  // 64 bytes
    
    uint32_t base_addr = rom_.bank_to_flat(OBJ_PAL_BANK, OBJ_PAL_ADDR);
    
    // Read 4 time-of-day sets
    for (int tod = 0; tod < 4; ++tod) {
        uint32_t set_addr = base_addr + tod * SET_SIZE;
        
        if (set_addr + SET_SIZE > rom_.size()) {
            result.error = "OBJ palette data out of bounds";
            stats_.bounds_check_failures++;
            return result;
        }
        
        auto pal_data = rom_.read_bytes(set_addr, SET_SIZE);
        
        for (int pal_id = 0; pal_id < 8; ++pal_id) {
            SpriteObjPalette& pal = result.palettes.time_palettes[tod][pal_id];
            
            for (int c = 0; c < 4; ++c) {
                size_t offset = pal_id * COLORS_PER_PAL * BYTES_PER_COLOR + c * BYTES_PER_COLOR;
                uint16_t gbc = pal_data[offset] | (pal_data[offset + 1] << 8);
                pal.colors_gbc[c] = gbc;
            }
        }
    }
    
    result.success = true;
    return result;
}

} // namespace crystal

//=============================================================================
// SpriteObjPalette::to_rgba32 (in enginemon namespace)
//=============================================================================

std::array<uint32_t, 4> enginemon::SpriteObjPalette::to_rgba32() const {
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

namespace crystal {

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

} // namespace crystal
