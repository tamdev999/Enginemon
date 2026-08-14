// runtime/render/vulkan_bootstrap.cpp
// Minimal Vulkan bootstrap implementation

#include "render/vulkan_bootstrap.hpp"
#include "platform/sdl3_platform.hpp"
#include <iostream>
#include <set>
#include <algorithm>
#include <limits>
#include <cstring>

namespace enginemon {

// Validation layers
static const std::vector<const char*> VALIDATION_LAYERS = {
    "VK_LAYER_KHRONOS_validation"
};

// Required device extensions
static const std::vector<const char*> DEVICE_EXTENSIONS = {
    VK_KHR_SWAPCHAIN_EXTENSION_NAME
};

// Debug callback
static VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT type,
    const VkDebugUtilsMessengerCallbackDataEXT* data,
    void* user_data
) {
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        std::cerr << "[Vulkan] " << data->pMessage << "\n";
    }
    return VK_FALSE;
}

VulkanBootstrap::VulkanBootstrap() = default;

VulkanBootstrap::~VulkanBootstrap() {
    shutdown();
}

bool VulkanBootstrap::initialize(Sdl3Platform& platform, const VulkanConfig& config) {
    if (initialized_) return true;
    
    platform_ = &platform;
    config_ = config;
    
    auto sdl_extensions = platform.get_required_vulkan_extensions();
    
    if (!create_instance(sdl_extensions)) return false;
    if (config_.enable_validation && !setup_debug_messenger()) {
        std::cerr << "Warning: Debug messenger setup failed\n";
    }
    
    surface_ = platform.create_vulkan_surface(instance_);
    if (surface_ == VK_NULL_HANDLE) return false;
    
    if (!select_physical_device()) return false;
    if (!create_logical_device()) return false;
    if (!create_swapchain(platform.width(), platform.height())) return false;
    if (!create_image_views()) return false;
    if (!create_command_pool()) return false;
    if (!create_command_buffers()) return false;
    if (!create_sync_objects()) return false;
    
    initialized_ = true;
    return true;
}

void VulkanBootstrap::shutdown() {
    if (!initialized_) return;
    
    if (device_) {
        vkDeviceWaitIdle(device_);
    }
    
    // Cleanup sync objects
    for (auto& sync : frame_sync_) {
        if (sync.image_available) vkDestroySemaphore(device_, sync.image_available, nullptr);
        if (sync.render_finished) vkDestroySemaphore(device_, sync.render_finished, nullptr);
        if (sync.in_flight) vkDestroyFence(device_, sync.in_flight, nullptr);
    }
    frame_sync_.clear();
    
    cleanup_swapchain();
    
    if (command_pool_) {
        vkDestroyCommandPool(device_, command_pool_, nullptr);
        command_pool_ = VK_NULL_HANDLE;
    }
    
    if (device_) {
        vkDestroyDevice(device_, nullptr);
        device_ = VK_NULL_HANDLE;
    }
    
    if (surface_) {
        vkDestroySurfaceKHR(instance_, surface_, nullptr);
        surface_ = VK_NULL_HANDLE;
    }
    
    if (debug_messenger_) {
        auto func = (PFN_vkDestroyDebugUtilsMessengerEXT)
            vkGetInstanceProcAddr(instance_, "vkDestroyDebugUtilsMessengerEXT");
        if (func) {
            func(instance_, debug_messenger_, nullptr);
        }
        debug_messenger_ = VK_NULL_HANDLE;
    }
    
    if (instance_) {
        vkDestroyInstance(instance_, nullptr);
        instance_ = VK_NULL_HANDLE;
    }
    
    initialized_ = false;
}

bool VulkanBootstrap::begin_frame() {
    auto& sync = frame_sync_[current_frame_];
    
    // Wait for previous frame to finish
    vkWaitForFences(device_, 1, &sync.in_flight, VK_TRUE, UINT64_MAX);
    
    // Acquire next swapchain image
    VkResult result = vkAcquireNextImageKHR(
        device_, swapchain_, UINT64_MAX,
        sync.image_available, VK_NULL_HANDLE, &current_image_index_
    );
    
    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        return false;  // Need to recreate swapchain
    } else if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
        std::cerr << "Failed to acquire swapchain image\n";
        return false;
    }
    
    // Reset fence only after we know we're submitting work
    vkResetFences(device_, 1, &sync.in_flight);
    
    // Reset and begin command buffer
    VkCommandBuffer cmd = command_buffers_[current_frame_];
    vkResetCommandBuffer(cmd, 0);
    
    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    
    if (vkBeginCommandBuffer(cmd, &begin_info) != VK_SUCCESS) {
        std::cerr << "Failed to begin command buffer\n";
        return false;
    }
    
    return true;
}

