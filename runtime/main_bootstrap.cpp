// runtime/main_bootstrap.cpp
// Minimal SDL3 + Vulkan 1.3 bootstrap for Enginemon
//
// This is a standalone test for the presentation layer.
// - Opens SDL3 window with Vulkan
// - Creates Vulkan 1.3 instance, device, swapchain
// - Uses core dynamic rendering (Vulkan 1.3)
// - Clears and presents frames
// - Handles resize
// - Feeds SDL3 events to InputSystem
//
// Does NOT render anything yet - just proves the plumbing works.

#include "platform/sdl3_platform.hpp"
#include "render/vulkan_bootstrap.hpp"
#include "engine/input/input_system.hpp"
#include <iostream>
#include <cmath>

using namespace enginemon;

// Record a clear frame using dynamic rendering
void record_clear_frame(
    VkCommandBuffer cmd,
    VkImage swapchain_image,
    VkImageView swapchain_view,
    VkExtent2D extent,
    float time
) {
    // Transition image to color attachment
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = swapchain_image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier);
    
    // Begin dynamic rendering
    VkRenderingAttachmentInfo color_attachment{};
    color_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    color_attachment.imageView = swapchain_view;
    color_attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    
    // Animate clear color based on time
    float r = 0.1f + 0.05f * sinf(time);
    float g = 0.1f + 0.05f * sinf(time * 1.3f);
    float b = 0.2f + 0.1f * sinf(time * 0.7f);
    color_attachment.clearValue.color = {{r, g, b, 1.0f}};
    
    VkRenderingInfo render_info{};
    render_info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    render_info.renderArea.offset = {0, 0};
    render_info.renderArea.extent = extent;
    render_info.layerCount = 1;
    render_info.colorAttachmentCount = 1;
    render_info.pColorAttachments = &color_attachment;
    
    vkCmdBeginRendering(cmd, &render_info);
    // Nothing to draw yet - just clear
    vkCmdEndRendering(cmd);
    
    // Transition image to present
    barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.dstAccessMask = 0;
    
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier);
}

int main(int argc, char* argv[]) {
    std::cout << "Enginemon - SDL3 + Vulkan 1.3 Bootstrap\n";
    std::cout << "========================================\n";
    
    // Initialize platform
    Sdl3Platform platform;
    WindowConfig window_config;
    window_config.title = "Enginemon [Vulkan 1.3 Bootstrap]";
    window_config.width = 1280;
    window_config.height = 720;
    window_config.resizable = true;
    
    if (!platform.initialize(window_config)) {
        std::cerr << "Failed to initialize SDL3 platform\n";
        return 1;
    }
    std::cout << "SDL3 platform initialized\n";
    
    // Initialize Vulkan
    VulkanBootstrap vulkan;
    VulkanConfig vulkan_config;
    vulkan_config.enable_validation = true;
    vulkan_config.vsync = true;
    
    if (!vulkan.initialize(platform, vulkan_config)) {
        std::cerr << "Failed to initialize Vulkan\n";
        return 1;
    }
    std::cout << "Vulkan initialized\n";
    std::cout << "Swapchain: " << vulkan.swapchain_extent().width 
              << "x" << vulkan.swapchain_extent().height << "\n";
    
    // Initialize input system
    InputSystem input;
    std::cout << "Input system initialized\n";
    
    std::cout << "\nControls:\n";
    std::cout << "  WASD/Arrows - Movement (check console output)\n";
    std::cout << "  Z/Space - A button\n";
    std::cout << "  X - B button\n";
    std::cout << "  ESC - Quit\n\n";
    
    // Main loop
    bool running = true;
    uint64_t start_time = platform.get_ticks_ms();
    uint32_t frame_count = 0;
    uint64_t last_fps_time = start_time;
    
    while (running) {
        // Process events
        PlatformEvent event = platform.poll_events(input);
        
        if (event == PlatformEvent::Quit) {
            running = false;
            continue;
        }
        
        // Check for ESC key
        if (input.snapshot().was_pressed(InputButton::A)) {
            std::cout << "[Input] A button pressed\n";
        }
        if (input.snapshot().was_pressed(InputButton::B)) {
            std::cout << "[Input] B button pressed\n";
        }
        
        // Check movement - convert to InputAction like simulation would
        InputAction action = input.get_action(false);
        if (action != InputAction::None && action != InputAction::Interact) {
            const char* dir_name = "?";
            switch (action) {
                case InputAction::MoveUp: dir_name = "Up"; break;
                case InputAction::MoveDown: dir_name = "Down"; break;
                case InputAction::MoveLeft: dir_name = "Left"; break;
                case InputAction::MoveRight: dir_name = "Right"; break;
                default: break;
            }
            // Only print on press, not hold
            static InputAction last_action = InputAction::None;
            if (action != last_action) {
                std::cout << "[Input] Move " << dir_name << "\n";
            }
            last_action = action;
        }
        
        // Handle resize
        if (event == PlatformEvent::Resized || platform.was_resized()) {
            std::cout << "Window resized to " << platform.width() << "x" << platform.height() << "\n";
            vulkan.recreate_swapchain(platform.width(), platform.height());
            platform.clear_resize_flag();
        }
        
        // Begin frame
        if (!vulkan.begin_frame()) {
            // Swapchain out of date - recreate
            vulkan.recreate_swapchain(platform.width(), platform.height());
            continue;
        }
        
        // Get current swapchain image info
        uint32_t image_idx = vulkan.current_image_index();
        VkCommandBuffer cmd = vulkan.get_command_buffer();
        
        // Record clear frame
        float time = (platform.get_ticks_ms() - start_time) / 1000.0f;
        record_clear_frame(
            cmd,
            vulkan.swapchain_images()[image_idx],
            vulkan.swapchain_image_views()[image_idx],
            vulkan.swapchain_extent(),
            time
        );
        
        // End frame
        if (!vulkan.end_frame(cmd)) {
            // Swapchain out of date - recreate
            vulkan.recreate_swapchain(platform.width(), platform.height());
        }
        
        // FPS counter
        frame_count++;
        uint64_t now = platform.get_ticks_ms();
        if (now - last_fps_time >= 1000) {
            std::cout << "FPS: " << frame_count << "\n";
            frame_count = 0;
            last_fps_time = now;
        }
    }
    
    std::cout << "\nShutting down...\n";
    vulkan.shutdown();
    platform.shutdown();
    
    std::cout << "Done.\n";
    return 0;
}
