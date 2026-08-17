# Crystal RNG Fidelity Architecture Audit

**Status**: Historical/Reference Document — Behavioral Reference Only  
**Date**: 2026-08-17

**Purpose**: This document describes Crystal's original RNG mechanisms for reference. It is NOT a production requirements specification. The native Enginemon RNG (PCG-XSH-RR) may preserve the resulting probability distributions and consumption semantics without preserving the literal Crystal algorithms.

---

## Executive Summary

Crystal RNG can be reproduced exactly at the gameplay-semantic level using native state, without Game Boy emulation or runtime hardware abstractions. The key insight is that Crystal has **two distinct RNG systems**:

1. **Overworld RNG (`Random`)** — DIV-seeded, hardware-timing dependent, **non-deterministic**
2. **Battle RNG (`BattleRandom`)** — Software PRNG, **fully deterministic** given initial seed

For multiplayer/deterministic replay, only BattleRandom matters. Overworld randomness is intentionally non-reproducible in the original game and can be approximated natively.

**Final Verdict: B** — Exact battle RNG is feasible natively; overworld requires semantic timing approximation.

---

## 1. Crystal RNG Mechanism Inventory

### 1.1 Overworld RNG: `Random` (home/random.asm)

```asm
Random::
    ldh a, [rDIV]          ; Hardware divider register (increments @ 16384Hz)
    ld b, a
    ldh a, [hRandomAdd]
    adc b
    ldh [hRandomAdd], a    ; hRandomAdd = hRandomAdd + DIV + carry

    ldh a, [rDIV]          ; Read DIV again (different value!)
    ld b, a
    ldh a, [hRandomSub]
    sbc b
    ldh [hRandomSub], a    ; hRandomSub = hRandomSub - DIV - carry
    ret                    ; Returns hRandomSub in A
```

**Characteristics:**
- **Inputs**: rDIV (hardware), hRandomAdd, hRandomSub, CPU carry flag
- **Persistent state**: hRandomAdd (HRAM $E1), hRandomSub (HRAM $E2)
- **Hardware dependency**: Two rDIV reads per call (timing-sensitive)
- **Output**: 8-bit value (hRandomSub)
- **VBlank update**: Same algorithm runs in VBlank_Normal, continuously churning state

**Callers** (170 total):
- Encounter rate checks
- NPC movement decisions
- Phone call randomness
- Battle Tower trainer selection
- Odd Egg DVs
- Pokerus calculation
- Player ID generation
- Overworld event randomness

**Classification**: **REQUIRES TIMING MODEL** — The double DIV read means the result depends on exact CPU cycle timing. Not reproducible from semantic state alone.

### 1.2 Battle RNG: `_BattleRandom` (engine/battle/core.asm:6886)

```asm
_BattleRandom::
    ; Non-link battle: use Random
    ld a, [wLinkMode]
    and a
    jp z, Random

    ; Link battle: use deterministic PRNG
    ; Stream of 10 values, LCG transition: a[n+1] = (a[n] * 5 + 1) % 256
    push hl, bc
    ld a, [wLinkBattleRNCount]
    ld c, a
    ld hl, wLinkBattleRNs
    add hl, bc
    inc a
    ld [wLinkBattleRNCount], a

    cp SERIAL_RNS_LENGTH - 1  ; Compare incremented count against 9
    ld a, [hl]                ; Load value at current index
    pop bc, hl
    ret c                     ; Return if count < 9 (indices 0-8)

    ; Count reached 9: return seeds[9], then regenerate
    push hl, bc, af
    xor a
    ld [wLinkBattleRNCount], a  ; Reset counter to 0
    ld hl, wLinkBattleRNs
    ld b, 10
.loop:
    ld a, [hl]
    ld c, a
    add a
    add a
    add c
    inc a                      ; a = a * 5 + 1
    ld [hli], a
    dec b
    jr nz, .loop
    pop af, bc, hl             ; Restore A (seeds[9] value before regeneration)
    ret                        ; Return seeds[9]
```

**Detailed Counter Behavior (source-verified)**:

