# ROM-Only Species Domain Discovery — Architecture Design

## Goal

Given ONLY Crystal-compatible ROM bytes, derive the authoritative species domain automatically.

**Hard constraints:**
- No .sym files
- No pokecrystal source at runtime
- No manually selected hack profile
- No hardcoded 251
- No per-hack `num_pokemon` override
- No heuristic that silently falls back to vanilla

---

## 1. Supported ROM Dialects

| Dialect | Support | Notes |
|---------|---------|-------|
| Sequential-index Crystal layout | ✓ Supported | `record[i].DEX_NO == i + 1` — vanilla and most hacks |
| Reordered/remapped species IDs | ✗ Not supported | Would require index-mapping discovery |
| Changed BaseData record format | ✗ Unsupported dialect | Different field sizes/offsets |

The detector assumes vanilla Crystal BaseData layout (32-byte records, DEX_NO at byte 0, sequential IDs). Hacks that preserve this format but relocate tables are supported. Hacks that change the fundamental record structure are not.

---

## 2. SpeciesDomain Representation

The output is NOT simply `1..N`. It maps source species IDs to their BaseData record locations:

```cpp
struct SpeciesRecord {
    uint16_t source_id;      // DEX_NO from ROM (1-based)
    uint32_t record_offset;  // Flat ROM offset of this record
};

struct SpeciesDomain {
    uint32_t base_data_table_offset;  // Start of BaseData table
    std::vector<SpeciesRecord> species;  // All discovered species
    
    // For vanilla Crystal, species[i].source_id == i + 1
    // but the structure allows non-contiguous mappings if needed
    
    size_t count() const { return species.size(); }
    
    bool is_valid(uint16_t id) const {
        return std::any_of(species.begin(), species.end(),
            [id](const auto& s) { return s.source_id == id; });
    }
    
    std::optional<uint32_t> record_offset(uint16_t id) const;
};
```

---

## 3. BaseData Candidate Validator

### Record Structure (32 bytes)

```
Offset  Size  Field           Validation
0       1     DEX_NO          Sequential check (see below)
1-6     6     Stats           All non-zero (1-255)
7       1     TYPE_1          Valid type (0-9, 19, 20-27)
8       1     TYPE_2          Valid type (0-9, 19, 20-27)
9       1     CATCH_RATE      Any (0-255 valid)
10      1     BASE_EXP        Any (0-255 valid)
11-12   2     ITEMS           Any (item IDs)
13      1     GENDER          Any (gender ratio encoding)
14      1     (unknown)       Any
15      1     EGG_STEPS       Any (1-120 typical, but no hard limit)
16      1     (unknown)       Any
17      1     PIC_SIZE        Valid dimensions (0x00-0x77 typical)
18-21   4     (unused ptrs)   Any
22      1     GROWTH_RATE     0-5 only
23      1     EGG_GROUPS      Valid nibbles (each 0-15)
24-31   8     TM/HM flags     Any
```

### Validation Invariants

**Binary-format guarantees** (must hold for any Crystal-compatible ROM):
- Record size is exactly 32 bytes
- DEX_NO is at byte 0
- GROWTH_RATE at byte 22 is 0-5
- TYPE fields use Crystal type encoding

**Vanilla/source-layout conventions** (assumed for detection):
- DEX_NO values are sequential: `record[i].DEX_NO == i + 1`
- First record is Bulbasaur (DEX_NO = 1)
- No gaps in species numbering

**Heuristic validation signals** (increase confidence but not required):
- Stats are "reasonable" (5-255, no 0s, no identical 6-tuple)
- PIC_SIZE encodes valid dimensions
- EGG_GROUPS nibbles are in valid range (1-15)

### Full-ROM Scan Algorithm

```
candidates = []

for offset in range(0, rom_size - 32 * MIN_SEED):
    if rom[offset] != 1:  # Quick filter: first DEX_NO must be 1
        continue
    
    run_length = count_valid_sequential_records(rom, offset)
    
    if run_length >= MIN_SEED:
        candidates.append((offset, run_length))

if len(candidates) == 0:
    FAIL("No BaseData candidates found")

if len(candidates) > 1:
    # Multiple candidates remain AMBIGUOUS
    # Do NOT auto-select longest run
    # Wait for independent secondary validation
```

