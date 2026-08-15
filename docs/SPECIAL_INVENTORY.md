# Special ID Inventory - Batch 3 Complete

## Part 1: Authoritative Special Domain

**Source**: `references/pokecrystal/data/events/special_pointers.asm`

| Metric | Value |
|--------|-------|
| Exact entry count | 169 |
| Valid IDs | 0-168 (contiguous, no holes) |
| First valid ID | 0 (WarpToSpawnPoint) |
| Last valid ID | 168 (UnusedDummySpecial) |
| Null entries | None |

## Part 2: Batch 3 Final Results

### Pre-Batch 3 (Batches 1+2 Complete)
- **Remaining Sem_Special unique IDs**: 108
- **Remaining Sem_Special occurrences**: 283

### Post-Batch 3 
- **Remaining Sem_Special unique IDs**: 107
- **Remaining Sem_Special occurrences**: 275

### Lowered to Semantic Ops (Batch 3)

| Semantic Op | Occurrences | Special IDs Lowered |
|-------------|-------------|---------------------|
| Sem_HealParty | 8 | 27 |
| **Total** | **8** | **1 unique ID** |

### Batch 3 Reduction
- Unique IDs: 108 → 107 (-1)
- Occurrences: 283 → 275 (-8)

---

## Part 2.1: Batch 3 Semantic Op Definition

### Sem_HealParty (Special 27)

```cpp
struct Sem_HealParty {};  // Empty struct, 1 byte
```

| Special ID | Symbol | Count |
|------------|--------|-------|
| 27 | HealParty | 8 |

**Contract** (source-proven from `pokecrystal/engine/pokemon/health.asm`):
- Iterates through all party slots
- **SKIPS eggs** (`cp EGG` / `jr z, .next`)
- For each non-egg member:
  1. Restore HP to max (revives fainted Pokemon)
  2. Clear all status conditions (poison, burn, sleep, freeze, paralyze)
  3. Restore all move PP to maximum (preserving PP Up investment)

**PP Restoration Formula** (from Gen2Recomped):
```
max_pp = base_pp + (base_pp / 5) * pp_ups
```
- Each PP Up adds 20% of base PP to the maximum
- PP Up investment (0-3 per move) is PRESERVED, not modified

**What is NOT modified**:
- DVs, Stat Exp, Level, Experience
- Friendship/Happiness
- Held items
- Pokérus status
- Met info (location, level, time)
- Egg status/cycles (eggs are skipped entirely)

**Script Result**:
- Does NOT modify wScriptVar
- Produces no script result value

**Source**: `pokecrystal/engine/pokemon/health.asm HealParty`

---

## Part 3: Cumulative Results (Batches 1+2+3)

### Initial State
- **Unique Special IDs encountered**: 122
- **Total Special occurrences**: 400

### Post Batch 1+2+3
- **Remaining Sem_Special unique IDs**: 107
- **Remaining Sem_Special occurrences**: 275

### All Lowered Semantic Ops

| Semantic Op | Occurrences | Special IDs |
|-------------|-------------|-------------|
| Sem_ScreenFade | 36 | 46, 48, 49, 50 |
| Sem_SyncPalettes | 7 | 51, 52, 164 |
| Sem_RefreshPlayerSprite | 5 | 56 |
| Sem_SyncSprites | 18 | 94 |
| Sem_RebuildSprites | 1 | 158 |
| Sem_RestartMapMusic | 24 | 61 |
| Sem_FadeToSilence | 10 | 106 |
| Sem_WaitSound | 1 | 59 |
| Sem_PlayMapMusic | 15 | 60 |
| Sem_HealParty | 8 | 27 |
| **Total** | **125** | **15 unique IDs** |

### Cumulative Reduction
- Unique IDs: 122 → 107 (-15)
- Occurrences: 400 → 275 (-125)

---

## Part 4: Deliberately Unclassified (need context tracking)

| Special ID | Symbol | Count | Reason |
|------------|--------|-------|--------|
| 57 | GameCornerPrizeMonCheckDex | ? | Uses wScriptVar for species - needs setval→special context tracking |
| 95 | PlaySlowCry | 2 | Uses wScriptVar for species - needs setval→special context tracking |
| 100 | PlayCurMonCry | 7 | Uses wCurPartySpecies - needs grooming/selection context pattern |
| 152 | SetPlayerPalette | 5 | Uses wScriptVar for palette_id - needs setval→special context tracking |