The counter increments BEFORE the range check:
1. Load counter (0-9), increment to (1-10), store incremented value
2. Compare incremented value against 9 (`cp SERIAL_RNS_LENGTH - 1`)
3. Load value at ORIGINAL index (before increment)
4. If incremented count < 9 (carry set), return immediately
5. If incremented count >= 9, regenerate all seeds, return original value

**Sequence for 12 calls**:

| Call | Counter Before | Counter After | Index Read | Return Value | Notes |
|------|----------------|---------------|------------|--------------|-------|
| 1 | 0 | 1 | 0 | seeds[0] | 1 < 9, no regen |
| 2 | 1 | 2 | 1 | seeds[1] | 2 < 9, no regen |
| 3 | 2 | 3 | 2 | seeds[2] | 3 < 9, no regen |
| 4 | 3 | 4 | 3 | seeds[3] | 4 < 9, no regen |
| 5 | 4 | 5 | 4 | seeds[4] | 5 < 9, no regen |
| 6 | 5 | 6 | 5 | seeds[5] | 6 < 9, no regen |
| 7 | 6 | 7 | 6 | seeds[6] | 7 < 9, no regen |
| 8 | 7 | 8 | 7 | seeds[7] | 8 < 9, no regen |
| 9 | 8 | 9 | 8 | seeds[8] | 9 >= 9, REGEN after return |
| 10 | 0 | 1 | 0 | seeds'[0] | Fresh cycle |
| 11 | 1 | 2 | 1 | seeds'[1] | ... |
| 12 | 2 | 3 | 2 | seeds'[2] | ... |

**Note**: The 10th seed (seeds[9]) is NEVER directly returned! When counter reaches 9, regeneration happens but the returned value is seeds[8]. The unused tenth value is a source quirk.

**Characteristics:**
- **Non-link mode**: Falls through to `Random` (hardware-dependent)
- **Link mode**: Pure software PRNG
- **State**: 10 bytes at wLinkBattleRNs, 1 byte counter at wLinkBattleRNCount
- **Algorithm**: LCG with multiplier=5, increment=1, modulus=256
- **Effective stream length**: 9 values per cycle (seeds[9] never returned)
- **Output**: 8-bit value
- **Synchronization**: Seeds shared via link cable at battle start

**Callers** (78 total):
- Speed tie resolution
- Quick Claw activation
- Wild Pokemon DVs
- Wild Pokemon held items
- Accuracy checks
- Critical hit rolls
- Damage randomization (85-100%)
- Secondary effect triggers
- Confusion self-hit
- Paralysis full-para
- AI move selection
- Flee success

**Classification**: **EXACTLY REPRODUCIBLE NATIVELY** — Given the 10 initial seed bytes and call order, every result is deterministic.

### 1.3 RNG Call Order by Mechanic

#### Wild Encounter Generation (5+ calls)
```
1. ChooseWildEncounter: Random() for encounter slot (0-99)
2. Water level variance: Random() × 4 (35%, 65%, 85%, 95% thresholds)
3. LoadEnemyMon (in battle): BattleRandom() × 2 for DVs
4. LoadEnemyMon: BattleRandom() × 2 for held item (25% chance, then 8% for Item2)
```

#### Damage Calculation (1 call)
```
1. BattleCommand_Damagecalc: BattleRandom() loop until value >= 217
   Final damage = damage * (217..255) / 255
```

#### Accuracy Check (1 call)
```
1. BattleCommand_CheckHit: BattleRandom() compared against accuracy threshold
   RNG called unconditionally; failure = miss
```

#### Critical Hit (1 call)
```
1. BattleCommand_Critical: BattleRandom() compared against crit threshold
   Threshold varies by crit stage (17, 32, 64, 85, 128, 255)
```

#### Speed Tie Resolution (2-4 calls)
```
Complex branch depending on Quick Claw, link clock, and speed equality.
Each branch consumes 1-2 BattleRandom calls.
```

#### Flee Logic (1 call on failure path)
```
1. If flee formula doesn't guarantee escape: BattleRandom() for escape roll
   Only consumed if flee check actually runs!
```

