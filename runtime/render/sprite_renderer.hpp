#pragma once
// runtime/render/sprite_renderer.hpp
// Sprite renderer for player and NPCs using Vulkan 1.3
//
// Renders sprites on top of the tile map.
// Uses nearest-neighbor sampling and alpha blending.
// Supports facing direction, walk animation, and position interpolation.

#include <vulkan/vulkan.h>
#include "engine/world/sprite_atlas.hpp"
#include "engine/world/movement_manager.hpp"
#include "render/vulkan_texture.hpp"
#include <vector>
#include <string>
#include <array>
#include <unordered_map>

namespace enginemon {

class VulkanBootstrap;

// Sprite vertex (position + UV)
struct SpriteVertex {
    float x, y;     // Position in pixels
    float u, v;     // Texture coordinates
};

// Configuration for sprite renderer
struct SpriteRendererConfig {
    // Logical game resolution (must match tile renderer)
    uint32_t logical_width = 320;
    uint32_t logical_height = 288;
};

// Sprite instance to render
struct SpriteInstance {
    std::string sprite_id;
    
    // Semantic facing direction (no Crystal encoding)
    SpriteFacing facing = SpriteFacing::Down;
    
    // Animation state (for frame selection only)
    bool walking = false;       // Show walk animation frame (not standing)
    bool step_flip = false;     // Alternate step (flip up/down walk frames)
    
    // Movement interpolation (separate from animation frame selection)
    // Reference: Gen2Recomped Player.lua - position interpolates from origin to target
    bool is_moving = false;         // Whether actor is mid-step (enables interpolation)
    float walk_progress = 0.0f;     // 0.0-1.0 within current step
    float start_x = 0, start_y = 0; // Position at step start (origin)
    float target_x = 0, target_y = 0; // Position at step end (destination)
    
    // Get interpolated render position
    // When moving: interpolate from start to target based on progress
    // When standing: use start position (which equals current logical position)
    float render_x() const {
        if (!is_moving || walk_progress <= 0) return start_x;
        return start_x + (target_x - start_x) * walk_progress;
    }
    
    float render_y() const {
        if (!is_moving || walk_progress <= 0) return start_y;
        return start_y + (target_y - start_y) * walk_progress;
    }
};

// Sprite renderer
class SpriteRenderer {
public:
    SpriteRenderer();
    ~SpriteRenderer();
    
    // Non-copyable
    SpriteRenderer(const SpriteRenderer&) = delete;
    SpriteRenderer& operator=(const SpriteRenderer&) = delete;
    
    // Initialize renderer
    bool initialize(VulkanBootstrap& vk, const SpriteRendererConfig& config);
    
    // Set sprite atlas (uploads to GPU)
    bool set_atlas(VulkanBootstrap& vk, const RuntimeSpriteAtlas& atlas);
    
    // Set sprite data for semantic frame selection
    void set_sprite_data(const std::vector<RuntimeSprite>& sprites);
    
    // Update view (must match tile renderer camera)
    void set_view(float camera_x, float camera_y);
    
    // Update for window resize (must match tile renderer)
    void update_viewport(uint32_t window_width, uint32_t window_height);
    
    // Set sprites to render this frame
    void set_sprites(const std::vector<SpriteInstance>& sprites);
    
    // Pre-allocate buffers for sprite count (call before render loop)
    bool prepare_buffers(VulkanBootstrap& vk, size_t max_sprites);
    
    // Render sprites (call after tile renderer, between begin/end rendering)
    void render(VkCommandBuffer cmd);
    
    // Cleanup
    void destroy();

private:
    VkDevice device_ = VK_NULL_HANDLE;
    SpriteRendererConfig config_;
    
    // Pipeline (shares same structure as tile renderer)
    VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptor_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
    VkDescriptorSet descriptor_set_ = VK_NULL_HANDLE;
    
    // Sprite atlas texture
    VulkanTexture atlas_texture_;
    
    // UV lookup for sprites
    std::unordered_map<std::string, RuntimeSpriteAtlas::SpriteUVs> sprite_uvs_;
    
    // Sprite data for semantic frame selection
    std::unordered_map<std::string, RuntimeSprite> sprites_data_;
    
    // Dynamic vertex/index buffers - per frame to avoid data races with GPU
    // With MAX_FRAMES_IN_FLIGHT = 2, we need separate buffers for each frame
    static constexpr uint32_t MAX_FRAMES_IN_FLIGHT = 2;
    
    struct FrameBuffers {
        VkBuffer vertex_buffer = VK_NULL_HANDLE;
        VkDeviceMemory vertex_memory = VK_NULL_HANDLE;
        VkBuffer index_buffer = VK_NULL_HANDLE;
        VkDeviceMemory index_memory = VK_NULL_HANDLE;
        size_t vertex_buffer_size = 0;
        size_t index_buffer_size = 0;
    };
    std::array<FrameBuffers, MAX_FRAMES_IN_FLIGHT> frame_buffers_;
    uint32_t current_frame_ = 0;
    uint32_t index_count_ = 0;
    
    // View state
    float camera_x_ = 0.0f;
    float camera_y_ = 0.0f;
    VkViewport scaled_viewport_ = {};
    VkRect2D scaled_scissor_ = {};
    
    // Current sprites to render
    std::vector<SpriteInstance> sprites_;
    
    // Physical device for memory allocation
    VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
    
    // Helpers
    VkShaderModule create_shader_module(const uint32_t* code, size_t size);
    bool create_pipeline(VulkanBootstrap& vk);
    bool ensure_buffers(VulkanBootstrap& vk, size_t vertex_count, size_t index_count, uint32_t frame_index);
    void destroy_buffers();
    uint32_t find_memory_type(uint32_t type_filter, VkMemoryPropertyFlags properties);
    
    // Build geometry for current sprites
    void build_geometry(std::vector<SpriteVertex>& vertices, std::vector<uint16_t>& indices);
};

} // namespace enginemon
