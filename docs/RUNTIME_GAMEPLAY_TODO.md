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

---

## E. Ledge Hop Execution

**Status**: Deferred - Semantic Identity Preserved

**Current Behavior**:
- Directional ledge identity is now preserved: LedgeRight, LedgeLeft, LedgeUp, LedgeDown
- Ledges are classified as non-walkable (cannot walk onto directly)
- `collision_can_hop_ledge(class, facing)` determines if hop is allowed
- Full player/NPC hop animation and movement execution is NOT implemented

**Crystal Semantic Behavior**:
- Ledges are one-way passages: player can hop DOWN/across the ledge, but not back up
- Crystal triggers a special hop animation when player attempts to move onto a valid ledge
- NPC ledge traversal follows similar rules

**Reason Deferred**:
Semantic ledge direction is preserved. The actual hop execution (animation, movement, sound)
requires additional animation and movement system work. The foundation is ready.

**Intended Milestone**: Movement Animation System / Field Mechanics

**Relevant Files**:
- `engine/include/engine/world/collision_types.hpp` - LedgeRight/Left/Up/Down enums
- `frontends/crystal/include/crystal/world/collision_classifier.hpp` - Crystal→semantic mapping
- `engine/core/game_loop.cpp` - Would need hop execution logic

**Invariant**: Future hop execution must use semantic ledge direction, not raw Crystal IDs.

---

## F. HP Recalculation Semantics

**Status**: Deferred - Incomplete Feature

**Current Behavior**:
- HP preservation formula for level-up/evolution is not source-validated
- Level-up and stat recalculation mechanics are incomplete
- Current implementation may not match Crystal's HP preservation behavior

**Crystal Semantic Behavior**:
When stats change (level-up, evolution), HP is preserved proportionally:
```
new_current_hp = current_hp + (new_max_hp - old_max_hp)
```
Or depending on interpretation, ratio-based preservation.

**Reason Deferred**:
Requires source-tracing pokecrystal's HP recalculation path:
- `_CalcPlayerStats` and related routines
- Level-up flow vs evolution flow vs battle stat recalculation

**Intended Milestone**: Battle Mechanics / Level-Up System

**Relevant Files**:
- `engine/party/pokemon.cpp` - stat calculation
- References: `pokecrystal/engine/pokemon/stats.asm`, `pokecrystal/engine/battle/*`

**Invariant**: HP preservation must match Crystal's exact formula for level-up and evolution.

---

## Updated Summary

| ID | Issue | Milestone | Status |
|----|-------|-----------|--------|
| A | DV RNG Consumption | Native RNG | Deferred |
| B | NPC LCG Low-Bit Bias | Native RNG | Deferred |
| C | Surf/Whirlpool | Field Moves | Unfinished |
| D | Determinism Hash | Enhancement | Documented |
| E | Ledge Hop Execution | Movement System | Deferred (semantics preserved) |
| F | HP Recalculation | Level-Up System | Deferred |

---

## Completed in Final Pre-RNG Pass

### Fixed: Sprite ID Mapping 59-102
- **Issue**: MapExtractor sprite table truncated at 58, valid IDs 59-102 unmapped
- **Fix**: Created shared `sprite_ids.hpp` with authoritative 1-102 mapping
- **Files**: `frontends/crystal/include/crystal/extract/sprite_ids.hpp` (new), 
  `frontends/crystal/extract/sprites.cpp`, `frontends/crystal/extract/maps.cpp`

### Fixed: Map Connection Direction-Specific Offset
- **Issue**: `extract_connections()` used data[8] for all directions
- **Fix**: N/S connections use data[9] (X offset), E/W use data[8] (Y offset)
- **File**: `frontends/crystal/extract/maps.cpp`

### Fixed: Directional Ledge Semantic Preservation
- **Issue**: All Crystal ledges (0xA0-0xA7) collapsed to single `CollisionClass::Ledge`
- **Fix**: Separate `LedgeRight`, `LedgeLeft`, `LedgeUp`, `LedgeDown` enum values
- **Files**: `engine/include/engine/world/collision_types.hpp`, 
  `frontends/crystal/include/crystal/world/collision_classifier.hpp`
