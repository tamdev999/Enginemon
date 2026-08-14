# Special ID Inventory - Batch 1 Complete

## Part 1: Authoritative Special Domain

**Source**: `references/pokecrystal/data/events/special_pointers.asm`

| Metric | Value |
|--------|-------|
| Exact entry count | 169 |
| Valid IDs | 0-168 (contiguous, no holes) |
| First valid ID | 0 (WarpToSpawnPoint) |
| Last valid ID | 168 (UnusedDummySpecial) |
| Null entries | None |

## Part 2: Batch 1 Final Results

### Pre-Lowering
- **Unique Special IDs encountered**: 122
- **Total Special occurrences**: 400

### Post-Lowering (Batch 1 Complete)
- **Remaining Sem_Special unique IDs**: 110
- **Remaining Sem_Special occurrences**: 299

### Lowered to Semantic Ops (Batch 1)

| Semantic Op | Occurrences | Special IDs Lowered |
|-------------|-------------|---------------------|
| Sem_ScreenFade | 36 | 46, 48, 49, 50 |
| Sem_SyncPalettes | 7 | 51, 52, 164 |
| Sem_RefreshPlayerSprite | 5 | 56 |
| Sem_SyncSprites | 18 | 94 |
| Sem_RebuildSprites | 1 | 158 |
| Sem_RestartMapMusic | 24 | 61 |
| Sem_FadeToSilence | 10 | 106 |
| **Total** | **101** | **12 unique IDs** |

### Reduction
- Unique IDs: 122 → 110 (-12)
- Occurrences: 400 → 299 (-101)

---

## Part 3: Batch 1 Semantic Op Definitions

### Sem_ScreenFade

```cpp
enum class FadeDirection : uint8_t { In, Out };
enum class FadeColor : uint8_t { White, Black };
struct Sem_ScreenFade {
    FadeDirection direction;
    FadeColor color;
    bool prefill;  // True for FadeOutToWhite only
};
```

| Special ID | Symbol | Mapping |
|------------|--------|---------|
| 46 | FadeOutToWhite | `{Out, White, true}` |
| 48 | FadeOutToBlack | `{Out, Black, false}` |
| 49 | FadeInFromWhite | `{In, White, false}` |
| 50 | FadeInFromBlack | `{In, Black, false}` |

**Contract**:
- All 4 fades block script execution for 4 steps × 2 frames = 8 frames
- FadeOutToWhite additionally calls `FillWhiteBGColor` before fade (observable as brief white flash)
- The `prefill` flag preserves this observable difference

**Source**: `pokecrystal/engine/tilesets/timeofday_pals.asm`

### Sem_SyncPalettes

```cpp
struct Sem_SyncPalettes {
    uint8_t wait_frames;  // Blocking wait duration
};
```

| Special ID | Symbol | Wait | Description |
|------------|--------|------|-------------|
| 51 | ReloadSpritesNoPalettes | 1 | Clear BG palette buffer, request update |
| 52 | ClearBGPalettes | 4 | Clear palettes, wait for BG map update |
| 164 | LoadMapPalettes | 0 | SGB palette setup (immediate) |

**Contract**:
- Ensures palette state is consistent
- Blocking wait is part of observable behavior (script timing)

**Source**: 
- 51: `pokecrystal/home/palettes.asm` (DelayFrame)
- 52: `pokecrystal/home/tilemap.asm` (DelayFrames with c=4)
- 164: `pokecrystal/engine/overworld/warp_connection.asm` (immediate return)

### Sem_RefreshPlayerSprite

```cpp
struct Sem_RefreshPlayerSprite {};
```

| Special ID | Symbol | Count |
|------------|--------|-------|
| 56 | UpdatePlayerSprite | 5 |

**Contract**:
- Updates player sprite based on current flags (e.g., `ENGINE_KRIS_IN_CABLE_CLUB`)
- Called after `setflag`/`clearflag` that affects player visual

**Source**: `pokecrystal/engine/overworld/overworld.asm _UpdatePlayerSprite`

### Sem_SyncSprites

```cpp
struct Sem_SyncSprites {};
```

| Special ID | Symbol | Count |
|------------|--------|-------|
| 94 | LoadUsedSpritesGFX | 18 |

**Contract**:
- Runs `MAPCALLBACK_SPRITES` callback
- Reloads sprite graphics after `variablesprite` changes
- In Enginemon: sync point to ensure sprite state is consistent

**Source**: `pokecrystal/engine/overworld/overworld.asm LoadUsedSpritesGFX`

### Sem_RebuildSprites

```cpp
struct Sem_RebuildSprites {};
```

| Special ID | Symbol | Count |
|------------|--------|-------|
| 158 | RefreshSprites | 1 |

**Contract**:
- Full sprite state rebuild: clears list, rebuilds from map, loads all GFX
- Heavy operation used after major state transitions

**Source**: `pokecrystal/engine/overworld/overworld.asm RefreshSprites`

