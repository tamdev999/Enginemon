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

---

## G. Player-State Ownership Divergence

**Status**: Deferred - Architectural Clarification Needed

**Current Behavior**:
Two authoritative player state structures exist:
- `GameState::player` (PlayerSaveState): x, y, facing, current_map_id - used for persistence and WorldManager
- `HeadlessGameLoop::player_` (PlayerState): x, y, facing, is_moving, target_x, target_y - used for simulation

During normal gameplay:
- HeadlessGameLoop updates player position/facing during movement
- GameState::player is NOT synced during movement
- WorldManager::execute_warp() reads GameState::player for backup warp coordinates
- Result: backup warp (LAST_WARP) data may use stale position/facing

**Example Scenario**:
1. Player walks north 2 tiles: HeadlessGameLoop=(4,4), GameState=(4,6) stale
2. Player steps on warp at (4,4)
3. execute_warp() reads GameState::player=(4,6) for backup
4. Interior exit via LAST_WARP returns to (4,6) instead of (4,4)

**Reason Deferred**:
Not RNG-dependent. Requires architectural decision:
- Option A: Single authoritative player state (GameState only, HeadlessGameLoop derives)
- Option B: Explicit sync boundary (sync HeadlessGameLoop→GameState before WorldManager ops)

**Intended Milestone**: World/Save System Cleanup

**Relevant Files**:
- `engine/include/engine/core/game_state.hpp` - PlayerSaveState
- `engine/include/engine/core/game_loop.hpp` - PlayerState
- `engine/world/world_manager.cpp` - execute_warp(), remember_backup_warp()
- `runtime/main_tiles.cpp` - transition_to_map()

**Invariant**: Future fix must ensure backup warp coordinates match actual warp-trigger position.

---

## H. Connection Activation Range vs Landing Adjustment

**Status**: Deferred - Semantic Conflation

**Current Behavior**:
`RuntimeConnection::strip_offset` is used for both:
1. Source edge activation range: `strip_start_cells = strip_offset * 2`
2. Landing position adjustment: `out_x/y = player_coord - offset_tiles`

Crystal's serialized `_x`/`_y` values represent the landing adjustment (offset×-2),
not the source strip activation start. The source strip is always `[0, strip_length)`.

**Bug Effect**:
Connections with non-zero offset have incorrect activation bounds check.
For offset=-10: strip_start=-20, but the source strip actually starts at 0.

**Correct Interpretation**:
- Source strip activation: `[0, strip_length_cells)` along the source edge
- Landing adjustment: `player_coord - (offset * 2)` on destination map

**Reason Deferred**:
Most connections use offset=0 where the bug is invisible.
Not RNG-dependent. Fix requires separating source activation range from landing adjustment.

**Intended Milestone**: Map Connection System Fix

**Relevant Files**:
- `frontends/crystal/extract/maps.cpp` - extract_connections()
- `engine/world/world_manager.cpp` - resolve_connection(), calculate_connection_landing()
- `references/pokecrystal/data/maps/attributes.asm` - connection macro definition

**Invariant**: Future fix must verify activation range separately from landing calculation.

---

## I. Input Edge Consumption in Catch-Up Ticks

**Status**: Deferred - API Footgun

**Current Behavior**:
Production code uses `was_pressed()` which queries the snapshot but does NOT consume the edge:
```cpp
if (input.snapshot().was_pressed(InputButton::A)) {
    action = InputAction::Interact;
}
```

The correct API `consume_pressed()` exists and is documented for single-consumption semantics:
```cpp
bool consume_pressed(InputButton btn);  // Returns true once per physical press
```

**Bug Effect**:
During catch-up scenarios (multiple simulation ticks per render frame), the same pressed
edge can be observed by multiple ticks. Mitigated by:
- Dialog: `frame_counter > dialog_open_frame` check
- Interaction: Script lock blocks subsequent interactions

**Reason Deferred**:
Bug is latent - only manifests during severe frame drops causing 3+ catch-up ticks.
Existing guards prevent most observable issues. Not RNG-dependent.

**Intended Milestone**: Input System Hardening

**Relevant Files**:
- `runtime/main_tiles.cpp` - input handling in fixed-step loop
- `engine/include/engine/input/input_system.hpp` - consume_pressed() API
- `engine/input/input_system.cpp` - edge consumption implementation

**Invariant**: Future fix should use consume_pressed() for all edge-triggered actions.

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
| G | Player-State Ownership | World/Save Cleanup | Deferred |
| H | Connection Activation Range | Map Connections | Deferred |
| I | Input Edge Consumption | Input Hardening | Deferred |

---

## J. String Formatting Command Operand Order - FIXED ✓

**Status**: FIXED - August 2026

**Prior Bug**:
The typed decoder read operands in wrong order for several string formatting commands.

**Fix Applied**:
Updated `typed_decoder.cpp` and `crystal_command.cpp` to match pokecrystal ROM layout:
- getmoney (0x3D): ROM=[account, strbuf] - decoder now reads account first
- getmonname (0x40): ROM=[pokemon, strbuf] - decoder now reads pokemon first
- getitemname (0x41): ROM=[item, strbuf] - decoder now reads item first
- gettrainername (0x43): ROM=[group, id, strbuf] - decoder now reads group, id first
- getstring (0x44): ROM=[pointer(lo,hi), strbuf] - decoder now reads pointer first

**Authoritative Source**: `pokecrystal/macros/scripts/events.asm`