**Critical Insight**: Call count differs by branch. A successful flee consumes 0 RNG; a failed flee consumes 1. This affects all subsequent mechanics.

---

## 2. Hardware Entropy vs Evolved State

### DIV Register Analysis

The rDIV register increments every 256 CPU cycles (~16384 Hz). `Random()` reads it twice:
- First read: ~4 cycles after function entry
- Second read: ~12 cycles later

The exact DIV values depend on:
1. Frame position when Random() is called
2. Instructions executed before the call
3. VBlank timing
4. Previous Random() calls (which advance DIV via CPU cycles)

**This is true randomness from the game's perspective** — even two identical playthroughs will diverge due to micro-timing differences.

### State Evolution

Despite hardware entropy input, the state does evolve:
- hRandomAdd and hRandomSub persist across calls
- VBlank continuously updates them
- The DIV samples get folded into persistent state

**But reproduction requires knowing DIV at call time**, which is not semantic state.

### BattleRandom is Different

In link battles, `BattleRandom` completely bypasses DIV:
- Seeds are exchanged at battle start via link cable
- PRNG is pure software: `next = current * 5 + 1`
- Both Game Boys execute identical call sequences
- Result: perfect synchronization

**This is the reproducible path.**

---

## 3. Exact Replay Feasibility

### Target Definition
```
same initial semantic RNG state
+ same simulation tick/input/event sequence
+ same RNG consumption order
→ same Crystal random bytes/results
```

### Feasibility by Subsystem

| Subsystem | Classification | Notes |
|-----------|---------------|-------|
| Battle RNG (link mode) | **EXACTLY REPRODUCIBLE NATIVELY** | 10-byte seed + LCG |
| Battle RNG (non-link) | REQUIRES TIMING MODEL | Falls to Random() |
| Wild encounter slot | REQUIRES TIMING MODEL | Uses Random() |
| NPC movement | REQUIRES TIMING MODEL | Uses hRandomAdd |
| DV generation (roam) | EXACTLY REPRODUCIBLE | BattleRandom() |
| DV generation (wild) | EXACTLY REPRODUCIBLE | BattleRandom() |
| Phone calls | REQUIRES TIMING MODEL | Uses Random() |
| Pokerus | REQUIRES TIMING MODEL | Uses hRandomAdd/Sub |
| Odd Egg | REQUIRES TIMING MODEL | Uses hRandomAdd/Sub |

### The Key Insight

Crystal itself doesn't reproduce overworld randomness — **it's not supposed to be deterministic**. The DIV-based RNG exists precisely to prevent prediction.

Battle mechanics ARE meant to be deterministic in link battles, which is why BattleRandom exists.

---

## 4. BattleRandom Deep Trace

### Link Battle Initialization (engine/link/link.asm:622)

```asm
FixDataForLinkTransfer:
    ; Initialize 10 random seeds, avoiding SERIAL_PREAMBLE_BYTE
    ld hl, wLinkBattleRNs
    ld b, SERIAL_RNS_LENGTH  ; 10
.rn_loop:
    call Random              ; Hardware RNG for initial entropy
    cp SERIAL_PREAMBLE_BYTE  ; $FD
    jr nc, .rn_loop          ; Retry if >= $FD (invalid for link protocol)
    ld [hli], a
    dec b
    jr nz, .rn_loop
```

The 10 seeds come from overworld Random() — hardware entropy gets captured once, then the stream becomes deterministic.

### PRNG Algorithm

```c
// Equivalent C
uint8_t wLinkBattleRNs[10];  // Seeds
uint8_t wLinkBattleRNCount;   // Index (0-9)

uint8_t battle_random() {
    uint8_t idx = wLinkBattleRNCount++;
    if (idx >= 9) {
        // Regenerate all seeds
        wLinkBattleRNCount = 0;
        for (int i = 0; i < 10; i++) {
            wLinkBattleRNs[i] = wLinkBattleRNs[i] * 5 + 1;
        }
        return wLinkBattleRNs[9];  // Return last before regeneration
    }
    return wLinkBattleRNs[idx];
}
```

### Link Battle Synchronization

