#pragma once
// runtime/render/textbox_renderer.hpp
// Native textbox/dialogue renderer using Vulkan 1.3
//
// Renders textbox with font glyphs and border tiles.
// Font atlas built from compiled FontDefinition in EMON package.
//
// Text flow semantics (engine-owned):
//   opentext  → Open textbox (no yield)
//   writetext → Display text (no yield) 
//   waitbutton / promptbutton → Wait for A-button (yield)
//   closetext → Close textbox (no yield)
//
// Multi-page support uses semantic TextControl enum:
//   Line   → Move to line 2 (no wait)
//   Next   → Clear box, continue (no wait)
//   Para   → Wait, clear, continue
//   Cont   → Wait, scroll, continue
//   Done   → End text processing
//   Prompt → Show cursor, wait, then end
//
// NOTE: This file contains ZERO Crystal-specific encoding.
// All text arrives as semantic RuntimeTextSequence from the frontend.

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
// RUNTIME FONT ATLAS (loaded from compiled FontDefinition in package)
//=============================================================================

// Semantic text control codes - NO Crystal byte values
// These are engine-owned identities, not source-platform codes.
enum class TextControl : uint8_t {
    None = 0,
    Line = 1,      // Move to line 2 (no wait)
    Next = 2,      // Clear box, continue (no wait)
    Para = 3,      // Wait, clear, continue
    Cont = 4,      // Wait, scroll, continue
    Done = 5,      // End text processing
    Prompt = 6,    // Show cursor, wait, end
    Terminator = 7, // End of string
    // Deferred dynamic ops: op is known but not yet rendered as visible text.
    // The renderer should pass these through without treating them as display-control.
    // The op_name field identifies the specific dynamic operation.
    DeferredDynamic = 8,
};

// Runtime font atlas
// Built from compiled FontDefinition in EMON package.
// Provides native UTF-8 → GlyphId lookup (no Crystal charmap knowledge).
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
    
    // Native UTF-8 character → GlyphId lookup
    // Key: UTF-8 character string (e.g., "A", "é", "0")
    // Value: glyph index in atlas
    std::unordered_map<std::string, uint16_t> utf8_to_glyph;
    
    // Special glyphs
    uint16_t border_tl, border_t, border_tr;
    uint16_t border_l, border_bl, border_br;
    uint16_t space_glyph, cursor_glyph;
    
    // Look up glyph for a UTF-8 character
    uint16_t glyph_for_utf8(const std::string& ch) const {
        auto it = utf8_to_glyph.find(ch);
        return (it != utf8_to_glyph.end()) ? it->second : space_glyph;
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
    
    // Textbox dimensions (in 8x8 tiles)
    // Standard textbox: X=0, Y=12, width=20, inner_height=4
    // At 2x scale: 20 tiles × 16px = 320px wide, positioned at Y=192 (tile 12 × 16)
    uint32_t box_tile_x = 0;
    uint32_t box_tile_y = 12;       // 12 tiles down
    uint32_t box_width_tiles = 20;  // Full width
    uint32_t box_inner_height = 4;  // 4 lines of text
    uint32_t tile_size = 16;        // 8px × 2x scale = 16px
    
    // Text positioning inside box
    uint32_t text_start_tile_x = 1;
    uint32_t text_start_tile_y = 13;  // First text line (box_tile_y + 1)
    uint32_t text_width_tiles = 18;   // Inner width
    uint32_t max_lines = 2;           // Visible lines at a time
};

//=============================================================================
// NATIVE TEXT TYPES (no Crystal encoding)
//=============================================================================

// A single text element - either a printable text run or a control operation
struct NativeTextElement {
    TextControl control = TextControl::None;  // None = text run
    std::string text;  // UTF-8 text (only for control == None)
    // For DeferredDynamic: op_name identifies the dynamic op; addr/param carry operands
    std::string op_name;
    uint32_t addr = 0;
    uint8_t param = 0;
    uint8_t param2 = 0;
    
    bool is_text() const { return control == TextControl::None && !text.empty(); }
    bool is_control() const { return control != TextControl::None; }
    
    static NativeTextElement make_text(const std::string& s) { 
        return {TextControl::None, s}; 
    }
    static NativeTextElement make_line() { return {TextControl::Line, ""}; }
    static NativeTextElement make_para() { return {TextControl::Para, ""}; }
    static NativeTextElement make_cont() { return {TextControl::Cont, ""}; }
    static NativeTextElement make_done() { return {TextControl::Done, ""}; }
    static NativeTextElement make_prompt() { return {TextControl::Prompt, ""}; }
    static NativeTextElement make_deferred(const std::string& name, uint32_t a = 0, uint8_t p = 0, uint8_t p2 = 0) {
        NativeTextElement e;
        e.control = TextControl::DeferredDynamic;
        e.op_name = name;
        e.addr = a;
        e.param = p;
        e.param2 = p2;
        return e;
    }
};

// Complete native text sequence
struct NativeTextSequence {
    std::vector<NativeTextElement> elements;
    
    bool empty() const { return elements.empty(); }
    void clear() { elements.clear(); }
    
    // Build from RuntimeTextSequence (from Lua api_bindings)
    static NativeTextSequence from_runtime(const RuntimeTextSequence& seq);
};

