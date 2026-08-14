#pragma once
// runtime/render/textbox_renderer.hpp
// Crystal-authentic textbox/dialogue renderer using Vulkan 1.3
//
// Renders textbox with Crystal font glyphs and border tiles.
// Uses font atlas extracted from pokecrystal gfx/font/ assets.
//
// Text flow semantics (from Crystal):
//   opentext  → Open textbox (no yield)
//   writetext → Display text (no yield) 
//   waitbutton / promptbutton → Wait for A-button (yield)
//   closetext → Close textbox (no yield)
//
// Multi-page support:
//   <LINE>  (0x4F) → Move to line 2
//   <NEXT>  (0x4E) → Move to next page (clears box)
//   <PARA>  (0x51) → Wait, then clear and continue
//   <CONT>  (0x55) → Scroll and continue
//   <DONE>  (0x57) → End (close box)
//   <PROMPT>(0x58) → Show cursor, wait, then end

#include <vulkan/vulkan.h>
#include <string>
#include <vector>
#include <array>
#include <cstdint>
#include <unordered_map>

namespace enginemon {

class VulkanBootstrap;

// Forward declaration for semantic text sequence from api_bindings
struct RuntimeTextSequence;
struct RuntimeTextElement;
enum class RuntimeTextOp : uint8_t;

//=============================================================================
// RUNTIME FONT ATLAS (loaded from extracted Crystal data)
//=============================================================================

// Charmap entry for runtime
struct RuntimeCharmapEntry {
    uint8_t crystal_code;
    uint16_t glyph_index;
    bool is_control;
    uint8_t control_type;  // 0=none, 1=LINE, 2=NEXT, 3=PARA, 4=CONT, 5=DONE, 6=PROMPT
};

// Control character types
enum class TextControl : uint8_t {
    None = 0,
    Line = 1,      // <LINE> - move to line 2
    Next = 2,      // <NEXT> - new page
    Para = 3,      // <PARA> - wait, clear, continue
    Cont = 4,      // <CONT> - scroll, continue
    Done = 5,      // <DONE> - end
    Prompt = 6,    // <PROMPT> - show cursor, wait, end
    Terminator = 7 // @ - string end
};

// Runtime font atlas
struct RuntimeFontAtlas {
    std::string font_id;
    uint32_t atlas_width;
    uint32_t atlas_height;
    std::vector<uint32_t> pixels;  // RGBA32
    
    // Glyph UVs indexed by glyph_index
    struct GlyphUV {
        float u0, v0, u1, v1;
    };
    std::vector<GlyphUV> glyph_uvs;
    
    // Charmap (Crystal byte → glyph)
    std::unordered_map<uint8_t, uint16_t> code_to_glyph;
    std::unordered_map<uint8_t, TextControl> code_to_control;
    
    // Special glyphs
    uint16_t border_tl, border_t, border_tr;
    uint16_t border_l, border_bl, border_br;
    uint16_t space_glyph, cursor_glyph;
    
    uint16_t glyph_for_code(uint8_t code) const {
        auto it = code_to_glyph.find(code);
        return (it != code_to_glyph.end()) ? it->second : space_glyph;
    }
    
    TextControl control_for_code(uint8_t code) const {
        auto it = code_to_control.find(code);
        return (it != code_to_control.end()) ? it->second : TextControl::None;
    }
    
    // Parse font atlas from serialized package data
    // Returns true on success
    static bool from_package_data(const std::vector<uint8_t>& data, RuntimeFontAtlas& atlas);
};

//=============================================================================
// TEXTBOX VERTEX
//=============================================================================

struct TextboxVertex {
    float x, y;        // Position in NDC
    float u, v;        // Texture coordinates
    float r, g, b, a;  // Color tint
};

//=============================================================================
// TEXTBOX CONFIGURATION
//=============================================================================

struct TextboxRendererConfig {
    // Logical game resolution (must match tile/sprite renderers)
    uint32_t logical_width = 320;   // 160 doubled
    uint32_t logical_height = 288;  // 144 doubled
    
