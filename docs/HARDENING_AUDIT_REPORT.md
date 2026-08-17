# Runtime/Core Correctness Hardening Audit Report

**Date:** August 17, 2026  
**Audit Source:** Third-party code review  
**Status:** COMPLETE

---

## Summary

8 items were reviewed from the third-party audit. Results:

| Category | Count |
|----------|-------|
| CONFIRMED BUG — Fixed | 5 |
| VERIFIED CLEAN | 2 |
| PARTIALLY COMPLETE — Deferred | 1 |

All fixes verified with full regression:
- runtime_test: 232/232 PASS
- golden_test: 56/56 PASS  
- linker_test: 1679/1679 PASS
- legality_gate_test: 14/14 PASS
- corpus_lowering_audit: 1679/1679 PASS

---

## Item-by-Item Analysis

### Item 1: RNG in create_pokemon() — CONFIRMED BUG, FIXED

**Audit claim:** `create_pokemon()` creates private `std::mt19937` substreams, violating deterministic simulation requirement.

**Verification:** TRUE — `engine/party/pokemon.cpp` previously seeded a local RNG from a passed seed value.

**Fix:** Changed `create_pokemon()` and `create_wild_pokemon()` signatures to take `RngState&` reference directly. All authoritative randomness is now drawn from the canonical GameState RNG via `rng.next()` calls. No private RNG substreams.

**Behavioral change:**
- Same `GameState.rng` → same Pokemon DVs across save/load/replay
- Multiplayer synchronized simulation determinism preserved

**Files changed:**
- `engine/party/pokemon.cpp` — Takes `RngState&`, calls `rng.next()` directly
- `engine/include/engine/party/pokemon.hpp` — Signature change

---

### Item 2: Movement ID Mismatch — VERIFIED CLEAN

**Audit claim:** `MovementManager::update()` return value semantics are confused with actor IDs.

**Verification:** FALSE — Traced through code:
1. `MovementManager::update()` returns `coroutine_id` from completed movements
2. Callback receives `(actor_id, coroutine_id)` separately
3. `enqueue_movement(actor_id, ...)` returns `coroutine_id`
4. Completion notifications correctly use `coroutine_id` for script resume

**Decision:** No change needed. The naming was reviewed and found to be semantically correct. The callback receives both actor_id and coroutine_id as separate parameters, enabling proper correlation.

---

### Item 3: Binary Parser Bounds — CONFIRMED BUG, FIXED

**Audit claim:** `PackageReader` trusts count fields from untrusted package data without bounds validation.

**Verification:** TRUE — Various parsing methods used counts directly without sanity checks.

**Fix:** Added comprehensive bounds validation:
- `BoundsReader` helper class for checked reads
- File size validation before TOC parsing
- TOC entry bounds validation (offset + size <= file_size)
- Chunk count limits via `PackageLimits` namespace
- Index entry bounds validation within chunks
- String length validation

**Defense-in-depth limits:**
```cpp
namespace PackageLimits {
    MAX_STRING_LENGTH = 64KB
    MAX_ARRAY_COUNT = 1M elements
    MAX_BLOCK_COUNT = 4M blocks
    MAX_CHUNK_SIZE = 256MB per chunk
    MAX_TOC_ENTRIES = 64K entries
}
```

**Files changed:**
- `engine/package/package_reader.cpp` — Full bounds validation
- `engine/include/engine/package/package_reader.hpp` — Added `file_size_` member

---

### Item 4: Save Deserialization — CONFIRMED BUG, FIXED

**Audit claim:** `GameState::deserialize()` returns default state on ANY error, making corrupt saves indistinguishable from new games.

**Verification:** TRUE — Legacy `deserialize()` silently returned empty state on truncation, bad magic, version mismatch, or parse errors.

**Fix:** Added explicit error handling:
- `DeserializeError` enum: `Success`, `TruncatedData`, `InvalidMagic`, `UnsupportedVersion`, `CorruptedPayload`
- `DeserializeResult` struct with explicit error code and state
- `GameState::try_deserialize()` method with full error discrimination
- Legacy `deserialize()` throws in debug builds, returns empty state in release (marked `[[deprecated]]`)

**Files changed:**
- `engine/include/engine/core/game_state.hpp` — DeserializeResult types, deprecated legacy API
- `engine/core/game_state.cpp` — try_deserialize() implementation, debug throw in legacy wrapper

---

### Item 5: Crystal Collision Boundary — PARTIALLY COMPLETE, DEFERRED

**Audit claim:** `johto_collision.hpp` in engine interprets Crystal-specific collision byte values, violating compiler/runtime boundary.

**Verification:** TRUE — The collision interpretation is correct behavior but wrong module location.

**Partial fix implemented:**
- Created semantic `CollisionClass` enum in `engine/include/engine/world/collision_types.hpp`
- Created Crystal frontend classifier in `frontends/crystal/include/crystal/world/collision_classifier.hpp`
- Deprecated raw byte functions in `engine/include/engine/world/johto_collision.hpp`