// Page metadata for multi-page tracking
struct PageMeta {
    size_t stream_start = 0;        // Starting index in element stream
    size_t stream_end = 0;          // Ending index (exclusive)
    bool ends_with_para = false;    // PARA follows this page
    bool ends_with_cont = false;    // CONT follows this page
    bool is_final = false;          // DONE/PROMPT terminates here
};

// Page of text (what's visible at once)
// Lines are stored as UTF-8 strings
struct TextPage {
    std::vector<std::string> lines;  // Up to 2 lines of UTF-8 text
    bool is_cont_page = false;       // If true, this is a scroll-continuation page
};

// Visible text buffer - tracks what's currently displayed
// Uses UTF-8 strings (no Crystal encoding)
struct VisibleTextBuffer {
    std::string line1;  // Top visible line (row 0)
    std::string line2;  // Bottom visible line (row 1)
    
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

//=============================================================================
// TEXTBOX STATE (native - no Crystal encoding)
//=============================================================================

struct TextboxState {
    bool is_open = false;
    bool waiting_for_input = false;
    bool show_cursor = false;
    
    // Page metadata
    std::vector<PageMeta> page_meta;
    
    // Control flags
    bool waiting_for_para = false;   // PARA: A will clear and continue
    bool waiting_for_cont = false;   // CONT: A will scroll and continue  
    bool text_complete = false;      // Text stream finished (DONE/PROMPT reached)
    
    // Visible text buffer - UTF-8 strings currently displayed
    VisibleTextBuffer visible;
    
    // Native text sequence (semantic operations, no Crystal codes)
    NativeTextSequence text_sequence;
    
    // Parsed into pages (for rendering)
    std::vector<TextPage> pages;
    size_t current_page = 0;
    
    // Character reveal animation
    size_t chars_revealed = 0;
    bool reveal_complete = true;
    
    // Open with semantic text sequence (the correct API)
    // RuntimeTextSequence comes from the frontend decoder which preserves semantics.
    void open_with_sequence(const RuntimeTextSequence& seq);
    
    void open() {
        is_open = true;
        waiting_for_input = false;
        show_cursor = false;
        waiting_for_para = false;
        waiting_for_cont = false;
        text_complete = false;
        chars_revealed = 0;
        current_page = 0;
        visible.clear();
        text_sequence.clear();
        pages.clear();
        page_meta.clear();
    }
    
    void close() {
        is_open = false;
        waiting_for_input = false;
        show_cursor = false;
        waiting_for_para = false;
        waiting_for_cont = false;
        text_complete = false;
        text_sequence.clear();
        pages.clear();
        page_meta.clear();
        current_page = 0;
        chars_revealed = 0;
        visible.clear();
    }
    
    bool has_more_pages() const {
        return current_page + 1 < pages.size();
    }
    
    // Advance to next page based on control code semantics
    // Returns true if we successfully moved to a new page that needs display
    // Returns false if we're already on the final page (no advancement possible)
    bool advance_page() {
        if (current_page >= page_meta.size()) {
            return false;
        }
        
        const auto& meta = page_meta[current_page];
        
        // If this page is final (DONE/PROMPT), we've already shown it
        if (meta.is_final) {
            text_complete = true;
            waiting_for_para = false;
            waiting_for_cont = false;
            return false;
        }
        
        // Determine transition type BEFORE incrementing page
        bool was_para = meta.ends_with_para;
        bool was_cont = meta.ends_with_cont;
        
        // Move to next page
        current_page++;
        chars_revealed = 0;
        reveal_complete = false;
        
        if (current_page < pages.size()) {
            const auto& new_page = pages[current_page];
            
            if (was_para) {
                // PARA: Clear visible buffer, show new page fresh
                visible.clear();
                if (new_page.lines.size() > 0) {
                    visible.line1 = new_page.lines[0];
                }
                if (new_page.lines.size() > 1) {
                    visible.line2 = new_page.lines[1];
                }
            }
            else if (was_cont || new_page.is_cont_page) {
                // CONT: Scroll visible buffer, add one new line
                visible.scroll();
                if (new_page.lines.size() > 0) {
                    visible.line2 = new_page.lines[0];
                }
            }
            else {
                // Default: treat like fresh page
                visible.clear();
                if (new_page.lines.size() > 0) {
                    visible.line1 = new_page.lines[0];
                }
                if (new_page.lines.size() > 1) {
                    visible.line2 = new_page.lines[1];
                }
            }
            
            // Update wait state for the NEW page
            const auto& new_meta = page_meta[current_page];
            waiting_for_para = new_meta.ends_with_para;
            waiting_for_cont = new_meta.ends_with_cont;
            return true;
        }
        
        text_complete = true;
        return false;
    }
    
    // Get current page wait reason (for A-button behavior)
    enum class WaitReason { None, Para, Cont, Done, Prompt };
    WaitReason current_wait_reason() const {
        if (current_page >= page_meta.size()) {
            return WaitReason::Done;
        }
        const auto& meta = page_meta[current_page];
        if (meta.is_final) {
            return WaitReason::Done;
        }
        if (meta.ends_with_para) {
            return WaitReason::Para;
        }
        if (meta.ends_with_cont) {
            return WaitReason::Cont;
        }
        return WaitReason::None;
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
    
    // Load font atlas (call after initialize)
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
