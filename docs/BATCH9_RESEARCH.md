# Batch 9 Research - Final Reclassification

## Corpus Baseline

**Authoritative Frozen Corpus:**
- Maps: 378
- Executable bodies: 1679
- Compiler corpus = inventory corpus = legality corpus = linker corpus
- Duplicate decoded commands within body: 0
- InvalidOwnership: 0 (fixed in a4c7fca)

## Part 1: Authoritative Sem_Special Inventory

**Source**: `special_inventory.exe` against 1679-body production corpus

| Metric | Value |
|--------|-------|
| Remaining Sem_Special unique IDs | **109** |
| Remaining Sem_Special occurrences | **232** |

This confirms the provisional expanded result (109 unique / 232 occurrences).

Previous historical counts (97 / 188) were from the pre-expanded corpus (1362 bodies).

## Part 2: Batch 9 Candidate Analysis

### Candidate IDs: 40, 57, 66, 67, 95, 152

All occurrences analyzed via `batch9_analysis.exe`:

### Special 40 (MapRadio) - 2 occurrences

| Occ | Body Root | Root Type | Source Addr | setval | Literal | Commands Between | Context Valid |
|-----|-----------|-----------|-------------|--------|---------|------------------|---------------|
| 1 | 0xbc195 | std_script | 0xbc198 | 0xbc196 | 0 | 0 | YES |
| 2 | 0xbc19d | std_script | 0xbc1a0 | 0xbc19e | 4 | 0 | YES |

**Semantic Mapping**: `Sem_PlayRadio{RadioChannelId}`
- Literal 0 = Channel 0 (off/silent)
- Literal 4 = Channel 4 (specific radio station)

**Domain**: RadioChannelId (valid range: 0-N)
**Fully Removable**: YES (2/2 context-valid)

---

### Special 57 (GameCornerPrizeMonCheckDex) - 6 occurrences

| Occ | Body Root | Root Type | Owning Map | Source Addr | setval | Literal | Species | Commands Between | Context Valid |
|-----|-----------|-----------|------------|-------------|--------|---------|---------|------------------|---------------|
| 1 | 0x56d01 | object | 0xb13 (CeladonGameCorner) | 0x56da2 | 0x56da0 | 202 | Wobbuffet | 0 | YES |
| 2 | 0x56d01 | object | 0xb13 | 0x56d74 | 0x56d72 | 104 | Cubone | 0 | YES |
| 3 | 0x56d01 | object | 0xb13 | 0x56d46 | 0x56d44 | 63 | Abra | 0 | YES |
| 4 | 0x727c8 | bg_event | 0x1514 (GoldenrodGameCorner) | 0x72869 | 0x72867 | 246 | Larvitar | 0 | YES |
| 5 | 0x727c8 | bg_event | 0x1514 | 0x7283b | 0x72839 | 137 | Porygon | 0 | YES |
| 6 | 0x727c8 | bg_event | 0x1514 | 0x7280d | 0x7280b | 25 | Pikachu | 0 | YES |

**Semantic Mapping**: `Sem_RegisterNewDexEntry{SpeciesId}`
**Domain**: SpeciesId (validated: 25, 63, 104, 137, 202, 246 - all valid Gen 2 species)
**Fully Removable**: YES (6/6 context-valid)

---

### Special 66 (FindPartyMonThatSpecies) - 1 occurrence

| Occ | Body Root | Root Type | Owning Map | Source Addr | setval | Literal | Species | Commands Between | Context Valid |
|-----|-----------|-----------|------------|-------------|--------|---------|---------|------------------|---------------|
| 1 | 0x19a6ae | object | 0x902 (LakeOfRage) | 0x19a6e2 | 0x19a6e0 | 129 | Magikarp | 0 | YES |

**Semantic Mapping**: `Sem_FindPartyMon{SpeciesId, require_ot=false}`
**Domain**: SpeciesId (validated: 129 = Magikarp)
**Fully Removable**: YES (1/1 context-valid)

---

### Special 67 (FindPartyMonThatSpeciesYourTrainerID) - 4 occurrences

| Occ | Body Root | Root Type | Owning Map | Source Addr | setval | Literal | Species | Commands Between | Context Valid |
|-----|-----------|-----------|------------|-------------|--------|---------|---------|------------------|---------------|
| 1 | 0x78be0 | object | 0x1805 (CianwoodCity) | 0x78c0e | 0x78c0c | 175 | Togepi | 0 | YES |
| 2 | 0x78be0 | object | 0x1805 | 0x78c16 | 0x78c14 | 176 | Togetic | 0 | YES |
| 3 | 0x78be0 | object | 0x1805 | 0x78c24 | 0x78c22 | 175 | Togepi | 0 | YES |
| 4 | 0x78be0 | object | 0x1805 | 0x78c2c | 0x78c2a | 176 | Togetic | 0 | YES |