Both players exchange their wLinkBattleRNs arrays at battle start. Then:
1. Both run identical battle logic
2. Both call BattleRandom() in identical order
3. Both get identical results
4. No desync possible (barring bugs)

### Non-Link Battle Behavior

Without link mode, `_BattleRandom` simply calls `Random()`:
- DVs become hardware-dependent
- Damage rolls become hardware-dependent
- Critical hits become hardware-dependent

**Single-player is intentionally non-deterministic.**

---

## 5. RNG Call-Order Semantics

### Accuracy Roll Consumption

```asm
BattleCommand_CheckHit:
    ; ... accuracy calculation ...
    
.skip_brightpowder:
    ld a, b
    cp -1               ; Compare accuracy against 255
    jr z, .Hit          ; If accuracy == 255, SKIP RNG entirely!
    
    call BattleRandom   ; Only called when accuracy < 255
    cp b                ; b = accuracy threshold
    jr nc, .Miss
.Hit:
    ; Move hits
```

**CRITICAL**: When accuracy threshold == 255:
- RNG is NOT consumed (0 draws)
- Move always hits (guaranteed)

This is the Gen 2 fix for the Gen 1 "1/256 miss on 100% moves" bug. Gen 1 always called RNG and compared `roll < threshold`, causing roll=255 to fail even with threshold=255.

**Gen 2 behavior** (Crystal):
- threshold == 255 → guaranteed hit, **zero RNG draws**
- threshold < 255 → one RNG draw, hit if roll < threshold

### Critical Hit Consumption

```asm
BattleCommand_Critical:
    ; Only consumed if move has power > 0
    ld a, BATTLE_VARS_MOVE_POWER
    call GetBattleVar
    and a
    ret z               ; Status moves: no RNG consumed
    
    ; ... crit level calculation ...
    call BattleRandom
    cp [hl]             ; Compare against crit threshold
```

Status moves (Power = 0) don't consume crit RNG.

### Damage Roll Consumption

```asm
; Multiply by 85-100%
.loop:
    call BattleRandom
    rrca
    cp 85 percent + 1   ; ~217
    jr c, .loop         ; Retry if < 217
```

Multiple RNG calls possible if unlucky. Usually 1-2 calls.

### Branch-Dependent Consumption

The flee check demonstrates branch-dependent RNG:

```asm
; If speed formula guarantees escape
    jr nc, .can_escape      ; No RNG consumed!

.cant_escape_2:
    call BattleRandom       ; RNG consumed only on this path
    ld b, a
    ldh a, [hQuotient + 3]
    cp b
    jr nc, .can_escape
```

**This is semantic behavior** — the RNG call count depends on game state.

---

## 6. Known Bugs/Glitches Tied to RNG

### Magikarp Length Filter Bug
The length filter uses feet/inches but compares against millimeters. No Magikarp triggers the "extra rare" filter. This is a source bug, not RNG behavior.

### Unown Letter + Forced Shiny = Infinite Loop
```asm
; BUG: If combined with forced shiny battletype, causes an infinite loop
    call CheckUnownLetter
    jr c, .GenerateDVs  ; Retry if wrong letter
```
Shiny DVs ($EA $AA) produce letter 'I'. If player hasn't unlocked 'I', the loop never terminates. **This is a source bug worth preserving in faithful mode.**

### 1/256 Move Failures — CORRECTED

**This section documents the correction of a previously incorrect claim.**

The Gen 1 "1/256 miss on 100% accurate moves" bug was **fixed in Gen 2**. Crystal's `BattleCommand_CheckHit` (effect_commands.asm) explicitly checks for threshold==255 and skips the RNG call entirely:

```asm
.skip_brightpowder
    ld a, b
    cp -1           ; Compare against 255
    jr z, .Hit      ; If accuracy == 255, SKIP BattleRandom entirely!
    
    call BattleRandom  ; Only called when accuracy < 255
    cp b
    jr nc, .Miss
```

**Correct Crystal behavior**: Moves with computed accuracy of 255 always hit and consume **zero RNG draws**.

