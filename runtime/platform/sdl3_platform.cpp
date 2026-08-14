// runtime/platform/sdl3_platform.cpp
// SDL3 platform implementation

#include "platform/sdl3_platform.hpp"
#include <vulkan/vulkan.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <iostream>

namespace enginemon {

Sdl3Platform::Sdl3Platform() = default;

Sdl3Platform::~Sdl3Platform() {
    shutdown();
}

bool Sdl3Platform::initialize(const WindowConfig& config) {
    if (initialized_) return true;
    
    // Initialize SDL3 with video subsystem
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        std::cerr << "SDL3 init failed: " << SDL_GetError() << "\n";
        return false;
    }
    
    // Create window with Vulkan support
    SDL_WindowFlags flags = SDL_WINDOW_VULKAN;
    if (config.resizable) {
        flags |= SDL_WINDOW_RESIZABLE;
    }
    if (config.fullscreen) {
        flags |= SDL_WINDOW_FULLSCREEN;
    }
    
    window_ = SDL_CreateWindow(
        config.title.c_str(),
        static_cast<int>(config.width),
        static_cast<int>(config.height),
        flags
    );
    
    if (!window_) {
        std::cerr << "SDL3 window creation failed: " << SDL_GetError() << "\n";
        SDL_Quit();
        return false;
    }
    
    width_ = config.width;
    height_ = config.height;
    initialized_ = true;
    
    return true;
}

void Sdl3Platform::shutdown() {
    if (!initialized_) return;
    
    if (window_) {
        SDL_DestroyWindow(window_);
        window_ = nullptr;
    }
    
    SDL_Quit();
    initialized_ = false;
}

std::vector<const char*> Sdl3Platform::get_required_vulkan_extensions() const {
    uint32_t count = 0;
    const char* const* extensions = SDL_Vulkan_GetInstanceExtensions(&count);
    
    std::vector<const char*> result;
    if (extensions) {
        for (uint32_t i = 0; i < count; ++i) {
            result.push_back(extensions[i]);
        }
    }
    return result;
}

VkSurfaceKHR Sdl3Platform::create_vulkan_surface(VkInstance instance) const {
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    if (!SDL_Vulkan_CreateSurface(window_, instance, nullptr, &surface)) {
        std::cerr << "SDL3 Vulkan surface creation failed: " << SDL_GetError() << "\n";
        return VK_NULL_HANDLE;
    }
    return surface;
}

PlatformEvent Sdl3Platform::poll_events(InputSystem& input) {
    PlatformEvent result = PlatformEvent::None;
    
    // Begin new input frame
    input.begin_frame();
    
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
            case SDL_EVENT_QUIT:
                result = PlatformEvent::Quit;
                break;
                
            case SDL_EVENT_WINDOW_RESIZED:
            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
                width_ = static_cast<uint32_t>(event.window.data1);
                height_ = static_cast<uint32_t>(event.window.data2);
                resized_ = true;
                if (result != PlatformEvent::Quit) {
                    result = PlatformEvent::Resized;
                }
                break;
                
            case SDL_EVENT_WINDOW_FOCUS_LOST:
                if (result == PlatformEvent::None) {
                    result = PlatformEvent::FocusLost;
                }
                break;
                
            case SDL_EVENT_WINDOW_FOCUS_GAINED:
                if (result == PlatformEvent::None) {
                    result = PlatformEvent::FocusGained;
                }
                break;
                
            case SDL_EVENT_KEY_DOWN:
                if (!event.key.repeat) {
                    input.on_key_down(static_cast<int>(event.key.scancode));
                }
                break;
                
            case SDL_EVENT_KEY_UP:
                input.on_key_up(static_cast<int>(event.key.scancode));
                break;
                
            case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
                input.on_gamepad_button_down(static_cast<int>(event.gbutton.button));
                break;
                
            case SDL_EVENT_GAMEPAD_BUTTON_UP:
                input.on_gamepad_button_up(static_cast<int>(event.gbutton.button));
                break;
        }
    }
    
    return result;
}

uint64_t Sdl3Platform::get_ticks_ms() const {
    return SDL_GetTicks();
}

void Sdl3Platform::delay_ms(uint32_t ms) const {
    SDL_Delay(ms);
}

} // namespace enginemon