**Critical:** No bank window restriction. Scan from byte 0 to `rom_size - 32 * MIN_SEED`.

---

## 4. Secondary Structural Proof Audit

### 4.1 EvosAttacksPointers — CANNOT Discover Independently

**Structure:** 2-byte pointers per species, pointing to variable-length evolution/move blobs.

**Evolution/Move Blob Grammar** (from `evos_attacks.asm`):

```
EvosAttacks := EvolutionList MoveList
EvolutionList := (EvolutionRecord)* 0x00
MoveList := (MoveRecord)* 0x00

EvolutionRecord := 
  | EVOLVE_LEVEL (1) level (1) species (1)      ; 3 bytes
  | EVOLVE_ITEM (2) item (1) species (1)        ; 3 bytes
  | EVOLVE_TRADE (3) held_item (1) species (1)  ; 3 bytes (held=-1 for none)
  | EVOLVE_HAPPINESS (4) trigger (1) species (1); 3 bytes (TR_ANYTIME=1, TR_MORNDAY=2, TR_NITE=3)
  | EVOLVE_STAT (5) level (1) atk_def (1) species (1) ; 4 bytes

MoveRecord := level (1) move (1)  ; 2 bytes
```

**Why it CANNOT be discovered independently:**

1. **No table start signature:** The pointer table is just a sequence of 2-byte values. There's no header, magic number, or self-identifying structure.

2. **Pointer validation is weak:** A "valid banked pointer" (0x4000-0x7FFF) can match many false positives in ROM code/data.

3. **Target validation requires N:** To validate that a candidate pointer table has N entries, you must:
   - Know N (the species count)
   - Verify N consecutive pointers point to valid evo/attack blobs
   - This creates a circular dependency: you need N to validate, but validation is supposed to discover N

4. **Grammar validation is complex:** Each target blob has variable length depending on:
   - Number of evolutions (0-N, each 3-4 bytes)
   - Evolution types (EVOLVE_STAT is 4 bytes, others are 3)
   - Number of level-up moves (0-N, each 2 bytes)
   
5. **No intrinsic terminator:** The table has no sentinel or count field. Its extent is defined by NUM_POKEMON.

**Classification:** **Consistency check only** — can validate BaseData-discovered N, cannot discover table start or count independently.

### 4.2 PokemonNames Table — CANNOT Discover Independently

**Structure:** 10 bytes per entry (NAME_LENGTH-1 = 9 characters + padding/terminator).

**Why it CANNOT be discovered independently:**

1. **No table start signature:** Just a sequence of fixed-width name records.

2. **Crystal text encoding is not unique:** Crystal character codes (0x80-0x99 for uppercase A-Z) can appear in other ROM data. A random sequence of bytes could look like valid names.

3. **No embedded count:** The table doesn't store its length. It's defined by NUM_POKEMON + padding entries.

4. **Padding entries are ambiguous:** After species 251, there are entries for "?????", "EGG", and more "?????" padding to entry 256. These use valid Crystal encoding.

5. **Table end detection requires external knowledge:** Can't distinguish real names from padding without knowing N.

**Classification:** **Consistency check only** — if table location is known, can verify N names exist.

### 4.3 PokemonCries Table — CANNOT Discover Independently

**Structure:** 6 bytes per entry: `dw cry_id, dw pitch, dw length`

**Why it CANNOT be discovered independently:**

1. **Cry IDs reuse:** Multiple species share cry IDs. cry_id ranges 1-38 in vanilla. This is too small a range to be a strong discriminator.

2. **Pitch and length are signed/arbitrary:** Pitch can be negative (signed 16-bit), length varies widely.

3. **Padding entries:** After species 251, there are 4 padding entries (252-255) all set to `CRY_NIDORAN_M, 0, 0`.

4. **No table start signature:** Just 6-byte records with no header.

5. **Values overlap with code/data:** 16-bit values like cry_id=1, pitch=128, length=129 can appear in ROM code.

**Classification:** **Consistency check only**.

### 4.4 PokemonPicPointers Table — POTENTIALLY Discoverable

**Structure:** 6 bytes per entry: `dba front, dba back` (bank + 16-bit address each)

**Independent discovery potential:**