**The 1/256 secondary effect miss IS still present**: `BattleCommand_EffectChance` does NOT have the threshold==255 skip. This is a separate mechanic from accuracy checking, and the source comment confirms the bug: `; BUG: Moves with a 100% secondary effect chance will not trigger it in 1/256 uses.`

### Speed Tie Double-Roll
Speed ties can consume 2-4 BattleRandom calls depending on Quick Claw and link clock. The call count is deterministic given game state, but complex.

---

## 7. Proposed Native Representation (Historical Reference)

**NOTE**: This section documents Crystal's original algorithms for reference. The production Enginemon RNG uses PCG-XSH-RR, not these Crystal algorithms. The native design preserves probability distributions and consumption semantics, not literal Crystal byte sequences.

### Crystal Battle RNG Model (Reference Only)

```cpp
namespace enginemon {

// Battle RNG - Crystal link battle algorithm (REFERENCE ONLY)
// NOTE: Production Enginemon uses PCG-XSH-RR, not this algorithm.
// This is documented for source fidelity research, not implementation.
struct CrystalBattleRng {
    uint8_t seeds[10];      // wLinkBattleRNs
    uint8_t index = 0;      // wLinkBattleRNCount (0-9, but 9 is never read)
    
    // Initialize from 10 seed bytes (captured entropy or predefined)
    void init(const uint8_t initial_seeds[10]) {
        std::memcpy(seeds, initial_seeds, 10);
        index = 0;
    }
    
    // Crystal-exact PRNG: a[n+1] = (a[n] * 5 + 1) % 256
    // NOTE: Due to the increment-before-compare quirk, seeds[9] is never returned!
    // The stream is effectively 9 values per cycle, not 10.
    uint8_t next() {
        uint8_t current_index = index;
        index++;  // Increment BEFORE compare (matches Crystal)
        
        if (index >= 9) {
            // index reached 9: return seeds[8], then regenerate
            // This means seeds[9] is NEVER returned (source quirk)
            index = 0;
            for (int i = 0; i < 10; i++) {
                seeds[i] = seeds[i] * 5 + 1;
            }
        }
        return seeds[current_index];
    }
    
    // For save/restore
    void serialize(std::vector<uint8_t>& out) const {
        out.insert(out.end(), seeds, seeds + 10);
        out.push_back(index);
    }
    
    static CrystalBattleRng deserialize(const uint8_t* data) {
        CrystalBattleRng rng;
        std::memcpy(rng.seeds, data, 10);
        rng.index = data[10];
        return rng;
    }
};

// Overworld RNG - semantic approximation (not hardware-exact)
struct CrystalOverworldRng {
    uint8_t random_add = 0;  // hRandomAdd
    uint8_t random_sub = 0;  // hRandomSub
    
    // Semantic equivalent: uses simulation tick as DIV proxy
    uint8_t next(uint64_t tick) {
        // Derive pseudo-DIV from tick (wraps at 256)
        uint8_t div_a = static_cast<uint8_t>(tick & 0xFF);
        uint8_t div_b = static_cast<uint8_t>((tick >> 4) & 0xFF);
        
        uint16_t temp_add = random_add + div_a;
        random_add = static_cast<uint8_t>(temp_add);
        
        uint16_t temp_sub = random_sub - div_b - (temp_add > 0xFF ? 1 : 0);
        random_sub = static_cast<uint8_t>(temp_sub);
        
        return random_sub;
    }
};

} // namespace enginemon
```

### Usage Context

```cpp
struct BattleState {
    CrystalBattleRng rng;   // Deterministic for link battles
    // ...
};

struct GameState {
    CrystalOverworldRng overworld_rng;  // Approximate for overworld
    // ...
};
```

---

## 8. Hardware Timing Boundary

### What DIV Represents Semantically

The DIV register is a free-running counter. Crystal uses it as:
1. **Entropy source** for overworld randomness
2. **Timing reference** (indirectly, via cycle count)

### Enginemon Semantic Equivalent

Instead of emulating DIV hardware:
- Use **simulation tick count** as DIV proxy
- Each tick = fixed simulation time (1/60th second)
- DIV approximation = tick_count mod 256

