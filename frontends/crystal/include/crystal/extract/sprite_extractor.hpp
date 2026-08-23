#pragma once
// crystal/extract/sprite_extractor.hpp
// Overworld sprite extraction from Crystal ROM
//
// Compiles Crystal ROM sprite data into semantic RuntimeSprite format.
// The output contains no Crystal-specific concepts (VRAM banking, OAM tile
// addressing, $80 offsets) - only semantic frames and flip metadata.
//
// Reference: pokecrystal data/sprites/sprites.asm (OverworldSprites table)
// Reference: pokecrystal data/sprites/facings.asm (tile layout traced)
// Reference: Gen2Recomped src/import/RomExtractorGen2.lua (24-tile confirmation)

#include "crystal/rom/loader.hpp"
#include "crystal/rom/profile.hpp"
#include "engine/world/sprite_atlas.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace crystal {

using namespace enginemon;

//=============================================================================
// EXTRACTION RESULT
//=============================================================================

struct SpriteExtractionResult {
    bool success = false;
    std::string error;
    RuntimeSprite sprite;
};

struct SpritePaletteExtractionResult {
    bool success = false;
    std::string error;
    SpriteObjPalettes palettes;
};

//=============================================================================
// EXTRACTOR
//=============================================================================

class SpriteExtractor {
public:
    SpriteExtractor(const RomData& rom, const ExtractionProfile& profile);
    
    // Extract single sprite by index (1-indexed, matching SPRITE_* constants)
    SpriteExtractionResult extract_sprite(uint8_t sprite_index) const;
    
    // Extract sprite by semantic ID
    SpriteExtractionResult extract_sprite(const std::string& sprite_id) const;
    
    // Extract all sprites needed for a map's objects
    std::vector<RuntimeSprite> extract_sprites_for_map(const std::vector<std::string>& sprite_ids) const;
    
    // Extract player sprite
    SpriteExtractionResult extract_player_sprite(bool is_female = false) const;

    // Extract a Pokémon overworld icon sprite.
    // icon_type_name: semantic icon name (e.g., "pikachu", "clefairy", "snorlax")
    //   matching the ICON_* constants from icon_constants.asm.
    // Returns a RuntimeSprite with SpriteType::Icon and 2 IconFrames (32×32 each).
    // Source: Crystal MonMenuIcons / IconPointers / Icons GFX (bank 23)
    SpriteExtractionResult extract_pokemon_icon(const std::string& icon_type_name) const;
    
    // Extract OBJ palettes
    SpritePaletteExtractionResult extract_obj_palettes() const;
    
    // Stats
    struct Stats {
        uint32_t sprites_extracted = 0;
        uint32_t frames_compiled = 0;
        uint32_t bounds_check_failures = 0;
    };
    const Stats& stats() const { return stats_; }

private:
    const RomData& rom_;
    const ExtractionProfile& profile_;
    mutable Stats stats_;
    
    // Sprite ID mapping
    static std::string make_sprite_id(uint8_t index);
    static uint8_t sprite_id_to_index(const std::string& id);
    
    // Decode a single 8x8 2bpp tile to pixels
    void decode_tile(const uint8_t* data, uint8_t* pixels) const;
    
    // Compile a 16x16 frame from 4 consecutive tiles (2x2 layout)
    // tiles are arranged: [0][1] top row, [2][3] bottom row
    SpriteFrame compile_frame(const uint8_t* tile_data) const;
};

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

} // namespace crystal