1. **`dba` format is distinctive:** Bank byte + 16-bit banked address (0x4000-0x7FFF).

2. **Pointer pairs constraint:** Each entry has TWO valid `dba` pointers that should point to:
   - Compressed sprite data in graphics banks
   - Decompressible LZ data

3. **Graphics bank range is constrained:** Sprite data lives in specific high banks (0x48-0x80 approximately in vanilla).

4. **Special entry markers:**
   - Unown (entry 201): `dba_pics ; empty` = 0xFF:FFFF, 0xFF:FFFF
   - Entry 252 (unused): 0xFF:FFFF, 0xFF:FFFF
   - Entry 253 (Egg): has front only, back = 0xFF:FFFF

**Algorithm for independent discovery:**

```
for offset in range(0, rom_size - 6 * MIN_SEED):
    candidate_valid = true
    run_length = 0
    
    for i in range(0, max_species):
        entry_offset = offset + i * 6
        front_bank = rom[entry_offset]
        front_addr = rom[entry_offset+1:entry_offset+3]
        back_bank = rom[entry_offset+3]
        back_addr = rom[entry_offset+4:entry_offset+6]
        
        # Special marker (0xFF:FFFF) is valid
        if (front_bank == 0xFF and front_addr == 0xFFFF):
            run_length++
            continue
        
        # Bank must be reasonable (< 0x80 for 2MB ROM)
        if front_bank > 0x7F or back_bank > 0x7F:
            break
        
        # Addresses must be banked (0x4000-0x7FFF)
        if front_addr < 0x4000 or front_addr >= 0x8000:
            break
        if back_addr != 0xFFFF and (back_addr < 0x4000 or back_addr >= 0x8000):
            break
        
        # STRONG VALIDATION: Verify pointer targets contain LZ-compressed data
        front_flat = front_bank * 0x4000 + (front_addr - 0x4000)
        if !is_valid_lz_header(rom, front_flat):
            break
        
        run_length++
    
    if run_length >= MIN_SEED:
        candidates.append((offset, run_length))
```

**Key insight:** LZ header validation is distinctive:
- Crystal sprites use LZ compression
- LZ headers have recognizable structure (dimension bytes, compression markers)
- This provides target validation without knowing N

**Problem:** This still requires profile knowledge of graphics bank ranges, or must scan the entire ROM for valid LZ data, which is computationally expensive.

**Assessment:** Partially viable but with significant caveats:
- LZ validation adds complexity
- Graphics bank locations vary in hacks
- Unown special case complicates validation
- False positive rate needs empirical measurement

**Classification:** **Potential independent discovery** with heavy caveats.

### 4.5 PokemonPalettes Table — CANNOT Discover Independently

**Structure:** 8 bytes per entry: 4 colors × 2 bytes each (normal front/back, shiny front/back).

**Why it CANNOT be discovered independently:**

1. **Color values are arbitrary:** RGB555 values (0x0000-0x7FFF) can match anything.

2. **No structural constraint:** Any sequence of 8-byte records could look like palettes.

3. **No table start signature:** No header or magic number.

**Classification:** **Consistency check only**.

### 4.6 PokedexDataPointerTable — CANNOT Discover Independently

**Structure:** 2-byte pointers to Pokédex entries across 4 banks.

**Why it CANNOT be discovered independently:**

1. **Same issues as EvosAttacksPointers:** No start signature, weak pointer validation.

2. **Target validation is complex:** Pokédex entries have variable-length text.

3. **Spread across banks:** Entries are split across NUM_DEX_ENTRY_BANKS = 4 banks.

**Classification:** **Consistency check only**.

---

## 5. Minimum Run Policy Analysis

### Current: MIN_SEED = 50

The minimum run length of 50 was chosen arbitrarily for "confidence."

### Analysis of false-positive probability

**BaseData false-positive scenario:**
- Random 32-byte data at offset O
- Probability that byte[0] = 1: 1/256
- Probability that bytes[1-6] are all non-zero: (255/256)^6 ≈ 0.977
- Probability that bytes[7-8] are valid types (14 valid values): (14/256)^2 ≈ 0.003
- Probability that byte[22] is 0-5: 6/256 ≈ 0.023
- Combined per-record: ~1/256 × 0.977 × 0.003 × 0.023 ≈ 2.6 × 10^-7

