#pragma once
// render/presentation.hpp
// Presentation configuration and pixel-perfect rendering
//
// REQUIREMENTS:
// - VSync, low-latency present modes, VRR-friendly presentation
// - Frame caps / uncapped rendering  
// - Nearest-neighbor sampling
// - Aspect-ratio preservation
// - Pixel integrity
//
// 2D: Full-scene integer scaling
// 3D: Native output resolution geometry, original texel grid on textures/sprites/UI

#include "engine/core/timing.hpp"
#include <cstdint>

namespace enginemon {

// Scaling mode for 2D rendering
enum class ScalingMode : uint8_t {
    IntegerScale,       // Nearest integer multiple (pixel-perfect)
    AspectFit,          // Fit to window, preserve aspect, may not be integer
    AspectFill,         // Fill window, preserve aspect, may crop
    Stretch             // Stretch to window (not recommended)
};

// Filter mode for texture sampling
enum class FilterMode : uint8_t {
    Nearest,            // Nearest-neighbor (pixel-perfect)
    Bilinear            // Smooth scaling (for 3D when appropriate)
};

// Presentation configuration
struct PresentationConfig {
    // Window/output
    uint32_t window_width = 1280;
    uint32_t window_height = 720;
    bool fullscreen = false;
    bool borderless = false;
    
    // Native resolution (original game)
    uint32_t native_width = 160;    // Game Boy: 160x144
    uint32_t native_height = 144;
    
    // 2D presentation
    ScalingMode scaling_mode_2d = ScalingMode::IntegerScale;
    FilterMode filter_mode_2d = FilterMode::Nearest;
    uint32_t forced_scale = 0;      // 0 = auto, otherwise force this scale
    
    // 3D presentation  
    FilterMode filter_mode_textures = FilterMode::Nearest;  // Texel grid
    FilterMode filter_mode_sprites = FilterMode::Nearest;   // Sprite slabs
    FilterMode filter_mode_ui = FilterMode::Nearest;        // UI elements
    FilterMode filter_mode_geometry = FilterMode::Bilinear; // 3D geometry edges
    
    // Aspect ratio
    float aspect_ratio = 160.0f / 144.0f;  // 10:9 for Game Boy
    bool preserve_aspect = true;
    
    // VSync / timing
    PresentMode present_mode = PresentMode::VSync;
    uint32_t frame_cap = 0;         // 0 = no cap
    bool allow_tearing = false;     // For lowest latency
    
    // VRR
    bool vrr_enabled = true;        // Use if available
    uint32_t vrr_min_fps = 30;
    uint32_t vrr_max_fps = 144;
};

// Computed presentation state
struct PresentationState {
    // Actual render target size (may differ from window)
    uint32_t render_width;
    uint32_t render_height;
    
    // Viewport within window (for letterboxing)
    int32_t viewport_x;
    int32_t viewport_y;
    uint32_t viewport_width;
    uint32_t viewport_height;
    
    // Integer scale factor (for 2D)
    uint32_t integer_scale;
    
    // Whether we achieved pixel-perfect scaling
    bool pixel_perfect;
};

// Calculate presentation state from config and window size
PresentationState calculate_presentation(
    const PresentationConfig& config,
    uint32_t actual_window_width,
    uint32_t actual_window_height);

// 2D-specific helpers
namespace Presentation2D {
    // Calculate best integer scale for window
    uint32_t calculate_integer_scale(
        uint32_t native_width, uint32_t native_height,
        uint32_t window_width, uint32_t window_height);
    
    // Calculate viewport for centered letterboxed output
    void calculate_letterbox(
        uint32_t scaled_width, uint32_t scaled_height,
        uint32_t window_width, uint32_t window_height,
        int32_t& out_x, int32_t& out_y);
}

// 3D-specific helpers
namespace Presentation3D {
    // Calculate texture sampling to preserve texel grid
    struct TexelGridParams {
        float texel_size;       // Size of one texel in screen pixels
        float offset_x;         // Sub-pixel offset for alignment
        float offset_y;
    };
    
    TexelGridParams calculate_texel_grid(
        uint32_t texture_size,
        uint32_t screen_size,
        float world_scale);
}

// Vulkan swapchain configuration derived from presentation config
struct SwapchainConfig {
    uint32_t image_count;           // 2 = double buffer, 3 = triple
    bool vsync;
    bool mailbox;                   // Triple buffer without vsync
    bool immediate;                 // No sync
    bool fifo_relaxed;              // VRR-friendly
};

SwapchainConfig derive_swapchain_config(const PresentationConfig& config);

} // namespace enginemon