bool VulkanBootstrap::end_frame(VkCommandBuffer command_buffer) {
    if (vkEndCommandBuffer(command_buffer) != VK_SUCCESS) {
        std::cerr << "Failed to end command buffer\n";
        return false;
    }
    
    auto& sync = frame_sync_[current_frame_];
    
    // Submit command buffer
    VkSubmitInfo submit_info{};
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    
    VkSemaphore wait_semaphores[] = {sync.image_available};
    VkPipelineStageFlags wait_stages[] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
    submit_info.waitSemaphoreCount = 1;
    submit_info.pWaitSemaphores = wait_semaphores;
    submit_info.pWaitDstStageMask = wait_stages;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &command_buffer;
    
    VkSemaphore signal_semaphores[] = {sync.render_finished};
    submit_info.signalSemaphoreCount = 1;
    submit_info.pSignalSemaphores = signal_semaphores;
    
    if (vkQueueSubmit(graphics_queue_, 1, &submit_info, sync.in_flight) != VK_SUCCESS) {
        std::cerr << "Failed to submit draw command buffer\n";
        return false;
    }
    
    // Present
    VkPresentInfoKHR present_info{};
    present_info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present_info.waitSemaphoreCount = 1;
    present_info.pWaitSemaphores = signal_semaphores;
    
    VkSwapchainKHR swapchains[] = {swapchain_};
    present_info.swapchainCount = 1;
    present_info.pSwapchains = swapchains;
    present_info.pImageIndices = &current_image_index_;
    
    VkResult result = vkQueuePresentKHR(present_queue_, &present_info);
    
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
        return false;  // Need to recreate swapchain
    } else if (result != VK_SUCCESS) {
        std::cerr << "Failed to present swapchain image\n";
        return false;
    }
    
    current_frame_ = (current_frame_ + 1) % MAX_FRAMES_IN_FLIGHT;
    return true;
}

bool VulkanBootstrap::recreate_swapchain(uint32_t width, uint32_t height) {
    // Wait for device idle
    vkDeviceWaitIdle(device_);
    
    // Cleanup old swapchain
    cleanup_swapchain();
    
    // Create new swapchain
    if (!create_swapchain(width, height)) return false;
    if (!create_image_views()) return false;
    
    return true;
}

VkCommandBuffer VulkanBootstrap::get_command_buffer() const {
    return command_buffers_[current_frame_];
}

void VulkanBootstrap::cleanup_swapchain() {
    for (auto view : swapchain_image_views_) {
        vkDestroyImageView(device_, view, nullptr);
    }
    swapchain_image_views_.clear();
    swapchain_images_.clear();
    
    if (swapchain_) {
        vkDestroySwapchainKHR(device_, swapchain_, nullptr);
        swapchain_ = VK_NULL_HANDLE;
    }
}

bool VulkanBootstrap::create_instance(const std::vector<const char*>& sdl_extensions) {
    VkApplicationInfo app_info{};
    app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app_info.pApplicationName = "Enginemon";
    app_info.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    app_info.pEngineName = "Enginemon";
    app_info.engineVersion = VK_MAKE_VERSION(0, 1, 0);
    app_info.apiVersion = VK_API_VERSION_1_3;
    
    VkInstanceCreateInfo create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    create_info.pApplicationInfo = &app_info;
    
    // Extensions
    std::vector<const char*> extensions = sdl_extensions;
    if (config_.enable_validation) {
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }
    create_info.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    create_info.ppEnabledExtensionNames = extensions.data();
    
    // Validation layers
    if (config_.enable_validation && check_validation_layer_support()) {
        create_info.enabledLayerCount = static_cast<uint32_t>(VALIDATION_LAYERS.size());
        create_info.ppEnabledLayerNames = VALIDATION_LAYERS.data();
    } else {
        create_info.enabledLayerCount = 0;
    }
    
    if (vkCreateInstance(&create_info, nullptr, &instance_) != VK_SUCCESS) {
        std::cerr << "Failed to create Vulkan instance\n";
        return false;
    }
    
    return true;
}