For 10 consecutive valid records:
- (2.6 × 10^-7)^10 ≈ 10^-66

For even 5 consecutive valid records:
- (2.6 × 10^-7)^5 ≈ 10^-33

**Conclusion:** The type and growth-rate constraints are so restrictive that even 5-10 consecutive valid records is astronomically unlikely to occur randomly.

### Recommended policy

**MIN_SEED = 10** is defensible:
- 10 consecutive valid BaseData records is sufficient discrimination
- False positive probability ≈ 10^-66 across entire ROM
- A 20-species hack can be discovered

**However:** If we maintain MIN_SEED = 10, we MUST extend the candidate:
- Find seed of 10+ valid records
- Extend forward until first invalid record
- The extended length is the true species count

---

## 6. Candidate Ambiguity Policy

**Rule:** Multiple candidates with similar scores remain AMBIGUOUS.

Do NOT auto-select longest run. The longest run is not proof of correctness.

**Resolution options:**
1. **Independent secondary agreement:** If PokemonPicPointers can be discovered independently, use it to disambiguate
2. **Hard fail:** If multiple candidates survive all available validation, compilation fails
3. **User hint:** Allow optional profile hint to select among candidates

**Current recommendation:** Option 2 (hard fail) unless secondary proof is available.

---

## 7. Final Recommendation

### Assessment: **B. BaseData alone is strong enough, with quantified residual risk**

**Reasoning:**

1. **BaseData has unique structural constraints:**
   - Sequential DEX_NO (1, 2, 3, ...)
   - Valid Crystal type encoding (NOT contiguous 0-17, but 0-9, 19, 20-27)
   - Growth rate 0-5 only
   - All stats non-zero
   
2. **False positive probability is negligible:**
   - Even 10 consecutive valid records: P < 10^-66
   - Full ROM scan will find at most one valid candidate

3. **Secondary tables cannot help:**
   - EvosAttacksPointers: Cannot discover table start independently
   - PokemonNames: No unique signature
   - PokemonCries: No unique signature
   - PokemonPalettes: No unique signature
   - PokedexDataPointerTable: Cannot discover independently

4. **PokemonPicPointers is partially viable but complex:**
   - LZ header validation provides target verification
   - But requires graphics bank knowledge or expensive full-ROM LZ scan
   - Unown and special entries complicate validation
   - Not recommended as required secondary proof

### Recommended Algorithm

```
1. Full-ROM scan for BaseData candidates:
   - For each offset where byte[0] = 1
   - Extend while records are valid (DEX_NO sequential, types valid, growth valid, stats nonzero)
   - Record (offset, count) if count >= 10

2. Candidate selection:
   - If 0 candidates: HARD FAIL "No BaseData table found"
   - If 1 candidate: Use it
   - If >1 candidates with different counts: HARD FAIL "Ambiguous BaseData candidates"
   - If >1 candidates with same counts: Use lower offset (arbitrary tiebreaker), WARN

3. Optional consistency checks (if profile provides table offsets):
   - EvosAttacksPointers: Verify N valid banked pointers
   - PokemonNames: Verify N readable names
   - Mismatch: HARD FAIL

4. Output:
   - SpeciesDomain with discovered count N
   - BaseData table offset
   - Confidence: HIGH (single candidate) or MEDIUM (tiebreaker used)
```

### Residual Risk

| Risk | Probability | Mitigation |
|------|-------------|------------|
| False positive BaseData in random data | < 10^-66 | Negligible |
| Hack with non-sequential DEX_NO | Unknown but rare | Document as unsupported |
| Hack with modified record format | Unknown | Document as unsupported |
| Multiple valid BaseData regions | Very low | HARD FAIL or tiebreaker |

---

## 8. Expected Vanilla Evidence

For **Pokemon Crystal (USA/Europe) v1.1** (SHA1: `f2f52230b536214ef7c9924f483392993e226cfb`):

**Expected values (from pokecrystal source analysis):**
| Table | Expected Offset | Expected Count | Notes |
|-------|-----------------|----------------|-------|
| BaseData | 0x51424 | 251 | Primary anchor |
| EvosAttacksPointers | 0x425b1 | 251 | Consistency check only |
| PokemonNames | 0x53a04 | 251 + padding | Consistency check only |
| PokemonPicPointers | 0x120000 | 253 (251 + unused + Egg) | Not used for discovery |
| PokemonCries | (not audited) | 251 + 4 padding | Consistency check only |