    // Crystal textbox dimensions (in 8x8 tiles)
    // Standard textbox: TEXTBOX_X=0, TEXTBOX_Y=12, width=20, inner_height=4
    // At 2x scale: 20 tiles × 16px = 320px wide, positioned at Y=192 (tile 12 × 16)
    uint32_t box_tile_x = 0;
    uint32_t box_tile_y = 12;       // 12 tiles down (144-48 at 1x = 96px up from bottom at 2x)
    uint32_t box_width_tiles = 20;  // Full width
    uint32_t box_inner_height = 4;  // 4 lines of text
    uint32_t tile_size = 16;        // 8px × 2x scale = 16px
    
    // Text positioning inside box (Crystal: TEXTBOX_INNERX=1, TEXTBOX_INNERY=14)
    uint32_t text_start_tile_x = 1;
    uint32_t text_start_tile_y = 13;  // First text line (box_tile_y + 1)
    uint32_t text_width_tiles = 18;   // Inner width
    uint32_t max_lines = 2;           // Visible lines at a time
};

//=============================================================================
// TEXTBOX STATE
//=============================================================================

// Parsed text segment (text or control)
struct TextSegment {
    bool is_control;
    TextControl control;
    std::vector<uint8_t> glyphs;  // Crystal codes for text segments
};

// Text stream state - tracks position within the full text stream
// Crystal text is a single stream with control codes; pages are views into it
struct TextStreamState {
    size_t stream_index = 0;        // Current position in full_text_encoded
    
    // Wait reason after current display is shown
    enum class WaitReason { None, Para, Cont, Done, Prompt };
    WaitReason wait_reason = WaitReason::None;
    
    // Visible text state (2 lines max)
    std::vector<uint8_t> line1;     // First visible line
    std::vector<uint8_t> line2;     // Second visible line
    int cursor_line = 0;            // 0 = line1, 1 = line2
    size_t cursor_col = 0;          // Column within current line
};

// Page metadata for multi-page tracking
struct PageMeta {
    size_t stream_start = 0;        // Starting index in text stream
    size_t stream_end = 0;          // Ending index (exclusive)
    bool ends_with_para = false;    // PARA follows this page
    bool ends_with_cont = false;    // CONT follows this page
    bool is_final = false;          // DONE/PROMPT terminates here
};

// Page of text (what's visible at once)
// For PARA pages: lines contains all lines to show fresh
// For CONT pages: first line is carried from previous page (scroll), rest is new
struct TextPage {
    std::vector<std::vector<uint8_t>> lines;  // Up to 2 lines of Crystal codes
    bool is_cont_page = false;  // If true, first line is from previous page scroll
};

// Visible text buffer - tracks what's currently displayed
// This is separate from page parsing and handles scroll/clear semantics
struct VisibleTextBuffer {
    std::vector<uint8_t> line1;  // Top visible line (row 0)
    std::vector<uint8_t> line2;  // Bottom visible line (row 1)
    
    void clear() {
        line1.clear();
        line2.clear();
    }
    
    // Scroll: move line2 to line1, clear line2
    void scroll() {
        line1 = std::move(line2);
        line2.clear();
    }
};

struct TextboxState {
    bool is_open = false;
    bool waiting_for_input = false;
    bool show_cursor = false;
    
    // Text stream state machine (Crystal-authentic)
    TextStreamState stream;
    std::vector<PageMeta> page_meta;  // Metadata per page
    
    // Legacy flags for backward compatibility
    bool waiting_for_para = false;   // PARA: A will clear and continue
    bool waiting_for_cont = false;   // CONT: A will scroll and continue  
    bool text_complete = false;      // Text stream finished (DONE/PROMPT reached)
    
    // Visible text buffer - what's actually displayed
    // This handles PARA (clear) vs CONT (scroll) rendering
    VisibleTextBuffer visible;
    
    // Full text - stored as UTF-8 string for compatibility
    std::string text;
    std::string visible_text;
    
    // Crystal-native encoded text stream
    std::vector<uint8_t> full_text_encoded;
    
    // Parsed into pages (for rendering)
    std::vector<TextPage> pages;
    size_t current_page = 0;
    
    // Character reveal animation
    size_t chars_revealed = 0;
    bool reveal_complete = true;
    
    // Methods - backward compatible with old API
    void open(const std::string& new_text) {
        is_open = true;
        waiting_for_input = true;
        show_cursor = true;
        waiting_for_para = false;
        waiting_for_cont = false;
        text_complete = false;
        text = new_text;
        visible_text = new_text;
        chars_revealed = new_text.size();
        current_page = 0;
        pages.clear();
        page_meta.clear();
        stream = TextStreamState{};  // Reset stream state
        visible.clear();  // Reset visible buffer
        
        // Encode to Crystal codes for page parsing
        encode_text_to_crystal();
    }
    