**Semantic Mapping**: `Sem_FindPartyMon{SpeciesId, require_ot=true}`
**Domain**: SpeciesId (validated: 175 = Togepi, 176 = Togetic)
**Fully Removable**: YES (4/4 context-valid)

---

### Special 95 (PlaySlowCry) - 2 occurrences

| Occ | Body Root | Root Type | Owning Map | Source Addr | setval | Literal | Species | Commands Between | Context Valid |
|-----|-----------|-----------|------------|-------------|--------|---------|---------|------------------|---------------|
| 1 | 0x60c3a | object | 0x32f (OlivineLighthouse) | 0x60c47 | 0x60c45 | 181 | Ampharos | 0 | YES |
| 2 | 0x9ccaa | object | 0x10a (Route10) | 0x9ccb6 | 0x9ccb4 | 241 | Miltank | 0 | YES |

**Semantic Mapping**: `Sem_PlayCry{SpeciesId, variant=Slow}`
**Domain**: SpeciesId (validated: 181 = Ampharos, 241 = Miltank)
**Fully Removable**: YES (2/2 context-valid)

---

### Special 152 (SetPlayerPalette) - 6 occurrences (EXPANDED FROM 3)

| Occ | Body Root | Root Type | Owning Map | Source Addr | setval | Literal | Palette | Commands Between | Context Valid |
|-----|-----------|-----------|------------|-------------|--------|---------|---------|------------------|---------------|
| 1 | 0x19289d | object | 0x1401 (Pokecenter2F) | 0x192b34 | 0x192b32 | 0x80 | 0 (normal) | 0 | YES |
| 2 | 0x192952 | object | 0x1401 | 0x192b34 | 0x192b32 | 0x80 | 0 (normal) | 0 | YES |
| 3 | 0x192a2d | object | 0x1401 | 0x192c2f | 0x192c2d | 0x80 | 0 (normal) | 0 | YES |
| 4 | 0x192ab6 | **deferred** | 0x1401 | 0x192b77 | 0x192b75 | 0x90 | 1 (disguise) | 0 | YES |
| 5 | 0x192add | **deferred** | 0x1401 | 0x192bb1 | 0x192baf | 0x90 | 1 (disguise) | 0 | YES |
| 6 | 0x192c4e | **deferred** | 0x1401 | 0x192c7a | 0x192c78 | 0x90 | 1 (disguise) | 0 | YES |

**Semantic Mapping**: `Sem_SetPlayerPalette{PaletteId}`

**Crystal Palette Encoding**: Shifted nibble format
- `0x80` → semantic palette 0 (normal player colors)
- `0x90` → semantic palette 1 (Team Rocket disguise)

**Normalization Required**:
```
Crystal literal 0x80 → PaletteId(0)
Crystal literal 0x90 → PaletteId(1)
```

**All 5 known source addresses now represented**:
| Source Address | Body Roots | Discovery |
|----------------|------------|-----------|
| 0x192b34 | 0x19289d, 0x192952 | Initial (object) |
| 0x192b77 | 0x192ab6 | Fixed-point (deferred) |
| 0x192bb1 | 0x192add | Fixed-point (deferred) |
| 0x192c2f | 0x192a2d | Initial (object) |
| 0x192c7a | 0x192c4e | Fixed-point (deferred) |

**Domain**: PaletteId (validated: 0, 1)
**Fully Removable**: YES (6/6 context-valid)

---

## Part 3: Approved Semantic Mappings (Reconfirmed)

| Special ID | Symbol | Semantic Op | Domain |
|------------|--------|-------------|--------|
| 40 | MapRadio | `Sem_PlayRadio{RadioChannelId}` | RadioChannelId |
| 57 | GameCornerPrizeMonCheckDex | `Sem_RegisterNewDexEntry{SpeciesId}` | SpeciesId |
| 66 | FindPartyMonThatSpecies | `Sem_FindPartyMon{SpeciesId, require_ot=false}` | SpeciesId |
| 67 | FindPartyMonThatSpeciesYourTrainerID | `Sem_FindPartyMon{SpeciesId, require_ot=true}` | SpeciesId |
| 95 | PlaySlowCry | `Sem_PlayCry{SpeciesId, variant=Slow}` | SpeciesId |
| 152 | SetPlayerPalette | `Sem_SetPlayerPalette{PaletteId}` | PaletteId |