**These are reference values from source analysis, not verified detector output.**

The detector, once implemented, should:
1. Find exactly one BaseData candidate at 0x51424
2. Count exactly 251 sequential valid records
3. If profile provides secondary offsets, verify consistency

---

## 9. Failure Conditions

| Condition | Result |
|-----------|--------|
| No BaseData candidates found | HARD FAIL |
| Multiple ambiguous candidates | HARD FAIL |
| BaseData record validation failure | HARD FAIL |
| Secondary table count mismatch (if checked) | HARD FAIL |
| Species count < 10 | HARD FAIL |
| Species count = 0 | HARD FAIL |

**Never:**
- Silently use 251
- Fall back to vanilla profile
- Accept ambiguous candidates without user hint

---

## 10. Invariant Classification Summary

### Binary-Format Guarantees
- BaseData record size = 32 bytes
- DEX_NO at byte 0
- GROWTH_RATE at byte 22 is 0-5
- Crystal type encoding (0-9, 19, 20-27)
- Evolution record types: 1-5 with specific byte lengths

### Vanilla/Source-Layout Conventions
- Sequential DEX_NO values starting at 1
- No species ID gaps
- BaseData is single contiguous table
- NUM_ATTACKS = 251 (0xFB) for move ID validation

### Heuristic Validation Signals
- Stats are non-zero
- PIC_SIZE is reasonable
- EGG_GROUPS nibbles are valid (1-15)
- Minimum 10-record seed length

---

## 11. Integration Target

```
ROM bytes
    ↓
SpeciesDomainDiscovery::discover(rom)
    ↓
SpeciesDomain {
    base_data_table_offset = discovered
    species = [(1, offset+0), (2, offset+32), ...]
}
    ↓
CrystalGameProfile {
    discovered_species = domain
}
    ↓
SemanticLegalizer::validate_species(id) → domain.is_valid(id)
```

---

## 12. EvosAttacksPointers Target Validation Grammar (Reference)

For consistency checking, here's the exact validation grammar:

```cpp
struct EvosAttacksValidator {
    // Evolution types
    static constexpr uint8_t EVOLVE_LEVEL = 1;      // 3 bytes: type, level, species
    static constexpr uint8_t EVOLVE_ITEM = 2;       // 3 bytes: type, item, species
    static constexpr uint8_t EVOLVE_TRADE = 3;      // 3 bytes: type, held_item, species
    static constexpr uint8_t EVOLVE_HAPPINESS = 4;  // 3 bytes: type, trigger, species
    static constexpr uint8_t EVOLVE_STAT = 5;       // 4 bytes: type, level, stat_cmp, species
    
    static constexpr uint8_t NUM_ATTACKS = 251;     // 0xFB
    
    bool validate_blob(const uint8_t* data, size_t max_len, uint16_t species_count) {
        size_t pos = 0;
        
        // Parse evolution records
        while (pos < max_len && data[pos] != 0) {
            uint8_t evo_type = data[pos];
            if (evo_type < 1 || evo_type > 5) return false;
            
            size_t record_len = (evo_type == EVOLVE_STAT) ? 4 : 3;
            if (pos + record_len > max_len) return false;
            
            uint8_t target_species = data[pos + record_len - 1];
            if (target_species < 1 || target_species > species_count) return false;
            
            pos += record_len;
        }
        if (pos >= max_len) return false;
        pos++;  // Skip evolution terminator
        
        // Parse level-up move records
        while (pos + 1 < max_len && data[pos] != 0) {
            uint8_t level = data[pos];
            uint8_t move = data[pos + 1];
            
            if (level > 100) return false;  // Max level is 100
            if (move < 1 || move > NUM_ATTACKS) return false;
            
            pos += 2;
        }
        if (pos >= max_len) return false;
        
        return true;  // Valid blob
    }
};
```

---

## Status

**Architecture audit complete. Recommendation: B (BaseData alone is strong enough).**

No production implementation yet. Next step: Implement detector and run against vanilla ROM.
