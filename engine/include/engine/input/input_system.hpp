#pragma once
// engine/input/input_system.hpp
// Input abstraction layer for SDL3 (and other backends)
//
// Keeps simulation logic independent from SDL.
// Remappable bindings structure.
//
// Reference: Gen2Recomped joyLatch pattern for held-direction gating

#include "engine/core/types.hpp"
#include "engine/core/game_loop.hpp"  // For InputAction
#include <cstdint>
#include <string>
#include <unordered_map>
#include <functional>
#include <optional>

namespace enginemon {

//=============================================================================
// INPUT STATE
// Raw button/key states
//=============================================================================

enum class InputButton {
    // D-pad / movement
    Up,
    Down,
    Left,
    Right,
    
    // Face buttons
    A,          // Interact, confirm
    B,          // Cancel, run
    
    // System
    Start,      // Menu
    Select,     // Registered item (GSC)
    
    Count
};

//=============================================================================
// KEY BINDINGS
// Remappable keyboard → button mapping
//=============================================================================

struct KeyBinding {
    int scancode;           // SDL scancode (or platform key code)
    InputButton button;
};

struct InputBindings {
    // Default keyboard bindings
    std::unordered_map<int, InputButton> keyboard;
    
    // Gamepad button bindings (SDL_GamepadButton values)
    std::unordered_map<int, InputButton> gamepad;
    
    // Initialize with defaults
    void set_defaults();
    
    // Rebind
    void bind_key(int scancode, InputButton button);
    void bind_gamepad(int button_id, InputButton button);
    
    // Lookup
    std::optional<InputButton> get_button_for_key(int scancode) const;
    std::optional<InputButton> get_button_for_gamepad(int button_id) const;
};

//=============================================================================
// INPUT STATE SNAPSHOT
// Current frame's input state
//=============================================================================

struct InputSnapshot {
    // Button states (indexed by InputButton)
    bool held[static_cast<int>(InputButton::Count)] = {};
    bool pressed[static_cast<int>(InputButton::Count)] = {};   // Just pressed this frame
    bool released[static_cast<int>(InputButton::Count)] = {};  // Just released this frame
    
    // Helpers
    bool is_held(InputButton btn) const { return held[static_cast<int>(btn)]; }
    bool was_pressed(InputButton btn) const { return pressed[static_cast<int>(btn)]; }
    bool was_released(InputButton btn) const { return released[static_cast<int>(btn)]; }
    
    // Direction helpers
    bool any_direction_held() const {
        return is_held(InputButton::Up) || is_held(InputButton::Down) ||
               is_held(InputButton::Left) || is_held(InputButton::Right);
    }
    
    std::optional<Direction> held_direction() const {
        // Priority: most recent press wins, or up > down > left > right
        if (is_held(InputButton::Up)) return Direction::Up;
        if (is_held(InputButton::Down)) return Direction::Down;
        if (is_held(InputButton::Left)) return Direction::Left;
        if (is_held(InputButton::Right)) return Direction::Right;
        return std::nullopt;
    }
};

//=============================================================================
// INPUT SYSTEM
// Manages input state and converts to game actions
//=============================================================================

class InputSystem {
public:
    InputSystem();
    ~InputSystem();
    
    //=========================================================================
    // BINDINGS
    //=========================================================================
    
    InputBindings& bindings() { return bindings_; }
    const InputBindings& bindings() const { return bindings_; }
    
    //=========================================================================
    // STATE UPDATE
    // Call once per frame before processing input
    //=========================================================================
    
    // Begin new frame (clears pressed/released)
    void begin_frame();
    
    // Process raw key event
    void on_key_down(int scancode);
    void on_key_up(int scancode);
    
    // Process raw gamepad event
    void on_gamepad_button_down(int button_id);
    void on_gamepad_button_up(int button_id);
    
    //=========================================================================
    // INPUT QUERIES
    //=========================================================================
    
    const InputSnapshot& snapshot() const { return snapshot_; }
    
    // Convert current input to game action
    // Reference: Gen2Recomped handleInput() gating logic
    InputAction get_action(bool input_locked) const;
    
    //=========================================================================
    // JOYPAD LATCH
    // From Gen2Recomped: mid-step button presses are held until landing
    //=========================================================================
    
    // Latch a button press during movement
    void latch_button(InputButton btn);
    
    // Clear latched buttons (on step landing)
    void clear_latch();
    
    // Check if button was latched and is still held
    bool check_latch(InputButton btn) const;

private:
    InputBindings bindings_;
    InputSnapshot snapshot_;
    
    // Previous frame state for edge detection
    bool prev_held_[static_cast<int>(InputButton::Count)] = {};
    
    // Joypad latch (buttons pressed mid-step)
    bool latched_[static_cast<int>(InputButton::Count)] = {};
    
    // Set button state
    void set_button(InputButton btn, bool down);
};

//=============================================================================
// SDL3 SCANCODE DEFAULTS
// From SDL3 SDL_Scancode enum
//=============================================================================

namespace Sdl3Scancode {
    // Common scancodes for default bindings
    constexpr int W = 26;
    constexpr int A = 4;
    constexpr int S = 22;
    constexpr int D = 7;
    constexpr int UP = 82;
    constexpr int DOWN = 81;
    constexpr int LEFT = 80;
    constexpr int RIGHT = 79;
    constexpr int Z = 29;       // A button
    constexpr int X = 27;       // B button
    constexpr int RETURN = 40;  // Start
    constexpr int RSHIFT = 229; // Select
    constexpr int SPACE = 44;   // A button alt
}

//=============================================================================
// SDL3 GAMEPAD DEFAULTS
// From SDL3 SDL_GamepadButton enum
//=============================================================================

namespace Sdl3Gamepad {
    constexpr int DPAD_UP = 11;
    constexpr int DPAD_DOWN = 12;
    constexpr int DPAD_LEFT = 13;
    constexpr int DPAD_RIGHT = 14;
    constexpr int A = 0;
    constexpr int B = 1;
    constexpr int START = 6;
    constexpr int BACK = 4;  // Select
}

} // namespace enginemon
