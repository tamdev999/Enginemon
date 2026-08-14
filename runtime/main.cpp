// runtime/main.cpp
// Enginemon main entry point

#include "engine/core/game_definition.hpp"
#include "engine/scripting/lua_runtime.hpp"
#include "engine/mod/mod_manager.hpp"
#include "engine/world/world.hpp"
#include "engine/audio/audio_system.hpp"
#include "render/renderer.hpp"

#include <SDL3/SDL.h>
#include <iostream>
#include <filesystem>

using namespace enginemon;

namespace {

void print_usage(const char* program) {
    std::cout << "Usage: " << program << " <game_directory> [options]\n"
              << "\n"
              << "Options:\n"
              << "  --mode=2d|3d|3d+    Render mode (default: 2d)\n"
              << "  --fullscreen        Start in fullscreen\n"
              << "  --scale=N           2D pixel scale (default: 4)\n"
              << "  --no-mods           Disable mod loading\n"
              << "  --help              Show this help\n";
}

struct Options {
    std::filesystem::path game_path;
    RenderMode render_mode = RenderMode::Mode2D;
    bool fullscreen = false;
    uint32_t scale = 4;
    bool load_mods = true;
};

bool parse_args(int argc, char* argv[], Options& opts) {
    if (argc < 2) {
        return false;
    }
    
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        
        if (arg == "--help") {
            return false;
        } else if (arg == "--fullscreen") {
            opts.fullscreen = true;
        } else if (arg == "--no-mods") {
            opts.load_mods = false;
        } else if (arg.starts_with("--mode=")) {
            std::string mode = arg.substr(7);
            if (mode == "2d") opts.render_mode = RenderMode::Mode2D;
            else if (mode == "3d") opts.render_mode = RenderMode::Mode3D;
            else if (mode == "3d+") opts.render_mode = RenderMode::Mode3DPlus;
            else {
                std::cerr << "Unknown render mode: " << mode << "\n";
                return false;
            }
        } else if (arg.starts_with("--scale=")) {
            opts.scale = std::stoul(arg.substr(8));
        } else if (!arg.starts_with("-")) {
            opts.game_path = arg;
        }
    }
    
    return !opts.game_path.empty();
}

} // namespace

int main(int argc, char* argv[]) {
    Options opts;
    if (!parse_args(argc, argv, opts)) {
        print_usage(argv[0]);
        return 1;
    }
    
    // Initialize SDL
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD) < 0) {
        std::cerr << "SDL init failed: " << SDL_GetError() << "\n";
        return 1;
    }
    
    // Load game definition
    std::cout << "Loading game from: " << opts.game_path << "\n";
    auto game = GameDefinition::load(opts.game_path);
    if (!game) {
        std::cerr << "Failed to load game definition\n";
        SDL_Quit();
        return 1;
    }
    
    std::cout << "Game: " << game->metadata.name << " v" << game->metadata.version << "\n";
    
    // Initialize Lua runtime
    LuaRuntime lua;
    
    // Load mods
    ModManager mods;
    if (opts.load_mods) {
        auto mods_path = opts.game_path / "mods";
        if (std::filesystem::exists(mods_path)) {
            std::cout << "Scanning mods...\n";
            mods.scan_directory(mods_path);
            
            // Load enabled mods
            if (!mods.load_all(*game, lua)) {
                std::cerr << "Warning: Some mods failed to load\n";
            }
        }
    }
    
    // Freeze registries (no more modifications after this)
    game->registries.freeze_all();
    
    // Create window
    uint32_t window_flags = SDL_WINDOW_VULKAN;
    if (opts.fullscreen) {
        window_flags |= SDL_WINDOW_FULLSCREEN;
    }
    
    SDL_Window* window = SDL_CreateWindow(
        game->metadata.name.c_str(),
        1280, 720,
        window_flags
    );
    
    if (!window) {
        std::cerr << "Window creation failed: " << SDL_GetError() << "\n";
        SDL_Quit();
        return 1;
    }
    
    // Initialize renderer
    RendererConfig render_config;
    render_config.mode = opts.render_mode;
    render_config.fullscreen = opts.fullscreen;
    render_config.scale_2d = opts.scale;
    
    auto renderer = Renderer::create();
    if (!renderer->initialize(window, render_config)) {
        std::cerr << "Renderer initialization failed\n";
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    
    // Initialize audio
    AudioSystem audio;
    if (!audio.initialize()) {
        std::cerr << "Warning: Audio initialization failed\n";
    }
    
    // Initialize world
    World world;
    world.initialize(*game);
    
    // Bind Lua APIs
    // TODO: Create GameContext and bind
    
    // Main loop
    bool running = true;
    uint64_t last_time = SDL_GetTicks();
    
    while (running) {
        // Calculate delta time
        uint64_t current_time = SDL_GetTicks();
        float delta = (current_time - last_time) / 1000.0f;
        last_time = current_time;
        
        // Process events
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            switch (event.type) {
                case SDL_EVENT_QUIT:
                    running = false;
                    break;
                case SDL_EVENT_KEY_DOWN:
                    if (event.key.key == SDLK_ESCAPE) {
                        running = false;
                    }
                    // TODO: Handle input
                    break;
            }
        }
        
        // Update
        lua.update(delta);
        world.update_movement(delta);
        audio.update(delta);
        
        // Render
        renderer->begin_frame();
        renderer->render_world(world, *game);
        renderer->render_ui();
        renderer->end_frame();
    }
    
    // Cleanup
    audio.shutdown();
    renderer->shutdown();
    SDL_DestroyWindow(window);
    SDL_Quit();
    
    return 0;
}