    void open() {
        is_open = true;
        waiting_for_input = false;
        show_cursor = false;
        waiting_for_para = false;
        waiting_for_cont = false;
        text_complete = false;
        chars_revealed = 0;
        current_page = 0;
        stream = TextStreamState{};  // Reset stream state
        visible.clear();  // Reset visible buffer
    }
    
    // Open with semantic text sequence (preserves LINE/CONT/PARA distinctions)
    // This is the correct path that avoids lossy encode_text_to_crystal().
    // The RuntimeTextSequence comes from the frontend decoder which preserves semantics.
    void open_with_sequence(const RuntimeTextSequence& seq);
    
    // Helper to encode UTF-8 text (just characters, no control codes) to Crystal codes
    static void encode_utf8_to_crystal(const std::string& utf8_text, std::vector<uint8_t>& output);
    
    void set_text(const std::vector<uint8_t>& encoded_text) {
        full_text_encoded = encoded_text;
        // Pages will be built by renderer using charmap
    }
    
    void close() {
        is_open = false;
        waiting_for_input = false;
        show_cursor = false;
        waiting_for_para = false;
        waiting_for_cont = false;
        text_complete = false;
        text.clear();
        visible_text.clear();
        full_text_encoded.clear();
        pages.clear();
        page_meta.clear();
        current_page = 0;
        chars_revealed = 0;
        stream = TextStreamState{};
        visible.clear();  // Reset visible buffer
    }
    
    // Encode UTF-8 text to Crystal codes
    // This allows us to use the Crystal-aware page parsing
    void encode_text_to_crystal() {
        full_text_encoded.clear();
        if (visible_text.empty()) return;
        
        const char* p = visible_text.c_str();
        const char* end = p + visible_text.size();
        
        while (p < end) {
            unsigned char ch = static_cast<unsigned char>(*p);
            
            // Handle newlines - they may represent control codes
            if (*p == '\n') {
                // Check if this is a double newline (PARA)
                if (p + 1 < end && p[1] == '\n') {
                    full_text_encoded.push_back(0x51);  // PARA
                    p += 2;
                    continue;
                }
                // Single newline - LINE
                full_text_encoded.push_back(0x4F);  // LINE
                p++;
                continue;
            }
            
            // Check for multi-byte UTF-8 sequences
            if ((ch & 0xE0) == 0xC0 && (p + 1 < end)) {
                // 2-byte UTF-8 sequence
                unsigned char b1 = ch;
                unsigned char b2 = static_cast<unsigned char>(p[1]);
                
                // é = 0xC3 0xA9 (UTF-8 for U+00E9)
                if (b1 == 0xC3 && b2 == 0xA9) {
                    full_text_encoded.push_back(0xEA);  // Crystal code for é
                    p += 2;
                    continue;
                }
                // Ä = 0xC3 0x84
                else if (b1 == 0xC3 && b2 == 0x84) {
                    full_text_encoded.push_back(0xC0);
                    p += 2;
                    continue;
                }
                // Ö = 0xC3 0x96
                else if (b1 == 0xC3 && b2 == 0x96) {
                    full_text_encoded.push_back(0xC1);
                    p += 2;
                    continue;
                }
                // Ü = 0xC3 0x9C
                else if (b1 == 0xC3 && b2 == 0x9C) {
                    full_text_encoded.push_back(0xC2);
                    p += 2;
                    continue;
                }
                // ä = 0xC3 0xA4
                else if (b1 == 0xC3 && b2 == 0xA4) {
                    full_text_encoded.push_back(0xC3);
                    p += 2;
                    continue;
                }
                // ö = 0xC3 0xB6
                else if (b1 == 0xC3 && b2 == 0xB6) {
                    full_text_encoded.push_back(0xC4);
                    p += 2;
                    continue;
                }
                // ü = 0xC3 0xBC
                else if (b1 == 0xC3 && b2 == 0xBC) {
                    full_text_encoded.push_back(0xC5);
                    p += 2;
                    continue;
                }
                // Unknown 2-byte sequence, skip
                p += 2;
                full_text_encoded.push_back(0x7F);  // Space
                continue;
            }
            else if ((ch & 0xF0) == 0xE0 && (p + 2 < end)) {
                // 3-byte UTF-8 sequence (skip)
                p += 3;
                full_text_encoded.push_back(0x7F);
                continue;
            }
            else if ((ch & 0xF8) == 0xF0 && (p + 3 < end)) {
                // 4-byte UTF-8 sequence (skip)
                p += 4;
                full_text_encoded.push_back(0x7F);
                continue;
            }
            
            // Single-byte ASCII
            char c = *p;
            p++;
            
            uint8_t code = 0x7F;  // Space by default
            if (c >= 'A' && c <= 'Z') code = 0x80 + (c - 'A');
            else if (c >= 'a' && c <= 'z') code = 0xA0 + (c - 'a');
            else if (c >= '0' && c <= '9') code = 0xF6 + (c - '0');
            else if (c == ' ') code = 0x7F;
            else if (c == '.') code = 0xE8;
            else if (c == ',') code = 0xF4;
            else if (c == '!') code = 0xE7;
            else if (c == '?') code = 0xE6;
            else if (c == '-') code = 0xE3;
            else if (c == '\'') code = 0xE0;
            else if (c == ':') code = 0x9C;
            else if (c == ';') code = 0x9D;
            else if (c == '(') code = 0x9A;
            else if (c == ')') code = 0x9B;
            else if (c == '/') code = 0xF3;
            else if (c == '&') code = 0xE9;
            
            full_text_encoded.push_back(code);
        }
        
        // Add terminator
        full_text_encoded.push_back(0x57);  // DONE
    }
    
