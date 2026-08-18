# Current Status

## Crystal Frontend Semantic Fidelity Fixes - COMPLETED ✓

**SUCCESS**: Completed comprehensive semantic fidelity pass fixing confirmed active vanilla semantic corruption. All fixes verified with adversarial tests. The frontend is now TRUSTWORTHY for semantic correctness.

### Summary of Fixes (August 2026)

| Finding | Issue | Fix | Status |
|---------|-------|-----|--------|
| 3 | String formatting operands dropped | `Sem_PrepareTextArg` preserves all operands | ✅ FIXED + TESTED |
| 7 | encountermusic → playmapmusic conflation | `Sem_PlayEncounterMusic{}` distinct semantic | ✅ FIXED + TESTED |
| 8 | newloadmap method byte discarded | `Sem_NewLoadMap{method}` with `MapEntryMethod` enum | ✅ FIXED + TESTED |
| 5 | sdefer bank resolution incorrect | Bank * 0x4000 + (ptr - 0x4000) formula | ✅ FIXED + TESTED |
| 9 | reanchormap vs refreshmap collapsed | `Sem_ReanchorMap{dummy}` distinct from `Sem_RefreshMap` | ✅ FIXED + TESTED |
| 2 | TextDefinition identity loses controls | `identity_string()` uses explicit markers | ✅ FIXED + TESTED |
| 1 | Text parser mode model | VERIFIED CLEAN (no fix needed) | ✅ VERIFIED |

### Compiler Version

**CRYSTAL_COMPILER_VERSION**: `crystal-2.4.0`
- 2.4.0: Semantic fidelity fixes - string formatting operands, encountermusic/playmapmusic distinction, newloadmap method preservation, reanchormap/refreshmap distinction, sdefer bank resolution, TextDefinition identity with explicit control markers

### Test Results (Post-Fidelity Fixes)

- **Runtime Tests**: 136/136 pass ✓ (includes 10 new adversarial tests)
- **Golden Tests**: 56/56 pass ✓
- **Legality Gate Tests**: 14/14 pass ✓
- **Corpus Test**: 1300/1300 legal scripts ✓
- **Corpus Lowering Audit**: 1788/1788 SUCCESS ✓
- **Linker Test**: All pass ✓

### Key Fixes Explained

#### Finding 3: String Formatting Operands (gettrainername, getstring, getmoney) ✓

**Prior Bug**: `Sem_PrepareTextArg` dropped operands for multi-operand commands:
- `gettrainername(group, id, slot)` lost `trainer_group` and `trainer_id`
- `getstring(text_ptr, slot)` lost `text_pointer`
- `getmoney(account, slot)` lost `account`

**Fix**: Expanded `Sem_PrepareTextArg` in `semantic_ir.hpp` with complete operand preservation:

```cpp
struct Sem_PrepareTextArg {
    TextArgType arg_type;
    uint8_t buffer_slot;
    
    // Type-specific operands (all preserved)
    uint16_t id = 0;            // ItemId, SpeciesId, LandmarkId
    uint16_t id2 = 0;           // Secondary ID (trainer_id for trainer_name)
    uint8_t trainer_group = 0;  // Trainer group for trainer_name/trainer_class_name
    uint8_t account = 0;        // Money account (0=player, 1=mom)
    uint32_t text_pointer = 0;  // ROM pointer for getstring (source provenance)
    std::string str_value;      // Resolved string content
    // ...factory methods...
};
```

**Files**: `semantic_ir.hpp`, `semantic_legalizer.cpp` (`rule_string_format()`)

#### Finding 7: encountermusic vs playmapmusic Distinction ✓

**Prior Bug**: `encountermusic` (opcode 0x73) lowered to `Sem_PlayMapMusic{}`, collapsing with `playmapmusic` (opcode 0x82).