bool VulkanBootstrap::setup_debug_messenger() {
    VkDebugUtilsMessengerCreateInfoEXT create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    create_info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT |
                                  VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                  VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    create_info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                              VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                              VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    create_info.pfnUserCallback = debug_callback;
    
    auto func = (PFN_vkCreateDebugUtilsMessengerEXT)
        vkGetInstanceProcAddr(instance_, "vkCreateDebugUtilsMessengerEXT");
    if (!func || func(instance_, &create_info, nullptr, &debug_messenger_) != VK_SUCCESS) {
        return false;
    }
    return true;
}

bool VulkanBootstrap::select_physical_device() {
    uint32_t device_count = 0;
    vkEnumeratePhysicalDevices(instance_, &device_count, nullptr);
    
    if (device_count == 0) {
        std::cerr << "No GPUs with Vulkan support\n";
        return false;
    }
    
    std::vector<VkPhysicalDevice> devices(device_count);
    vkEnumeratePhysicalDevices(instance_, &device_count, devices.data());
    
    // Find best device
    int best_score = -1;
    for (const auto& device : devices) {
        int score = rate_device_suitability(device);
        if (score > best_score) {
            best_score = score;
            physical_device_ = device;
        }
    }
    
    if (physical_device_ == VK_NULL_HANDLE) {
        std::cerr << "No suitable GPU found\n";
        return false;
    }
    
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(physical_device_, &props);
    std::cout << "Using GPU: " << props.deviceName << "\n";
    
    return true;
}

bool VulkanBootstrap::create_logical_device() {
    if (!find_queue_families(physical_device_, graphics_family_, present_family_)) {
        return false;
    }
    
    std::vector<VkDeviceQueueCreateInfo> queue_create_infos;
    std::set<uint32_t> unique_families = {graphics_family_, present_family_};
    
    float priority = 1.0f;
    for (uint32_t family : unique_families) {
        VkDeviceQueueCreateInfo queue_info{};
        queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queue_info.queueFamilyIndex = family;
        queue_info.queueCount = 1;
        queue_info.pQueuePriorities = &priority;
        queue_create_infos.push_back(queue_info);
    }
    
    // Vulkan 1.3 core features - dynamic rendering is required
    VkPhysicalDeviceVulkan13Features vulkan13_features{};
    vulkan13_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    vulkan13_features.dynamicRendering = VK_TRUE;
    vulkan13_features.synchronization2 = VK_TRUE;
    
    VkPhysicalDeviceFeatures2 device_features2{};
    device_features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    device_features2.pNext = &vulkan13_features;
    
    VkDeviceCreateInfo create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    create_info.pNext = &device_features2;
    create_info.queueCreateInfoCount = static_cast<uint32_t>(queue_create_infos.size());
    create_info.pQueueCreateInfos = queue_create_infos.data();
    create_info.pEnabledFeatures = nullptr;  // Using pNext chain instead
    create_info.enabledExtensionCount = static_cast<uint32_t>(DEVICE_EXTENSIONS.size());
    create_info.ppEnabledExtensionNames = DEVICE_EXTENSIONS.data();
    
    if (vkCreateDevice(physical_device_, &create_info, nullptr, &device_) != VK_SUCCESS) {
        std::cerr << "Failed to create logical device\n";
        return false;
    }
    
    vkGetDeviceQueue(device_, graphics_family_, 0, &graphics_queue_);
    vkGetDeviceQueue(device_, present_family_, 0, &present_queue_);
    
    return true;
}

