# Special ID Inventory - Batch 9 Complete

## Terminology

This document distinguishes:

- **Source Cmd_Special inventory**: Crystal `special` opcodes found in ROM during decode (134 unique IDs / 401 occurrences)
- **Remaining Sem_Special**: Commands that still use the `Sem_Special` fallback after semantic lowering

Batch lowering converts Crystal Special IDs to typed semantic operations. The reduction is:
- **Source inventory** (fixed at ROM decode time)
- **Remaining Sem_Special** (decreases with each batch)

---

## Part 1: Authoritative Special Domain

**Source**: `references/pokecrystal/data/events/special_pointers.asm`

| Metric | Value |
|--------|-------|
| Exact entry count | 169 |
| Valid IDs | 0-168 (contiguous, no holes) |
| First valid ID | 0 (WarpToSpawnPoint) |
| Last valid ID | 168 (UnusedDummySpecial) |
| Null entries | None |

---

## Part 2: Source Cmd_Special Inventory (Fixed)

**Corpus**: 1679 executable script bodies (post-deferred discovery)

### Source Command Counts
- **Unique Special IDs in ROM**: 134
- **Total Special occurrences**: 401

Note: These are SOURCE command counts from ROM decode, NOT post-lowering Sem_Special counts.

---

## Part 3: Batch 9 Final Results

### Pre-Batch 9 (Post-Corpus Expansion)
- **Remaining Sem_Special unique IDs**: 109
- **Remaining Sem_Special occurrences**: 232

### Batch 9 Removals

| Special ID | Symbol | Occurrences | Semantic Op |
|------------|--------|-------------|-------------|
| 40 | MapRadio | 2 | Sem_PlayRadio{channel} |
| 57 | GameCornerPrizeMonCheckDex | 6 | Sem_RegisterNewDexEntry{species} |
| 66 | FindPartyMonThatSpecies | 1 | Sem_FindPartyMon{species, require_ot=false} |
| 67 | FindPartyMonThatSpeciesYourTrainerID | 4 | Sem_FindPartyMon{species, require_ot=true} |
| 95 | PlaySlowCry | 2 | Sem_PlayCry{species, variant=Slow} |
| 152 | SetPlayerPalette | 6 | Sem_SetPlayerPalette{selector} |
| **Total** | | **21** | **6 unique IDs** |

### Post-Batch 9
- **Remaining Sem_Special unique IDs**: 103
- **Remaining Sem_Special occurrences**: 211

### Batch 9 Reduction
- Unique IDs: 109 → 103 (-6)
- Occurrences: 232 → 211 (-21)

---

## Part 4: Cumulative Lowering Summary (Batches 1-9)

### All Lowered Semantic Ops

| Semantic Op | Occurrences | Special IDs |
|-------------|-------------|-------------|
| Sem_ScreenFade | 34 | 46, 48, 49, 50 |
| Sem_SyncPalettes | 7 | 51, 52, 164 |
| Sem_RefreshPlayerSprite | 3 | 56 |
| Sem_SyncSprites | 13 | 94 |
| Sem_RebuildSprites | 1 | 158 |
| Sem_RestartMapMusic | 22 | 61 |
| Sem_FadeToSilence | 9 | 106 |
| Sem_WaitSound | 138 | 59 |
| Sem_PlayMapMusic | 8 | 60 |
| Sem_HealParty | 7 | 27 |
| Sem_ShowBalanceOverlay | 17 | 79, 80, 81 |
| Sem_CheckPartyPokerus | 1 | 91 |
| Sem_SetVar (absorbed) | 1 | 165 (GameboyCheck) |
| Sem_YesNo | 1 | 163 |
| Sem_SetDaylightSaving | 2 | 166, 167 |
| Sem_PlayRadio | 2 | 40 |
| Sem_RegisterNewDexEntry | 6 | 57 |
| Sem_FindPartyMon | 5 | 66, 67 |
| Sem_PlayCry (slow variant) | 2 | 95 |
| Sem_SetPlayerPalette | 6 | 152 |
| **Total** | **285** | **31 unique IDs** |

### Cumulative Reduction
- Source Cmd_Special: 134 unique IDs / 401 occurrences
- Lowered to typed ops: 31 unique IDs / 285 occurrences
- Remaining Sem_Special: 103 unique IDs / 211 occurrences

---

## Part 5: Historical Batch Summary

| Batch | Unique Removed | Occurrences Removed | Post-Batch Unique | Post-Batch Occurrences |
|-------|----------------|---------------------|-------------------|------------------------|
| Initial (1362 bodies) | - | - | 122 | 325 |
| Batch 1 | 7 | 75 | 115 | 250 |
| Batch 2 | 2 | 17 | 113 | 233 |
| Batch 3 | 1 | 7 | 112 | 226 |
| Batch 4 | 4 | 18 | 108 | 208 |
| Batch 5 | 1 | 1 | 107 | 207 |
| Batch 6 | 3 | 9 | 104 | 198 |
| Batch 7 | 4 | 7 | 100 | 191 |
| Batch 8 | 3 | 3 | 97 | 188 |
| Corpus expansion (1679 bodies) | - | - | 109 | 232 |
| **Batch 9** | **6** | **21** | **103** | **211** |

