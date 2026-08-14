#pragma once
// runtime/render/tile_renderer.hpp
// 2D tile map renderer using Vulkan 1.3
//
// Renders maps using native 8×8 indexed tiles from RuntimeTileset.
// Resolves palettes at render time based on map environment + time policy.
// Batches tile instances efficiently on map load.
// Uses nearest-neighbor sampling for pixel-perfect graphics.
//
// Architecture:
//   Map BlockIds → expand each block's 16 TileIds → 
//   look up palette_id per tile → generate tile instance vertices →
//   upload/batch → render with palette buffer
//
// This preserves BlockId → 16 TileIds semantics for future 3D/RT.

#include <vulkan/vulkan.h>
#include "engine/world/runtime_map.hpp"
#include "engine/world/runtime_tileset.hpp"
#include "render/vulkan_texture.hpp"
#include <vector>

namespace enginemon {

class VulkanBootstrap;

// Vertex for tile rendering (position + UV + palette_id)
struct TileVertex {
    float x, y;     // Position in pixels
    float u, v;     // Texture coordinates
    float pal_id;   // Palette ID (0-6) for this tile instance
};

// Configuration for tile renderer
struct TileRendererConfig {
    // Logical game resolution (Crystal = 160x144, doubled = 320x288)
    uint32_t logical_width = 320;
    uint32_t logical_height = 288;
    
    // Integer scale factor (1x, 2x, 3x, 4x, etc.)
    uint32_t scale_factor = 1;
    
    // Tile size in pixels (8×8 native)
    uint32_t tile_size = 8;
    
    // Block size (4×4 tiles = 32×32 pixels)
    uint32_t block_size = 32;
};

// Tile map renderer
class TileRenderer {
public:
    TileRenderer();
    ~TileRenderer();
    
    // Non-copyable
    TileRenderer(const TileRenderer&) = delete;
    TileRenderer& operator=(const TileRenderer&) = delete;
    
    // Initialize renderer
    bool initialize(VulkanBootstrap& vk, const TileRendererConfig& config);
    
    // Set tileset (uploads indexed tile atlas + palette data to GPU)
    // The active_row parameter selects which palette row to use based on
    // the map's environment + time_policy resolved against RTC
    bool set_tileset(VulkanBootstrap& vk, const RuntimeTileset& tileset, PaletteRow active_row);
    
    // Build map geometry (expands blocks to tile instances with palette_id)
    bool build_map(VulkanBootstrap& vk, const RuntimeMap& map, const RuntimeTileset& tileset);
    
    // Update view (camera position in pixels)
    void set_view(float camera_x, float camera_y);
    
    // Update for window resize
    void update_viewport(uint32_t window_width, uint32_t window_height);
    
    // Render (call between vkCmdBeginRendering and vkCmdEndRendering)
    void render(VkCommandBuffer cmd);
    
    // Cleanup
    void destroy();
    
    // Accessors
    uint32_t internal_width() const { return config_.logical_width * config_.scale_factor; }
    uint32_t internal_height() const { return config_.logical_height * config_.scale_factor; }
    bool is_initialized() const { return pipeline_ != VK_NULL_HANDLE; }
    
    // Palette resolution helper
    // Resolves map environment + time_policy + RTC to a PaletteRow
    static PaletteRow resolve_palette_row(
        Environment env, 
        PalettePolicy policy,
        PaletteRow rtc_time = PaletteRow::Day);  // Default to Day for now

private:
    VkDevice device_ = VK_NULL_HANDLE;
    TileRendererConfig config_;
    
    // Pipeline
    VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptor_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
    VkDescriptorSet descriptor_set_ = VK_NULL_HANDLE;
    
    // Tile atlas texture (all 8×8 tiles, pre-colored RGBA for now)
    VulkanTexture tile_texture_;
    
    // Map geometry (all tile instances)
    VkBuffer vertex_buffer_ = VK_NULL_HANDLE;
    VkDeviceMemory vertex_memory_ = VK_NULL_HANDLE;
    VkBuffer index_buffer_ = VK_NULL_HANDLE;
    VkDeviceMemory index_memory_ = VK_NULL_HANDLE;
    uint32_t index_count_ = 0;
    
    // View/projection
    float camera_x_ = 0.0f;
    float camera_y_ = 0.0f;
    VkExtent2D window_extent_ = {0, 0};
    
    // Computed viewport (integer-scaled, centered)
    VkViewport scaled_viewport_ = {};
    VkRect2D scaled_scissor_ = {};
    
    // Cached data for building geometry
    std::vector<TileUV> tile_uvs_;
    std::vector<RuntimeBlock> blocks_;
    std::vector<uint8_t> palette_map_;  // tile_id → palette_id
    uint8_t map_width_ = 0;
    uint8_t map_height_ = 0;
    
    // Create shader module from SPIR-V
    VkShaderModule create_shader_module(const uint32_t* code, size_t size);
    
    // Create pipeline
    bool create_pipeline(VulkanBootstrap& vk);
    
    // Create/destroy geometry buffers
    bool create_buffers(VulkanBootstrap& vk, const std::vector<TileVertex>& vertices,
                        const std::vector<uint32_t>& indices);
    void destroy_buffers();
    
    // Memory helper
    uint32_t find_memory_type(VkPhysicalDevice physical_device,
                              uint32_t type_filter,
                              VkMemoryPropertyFlags properties);
};

} // namespace enginemon