bool VulkanBootstrap::create_swapchain(uint32_t width, uint32_t height) {
    VkSurfaceCapabilitiesKHR capabilities;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_device_, surface_, &capabilities);
    
    uint32_t format_count;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device_, surface_, &format_count, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(format_count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device_, surface_, &format_count, formats.data());
    
    uint32_t mode_count;
    vkGetPhysicalDeviceSurfacePresentModesKHR(physical_device_, surface_, &mode_count, nullptr);
    std::vector<VkPresentModeKHR> modes(mode_count);
    vkGetPhysicalDeviceSurfacePresentModesKHR(physical_device_, surface_, &mode_count, modes.data());
    
    VkSurfaceFormatKHR surface_format = choose_surface_format(formats);
    VkPresentModeKHR present_mode = choose_present_mode(modes);
    VkExtent2D extent = choose_swap_extent(capabilities, width, height);
    
    swapchain_format_ = surface_format.format;
    swapchain_extent_ = extent;
    
    uint32_t image_count = capabilities.minImageCount + 1;
    if (capabilities.maxImageCount > 0 && image_count > capabilities.maxImageCount) {
        image_count = capabilities.maxImageCount;
    }
    
    VkSwapchainCreateInfoKHR create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    create_info.surface = surface_;
    create_info.minImageCount = image_count;
    create_info.imageFormat = surface_format.format;
    create_info.imageColorSpace = surface_format.colorSpace;
    create_info.imageExtent = extent;
    create_info.imageArrayLayers = 1;
    create_info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    
    uint32_t queue_indices[] = {graphics_family_, present_family_};
    if (graphics_family_ != present_family_) {
        create_info.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        create_info.queueFamilyIndexCount = 2;
        create_info.pQueueFamilyIndices = queue_indices;
    } else {
        create_info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }
    
    create_info.preTransform = capabilities.currentTransform;
    create_info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    create_info.presentMode = present_mode;
    create_info.clipped = VK_TRUE;
    create_info.oldSwapchain = VK_NULL_HANDLE;
    
    if (vkCreateSwapchainKHR(device_, &create_info, nullptr, &swapchain_) != VK_SUCCESS) {
        std::cerr << "Failed to create swapchain\n";
        return false;
    }
    
    vkGetSwapchainImagesKHR(device_, swapchain_, &image_count, nullptr);
    swapchain_images_.resize(image_count);
    vkGetSwapchainImagesKHR(device_, swapchain_, &image_count, swapchain_images_.data());
    
    return true;
}

bool VulkanBootstrap::create_image_views() {
    swapchain_image_views_.resize(swapchain_images_.size());
    
    for (size_t i = 0; i < swapchain_images_.size(); ++i) {
        VkImageViewCreateInfo create_info{};
        create_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        create_info.image = swapchain_images_[i];
        create_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        create_info.format = swapchain_format_;
        create_info.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
        create_info.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
        create_info.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
        create_info.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
        create_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        create_info.subresourceRange.baseMipLevel = 0;
        create_info.subresourceRange.levelCount = 1;
        create_info.subresourceRange.baseArrayLayer = 0;
        create_info.subresourceRange.layerCount = 1;
        
        if (vkCreateImageView(device_, &create_info, nullptr, &swapchain_image_views_[i]) != VK_SUCCESS) {
            std::cerr << "Failed to create image view\n";
            return false;
        }
    }
    return true;
}

bool VulkanBootstrap::create_command_pool() {
    VkCommandPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = graphics_family_;
    
    if (vkCreateCommandPool(device_, &pool_info, nullptr, &command_pool_) != VK_SUCCESS) {
        std::cerr << "Failed to create command pool\n";
        return false;
    }
    return true;
}

bool VulkanBootstrap::create_command_buffers() {
    command_buffers_.resize(MAX_FRAMES_IN_FLIGHT);
    
    VkCommandBufferAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc_info.commandPool = command_pool_;
    alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc_info.commandBufferCount = static_cast<uint32_t>(command_buffers_.size());
    
    if (vkAllocateCommandBuffers(device_, &alloc_info, command_buffers_.data()) != VK_SUCCESS) {
        std::cerr << "Failed to allocate command buffers\n";
        return false;
    }
    return true;
}

bool VulkanBootstrap::create_sync_objects() {
    frame_sync_.resize(MAX_FRAMES_IN_FLIGHT);
    
    VkSemaphoreCreateInfo sem_info{};
    sem_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    
    VkFenceCreateInfo fence_info{};
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;  // Start signaled
    
    for (auto& sync : frame_sync_) {
        if (vkCreateSemaphore(device_, &sem_info, nullptr, &sync.image_available) != VK_SUCCESS ||
            vkCreateSemaphore(device_, &sem_info, nullptr, &sync.render_finished) != VK_SUCCESS ||
            vkCreateFence(device_, &fence_info, nullptr, &sync.in_flight) != VK_SUCCESS) {
            std::cerr << "Failed to create sync objects\n";
            return false;
        }
    }
    return true;
}