No redesign required - all mappings remain valid against expanded corpus.

## Part 4: Typed-Domain Validation

### SpeciesId Validation (IDs 57, 66, 67, 95)

All literals must be valid Gen 2 species (1-251):

| Literal | Species | Valid |
|---------|---------|-------|
| 25 | Pikachu | ✓ |
| 63 | Abra | ✓ |
| 104 | Cubone | ✓ |
| 129 | Magikarp | ✓ |
| 137 | Porygon | ✓ |
| 175 | Togepi | ✓ |
| 176 | Togetic | ✓ |
| 181 | Ampharos | ✓ |
| 202 | Wobbuffet | ✓ |
| 241 | Miltank | ✓ |
| 246 | Larvitar | ✓ |

**All species literals validated** ✓

### RadioChannelId Validation (ID 40)

| Literal | Channel | Valid |
|---------|---------|-------|
| 0 | Off/Silent | ✓ |
| 4 | Radio Station | ✓ |

**All radio literals validated** ✓

### PaletteId Validation (ID 152)

Crystal encoding uses shifted nibble format:
- `value >> 4` extracts palette index
- `0x80 >> 4 = 8 - 8 = 0` (standard palette index calculation)
- Actually: `(value & 0xF0) >> 4 - 8`

**Normalization rule**:
```cpp
uint8_t crystal_to_semantic_palette(uint8_t crystal_literal) {
    return (crystal_literal >> 4) - 8;  // 0x80→0, 0x90→1
}
```

| Crystal Literal | Calculation | Semantic PaletteId |
|-----------------|-------------|-------------------|
| 0x80 | (0x80 >> 4) - 8 = 8 - 8 | 0 |
| 0x90 | (0x90 >> 4) - 8 = 9 - 8 | 1 |

**All palette literals validated** ✓

## Part 5: Block-Local Context Architecture

### Implementation Shape

```cpp
struct BlockLoweringContext {
    std::optional<uint8_t> known_script_var;  // Known literal value, if established
    
    void on_setval(uint8_t value) {
        known_script_var = value;
    }
    
    void on_special(/* ... */) {
        // Consume context if needed
    }
    
    void invalidate() {
        known_script_var = std::nullopt;
    }
};
```

### Context Establishment

Only `Cmd_Setval` establishes known ScriptVar value.

### Invalidation Rules (Conservative)

Commands that invalidate ScriptVar context:
- `Cmd_Special` (may modify wScriptVar)
- `Cmd_Callasm` (native code may modify)
- `Cmd_Readmem` (reads into wScriptVar)
- `Cmd_Random` (writes random value)
- `Cmd_Addval` (modifies wScriptVar)
- `Cmd_Readvar` (reads variable into wScriptVar)
- `Cmd_Checkscene` (reads scene into wScriptVar)
- `Cmd_Checkmapscene` (reads scene into wScriptVar)

Commands that preserve ScriptVar context:
- `Cmd_Sjump`, `Cmd_Scall`, `Cmd_End` (control flow)
- `Cmd_Faceplayer`, `Cmd_Opentext`, `Cmd_Closetext`, `Cmd_Waitbutton`
- `Cmd_Writetext`, `Cmd_Jumptext`
- `Cmd_Playsound`, `Cmd_Playmusic`, `Cmd_Cry`, `Cmd_Waitsfx`, `Cmd_Pause`
- `Cmd_Loadvar` (writes to VAR table, not ScriptVar)
- `Cmd_Showemote`

### Context Boundaries

Context **must not propagate across**:
- Block entry (new basic block starts fresh)
- Branch target
- Merge point
- Loop re-entry
- Call boundary
- Unknown side effects

### Batch 9 Pattern Confirmation

All 21 Batch 9 occurrences have:
- `setval` immediately preceding `special` (0 commands between)
- Same basic block
- No intervening commands

This is the simplest case: **direct setval→special pattern**.

---

## Part 6: Special 152 Verification

### Source Evidence

All 5 physical source SetPlayerPalette sites are now represented:

| Source Address | Compiled Bodies | Root Type | Owning Map |
|----------------|-----------------|-----------|------------|
| 0x192b34 | 0x19289d, 0x192952 | object | Pokecenter2F |
| 0x192b77 | 0x192ab6 | deferred | Pokecenter2F |
| 0x192bb1 | 0x192add | deferred | Pokecenter2F |
| 0x192c2f | 0x192a2d | object | Pokecenter2F |
| 0x192c7a | 0x192c4e | deferred | Pokecenter2F |

