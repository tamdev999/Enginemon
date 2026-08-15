// engine/input/input_system.cpp
// Input system implementation for SDL3 (and other backends)
//
// Reference: Gen2Recomped joyLatch pattern for held-direction gating
//
// CRITICAL: Edge lifetime model (Audit 8)
// Host edges remain pending until consumed by simulation.
// Render frame boundaries do NOT discard unconsumed edges.
//
// Model:
//   key_down: held = true, pending_pressed = true
//   key_up:   held = false, pending_released = true
//   consume_pressed(): if pending_pressed { pending_pressed = false; return true }
//   consume_released(): if pending_released { pending_released = false; return true }
//
// One host rising edge → at most one simulation edge event.
// During catch-up (multiple simulation ticks per render), only the first
// tick observes the pressed edge. Subsequent ticks see held=true but pressed=false.

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
        held_[i] = false;
        pending_pressed_[i] = false;
        pending_released_[i] = false;
        latched_[i] = false;
    }
}

InputSystem::~InputSystem() = default;

//=============================================================================
// STATE UPDATE
//=============================================================================

void InputSystem::begin_frame() {
    // CRITICAL (Audit 8): Do NOT clear pending edges here!
    // Pending edges remain until consumed by simulation.
    // Only update the snapshot's held state for queries.
    for (int i = 0; i < static_cast<int>(InputButton::Count); i++) {
        snapshot_.held[i] = held_[i];
        // Expose pending state to snapshot for legacy was_pressed() queries
        snapshot_.pressed[i] = pending_pressed_[i];
        snapshot_.released[i] = pending_released_[i];
    }
}

void InputSystem::set_button(InputButton btn, bool down) {
    int idx = static_cast<int>(btn);
    
    if (down && !held_[idx]) {
        // Rising edge: key just pressed
        pending_pressed_[idx] = true;
        snapshot_.pressed[idx] = true;  // Immediately visible to queries
        // Note: If press→release→press occurs before simulation tick,
        // we keep pending_pressed true (latest state wins for Crystal semantics)
    } else if (!down && held_[idx]) {
        // Falling edge: key just released
        pending_released_[idx] = true;
        snapshot_.released[idx] = true;  // Immediately visible to queries
    }
    
    held_[idx] = down;
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
// EDGE CONSUMPTION (Audit 8)
// Host edges remain pending until consumed by simulation.
// One physical rising edge → at most one simulation edge event.
//=============================================================================

bool InputSystem::consume_pressed(InputButton btn) {
    int idx = static_cast<int>(btn);
    if (pending_pressed_[idx]) {
        pending_pressed_[idx] = false;  // Consumed - clears pending
        snapshot_.pressed[idx] = false; // Update snapshot
        return true;
    }
    return false;
}

bool InputSystem::consume_released(InputButton btn) {
    int idx = static_cast<int>(btn);
    if (pending_released_[idx]) {
        pending_released_[idx] = false;  // Consumed - clears pending
        snapshot_.released[idx] = false; // Update snapshot
        return true;
    }
    return false;
}

bool InputSystem::has_pending_pressed(InputButton btn) const {
    return pending_pressed_[static_cast<int>(btn)];
}

bool InputSystem::has_pending_released(InputButton btn) const {
    return pending_released_[static_cast<int>(btn)];
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
    // Use pending_pressed for edge detection
    if (pending_pressed_[static_cast<int>(InputButton::A)]) {
        return InputAction::Interact;
    }
    
    // Movement (check latched first, then held)
    // Direction priority: up > down > left > right (arbitrary but consistent)
    if (check_latch(InputButton::Up) || held_[static_cast<int>(InputButton::Up)]) {
        return InputAction::MoveUp;
    }
    if (check_latch(InputButton::Down) || held_[static_cast<int>(InputButton::Down)]) {
        return InputAction::MoveDown;
    }
    if (check_latch(InputButton::Left) || held_[static_cast<int>(InputButton::Left)]) {
        return InputAction::MoveLeft;
    }
    if (check_latch(InputButton::Right) || held_[static_cast<int>(InputButton::Right)]) {
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
    return latched_[idx] && held_[idx];
}

} // namespace enginemon