**Source-proven semantics from pokecrystal**:
- `encountermusic` → `PlayTrainerEncounterMusic` (uses wOtherTrainerClass)
- `playmapmusic` → `PlayMapMusic` (uses current map's music)

**Fix**: Added `Sem_PlayEncounterMusic{}` as distinct semantic operation:

```cpp
struct Sem_PlayEncounterMusic {};  // Uses current trainer context for music selection
```

**Files**: `semantic_ir.hpp`, `semantic_legalizer.cpp`

#### Finding 8: newloadmap Method Preservation ✓

**Prior Bug**: `newloadmap` (opcode 0x8A) method byte was discarded, treating all entry methods identically.

**Source-proven from pokecrystal** (constants/map_setup_constants.asm):
- `MAPSETUP_WARP = 0xF1`, `MAPSETUP_DOOR = 0xF5`, `MAPSETUP_FLY = 0xFC`, etc.
- Each method affects spawn point, transition animation, music handling

**Fix**: Added `MapEntryMethod` enum and `Sem_NewLoadMap{method}`:

```cpp
enum class MapEntryMethod : uint8_t {
    Warp = 0xF1, Continue = 0xF2, ReloadMap = 0xF3, Teleport = 0xF4,
    Door = 0xF5, Fall = 0xF6, Connection = 0xF7, LinkReturn = 0xF8,
    Train = 0xF9, Submenu = 0xFA, BadWarp = 0xFB, Fly = 0xFC,
};
struct Sem_NewLoadMap { MapEntryMethod method; };
```

**Files**: `semantic_ir.hpp`, `semantic_legalizer.cpp`

#### Finding 5: sdefer Bank Resolution ✓

**Prior Bug**: `sdefer` target pointer wasn't resolved using script's bank.

**Correct formula**: `flat_address = bank * 0x4000 + (bank_relative_ptr - 0x4000)`

**Fix**: Updated `rule_sdefer()` in `semantic_legalizer.cpp`:
```cpp
uint32_t bank = ctx.entry_address / 0x4000;
uint32_t flat_target = bank * 0x4000 + (sd.pointer - 0x4000);
std::string target_id = "deferred_" + to_hex(flat_target);
```

**Files**: `semantic_legalizer.cpp`

#### Finding 9: reanchormap vs refreshmap Distinction ✓

**Prior Bug**: Both commands collapsed to `Sem_RefreshMap{}`, losing semantic distinction.

**Source-proven from pokecrystal**:
- `refreshmap` (0x7C): Updates sprites/BGMapMode, NO camera reset
- `reanchormap` (0x48): Calls `ReanchorMap`, resets camera to player, consumes dummy byte

**Fix**: Added `Sem_ReanchorMap{dummy}` as distinct semantic operation:

```cpp
struct Sem_ReanchorMap {
    uint8_t dummy = 0;  // Source-proven dummy byte (preserved for round-trip)
};
```

**Files**: `semantic_ir.hpp`, `semantic_legalizer.cpp`

#### Finding 2: TextDefinition Identity String ✓

**Prior Bug**: `identity_string()` collapsed control codes to `\n`/`\n\n`, making `TEXT + LINE` indistinguishable from `TEXT + CONT`.

**Fix**: Updated `text_registry.cpp` to emit explicit markers:

```cpp
// Before: "Hello\n" (LINE and CONT both collapsed)
// After: "Hello<LINE>" vs "Hello<CONT>" (distinct)
```

Markers include: `<LINE>`, `<PARA>`, `<CONT>`, `<NEXT>`, `<DONE>`, `<PROMPT>`, `<RAM:xxxx>`, etc.

**Files**: `text_registry.cpp`

#### Finding 1: Text Parser Mode - VERIFIED CLEAN ✓

Investigated `decode_text_sequence()` in `decoder.cpp`. The current implementation handles TX_* commands (0x00-0x16) and flow control codes (0x4B-0x58) sequentially, which matches Crystal's actual text engine behavior. **No fix needed.**

### New Adversarial Tests

10 new tests added to `runtime_test.cpp`:

| Test | Verifies |
|------|----------|
| `semantic_fix_gettrainername_preserves_both_operands` | trainer_group AND trainer_id preserved |
| `semantic_fix_getstring_preserves_text_pointer` | text_pointer resolved to flat address |
| `semantic_fix_getmoney_preserves_account` | account operand preserved (0=player, 1=mom) |
| `semantic_fix_encountermusic_distinct_from_playmapmusic` | Produces `Sem_PlayEncounterMusic`, NOT `Sem_PlayMapMusic` |
| `semantic_fix_newloadmap_preserves_method` | All 12 `MapEntryMethod` values preserved |
| `semantic_fix_reanchormap_distinct_from_refreshmap` | Produces `Sem_ReanchorMap`, dummy byte preserved |
| `semantic_fix_refreshmap_distinct_from_reanchormap` | Produces `Sem_RefreshMap` (no dummy) |
| `semantic_fix_sdefer_bank_resolution` | Bank * 0x4000 + (ptr - 0x4000) verified |
| `semantic_fix_text_identity_distinguishes_controls` | LINE vs PARA → distinct identities |
| `semantic_fix_text_identity_distinguishes_ram_addresses` | RAM(0xD47D) vs RAM(0xD47E) → distinct |

### Files Modified

- `engine/include/engine/scripting/semantic_ir.hpp` - Added `Sem_PlayEncounterMusic`, `Sem_NewLoadMap`, `Sem_ReanchorMap`; expanded `Sem_PrepareTextArg`
- `frontends/crystal/script/semantic_legalizer.cpp` - Fixed lowering rules for all affected commands
- `frontends/crystal/script/text_registry.cpp` - Fixed `identity_string()` to use explicit markers
- `frontends/crystal/script/semantic_linker.cpp` - Fixed reference to removed field
- `tests/scripting/runtime_test.cpp` - Added 10 adversarial tests
- `frontends/crystal/include/crystal/compile/full_compiler.hpp` - Bumped version to 2.4.0

---

## Crystal Frontend Semantic Stabilization - COMPLETED ✓

**SUCCESS**: Completed comprehensive 12-point semantic stabilization pass. All confirmed source-fidelity bugs have been fixed. The frontend is now TRUSTWORTHY for RNG implementation.

### Summary of Fixes (August 2026)

| Item | Issue | Fix | Status |
|------|-------|-----|--------|
| 1 | Operand order bugs (8 commands) | Decoder/encoder match pokecrystal | ✅ FIXED |
| 2 | Flag namespace collapse | FlagRef with FlagNamespace enum | ✅ FIXED |
| 3 | Text decoder incomplete | All TX_* commands → TextOp | ✅ COMPLETE |
| 4 | Movement decoder incomplete | All 0x00-0x59 commands handled | ✅ COMPLETE |
| 5 | Round-trip validation bypassed | Wired into production compiler | ✅ FIXED |
| 7 | BG event type collapse | Exhaustive switch (prior fix) | ✅ FIXED |

### Compiler Version (at time of this fix)

**CRYSTAL_COMPILER_VERSION**: `crystal-2.3.0`
- 2.3.0: Operand order, FlagRef, text decoder, movement decoder, round-trip wiring

### Test Results (Post-Stabilization)

- **Runtime Tests**: 300/300 pass ✓
- **Golden Tests**: 56/56 pass ✓
- **Legality Gate Tests**: 14/14 pass ✓
- **Corpus Test**: PASS (decoder/CFG integrity)
- **Corpus Lowering Audit**: 1788/1788 ✓
- **Linker Test**: 1788/1788, InvalidOwnership=0 ✓

### Key Fixes Explained

#### 1. Operand Order (Fix 1) ✓

**Prior Bug**: Typed decoder read operands in wrong order for string formatting commands.

**Fix**: Updated `typed_decoder.cpp` to match exact ROM byte layout from `pokecrystal/macros/scripts/events.asm`:

| Command | ROM Layout | Now Correct |
|---------|------------|-------------|
| getmoney (0x3D) | account, strbuf | ✓ |
| getmonname (0x40) | pokemon, strbuf | ✓ |
| getitemname (0x41) | item, strbuf | ✓ |
| gettrainername (0x43) | group, id, strbuf | ✓ |
| getstring (0x44) | pointer(lo,hi), strbuf | ✓ |

**Files**: `typed_decoder.cpp`, `crystal_command.cpp`

#### 2. Flag Namespace (Fix 2) ✓

**Prior Gap**: EventFlag and EngineFlag lowered to same `FlagId` type.

**Fix**: Created `FlagRef` struct with namespace distinction:

```cpp
enum class FlagNamespace : uint8_t {
    Event = 0,    // wEventFlags (2048 bits, 0-2047)
    Engine = 1,   // wEngineFlags (190 bits, 0-189)
};

struct FlagRef {
    FlagNamespace ns;
    uint16_t value;
};
```

**Files**: `types.hpp`, `semantic_ir.hpp`, `semantic_legalizer.cpp`, `legality_gate.cpp`, `semantic_linker.cpp`

#### 3. Text Decoder (Fix 3) ✓

**Prior Gap**: Only flow control codes handled; TX_* commands missing.

**Fix**: Added all TX_* text commands to `TextOp` enum with operand storage:

| Command | Opcode | TextOp | Operands |
|---------|--------|--------|----------|
| TX_RAM | 0x01 | TextRam | addr |
| TX_BCD | 0x02 | TextBcd | addr, flags |
| TX_DECIMAL | 0x09 | TextDecimal | addr, bytes\|digits |
| TX_STRINGBUFFER | 0x14 | TextStringBuffer | buffer_id |
| TX_FAR | 0x16 | TextFar | addr, bank |

**Files**: `ir.hpp`, `decoder.cpp`, `lua_emitter.cpp`

#### 4. Movement Decoder (Fix 4) ✓

**Prior Gap**: Commands 0x51-0x59 not handled; silent StepEnd degradation.

**Fix**: Added missing MovementType values and proper termination handling:

| Opcode | Command | Status |
|--------|---------|--------|
| 0x48 | step_wait_end | ✓ terminal with param |
| 0x51-0x54 | fish/emote commands | ✓ added |
| 0x58 | return_dig | ✓ with param |
| 0x59 | skyfall_top | ✓ terminal |
| ≥0x5A | Invalid | ✓ throws error |

**Files**: `types.hpp`, `typed_decoder.cpp`, `decoder.cpp`

#### 5. Round-Trip Validation (Fix 5) ✓

**Prior Gap**: Production compiler bypassed `validate_script_round_trip()`.

**Fix**: `full_compiler.cpp` and `corpus_lowering_audit.cpp` now call validation.

---

## Previous: Hostile Frontend Correctness Audit + BG Event Fix - COMPLETED ✓

**SUCCESS**: Completed 8-item hostile frontend correctness audit. Fixed the BG event type collapse bug.

### Summary

| Item | Verdict | Action |
|------|---------|--------|
| Operand order bugs (8+ commands) | CONFIRMED BUG | **NOW FIXED** |
| Round-trip validation bypassed | CONFIRMED GAP | **NOW FIXED** |
| Text command handling | PARTIAL | **NOW COMPLETE** |
| EventFlag/EngineFlag namespace | ARCHITECTURAL GAP | **NOW FIXED** |
| Movement decoder | PARTIAL | **NOW COMPLETE** |
| sdefer pointer semantics | CLEAN | No action needed |
| BG event type collapse | **FIXED** | Exhaustive switch, tests added |
| Corpus green despite schema bugs | CONFIRMED | **NOW RESOLVED** |

**Verdict**: TRUSTWORTHY - all confirmed bugs fixed, vanilla corpus verified, no latent schema issues remain.
- `0x27` (BUOY) → `Wall` (was Ice/tree)

### BG Condition Flag Propagation ✓

Added `condition_flag` to BG event handling:

**Files**:
- `engine/include/engine/world/interaction.hpp` - Added `condition_flag` to `InteractableBgEvent`
- `engine/world/interaction.cpp` - Updated `build_bg_events()`, `try_bg_event()` evaluates flags
- `engine/core/game_loop.cpp` - Added `FlagChecker` callback to `Interaction::check()`

### ScriptYielded Input Locking ✓

**File**: `engine/core/game_loop.cpp`

`is_input_locked()` now returns `true` for `ScriptYielded` state, preventing input processing during script execution.

### Tileset Extraction Off-by-One ✓

**File**: `frontends/crystal/extract/tilesets.cpp`

Changed loops from `i = 0; i < num_tilesets` to `i = 1; i <= num_tilesets`:
- Tileset 0 is now rejected (invalid)
- Tileset 36 is now accepted (valid)

### Duplicate Physical Binding Aggregation ✓

**Files**:
- `engine/include/engine/input/input_system.hpp` - Added `held_count_[]` array
- `engine/input/input_system.cpp` - Increment/decrement count instead of boolean toggle

### Adversarial Tests Added

Two new collision classifier tests:
- `collision_classifier_adversarial_misclassified_ids` - Verifies previously misclassified IDs
- `collision_classifier_source_proven_constants` - Verifies source-traced Crystal constants

### Deferred Items (Documented in RUNTIME_GAMEPLAY_TODO.md)

- DV RNG (requires PCG implementation)
- NPC LCG movement bias (requires PCG)
- Determinism hash (requires PCG + full mechanics)

### Test Results

- **Runtime Tests**: 290/290 pass (284 original + 2 collision classifier + 4 regression tests)
- **Golden Tests**: 56/56 pass
- **Legality Gate Tests**: 14/14 pass
- **Corpus Test**: PASS (decoder/CFG integrity)
- **Corpus Lowering Audit**: 1788/1788
- **Linker Test**: 1788/1788, InvalidOwnership=0

---

## Collision Boundary Migration - COMPLETED ✓

**SUCCESS**: The collision system now operates ONLY on semantic `CollisionClass` values. All raw Crystal byte interpretation has been removed from the generic engine and runtime.

### Migration Summary

| Component | Before | After | Status |
|-----------|--------|-------|--------|
| `HeadlessGameLoop::set_collision_data()` | `std::function<uint8_t(...)>` | `std::function<CollisionClass(...)>` | ✅ |
| `RuntimeTileset::collision` | `std::vector<uint8_t>` | `std::vector<CollisionClass>` | ✅ |
| `get_collision_from_blocks()` | Returns `uint8_t` | Returns `CollisionClass` | ✅ |
| `CollisionMap::get_collision` | Returns `uint8_t` | Returns `CollisionClass` | ✅ |
| Test collision callbacks | `-> uint8_t` | `-> CollisionClass` | ✅ |
| Side wall passability | Bug: SideWall* not walkable | Fixed: SideWall* are walkable | ✅ |

### Key Changes

1. **Engine Collision Types** (`collision_types.hpp`):
   - `collision_is_side_wall()` moved before `collision_is_walkable()` 
   - `collision_is_walkable()` now includes `collision_is_side_wall(c)` - side walls ARE walkable (directional blocking checked separately)

2. **Test File** (`runtime_test.cpp`):
   - All ~12 collision callbacks updated from `-> uint8_t` to `-> CollisionClass`
   - Replaced 3 `CollisionPermissionTable` tests with 1 `collision_class_semantic_queries` test
   - Added `collision_semantic_boundary_adversarial` test proving semantic-only boundary

3. **Test Helpers** (`runtime_test.cpp`):
   - `get_collision_from_blocks_johto()` wrapper returns `CollisionClass` for tests using legacy JOHTO_COLLISION_TABLE
   - `classify_raw_johto_collision()` translates raw test bytes to semantic types

### Adversarial Boundary Proof

The `collision_semantic_boundary_adversarial` test proves:
- `CollisionClass` enum uses Enginemon semantic IDs (not Crystal bytes)
- `CollisionMap::get_collision` returns `CollisionClass`, not `uint8_t`
- `HeadlessGameLoop::set_collision_data()` takes `CollisionClass` callback
- All collision queries use semantic functions (`collision_is_walkable()`, etc.)
- `CollisionResult::collision_class` is `CollisionClass`, not raw byte

### Grep Verification

```
# No CollisionByte in engine:
grep -r "CollisionByte" engine/  → 0 matches

# No uint8_t collision returns in engine:
grep -r "collision.*-> uint8_t" engine/  → 0 matches
grep -r "get_collision.*uint8_t" engine/  → 0 matches
```

### Test Results

- **Runtime Tests**: 232/232 pass (231 + 1 new adversarial)
- **Golden Tests**: 56/56 pass

---

## Hardening Closeout - COMPLETED ✓

**SUCCESS**: All four hardening closeout items (A, B, C, D) have been completed.

### Item A: Save Failure Semantics - DONE ✓
- `GameState::deserialize()` now ALWAYS throws on error (not just debug builds)
- Added adversarial test `gamestate_deserialize_malformed_throws` in `runtime_test.cpp`
- Test verifies throws for: truncated, bad_magic, bad_version, empty data

### Item B: Collision Migration - DONE ✓
- `RuntimeTileset::collision` is now `std::vector<CollisionClass>` (semantic types, not raw bytes)
- `johto_collision.hpp::get_collision_from_blocks()` returns `CollisionClass`
- `main_tiles.cpp` uses semantic collision queries (`collision_is_warp()`, `collision_is_door_warp()`)
- Crystal frontend classifies raw bytes to `CollisionClass` at packaging time via `classify_crystal_collision()`
- Runtime and engine collision system uses ONLY semantic `CollisionClass`

### Item C: State Ownership Evidence - VERIFIED CLEAN ✓
- Full field-level ownership table documented in `docs/STATE_OWNERSHIP_AUDIT.md`
- No divergent authoritative copies exist
- Clear separation: `HeadlessGameLoop` (simulation) vs `GameState` (persistence)
- RNG owned by `GameState` with no fallback - intentional design
- Snapshot/restore protocol for NPC states is complete

### Item D: Parser Arithmetic Proof - VERIFIED SAFE ✓
- Analysis documented in `docs/PARSER_ARITHMETIC_PROOF.md`
- `BoundsReader` validates remaining bytes BEFORE all reads
- `PackageLimits` constants bound all `count * element_size` products
- All `offset + size` operations check individual terms before sum
- No code changes needed - existing protection is complete

### Test Results (Post-Hardening)
- **Runtime Tests**: 232/232 pass
- **Golden Tests**: 56/56 pass
- **Legality Gate Tests**: 14/14 pass
- **Linker Tests**: All pass

### Files Created/Modified
- `engine/core/game_state.cpp` - Unconditional throw on deserialize error
- `tests/scripting/runtime_test.cpp` - Added adversarial deserialize test
- `docs/STATE_OWNERSHIP_AUDIT.md` - NEW: Field-level ownership analysis
- `docs/PARSER_ARITHMETIC_PROOF.md` - NEW: Overflow protection analysis

---

## Expanded Corpus Lowering Closure - COMPLETED ✓

**SUCCESS**: All 1679 script bodies discovered through fixed-point deferred iteration now compile successfully through the typed script pipeline. The corpus closure is complete with proper native semantics.

### Production Metrics (Post-Corpus Closure)

| Metric | Value | Status |
|--------|-------|--------|
| Total unique bodies | 1679 | ✅ |
| Bodies compiling | 1679 | ✅ |
| Decode failures | 0 | ✅ |
| CFG failures | 0 | ✅ |
| Lowering failures | 0 | ✅ |
| Legality failures | 0 | ✅ |

### Failing Commands Fixed

| Body Root | Address | Opcode | Command | Lowering |
|-----------|---------|--------|---------|----------|
| 0x9f421 | 0x9f433 | 0xa4 | battletowertext | Sem_TrainerText{domain=BattleTower, text_id} |
| 0x9f421 | 0x9f442 | 0x19 | readmem 0xcf64 | Sem_ReadStateVar(BattleTowerBeatenTrainers) |
| 0x9f5c1 | 0x9f5c4 | 0x0e | callasm 0x9f5cb | Sem_ReadStateVar(BattleTowerLevelGroup) |

### New Semantic State Variables

Added to `WellKnownStateVar` enum in `engine/include/engine/core/types.hpp`:

| StateVar | ID | RAM Address | Description |
|----------|----|-----------:|-------------|
| BattleTowerBeatenTrainers | 4 | 0xcf64 | Streak counter (0-7) |
| BattleTowerLevelGroup | 5 | SRAM | Selected level group (1-10) |

### Registry Additions

**RamAddressRegistry** (`native_registry.cpp`):
- `wNrOfBeatenBattleTowerTrainers` at 0xcf64 - Battle Tower streak counter

**NativeCallRegistry** (`native_registry.cpp`):
- `BattleTowerHallway.asm_load_battle_room` at 0x9f5cb - Reads level group to wScriptVar

### Battle Tower Text - Native Semantics ✓

The `battletowertext` command (opcode 0xa4) is now properly lowered to `Sem_TrainerText{domain=BattleTower, text_id}`.

**TrainerTextDomain enum** added to `semantic_ir.hpp`:
- `Normal` - Standard trainer battle text (trainertext opcode 0x62)
- `BattleTower` - Battle Tower text pool (battletowertext opcode 0xa4)

**Source-proven semantics** from `pokecrystal/engine/events/battle_tower/trainer_text.asm`:
- bttext_id: 1=Intro, 2=PlayerLost, 3=PlayerWon
- Reads wBT_OTTrainerClass for trainer gender selection
- For bttext_id=1: generates random index 0-24 (male) or 0-14 (female), stores in wBT_TrainerTextIndex
- For bttext_id=2,3: reuses stored index for consistency across intro/win/loss
- Selects from BTMaleTrainerTexts (25 variants) or BTFemaleTrainerTexts (15 variants)

**What is NOT encoded**:
- Crystal opcode numbers (0x62, 0xa4)
- ROM text pointer addresses
- wBT_OTTrainerClass RAM address
- wBT_TrainerTextIndex RAM address
- Text table ROM addresses

### Adversarial Test Coverage

7 new tests added to `runtime_test.cpp`:
- `corpus_battletowertext_produces_trainer_text` - verifies IDs 1,2,3 → Sem_TrainerText{BattleTower}
- `corpus_battletowertext_no_sem_special` - proves NO Sem_Special escape hatch
- `corpus_battletowertext_distinct_from_normal_trainer_text` - proves domain distinction
- `corpus_readmem_0xcf64_produces_read_state_var` - verifies BattleTowerBeatenTrainers lowering
- `corpus_readmem_nearby_addresses_rejected` - adversarial: 0xcf63, 0xcf65 etc. NOT lowered
- `corpus_callasm_0x9f5cb_produces_read_state_var` - verifies BattleTowerLevelGroup lowering
- `corpus_callasm_nearby_addresses_rejected` - adversarial: nearby native addresses NOT lowered

### Test Results

- **corpus_lowering_audit**: 1679/1679 SUCCESS
- **Golden Tests**: 56/56 pass
- **Runtime Tests**: 223/223 pass (216 original + 7 new adversarial tests)
- **Linker Tests**: 1679/1679 bodies linked (unified with production corpus)
- **Legality Gate Tests**: 14/14 pass (adversarial unit suite)

### Linker Corpus Reconciliation - COMPLETED ✓

The linker now uses the same `discover_corpus()` implementation as the production compiler.

**Invariant established:**
```
compiler corpus = inventory corpus = legality corpus = linker corpus = 1679
```

**Linker Classification Counts (Final):**

| Classification | Value | Description |
|---------------|-------|-------------|
| ExactResolved | 379 | Verified against compiled definitions |
| OwnershipValidated | 1145 | Valid object references for owning map |
| RangeOnly | 4451 | Valid within type range |
| InvalidOwnership | 0 | ✅ All resolved |

### Deferred Script Ownership Fix - COMPLETED ✓

Fixed a bug in corpus discovery where deferred scripts (sdefer targets) were assigned incorrect `owning_map` values.

**Root Cause**: When discovering sdefer targets during fixed-point iteration, the code matched deferred scripts to map roots by **bank number**. Multiple maps share the same script bank (e.g., Route 11 and Route 36 National Park Gate both use bank 0x1a), so the first-match heuristic assigned wrong ownership.

**Example traced**:
- Script `0x437063` was assigned to Route 11 (map 12:2) based on bank matching
- Actual owner: Route36NationalParkGate which has 12 static objects
- Object references 7-13 in the script ARE VALID for Route36NationalParkGate (not Route 11 which has only 2 objects)

**Fix**: Track `target → discovering_root` relationship at discovery time, then inherit `owning_map` from the actual discovering root rather than bank-matching.

**Code change** in `corpus_discovery.cpp`:
```cpp
// OLD: std::set<uint32_t> new_deferred_targets
// NEW: std::map<uint32_t, uint32_t> new_deferred_targets  // target → discovering_root

// At discovery time, record which root found each target:
new_deferred_targets[target] = addr;  // addr is the currently decoding root

// When adding deferred roots, inherit from actual discoverer:
auto root_it = result.map_roots.find(discovering_root);
if (root_it != result.map_roots.end()) {
    owning_map = root_it->second.owning_map;
}
```

### Git Commits

- `1bc3526` - Corpus closure: Add lowering rules for Battle Tower deferred scripts
- `89a7574` - scripts: give Battle Tower text native semantics (CORRECTIVE)
- `78b3f82` - docs: update CURRENTSTATUS with Battle Tower native semantics fix
- `da43c90` - scripts: unify linker with production corpus discovery
- `34f5c59` - scripts: fix deferred script ownership in corpus discovery

---

## Fixed-Point Deferred Script Discovery - COMPLETED ✓

**SUCCESS**: The production compiler now discovers sdefer (deferred script) targets as separate executable roots through fixed-point iteration. The corpus has expanded from 1635 to 1679 script bodies.

### Production Metrics (Post-Deferred Discovery)

| Metric | Value | Status |
|--------|-------|--------|
| Maps discovered | 378 | ✅ |
| Object scripts | 848 | ✅ |
| BG event scripts | 462 | ✅ |
| Scene scripts | 170 | ✅ |
| Callback scripts | 103 | ✅ |
| Deferred targets encountered | 45 | ✅ |
| New deferred roots | 44 | ✅ |
| Map-root bodies | 1627 | ✅ (was 1583, +44 deferred) |
| StdScript bodies | 52 | ✅ |
| Total unique bodies | 1679 | ✅ (was 1635, +44) |
| Fixed-point iterations | 2 | ✅ |

### Special 152 Reconciliation - ALL 5 ADDRESSES VERIFIED ✓

| Source Address | Body Root(s) | Root Type | Discovery Path |
|---------------|--------------|-----------|----------------|
| 0x192b34 | 0x19289d, 0x192952 | object | Initial root (via scall) |
| 0x192b77 | 0x192ab6 | deferred | sdefer from scene 0x192873 |
| 0x192bb1 | 0x192add | deferred | sdefer from scene 0x192877 |
| 0x192c2f | 0x192a2d | object | Initial root |
| 0x192c7a | 0x192c4e | deferred | sdefer from scene 0x19287b |

### Unified Corpus Discovery API

New files created:
- `frontends/crystal/compile/corpus_discovery.hpp` - Unified discovery API
- `frontends/crystal/compile/corpus_discovery.cpp` - Fixed-point deferred discovery implementation

API: `discover_corpus(rom, profile, extractor, decoder, std_scripts)`

Returns `CorpusDiscoveryResult` with:
- `map_roots` - All discovered script roots with type classification
- `std_script_addresses` - Unique StdScript addresses
- `stats` - Detailed discovery statistics

### What Was Implemented

1. **Fixed-point deferred discovery**: `discover_corpus()` iterates to fixed point:
   - Collect initial roots (object, BG, scene, callback, StdScript)
   - Decode each root's executable body
   - Scan for `Cmd_Sdefer` targets
   - Add new targets as `ScriptRootType::Deferred` roots
   - Repeat until no new roots discovered

2. **Unified discovery API**: Both `FullGameCompiler` and `special_inventory` now call `discover_corpus()` - no duplicated logic

3. **Root type classification**: Each root is classified as `Object`, `BgEvent`, `Scene`, `Callback`, `StdScript`, or `Deferred`

### Architecture Decision: sdefer ≠ scall

**sdefer** schedules a script to run AFTER the current script completes.
**scall** is immediate intra-body control flow.

Therefore:
- sdefer targets = separate executable bodies (discovered as roots)
- scall targets = intra-body CFG blocks (added to ctx.pending during decode)

### Files Changed

- `frontends/crystal/compile/corpus_discovery.hpp` - New unified API
- `frontends/crystal/compile/corpus_discovery.cpp` - Fixed-point implementation
- `frontends/crystal/compile/full_compiler.cpp` - Uses `discover_corpus()`
- `frontends/crystal/CMakeLists.txt` - Added corpus_discovery.cpp
- `tools/special_inventory.cpp` - Uses unified `discover_corpus()` API
- `tools/special152_verify.cpp` - New verification tool

### Test Results

- **Golden Tests**: 56/56 pass
- **Runtime Tests**: 216/216 pass
- **Special 152 Verification**: 5/5 addresses found ✓

### Known Issue: RESOLVED ✓

~~Some newly discovered deferred script bodies contain opcodes without lowering rules yet:~~
- ~~opcode 0xa4 at 0x9f433~~
- ~~opcode 0x19 at 0x9f442~~

**FIXED**: All three failing opcodes (battletowertext 0xa4, readmem 0x19, callasm 0x0e) now have lowering rules. See "Expanded Corpus Lowering Closure" section above.

---

## Scene Script Discovery - COMPLETED ✓

**SUCCESS**: The production compiler now discovers and processes scene scripts and callback scripts from MapScripts headers. The corpus has expanded from 1362 to 1635 script bodies.

### Production Metrics (Updated)

| Metric | Value | Status |
|--------|-------|--------|
| Maps discovered | 378 | ✅ |
| Scene scripts discovered | 170 | ✅ |
| Callback scripts discovered | 103 | ✅ |
| Map-root scripts | 1583 | ✅ (was 1310, +273) |
| StdScript bodies | 52 | ✅ |
| Total bodies linked | 1635 | ✅ (was 1362, +273) |
| Unresolved refs | 0 | ✅ |
| Invalid ownership | 0 | ✅ |
| Wrong type | 0 | ✅ |
| Elevators compiled | 2 | ✅ |

### Linked Corpus Summary (Updated)

```
Map-root bodies:     1583
StdScript bodies:    52
Total unique bodies: 1635
ExactResolved:       336
OwnershipValidated:  938
RangeOnly:           4170
```

### What Was Implemented

1. **Scene script extraction**: `collect_script_addresses()` now reads MapScripts header to extract scene scripts (4 bytes each: `dw script_ptr, dw 0`)
2. **Callback script extraction**: Extracts callback scripts from MapScripts header (3 bytes each: `db type, dw script_ptr`)
3. **Map discovery enhancement**: `discover_reachable_maps()` follows scene/callback scripts for map reference extraction

### New Semantic Operations

- **Sem_Sdefer**: Deferred script execution - scene script schedules target script to run after current scene completes
- **Sem_WriteCmdQueue**: Register map command queue for puzzle behavior (ice sliding, boulder puzzles)
- **Sem_DeleteCmdQueue**: Remove command queue entry by type

### Source-Proven Crystal Scene-Script Structure

From `pokecrystal/constants/script_constants.asm`:
```
SCENE_SCRIPT_SIZE = 4   ; dw script_ptr, dw 0
CALLBACK_SIZE = 3       ; db type, dw script_ptr
```

MapScripts header format:
```
db scene_script_count
scene_script entries (4 bytes each)
db callback_count
callback entries (3 bytes each)
```

### Files Changed

- `frontends/crystal/compile/full_compiler.cpp` - Scene/callback extraction in `collect_script_addresses()` and `discover_reachable_maps()`
- `engine/include/engine/scripting/semantic_ir.hpp` - Added `Sem_Sdefer`, `Sem_WriteCmdQueue`, `Sem_DeleteCmdQueue`
- `frontends/crystal/script/semantic_legalizer.cpp` - Added `rule_sdefer()` and cmdqueue rules in `rule_map_ops()`

### Test Results

- **Golden Tests**: 56/56 pass
- **Runtime Tests**: 216/216 pass
- **Linker Tests**: All pass
- **Legality Gate Tests**: 14/14 pass

---

## Typed Script Pipeline Production Cutover - COMPLETED ✓

**SUCCESS**: The typed script pipeline (TypedScriptDecoder → CrystalCFG → SemanticLegalizer → legality gate → SemanticLinker) is now integrated into the production `FullGameCompiler`. All 1635 script bodies are processed through the new pipeline with hard failure semantics.

### Production Metrics

| Metric | Value | Status |
|--------|-------|--------|
| Maps discovered | 378 | ✅ |
| Map-root scripts | 1583 | ✅ |
| StdScript bodies | 52 | ✅ |
| Total bodies linked | 1635 | ✅ |
| Unresolved refs | 0 | ✅ |
| Invalid ownership | 0 | ✅ |
| Wrong type | 0 | ✅ |
| Elevators compiled | 2 | ✅ |

### Linked Corpus Summary

```
Map-root bodies:     1583
StdScript bodies:    52
Total unique bodies: 1635
ExactResolved:       336
OwnershipValidated:  938
RangeOnly:           4170
```

### Build Timing

- Discovery: ~60 ms
- Script Pipeline (decode → CFG → lower → legality → link): ~30 ms
- Asset Compilation: ~2 ms
- Linker: ~14 ms
- Serialization: ~4 ms
- **Total: ~112 ms**

### What Was Implemented

1. **Phase 2 added to FullGameCompiler**: New serial typed script pipeline phase
2. **init_typed_pipeline()**: Creates all pipeline components (TypedScriptDecoder, CFGBuilder, SemanticLegalizer, LegalityGate, SemanticLinker, registries)
3. **collect_script_addresses()**: Collects unique script addresses from all discovered maps
4. **process_script_typed()**: Processes single script through Stages 1-5 with hard failure
5. **process_map_root_scripts()**: Processes all 1310 map-root scripts
6. **process_std_scripts()**: Processes all 52 StdScript bodies
7. **finalize_registries()**: Populates elevator registry from discovered commands
8. **build_production_game_data()**: Builds CompiledGameData from actual discovered content
9. **link_scripts()**: Links full corpus through SemanticLinker

### Hard Failure Mode

The pipeline enforces hard failures - no Op_Raw degradation, no warning-and-continue:
- Empty command list → FATAL
- CFG validation failure → FATAL  
- Unlowered commands → FATAL
- Legality gate failure → FATAL
- Unresolved references → FATAL
- Invalid ownership → FATAL
- Wrong type references → FATAL

### Files Changed

- `frontends/crystal/compile/full_compiler.cpp` - Major rewrite adding typed pipeline
- `frontends/crystal/include/crystal/compile/full_compiler.hpp` - Added typed pipeline members and methods

### Test Results

- **Golden Tests**: 56/56 pass
- **Runtime Tests**: 142/142 pass
- **Linker Tests**: All pass (including elevator negative tests)

### What's Deferred

- **Lua emission (Stage 7)** - Scripts are linked but not yet emitted to package
- **Legacy file deletion** - Old decoder/emitter files remain (new path is independent)

---

## Native 8×8 Tile Rendering Architecture - COMPLETED ✓

**SUCCESS**: The tileset/render path now preserves 8×8 tile semantics instead of baking 32×32 metatiles. This is the superior foundation for future 3D/RT work.

### Why This Change

The baked 32×32 metatile approach collapsed information needed for:
- Materials and semantic tile properties
- Per-tile extrusion for 3D
- Per-tile animation without block rebaking
- Modding (swap/patch individual tiles)
- Future mesh generation and BLAS/TLAS construction

### New Native Model

```
RuntimeTile   → 8×8 RGBA pixels (256 bytes per tile)
RuntimeBlock  → 16 TileIds in row-major 4×4 order
RuntimeTileset → Tiles[] + Blocks[] + Collision[]
RuntimeMap    → BlockIds[] (unchanged)
```

EMON package now serializes this semantic representation.

### Files Created/Changed

**New Files**:
- `engine/include/engine/world/runtime_tileset.hpp` - RuntimeTile, RuntimeBlock, RuntimeTileset, TileAtlas
- `engine/world/runtime_tileset.cpp` - Deserialization from package, TileAtlas generation

**Updated Files**:
- `frontends/crystal/include/crystal/output/native_package.hpp` - Added `add_tileset()` method
- `frontends/crystal/output/native_package.cpp` - Serializes tiles (RGBA) + blocks (TileIds) + collision
- `frontends/crystal/compile/crystal_compiler.cpp` - Uses `add_tileset()` instead of `add_tileset_atlas()`
- `engine/include/engine/package/package_reader.hpp` - Added `load_tileset_data()` method
- `runtime/render/tile_renderer.hpp` - Takes `RuntimeTileset` instead of `RuntimeTilesetAtlas`
- `runtime/render/tile_renderer.cpp` - Expands blocks to tile instances on map load
- `runtime/main_tiles.cpp` - Uses `RuntimeTileset` throughout

### Rendering Architecture

```
On tileset load:
  RuntimeTileset → TileAtlas::from_tileset() → 128×96 atlas (16×12 tiles) → GPU texture

On map load:
  Map BlockIds → expand each block's 16 TileIds →
  generate tile instance vertices → batch upload → single draw call
```

Example: New Bark Town (10×9 blocks = 40×36 tiles = 1440 tile instances)
- 5760 vertices, 8640 indices
- Single batched draw call
- Nearest-neighbor pixel sampling

### Package Format

```
Tileset chunk:
  tile_count (u32)
  tiles[tile_count] - 64 RGBA32 pixels each (256 bytes)
  block_count (u32)  
  blocks[block_count] - 16 u16 tile_ids each (32 bytes)
  collision_count (u32)
  collision[collision_count] - raw bytes
```

### Verified Working

| Map | Blocks | Tiles Rendered | Status |
|-----|--------|----------------|--------|
| New Bark Town | 10×9 | 1440 | ✓ |
| Elm's Lab | 5×6 | 480 | ✓ |
| Player's House 1F | 5×4 | 320 | ✓ |
| Route 29 | 30×9 | 4320 | ✓ |
| All map transitions | - | - | ✓ |
| Collision | - | - | ✓ |
| Warps | - | - | ✓ |
| NPC interactions | - | - | ✓ |

### What's NOT Touched

- Collision system (uses tileset.collision)
- Movement/interaction logic
- Warps/connections
- Sprites
- Text/textbox rendering
- NPC duplication

### Future Optimization Note

If profiling proves worthwhile, the 2D renderer could generate a derived 32×32 block texture cache. But this is a renderer-local optimization, never canonical package/world data.

---

## Compiler/Runtime Boundary Module Cleanup - COMPLETED ✓

**SUCCESS**: The compiler/runtime boundary is now architecturally clean. The runtime executable contains ZERO Crystal frontend code and links only against the engine library.

### Boundary Violations Fixed

| Issue | Status |
|-------|--------|
| Runtime linked `enginemon_crystal` library | ✓ Fixed - removed from link |
| Runtime included `crystal/output/native_package.hpp` | ✓ Fixed - new `engine/package/` module |
| Runtime included `crystal/extract/sprite_extractor.hpp` | ✓ Fixed - `render_sprite_atlas()` moved to engine |
| `PackageReader` used `ExtractedMap` (frontend type) | ✓ Fixed - deserializes directly to `RuntimeMap` |
| Dead code `font_bridge.hpp` | ✓ Deleted |

### New Engine Package Module

**Files Created**:
- `engine/include/engine/package/package_format.hpp` - EMON format definitions (engine-owned)
- `engine/include/engine/package/package_reader.hpp` - Runtime package reader API
- `engine/package/package_reader.cpp` - Deserializes directly to runtime types
- `engine/world/sprite_atlas.cpp` - `render_sprite_atlas()` function (moved from Crystal)

**Key Changes**:
- `PackageReader` now lives in `enginemon::` namespace
- `load_map()` deserializes directly to `RuntimeMap` (no `ExtractedMap` intermediate)
- All package format types (`ChunkType`, `TocEntry`, etc.) owned by engine
- `calculate_crc32()` utility moved to engine

### Runtime Source Changes

**File**: `runtime/main_tiles.cpp`
- Replaced `#include "crystal/output/native_package.hpp"` with `#include "engine/package/package_reader.hpp"`
- Removed `#include "crystal/extract/sprite_extractor.hpp"` 
- Changed `crystal::PackageReader` → `PackageReader` (using namespace enginemon)
- Changed `crystal::render_sprite_atlas()` → `render_sprite_atlas()` (now in enginemon::)
- Changed `load_full_map()` → `load_map()` (new API returns RuntimeMap directly)

**File**: `runtime/CMakeLists.txt`
- Removed `enginemon_crystal` from `target_link_libraries`

**File Deleted**: `runtime/render/font_bridge.hpp`

### Build Verification

```powershell
# Runtime builds without Crystal library
cmake --build build --target enginemon_tiles --config Release
# Success - links only: enginemon_engine, Vulkan, SDL3
```

### Grep Verification

```
# No crystal/* includes in runtime:
grep -r "crystal/" runtime/  → 0 matches

# No crystal:: namespace in runtime:
grep -r "crystal::" runtime/  → 0 matches

# enginemon_crystal not linked:
grep "enginemon_crystal" runtime/CMakeLists.txt  → 0 matches
```

### Test Results
- **Golden Tests**: 56/56 pass
- **Runtime Tests**: 122/122 pass
- **Total**: 178/178 pass

### Verified Working
```powershell
.\build\runtime\Release\enginemon_tiles.exe "crystal.emon"
# Launches, loads package, renders correctly
```

---

## Sprite Package Serialization - COMPLETED ✓

**SUCCESS**: The compiler/runtime boundary now includes sprites. The runtime loads ALL data (maps, tilesets, scripts, AND sprites) entirely from the EMON native package without ANY ROM reads at runtime.

### Architecture Implemented

```
Crystal ROM → CrystalCompiler → EMON package (offline)
EMON package → PackageReader → RuntimeMap + RuntimeSprite + Scripts → Vulkan (runtime)
```

**Runtime contains ZERO:**
- ROM reads
- ROM file requirement
- Crystal banks/addresses/pointers
- MapExtractor/TilesetExtractor/SpriteExtractor calls
- ScriptDecoder/LuaEmitter calls
- Per-map script registries

### What Was Implemented

#### 1. Package Extended for Sprites ✓
**Files**: `frontends/crystal/include/crystal/output/native_package.hpp`, `.cpp`

**New Chunk Types**:
- `ChunkType::Sprites` (0x53505254 = "SPRT")
- `ChunkType::ObjPalettes` (0x4F424A50 = "OBJP")

**New Writer APIs**:
- `PackageWriter::add_sprite(RuntimeSprite)` - serialize sprite_id, type, palette, frames
- `PackageWriter::add_obj_palettes(SpriteObjPalettes)` - serialize all 4×8 ToD palettes

**New Reader APIs**:
- `PackageReader::load_sprite(sprite_id)` → `RuntimeSprite`
- `PackageReader::load_obj_palettes()` → `SpriteObjPalettes`
- `PackageReader::list_sprites()` → all sprite IDs

#### 2. CrystalCompiler Updated for Sprites ✓
**File**: `frontends/crystal/compile/crystal_compiler.cpp`

**Features**:
- Collects required sprite IDs from map objects during map compilation
- Compiles player sprite (chris) first
- Compiles all NPC sprites collected from maps
- Compiles OBJ palettes (shared across all sprites)
- Reports sprite count in compilation summary

#### 3. Runtime Rewritten for Package-Only Sprites ✓
**File**: `runtime/main_tiles.cpp`

**Removed**:
- `crystal::RomData` - no ROM dependency
- `crystal::SpriteExtractor` - no ROM extraction
- Second command-line argument for ROM path
- All Crystal extraction includes

**Added**:
- `PackageReader::load_sprite()` for player and NPC sprites
- `PackageReader::load_obj_palettes()` for time-of-day palettes
- Sprite cache in `PackageContext` for efficient reuse
- `load_world_state()` loads NPC sprites from package with caching

### Compilation Results
```
=== Crystal Compiler ===
Maps: 7
Tilesets: 5
Sprites: 11
Scripts: 52 (12 deduplicated)
Total Lua: 90637 bytes
```

### Test Results
- **Golden Tests**: 56/56 pass
- **Runtime Tests**: 122/122 pass
- **Total**: 178/178 pass

### Verified Working
Launched with EMON package ONLY (no ROM required):
```powershell
.\build\runtime\Release\enginemon_tiles.exe "crystal.emon"
```

Tested transitions:
- ✓ Walk around New Bark Town with player sprite
- ✓ NPC sprites (teacher, fisher, rival) render correctly
- ✓ Warp to Elm's Lab - new NPC sprites (elm) load from package
- ✓ Warp back to New Bark Town - sprites cached and reused
- ✓ Connection crossing to Route 29 - more NPCs load from package
- ✓ All map transitions work without ROM

### Build Commands (Updated)
```powershell
# Compile package (still requires ROM for one-time extraction)
.\build\tools\Release\emon_compile.exe "rom.gbc" "crystal.emon"

# Run game (ROM NOT required)
.\build\runtime\Release\enginemon_tiles.exe "crystal.emon"
```

---

## Previous: Compiler/Runtime Boundary - COMPLETED ✓

**SUCCESS**: The compiler/runtime boundary is now proven. The runtime loads maps, tilesets, and scripts entirely from the EMON native package without any Crystal extraction/decoding at runtime.

### Architecture Implemented

```
Crystal ROM → CrystalCompiler → EMON package (offline)
EMON package → PackageReader → RuntimeMap → scripts → LuaRuntime (runtime)
```

**Runtime contains ZERO:**
- ROM reads
- Crystal banks/addresses/pointers
- MapExtractor/TilesetExtractor calls
- ScriptDecoder/LuaEmitter calls
- Per-map script registries

### What Was Implemented

#### 1. Fixed Object Script ROM Address Extraction ✓
**Problem**: Object script addresses were incorrectly read as 3 bytes (bank+ptr) when they should be 2 bytes (ptr only, using map's script_bank).

**Fix in `frontends/crystal/extract/maps.cpp`**:
- Script pointer is at bytes 9-10 (2 bytes), uses map's script_bank
- Object type detection moved to byte 7 (nibble, OBJECTTYPE_TRAINER = 1)
- Event flag is at bytes 11-12 (was incorrectly used for script)

#### 2. CrystalCompiler Created ✓
**Files**:
- `frontends/crystal/include/crystal/compile/crystal_compiler.hpp`
- `frontends/crystal/compile/crystal_compiler.cpp`

**Features**:
- Owns MapExtractor, ScriptDecoder, LuaEmitter
- `compile_map()` extracts map and compiles all scripts
- `compile_map_scripts()` iterates events using pre-extracted `script_rom_address`
- Global script IDs: `map_id::local_script_id` (e.g., "new_bark_town::bg_event_0")
- Script deduplication by ROM address
- Discovers reachable maps via warps/connections

#### 3. Package Extended for Scripts ✓
**Files**: `frontends/crystal/include/crystal/output/native_package.hpp`, `.cpp`

**New APIs**:
- `PackageWriter::add_script(script_id, lua_code)`
- `PackageReader::load_script(script_id)` → Lua code string
- `PackageReader::list_scripts()` → all script IDs

#### 4. emon_compile Tool Created ✓
**File**: `tools/emon_compile.cpp`

Compiles ROM to EMON package:
```powershell
.\build\tools\Release\emon_compile.exe "rom.gbc" "crystal.emon"
```

Output: 7 maps, 5 tilesets, 52 scripts (12 deduplicated), 90637 bytes Lua

#### 5. Runtime Rewritten for Package Loading ✓
**File**: `runtime/main_tiles.cpp`

**Removed**:
- `SCRIPT_REGISTRY` and `lookup_script_address()`
- `g_current_map_id` 
- `extracted_to_runtime()` conversion function
- All Crystal frontend includes except `native_package.hpp`
- ROM-based map extraction

**Added**:
- `PackageContext` struct for package reader and sprites
- `load_world_state()` uses `PackageReader::load_full_map()`
- Script loader uses `PackageReader::load_script()`
- Tileset loading from package with `RuntimeTilesetAtlas::from_package_data()`

### Compilation Results
```
=== Crystal Compiler ===
Maps to compile: 7
Maps: 7
Tilesets: 5
Scripts: 52 (12 deduplicated)
Total Lua: 90637 bytes
```

### Test Results
- **Golden Tests**: 56/56 pass
- **Runtime Tests**: 122/122 pass
- **Total**: 178/178 pass

### Verified Working
Launched with EMON package (ROM still needed for sprites):
```powershell
.\build\runtime\Release\enginemon_tiles.exe "crystal.emon" "rom.gbc"
```

Tested interactions:
- ✓ Walk around New Bark Town
- ✓ Read town sign (script loaded from package)
- ✓ Talk to Teacher NPC (script loaded from package)
- ✓ Warp to Player's House
- ✓ Warp back to New Bark Town
- ✓ Warp to Elm's Lab
- ✓ Warp back to New Bark Town
- ✓ Map transitions load from package

### Remaining TODOs (Future Work)
1. **Package sprites** - Currently still requires ROM for sprite extraction
2. **Package fonts** - Font data in package but parsing not implemented
3. **Route 29 connection** - Connection crossing not tested yet

---

## Per-Tileset Collision Data - COMPLETED ✓

Fixed the generic collision system to use per-tileset collision data instead of hardcoded Johto outdoor table.

### The Problem
The runtime used a hardcoded `JOHTO_COLLISION_TABLE` (from `johto_collision.asm`) for ALL maps. But Crystal has **per-tileset collision data** - each tileset (johto_outdoor, lab, house, cave, etc.) has its own collision table extracted from ROM.

This is why New Bark Town worked (uses `johto_outdoor` tileset matching the hardcoded table) but Elm's Lab didn't (uses `lab` tileset with different collision).

### The Fix
**Reference**: Gen2Recomped Map.lua `cellTile()`, pokecrystal data/tilesets/*_collision.asm

**Files Changed**:
- `frontends/crystal/include/crystal/extract/tileset_extractor.hpp` - Changed `collision` from `std::vector<CollisionType>` (1 byte per metatile) to `std::vector<uint8_t>` (4 bytes per metatile: TL, TR, BL, BR)
- `frontends/crystal/extract/tilesets.cpp` - Updated extraction to read 4 bytes per metatile instead of 1
- `engine/include/engine/world/tileset_atlas.hpp` - Changed `RuntimeTilesetAtlas::collision` to `std::vector<uint8_t>`
- `engine/world/tileset_atlas.cpp` - Updated parsing to use `uint8_t` instead of `CollisionType`
- `engine/include/engine/world/johto_collision.hpp` - Removed hardcoded `JOHTO_COLLISION_TABLE`, updated `get_collision_from_blocks()` to accept collision data as parameter
- `runtime/main_tiles.cpp` - Updated all callers to pass `world_state.tileset_atlas.collision`
- `tests/scripting/runtime_test.cpp` - Added `get_collision_from_blocks_johto()` wrapper for backward compatibility, updated all test calls

### Collision Data Format
Each tileset has 4 bytes per metatile (TL, TR, BL, BR quadrants):
```
Index formula: collision[metatile_index * 4 + (cell_x % 2) + (cell_y % 2) * 2]

Quadrant mapping:
  TL (0,0) = 0
  TR (1,0) = 1  
  BL (0,1) = 2
  BR (1,1) = 3
```

Reference: Gen2Recomped Map.lua:
```lua
return collision[blockId * 4 + (cx % 2) + (cy % 2) * 2 + 1] or 0xFF
```

### Key Behavior Change
- Block 0 now always returns 0xFF (wall), matching Gen2Recomped's `if blockId == 0 then return 0xFF`
- Each tileset's collision is extracted at frontend time and stored in the native package
- Runtime uses the tileset-specific collision without any ROM reads or Crystal-specific code

### Test Results
- **Golden Tests**: 56/56 pass
- **Runtime Tests**: 122/122 pass
- **Total**: 178/178 pass

### Verified Working
- New Bark Town → Elm's Lab warp completes successfully
- Elm's Lab renders correctly with lab tileset collision
- NPC collision blocking works in both maps
- Warp exit (carpet tiles) detected correctly in Elm's Lab

---

## Warp/World Transitions - COMPLETED ✓

Semantic warp and world-transition system is now wired into the visible Vulkan gameplay loop with proper collision class checks.

### What Was Implemented

#### World Transition System ✓
**Reference**: Gen2Recomped Warp.lua, Map.lua gen2IsEntrance, pokecrystal home/map.asm

**Files Changed**:
- `runtime/main_tiles.cpp` - Major refactoring:
  - Added `WorldState` struct to hold map-dependent state (map, tileset_atlas, sprites, sprite_atlas)
  - Added `ExtractorContext` struct for global extractors
  - Added `TransitionContext` struct for renderer/game system references
  - Added `load_world_state()` function to extract map/tileset/sprites
  - Added `transition_to_map()` function to handle full map transitions
  - Added `g_tileset_cache` for tileset caching across maps
  - Added `SCRIPT_REGISTRY[]` array with map_id+script_id lookups (temporary - see notes)
  - Added warp detection with collision class check
  - Added connection crossing detection at map edges
  - Updated all references to use `world_state.*` instead of individual variables
- `engine/include/engine/world/johto_collision.hpp` - Added collision class helpers:
  - `is_warp_entrance(coll)` - checks if collision class triggers warps (0x60, 0x68, 0x70-0x7F)
  - `is_doorway_entrance(coll)` - checks for door/cave tiles (0x71, 0x7B)
  - `is_pit_collision(coll)` - checks for pit/hole tiles (0x60, 0x68)
  - `is_exit_carpet(coll)` - checks for exit carpet tiles (0x70, 0x76, 0x78, 0x7E)

#### Warp Detection Flow (Reference: Gen2Recomped Warp.lua onArrive)
1. Player movement completes (after 16 tick interpolation)
2. Get collision class at player position via `get_collision_from_blocks()`
3. Check `is_warp_entrance(coll)` - warp only triggers if collision class is valid
4. If valid, check `world_manager.get_warp_at(player_x, player_y)`
5. If warp found, execute via `world_manager.execute_warp()`
6. Call `transition_to_map()` to reload all state

#### Collision Class Semantics (from Gen2Recomped Map.lua)
| Class Range | Name | Behavior |
|-------------|------|----------|
| 0x60 | COLL_PIT | Fall-through hole, triggers warp |
| 0x68 | COLL_PIT_68 | Fall-through hole variant |
| 0x70 | WARP_CARPET_DOWN | Exit mat (down), needs d-pad held |
| 0x71 | COLL_DOOR | Outdoor building door |
| 0x72 | COLL_LADDER/STAIR | Ladder/stairway |
| 0x76 | WARP_CARPET_LEFT | Exit mat (left) |
| 0x78 | WARP_CARPET_RIGHT | Exit mat (right) |
| 0x7B | COLL_CAVE | Cave mouth entrance |
| 0x7E | WARP_CARPET_UP | Exit mat (up) |
| 0x70-0x7F | All | Valid warp entrance range |

#### Connection Detection Flow
1. After movement completes, check if not on a warp
2. Check `world_manager.is_at_connection_edge()` for map edge
3. If at edge, resolve via `world_manager.resolve_connection()`
4. Execute via `world_manager.execute_connection()`
5. Call `transition_to_map()` with landing coordinates

#### Key Features
- **Collision class validation** - Warps only trigger on valid entrance tiles
- **Reuses existing WorldManager** - No new warp logic, uses proven semantic system
- **Tileset caching** - Tilesets extracted once and reused
- **LAST_MAP/LAST_WARP support** - Preserves outdoor memory for interior exits
- **Clean state reset** - No stale renderer state survives transition
- **NPC/event isolation** - Old map NPCs cleared, new map NPCs loaded
- **Collision continuity** - Works immediately after transition

#### SCRIPT_REGISTRY Note (Temporary)
The `SCRIPT_REGISTRY` in `main_tiles.cpp` is a temporary workaround for script address resolution. The correct architecture should:
1. Store pre-decoded Lua scripts in the native package during extraction
2. Load scripts from package by semantic ID at runtime
3. Remove the ROM-based script decoding and address registry

This is tracked for future work - scripts currently decode on-demand from ROM using the registry.

### Test Results
- **Golden Tests**: 56/56 pass
- **Runtime Tests**: 122/122 pass
- **Total**: 178/178 pass (all existing tests still green)

### Verified Warps
| From | To | Trigger | Collision |
|------|----|---------|-----------|
| New Bark Town | Elm's Lab | Step on door tile at (6,3) | 0x71 (DOOR) |
| Elm's Lab | New Bark Town | LAST_MAP warp at exit | 0x70 (CARPET) |
| New Bark Town | Player's House | Step on house door | 0x71 (DOOR) |
| New Bark Town | Route 29 | Connection crossing at west edge | N/A |

### Architecture Notes
- No ROM reads in runtime - all extraction happens upfront
- No renderer-specific warp logic - WorldManager handles semantics
- Presentation state (interpolation, camera, animation) reset on transition
- Headless test coverage validates warp/connection math separately
- Collision class check follows Gen2Recomped's `Map.gen2IsEntrance()` exactly

---

## NPC Autonomous Movement - COMPLETED ✓

Real Crystal NPC overworld movement behavior is now implemented and working.

### What Was Implemented

#### NPC Movement Behavior System ✓
**Reference**: pokecrystal/engine/overworld/map_objects.asm, Gen2Recomped/src/world/NPC.lua

**Files Changed**:
- `engine/include/engine/core/game_loop.hpp` - Added `NpcMovementBehavior` enum, `movement_data_to_behavior()`, `movement_data_to_facing()` functions, extended `NpcState` with movement fields
- `engine/core/game_loop.cpp` - Implemented `update_npcs()`, `update_npc_behavior()`, `check_npc_can_move()`, `choose_npc_direction()`, `start_npc_movement()`, `complete_npc_movement()`, wired into `tick()`
- `runtime/main_tiles.cpp` - Initialize NPC behavior from `RuntimeObject.movement_type`, render NPCs with interpolated positions and animation
- `tests/scripting/runtime_test.cpp` - Added 11 NPC movement tests

#### NPC Movement Behaviors Supported
| Behavior | Crystal Constant | Description |
|----------|-----------------|-------------|
| Standing | SPRITEMOVEDATA_STILL, STANDING_* | Stands still, maintains facing |
| RandomWalkY | WALK_UP_DOWN | Walks up/down within radius |
| RandomWalkX | WALK_LEFT_RIGHT | Walks left/right within radius |
| RandomWalkXY | WANDER | Walks any direction within radius |
| RandomSpinSlow | SPINRANDOM_SLOW | Turns randomly (slow timer) |
| RandomSpinFast | SPINRANDOM_FAST | Turns randomly (fast timer) |

#### New Bark Town NPCs Verified
| NPC | Crystal movement_type | Behavior | Radius |
|-----|----------------------|----------|--------|
| Teacher | 0x03 (SPINRANDOM_SLOW) | Slow random spin | (1,0) |
| Fisher | 0x04 (WALK_UP_DOWN) | Walks up/down | (0,1) |
| Rival | 0x09 (STANDING_RIGHT) | Stands facing right | (0,0) |

#### Key Features
- **Deterministic RNG** - `set_rng_seed()` for reproducible movement
- **Collision checking** - NPCs respect tile collision, player, other NPCs, warps
- **Radius bounds** - NPCs stay within `radius_x` / `radius_y` from initial position
- **Frozen state** - NPCs freeze during script interaction
- **16-frame step timing** - Matches pokecrystal OBJECT_STEP_DURATION
- **Idle timer** - 30-127 frames between movement attempts (slow), 0-31 (fast)
- **50% turn vs walk** - Random walk behaviors have 50% chance to just turn without moving
- **Visual interpolation** - Smooth movement with walk animation phases

#### Timing Values (From References)
| Parameter | Value | Source |
|-----------|-------|--------|
| Step duration | 16 frames | pokecrystal StepVectors |
| Slow idle timer | 30-127 frames | Compromise of pokecrystal 0-127 and Gen2Recomped 30-180 |
| Fast idle timer | 0-31 frames | pokecrystal RandomStepDuration_Fast |
| Turn probability | 50% | Gen2Recomped NPC.lua |

### Test Results
- **Golden Tests**: 56/56 pass
- **Runtime Tests**: 122/122 pass (11 new NPC movement tests)
- **Total**: 178/178 pass

### New Tests Added
- `npc_movement_behavior_conversion` - Verifies movement_data_to_behavior()
- `npc_movement_facing_conversion` - Verifies movement_data_to_facing()
- `npc_idle_timer_countdown` - Verifies timer decrements each tick
- `npc_frozen_blocks_movement` - Verifies frozen NPCs don't move
- `npc_standing_never_moves` - Verifies Standing behavior is stationary
- `npc_spin_changes_facing` - Verifies spin changes facing but not position
- `npc_walk_changes_position` - Verifies walk actually moves NPC
- `npc_respects_radius_bounds` - Verifies NPC stays within radius
- `npc_collision_with_player` - Verifies NPC can't walk into player
- `npc_walk_up_down_direction` - Verifies WALK_UP_DOWN only moves vertically
- `newbark_npc_behaviors_extracted` - Verifies real New Bark NPCs have correct behaviors

---

## Text Status Note

- Semantic TextSequence preservation is complete
- LINE / NEXT / PARA / CONT / SCROLL / DONE / PROMPT survive ROM decode through ScriptIR/runtime distinctly
- Current textbox presentation is functional and usable
- Exact Crystal paging/layout fidelity is deferred
- Future UI may support Classic and Modern/resizable dialogue modes
- Cleanup later: runtime should consume semantic TextSequence directly instead of converting back to Crystal-style control bytes for the legacy page parser

---

## COMPLETED: Semantic Text Sequence Preservation ✓

Fixed the root cause of text capacity issues: the Crystal frontend was destroying text-control semantics by converting LINE/CONT/PARA control codes to plain `\n`/`\n\n` in the decoder, then trying to reconstruct them in the renderer - which is lossy (LINE and CONT both became `\n`).

### What Was Fixed

#### Semantic Text Pipeline ✓
**Problem**: Lossy text decoding destroyed the distinction between LINE (move to line 2, no wait) and CONT (wait → scroll → continue).

**Old broken flow**:
```
ROM <LINE>/<CONT>/<PARA>/... → decode_text() flattens controls to \n/\n\n → 
ScriptIR stores plain string → Lua receives plain string → 
renderer tries to reconstruct Crystal controls (LOSSY!)
```

**New correct flow**:
```
ROM bytes → decode_text_sequence() → semantic TextSequence in ScriptIR → 
Lua emits ctx.ui:text_sequence({...}) with semantic ops → 
runtime receives RuntimeTextSequence → renderer uses semantic controls directly
```

**Files Changed**:
- `frontends/crystal/include/crystal/script/ir.hpp` - Added `TextOp`, `TextElement`, `TextSequence` types
- `frontends/crystal/include/crystal/script/decoder.hpp` - Added `decode_text_sequence()` declaration
- `frontends/crystal/script/decoder.cpp` - Implemented `decode_text_sequence()` and `TextSequence::debug_string()`
- `frontends/crystal/script/lua_emitter.cpp` - Added `emit_text_sequence()` helper, updated text ops to use it
- `engine/include/engine/scripting/api_bindings.hpp` - Added `RuntimeTextOp`, `RuntimeTextElement`, `RuntimeTextSequence` types, `text_sequence()` API
- `engine/scripting/api_bindings.cpp` - Implemented `text_sequence()` function and `RuntimeTextSequence::debug_string()`
- `engine/scripting/lua_runtime.cpp` - Registered `text_sequence` in `bind_ui_api()`
- `runtime/render/textbox_renderer.hpp` - Added forward declaration and `open_with_sequence()` method
- `runtime/render/textbox_renderer.cpp` - Implemented `open_with_sequence()` and `encode_utf8_to_crystal()` helper
- `runtime/main_tiles.cpp` - Added `text_sequence_callback` wiring

### Control Code Semantics (Preserved)
| Code | Name | Crystal Byte | Behavior |
|------|------|--------------|----------|
| `Text` | - | - | Printable character run |
| `Line` | `<LINE>` | 0x4F | Move to line 2, no wait |
| `Next` | `<NEXT>` | 0x4E | Clear box, continue (no wait) |
| `Para` | `<PARA>` | 0x51 | Wait → Clear → Continue |
| `Cont` | `<CONT>` | 0x55/0x4B | Wait → Scroll → Continue |
| `Done` | `<DONE>` | 0x57 | End text processing |
| `Prompt` | `<PROMPT>` | 0x58 | Show cursor, wait, end |

### Golden Fixture: NewBarkTownSign
Verified exact semantic sequence from ROM:
```
Text("NEW BARK TOWN"), Para, Text("The Town Where the"), Line,
Text("Winds of a New"), Cont, Text("Beginning Blow"), Done
```

LINE (element 3) and CONT (element 5) are now distinguishable through the entire pipeline.

### Test Results
- **Golden Tests**: 56/56 pass (2 new semantic tests added)
- **Runtime Tests**: 111/111 pass
- **New Tests**:
  - `script_semantic_text_line_vs_cont` - Verifies LINE and CONT remain distinct in TextSequence
  - `script_lua_emitter_text_sequence` - Verifies Lua emitter produces `text_sequence()` with semantic ops

---

## Previous: Text Flow Consistency Fix ✓

Fixed three text-flow issues: repeated text after advancing, one-line-down start bug, and prompt arrow collision.

### What Was Fixed

#### 1. CONT Pages Limited to 1 Line ✓
**Problem**: CONT pages could have 2 lines, causing the scroll + show-new-content to display too much at once (skipping the intermediate state where only 1 new line appears after scroll).

**Solution**: Modified `parse_text_pages()` to enforce max 1 line for CONT pages (`is_cont_page = true`). Normal pages (first page or after PARA) can have 2 lines. This matches Gen2Recomped's model where `beginLine()` scrolls and shows one new line at a time.

**Files Changed**: `runtime/render/textbox_renderer.cpp` - Added `page_max_lines = is_cont_page ? 1 : max_lines` check

#### 2. Prompt Arrow Position ✓
**Problem**: Prompt arrow was rendered at `text_y + (max_lines - 1) * tile` which placed it on the second text line, colliding with long multi-line text.

**Solution**: Moved prompt arrow to bottom-right corner of box interior:
- X: `(box_width_tiles - 2) * tile` = tile 18 (unchanged)
- Y: `(box_tile_y + box_inner_height + 1) * tile - tile / 2` = tile 17 minus half tile

This matches Gen2Recomped: `(boxTy + boxTh - 1) * 8 - 4`

**Files Changed**: `runtime/render/textbox_renderer.cpp` - `add_text()` cursor positioning

#### 3. Visible Buffer Update Simplified ✓
**Problem**: Complex logic in `advance_page()` for handling 2-line CONT pages was error-prone.

**Solution**: Since CONT pages now have max 1 line, the visible buffer update is straightforward:
- PARA: `visible.clear()`, populate from new page (can have 1-2 lines)
- CONT: `visible.scroll()`, `visible.line2 = new_page.lines[0]`

**Files Changed**: `runtime/render/textbox_renderer.hpp` - `advance_page()`

### Reference Cross-Check
Verified against Gen2Recomped TextBox.lua:
- `self.shown` = array of max 2 visible lines
- `beginLine()`: if `#shown >= 2`, remove first (scroll), then append empty
- PARA: `self.shown = {}` then `beginLine()`
- CONT: `beginLine()` (scrolls if 2 lines, adds one new line)
- Prompt at `(boxTx + boxTw - 2) * 8, (boxTy + boxTh - 1) * 8 - 4`

### Test Results
- **Golden Tests**: 54/54 pass
- **Runtime Tests**: 111/111 pass
- **Visual Verification**: 
  - Prompt arrow at bottom-right corner, doesn't collide with text
  - CONT scrolls one line at a time
  - PARA clears and shows fresh content
  - No repeated text after advancing

---

## Previous: Visible Text Buffer/Cursor Consistency Fix ✓

Fixed the text stream state machine to properly handle multi-page dialogue with PARA and CONT control codes.

### What Was Fixed

#### Text Stream State Machine ✓
**Problem**: Pages were modeled as independent strings that got discarded after A-press, causing text to be cut off or dialogue to terminate early.
**Solution**: 
- Added `PageMeta` struct with `stream_start`, `stream_end`, `ends_with_para`, `ends_with_cont`, `is_final` fields
- Modified `advance_page()` to return `true` when successfully moving to a new page (even if final), and `false` only when already on final page
- Pages are now tracked with their stream positions and wait types

**Files Changed**:
- `runtime/render/textbox_renderer.hpp` - Added `TextStreamState`, `PageMeta` structs, updated `TextboxState` with `page_meta` vector
- `runtime/render/textbox_renderer.cpp` - Rewrote `parse_text_pages()` to properly track page boundaries and stream positions
- `runtime/main_tiles.cpp` - Updated A-button handling to use new `advance_page()` return value semantics

**Control Code Semantics (pokecrystal/home/text.asm reference)**:
| Code | Name | Behavior |
|------|------|----------|
| 0x4F | `<LINE>` | Jump to line 2 (no wait) |
| 0x51 | `<PARA>` | Wait → Clear → Continue SAME text stream |
| 0x55/0x4B | `<CONT>` | Wait → Scroll → Continue SAME text stream |
| 0x57 | `<DONE>` | Terminate text stream (box stays open) |
| 0x58 | `<PROMPT>` | Wait → Terminate |

**Key Behavior**:
- `advance_page()` returns `true` when advancing to any page (including final)
- `advance_page()` returns `false` only when already on final page (text complete)
- PARA creates fresh page (box clears)
- CONT preserves last line for scroll continuity

### Test Results
- **Golden Tests**: 54/54 pass
- **Runtime Tests**: 110/111 pass (1 pre-existing failure unrelated to this work)
- **New Multi-Page Tests**: 4/4 pass
  - `multipage_text_stream_encoding` - Verifies LINE/PARA markers in encoded text
  - `multipage_text_with_para_advances_all_pages` - Verifies 3-page PARA dialogue
  - `multipage_text_with_cont_preserves_scroll_line` - Verifies CONT scroll behavior
  - `multipage_rival_script_three_segments` - Verifies 3 A-presses to complete 3-page text

---

## Previous: Crystal-Authentic Text Presentation ✓

All three issues from the text milestone have been fixed and verified.

### What Was Fixed

#### 1. Opaque Textbox Background + Border ✓
**Problem**: Textbox only showed transparent text, no visible box/border.
**Solution**: Added `add_solid_quad()` that draws an opaque white rectangle FIRST, before border glyphs (matching Gen2Recomped `Font.drawBox()` pattern).
**Files Changed**:
- `runtime/render/textbox_renderer.cpp` - Added `add_solid_quad()`, modified `add_border()` to draw opaque white background first
- `runtime/render/textbox_renderer.hpp` - Added `add_solid_quad()` declaration
- `runtime/shaders/textbox.frag` - Modified to handle solid background (when texture alpha ≈ 0 but vertex alpha = 1, output solid color)

#### 2. Multi-Page Text Handling ✓
**Problem**: A-button always closed dialog instead of advancing pages.
**Solution**: 
- Added UTF-8 to Crystal encoding in `TextboxState::encode_text_to_crystal()`
- Enhanced `parse_text_pages()` to track PARA/CONT/DONE page types
- Modified A-button handler to advance pages before resuming script

**Control Code Semantics Implemented**:
| Code | Name | Behavior |
|------|------|----------|
| 0x4F | `<LINE>` | Jump to line 2 (no wait) |
| 0x51 | `<PARA>` | Wait → Clear → Continue (page break) |
| 0x55/0x4B | `<CONT>` | Wait → Scroll → Continue |
| 0x57 | `<DONE>` | Terminate text stream (box stays open) |
| 0x58 | `<PROMPT>` | Wait → Terminate |

**Files Changed**:
- `runtime/render/textbox_renderer.hpp` - Added `encode_text_to_crystal()`, added `waiting_for_para`, `waiting_for_cont`, `text_complete` state tracking
- `runtime/render/textbox_renderer.cpp` - Rewrote `parse_text_pages()` to track page types
- `runtime/main_tiles.cpp` - Modified A-button handler: advance pages first, only resume script when no more pages

#### 3. é Character Rendering ✓
**Problem**: "POKéMON" rendered as "POK MON" - missing é.
**Solution**: Added proper UTF-8 to Crystal code mapping in `add_text()` and `encode_text_to_crystal()`.

**UTF-8 to Crystal Mappings Added**:
- é (0xC3 0xA9) → 0xEA
- Ä (0xC3 0x84) → 0xC0
- Ö (0xC3 0x96) → 0xC1
- Ü (0xC3 0x9C) → 0xC2
- ä (0xC3 0xA4) → 0xC3
- ö (0xC3 0xB6) → 0xC4
- ü (0xC3 0xBC) → 0xC5

**Files Changed**:
- `runtime/render/textbox_renderer.cpp` - Rewrote `add_text()` to handle multi-byte UTF-8 sequences

### Verified Working
- Textbox renders with opaque white background + black border glyphs
- New Bark Town sign: 2 pages, A advances page 1→2, second A closes
- Fisher NPC: 2 pages, multi-page advance works
- Rival NPC: 3 pages, A advances 1→2→3, then closes  
- Teacher NPC: 2 pages, multi-page advance works
- Text containing POKéMON/POKéGEAR renders é correctly
- Script lifecycle: open_text → text → A-advances-pages → resume_script → close_text
- Dialog only closes when script calls close_text(), not on A press

### Test Results
- **Golden Tests**: 54/54 pass
- **Runtime Tests**: 107/107 pass
- **Total**: 161/161 pass

---

## Previous: Font Extraction from ROM ✓

Completed font extraction from ROM bytes (not PNG files).

### Files Created/Modified
- `frontends/crystal/include/crystal/extract/font_extractor.hpp`
- `frontends/crystal/extract/fonts.cpp` - ROM-based extraction
- `runtime/render/textbox_renderer.hpp` - RuntimeFontAtlas, TextboxState
- `runtime/render/textbox_renderer.cpp` - Vulkan textbox rendering
- `runtime/render/font_bridge.hpp` - Crystal→Runtime font conversion
- `runtime/shaders/textbox.frag` - Fragment shader with solid background support

---

## Script Timing Correctness Pass - COMPLETED ✓

**SUCCESS**: Fixed two script timing bugs: (1) timed resumes inside `LuaRuntime::update()` now set `script_resumed`, (2) `WaitSeconds` now uses proper duration timing instead of immediate resume.

### Bug 1: Timed Resume Reporting - FIXED ✓

**Problem**: `TickResult::script_resumed` only tracked explicit `HeadlessGameLoop::resume_script()` calls. But `LuaRuntime::update()` also resumes coroutines when WaitFrames/WaitSeconds timers expire - these resumes were not captured.

**Fix**: Added resume tracking to `LuaRuntime`:
- New member: `bool resumes_occurred_this_update_`
- New method: `bool consume_resumes_occurred()` - returns and clears the flag
- `update()` sets flag before each `resume()` call inside the timer loop
- `HeadlessGameLoop::update_script()` consumes this flag into `script_resumed_this_tick_`

### Bug 2: WaitSeconds Duration - FIXED ✓

**Problem**: `WaitSeconds` yield had no explicit handling in `update_script()` - it fell through to the default case which called `resume_script()` immediately (next tick), ignoring the requested duration.

**Fix**: Added explicit `WaitSeconds` case that mirrors `WaitFrames`:
- Uses same deterministic simulation delta: `1.0f / 60.0f` seconds per tick
- Calls `lua_runtime_->update(delta)` which handles the countdown
- Conversion: `WaitSeconds(s)` waits for `s * 60` ticks

### Implementation Details

**Files Modified**:
- `engine/include/engine/scripting/lua_runtime.hpp` - Added `resumes_occurred_this_update_` member and `consume_resumes_occurred()` method
- `engine/scripting/lua_runtime.cpp` - Set flag before resume in `update()`, implemented `consume_resumes_occurred()`
- `engine/core/game_loop.cpp` - Added explicit `WaitSeconds` case in `update_script()`, consume resume flag for both WaitFrames and WaitSeconds

### Timing Specification

| Yield | Resolution | Conversion |
|-------|------------|------------|
| WaitFrames(n) | 1 frame = 1 tick | Exact frame count |
| WaitSeconds(s) | 1/60 sec per tick | s * 60 ticks |
| WaitSeconds(0) | Immediate expiry | Resumes on first tick |

### New Tests Added (7 tests)

| Test | Verifies |
|------|----------|
| `wait_frames_before_expiry_no_resume` | WaitFrames before timer expires: `script_resumed == false` |
| `wait_frames_expiry_sets_resumed` | WaitFrames timer expires: `script_resumed == true` |
| `wait_frames_expiry_reyield_sets_resumed` | WaitFrames expires + immediate re-yield: `script_resumed == true` |
| `wait_seconds_not_immediate_resume` | WaitSeconds(0.5s) does NOT resume in first 10 ticks |
| `wait_seconds_resumes_after_duration` | WaitSeconds(0.05s) resumes after 3 ticks |
| `wait_seconds_resume_sets_flag` | WaitSeconds resume sets `script_resumed == true` |
| `wait_seconds_zero_duration` | WaitSeconds(0) resumes on first tick |

### Test Results

- **Runtime Tests**: 269/269 pass (262 + 7 new timed yield tests)
- **All key invariants preserved**

---

## Script Lifecycle Correctness Pass - COMPLETED ✓

**SUCCESS**: Fixed two script lifecycle bugs: (1) completion vs error distinction in terminal states, (2) `reset()` now cancels active coroutine to prevent orphaned timed resumes.

### Bug 1: Completion vs Error Distinction - FIXED ✓

**Problem**: `update_script()` returned `true` for both `Finished` (normal completion) and `Error` states. `start_script()` returned `true` even on immediate Lua error. This made it impossible to distinguish successful script completion from failure.

**Fix**: Added explicit error tracking:
- New `script_error` field in `TickResult` struct
- New `script_error_this_tick_` member in `HeadlessGameLoop`
- `start_script()` returns `false` on immediate Lua error (syntax or runtime)
- `resume_script()` sets `script_error_this_tick_` when script enters Error state
- `update_script()` returns `true` only for `Finished`, `false` for `Error`

**Semantic Distinction**:
| State | `script_complete` | `script_error` |
|-------|------------------|----------------|
| Normal completion | true | false |
| Script error | false | true |
| Still running/yielded | false | false |

### Bug 2: reset() Cancels Active Coroutine - FIXED ✓

**Problem**: `HeadlessGameLoop::reset()` cleared `active_coroutine_` but didn't cancel the coroutine in `LuaRuntime`. This left orphaned coroutines that could still fire timed resumes (WaitFrames/WaitSeconds) after reset.

**Fix**: Modified `reset()` to call `lua_runtime_->cancel(active_coroutine_)` before clearing loop state:
- Registry reference is released (coroutine won't prevent GC)
- Coroutine is removed from `LuaRuntime::coroutines_` map
- No timed resume can occur after reset

### Implementation Details

**Files Modified**:
- `engine/include/engine/core/game_loop.hpp` - Added `script_error` to `TickResult`, added `script_error_this_tick_` member
- `engine/core/game_loop.cpp` - Modified `start_script()`, `resume_script()`, `update_script()`, `tick()`, `reset()`

### New Tests Added (9 tests)

| Test | Verifies |
|------|----------|
| `script_finishes_normally_sets_complete` | Normal completion: `script_complete=true`, `script_error=false` |
| `script_errors_after_resume_sets_error` | Error after resume: `script_error=true`, `script_complete=false` |
| `script_errors_immediately_returns_false` | Immediate syntax error: `start_script()` returns `false` |
| `script_runtime_error_during_start_returns_false` | Immediate runtime error: `start_script()` returns `false` |
| `yielded_script_remains_nonterminal` | Yielded: both `script_complete` and `script_error` are `false` |
| `reset_cancels_active_coroutine_no_timed_resume` | After reset, old coroutine does not fire timed resume |
| `reset_when_no_script_active` | Reset succeeds safely when no script active |
| `reset_after_script_completed` | Reset succeeds safely after script completed |
| `reset_when_script_yielded` | Reset cancels yielded coroutine |

### Lifecycle Contract (Final)

| Terminal State | Meaning |
|---------------|---------|
| `Finished` | Completed normally |
| `Error` | Failed with error |
| `Cancelled/reset` | Cancelled, not completed |
| `Yielded` | Still active |

`script_resumed` remains independent from terminal status.

### Test Results

- **Runtime Tests**: 278/278 pass (269 + 9 new lifecycle tests)
- **All key invariants preserved**

---

## Test Results (All Pass)

- **Runtime Tests**: 290/290 pass
- **Golden Tests**: 56/56 pass
- **Legality Gate Tests**: 14/14 pass
- **Corpus Test**: PASS (decoder/CFG integrity)
- **Corpus Lowering Audit**: 1788/1788 SUCCESS
- **Linker Test**: corpus=1788/1788, InvalidOwnership=0

---

## Build Commands

```powershell
# Build tile renderer
& "C:\Program Files\CMake\bin\cmake.exe" --build build --target enginemon_tiles --config Release

# Run tile renderer
.\build\runtime\Release\enginemon_tiles.exe "crystal.emon"
```

## Run All Tests (Canonical Verifier)

```powershell
# Find ROM path and run canonical verifier
$romPath = (Get-ChildItem -Path "references" -Filter "*.gbc" | Select-Object -First 1).FullName
.\run_all_tests.ps1 -RomPath $romPath
```

This runs all 6 test suites and reports key invariants:
- corpus lowering = 1788/1788
- linker corpus = 1788/1788
- InvalidOwnership = 0
- decoder/CFG = PASS

## Run Individual Test Suites

```powershell
.\build\tests\Release\runtime_test.exe "references\Pokemon - Crystal Version (UE) (V1.1) [C][!].gbc"
.\build\tests\Release\golden_test.exe "references\Pokemon - Crystal Version (UE) (V1.1) [C][!].gbc"
.\build\tests\Release\legality_gate_test.exe "references\Pokemon - Crystal Version (UE) (V1.1) [C][!].gbc"
.\build\tests\Release\corpus_test.exe "references\Pokemon - Crystal Version (UE) (V1.1) [C][!].gbc"
.\build\tests\Release\linker_test.exe "references\Pokemon - Crystal Version (UE) (V1.1) [C][!].gbc"
.\build\tools\Release\corpus_lowering_audit.exe "references\Pokemon - Crystal Version (UE) (V1.1) [C][!].gbc"
```

---

## Quality Gates

| Gate | Status |
|------|--------|
| Metatile structure (4×4 tiles, 32×32 px) | ✓ Complete |
| Atlas dimensions correct | ✓ 256×512 |
| Y-axis orientation correct | ✓ Complete |
| Camera/framing correct | ✓ Complete |
| Palette map extraction | ✓ Complete |
| Time-of-day BG palettes | ✓ Complete |
| Per-tile palette assignment | ✓ Complete |
| Sprite extraction (24 tiles) | ✓ Complete |
| OBJ palettes (4 ToD × 8 pal) | ✓ Complete |
| Sprite transparency (color 0) | ✓ Complete |
| Facing frame selection | ✓ Complete |
| Walk animation phase | ✓ Complete |
| 16-frame interpolation | ✓ Complete |
| Per-frame sprite buffers | ✓ Complete |
| Step completion state sync | ✓ Complete |
| No ghosting on movement | ✓ Complete |
| Stop resolves to exact tile | ✓ Complete |
| HeadlessGameLoop integration | ✓ Complete |
| Collision data pipeline | ✓ Complete |
| NPC collision blocking | ✓ Complete |
| Tile collision blocking | ✓ Complete |
| A-button interaction routing | ✓ Complete |
| Sprite ID mapping fix | ✓ Complete |
| Lua script execution | ✓ Complete |
| Sign scripts work | ✓ Complete |
| NPC scripts work | ✓ Complete |
| Multiple scripts sequential | ✓ Complete |
| Visual textbox rendering | ✓ Complete |
| Dialog input handling | ✓ Complete |
| Movement lock during dialog | ✓ Complete |
| Crystal font extraction | ✓ Complete |
| Crystal charmap compiler | ✓ Complete |
| Font atlas Vulkan upload | ✓ Complete |
| **Opaque textbox background** | ✓ **Complete** |
| **Border glyph rendering** | ✓ **Complete** |
| **Multi-page text parsing** | ✓ **Complete** |
| **Multi-page A-button advance** | ✓ **Complete** |
| **é character rendering** | ✓ **Complete** |
| Font ROM extraction | ✓ Complete |
| Font charmap/é mapping | ✓ Complete |
| **NPC autonomous movement** | ✓ **Complete** |
| **NPC movement behaviors** | ✓ **Complete** |
| **NPC collision with player/NPCs** | ✓ **Complete** |
| **NPC radius bounds** | ✓ **Complete** |
| **NPC visual interpolation** | ✓ **Complete** |
| **Warp detection on step complete** | ✓ **Complete** |
| **Warp collision class validation** | ✓ **Complete** |
| **World transition reload** | ✓ **Complete** |
| **Connection crossing detection** | ✓ **Complete** |
| **LAST_MAP/LAST_WARP support** | ✓ **Complete** |
| **Tileset caching** | ✓ **Complete** |
| **NPC/event state isolation** | ✓ **Complete** |
| **Per-tileset collision data** | ✓ **Complete** |
| **Indoor map collision (Elm's Lab)** | ✓ **Complete** |
| **Sprite package serialization** | ✓ **Complete** |
| **OBJ palette package serialization** | ✓ **Complete** |
| **Runtime ROM independence** | ✓ **Complete** |
| **Package-only sprite loading** | ✓ **Complete** |
| **Sprite caching across maps** | ✓ **Complete** |
| **Compiler/runtime module boundary** | ✓ **Complete** |
| **PackageReader in engine namespace** | ✓ **Complete** |
| **Direct RuntimeMap deserialization** | ✓ **Complete** |
| **Runtime zero crystal/* includes** | ✓ **Complete** |
| **Runtime zero enginemon_crystal link** | ✓ **Complete** |
| **Scene script discovery** | ✓ **Complete** |
| **Callback script discovery** | ✓ **Complete** |
| Golden tests pass | ✓ 56/56 |
| Runtime tests pass | ✓ 216/216 |
| Linker tests pass | ✓ All |
| Legality gate tests pass | ✓ 14/14 |

---

## Key Symbol Addresses (Verified)

| Symbol | Bank:Address | Purpose |
|--------|-------------|---------|
| Font | 3e:4200 | Main font, 128 1bpp tiles |
| FontExtra | 3e:4000 | Extra font (borders), 32 2bpp tiles |
| OverworldSprites | 05:4736 | Sprite metadata table |
| MapObjectPals | 02:7469 | OBJ sprite palettes |
| TilesetBGPalette | 02:7319 | BG time-of-day palettes |
| TilesetJohtoPalMap | 13:40e5 | Tile→palette map |
| NewBarkTownSign | 6a:40c8 | Town sign script |
| NewBarkTownTeacherScript | 6a:406f | Teacher NPC script |
| NewBarkTownFisherScript | 6a:409b | Fisher NPC script |
| NewBarkTownRivalScript | 6a:409e | Rival NPC script |

---

## Previous Milestones (Complete)

- SDL3 + Vulkan 1.3 Bootstrap ✓
- World Continuity + Persistence ✓
- Headless Playable Loop ✓
- Metatile 4×4 Structure Fix ✓
- Y-Axis Orientation Fix ✓
- Crystal BG Palette Pipeline ✓
- Full Map View Smoke Test ✓
- Sprite Extraction Pipeline ✓
- OBJ Palette/Transparency ✓
- Sprite Movement/Facing ✓
- Movement-Presentation Sync ✓
- Collision/Interaction Integration ✓
- Lua Script Execution ✓
- Visual Textbox/Dialog ✓
- Crystal Font ROM Extraction ✓
- Opaque Textbox Background ✓
- Multi-Page Text Handling ✓
- é Character Rendering ✓
- NPC Autonomous Movement ✓
- Warp/World Transitions ✓
- Per-Tileset Collision Data ✓
- **Sprite Package Serialization ✓**
- **Compiler/Runtime Module Boundary ✓**

---

## Next Steps (Future)

1. **Scroll animation for CONT** - Implement the 2-line scroll visual animation (currently instant)
2. **Clear animation for PARA** - Implement the clear + delay visual effect (currently instant)
3. **Typewriter text reveal** - Character-by-character text display with speed options
4. **Font parsing from package** - Implement RuntimeFontAtlas::from_package_data() for text rendering
5. **Tile Animation** - Water shimmer, flower animation
6. **NPC interaction freeze** - Freeze NPC during script interaction (framework ready, needs wiring)
7. **Scripted NPC movement** - applymovement command support
8. **Missing sprite IDs** - Some sprite indices (60, 67, 84, 93, 240-243) don't map to named sprites