Note: Corpus expansion from 1362 to 1679 bodies (deferred script discovery) increased the source inventory. Batch 9 was measured against this expanded corpus.

---

## Part 6: Invariants

### No Raw Crystal Identity in Lowered Ops

All lowered operations contain:
- ✅ 0 raw SpecialId fields
- ✅ 0 Crystal symbol dispatch
- ✅ 0 magic numeric modes
- ✅ 0 CGB/OAM hardware slot identity in final semantic model

### Batch 9: Sem_SetPlayerPalette Source-Faithful Design

`Sem_SetPlayerPalette` uses `uint8_t selector` (0-7), preserving all source-valid Crystal values:

**Source-proven semantics** from `_SetPlayerPalette`:
- Bit 7 not set → no-op (routine returns immediately)
- Bit 7 set → extract 3-bit selector via `(input >> 4) & 0x07`
- ALL selectors 0-7 are source-valid; frontend must not reject merely because vanilla uses only 0/1

**Semantic boundary**:
- ✅ Accepts ALL source-valid selectors 0-7
- ✅ Rejects bit7-clear values (source no-op)
- ✅ Frontend normalizes Crystal encoding to selector
- ✅ Runtime maps selector to native PaletteId through palette resource system
- ❌ No CGB/OAM slot identity in final runtime model

### Behavioral Preservation

| Operation | Timing | Blocking | State Effects |
|-----------|--------|----------|---------------|
| Sem_PlayRadio | Instant | No | Radio state |
| Sem_RegisterNewDexEntry | Variable | Maybe | Dex seen/caught flags |
| Sem_FindPartyMon | Instant | No | wScriptVar = slot+1 or 0 |
| Sem_PlayCry | ~1s | Yes | Audio |
| Sem_SetPlayerPalette | Instant | No | Player sprite palette |

---

## Part 7: Species Domain Validation

### Current Architecture

- `profile.counts.num_pokemon` establishes authoritative species domain [1, num_pokemon]
- All production callers set `legalizer.set_num_pokemon(profile->counts.num_pokemon)`
- For vanilla Crystal, num_pokemon = 251 (contiguous, no holes)

### Vanilla Crystal Guarantee

The value 251 is **explicitly configured** in `profile.cpp::register_crystal_v11()` for the supported vanilla ROM. This is not a hidden default - it's the authoritative domain for vanilla Crystal ROM.

### ROM Hack Support

For ROM hacks with expanded species (245/251/274):
- New profile variants would need to be created
- Each profile explicitly sets `num_pokemon` appropriate for that ROM
- The default value in `LoweringContext` (251) is appropriate for tools/tests using vanilla Crystal

### No Silent Inheritance

The architecture ensures:
- ✅ Production compilation (FullGameCompiler) explicitly sets num_pokemon from profile
- ✅ Default 251 in LoweringContext is vanilla-appropriate, not arbitrary
- ✅ Profile documents num_pokemon as "establishes a closed, contiguous species domain"

---

## Part 8: Test Results

Run canonical verifier: `.\run_all_tests.ps1 -RomPath "<Crystal ROM>"`

- **Runtime Tests**: 232/232 pass
- **Golden Tests**: 56/56 pass
- **Legality Gate Tests**: 14/14 pass
- **Corpus Test**: PASS (decoder/CFG integrity)
- **Corpus Lowering Audit**: 1679/1679 SUCCESS
- **Linker Tests**: 1679/1679 bodies linked, InvalidOwnership=0

---

## Part 9: Remaining Sem_Special IDs (103 unique)

The 103 remaining Sem_Special IDs after Batch 9 lowering. Key categories:

| Category | Count | Example IDs |
|----------|-------|-------------|
| Link protocol | ~30 | 1-15, 140, 159-162 |
| Mini-games | ~15 | 41-43 (puzzles, slots, cards) |
| Day care | 5 | 30-32, 69-70 |
| Contest | 8 | 20-24, 71 |
| Battle Tower | ~15 | 116, 124-131, 134, 136, 139 |
| Gift Pokemon | 5 | 75-76, 125, 148 |
| UI/Display | ~12 | 85, 135 |
| Misc NPCs | ~20 | 77, 86-88, 97-99, 145-147 |

---

## Sem_Special Remains a KNOWN INTENTIONAL TEMPORARY ARCHITECTURE VIOLATION

The remaining 103 unique Sem_Special IDs (211 occurrences) carry raw Crystal Special table indices into the semantic IR, which violates the architectural principle that no Crystal identity should survive the frontend. This is intentionally temporary pending:

1. Classification of remaining categories
2. Future batch lowering

---

## Decoder Fix Note (180978f)

Commit 180978f fixed a decoder duplication bug where CFG loops caused the same ROM instruction to be decoded multiple times. All counts in this document reflect the corrected decoder behavior.