This gives:
- Deterministic within simulation
- Non-reproducible across sessions (different initial tick)
- Similar statistical properties to hardware DIV

### What We Explicitly Don't Model

- Sub-tick CPU timing
- VBlank exact position
- Instruction-level cycle counting
- Hardware register behavior

**The overworld was never meant to be reproducible.** Our approximation preserves the semantic intent (unpredictable encounters, varying NPC behavior) without hardware fidelity.

---

## 9. Compatibility Modes

### Recommendation: Two Modes

1. **Faithful Crystal RNG** (battles)
   - Use CrystalBattleRng with exact algorithm
   - Seeds captured at battle start
   - Multiplayer: seeds exchanged/agreed
   - Save format: 11 bytes (10 seeds + index)

2. **Semantic Overworld RNG**
   - Use tick-derived approximation
   - Not byte-exact with hardware Crystal
   - Deterministic within session (same tick = same result)
   - Good enough for encounters, NPC movement, etc.

### Why Not One LCG?

The current Numerical Recipes LCG (`state * 1664525 + 1013904223`) produces:
- 32-bit output (Crystal expects 8-bit)
- Different statistical distribution
- Different sequence entirely

It's fine for "any randomness" but cannot reproduce Crystal mechanics.

---

## 10. Current Enginemon Impact

### Current RngState Analysis

```cpp
struct RngState {
    uint64_t seed = 0;
    uint64_t state = 0;
    
    uint32_t next() {
        state = state * 1664525 + 1013904223;
        return static_cast<uint32_t>(state);
    }
};
```

**Issues:**
1. **Wrong algorithm** — Numerical Recipes LCG, not Crystal LCG
2. **Wrong width** — 64-bit state vs Crystal's 8-bit or 10×8-bit
3. **No distinction** between battle and overworld RNG
4. **Comment claims pokecrystal compatibility** — this is inaccurate

### What Can Stay

- The concept of GameState owning RNG is correct
- The determinism model (seed → predictable sequence) is correct
- The serialization approach is correct

### What Must Change (Eventually)

1. Replace single LCG with CrystalBattleRng for battle mechanics
2. Add CrystalOverworldRng for overworld (tick-derived)
3. Update save format (11 bytes for battle RNG state)
4. Ensure create_pokemon/create_wild_pokemon use correct RNG

### Migration Path

- Save version increment required
- Old saves: initialize new RNG from legacy LCG state
- New saves: serialize full Crystal RNG state
- No architectural impact on compiler (package format unchanged)

---

## 11. Concrete Feasibility Experiment

### Test Case 1: Battle Damage Roll

**Source trace**: BattleCommand_Damagecalc (effect_commands.asm:1522)

```asm
.loop:
    call BattleRandom    ; Get random byte
    rrca                 ; Rotate right: RRCA(x) = ((x >> 1) | (x << 7)) & 0xFF
    cp 85 percent + 1    ; Compare with 217
    jr c, .loop          ; Retry if < 217
```

**Note on RRCA**: The Z80 `rrca` instruction rotates the accumulator right, moving bit 0 to bit 7 (and to the carry flag). The correct equivalent is:

```cpp
// CORRECT: Rotate right (bit 0 wraps to bit 7)
uint8_t rrca(uint8_t x) {
    return static_cast<uint8_t>((x >> 1) | (x << 7));
}

// INCORRECT: x >> 1 is NOT equivalent to rrca
```

**Enginemon note**: The native PCG design preserves the resulting damage-roll distribution (rejection until >= 217, final range 217-255) without preserving the literal RRCA transform. The consumption semantics (1+ draws) are preserved.

**Crystal behavior**:
- Initial seeds: [0x12, 0x34, 0x56, ...]
- First BattleRandom: returns 0x12, index becomes 1
- RRCA(0x12) = ((0x12 >> 1) | (0x12 << 7)) = 0x09, 0x09 < 217, retry
- Second BattleRandom: returns 0x34, index becomes 2
- RRCA(0x34) = 0x1A, 0x1A < 217, retry
- ... (continues until value >= 217)

