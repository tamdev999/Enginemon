// crystal/extract/font_extractor.hpp
// Crystal font and charmap extraction from ROM
//
// Extracts font glyphs from ROM:
//   - Font (3e:4200): 128 tiles × 8 bytes = 1024 bytes (1bpp)
//   - FontExtra (3e:4000): 32 tiles × 16 bytes = 512 bytes (2bpp)
//
// Output is semantic: the runtime knows nothing about Crystal VRAM or tile layout.

#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <optional>

namespace crystal {

// Forward declarations
class RomData;
struct ExtractionProfile;

//=============================================================================
// GLYPH DATA
//=============================================================================

// A single 8x8 glyph (2-bit pixels for consistent output)
struct Glyph {
    uint8_t pixels[64];  // 8x8, values 0-3
    uint8_t width = 8;   // Logical width (for variable-width future support)
};

// Charmap entry: maps Crystal byte code to glyph index and UTF-8 string
struct CharmapEntry {
    uint8_t crystal_code;     // Crystal byte code (0x00-0xFF)
    uint16_t glyph_index;     // Index into glyph atlas
    std::string utf8_char;    // UTF-8 representation (for debugging/display)
    bool is_control;          // True if this is a control character (<NEXT>, etc.)
    std::string control_name; // Control name if is_control (e.g., "NEXT", "LINE")
};

//=============================================================================
// EXTRACTED FONT DATA
//=============================================================================

// Extracted font data (intermediate representation)
struct ExtractedFont {
    std::string font_id;
    std::vector<Glyph> glyphs;           // All glyphs
    std::vector<CharmapEntry> charmap;   // Crystal byte → glyph mapping
    
    // Quick lookup
    std::unordered_map<uint8_t, size_t> code_to_charmap;  // Crystal code → charmap index
    
    // Textbox border glyphs (indices into glyphs array)
    uint16_t border_top_left = 0;      // ┌ = 0x79
    uint16_t border_top = 0;           // ─ = 0x7A
    uint16_t border_top_right = 0;     // ┐ = 0x7B
    uint16_t border_left = 0;          // │ = 0x7C
    uint16_t border_bottom_left = 0;   // └ = 0x7D
    uint16_t border_bottom_right = 0;  // ┘ = 0x7E
    uint16_t space_glyph = 0;          // Space = 0x7F
    
    // Cursor glyph
    uint16_t cursor_glyph = 0;         // ▼ = 0xEE
    
    // Lookup helpers
    const CharmapEntry* lookup(uint8_t crystal_code) const {
        auto it = code_to_charmap.find(crystal_code);
        if (it != code_to_charmap.end() && it->second < charmap.size()) {
            return &charmap[it->second];
        }
        return nullptr;
    }
    
    uint16_t glyph_for_code(uint8_t code) const {
        auto entry = lookup(code);
        return entry ? entry->glyph_index : space_glyph;
    }
};

//=============================================================================
// FONT PALETTE
//=============================================================================

// Font palette (Crystal uses 4 colors for text)
struct FontPalette {
    uint32_t colors[4];  // RGBA32
};

// Default Crystal text palette (black on transparent)
FontPalette default_text_palette();

//=============================================================================
// FONT ATLAS (rendered, ready for GPU)
//=============================================================================

// Rendered font atlas (ready for GPU upload)
struct FontAtlas {
    std::string font_id;
    uint32_t atlas_width;
    uint32_t atlas_height;
    std::vector<uint32_t> pixels;        // RGBA32
    std::vector<CharmapEntry> charmap;   // Crystal byte → glyph mapping
    
    // UV coordinates for each glyph (indexed by glyph_index)
    struct GlyphUV {
        float u0, v0, u1, v1;
    };
    std::vector<GlyphUV> glyph_uvs;
    
    // Special glyph indices
    uint16_t border_top_left = 0, border_top = 0, border_top_right = 0;
    uint16_t border_left = 0, border_bottom_left = 0, border_bottom_right = 0;
    uint16_t space_glyph = 0, cursor_glyph = 0;
};

//=============================================================================
// EXTRACTION RESULT
//=============================================================================

struct FontExtractionResult {
    bool success = false;
    std::string error;
    ExtractedFont font;
};

//=============================================================================
// FONT EXTRACTOR
//=============================================================================

// Font extractor
// Extracts font data directly from Crystal ROM using profile addresses
class FontExtractor {
public:
    // Constructor takes ROM and profile
    FontExtractor(const RomData& rom, const ExtractionProfile& profile);
    
    // Extract main font (Font + FontExtra from ROM)
    FontExtractionResult extract_font() const;
    
private:
    const RomData& rom_;
    const ExtractionProfile& profile_;
    
    // Decode 1bpp tile (8 bytes) to Glyph
    // 1bpp: each byte is one row, each bit is one pixel
    Glyph decode_1bpp_tile(const uint8_t* data) const;
    
    // Decode 2bpp tile (16 bytes) to Glyph
    // 2bpp: pairs of bytes per row (low bits, high bits)
    Glyph decode_2bpp_tile(const uint8_t* data) const;
    
    // Build complete charmap from Crystal byte codes
    void build_charmap(ExtractedFont& font) const;
};

//=============================================================================
// ATLAS RENDERING
//=============================================================================

// Render font atlas with specific palette
FontAtlas render_font_atlas(const ExtractedFont& font, const FontPalette& palette);

} // namespace crystal
