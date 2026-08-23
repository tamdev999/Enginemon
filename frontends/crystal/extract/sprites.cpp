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
#include "crystal/extract/sprite_ids.hpp"
#include <format>

namespace crystal {

//=============================================================================
// SPRITE ID MAPPING - Uses shared authoritative mapping
//=============================================================================

std::string SpriteExtractor::make_sprite_id(uint8_t index) {
    std::string id = crystal_sprite_index_to_id(index);
    if (!id.empty()) {
        return id;
    }
    // Invalid index - return empty to signal failure
    // (Caller should check crystal_sprite_index_valid() before calling)
    return "";
}

uint8_t SpriteExtractor::sprite_id_to_index(const std::string& id) {
    return crystal_sprite_id_to_index(id);
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

//=============================================================================
// POKÉMON ICON EXTRACTION
// Source: pokecrystal/engine/gfx/mon_icons.asm, data/icon_pointers.asm,
//         gfx/icons.asm, constants/icon_constants.asm
//
// Crystal icon rendering path:
//   species → MonMenuIcons[species-1] → ICON_* type → IconPointers[type] →
//   GFX address in bank 0x17 (23) → raw 2bpp data
//
// In Enginemon, the icon_type_name is the stable semantic identity
// (e.g., "pikachu" for ICON_PIKACHU = 4).
// The package key is "pokemon_icon:<icon_type_name>".
//
// Icon format: 32×32 pixels = 16 tiles (4×4 grid), 2 animation frames.
//   Total GFX: 2 frames × 16 tiles × 16 bytes = 512 bytes per icon.
//   Tile assembly: row-major 4×4 grid of 8×8 tiles.
//     Frame 0: tiles 0–15, frame 1: tiles 16–31.
//=============================================================================

// Icon type name → index mapping (ICON_* constants 0-38).
// Source: pokecrystal/constants/icon_constants.asm
static const char* ICON_TYPE_NAMES[] = {
    "null",         // 0  ICON_NULL
    "poliwag",      // 1  ICON_POLIWAG
    "jigglypuff",   // 2  ICON_JIGGLYPUFF
    "diglett",      // 3  ICON_DIGLETT
    "pikachu",      // 4  ICON_PIKACHU
    "staryu",       // 5  ICON_STARYU
    "fish",         // 6  ICON_FISH
    "bird",         // 7  ICON_BIRD
    "monster",      // 8  ICON_MONSTER
    "clefairy",     // 9  ICON_CLEFAIRY
    "oddish",       // 10 ICON_ODDISH
    "bug",          // 11 ICON_BUG
    "ghost",        // 12 ICON_GHOST
    "lapras",       // 13 ICON_LAPRAS
    "humanshape",   // 14 ICON_HUMANSHAPE
    "fox",          // 15 ICON_FOX
    "equine",       // 16 ICON_EQUINE
    "shell",        // 17 ICON_SHELL
    "blob",         // 18 ICON_BLOB
    "serpent",      // 19 ICON_SERPENT
    "voltorb",      // 20 ICON_VOLTORB
    "squirtle",     // 21 ICON_SQUIRTLE
    "bulbasaur",    // 22 ICON_BULBASAUR
    "charmander",   // 23 ICON_CHARMANDER
    "caterpillar",  // 24 ICON_CATERPILLAR
    "unown",        // 25 ICON_UNOWN
    "geodude",      // 26 ICON_GEODUDE
    "fighter",      // 27 ICON_FIGHTER
    "egg",          // 28 ICON_EGG
    "jellyfish",    // 29 ICON_JELLYFISH
    "moth",         // 30 ICON_MOTH
    "bat",          // 31 ICON_BAT
    "snorlax",      // 32 ICON_SNORLAX
    "ho_oh",        // 33 ICON_HO_OH
    "lugia",        // 34 ICON_LUGIA
    "gyarados",     // 35 ICON_GYARADOS
    "slowpoke",     // 36 ICON_SLOWPOKE
    "sudowoodo",    // 37 ICON_SUDOWOODO
    "bigmon",       // 38 ICON_BIGMON
};
constexpr uint8_t NUM_ICON_TYPES = 39;  // 0 through 38

// Decode a single 8×8 2bpp tile to 64 indexed pixels.
// Reuses the existing decode_tile logic but as a standalone function for icons.
static void decode_icon_tile(const uint8_t* data, uint8_t* pixels) {
    for (int row = 0; row < 8; ++row) {
        uint8_t low  = data[row * 2];
        uint8_t high = data[row * 2 + 1];
        for (int col = 0; col < 8; ++col) {
            int bit = 7 - col;
            pixels[row * 8 + col] =
                static_cast<uint8_t>(((low >> bit) & 1) | (((high >> bit) & 1) << 1));
        }
    }
}

// Assemble 16 tiles (4 columns × 4 rows of 8×8) into a 32×32 IconFrame.
// Tile order: row 0 = tiles 0,1,2,3; row 1 = tiles 4,5,6,7; etc.
static enginemon::IconFrame compile_icon_frame(const uint8_t* tile_data) {
    enginemon::IconFrame frame;
    uint8_t tile_pixels[16][64];  // 16 tiles × 64 pixels each

    for (int t = 0; t < 16; ++t) {
        decode_icon_tile(&tile_data[t * 16], tile_pixels[t]);
    }

    for (int y = 0; y < 32; ++y) {
        for (int x = 0; x < 32; ++x) {
            int tile_col = x / 8;   // 0-3
            int tile_row = y / 8;   // 0-3
            int tile_idx = tile_row * 4 + tile_col;
            int local_x  = x % 8;
            int local_y  = y % 8;
            frame.pixels[y * 32 + x] = tile_pixels[tile_idx][local_y * 8 + local_x];
        }
    }
    return frame;
}

SpriteExtractionResult SpriteExtractor::extract_pokemon_icon(
    const std::string& icon_type_name) const
{
    SpriteExtractionResult result;

    // Resolve icon_type_name → icon_type index (0-38)
    uint8_t icon_type = 0;
    bool found = false;
    for (uint8_t i = 0; i < NUM_ICON_TYPES; ++i) {
        if (icon_type_name == ICON_TYPE_NAMES[i]) {
            icon_type = i;
            found = true;
            break;
        }
    }
    if (!found || icon_type == 0 /* ICON_NULL */) {
        result.error = "Unknown icon type name: " + icon_type_name;
        return result;
    }

    // IconPointers table: bank 23 (0x17), address 0x6bbf
    // Format: 39 × dw (2 bytes little-endian), 0-indexed by icon_type
    // Source: pokecrystal/data/icon_pointers.asm, sym: 23:6bbf
    constexpr uint8_t  ICON_BANK        = 0x17;   // bank 23
    constexpr uint16_t ICON_PTRS_ADDR   = 0x6bbf; // IconPointers
    constexpr size_t   ICON_PTR_ENTRY   = 2;       // 2 bytes per dw entry

    uint32_t ptrs_addr = rom_.bank_to_flat(ICON_BANK, ICON_PTRS_ADDR);
    uint32_t entry_off = ptrs_addr + icon_type * ICON_PTR_ENTRY;

    if (entry_off + ICON_PTR_ENTRY > rom_.size()) {
        result.error = std::format("IconPointers entry out of bounds (icon_type={})", icon_type);
        stats_.bounds_check_failures++;
        return result;
    }

    auto ptr_bytes = rom_.read_bytes(entry_off, ICON_PTR_ENTRY);
    uint16_t gfx_ptr = ptr_bytes[0] | (ptr_bytes[1] << 8);

    // GFX data is in bank 23 at the address given by IconPointers.
    // Each icon: 2 frames × 16 tiles × 16 bytes/tile = 512 bytes raw.
    // Source: gfx/icons.asm — all files are raw uncompressed .2bpp
    uint32_t gfx_addr = rom_.bank_to_flat(ICON_BANK, gfx_ptr);
    constexpr size_t ICON_GFX_SIZE = 512;  // 2 frames × 16 tiles × 16 bytes

    if (gfx_addr + ICON_GFX_SIZE > rom_.size()) {
        result.error = std::format(
            "Icon GFX out of bounds (icon_type={}, gfx_addr=0x{:06X})",
            icon_type, gfx_addr);
        stats_.bounds_check_failures++;
        return result;
    }

    auto gfx_data = rom_.read_bytes(gfx_addr, ICON_GFX_SIZE);

    RuntimeSprite& sprite = result.sprite;
    sprite.sprite_id       = std::string("pokemon_icon:") + icon_type_name;
    sprite.type            = SpriteType::Icon;
    sprite.default_palette = SpritePalette::Red;  // OBJ palette 0 (icons use first OBJ pal)

    // Assemble 2 animation frames, each 16 tiles (32×32 pixels)
    constexpr size_t TILES_PER_FRAME = 16;
    constexpr size_t BYTES_PER_FRAME = TILES_PER_FRAME * 16;  // 256 bytes of raw tile data
    sprite.icon_frames.resize(2);
    sprite.icon_frames[0] = compile_icon_frame(gfx_data.data());
    sprite.icon_frames[1] = compile_icon_frame(gfx_data.data() + BYTES_PER_FRAME);

    stats_.sprites_extracted++;
    result.success = true;
    return result;
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