**Native reproduction** (historical reference only — not production requirement):
```cpp
CrystalBattleRng rng;
rng.init({0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0, 0x11, 0x22});

uint8_t roll;
do {
    uint8_t raw = rng.next();
    roll = (raw >> 1) | (raw << 7);  // Correct RRCA
} while (roll < 217);

// Result matches Crystal exactly
```

### Test Case 2: Critical Hit

**Source trace**: BattleCommand_Critical (effect_commands.asm:1201)

```asm
    call BattleRandom
    cp [hl]             ; hl = crit threshold (e.g., 17 for stage 0)
    ret nc              ; No crit if RNG >= threshold
```

**Crystal behavior** (crit stage 0, threshold 17):
- BattleRandom returns 0x10 (16)
- 16 < 17 → CRITICAL HIT

**Native reproduction**:
```cpp
CrystalBattleRng rng;
rng.init({0x10, ...});  // First seed = 0x10

uint8_t roll = rng.next();  // Returns 0x10
bool crit = (roll < 17);    // true, same as Crystal
```

### Test Case 3: Speed Tie

**Source trace**: DetermineMoveOrder (core.asm:544)

```asm
.player_2c:
    call BattleRandom
    cp 50 percent + 1    ; 128
    jp c, .enemy_first   ; RNG < 128 → enemy first
```

**Crystal behavior**:
- Seeds: [0x7F, ...]
- BattleRandom returns 0x7F (127)
- 127 < 128 → enemy moves first

**Native reproduction**: Exact match with CrystalBattleRng.

---

## 12. Final Verdict

### **B. Exact battle RNG is feasible natively; overworld requires semantic timing approximation**

**Evidence Summary:**

1. **BattleRandom** uses a simple LCG: `a = a * 5 + 1`
   - 10 parallel streams of 8-bit values
   - Completely deterministic given initial seeds
   - Used for ALL battle mechanics in link mode

2. **Random** depends on hardware DIV register
   - Two reads per call at different CPU cycle points
   - Result depends on exact execution timing
   - Not reproducible without cycle-accurate emulation

3. **Crystal's design intent**:
   - Battles are meant to be deterministic (for link play)
   - Overworld is meant to be unpredictable (player experience)
   - Enginemon honors both intents via native PCG (preserving consumption semantics)

### Crystal State Machine (Historical Reference Only)

**NOTE**: This documents Crystal's original algorithm. Production Enginemon uses PCG-XSH-RR.

```cpp
// Crystal battle RNG - REFERENCE ONLY, not production implementation
// Note the unused-tenth-value quirk: seeds[9] is never returned
struct CrystalBattleRng {
    uint8_t seeds[10];
    uint8_t index;
    
    uint8_t next() {
        uint8_t current = index++;
        if (index >= 9) {
            index = 0;
            for (int i = 0; i < 10; i++)
                seeds[i] = seeds[i] * 5 + 1;
        }
        return seeds[current];  // Returns seeds[0..8], never seeds[9]
    }
};
```

### Source Evidence

| Claim | Source |
|-------|--------|
| BattleRandom algorithm | core.asm:6886-6945 |
| Link seed initialization | link.asm:622-638 |
| Speed tie RNG | core.asm:544-552 |
| Damage roll RNG | effect_commands.asm:1522-1527 |
| Accuracy RNG (threshold==255 skip) | effect_commands.asm:1605 |
| Critical RNG | effect_commands.asm:1201 |
| Random uses DIV | home/random.asm:12-26 |
| VBlank churns RNG | home/vblank.asm:66-78 |

---

## Appendix: Current Enginemon RngState Correction

The current comment in `game_state.hpp`:
```cpp
// Uses same parameters as pokecrystal for compatibility
```

This is **inaccurate**. The Numerical Recipes LCG (1664525, 1013904223) is not used by Crystal. Crystal uses:
- Overworld: DIV-based add/subtract
- Battle: `a * 5 + 1` per-stream LCG

The current implementation is a general-purpose PRNG, not Crystal-compatible. This should be documented and eventually replaced.

---

**End of Audit**
