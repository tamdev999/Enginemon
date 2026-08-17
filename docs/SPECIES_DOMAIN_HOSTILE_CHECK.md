# ROM-Only Species Domain Discovery — Hostile Check

## Status

This document independently verifies the claims in `SPECIES_DOMAIN_DISCOVERY.md`.

---

## 1. Probability Claim: "< 10^-66 for 10 records"

### Verdict: **FALSE / OVERCLAIMED**

The previous audit claimed:
> false positive probability < 10^-66 for 10 BaseData records

This claim is **statistically indefensible** for multiple reasons:

### 1.1 ROM Bytes Are NOT Uniformly Random

**Empirical measurement from actual Crystal ROM:**

| Byte Class | Expected (Uniform) | Actual | Deviation |
|------------|-------------------|--------|-----------|
| Byte == 1 | 0.39% | 1.88% | **4.8× higher** |
| Valid type (0-9,19,20-27) | 7.42% | 37.65% | **5.07× higher** |
| Valid growth (0-5) | 2.34% | 32.63% | **13.92× higher** |
| 6-byte non-zero run | 97.68% | 60.69% | 0.62× lower |

ROM data is **highly structured**, not IID random. Low-value bytes (0-9) appear frequently in code, offsets, and constants. The uniform-random model is invalid.

### 1.2 Sequential DEX_NO Pattern Is Rare But Not Astronomically So

**Empirical finding:**
- Patterns of sequential bytes (1,2,3,...) at stride 32: **32 occurrences** in full ROM
- Only **1** survives full BaseData validation

However, the claim of "< 10^-66" implied essentially zero false positives in any conceivable ROM. The actual rate of ~32 sequential matches in 2MB is orders of magnitude higher than predicted.

### 1.3 Corrected Assessment

The BaseData validator works **empirically** on vanilla Crystal, but the probability claim should be:

> "BaseData validation produces a unique candidate on vanilla Crystal ROM empirically. The false positive probability cannot be quantified precisely because ROM bytes are not uniformly distributed."

---

## 2. Empirical BaseData Scanning

### Verdict: **VERIFIED BY ROM EXPERIMENT**

Full ROM scan results:

| min_seed | Candidates | Winning Offset | Run Length |
|----------|------------|----------------|------------|
| 3 | 1 | 0x51424 | 251 |
| 4 | 1 | 0x51424 | 251 |
| 5 | 1 | 0x51424 | 251 |
| 8 | 1 | 0x51424 | 251 |
| 10 | 1 | 0x51424 | 251 |
| 20 | 1 | 0x51424 | 251 |
| 50 | 1 | 0x51424 | 251 |

**Key findings:**

1. **UNIQUE candidate at all seed lengths** — Even min_seed=3 produces exactly 1 candidate
2. **Correct offset discovered** — 0x51424 matches expected from source analysis
3. **Correct count derived** — 251 records, terminated by `dex_no_mismatch:129!=252`
4. **NO hardcoded values used** — Pure structural validation

### Why Did Other Sequential Patterns Fail?

Sequential patterns at stride 32 were found at 32 locations. Here's why all but one failed:

| Offset | Seq Length | Fail Reason |
|--------|------------|-------------|
| 0x51424 | 251 | ✓ PASSES — Real BaseData |
| 0xd14fa | 5 | zero_stat_1 (stats contain 0) |
| 0xd12a0 | 4 | zero_stat_0 |
| 0x11dae6 | 4 | zero_stat_0 |
| 0x12c35a | 4 | invalid_type1 (type=67) |
| 0x4d0a6 | 3 | invalid_type1 (type=217) |
| 0x71171 | 3 | zero_stat_5 |

**Conclusion:** The combination of constraints (sequential DEX_NO + non-zero stats + valid types + valid growth) is sufficient for unique identification on vanilla Crystal.

---

## 3. "No Independently Discoverable Secondary Table"

### Verdict: **VERIFIED BY ROM EXPERIMENT**

### 3.1 EvosAttacksPointers

Cannot discover independently:
- Table has no start signature
- Pointer validation (0x4000-0x7FFF) produces many false positives
- Target grammar validation requires knowing N

### 3.2 Code Reference Discovery — WEAK

Attempted code-based discovery using:
- `ld bc/de, 32` followed by `call`
- Bank switch patterns
- AddNTimes indexing

**Results:**

| Table Address | Code References in Home Bank | Discoverable? |
|---------------|------------------------------|---------------|
| 0x5424 (BaseData) | 2 | NO — not unique |
| 0x5b00 | 7 | More references |
| 0x5afb | 4 | More references |
| 0x4000 | 9 | Most common |

**Conclusion:** BaseData address (0x5424) appears only **2 times** in home bank code. Other addresses appear more frequently. Code reference discovery **cannot** uniquely identify BaseData without prior knowledge of stride, bank, or address.