bool VulkanBootstrap::check_validation_layer_support() {
    uint32_t count;
    vkEnumerateInstanceLayerProperties(&count, nullptr);
    std::vector<VkLayerProperties> available(count);
    vkEnumerateInstanceLayerProperties(&count, available.data());
    
    for (const char* layer : VALIDATION_LAYERS) {
        bool found = false;
        for (const auto& props : available) {
            if (strcmp(layer, props.layerName) == 0) {
                found = true;
                break;
            }
        }
        if (!found) return false;
    }
    return true;
}

bool VulkanBootstrap::check_device_extension_support(VkPhysicalDevice device) {
    uint32_t count;
    vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> available(count);
    vkEnumerateDeviceExtensionProperties(device, nullptr, &count, available.data());
    
    std::set<std::string> required(DEVICE_EXTENSIONS.begin(), DEVICE_EXTENSIONS.end());
    for (const auto& ext : available) {
        required.erase(ext.extensionName);
    }
    return required.empty();
}

int VulkanBootstrap::rate_device_suitability(VkPhysicalDevice device) {
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(device, &props);
    
    // Require Vulkan 1.3
    if (props.apiVersion < VK_API_VERSION_1_3) {
        return -1;
    }
    
    // Check Vulkan 1.3 features (dynamic rendering, synchronization2)
    VkPhysicalDeviceVulkan13Features vulkan13_features{};
    vulkan13_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    
    VkPhysicalDeviceFeatures2 features2{};
    features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features2.pNext = &vulkan13_features;
    vkGetPhysicalDeviceFeatures2(device, &features2);
    
    if (!vulkan13_features.dynamicRendering || !vulkan13_features.synchronization2) {
        return -1;
    }
    
    // Check required features
    uint32_t graphics, present;
    if (!find_queue_families(device, graphics, present)) return -1;
    if (!check_device_extension_support(device)) return -1;
    
    // Check swapchain support
    uint32_t format_count, mode_count;
    vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface_, &format_count, nullptr);
    vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface_, &mode_count, nullptr);
    if (format_count == 0 || mode_count == 0) return -1;
    
    // Score
    int score = 0;
    if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
        score += 1000;
    }
    score += props.limits.maxImageDimension2D;
    
    return score;
}

bool VulkanBootstrap::find_queue_families(VkPhysicalDevice device, uint32_t& graphics, uint32_t& present) {
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, families.data());
    
    bool found_graphics = false, found_present = false;
    
    for (uint32_t i = 0; i < count; ++i) {
        if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            graphics = i;
            found_graphics = true;
        }
        
        VkBool32 present_support = false;
        vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface_, &present_support);
        if (present_support) {
            present = i;
            found_present = true;
        }
        
        if (found_graphics && found_present) break;
    }
    
    return found_graphics && found_present;
}

VkSurfaceFormatKHR VulkanBootstrap::choose_surface_format(const std::vector<VkSurfaceFormatKHR>& formats) {
    for (const auto& format : formats) {
        if (format.format == VK_FORMAT_B8G8R8A8_SRGB &&
            format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            return format;
        }
    }
    return formats[0];
}

VkPresentModeKHR VulkanBootstrap::choose_present_mode(const std::vector<VkPresentModeKHR>& modes) {
    if (!config_.vsync) {
        // Prefer mailbox (triple buffer) for uncapped
        for (auto mode : modes) {
            if (mode == VK_PRESENT_MODE_MAILBOX_KHR) return mode;
        }
        // Fall back to immediate
        for (auto mode : modes) {
            if (mode == VK_PRESENT_MODE_IMMEDIATE_KHR) return mode;
        }
    }
    // Default to FIFO (vsync)
    return VK_PRESENT_MODE_FIFO_KHR;
}

VkExtent2D VulkanBootstrap::choose_swap_extent(
    const VkSurfaceCapabilitiesKHR& capabilities,
    uint32_t width, uint32_t height
) {
    if (capabilities.currentExtent.width != std::numeric_limits<uint32_t>::max()) {
        return capabilities.currentExtent;
    }
    
    VkExtent2D extent = {width, height};
    extent.width = std::clamp(extent.width, 
        capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
    extent.height = std::clamp(extent.height,
        capabilities.minImageExtent.height, capabilities.maxImageExtent.height);
    return extent;
}

} // namespace enginemon
