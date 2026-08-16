# Special ID Inventory - Batch 8 Complete

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

## Part 2: Corrected Static Inventory (Post Decoder Fix 180978f)

### Initial State (Pre-Lowering)
- **Unique Special IDs encountered**: 122
- **Total Special occurrences**: 325

Note: Prior counts (400 occurrences) were inflated due to decoder duplication bug fixed in 180978f.

---

## Part 3: Batch 8 Final Results

### Pre-Batch 8
- **Remaining Sem_Special unique IDs**: 100
- **Remaining Sem_Special occurrences**: 191

### Batch 8 Removals

| Special ID | Symbol | Occurrences | Semantic Op |
|------------|--------|-------------|-------------|
| 163 | AskRememberPassword | 1 | Sem_YesNo (reuse) |
| 166 | InitialSetDSTFlag | 1 | Sem_SetDaylightSaving{enabled=true} |
| 167 | InitialClearDSTFlag | 1 | Sem_SetDaylightSaving{enabled=false} |
| **Total** | | **3** | **3 unique IDs** |

### Post-Batch 8
- **Remaining Sem_Special unique IDs**: 97
- **Remaining Sem_Special occurrences**: 188

### Batch 8 Reduction
- Unique IDs: 100 → 97 (-3)
- Occurrences: 191 → 188 (-3)

---

## Part 4: Cumulative Lowering Summary (Batches 1-8)

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
| **Total** | **264** | **25 unique IDs** |

### Cumulative Reduction
- Unique IDs: 122 → 97 (-25)
- Occurrences: 325 → 188 (-137)

---

## Part 5: Historical Batch Summary

| Batch | Unique Removed | Occurrences Removed | Post-Batch Unique | Post-Batch Occurrences |
|-------|----------------|---------------------|-------------------|------------------------|
| Initial | - | - | 122 | 325 |
| Batch 1 | 7 | 75 | 115 | 250 |
| Batch 2 | 2 | 17 | 113 | 233 |
| Batch 3 | 1 | 7 | 112 | 226 |
| Batch 4 | 4 | 18 | 108 | 208 |
| Batch 5 | 1 | 1 | 107 | 207 |
| Batch 6 | 3 | 9 | 104 | 198 |
| Batch 7 | 4 | 7 | 100 | 191 |
| Batch 8 | 3 | 3 | 97 | 188 |

---

## Part 6: Deliberately Unclassified (need context tracking)

| Special ID | Symbol | Count | Reason |
|------------|--------|-------|--------|
| 57 | GameCornerPrizeMonCheckDex | 6 | Uses wScriptVar for species - needs setval→special context tracking |
| 95 | PlaySlowCry | 2 | Uses wScriptVar for species - needs setval→special context tracking |
| 100 | PlayCurMonCry | 7 | Uses wCurPartySpecies - needs grooming/selection context pattern |
| 152 | SetPlayerPalette | 3 | Uses wScriptVar for palette_id - needs setval→special context tracking |

These require context establishment patterns (`setval` → `special`) that are not yet implemented.

---

## Part 7: Invariants

### No Raw Crystal Identity

All lowered operations contain:
- ✅ 0 raw SpecialId fields
- ✅ 0 Crystal symbol dispatch
- ✅ 0 magic numeric modes

### Behavioral Preservation

| Operation | Timing | Blocking | State Effects |
|-----------|--------|----------|---------------|
| Sem_WaitSound | Variable | Yes | None |
| Sem_PlayMapMusic | 1 frame gap | Yes (briefly) | wMapMusic updated |
| Sem_HealParty | Instant | No | Party HP/status/PP |
| Sem_YesNo | User input | Yes | wScriptVar = 0/1 |
| Sem_SetDaylightSaving | Instant | No | DST flag |

---

## Part 8: Test Results

- **Golden Tests**: 56/56 pass
- **Runtime Tests**: 216/216 pass
- **Linker Tests**: All pass
- **Legality Gate Tests**: 14/14 pass

---

## Part 9: Remaining Sem_Special IDs (97 unique)

The 97 remaining Sem_Special IDs after Batch 8 lowering. Key categories:

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

The remaining 97 unique Sem_Special IDs (188 occurrences) carry raw Crystal Special table indices into the semantic IR, which violates the architectural principle that no Crystal identity should survive the frontend. This is intentionally temporary pending:

1. Context tracking patterns (setval → special)
2. Classification of remaining categories
3. Future batch lowering

---

## Decoder Fix Note (180978f)

Commit 180978f fixed a decoder duplication bug where CFG loops caused the same ROM instruction to be decoded multiple times. Pre-fix counts were inflated:

| Metric | Pre-Fix (buggy) | Post-Fix (correct) |
|--------|-----------------|---------------------|
| Total Special occurrences | 400 | 325 |
| Remaining Sem_Special | 233 | 188 |

All counts in this document reflect the corrected decoder behavior.