### 3.3 Summary

The claim "no independently discoverable secondary table" is **correct**:
- Pointer tables have no unique signatures
- Code references are not distinctive
- All secondary tables require known offset or BaseData-derived N

---

## 4. Separated Claims Assessment

### 4.1 BaseData Can Be LOCATED Reliably

**Verdict: VERIFIED BY ROM EXPERIMENT**

The structural validator (sequential DEX_NO + type constraints + growth constraints + non-zero stats) produces a **unique candidate** on vanilla Crystal ROM at all tested seed lengths.

**Caveat:** Only tested on one ROM. Requires testing on ROM hacks for generalization.

### 4.2 BaseData Extent N Can Be DERIVED Reliably

**Verdict: VERIFIED BY ROM EXPERIMENT**

Once located, extension until first invalid record yields:
- Exact count: 251
- Termination reason: `dex_no_mismatch:129!=252` (record 252 has DEX_NO = 0x81 = 129)

The extent derivation is deterministic and correct.

### 4.3 N Can Be Independently CROSS-CHECKED

**Verdict: PLAUSIBLE BUT UNPROVEN**

The audit claimed secondary tables could only provide consistency checks, not independent discovery. This is correct. However:

- We did NOT test consistency checking with secondary tables
- We did NOT verify EvosAttacksPointers has exactly 251 valid entries
- We did NOT verify PokemonNames has 251 names

**Status:** The claim that cross-checking IS possible (given known offsets) was not tested. The claim that cross-checking CANNOT discover N independently is verified.

---

## 5. Summary of Verdicts

| Claim | Verdict | Notes |
|-------|---------|-------|
| False positive probability < 10^-66 | **FALSE** | Invalid statistical model; ROM bytes not uniform |
| BaseData validator produces unique candidate | **VERIFIED** | Empirically true for vanilla Crystal at all seed lengths |
| BaseData can be LOCATED reliably | **VERIFIED** | Unique candidate at 0x51424 |
| BaseData extent N can be DERIVED reliably | **VERIFIED** | Yields exactly 251 |
| N can be independently CROSS-CHECKED | **PLAUSIBLE BUT UNPROVEN** | Not tested |
| No independently discoverable secondary table | **VERIFIED** | Code references not distinctive |
| min_seed=10 is sufficient | **VERIFIED** | Even min_seed=3 yields unique candidate |
| Code-reference discovery viable | **FALSE** | BaseData has only 2 code refs, not unique |

---

## 6. Recommendations

### 6.1 Remove False Probability Claims

Replace:
> "False positive probability < 10^-66"

With:
> "Empirically produces unique candidate on vanilla Crystal ROM. Probability cannot be quantified due to non-uniform ROM byte distribution."

### 6.2 Document Empirical Basis

The discovery works because of **empirically observed** constraints:
- Sequential DEX_NO at stride 32 is rare (~32 matches in 2MB)
- Combining with type/growth/stat validation reduces to 1 match
- This is **observation**, not **proof**

### 6.3 Test on ROM Hacks

Before claiming hack tolerance, test on:
- 245-species ROM
- 274-species ROM
- Relocated BaseData ROM

### 6.4 Minimum Seed Length

**Verified:** min_seed=3 produces unique candidate.

However, recommend min_seed=10 for safety margin against unknown ROMs.

---

## Appendix: Probe Output Summary

### Sequential DEX_NO Patterns (stride=32)

```
Found 32 patterns of length >= 3:
  offset=0x51424 length=251  <-- REAL BaseData
  offset=0xd14fa length=5    (fails: zero stat)
  offset=0xd12a0 length=4    (fails: zero stat)
  offset=0x11dae6 length=4   (fails: zero stat)
  offset=0x12c35a length=4   (fails: invalid type)
  [27 more with length=3, all fail validation]
```

### Full BaseData Validation

```
Winning candidate: offset=0x51424 run=251
Fail reason: dex_no_mismatch:129!=252

First records:
  [1] DEX=1 stats=[45,49,49,45,65,65] types=22,3 growth=3  ✓
  [2] DEX=2 stats=[60,62,63,60,80,80] types=22,3 growth=3  ✓
  ...
  [251] DEX=251 stats=[100,100,100,100,100,100] types=24,22 growth=3  ✓

First failing record (#252):
  DEX=129 stats=[148,139,129,128,146,128] types=148,145 growth=141  ✗
```

---

## Files Created

- `tools/basedata_probe.py` — Full ROM scan with distribution analysis
- `tools/basedata_probe2.py` — Analysis of failed candidates
- `tools/code_reference_probe.py` — Code reference discovery attempt
- `tools/code_pattern_probe.py` — GetBaseData pattern analysis
- `tools/code_pattern_probe2.py` — Deeper code analysis

All probes are temporary and should be removed after audit.