**NOT YET DONE:**
- Package format change to store `CollisionClass` instead of raw bytes
- Runtime migration to use semantic types throughout

**Decision:** Partial implementation provides the correct abstractions. Full migration deferred as it requires package format versioning and runtime changes. Current implementation is functionally correct.

**Files created:**
- `engine/include/engine/world/collision_types.hpp` — Semantic collision enum
- `frontends/crystal/include/crystal/world/collision_classifier.hpp` — Crystal→semantic translator

**Files modified:**
- `engine/include/engine/world/johto_collision.hpp` — Added deprecation warnings

---

### Item 6: Lua Coroutine Lifecycle — CONFIRMED BUG, FIXED

**Audit claim:** `cleanup_coroutine()` doesn't properly release Lua registry refs and track final states.

**Verification:** TRUE — Initial implementation had issues:
1. Lua registry ref not always released
2. `get_state()` returned Error for completed coroutines
3. Map entries accumulated without bound

**Fix:**
- `cleanup_coroutine()` now releases Lua ref via `luaL_unref()`, records final state in `completed_states_` map, AND erases from `coroutines_` map
- `get_state()` checks both active `coroutines_` and `completed_states_`
- `cancel_all()` properly releases refs and records final states
- Fixed duplicate return statement bug in `get_state()`

**Lifecycle guarantees:**
- Normal completion: ref released, state=Finished preserved, entry erased
- Error: ref released, state=Error preserved, entry erased  
- Cancel: ref released, state=Finished preserved, entry erased
- Shutdown: all refs released via cancel_all()

**Files changed:**
- `engine/scripting/lua_runtime.cpp` — Proper cleanup with completed_states_
- `engine/include/engine/scripting/lua_runtime.hpp` — Added `completed_states_` map

---

### Item 7: State Ownership — VERIFIED CLEAN

**Audit claim:** `GameState` and `HeadlessGameLoop` have overlapping position/facing/movement state.

**Verification:** FALSE — Intentional design:
- `GameState` owns **persistent** gameplay state (survives save/load)
- `HeadlessGameLoop` owns **transient** simulation state (NPC movement progress, idle timers)
- `snapshot_npc_states()` / `restore_npc_states()` synchronize them on save/load

**Decision:** No change needed. The separation is intentional:
- GameState.player = authoritative position (serialized)
- NpcState.move_progress = transient presentation state (not serialized between sessions)
- HeadlessGameLoop ticks advance transient state, periodically syncs to persistent

---

### Item 8: Evidence Checks — CONFIRMED, TESTS ADDED

**Audit claim:** Need tests for scheduler retained-debt interpolation and serialization determinism.

**Fix:** Added two new tests to `tests/scripting/runtime_test.cpp`:

1. **`gamestate_serialize_insertion_order_determinism`**
   - Creates same GameState with flags/variables inserted in different orders
   - Verifies byte-identical serialization output
   - Proves canonical (sorted) ordering is used, not hash table iteration order

2. **`scheduler_interpolation_alpha_clamped`**
   - Creates massive debt (1 second with max 5 ticks)
   - Verifies naive alpha would exceed 1.0 
   - Documents that clamping to 1.0 prevents visual artifacts

**Files changed:**
- `tests/scripting/runtime_test.cpp` — Added 2 adversarial tests

---

## Files Modified (Summary)

| File | Changes |
|------|---------|
| `engine/party/pokemon.cpp` | RNG takes RngState& reference |
| `engine/include/engine/party/pokemon.hpp` | RNG signature change |
| `engine/package/package_reader.cpp` | Comprehensive bounds validation |
| `engine/include/engine/package/package_reader.hpp` | Added file_size_ member |
| `engine/include/engine/core/game_state.hpp` | DeserializeResult types |
| `engine/core/game_state.cpp` | try_deserialize() + canonical ordering + debug throw |
| `engine/include/engine/world/collision_types.hpp` | NEW: Semantic CollisionClass enum |
| `frontends/crystal/include/crystal/world/collision_classifier.hpp` | NEW: Crystal→semantic translator |
| `engine/include/engine/world/johto_collision.hpp` | Deprecated raw byte functions |
| `engine/scripting/lua_runtime.cpp` | Proper coroutine lifecycle with completed_states_ |
| `engine/include/engine/scripting/lua_runtime.hpp` | Added completed_states_ map |
| `tests/scripting/runtime_test.cpp` | Added Item 8 evidence tests |

---

## Regression Results

All tests pass after hardening:

```
runtime_test:          232/232 PASS (2 new Item 8 tests)
golden_test:            56/56 PASS
linker_test:           1679/1679 PASS
legality_gate_test:     14/14 PASS
corpus_lowering_audit: 1679/1679 PASS
```

---

## Remaining Architectural Debt

The following item is documented as known debt, not a correctness bug:

1. **Crystal collision full migration** — Semantic types created, full package/runtime migration deferred

This may be addressed in future work but does not affect gameplay correctness or determinism.