    bool has_more_pages() const {
        return current_page + 1 < pages.size();
    }
    
    // Advance to next page based on control code semantics
    // Returns true if we successfully moved to a new page that needs display
    // Returns false if we're already on the final page (no advancement possible)
    // 
    // IMPORTANT: This also updates the visible buffer:
    // - PARA pages: clear visible buffer, populate from new page lines
    // - CONT pages: scroll visible buffer (line2→line1), add new line to line2
    bool advance_page() {
        if (current_page >= page_meta.size()) {
            return false;
        }
        
        const auto& meta = page_meta[current_page];
        
        // If this page is final (DONE/PROMPT), we've already shown it
        // A-press on final page means dialogue is complete
        if (meta.is_final) {
            text_complete = true;
            waiting_for_para = false;
            waiting_for_cont = false;
            return false;  // No more pages to show
        }
        
        // Determine transition type BEFORE incrementing page
        bool was_para = meta.ends_with_para;
        bool was_cont = meta.ends_with_cont;
        
        // Move to next page
        current_page++;
        chars_revealed = 0;
        reveal_complete = false;
        
        // Update visible buffer based on transition type
        if (current_page < pages.size()) {
            const auto& new_page = pages[current_page];
            
            if (was_para) {
                // PARA: Clear visible buffer, show new page fresh
                // New page can have 1 or 2 lines (first page after PARA is not CONT)
                visible.clear();
                if (new_page.lines.size() > 0) {
                    visible.line1 = new_page.lines[0];
                }
                if (new_page.lines.size() > 1) {
                    visible.line2 = new_page.lines[1];
                }
            }
            else if (was_cont || new_page.is_cont_page) {
                // CONT: Scroll visible buffer (line2 → line1), add one new line to line2
                // CONT pages have max 1 line by design, so this is straightforward
                visible.scroll();
                if (new_page.lines.size() > 0) {
                    visible.line2 = new_page.lines[0];
                }
                // Note: CONT pages should only have 1 line, but handle 2 for safety
                if (new_page.lines.size() > 1) {
                    // This means we need to scroll again - shouldn't happen with proper parsing
                    visible.scroll();
                    visible.line2 = new_page.lines[1];
                }
            }
            else {
                // Default (shouldn't happen): treat like fresh page
                visible.clear();
                if (new_page.lines.size() > 0) {
                    visible.line1 = new_page.lines[0];
                }
                if (new_page.lines.size() > 1) {
                    visible.line2 = new_page.lines[1];
                }
            }
            
            // Update wait state for the NEW page we just moved to
            const auto& new_meta = page_meta[current_page];
            waiting_for_para = new_meta.ends_with_para;
            waiting_for_cont = new_meta.ends_with_cont;
            return true;  // Successfully advanced to a new page that needs display
        }
        
        // No more pages (shouldn't happen if page_meta is correct)
        text_complete = true;
        return false;
    }
    
