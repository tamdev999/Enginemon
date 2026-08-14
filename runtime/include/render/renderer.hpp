#pragma once
// render/renderer.hpp
// Vulkan-based rendering for 2D, 3D, and 3D+ modes
// All modes consume the same native game state

#include "engine/core/types.hpp"
#include <memory>
#include <cstdint>

struct SDL_Window;

namespace enginemon {

class World;
class GameDefinition;

// Render mode
enum class RenderMode {
    Mode2D,     // Classic tile-based
    Mode3D,     // Voxel/extruded diorama
    Mode3DPlus  // 3D with RT shadows/reflections
};

// Camera for 3D modes
struct Camera3D {
    float x, y, z;              // Position
    float pitch, yaw;           // Rotation
    float fov = 60.0f;
    float near_plane = 0.1f;
    float far_plane = 10000.0f;  // Long sightlines
};

// Renderer configuration
struct RendererConfig {
    RenderMode mode = RenderMode::Mode2D;
    uint32_t width = 1280;
    uint32_t height = 720;
    bool vsync = true;
    bool fullscreen = false;
    
    // 2D specific
    uint32_t scale_2d = 4;      // Pixel scaling
    
    // 3D specific
    float voxel_scale = 1.0f;
    bool enable_shadows = true;
    bool enable_ao = true;
    
    // 3D+ / RT specific
    bool enable_rt_shadows = false;
    bool enable_rt_reflections = false;
    bool enable_rt_ao = false;
};

// Main renderer interface
class Renderer {
public:
    virtual ~Renderer() = default;
    
    // Lifecycle
    virtual bool initialize(SDL_Window* window, const RendererConfig& config) = 0;
    virtual void shutdown() = 0;
    
    // Frame
    virtual void begin_frame() = 0;
    virtual void end_frame() = 0;
    
    // World rendering
    virtual void render_world(const World& world, const GameDefinition& game) = 0;
    
    // UI rendering (always 2D overlay)
    virtual void render_ui() = 0;
    
    // Mode switching
    virtual void set_mode(RenderMode mode) = 0;
    virtual RenderMode mode() const = 0;
    
    // Camera (3D modes)
    virtual void set_camera(const Camera3D& camera) = 0;
    virtual const Camera3D& camera() const = 0;
    
    // Resize
    virtual void resize(uint32_t width, uint32_t height) = 0;
    
    // Configuration
    virtual void apply_config(const RendererConfig& config) = 0;
    virtual const RendererConfig& config() const = 0;
    
    // Factory
    static std::unique_ptr<Renderer> create();
};

// 2D renderer implementation
class Renderer2D : public Renderer {
public:
    bool initialize(SDL_Window* window, const RendererConfig& config) override;
    void shutdown() override;
    void begin_frame() override;
    void end_frame() override;
    void render_world(const World& world, const GameDefinition& game) override;
    void render_ui() override;
    void set_mode(RenderMode mode) override;
    RenderMode mode() const override { return RenderMode::Mode2D; }
    void set_camera(const Camera3D& camera) override;
    const Camera3D& camera() const override;
    void resize(uint32_t width, uint32_t height) override;
    void apply_config(const RendererConfig& config) override;
    const RendererConfig& config() const override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// 3D renderer implementation (voxel diorama style)
class Renderer3D : public Renderer {
public:
    bool initialize(SDL_Window* window, const RendererConfig& config) override;
    void shutdown() override;
    void begin_frame() override;
    void end_frame() override;
    void render_world(const World& world, const GameDefinition& game) override;
    void render_ui() override;
    void set_mode(RenderMode mode) override;
    RenderMode mode() const override { return mode_; }
    void set_camera(const Camera3D& camera) override;
    const Camera3D& camera() const override;
    void resize(uint32_t width, uint32_t height) override;
    void apply_config(const RendererConfig& config) override;
    const RendererConfig& config() const override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    RenderMode mode_ = RenderMode::Mode3D;
};

} // namespace enginemon
