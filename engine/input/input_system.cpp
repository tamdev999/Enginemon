// engine/input/input_system.cpp
// Input system implementation for SDL3 (and other backends)
//
// Reference: Gen2Recomped joyLatch pattern for held-direction gating

#include "engine/input/input_system.hpp"
#include <algorithm>

namespace enginemon {

//=============================================================================
// INPUT BINDINGS
//=============================================================================

void InputBindings::set_defaults() {
    // Clear existing
    keyboard.clear();
    gamepad.clear();
    
    // Keyboard defaults (WASD + Arrow keys)
    keyboard[Sdl3Scancode::W] = InputButton::Up;
    keyboard[Sdl3Scancode::S] = InputButton::Down;
    keyboard[Sdl3Scancode::A] = InputButton::Left;
    keyboard[Sdl3Scancode::D] = InputButton::Right;
    
    keyboard[Sdl3Scancode::UP] = InputButton::Up;
    keyboard[Sdl3Scancode::DOWN] = InputButton::Down;
    keyboard[Sdl3Scancode::LEFT] = InputButton::Left;
    keyboard[Sdl3Scancode::RIGHT] = InputButton::Right;
    
    keyboard[Sdl3Scancode::Z] = InputButton::A;
    keyboard[Sdl3Scancode::SPACE] = InputButton::A;
    keyboard[Sdl3Scancode::X] = InputButton::B;
    keyboard[Sdl3Scancode::RETURN] = InputButton::Start;
    keyboard[Sdl3Scancode::RSHIFT] = InputButton::Select;
    
    // Gamepad defaults (SDL3 gamepad standard)
    gamepad[Sdl3Gamepad::DPAD_UP] = InputButton::Up;
    gamepad[Sdl3Gamepad::DPAD_DOWN] = InputButton::Down;
    gamepad[Sdl3Gamepad::DPAD_LEFT] = InputButton::Left;
    gamepad[Sdl3Gamepad::DPAD_RIGHT] = InputButton::Right;
    
    gamepad[Sdl3Gamepad::A] = InputButton::A;
    gamepad[Sdl3Gamepad::B] = InputButton::B;
    gamepad[Sdl3Gamepad::START] = InputButton::Start;
    gamepad[Sdl3Gamepad::BACK] = InputButton::Select;
}

void InputBindings::bind_key(int scancode, InputButton button) {
    keyboard[scancode] = button;
}

void InputBindings::bind_gamepad(int button_id, InputButton button) {
    gamepad[button_id] = button;
}

std::optional<InputButton> InputBindings::get_button_for_key(int scancode) const {
    auto it = keyboard.find(scancode);
    if (it != keyboard.end()) {
        return it->second;
    }
    return std::nullopt;
}

std::optional<InputButton> InputBindings::get_button_for_gamepad(int button_id) const {
    auto it = gamepad.find(button_id);
    if (it != gamepad.end()) {
        return it->second;
    }
    return std::nullopt;
}

//=============================================================================
// INPUT SYSTEM
//=============================================================================

InputSystem::InputSystem() {
    bindings_.set_defaults();
    
    // Clear all state
    for (int i = 0; i < static_cast<int>(InputButton::Count); i++) {
        snapshot_.held[i] = false;
        snapshot_.pressed[i] = false;
        snapshot_.released[i] = false;
        prev_held_[i] = false;
        latched_[i] = false;
    }
}

InputSystem::~InputSystem() = default;

//=============================================================================
// STATE UPDATE
//=============================================================================

void InputSystem::begin_frame() {
    // Copy current held state to previous
    for (int i = 0; i < static_cast<int>(InputButton::Count); i++) {
        prev_held_[i] = snapshot_.held[i];
        snapshot_.pressed[i] = false;
        snapshot_.released[i] = false;
    }
}

void InputSystem::set_button(InputButton btn, bool down) {
    int idx = static_cast<int>(btn);
    
    if (down && !snapshot_.held[idx]) {
        // Just pressed
        snapshot_.pressed[idx] = true;
    } else if (!down && snapshot_.held[idx]) {
        // Just released
        snapshot_.released[idx] = true;
    }
    
    snapshot_.held[idx] = down;
}

void InputSystem::on_key_down(int scancode) {
    auto btn = bindings_.get_button_for_key(scancode);
    if (btn.has_value()) {
        set_button(btn.value(), true);
    }
}

void InputSystem::on_key_up(int scancode) {
    auto btn = bindings_.get_button_for_key(scancode);
    if (btn.has_value()) {
        set_button(btn.value(), false);
    }
}

void InputSystem::on_gamepad_button_down(int button_id) {
    auto btn = bindings_.get_button_for_gamepad(button_id);
    if (btn.has_value()) {
        set_button(btn.value(), true);
    }
}

void InputSystem::on_gamepad_button_up(int button_id) {
    auto btn = bindings_.get_button_for_gamepad(button_id);
    if (btn.has_value()) {
        set_button(btn.value(), false);
    }
}

//=============================================================================
// INPUT QUERIES
//=============================================================================

InputAction InputSystem::get_action(bool input_locked) const {
    // Reference: Gen2Recomped handleInput() gating logic
    // When input is locked (moving or script), most inputs are rejected
    
    if (input_locked) {
        // Only interactions can be latched for next available frame
        // Movement held is remembered via latch system
        return InputAction::None;
    }
    
    // A-button has highest priority (interact/confirm)
    if (snapshot_.was_pressed(InputButton::A)) {
        return InputAction::Interact;
    }
    
    // Movement (check latched first, then held)
    // Direction priority: up > down > left > right (arbitrary but consistent)
    if (check_latch(InputButton::Up) || snapshot_.is_held(InputButton::Up)) {
        return InputAction::MoveUp;
    }
    if (check_latch(InputButton::Down) || snapshot_.is_held(InputButton::Down)) {
        return InputAction::MoveDown;
    }
    if (check_latch(InputButton::Left) || snapshot_.is_held(InputButton::Left)) {
        return InputAction::MoveLeft;
    }
    if (check_latch(InputButton::Right) || snapshot_.is_held(InputButton::Right)) {
        return InputAction::MoveRight;
    }
    
    return InputAction::None;
}

//=============================================================================
// JOYPAD LATCH
//=============================================================================

void InputSystem::latch_button(InputButton btn) {
    latched_[static_cast<int>(btn)] = true;
}

void InputSystem::clear_latch() {
    for (int i = 0; i < static_cast<int>(InputButton::Count); i++) {
        latched_[i] = false;
    }
}

bool InputSystem::check_latch(InputButton btn) const {
    // Return true if button was latched AND is still held
    int idx = static_cast<int>(btn);
    return latched_[idx] && snapshot_.held[idx];
}

} // namespace enginemon