### Occurrence Count

- **Previous count**: 3 (pre-expansion corpus, object scripts only)
- **Current count**: 6 (expanded corpus with deferred discovery)

### Context Validity

All 6 occurrences have valid block-local context:
- setval immediately precedes special
- 0 intervening commands
- All within same basic block

**152 is fully removable** ✓

---

## Part 7: Newly Exposed Candidate Occurrences

The expanded corpus (1679 vs 1362 bodies) increased counts for:

| ID | Symbol | Old Count | New Count | Increase |
|----|--------|-----------|-----------|----------|
| 40 | MapRadio | 2 | 2 | +0 |
| 57 | GameCornerPrizeMonCheckDex | 6 | 6 | +0 |
| 66 | FindPartyMonThatSpecies | 1 | 1 | +0 |
| 67 | FindPartyMonThatSpeciesYourTrainerID | 4 | 4 | +0 |
| 95 | PlaySlowCry | 2 | 2 | +0 |
| 152 | SetPlayerPalette | 3 | **6** | **+3** |

Only Special 152 gained occurrences (from deferred script discovery).

**All newly exposed occurrences have valid context** ✓

---

## Part 8: Final Batch 9 Recommendation

### Authoritative Before Count

- **Remaining Sem_Special unique IDs**: 109
- **Remaining Sem_Special occurrences**: 232

### Per-Candidate Summary

| ID | Symbol | Total Occ | Context-Valid | Fully Removable |
|----|--------|-----------|---------------|-----------------|
| 40 | MapRadio | 2 | 2 | **YES** |
| 57 | GameCornerPrizeMonCheckDex | 6 | 6 | **YES** |
| 66 | FindPartyMonThatSpecies | 1 | 1 | **YES** |
| 67 | FindPartyMonThatSpeciesYourTrainerID | 4 | 4 | **YES** |
| 95 | PlaySlowCry | 2 | 2 | **YES** |
| 152 | SetPlayerPalette | 6 | 6 | **YES** |

### Batch 9 Reduction

- **IDs removable**: 6
- **Occurrences removable**: 21

### Projected After Batch 9

| Metric | Before | Removed | After |
|--------|--------|---------|-------|
| Unique IDs | 109 | -6 | **103** |
| Occurrences | 232 | -21 | **211** |

---

## Implementation Requirements (For Future Reference)

### New SemanticOps Required

| Op | Fields | Notes |
|----|--------|-------|
| `Sem_PlayRadio` | `RadioChannelId channel` | New op |
| `Sem_RegisterNewDexEntry` | `SpeciesId species` | New op |
| `Sem_FindPartyMon` | `SpeciesId species, bool require_ot` | New op |

### Existing SemanticOps Reused

| Op | Fields | Notes |
|----|--------|-------|
| `Sem_PlayCry` | `SpeciesId species` | Extend with `CryVariant variant` field |
| `Sem_SetPlayerPalette` | `uint8_t palette_id` | Already exists |

### BlockLoweringContext Fields

```cpp
struct BlockLoweringContext {
    std::optional<uint8_t> known_script_var;
};
```

### Invalidation Rules

Reset `known_script_var` on:
- Block entry
- Any command that may modify wScriptVar (conservative list above)

### Adversarial Tests Required

1. **Context validity tests**:
   - Valid setval→special pattern recognized
   - Missing setval rejected (no context)
   - Intervening clobber command rejected
   - Cross-block setval rejected

2. **Domain validation tests**:
   - Invalid SpeciesId (0, 252+) rejected
   - Invalid RadioChannelId rejected
   - Invalid PaletteId rejected (raw Crystal encoding without normalization)

3. **Normalization tests**:
   - Crystal palette 0x80 → semantic 0
   - Crystal palette 0x90 → semantic 1
   - Raw Crystal value NOT passed through

4. **Production corpus tests**:
   - All 21 occurrences lowered correctly
   - No Sem_Special for IDs 40, 57, 66, 67, 95, 152

---

## Sem_Special Architecture Note

**Sem_Special remains a KNOWN INTENTIONAL TEMPORARY ARCHITECTURE VIOLATION.**

After Batch 9:
- 103 unique Sem_Special IDs remaining
- 211 Sem_Special occurrences remaining

These carry raw Crystal Special table indices into the semantic IR.
Future batches will continue to classify and remove them.

---

## Research Complete

**No implementation in this commit.**
**No Stage 7 work.**
**Stop after this recommendation.**
