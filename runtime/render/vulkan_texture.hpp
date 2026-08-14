#pragma once
// runtime/render/vulkan_texture.hpp
// Simple Vulkan texture for tileset atlas upload
//
// Uploads RGBA32 pixel data to GPU with nearest-neighbor sampling.

#include <vulkan/vulkan.h>
#include <cstdint>
#include <vector>

namespace enginemon {

class VulkanBootstrap;

class VulkanTexture {
public:
    VulkanTexture() = default;
    ~VulkanTexture();
    
    // Non-copyable
    VulkanTexture(const VulkanTexture&) = delete;
    VulkanTexture& operator=(const VulkanTexture&) = delete;
    
    // Move-able
    VulkanTexture(VulkanTexture&& other) noexcept;
    VulkanTexture& operator=(VulkanTexture&& other) noexcept;
    
    // Create from RGBA32 pixel data
    bool create(VulkanBootstrap& vk, uint32_t width, uint32_t height,
                const uint32_t* pixels);
    
    // Cleanup
    void destroy();
    
    // Accessors
    VkImage image() const { return image_; }
    VkImageView view() const { return view_; }
    VkSampler sampler() const { return sampler_; }
    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }
    bool is_valid() const { return image_ != VK_NULL_HANDLE; }

private:
    VkDevice device_ = VK_NULL_HANDLE;
    VkImage image_ = VK_NULL_HANDLE;
    VkDeviceMemory memory_ = VK_NULL_HANDLE;
    VkImageView view_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    
    uint32_t find_memory_type(VkPhysicalDevice physical_device,
                              uint32_t type_filter,
                              VkMemoryPropertyFlags properties);
};

} // namespace enginemon
