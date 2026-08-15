# Special ID Inventory - Batch 2 Complete

## Part 1: Authoritative Special Domain

**Source**: `references/pokecrystal/data/events/special_pointers.asm`

| Metric | Value |
|--------|-------|
| Exact entry count | 169 |
| Valid IDs | 0-168 (contiguous, no holes) |
| First valid ID | 0 (WarpToSpawnPoint) |
| Last valid ID | 168 (UnusedDummySpecial) |
| Null entries | None |

## Part 2: Batch 2 Final Results

### Pre-Batch 2 (Batch 1 Complete)
- **Remaining Sem_Special unique IDs**: 110
- **Remaining Sem_Special occurrences**: 299

### Post-Batch 2 
- **Remaining Sem_Special unique IDs**: 108
- **Remaining Sem_Special occurrences**: 283

### Lowered to Semantic Ops (Batch 2)

| Semantic Op | Occurrences | Special IDs Lowered |
|-------------|-------------|---------------------|
| Sem_WaitSound | 1 | 59 |
| Sem_PlayMapMusic | 15 | 60 |
| **Total** | **16** | **2 unique IDs** |

### Batch 2 Reduction
- Unique IDs: 110 → 108 (-2)
- Occurrences: 299 → 283 (-16)

---

## Part 2.1: Batch 2 Semantic Op Definitions

### Sem_WaitSound (Special 59)

```cpp
struct Sem_WaitSound {};  // Empty struct, 1 byte
```

| Special ID | Symbol | Count |
|------------|--------|-------|
| 59 | WaitSFX | 1 |

**Contract**:
- Suspend script progression until currently active SFX completion
- Polls SFX channels 5-8 until all are inactive
- NOT: wait for music, fixed delay, or GB channel polling abstraction

**Semantic Equivalence**:
- Same operation as opcode `waitsfx` (0x99)
- Both call the same `WaitSFX` routine in `home/audio.asm`

**Source**: `pokecrystal/home/audio.asm WaitSFX`

### Sem_PlayMapMusic (Special 60)

```cpp
struct Sem_PlayMapMusic {};  // Empty struct, 1 byte
```

| Special ID | Symbol | Count |
|------------|--------|-------|
| 60 | PlayMapMusic | 15 |

**Contract**:
- Synchronize/play the music appropriate to current world/map state
- Queries map music with special handling (surf music, bug contest music)
- Stops current music, waits 1 frame, starts new music
- Updates `wMapMusic` to the new track

**Distinct From**:
- `Sem_RestartMapMusic` (Special 61): Restarts stored `wMapMusic` without querying map

**Semantic Equivalence**:
- Same operation as opcode `playmapmusic` (0x8B) and `encountermusic` (0x8A)
- All call the same `PlayMapMusic` routine in `home/audio.asm`

**Source**: `pokecrystal/home/audio.asm PlayMapMusic`

---

## Part 3: Cumulative Results (Batches 1+2)

### Initial State
- **Unique Special IDs encountered**: 122
- **Total Special occurrences**: 400

### Post Batch 1+2
- **Remaining Sem_Special unique IDs**: 108
- **Remaining Sem_Special occurrences**: 283

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
| **Total** | **117** | **14 unique IDs** |

### Cumulative Reduction
- Unique IDs: 122 → 108 (-14)
- Occurrences: 400 → 283 (-117)

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
```

### Behavioral Preservation (Batch 2 Additions)

| Operation | Timing | Blocking | State Effects | Observable |
|-----------|--------|----------|---------------|------------|
| Sem_WaitSound | Variable | Yes | None | Script waits for SFX |
| Sem_PlayMapMusic | 1 frame gap | Yes (briefly) | wMapMusic updated | Music change |

---

## Part 6: Test Results

- **Golden Tests**: 56/56 pass
- **Runtime Tests**: 176/176 pass (4 new Batch 2 tests)
- **Linker Tests**: All pass (1362/1362 bodies linked)
- **Legality Gate Tests**: 14/14 pass

### New Batch 2 Tests

| Test | Validates |
|------|-----------|
| `batch2_special_59_waits_sfx` | Special 59 → Sem_WaitSound with correct contract |
| `batch2_special_60_plays_map_music` | Special 60 → Sem_PlayMapMusic with correct contract |
| `batch2_no_sem_special_for_59_60` | Neither produces Sem_Special |
| `batch2_59_not_60_60_not_61` | 59≠60≠61 - each maps to distinct semantic op |

---

## Part 7: Files Changed (Batch 2)

- `frontends/crystal/script/semantic_legalizer.cpp` - Added SPECIAL_WAIT_SFX (59) and SPECIAL_PLAY_MAP_MUSIC (60) lowering
- `tests/scripting/runtime_test.cpp` - Added 4 Batch 2 tests
- `docs/SPECIAL_INVENTORY.md` - Updated documentation

---

## Part 8: Remaining Sem_Special IDs (108 unique)

The 108 remaining Sem_Special IDs after Batch 2 lowering. Key categories:

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

The remaining 108 unique Sem_Special IDs (283 occurrences) carry raw Crystal Special table indices into the semantic IR, which violates the architectural principle that no Crystal identity should survive the frontend. This is intentionally temporary pending:

1. Context tracking patterns (setval → special)
2. Classification of remaining categories
3. Future batch lowering