    // Get current page wait reason (for A-button behavior)
    TextStreamState::WaitReason current_wait_reason() const {
        if (current_page >= page_meta.size()) {
            return TextStreamState::WaitReason::Done;
        }
        const auto& meta = page_meta[current_page];
        if (meta.is_final) {
            return TextStreamState::WaitReason::Done;
        }
        if (meta.ends_with_para) {
            return TextStreamState::WaitReason::Para;
        }
        if (meta.ends_with_cont) {
            return TextStreamState::WaitReason::Cont;
        }
        return TextStreamState::WaitReason::None;
    }
};

//=============================================================================
// TEXTBOX RENDERER
//=============================================================================

class TextboxRenderer {
public:
    TextboxRenderer();
    ~TextboxRenderer();
    
    TextboxRenderer(const TextboxRenderer&) = delete;
    TextboxRenderer& operator=(const TextboxRenderer&) = delete;
    
    // Initialize with configuration
    bool initialize(VulkanBootstrap& vk, const TextboxRendererConfig& config);
    
    // Load Crystal font atlas (call after initialize)
    bool load_font_atlas(VulkanBootstrap& vk, const RuntimeFontAtlas& atlas);
    
    // Update for window resize
    void update_viewport(uint32_t window_width, uint32_t window_height);
    
    // Set textbox state
    void set_state(const TextboxState& state);
    
    // Parse text into pages (call when text changes)
    void parse_text_pages(TextboxState& state);
    
    // Render textbox overlay
    void render(VkCommandBuffer cmd);
    
    // Cleanup
    void destroy();
    
    // Get font atlas for text parsing
    const RuntimeFontAtlas* font_atlas() const { return has_font_ ? &font_atlas_ : nullptr; }

private:
    VkDevice device_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
    TextboxRendererConfig config_;
    TextboxState state_;
    
    // Font atlas
    RuntimeFontAtlas font_atlas_;
    bool has_font_ = false;
    
    // Pipeline
    VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptor_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
    VkDescriptorSet descriptor_set_ = VK_NULL_HANDLE;
    
    // Font texture
    VkImage font_image_ = VK_NULL_HANDLE;
    VkDeviceMemory font_memory_ = VK_NULL_HANDLE;
    VkImageView font_view_ = VK_NULL_HANDLE;
    VkSampler font_sampler_ = VK_NULL_HANDLE;
    
    // Geometry buffers
    VkBuffer vertex_buffer_ = VK_NULL_HANDLE;
    VkDeviceMemory vertex_memory_ = VK_NULL_HANDLE;
    VkBuffer index_buffer_ = VK_NULL_HANDLE;
    VkDeviceMemory index_memory_ = VK_NULL_HANDLE;
    size_t vertex_buffer_size_ = 0;
    size_t index_buffer_size_ = 0;
    uint32_t index_count_ = 0;
    
    // Viewport
    VkViewport scaled_viewport_ = {};
    VkRect2D scaled_scissor_ = {};
    
    // Helpers
    VkShaderModule create_shader_module(const uint32_t* code, size_t size);
    bool create_pipeline(VulkanBootstrap& vk);
    bool create_font_texture(VulkanBootstrap& vk);
    bool ensure_buffers(VulkanBootstrap& vk, size_t vertex_count, size_t index_count);
    void build_geometry(std::vector<TextboxVertex>& vertices, std::vector<uint16_t>& indices);
    uint32_t find_memory_type(uint32_t type_filter, VkMemoryPropertyFlags properties);
    
    // Geometry building
    void add_solid_quad(std::vector<TextboxVertex>& vertices, std::vector<uint16_t>& indices,
                        float px, float py, float width, float height, float r, float g, float b, float a);
    void add_glyph_quad(std::vector<TextboxVertex>& vertices, std::vector<uint16_t>& indices,
                        float px, float py, uint16_t glyph_index, float r, float g, float b, float a);
    void add_border(std::vector<TextboxVertex>& vertices, std::vector<uint16_t>& indices);
    void add_text(std::vector<TextboxVertex>& vertices, std::vector<uint16_t>& indices);
};

} // namespace enginemon