These require context establishment patterns (`setval` → `special`) that are not yet implemented.

---

## Part 5: Invariants

### No Raw Crystal Identity

All lowered operations contain:
- ✅ 0 raw SpecialId fields
- ✅ 0 Crystal symbol dispatch
- ✅ 0 magic numeric modes

Verified by compile-time static assertions:
```cpp
// Batch 1
static_assert(sizeof(Sem_ScreenFade) == 3);        // direction + color + prefill only
static_assert(sizeof(Sem_SyncPalettes) == 1);      // wait_frames only
static_assert(sizeof(Sem_RefreshPlayerSprite) == 1);
static_assert(sizeof(Sem_SyncSprites) == 1);
static_assert(sizeof(Sem_RebuildSprites) == 1);
static_assert(sizeof(Sem_RestartMapMusic) == 1);
static_assert(sizeof(Sem_FadeToSilence) == 1);

// Batch 2
static_assert(sizeof(Sem_WaitSound) == 1);
static_assert(sizeof(Sem_PlayMapMusic) == 1);

// Batch 3
static_assert(sizeof(Sem_HealParty) == 1);
```

### Behavioral Preservation

| Operation | Timing | Blocking | State Effects | Observable |
|-----------|--------|----------|---------------|------------|
| Sem_WaitSound | Variable | Yes | None | Script waits for SFX |
| Sem_PlayMapMusic | 1 frame gap | Yes (briefly) | wMapMusic updated | Music change |
| Sem_HealParty | Instant | No | Party HP/status/PP | Party healed |

---

## Part 6: Test Results

- **Golden Tests**: 56/56 pass
- **Runtime Tests**: TBD (5 new Batch 3 tests expected)
- **Linker Tests**: All pass
- **Legality Gate Tests**: All pass

### Batch 3 Tests

| Test | Validates |
|------|-----------|
| `batch3_special_27_heals_party` | Special 27 → Sem_HealParty with correct contract |
| `batch3_no_sem_special_for_27` | Does not produce Sem_Special |
| `batch3_heal_party_pp_formula` | PP formula: base + (base/5)*pp_ups |
| `batch3_heal_party_egg_skip` | Eggs skipped, non-eggs healed, fainted revived |
| `batch3_heal_party_no_script_result` | Does not modify wScriptVar |

---

## Part 7: Files Changed (Batch 3)

- `engine/include/engine/scripting/semantic_ir.hpp` - Added Sem_HealParty documentation
- `engine/party/party.cpp` - Implemented Party::heal_all() with correct contract
- `engine/party/pokemon.cpp` - Implemented PP restoration helpers
- `engine/CMakeLists.txt` - Added party sources
- `frontends/crystal/script/semantic_legalizer.cpp` - Added SPECIAL_HEAL_PARTY (27) lowering
- `tests/scripting/runtime_test.cpp` - Added 5 Batch 3 tests
- `docs/SPECIAL_INVENTORY.md` - Updated documentation

---

## Part 8: Remaining Sem_Special IDs (107 unique)

The 107 remaining Sem_Special IDs after Batch 3 lowering. Key categories:

| Category | Count | Example IDs |
|----------|-------|-------------|
| Link protocol | ~30 | 1-15, 140, 159-162 |
| Mini-games | ~15 | 41-43 (puzzles, slots, cards) |
| Day care | 5 | 30-32, 69-70 |
| Contest | 8 | 20-24, 71 |
| Battle Tower | ~15 | 116, 124-131, 134, 136, 139 |
| Gift Pokemon | 5 | 75-76, 125, 148 |
| UI/Display | ~12 | 79-81, 85, 135 |
| Misc NPCs | ~20 | 77, 86-88, 97-99, 145-147 |

---

## Sem_Special Remains a KNOWN INTENTIONAL TEMPORARY ARCHITECTURE VIOLATION

The remaining 107 unique Sem_Special IDs (275 occurrences) carry raw Crystal Special table indices into the semantic IR, which violates the architectural principle that no Crystal identity should survive the frontend. This is intentionally temporary pending:

1. Context tracking patterns (setval → special)
2. Classification of remaining categories
3. Future batch lowering
