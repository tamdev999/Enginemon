// crystal/extract/fonts.cpp
// Crystal font extraction implementation
//
// Extracts glyphs directly from ROM bytes:
//   - Font: 128 1bpp tiles at 3e:4200 (1024 bytes)
//   - FontExtra: 32 2bpp tiles at 3e:4000 (512 bytes)
//
// Reference: pokecrystal/gfx/font/*.png, constants/charmap.asm, suiCune charmap.h

#include "crystal/extract/font_extractor.hpp"
#include "crystal/rom/loader.hpp"
#include "crystal/rom/profile.hpp"

#include <cstring>
#include <algorithm>

namespace crystal {

//=============================================================================
// ROM SYMBOL ADDRESSES FOR CRYSTAL v1.1
// Reference: pokecrystal11.sym
//   Font     = 3e:4200 (128 1bpp tiles = 1024 bytes)
//   FontExtra = 3e:4000 (32 2bpp tiles = 512 bytes)
//=============================================================================

// These should eventually move to ProfileOffsets but are hardcoded for now
static constexpr uint8_t FONT_BANK = 0x3E;
static constexpr uint16_t FONT_ADDR = 0x4200;        // Main font (1bpp)
static constexpr uint16_t FONT_EXTRA_ADDR = 0x4000;  // Extra font with borders (2bpp)

static constexpr size_t FONT_TILE_COUNT = 128;       // Main font tiles
static constexpr size_t FONT_EXTRA_TILE_COUNT = 32;  // Extra font tiles
static constexpr size_t BYTES_PER_1BPP_TILE = 8;     // 1 bit per pixel, 8x8
static constexpr size_t BYTES_PER_2BPP_TILE = 16;    // 2 bits per pixel, 8x8

//=============================================================================
// CHARMAP DEFINITIONS
// From pokecrystal/constants/charmap.asm and suiCune charmap.h
//=============================================================================

// Control characters (not rendered, but need charmap entries)
struct ControlChar {
    uint8_t code;
    const char* name;
};

static constexpr ControlChar CONTROL_CHARS[] = {
    {0x00, "NULL"},
    {0x14, "PLAY_G"},
    {0x15, "MOBILE"},
    {0x16, "CR"},
    {0x1F, "BSP"},       // Breakable space
    {0x22, "LF"},
    {0x24, "POKE"},      // "<PO><KE>"
    {0x25, "WBR"},       // Word-break opportunity
    {0x38, "RED"},
    {0x39, "GREEN"},
    {0x3F, "ENEMY"},
    {0x49, "MOM"},
    {0x4A, "PKMN"},      // "<PK><MN>"
    {0x4B, "_CONT"},     // Implements <CONT> - wait → scroll → continue
    {0x4C, "SCROLL"},    // Scroll 2 lines (no wait)
    {0x4E, "NEXT"},      // Advance 2 rows (no wait)
    {0x4F, "LINE"},      // Jump to line 2 (no wait)
    {0x50, "@"},         // String terminator
    {0x51, "PARA"},      // Wait → clear → continue
    {0x52, "PLAYER"},
    {0x53, "RIVAL"},
    {0x54, "#"},         // "POKé"
    {0x55, "CONT"},      // Writes 0x4B to stream
    {0x56, "......"},    // "......"
    {0x57, "DONE"},      // Terminate text processing only
    {0x58, "PROMPT"},    // Wait then terminate
    {0x59, "TARGET"},
    {0x5A, "USER"},
    {0x5B, "PC"},
    {0x5C, "TM"},
    {0x5D, "TRAINER"},
    {0x5E, "ROCKET"},
    {0x5F, "DEXEND"},
};

// Printable characters mapping
struct PrintableChar {
    uint8_t code;
    const char* utf8;
};

// Main font characters (from Font, codes 0x80-0xFF)
// Layout in ROM: 128 tiles in sequence, map to codes 0x80-0xFF
static constexpr PrintableChar FONT_CHARS[] = {
    // Uppercase A-Z (0x80-0x99)
    {0x80, "A"}, {0x81, "B"}, {0x82, "C"}, {0x83, "D"},
    {0x84, "E"}, {0x85, "F"}, {0x86, "G"}, {0x87, "H"},
    {0x88, "I"}, {0x89, "J"}, {0x8A, "K"}, {0x8B, "L"},
    {0x8C, "M"}, {0x8D, "N"}, {0x8E, "O"}, {0x8F, "P"},
    {0x90, "Q"}, {0x91, "R"}, {0x92, "S"}, {0x93, "T"},
    {0x94, "U"}, {0x95, "V"}, {0x96, "W"}, {0x97, "X"},
    {0x98, "Y"}, {0x99, "Z"},
    
    // Punctuation (0x9A-0x9F)
    {0x9A, "("}, {0x9B, ")"}, {0x9C, ":"}, {0x9D, ";"},
    {0x9E, "["}, {0x9F, "]"},
    
    // Lowercase a-z (0xA0-0xB9)
    {0xA0, "a"}, {0xA1, "b"}, {0xA2, "c"}, {0xA3, "d"},
    {0xA4, "e"}, {0xA5, "f"}, {0xA6, "g"}, {0xA7, "h"},
    {0xA8, "i"}, {0xA9, "j"}, {0xAA, "k"}, {0xAB, "l"},
    {0xAC, "m"}, {0xAD, "n"}, {0xAE, "o"}, {0xAF, "p"},
    {0xB0, "q"}, {0xB1, "r"}, {0xB2, "s"}, {0xB3, "t"},
    {0xB4, "u"}, {0xB5, "v"}, {0xB6, "w"}, {0xB7, "x"},
    {0xB8, "y"}, {0xB9, "z"},
    
    // German umlauts (0xC0-0xC5)
    {0xC0, "Ä"}, {0xC1, "Ö"}, {0xC2, "Ü"},
    {0xC3, "ä"}, {0xC4, "ö"}, {0xC5, "ü"},
    
    // Contractions (0xD0-0xD6)
    {0xD0, "'d"}, {0xD1, "'l"}, {0xD2, "'m"}, {0xD3, "'r"},
    {0xD4, "'s"}, {0xD5, "'t"}, {0xD6, "'v"},
    
    // Symbols (0xDF-0xF5)
    {0xDF, "←"},
    {0xE0, "'"}, 
    {0xE1, "PK"}, {0xE2, "MN"},  // PKMN ligatures
    {0xE3, "-"},
    // 0xE4, 0xE5 Japanese dakuten marks
    {0xE6, "?"}, {0xE7, "!"}, {0xE8, "."}, {0xE9, "&"},
    {0xEA, "é"},  // CRITICAL: é in POKéMON
    {0xEB, "→"}, {0xEC, "▷"}, {0xED, "▶"}, {0xEE, "▼"},
    {0xEF, "♂"},
    {0xF0, "¥"},  // Poké Dollar
    {0xF1, "×"},
    {0xF2, "."},  // Decimal point
    {0xF3, "/"},
    {0xF4, ","},
    {0xF5, "♀"},
    
    // Numbers 0-9 (0xF6-0xFF)
    {0xF6, "0"}, {0xF7, "1"}, {0xF8, "2"}, {0xF9, "3"},
    {0xFA, "4"}, {0xFB, "5"}, {0xFC, "6"}, {0xFD, "7"},
    {0xFE, "8"}, {0xFF, "9"},
};

// Extra font characters (from FontExtra, codes 0x60-0x7F)
// Layout in ROM: 32 tiles in sequence, map to codes 0x60-0x7F
static constexpr PrintableChar FONT_EXTRA_CHARS[] = {
    // Bold/special (0x60-0x6F)
    {0x60, "■"},  // Black square
    {0x61, "▲"},  // Up arrow
    {0x62, "☎"},  // Phone icon
    // 0x63-0x6C: mostly unused bold letters
    {0x6D, ":"},  // Tiny colon
    {0x6E, "′"},  // Feet symbol
    {0x6F, "″"},  // Inches symbol
    
    // PO/KE ligatures (0x70-0x71)
    {0x70, "PO"}, {0x71, "KE"},
    
    // Quotes (0x72-0x73)
    {0x72, """}, {0x73, """},
    
    // Punctuation (0x74-0x75)
    {0x74, "·"}, {0x75, "…"},
    
    // Japanese small chars (0x76-0x78) - usually blank in English
    {0x76, " "}, {0x77, " "}, {0x78, " "},
    
    // Box drawing (0x79-0x7E) - CRITICAL for textbox
    {0x79, "┌"}, {0x7A, "─"}, {0x7B, "┐"},
    {0x7C, "│"}, {0x7D, "└"}, {0x7E, "┘"},
    
    // Space (0x7F)
    {0x7F, " "},
};

//=============================================================================
// CONSTRUCTOR
//=============================================================================

FontExtractor::FontExtractor(const RomData& rom, const ExtractionProfile& profile)
    : rom_(rom), profile_(profile) {}

//=============================================================================
// 1BPP TILE DECODING
// Each tile is 8 bytes (one byte per row, each bit is one pixel)
// Bit order: MSB = leftmost pixel
// Output: 0 = transparent (white), 3 = opaque (black)
//=============================================================================

Glyph FontExtractor::decode_1bpp_tile(const uint8_t* data) const {
    Glyph glyph;
    std::memset(glyph.pixels, 0, sizeof(glyph.pixels));
    glyph.width = 8;
    
    for (int row = 0; row < 8; ++row) {
        uint8_t byte = data[row];
        for (int col = 0; col < 8; ++col) {
            // MSB is leftmost pixel
            bool set = (byte >> (7 - col)) & 1;
            // 1bpp: set pixel = black (3), clear = transparent (0)
            glyph.pixels[row * 8 + col] = set ? 3 : 0;
        }
    }
    
    return glyph;
}

//=============================================================================
// 2BPP TILE DECODING
// Each tile is 16 bytes (2 bytes per row: low bits then high bits)
// Combines into 2-bit value per pixel
// Output: 0-3 palette index
//=============================================================================

Glyph FontExtractor::decode_2bpp_tile(const uint8_t* data) const {
    Glyph glyph;
    std::memset(glyph.pixels, 0, sizeof(glyph.pixels));
    glyph.width = 8;
    
    for (int row = 0; row < 8; ++row) {
        uint8_t low_byte = data[row * 2];
        uint8_t high_byte = data[row * 2 + 1];
        
        for (int col = 0; col < 8; ++col) {
            // MSB is leftmost pixel
            int bit_pos = 7 - col;
            uint8_t low_bit = (low_byte >> bit_pos) & 1;
            uint8_t high_bit = (high_byte >> bit_pos) & 1;
            uint8_t value = (high_bit << 1) | low_bit;
            
            glyph.pixels[row * 8 + col] = value;
        }
    }
    
    return glyph;
}

//=============================================================================
// CHARMAP BUILDING
//=============================================================================

void FontExtractor::build_charmap(ExtractedFont& font) const {
    font.charmap.clear();
    font.code_to_charmap.clear();
    
    // Add control characters (no glyph, just markers)
    for (const auto& ctrl : CONTROL_CHARS) {
        CharmapEntry entry;
        entry.crystal_code = ctrl.code;
        entry.glyph_index = font.space_glyph;  // Use space as placeholder
        entry.utf8_char = "";
        entry.is_control = true;
        entry.control_name = ctrl.name;
        
        font.code_to_charmap[ctrl.code] = font.charmap.size();
        font.charmap.push_back(entry);
    }
    
    // Add font_extra characters (0x60-0x7F)
    // These are the FIRST glyphs in the atlas (loaded from FontExtra)
    uint16_t extra_base = 0;  // FontExtra glyphs start at index 0
    for (const auto& ch : FONT_EXTRA_CHARS) {
        CharmapEntry entry;
        entry.crystal_code = ch.code;
        // Map 0x60-0x7F to glyph indices 0-31
        entry.glyph_index = extra_base + (ch.code - 0x60);
        entry.utf8_char = ch.utf8;
        entry.is_control = false;
        
        font.code_to_charmap[ch.code] = font.charmap.size();
        font.charmap.push_back(entry);
    }
    
    // Add main font characters (0x80-0xFF)
    // These follow FontExtra glyphs in the atlas
    uint16_t main_base = FONT_EXTRA_TILE_COUNT;  // After 32 FontExtra glyphs
    for (const auto& ch : FONT_CHARS) {
        CharmapEntry entry;
        entry.crystal_code = ch.code;
        // Map 0x80-0xFF to glyph indices 32-159
        entry.glyph_index = main_base + (ch.code - 0x80);
        entry.utf8_char = ch.utf8;
        entry.is_control = false;
        
        font.code_to_charmap[ch.code] = font.charmap.size();
        font.charmap.push_back(entry);
    }
    
    // Set special glyph indices
    // Border glyphs are in FontExtra (0x60-0x7F range, indices 0-31)
    font.border_top_left = extra_base + (0x79 - 0x60);     // ┌
    font.border_top = extra_base + (0x7A - 0x60);          // ─
    font.border_top_right = extra_base + (0x7B - 0x60);    // ┐
    font.border_left = extra_base + (0x7C - 0x60);         // │
    font.border_bottom_left = extra_base + (0x7D - 0x60);  // └
    font.border_bottom_right = extra_base + (0x7E - 0x60); // ┘
    font.space_glyph = extra_base + (0x7F - 0x60);         // Space
    
    // Cursor glyph is in main font (0x80-0xFF range, indices 32+)
    font.cursor_glyph = main_base + (0xEE - 0x80);         // ▼
}

//=============================================================================
// MAIN EXTRACTION
//=============================================================================

FontExtractionResult FontExtractor::extract_font() const {
    FontExtractionResult result;
    result.font.font_id = "crystal_main";
    
    // Calculate flat ROM addresses
    uint32_t font_extra_flat = rom_.bank_to_flat(FONT_BANK, FONT_EXTRA_ADDR);
    uint32_t font_flat = rom_.bank_to_flat(FONT_BANK, FONT_ADDR);
    
    // Verify ROM has enough data
    size_t font_extra_bytes = FONT_EXTRA_TILE_COUNT * BYTES_PER_2BPP_TILE;
    size_t font_bytes = FONT_TILE_COUNT * BYTES_PER_1BPP_TILE;
    
    if (font_extra_flat + font_extra_bytes > rom_.size()) {
        result.error = "ROM too small for FontExtra data";
        return result;
    }
    if (font_flat + font_bytes > rom_.size()) {
        result.error = "ROM too small for Font data";
        return result;
    }
    
    // Extract FontExtra (2bpp tiles, 0x60-0x7F range)
    // These must come FIRST in the atlas (indices 0-31)
    auto font_extra_data = rom_.read_bytes(font_extra_flat, font_extra_bytes);
    for (size_t i = 0; i < FONT_EXTRA_TILE_COUNT; ++i) {
        result.font.glyphs.push_back(
            decode_2bpp_tile(&font_extra_data[i * BYTES_PER_2BPP_TILE]));
    }
    
    // Extract Font (1bpp tiles, 0x80-0xFF range)
    // These come AFTER FontExtra (indices 32-159)
    auto font_data = rom_.read_bytes(font_flat, font_bytes);
    for (size_t i = 0; i < FONT_TILE_COUNT; ++i) {
        result.font.glyphs.push_back(
            decode_1bpp_tile(&font_data[i * BYTES_PER_1BPP_TILE]));
    }
    
    // Build charmap
    build_charmap(result.font);
    
    result.success = true;
    return result;
}

//=============================================================================
// DEFAULT PALETTE
//=============================================================================

FontPalette default_text_palette() {
    FontPalette pal;
    // Crystal text: color 0 = transparent, color 3 = black (text)
    // For text rendering, we want black text on transparent background
    pal.colors[0] = 0x00000000;  // Transparent
    pal.colors[1] = 0xFFAAAAAA;  // Light gray (unused in text)
    pal.colors[2] = 0xFF555555;  // Dark gray (unused in text)
    pal.colors[3] = 0xFF000000;  // Black (text color)
    return pal;
}

//=============================================================================
// ATLAS RENDERING
//=============================================================================

FontAtlas render_font_atlas(const ExtractedFont& font, const FontPalette& palette) {
    FontAtlas atlas;
    atlas.font_id = font.font_id;
    
    // Layout: 16 glyphs per row, 8x8 pixels each
    constexpr uint32_t GLYPHS_PER_ROW = 16;
    constexpr uint32_t GLYPH_SIZE = 8;
    
    size_t glyph_count = font.glyphs.size();
    uint32_t rows = static_cast<uint32_t>((glyph_count + GLYPHS_PER_ROW - 1) / GLYPHS_PER_ROW);
    
    atlas.atlas_width = GLYPHS_PER_ROW * GLYPH_SIZE;  // 128
    atlas.atlas_height = rows * GLYPH_SIZE;
    atlas.pixels.resize(atlas.atlas_width * atlas.atlas_height, 0x00000000);
    
    // Render each glyph
    for (size_t i = 0; i < glyph_count; ++i) {
        const Glyph& g = font.glyphs[i];
        
        uint32_t gx = static_cast<uint32_t>(i % GLYPHS_PER_ROW) * GLYPH_SIZE;
        uint32_t gy = static_cast<uint32_t>(i / GLYPHS_PER_ROW) * GLYPH_SIZE;
        
        for (int py = 0; py < 8; ++py) {
            for (int px = 0; px < 8; ++px) {
                uint8_t pixel = g.pixels[py * 8 + px];
                uint32_t color = palette.colors[pixel];
                
                uint32_t ax = gx + static_cast<uint32_t>(px);
                uint32_t ay = gy + static_cast<uint32_t>(py);
                atlas.pixels[ay * atlas.atlas_width + ax] = color;
            }
        }
        
        // Store UV coordinates
        FontAtlas::GlyphUV uv;
        uv.u0 = static_cast<float>(gx) / static_cast<float>(atlas.atlas_width);
        uv.v0 = static_cast<float>(gy) / static_cast<float>(atlas.atlas_height);
        uv.u1 = static_cast<float>(gx + GLYPH_SIZE) / static_cast<float>(atlas.atlas_width);
        uv.v1 = static_cast<float>(gy + GLYPH_SIZE) / static_cast<float>(atlas.atlas_height);
        atlas.glyph_uvs.push_back(uv);
    }
    
    // Copy charmap
    atlas.charmap = font.charmap;
    
    // Copy special indices
    atlas.border_top_left = font.border_top_left;
    atlas.border_top = font.border_top;
    atlas.border_top_right = font.border_top_right;
    atlas.border_left = font.border_left;
    atlas.border_bottom_left = font.border_bottom_left;
    atlas.border_bottom_right = font.border_bottom_right;
    atlas.space_glyph = font.space_glyph;
    atlas.cursor_glyph = font.cursor_glyph;
    
    return atlas;
}

} // namespace crystal
