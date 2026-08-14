#pragma once
// runtime/render/vulkan_bootstrap.hpp
// Minimal Vulkan 1.3 bootstrap for Enginemon
//
// Creates: instance, physical device, logical device, queues, surface, swapchain
// Handles: resize/swapchain recreation, frame presentation
// Requires: Vulkan 1.3 with core dynamicRendering and synchronization2
// Does NOT: render anything yet - just clears and presents

#include <vulkan/vulkan.h>
#include <cstdint>
#include <vector>
#include <functional>

struct SDL_Window;

namespace enginemon {

// Forward declarations
class Sdl3Platform;

// Vulkan bootstrap configuration
struct VulkanConfig {
    bool enable_validation = true;  // Debug layers
    bool vsync = true;
    uint32_t preferred_image_count = 2;  // Double buffering
};

// Frame synchronization primitives
struct FrameSync {
    VkSemaphore image_available = VK_NULL_HANDLE;
    VkSemaphore render_finished = VK_NULL_HANDLE;
    VkFence in_flight = VK_NULL_HANDLE;
};

// Minimal Vulkan context for presentation
class VulkanBootstrap {
public:
    VulkanBootstrap();
    ~VulkanBootstrap();
    
    // Non-copyable
    VulkanBootstrap(const VulkanBootstrap&) = delete;
    VulkanBootstrap& operator=(const VulkanBootstrap&) = delete;
    
    //=========================================================================
    // LIFECYCLE
    //=========================================================================
    
    // Initialize Vulkan with SDL3 window
    bool initialize(Sdl3Platform& platform, const VulkanConfig& config);
    
    // Shutdown and cleanup
    void shutdown();
    
    // Check if initialized
    bool is_initialized() const { return initialized_; }
    
    //=========================================================================
    // FRAME PRESENTATION
    //=========================================================================
    
    // Begin frame - acquires swapchain image, waits for fence
    // Returns false if swapchain needs recreation
    bool begin_frame();
    
    // End frame - submits command buffer and presents
    // command_buffer should contain all rendering commands
    // Returns false if swapchain needs recreation
    bool end_frame(VkCommandBuffer command_buffer);
    
    // Recreate swapchain (after resize)
    bool recreate_swapchain(uint32_t width, uint32_t height);
    
    //=========================================================================
    // ACCESSORS
    //=========================================================================
    
    VkInstance instance() const { return instance_; }
    VkPhysicalDevice physical_device() const { return physical_device_; }
    VkDevice device() const { return device_; }
    VkQueue graphics_queue() const { return graphics_queue_; }
    VkQueue present_queue() const { return present_queue_; }
    VkSurfaceKHR surface() const { return surface_; }
    VkSwapchainKHR swapchain() const { return swapchain_; }
    VkFormat swapchain_format() const { return swapchain_format_; }
    VkExtent2D swapchain_extent() const { return swapchain_extent_; }
    const std::vector<VkImage>& swapchain_images() const { return swapchain_images_; }
    const std::vector<VkImageView>& swapchain_image_views() const { return swapchain_image_views_; }
    uint32_t current_image_index() const { return current_image_index_; }
    uint32_t current_frame() const { return current_frame_; }
    VkCommandPool command_pool() const { return command_pool_; }
    
    // Get command buffer for current frame
    VkCommandBuffer get_command_buffer() const;
    
    // Max frames in flight
    static constexpr uint32_t MAX_FRAMES_IN_FLIGHT = 2;

private:
    //=========================================================================
    // VULKAN OBJECTS
    //=========================================================================
    
    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debug_messenger_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    
    // Queues
    VkQueue graphics_queue_ = VK_NULL_HANDLE;
    VkQueue present_queue_ = VK_NULL_HANDLE;
    uint32_t graphics_family_ = 0;
    uint32_t present_family_ = 0;
    
    // Surface and swapchain
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat swapchain_format_ = VK_FORMAT_UNDEFINED;
    VkExtent2D swapchain_extent_ = {0, 0};
    std::vector<VkImage> swapchain_images_;
    std::vector<VkImageView> swapchain_image_views_;
    
    // Command pool and buffers
    VkCommandPool command_pool_ = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> command_buffers_;
    
    // Synchronization
    std::vector<FrameSync> frame_sync_;
    uint32_t current_frame_ = 0;
    uint32_t current_image_index_ = 0;
    
    // State
    bool initialized_ = false;
    VulkanConfig config_;
    Sdl3Platform* platform_ = nullptr;
    
    //=========================================================================
    // INITIALIZATION HELPERS
    //=========================================================================
    
    bool create_instance(const std::vector<const char*>& sdl_extensions);
    bool setup_debug_messenger();
    bool select_physical_device();
    bool create_logical_device();
    bool create_swapchain(uint32_t width, uint32_t height);
    bool create_image_views();
    bool create_command_pool();
    bool create_command_buffers();
    bool create_sync_objects();
    
    void cleanup_swapchain();
    
    // Helpers
    bool check_validation_layer_support();
    bool check_device_extension_support(VkPhysicalDevice device);
    int rate_device_suitability(VkPhysicalDevice device);
    bool find_queue_families(VkPhysicalDevice device, uint32_t& graphics, uint32_t& present);
    VkSurfaceFormatKHR choose_surface_format(const std::vector<VkSurfaceFormatKHR>& formats);
    VkPresentModeKHR choose_present_mode(const std::vector<VkPresentModeKHR>& modes);
    VkExtent2D choose_swap_extent(const VkSurfaceCapabilitiesKHR& capabilities, uint32_t width, uint32_t height);
};

} // namespace enginemon