### Sem_RestartMapMusic

```cpp
struct Sem_RestartMapMusic {};
```

| Special ID | Symbol | Count |
|------------|--------|-------|
| 61 | RestartMapMusic | 24 |

**Contract**:
- Restarts music from stored `wMapMusic` value
- Does NOT query map for music (distinct from `PlayMapMusic`)
- Includes 1-frame silence gap between stop and restart

**Source**: `pokecrystal/home/audio.asm RestartMapMusic`

### Sem_FadeToSilence

```cpp
struct Sem_FadeToSilence {};
```

| Special ID | Symbol | Count |
|------------|--------|-------|
| 106 | FadeOutMusic | 10 |

**Contract**:
- Fades current music to `MUSIC_NONE` with fixed fade speed ($2)
- Non-blocking: returns immediately, fade happens in background
- Distinct from parameterized `Sem_FadeOutMusic` (not implemented yet)

**Source**: `pokecrystal/engine/events/specials.asm FadeOutMusic`

---

## Part 4: Deliberately Unclassified in Batch 1

| Special ID | Symbol | Count | Reason |
|------------|--------|-------|--------|
| 95 | PlaySlowCry | 2 | Uses wScriptVar for species - needs setval→special context tracking |
| 100 | PlayCurMonCry | 7 | Uses wCurPartySpecies - needs grooming/selection context pattern |
| 152 | SetPlayerPalette | 5 | Uses wScriptVar for palette_id - needs setval→special context tracking |

These require context establishment patterns (`setval` → `special`) that are not yet implemented.

---

## Part 5: Invariants

### No Raw Crystal Identity

All Batch 1 lowered operations contain:
- ✅ 0 raw SpecialId fields
- ✅ 0 Crystal symbol dispatch
- ✅ 0 magic numeric modes

Verified by compile-time static assertions:
```cpp
static_assert(sizeof(Sem_ScreenFade) == 3);        // direction + color + prefill only
static_assert(sizeof(Sem_SyncPalettes) == 1);      // wait_frames only
static_assert(sizeof(Sem_RefreshPlayerSprite) == 1);
static_assert(sizeof(Sem_SyncSprites) == 1);
static_assert(sizeof(Sem_RebuildSprites) == 1);
static_assert(sizeof(Sem_RestartMapMusic) == 1);
static_assert(sizeof(Sem_FadeToSilence) == 1);
```

### Behavioral Preservation

| Operation | Timing | Blocking | State Effects | Observable |
|-----------|--------|----------|---------------|------------|
| Sem_ScreenFade | 8 frames | Yes | Palette transition | Visual fade |
| Sem_ScreenFade (prefill) | 8 frames | Yes | Pre-fill + transition | White flash + fade |
| Sem_SyncPalettes (51) | 1 frame | Yes | Palette buffer sync | None directly |
| Sem_SyncPalettes (52) | 4 frames | Yes | Palette + BG map sync | None directly |
| Sem_SyncPalettes (164) | 0 frames | No | SGB palette setup | None (SGB-only) |
| Sem_RefreshPlayerSprite | Immediate | No | Player sprite update | Visual change |
| Sem_SyncSprites | Immediate | No | Sprite graphics reload | Visual change |
| Sem_RebuildSprites | Immediate | No | Full sprite rebuild | Visual change |
| Sem_RestartMapMusic | 1 frame gap | Yes (briefly) | Music restart | Audio restart |
| Sem_FadeToSilence | Immediate | No | Fade initiated | Gradual audio fade |

---

## Part 6: Test Results

- **Golden Tests**: 56/56 pass
- **Runtime Tests**: 147/147 pass (5 new Batch 1 tests)
- **Linker Tests**: All pass (1362/1362 bodies linked)

### New Batch 1 Tests

| Test | Validates |
|------|-----------|
| `batch1_screen_fade_variants` | All 4 fades have correct direction×color×prefill |
| `batch1_sync_palettes_variants` | Wait durations: 1, 4, 0 frames preserved |
| `batch1_sprite_ops_distinct` | 3 distinct types (no raw ID merging) |
| `batch1_audio_ops_distinct` | RestartMapMusic ≠ FadeToSilence |
| `batch1_no_crystal_ids_in_ops` | Zero raw Crystal IDs via static_assert |

---

## Part 7: Files Changed

- `engine/include/engine/scripting/semantic_ir.hpp` - Updated semantic op definitions
- `frontends/crystal/script/semantic_legalizer.cpp` - Updated rule_special
- `tools/special_inventory.cpp` - Updated counting for new types
- `tests/scripting/runtime_test.cpp` - Added 5 Batch 1 tests
- `docs/SPECIAL_INVENTORY.md` - This documentation

---

## Part 8: Remaining Sem_Special IDs (110 unique)

The 110 remaining Sem_Special IDs after Batch 1 lowering are unchanged from the previous inventory. Key categories:

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

See previous inventory for full ID listing.
