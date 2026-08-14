#pragma once
// runtime/platform/sdl3_platform.hpp
// SDL3 platform layer for window, input, and Vulkan surface
//
// Keeps SDL3 details isolated from engine simulation.
// Feeds events into the semantic InputSystem.

#include "engine/input/input_system.hpp"
#include <cstdint>
#include <string>
#include <vector>
#include <functional>

struct SDL_Window;
typedef struct VkInstance_T* VkInstance;
typedef struct VkSurfaceKHR_T* VkSurfaceKHR;

namespace enginemon {

// Platform event types
enum class PlatformEvent {
    None,
    Quit,
    Resized,
    FocusLost,
    FocusGained,
};

// Window configuration
struct WindowConfig {
    std::string title = "Enginemon";
    uint32_t width = 1280;
    uint32_t height = 720;
    bool fullscreen = false;
    bool resizable = true;
    bool vsync = true;
};

// SDL3 Platform layer
class Sdl3Platform {
public:
    Sdl3Platform();
    ~Sdl3Platform();
    
    // Lifecycle
    bool initialize(const WindowConfig& config);
    void shutdown();
    
    // Window
    SDL_Window* window() const { return window_; }
    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }
    bool was_resized() const { return resized_; }
    void clear_resize_flag() { resized_ = false; }
    
    // Vulkan surface creation
    std::vector<const char*> get_required_vulkan_extensions() const;
    VkSurfaceKHR create_vulkan_surface(VkInstance instance) const;
    
    // Event processing
    // Returns the most significant platform event this frame
    // Feeds keyboard/gamepad input into the provided InputSystem
    PlatformEvent poll_events(InputSystem& input);
    
    // Timing
    uint64_t get_ticks_ms() const;
    void delay_ms(uint32_t ms) const;

private:
    SDL_Window* window_ = nullptr;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    bool resized_ = false;
    bool initialized_ = false;
};

} // namespace enginemon