**Files Modified**:
- `frontends/crystal/script/typed_decoder.cpp` - Fixed decode order
- `frontends/crystal/script/crystal_command.cpp` - Fixed encode order (already matched)

---

## K. Round-Trip Validation - FIXED ✓

**Status**: FIXED - August 2026

**Prior Gap**:
Production compiler manually set validation inputs without invoking the validator:
```cpp
input.decode_complete = true;
input.round_trip_failures = 0;  // Set to 0 without validation
```

**Fix Applied**:
Updated `full_compiler.cpp` `process_script_typed()` to actually call 
`typed_decoder_->validate_script_round_trip(ir)` and use the real results.

**Files Modified**:
- `frontends/crystal/compile/full_compiler.cpp` - Added validation call
- `tools/corpus_lowering_audit.cpp` - Same fix for audit tool

---

## L. EventFlag vs EngineFlag Namespace - FIXED ✓

**Status**: FIXED - August 2026

**Prior Gap**:
Both checkevent (event_flag) and checkflag (engine_flag) lowered to same type:
```cpp
Sem_CheckFlag op;
op.flag = FlagId{p->event_flag};  // No namespace
```

**Fix Applied**:
Created `FlagRef` struct with `FlagNamespace` enum to preserve the distinction:
```cpp
enum class FlagNamespace : uint8_t {
    Event = 0,    // wEventFlags (2048 bits)
    Engine = 1,   // wEngineFlags (190 bits)
};

struct FlagRef {
    FlagNamespace ns;
    uint16_t value;
};
```

Semantic operations (`Sem_SetFlag`, `Sem_ClearFlag`, `Sem_CheckFlag`) now use `FlagRef`.
Linker validates ranges: EventFlags 0-2047, EngineFlags 0-189.

**Files Modified**:
- `engine/include/engine/core/types.hpp` - Added `FlagRef`, `FlagNamespace`
- `engine/include/engine/scripting/semantic_ir.hpp` - Updated Sem_*Flag ops
- `frontends/crystal/script/semantic_legalizer.cpp` - Create FlagRef with namespace
- `frontends/crystal/script/legality_gate.cpp` - Validate flag ranges per namespace
- `frontends/crystal/script/semantic_linker.cpp` - Encode/validate namespaced flags

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
| G | Player-State Ownership | World/Save Cleanup | Deferred |
| H | Connection Activation Range | Map Connections | Deferred |
| I | Input Edge Consumption | Input Hardening | Deferred |
| J | String Formatting Operand Order | Frontend Hardening | **FIXED** ✓ |
| K | Round-Trip Validation | Frontend Hardening | **FIXED** ✓ |
| L | Flag Namespace Distinction | Multi-Frontend | **FIXED** ✓ |

---

## J. state_hash() Insufficient for Long-Playthrough E2E Checkpoints

**Status**: Tracked — prerequisite before E2E replay/TAS regression tests

**Current Behavior**:
`HeadlessGameLoop::state_hash()` produces a narrow 64-bit XOR of player
position, facing, is_moving, and loop state:

```cpp
hash ^= static_cast<uint64_t>(player_.x) << 0;
hash ^= static_cast<uint64_t>(player_.y) << 8;
hash ^= static_cast<uint64_t>(player_.facing) << 16;
hash ^= static_cast<uint64_t>(player_.is_moving) << 20;
hash ^= static_cast<uint64_t>(state_) << 24;
```

This does NOT cover flags, variables, RNG state, inventory, or party.
Two different game states at the same map position produce identical hashes.

**Required Before E2E Tests**:
Any deterministic long-playthrough regression test that asserts "state at
checkpoint N equals expected" must compare full `GameState` serialization,
not `state_hash()`. Either:
- Use `GameState::serialize()` comparison directly, or
- Extend `state_hash()` to hash the full serialized `GameState` bytes

The current `state_hash()` is useful only for quick position sanity checks.
It is not authoritative for E2E regression.

**Intended Milestone**: Before first long-playthrough / TAS-replay E2E test
**Relevant Files**:
- `engine/core/game_loop.cpp` — `state_hash()`
- `engine/core/game_state.cpp` — `serialize()` / `deserialize()`

---

## K. HeadlessGameLoop::player_ vs GameState::player Ownership Divergence

**Status**: Tracked — prerequisite before save/load mid-movement E2E tests

**Current Behavior** (documented in item G above):
`HeadlessGameLoop::player_` (the simulation's player state) is NOT synced
back to `GameState::player` during movement. During a move sequence,
`GameState::player` is stale at the pre-movement position.

**Impact on E2E Tests**:
Any E2E test that:
1. Steps the simulation while a movement is in progress
2. Calls `GameState::serialize()` (save checkpoint)
3. Then `GameState::deserialize()` (load checkpoint)

will restore the player to the wrong position — the pre-movement tile, not
the mid-movement interpolated position.

TAS-style input replays that save/load at non-idle frames will produce
incorrect results until this is resolved.

**Required Before E2E Tests**:
Resolve the ownership model (see item G for options) before writing any
E2E test that save/loads during player movement. E2E tests limited to
idle-frame checkpoints (after movement fully completes) are safe now.

**Intended Milestone**: World/Save System Cleanup (item G) — must land
before mid-movement E2E checkpoint tests
**Relevant Files**:
- `engine/include/engine/core/game_loop.hpp` — `PlayerState`
- `engine/include/engine/core/game_state.hpp` — `PlayerSaveState`
- `engine/world/world_manager.cpp` — `execute_warp()`, `remember_backup_warp()`
