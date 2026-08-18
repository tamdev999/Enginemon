# Runtime Gameplay TODO

Deferred gameplay and runtime issues documented during pre-RNG correctness passes.

---

## A. Pokémon DV RNG Consumption

**Status**: Deferred - Fix with PCG migration

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
Fix during native RNG implementation (PCG) so canonical draw accounting changes only once.
Fixing now would require a second RNG accounting change when PCG is implemented.

**Intended Milestone**: Native RNG Migration (PCG-XSH-RR implementation)

**Relevant Files**:
- `engine/party/pokemon.cpp` - `create_pokemon()`, `create_wild_pokemon()`

**Invariant**: After fix, DV generation must match Crystal's 2-draw semantics exactly.

---

## B. NPC Random-Walk LCG Low-Bit Bias

**Status**: Deferred - Verify disappearance with PCG

**Current Behavior**:
Legacy LCG `state = state * 1664525 + 1013904223` has low-bit correlation.
NPC direction choices use `r & 1`, `r & 3` which are the most biased bits.

**Example**:
- `choose_npc_direction()` uses `r & 1` for Y-only walk, `r & 3` for XY walk
- LCG bit 0 alternates deterministically for certain seed classes
- This creates observable directional bias in NPC movement

**Crystal Semantic Behavior**:
Crystal uses Division result `([hRandomAdd] * [hRandomSub]) / 256` which extracts high bits,
not low bits. The effective randomness quality differs.

**Reason Deferred**:
Do not introduce temporary "use high bits" patches for legacy LCG.
PCG-XSH-RR produces high-quality low bits. After migration, verify the bias disappears.

**Intended Milestone**: Native RNG Migration (PCG-XSH-RR implementation)

**Relevant Files**:
- `engine/core/game_loop.cpp` - `choose_npc_direction()`, `next_random()`
- `engine/include/engine/core/game_state.hpp` - `RngState::next()`

**Invariant**: After PCG, add adversarial random-walk distribution test.

---

## C. Surf / Whirlpool Capability Semantics

**Status**: Deferred - Unfinished Feature

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

## D. Determinism Hash Limitations

**Status**: Documented - Not a Bug

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
Eventual authoritative determinism fingerprint derives from canonical authoritative serialization 
of the complete `GameState`, not a partial hash.

**Reason Not Changed**:
Current `state_hash()` is useful for quick sanity checks. Full determinism verification via 
serialization comparison exists.

**Intended Milestone**: No specific milestone - enhancement when needed

**Relevant Files**:
- `engine/core/game_loop.cpp` - `state_hash()`
- `engine/core/game_state.cpp` - `serialize()` / `deserialize()`

**Invariant**: Authoritative determinism must use full state serialization comparison.

---

## Summary

| ID | Issue | Milestone | Status |
|----|-------|-----------|--------|
| A | DV RNG Consumption | Native RNG | Deferred |
| B | NPC LCG Low-Bit Bias | Native RNG | Deferred |
| C | Surf/Whirlpool | Field Moves | Unfinished |
| D | Determinism Hash | Enhancement | Documented |

None of these issues are blocking for the current development phase. Items A and B will be 
addressed during the PCG RNG migration. Item C is a feature that remains incomplete.
Item D is a known limitation, not a bug.

---

## Completed in Pre-RNG Correctness Pass

The following items were identified and **fixed** in the pre-RNG correctness cleanup:

### Fixed: Crystal Collision Classification
- **Issue**: Range-based collision classifier misclassified sparse Crystal IDs
- **Examples**: 0x18=TALL_GRASS (not water), 0x29=WATER, 0x33=WATERFALL, etc.
- **Fix**: Explicit switch-case mapping from Crystal constants to CollisionClass
- **File**: `frontends/crystal/include/crystal/world/collision_classifier.hpp`

### Fixed: BG Condition Flag Propagation
- **Issue**: IFSET/IFNOTSET/hidden-item flags not evaluated during interaction
- **Fix**: Added `condition_flag` to `InteractableBgEvent`, flag evaluation in `try_bg_event()`
- **Files**: `engine/include/engine/world/interaction.hpp`, `engine/world/interaction.cpp`

### Fixed: ScriptYielded Input Locking
- **Issue**: `is_input_locked()` only blocked for `ScriptRunning`, not `ScriptYielded`
- **Fix**: Added `ScriptYielded` to locked states - yielded scripts own the interaction
- **File**: `engine/core/game_loop.cpp`

### Fixed: Tileset 1..36 Extraction
- **Issue**: Loop extracted tilesets 0..35 instead of Crystal's 1-indexed 1..36
- **Fix**: Changed loops to `i = 1; i <= num_tilesets`
- **File**: `frontends/crystal/extract/tilesets.cpp`

### Fixed: Duplicate Physical Binding Aggregation
- **Issue**: Multiple keys mapping to same button caused phantom releases
- **Fix**: Added `held_count_[]` to track physical sources per logical button
- **Files**: `engine/input/input_system.cpp`, `engine/include/engine/input/input_system.hpp`
