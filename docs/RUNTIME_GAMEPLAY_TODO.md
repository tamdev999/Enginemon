# Runtime Gameplay TODO

Deferred gameplay and runtime issues documented during pre-RNG correctness passes.

---

## A. Pokémon DV RNG Consumption

**Status**: Deferred

**Current Behavior**:
`create_pokemon()` and `create_wild_pokemon()` consume **four** RNG draws for four DVs:
```cpp
dvs.attack = rng.next() & 0x0F;
dvs.defense = rng.next() & 0x0F;
dvs.speed = rng.next() & 0x0F;
dvs.special = rng.next() & 0x0F;
```

**Crystal Semantic Behavior**:
Crystal consumes **two** random bytes:
- Byte 1: Attack (high nibble) / Defense (low nibble)
- Byte 2: Speed (high nibble) / Special (low nibble)

**Reason Deferred**:
Fix during native RNG implementation so canonical draw accounting changes only once. Fixing now would require a second RNG accounting change when PCG is implemented.

**Intended Milestone**: Native RNG Migration (PCG-XSH-RR implementation)

**Relevant Files**:
- `engine/party/pokemon.cpp` - `create_pokemon()`, `create_wild_pokemon()`

**Invariant**: After fix, DV generation must match Crystal's 2-draw semantics exactly.

---

## B. Duplicate Physical Bindings / Phantom Releases

**Status**: Deferred

**Current Behavior**:
Multiple physical inputs can map to one logical button. Releasing one physical source may clear the logical held state while another source remains held.

**Example**:
- W and ↑ both map to `MoveUp`
- User holds W, then holds ↑, then releases W
- Current: `MoveUp` may become released despite ↑ still held
- Expected: `MoveUp` remains held until all physical sources released

**Intended Fix**:
Track physical-source state or per-logical held-source count/set, then derive logical state.

**Reason Deferred**:
Input-system correctness cleanup after RNG unless it becomes blocking for gameplay testing.

**Intended Milestone**: Input System Correctness Cleanup

**Relevant Files**:
- `engine/input/input_system.cpp`
- `engine/include/engine/input/input_system.hpp`

**Invariant**: Logical button state must reflect the union of all physical source states.

---

## C. Surf / Whirlpool Capability Semantics

**Status**: Deferred (Unfinished Feature)

**Current Behavior**:
- Player `surfing` state exists but is hardcoded `false` in active collision paths
- Whirlpool tiles are currently treated as ordinary swimmable water
- No capability check distinguishes Surf vs Whirlpool

**Intended Model**:
Traversal capability must distinguish:
- Walking (default)
- Surfing (requires Surf HM + badge)
- Whirlpool capability (requires Whirlpool HM + badge)
- Any later terrain-specific requirements

**Reason Deferred**:
Field-move mechanics are a larger feature set. Current collision system handles land traversal correctly.

**Intended Milestone**: Surf / Field-Movement Mechanics Implementation

**Relevant Files**:
- `engine/world/collision.cpp` - `check_tile()` surfing parameter
- `engine/include/engine/core/game_loop.hpp` - `PlayerState::surfing`
- `engine/include/engine/world/collision_types.hpp` - Water/Whirlpool classes

**Invariant**: After fix, water traversal requires Surf, Whirlpool tiles require Whirlpool move.

---

## D. ScriptYielded Input Ownership

**Status**: Deferred (API Footgun)

**Current Behavior**:
`ScriptYielded` remains an active coroutine state. Normal input gating checks for `ScriptRunning` but may not treat `ScriptYielded` identically.

From `HeadlessGameLoop::is_input_locked()`:
```cpp
return state_ == LoopState::Moving || state_ == LoopState::ScriptRunning;
// ScriptYielded is NOT included
```

**Current Mitigation**:
Visual runtime handles this at the presentation layer.

**Intended Fix**:
Clarify whether `ScriptYielded` should block input (if script owns the interaction) or allow input (if yielding for user choice). The distinction matters for:
- Dialog yields (script owns interaction)
- Movement yields (may allow cancel?)
- Choice yields (script waits for user input)

**Reason Deferred**:
Requires design decision on yield-type-specific input ownership. Current behavior works for typical dialog scripts.

**Intended Milestone**: Script / Input Ownership Cleanup

**Relevant Files**:
- `engine/core/game_loop.cpp` - `is_input_locked()`
- `engine/include/engine/core/game_loop.hpp` - `LoopState` enum

**Invariant**: After fix, input ownership during yields must be explicitly defined per yield type.

---

## E. Determinism Hash

**Status**: Deferred (Not a Bug)

**Current Behavior**:
`state_hash()` provides a narrow diagnostic hash of player position and loop state:
```cpp
uint64_t HeadlessGameLoop::state_hash() const {
    uint64_t hash = 0;
    hash ^= static_cast<uint64_t>(player_.x) << 0;
    hash ^= static_cast<uint64_t>(player_.y) << 8;
    hash ^= static_cast<uint64_t>(player_.facing) << 16;
    hash ^= static_cast<uint64_t>(player_.is_moving) << 20;
    hash ^= static_cast<uint64_t>(state_) << 24;
    return hash;
}
```

**Intended Model**:
Eventual authoritative determinism fingerprint derives from canonical authoritative serialization of the complete `GameState`, not a partial hash.

**Reason Deferred**:
Current `state_hash()` is useful for quick sanity checks. Full determinism verification via serialization comparison exists.

**Intended Milestone**: No specific milestone - enhancement when needed

**Relevant Files**:
- `engine/core/game_loop.cpp` - `state_hash()`
- `engine/core/game_state.cpp` - `serialize()` / `deserialize()`

**Invariant**: Authoritative determinism must use full state serialization comparison.

---

## Summary

| ID | Issue | Milestone | Blocking |
|----|-------|-----------|----------|
| A | DV RNG Consumption | Native RNG | No |
| B | Phantom Releases | Input Cleanup | No |
| C | Surf/Whirlpool | Field Moves | No |
| D | ScriptYielded Input | Script/Input | No |
| E | Determinism Hash | Enhancement | No |

None of these issues are blocking for the current development phase. All will be addressed in their respective milestone passes.
