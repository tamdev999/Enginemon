# Enginemon Native Deterministic RNG Architecture

**Status**: Design Audit Complete — Implementation Ready  
**Date**: 2026-08-17  
**Prerequisite**: Crystal RNG Fidelity Audit (completed)

---

## Executive Summary

Enginemon adopts a single canonical deterministic PRNG that replaces Crystal's hardware entropy while preserving all gameplay probability mechanics and RNG consumption semantics.

**Recommended PRNG**: PCG-XSH-RR (32-bit output, 64-bit state)

**Core Principle**: Crystal formulas and call ordering are authoritative; the entropy source is modernized.

**Terminology Clarification**: The term "Crystal-faithful" refers to preserving Crystal's probability formulas, threshold comparisons, and RNG consumption ordering—NOT reproducing Crystal's exact RNG sequence. The native PCG produces different byte values than Crystal's hardware DIV-based RNG. What is preserved is the *structure* of randomness consumption, not the specific values.

---

## 1. Compatibility Boundary

### What Remains Crystal-Faithful

| Category | Preserved Exactly |
|----------|-------------------|
| Encounter probabilities | ✅ Slot thresholds (1-100), time-of-day tables |
| Accuracy thresholds | ✅ threshold==255 skips RNG (0 draws); otherwise 1 draw |
| Critical hit thresholds | ✅ Stage-based thresholds (17, 32, 64, 85, 128, 255) |
| Damage variation | ✅ Rejection loop until byte ≥ 217, then damage * byte / 255 |
| Secondary effect probabilities | ✅ Exact threshold comparisons (1/256 bug preserved) |
| Speed tie resolution | ✅ 50% chance, Quick Claw logic |
| Flee formulas | ✅ Speed ratio calculation, random comparison |
| AI random choices | ✅ Move selection, item use probabilities |
| Branch-dependent consumption | ✅ Exact call count per branch |
| Rejection loops | ✅ Repeated draws until condition met |

### What Is Replaced

| Category | Replacement |
|----------|-------------|
| DIV register reads | PCG next_u8() |
| VBlank RNG churn | Not reproduced (no semantic effect) |
| hRandomAdd/Sub state | Single PCG state |
| Link-cable 10-byte PRNG | Same single PCG |
| Hardware timing entropy | Secure initial seed, then deterministic |

### Policy Coherence Verification

**Question**: Can we preserve Crystal mechanics while replacing entropy?

**Answer**: Yes. Crystal's gameplay mechanics are defined by:

1. Threshold comparisons (byte < threshold)
2. Formula applications (damage * byte / 255)
3. Call ordering (which branch consumes how many draws)

None of these depend on *how* the byte was generated—only on *what value* it has. The statistical distribution (uniform 0-255) matters, not the physical source.

**The compatibility boundary is coherent.**

---

## 2. PRNG Selection

### Candidates Evaluated

| Generator | State Size | Output | Period | Quality | Deterministic | Portable |
|-----------|------------|--------|--------|---------|---------------|----------|
| PCG-XSH-RR | 64-bit | 32-bit | 2^64 | Excellent | ✅ | ✅ |
| xoroshiro128+ | 128-bit | 64-bit | 2^128 | Good | ✅ | ✅ |
| SplitMix64 | 64-bit | 64-bit | 2^64 | Good | ✅ | ✅ |
| Numerical Recipes LCG | 64-bit | 32-bit | 2^64 | Poor | ✅ | ✅ |
| std::mt19937 | 624×32-bit | 32-bit | 2^19937 | Excellent | ❌ | ❌ |

### Ranking Criteria

1. **Statistical quality**: PCG-XSH-RR > xoroshiro > SplitMix > LCG
2. **Deterministic specification**: All except std:: qualify
3. **Cross-platform reproducibility**: Must use explicit algorithm, not stdlib
4. **Compact serialized state**: PCG (8 bytes) < xoroshiro (16 bytes) << mt19937 (2.5KB)
5. **Speed**: All candidates are fast enough
6. **Jump/split capabilities**: PCG has well-defined jump (optional)
7. **Implementation simplicity**: PCG/SplitMix are ~10 lines each
8. **Long-term stability**: PCG has stable published specification
9. **Replay/network suitability**: Small state = easy sync

### Recommendation: PCG-XSH-RR (32-bit output)

**Reasons**:
- Excellent statistical quality (passes PractRand, TestU01)
- Compact state (8 bytes = 64 bits)
- Published, stable specification
- No standard library dependency
- Well-suited for bounded sampling
- Author (Melissa O'Neill) provides reference implementation
- Used successfully in many game engines

**Algorithm (O'Neill reference-faithful)**:

```cpp
// PCG-XSH-RR: Permuted Congruential Generator
// State: 64-bit, Output: 32-bit
struct PcgState {
    uint64_t state_;
    
    static constexpr uint64_t MULTIPLIER = 6364136223846793005ULL;
    static constexpr uint64_t INCREMENT  = 1442695040888963407ULL;  // Must be odd
    
    // Reference-faithful seeding: O'Neill's canonical initialization
    void seed(uint64_t seed) {
        state_ = 0;
        step();                    // Advance from zero state
        state_ += seed;            // Mix in seed
        step();                    // Advance again
    }
    
    // Internal state advancement
    void step() {
        state_ = state_ * MULTIPLIER + INCREMENT;
    }
    
    uint32_t next() {
        uint64_t old_state = state_;
        state_ = old_state * MULTIPLIER + INCREMENT;
        
        // XSH-RR: xorshift high, random rotate
        uint32_t xorshifted = static_cast<uint32_t>(((old_state >> 18) ^ old_state) >> 27);
        uint32_t rot = static_cast<uint32_t>(old_state >> 59);
        return (xorshifted >> rot) | (xorshifted << ((-rot) & 31));
    }
};
```

**Note**: The `seed()` function uses O'Neill's canonical two-step initialization (`state=0; step(); state+=seed; step();`) rather than directly assigning the seed. This ensures proper mixing regardless of seed value.


---

## 3. Output API

### Terminology: "Draw" Definition

Throughout this document, **"draw"** means exactly one core PRNG state advancement via `next_u32()`. All consumption counts are expressed in draws.

### Primitive Draw Counts (Contractual)

| API Call | Core Draws | Notes |
|----------|------------|-------|
| `next_u32()` | 1 | Core primitive, advances state once |
| `next_u8()` | 1 | Calls `next_u32()` once, masks to 8 bits |
| `next_u64()` | 2 | Calls `next_u32()` twice in defined sequence |
| `bounded(n)` | 1+ | Rejection sampling; 1 draw typical, more if rejected |

### Primitive Operations

```cpp
class GameplayRng {
public:
    // === Core primitive (1 draw) ===
    uint32_t next_u32();
    
    // === Derived primitives ===
    
    // 8-bit draw for Crystal-derived mechanics (1 draw)
    // Used wherever Crystal calls BattleRandom
    uint8_t next_u8() {
        return static_cast<uint8_t>(next_u32());
    }
    
    // 64-bit draw with DEFINED sequencing (2 draws)
    // First draw = high 32 bits, second draw = low 32 bits
    uint64_t next_u64() {
        uint64_t hi = next_u32();  // Draw 1
        uint64_t lo = next_u32();  // Draw 2
        return (hi << 32) | lo;
    }
    
    // === Bounded sampling (1+ draws) ===
    // PRECONDITION: exclusive_max > 0 (zero is programmer error)
    uint32_t bounded(uint32_t exclusive_max);
    
    // === State management ===
    uint64_t state() const;           // Current internal state
    void restore_state(uint64_t s);   // Direct state restoration (save/load)
    void seed(uint64_t seed_value);   // O'Neill canonical initialization
    
    // === Serialization ===
    void serialize(std::vector<uint8_t>& out) const;
    static GameplayRng deserialize(const uint8_t* data);
};
```

### CRITICAL: next_u64() Sequencing

The `next_u64()` implementation MUST use explicit sequencing:

```cpp
// REQUIRED implementation shape
uint64_t next_u64() {
    uint64_t hi = next_u32();  // First draw → high bits
    uint64_t lo = next_u32();  // Second draw → low bits
    return (hi << 32) | lo;
}
```

The following are FORBIDDEN due to unsequenced evaluation:
```cpp
// FORBIDDEN: Unsequenced - compiler may evaluate in either order
return next_u32() | (static_cast<uint64_t>(next_u32()) << 32);
return (next_u32() << 32) | next_u32();
```

A golden test MUST pin this ordering with known-answer vectors.

### bounded() Contract

```cpp
// Lemire's fast unbiased bounded random (2019)
// PRECONDITION: range > 0
// DRAWS: 1+ (typically 1, rejection loop if unlucky)
uint32_t GameplayRng::bounded(uint32_t range) {
    // PROGRAMMER ERROR: range == 0
    if (range == 0) {
        // Implementation must fail explicitly here
        // Options: assert, throw, or trap
        // Do NOT silently return 0 or consume a draw
        assert(false && "bounded(0) is invalid");
    }
    
    uint64_t random = next_u32();  // Draw 1
    uint64_t product = random * static_cast<uint64_t>(range);
    uint32_t low = static_cast<uint32_t>(product);
    
    if (low < range) {
        uint32_t threshold = (-range) % range;  // 2^32 mod range
        while (low < threshold) {
            random = next_u32();  // Draw 2, 3, ... (rejection)
            product = random * static_cast<uint64_t>(range);
            low = static_cast<uint32_t>(product);
        }
    }
    return static_cast<uint32_t>(product >> 32);
}
```

**bounded(0) behavior**: Programmer error. Consumes 0 draws. Fails explicitly (assert/throw/trap). NOT silent.

### chance() — DEFERRED

The `chance(numerator, denominator)` API is **deferred** from this milestone.

**Reason**: If implemented via `bounded(denominator)`, it would consume 1+ draws (not exactly 1), which creates a contract mismatch with naive expectations. Crystal-derived mechanics don't need this primitive—they use `next_u8()` with explicit threshold comparisons.

**If implemented later**, the contract must be:
- `chance(n, 0)` → programmer error, 0 draws, explicit failure
- `chance(n, d)` where n > d → programmer error OR defined behavior (document which)
- `chance(n, d)` otherwise → 1+ draws (same as bounded)

**Crystal-derived mechanics**: Use `next_u8()` + source threshold logic directly.
**Native/mod mechanics**: May use `bounded()` directly with explicit awareness of 1+ draw consumption.

### Crystal Mechanics Translation

For exact Crystal formula translation, mechanics use raw byte output:

```cpp
// Crystal: call BattleRandom, compare against threshold
// Maps to: 1 draw via next_u8()
uint8_t roll = rng.next_u8();
bool crit = (roll < crit_threshold);

// Crystal: damage = damage * (217..255) / 255
// Maps to: 1+ draws (rejection loop)
uint8_t variation;
do {
    variation = rng.next_u8();
} while (variation < 217);
damage = damage * variation / 255;
```

The `next_u8()` call is the 8-bit gameplay draw used by Crystal-derived mechanics. It does NOT produce Crystal-identical byte sequences—only structurally equivalent consumption.


---

## 4. RNG Consumption Semantics

### Principle: Consumption Is Semantic Behavior

The number of RNG calls per operation is part of the game's mechanics, not an implementation detail. Changing call count changes all future outcomes.

### Explicit Consumption Documentation

Each mechanic must document its consumption:

| Mechanic | Draws Consumed | Condition |
|----------|----------------|-----------|
| Accuracy check | 0 or 1 | **0 if threshold==255** (guaranteed hit); 1 otherwise |
| Critical hit | 0 or 1 | 0 if move power == 0; 1 otherwise |
| Damage roll | 1+ | Rejection loop until ≥ 217 |
| Secondary effect | 1 | Only if effect can trigger |
| Speed tie | 1-4 | Branch-dependent |
| Flee | 0 or 1 | 0 if guaranteed escape; 1 otherwise |
| Wild DVs | 2 | Exactly 2 draws (one per DV byte) — **VERIFIED** |
| Wild item | 1 or 2 | 1 for 75% check; +1 if item assigned (8% Item2) — **VERIFIED** |
| AI move choice | 1+ | Per evaluation loop |

**CRITICAL FIX**: The accuracy check consumption was incorrectly documented as "1, Always" in earlier versions. Crystal explicitly skips the RNG call when `accuracy == 255` (source: `effect_commands.asm:BattleCommand_CheckHit`):

```asm
.skip_brightpowder
    ld a, b
    cp -1           ; Compare against 255
    jr z, .Hit      ; If accuracy == 255, SKIP RNG CALL entirely!
    
    call BattleRandom  ; Only called if accuracy < 255
    cp b
    jr nc, .Miss
```

This is the Gen 2 fix for the Gen 1 "1/256 miss on 100% moves" bug.

### IR Representation

Semantic mechanics IR should make RNG explicit and handle the threshold==255 case:

```cpp
// In SemanticScriptIR or MechanicsIR
enum class RngOp {
    RandomByte,           // Consume 1 draw, return 0-255
    RandomBounded,        // Consume 1+ draws, return 0..(n-1)
};

struct Sem_AccuracyCheck {
    uint8_t threshold;    // Crystal accuracy value (255 = guaranteed hit)
    // Consumes 0 RNG draws if threshold == 255
    // Consumes 1 RNG draw otherwise
    // Result: (threshold == 255) || (random_byte < threshold)
};

struct Sem_DamageRoll {
    // Consumes 1+ RNG draws (rejection loop)
    // Result: 217-255
};
```


### Runtime Execution

```cpp
void execute_accuracy_check(const Sem_AccuracyCheck& op, BattleState& battle) {
    // CRITICAL: Crystal skips RNG when threshold == 255 (Gen 2 bug fix)
    if (op.threshold == 255) {
        battle.last_move_hit = true;  // 0 draws consumed
        return;
    }
    uint8_t roll = battle.game_state.rng.next_u8();  // 1 draw consumed
    battle.last_move_hit = (roll < op.threshold);
}

void execute_damage_variation(const Sem_DamageVariation& op, BattleState& battle) {
    uint8_t multiplier;
    do {
        multiplier = battle.game_state.rng.next_u8();  // 1+ draws consumed
    } while (multiplier < 217);
    
    battle.current_damage = battle.current_damage * multiplier / 255;
}
```

### Drift Detection

Accidental RNG drift is detectable via:

1. **Golden fixtures**: Expected RNG state after known operation sequences
2. **Consumption counters**: Track total draws per test
3. **State snapshots**: Compare RNG state at checkpoints

---

## 5. Stream Policy

### Recommendation: Single Canonical Stream

**Decision**: One authoritative RNG stream for all gameplay.

**Rationale**:
- Multiple streams can hide call-order drift
- Crystal uses one stream per context (overworld or battle), not parallel streams
- Single stream makes consumption auditing trivial
- Network sync is simpler with one state

### Rejected Alternative: Typed Streams

Typed streams (overworld/battle/generation/AI) would:
- Require explicit seeding for each
- Hide cross-subsystem drift
- Complicate serialization
- Not match Crystal's actual model

### Exception: Presentation-Only RNG

A separate non-authoritative RNG MAY exist for:
- Particle effects
- Animation variation
- UI flourishes
- Audio timing jitter

**Rule**: If a random value can affect `GameState`, it MUST come from the canonical stream.

```cpp
struct RuntimeServices {
    GameplayRng& gameplay_rng;        // Canonical, serialized
    PresentationRng presentation_rng;  // Non-authoritative, not saved
};
```


---

## 6. Save/Replay/Network Requirements

### Serialization

```cpp
// Serialized state: exactly 8 bytes
struct SerializedRngState {
    uint64_t pcg_state;  // PCG internal state
};

void GameplayRng::serialize(std::vector<uint8_t>& out) const {
    // Little-endian, platform-independent
    for (int i = 0; i < 8; i++) {
        out.push_back(static_cast<uint8_t>(state_ >> (i * 8)));
    }
}

void GameplayRng::deserialize(const uint8_t* data) {
    state_ = 0;
    for (int i = 0; i < 8; i++) {
        state_ |= static_cast<uint64_t>(data[i]) << (i * 8);
    }
}
```

### Invariants

| Scenario | Requirement |
|----------|-------------|
| Save/Load | Serialized state continues sequence exactly |
| Save states | Full RNG state captured and restored |
| Replay | Same initial state + same inputs = same outputs |
| Regression tests | Deterministic with fixed seed |
| Multiplayer lockstep | Agreed seed, identical state progression |
| Rollback/resim | Restore state, replay produces identical results |

### Cross-Platform Guarantee

**INVARIANT**: Given state S and sequence of calls C, the resulting sequence of values V is identical on:
- All supported operating systems
- All supported compilers
- All CPU architectures
- 32-bit and 64-bit builds
- Debug and release builds

This is achieved by:
- No stdlib random functions
- No implementation-defined behavior
- Explicit integer arithmetic
- No floating-point in PRNG core

---

## 7. Initial Seeding

### Terminology: Seed vs State

| Term | Definition |
|------|------------|
| **seed** | Initialization input value (used once at new game) |
| **state** | Current 64-bit PCG internal state (evolves with each draw) |

These are distinct concepts:
- `seed(value)` → O'Neill canonical initialization → produces initial state
- `state()` → returns current internal state
- `restore_state(s)` → directly sets internal state (for save/load)

**CRITICAL**: Save/load restores raw current state via `restore_state()`. It MUST NOT pass serialized state back through `seed()`—that would apply the initialization sequence again and produce wrong continuation.

### Seeding vs. Evolution

- **Seeding**: Choosing the initial RNG state (one-time, may use external entropy)
- **Evolution**: Advancing RNG state during gameplay (deterministic, internal)

These are strictly separated. After seeding is complete, external entropy sources are never touched again.

### External Entropy Boundary

**Permitted (one-time only)**:
- `std::random_device` to generate new-game seed
- System entropy to generate multiplayer session seed

**FORBIDDEN after initialization**:
- `std::random_device`
- Wall clock / frame timing
- DIV/VBlank proxies
- Any non-deterministic input

Once `seed()` or `restore_state()` has been called, ALL gameplay randomness comes from deterministic PCG evolution. External entropy is completely outside authoritative RNG evolution.

### Seeding Policies by Context

| Context | Method | Notes |
|---------|--------|-------|
| New game | `seed(entropy)` | System entropy once, then deterministic |
| Deterministic tests | `seed(0xDEADBEEF)` | Fixed known seed |
| Replay | `seed(recorded_seed)` | From replay file header |
| Multiplayer | `seed(session_seed)` | Host generates, clients receive |
| Save resume | `restore_state(saved_state)` | Direct state restoration, NO re-seeding |

### Implementation

```cpp
void GameplayRng::seed_from_entropy() {
    // Use system entropy ONCE to generate initial seed
    std::random_device rd;
    uint64_t entropy = (static_cast<uint64_t>(rd()) << 32) | rd();
    seed(entropy);  // Uses O'Neill canonical initialization
}

void GameplayRng::seed(uint64_t seed_value) {
    // O'Neill canonical initialization
    state_ = 0;
    step();
    state_ += seed_value;
    step();
}

void GameplayRng::restore_state(uint64_t saved_state) {
    // Direct state restoration - NO initialization sequence
    state_ = saved_state;
}
```

### After Seeding

Once seeded, `std::random_device` is never touched again. All gameplay randomness comes from deterministic PCG evolution.

---

## 8. Modding

### Rule: All Gameplay RNG Through Canonical API

```cpp
// In RuntimeServices (available to mods)
struct RuntimeServices {
    GameplayRng& rng;  // THE canonical RNG
    // ...
};

// Mod code
int my_mod_random_effect(RuntimeServices& services, int max_value) {
    // bounded() consumes 1+ draws (rejection possible)
    return services.rng.bounded(max_value);
}
```

### Prohibited Patterns

```cpp
// FORBIDDEN: Private RNG instance
void bad_mod() {
    std::mt19937 my_rng(12345);  // Hidden state!
    int result = my_rng() % 100;  // Not serialized!
}

// FORBIDDEN: Direct random_device usage
void also_bad() {
    std::random_device rd;
    int result = rd() % 100;  // Non-deterministic!
}
```

### Lua API

```lua
-- ctx.util:random_byte() → 1 draw, returns 0-255
local roll = ctx.util:random_byte()
if roll < 128 then
    -- 50% chance
end

-- ctx.util:random_bounded(n) → 1+ draws, returns 0..(n-1)
-- PRECONDITION: n > 0
local slot = ctx.util:random_bounded(6)  -- 0-5, consumes 1+ draws
```

**Note**: The Lua `random_bounded()` consumes 1+ draws due to rejection sampling. Mod authors must account for this in consumption-sensitive logic.

### Mod Composition Ordering

When multiple mods consume RNG in the same tick:
1. Mod execution order is deterministic (alphabetical or explicit priority)
2. RNG consumption order follows execution order

This is documented as part of mod API contract.


---

## 9. Battle Compiler Integration

### Design Principle

The battle compiler translates Crystal mechanics structure, not Crystal RNG implementation.

**Crystal source formula**:
```asm
    ld a, b
    cp -1
    jr z, .Hit      ; threshold==255 skips RNG!
    
    call BattleRandom
    cp [accuracy_threshold]
    jr nc, .miss
```

**Compiled to**:
```cpp
Sem_AccuracyCheck { threshold }
```

**Runtime executes**:
```cpp
if (threshold == 255) {
    hit();  // 0 draws
} else {
    uint8_t roll = gameplay_rng.next_u8();  // 1 draw
    if (roll >= threshold) { miss(); }
}
```

### MechanicsIR Random Operations

```cpp
// Battle mechanics IR preserves RNG call structure
struct Sem_AccuracyCheck {
    uint8_t threshold;
    // Semantics: 
    //   if threshold == 255: hit, consume 0 draws (Gen 2 fix!)
    //   else: consume 1 draw, hit if draw < threshold
};

struct Sem_CriticalCheck {
    CritStage stage;
    // Semantics: consume 1 draw if power > 0, crit if draw < threshold[stage]
};

struct Sem_DamageVariation {
    // Semantics: consume 1+ draws until draw >= 217
    // Result: multiplier in range 217-255
};

struct Sem_SecondaryEffect {
    uint8_t chance;  // e.g., 30 for 30%
    EffectId effect;
    // Semantics: consume 1 draw, trigger if draw < chance
};
```

---

## 10. Crystal Quirk Policy

### Classification Framework

| Quirk Type | Policy | Example |
|------------|--------|---------|
| Threshold semantics | Preserve | Secondary effect 1/256 miss (BattleCommand_EffectChance) |
| Formula quirks | Preserve | Specific damage calculations |
| Call ordering | Preserve | Branch-dependent consumption |
| Hardware entropy | Do not preserve | DIV timing unpredictability |
| Link protocol | Do not preserve | 10-byte synchronized PRNG |


### Decision Procedure

For each quirk, ask:
1. Does it affect probability distribution? → Preserve threshold
2. Does it affect call ordering? → Preserve branching
3. Is it caused by hardware timing? → Do not preserve
4. Is it a synchronization mechanism? → Replace with native sync

### Specific Decisions

| Quirk | Decision | Reason |
|-------|----------|--------|
| 100% accuracy skip | **Preserve (zero draws)** | Gen 2 intentional fix—threshold==255 must NOT call RNG |
| 1/256 secondary effect miss | Preserve | BattleCommand_EffectChance threshold semantics (separate from accuracy) |
| Damage range 217-255 | Preserve | Formula specification |
| Speed tie 50% | Preserve | Probability specification |
| Quick Claw multi-roll | Preserve | Call ordering |
| DIV-based encounter timing | Do not preserve | Hardware entropy |
| VBlank RNG churn | Do not preserve | No gameplay effect |
| Link 10-byte PRNG | Do not preserve | Synchronization mechanism |

### CRITICAL CORRECTION: Gen 1 vs Gen 2 Accuracy Behavior

**Gen 1 Bug**: Accuracy check always calls RNG and compares `roll < threshold`. When threshold=255, roll=255 fails (1/256 miss on "100% accurate" moves).

**Gen 2 Fix**: Crystal explicitly checks `cp -1` (255) BEFORE calling BattleRandom and jumps directly to Hit, consuming ZERO RNG draws.

**Enginemon**: Must match Gen 2 behavior. Implementing Gen 1 behavior would:
1. Reintroduce a bug that was fixed 25+ years ago
2. Break consumption count for all downstream mechanics
3. Cause golden fixture / state hash divergence from real Crystal

---

## 11. Current RngState Replacement

### Current Implementation Analysis

```cpp
// Current (engine/include/engine/core/game_state.hpp)
struct RngState {
    uint64_t seed = 0;
    uint64_t state = 0;
    
    uint32_t next() {
        state = state * 1664525 + 1013904223;  // Numerical Recipes LCG
        return static_cast<uint32_t>(state);
    }
};
```

**Issues**:
1. Numerical Recipes LCG has poor statistical quality
2. Comment claims Crystal compatibility (inaccurate)
3. Missing `next_u8()`, `bounded()`, `chance()` primitives
4. No explicit consumption documentation
5. `set_seed()` doesn't use proper initialization


### Proposed Replacement

```cpp
// Proposed (engine/include/engine/core/game_state.hpp)
class GameplayRng {
public:
    // O'Neill canonical seeding (new game / deterministic test)
    void seed(uint64_t s) {
        state_ = 0;
        step();
        state_ += s;
        step();
    }
    
    // Direct state restoration (save/load ONLY - no re-initialization)
    void restore_state(uint64_t s) { state_ = s; }
    
    void seed_from_entropy();
    
    // Core advance (PCG-XSH-RR) - 1 draw
    uint32_t next_u32() {
        uint64_t old = state_;
        state_ = old * 6364136223846793005ULL + 1442695040888963407ULL;
        
        uint32_t xorshifted = static_cast<uint32_t>(((old >> 18) ^ old) >> 27);
        uint32_t rot = static_cast<uint32_t>(old >> 59);
        return (xorshifted >> rot) | (xorshifted << ((-rot) & 31));
    }
    
    // 8-bit draw for Crystal-derived mechanics (1 draw)
    uint8_t next_u8() { return static_cast<uint8_t>(next_u32()); }
    
    // 64-bit draw with defined sequencing (2 draws)
    uint64_t next_u64() {
        uint64_t hi = next_u32();  // Draw 1 → high bits
        uint64_t lo = next_u32();  // Draw 2 → low bits
        return (hi << 32) | lo;
    }
    
    // Unbiased bounded (Lemire's method) - 1+ draws
    // PRECONDITION: range > 0
    uint32_t bounded(uint32_t range);
    
    // State access
    uint64_t state() const { return state_; }
    
    // Serialization
    void serialize(std::vector<uint8_t>& out) const;
    static GameplayRng deserialize(const uint8_t* data);

private:
    uint64_t state_ = 0;
    
    void step() {
        state_ = state_ * 6364136223846793005ULL + 1442695040888963407ULL;
    }
};
```

### Migration Strategy

1. **Save format version increment**: Old saves get migrated
2. **Migration path**: `old_state → new_state = seed(old_state)`
3. **Test suite update**: All RNG tests use new API
4. **Golden fixture update**: Recalculate expected values with new PRNG

### Backward Compatibility

Old saves can be loaded:

```cpp
GameplayRng migrate_from_old(uint64_t old_lcg_state) {
    // Use old state as seed for new PRNG
    GameplayRng rng;
    rng.seed(old_lcg_state);
    return rng;
}
```

This changes future RNG values but allows old saves to load.


---

## 12. Determinism Verification

### Adversarial Test Plan

**Test 1: Sequence Determinism**
```cpp
TEST(rng_sequence_determinism) {
    GameplayRng rng1, rng2;
    rng1.seed(0x12345678);
    rng2.seed(0x12345678);
    
    for (int i = 0; i < 10000; i++) {
        ASSERT_EQ(rng1.next_u32(), rng2.next_u32());
    }
}
```

**Test 2: Serialize/Deserialize Continuity**
```cpp
TEST(rng_serialize_continuity) {
    GameplayRng rng;
    rng.seed(0xABCDEF);
    
    // Advance partway
    for (int i = 0; i < 100; i++) rng.next_u32();
    
    // Serialize
    std::vector<uint8_t> data;
    rng.serialize(data);
    
    // Record next values
    uint32_t expected[100];
    for (int i = 0; i < 100; i++) expected[i] = rng.next_u32();
    
    // Deserialize
    GameplayRng restored = GameplayRng::deserialize(data.data());
    
    // Verify continuation
    for (int i = 0; i < 100; i++) {
        ASSERT_EQ(restored.next_u32(), expected[i]);
    }
}
```

**Test 3: Frame Rate Independence**
```cpp
TEST(rng_frame_rate_independent) {
    // Simulate 60Hz
    GameplayRng rng60;
    rng60.seed(0x1234);
    simulate_ticks(rng60, 60, /*ticks=*/60);  // 1 second at 60 ticks/sec
    
    // Simulate 144Hz (same tick count)
    GameplayRng rng144;
    rng144.seed(0x1234);
    simulate_ticks(rng144, 144, /*ticks=*/60);  // Same 60 ticks
    
    // RNG state must match
    ASSERT_EQ(rng60.state(), rng144.state());
}
```

**Test 4: Save/Resume Exact Continuation**
```cpp
TEST(rng_save_resume_exact) {
    GameState gs;
    gs.rng.seed(0x9999);
    
    // Play for a while
    for (int i = 0; i < 500; i++) {
        gs.rng.next_u8();
    }
    
    // Save
    auto save_data = gs.serialize();
    uint64_t state_at_save = gs.rng.state();
    
    // Continue playing
    uint32_t values_after_save[100];
    for (int i = 0; i < 100; i++) {
        values_after_save[i] = gs.rng.next_u32();
    }
    
    // Load
    GameState loaded;
    loaded.deserialize(save_data);
    
    // State must match save point
    ASSERT_EQ(loaded.rng.state(), state_at_save);
    
    // Continuation must match
    for (int i = 0; i < 100; i++) {
        ASSERT_EQ(loaded.rng.next_u32(), values_after_save[i]);
    }
}
```


**Test 5: Bounded Sampling Unbiased**
```cpp
TEST(rng_bounded_unbiased) {
    GameplayRng rng;
    rng.seed(0x5555);
    
    const uint32_t RANGE = 7;  // Prime, exposes modulo bias
    const int SAMPLES = 700000;
    int counts[RANGE] = {0};
    
    for (int i = 0; i < SAMPLES; i++) {
        counts[rng.bounded(RANGE)]++;
    }
    
    int expected = SAMPLES / RANGE;  // 100000
    for (int i = 0; i < RANGE; i++) {
        // Within 1% of expected (chi-squared would be more rigorous)
        ASSERT_TRUE(counts[i] > expected * 0.99);
        ASSERT_TRUE(counts[i] < expected * 1.01);
    }
}
```

**Test 5b: bounded(0) Explicit Failure**
```cpp
TEST(rng_bounded_zero_fails) {
    GameplayRng rng;
    rng.seed(0x6666);
    uint64_t state_before = rng.state();
    
    // bounded(0) must fail explicitly and consume 0 draws
    ASSERT_THROWS(rng.bounded(0));  // Or ASSERT_DEATH for assert()
    
    // State must be unchanged (0 draws consumed)
    ASSERT_EQ(rng.state(), state_before);
}
```

**Test 5c: next_u64 Ordering**
```cpp
TEST(rng_next_u64_ordering) {
    GameplayRng rng;
    rng.seed(0x7777);
    
    // Get u64 via combined call
    uint64_t combined = rng.next_u64();
    
    // Reset and get same values via separate calls
    rng.seed(0x7777);
    uint64_t hi = rng.next_u32();  // First draw
    uint64_t lo = rng.next_u32();  // Second draw
    uint64_t manual = (hi << 32) | lo;
    
    // Must match: first draw = high bits, second draw = low bits
    ASSERT_EQ(combined, manual);
}
```

**Test 6: Accuracy Check Zero-Draw on Threshold 255**
```cpp
TEST(accuracy_check_255_zero_draws) {
    GameplayRng rng;
    rng.seed(0x1111);
    uint64_t state_before = rng.state();
    
    // Accuracy check with threshold 255: MUST consume 0 draws
    Sem_AccuracyCheck acc_255{255};
    BattleState battle;
    battle.game_state.rng = rng;
    
    execute_accuracy_check(acc_255, battle);
    
    // RNG state must be UNCHANGED
    ASSERT_EQ(battle.game_state.rng.state(), state_before);
    ASSERT_TRUE(battle.last_move_hit);  // Always hits
}

TEST(accuracy_check_254_one_draw) {
    GameplayRng rng;
    rng.seed(0x2222);
    uint64_t state_before = rng.state();
    
    // Accuracy check with threshold 254: MUST consume exactly 1 draw
    Sem_AccuracyCheck acc_254{254};
    BattleState battle;
    battle.game_state.rng = rng;
    
    execute_accuracy_check(acc_254, battle);
    
    // RNG state must have advanced
    ASSERT_NE(battle.game_state.rng.state(), state_before);
}
```

**Test 7: Consumption Count Verification**
```cpp
TEST(rng_consumption_documented) {
    GameplayRng rng;
    rng.seed(0x1111);
    uint64_t state_before = rng.state();
    
    // Critical check (power > 0): exactly 1 draw
    state_before = rng.state();
    bool crit = simulate_crit_check(rng, CritStage::Normal, /*power=*/50);
    ASSERT_NE(state_before, rng.state());
    
    // Critical check (power = 0): exactly 0 draws
    state_before = rng.state();
    crit = simulate_crit_check(rng, CritStage::Normal, /*power=*/0);
    ASSERT_EQ(state_before, rng.state());  // Not consumed
}
```

### Determinism Hash Policy

**Requirement**: GameplayRng state MUST be represented in canonical authoritative serialization.

**Correct approach**: The eventual authoritative determinism hash should derive from canonical serialized authoritative state (GameState.serialize()), rather than maintaining a second manual field list.

**Rationale**:
- Checkpoint hashing of full canonical state is the correctness-first design
- A manual `state_hash()` that lists fields separately will diverge from serialization
- Per-tick/incremental hashing may be profiled and designed later if performance requires it

**NOT in this RNG milestone**:
- Do not expand the current narrow diagnostic state_hash()
- Do not create a parallel field list for hashing
- Defer hash design to the serialization/replay milestone


---

## 13. Wild Encounter RNG Verification

The following consumption counts were verified against `pokecrystal/engine/battle/core.asm` (lines ~6030-6100):

### Wild DVs — VERIFIED: 2 draws

```asm
.GenerateDVs:
    call BattleRandom      ; Draw 1 → high nibbles (Atk/Def)
    ld b, a
    call BattleRandom      ; Draw 2 → low nibbles (Spd/Spc)
    ld c, a
```

DVs are 2 bytes total: one byte for Atk/Def DVs (high nibbles), one byte for Spd/Spc DVs. Each byte requires one `BattleRandom` call. **Exactly 2 draws, always.**

### Wild Item — VERIFIED: 1 or 2 draws (branch-dependent)

```asm
; First check: 75% chance to have no item
    call BattleRandom      ; Draw 1
    cp 75 percent + 1      ; = 192
    jr c, .UpdateItem      ; < 192 → no item, DONE (1 draw only)
    
; If 75% check passes, 8% chance for Item2
    call BattleRandom      ; Draw 2
    cp 8 percent + 1       ; = 21
    ld a, [wBaseData + BASE_ITEM1]
    jr nc, .UpdateItem     ; >= 21 → Item1
    ld a, [wBaseData + BASE_ITEM2]  ; < 21 → Item2
```

- **75% of encounters**: 1 draw (no item assigned)
- **25% of encounters**: 2 draws (item assigned, either Item1 or Item2)

| Mechanic | Draws | Condition | Status |
|----------|-------|-----------|--------|
| Wild DVs | 2 | Always exactly 2 | ✅ VERIFIED |
| Wild Item | 1 or 2 | 1 if no item (75%), 2 if item assigned (25%) | ✅ VERIFIED |

---

## Final Architecture

```
GameState
└── GameplayRng (single canonical instance)
    ├── Algorithm: PCG-XSH-RR (64-bit state, 32-bit output)
    ├── Seeding: O'Neill canonical (state=0; step; state+=seed; step)
    ├── State: uint64 internal, deterministic integer-only evolution
    ├── Serialized: 8 bytes (little-endian uint64)
    └── API:
        ├── next_u32()              // 1 draw
        ├── next_u8()               // 1 draw
        ├── next_u64()              // 2 draws (hi=first, lo=second)
        ├── bounded(n)              // 1+ draws, n>0 required
        ├── state()                 // Read current state
        ├── restore_state(s)        // Direct state restoration (save/load)
        ├── seed(s)                 // O'Neill initialization (new game)
        └── serialize/deserialize   // Canonical persistence

RuntimeServices
├── gameplay_rng → GameState.rng    // Authoritative, serialized
└── presentation_rng                 // Optional, non-authoritative

Crystal-Derived Mechanics
├── Preserve formulas               // Thresholds, ranges, calculations
├── Preserve threshold comparisons  // roll < threshold semantics
├── Preserve branch structure       // Conditional RNG consumption
├── Preserve consumption ordering   // Which branch consumes how many draws
└── NOT preserved: exact RNG values // Native PCG, not Crystal DIV/VBlank

Intentionally NOT Preserved
├── DIV register entropy
├── VBlank RNG churn
├── hRandomAdd/Sub hardware state
└── Link-cable 10-byte PRNG implementation

Mod API
└── ctx.util:random_byte()          // 1 draw
└── ctx.util:random_bounded(n)      // 1+ draws

Battle Compiler
├── Crystal bytecode → MechanicsIR  // Preserves random operations
├── Sem_AccuracyCheck { threshold } // 0 or 1 draw based on threshold
├── Sem_DamageVariation { }         // 1+ draws (rejection loop)
└── Runtime → gameplay_rng.next_u8()// Native execution
```


---

## Summary Table

| Aspect | Decision |
|--------|----------|
| PRNG | PCG-XSH-RR (64-bit state, 32-bit output) |
| Seeding | O'Neill canonical initialization |
| State size | 8 bytes (uint64) |
| next_u32() | 1 draw |
| next_u8() | 1 draw (8-bit draw for Crystal-derived mechanics) |
| next_u64() | 2 draws (first=high, second=low) |
| bounded(n) | 1+ draws (Lemire unbiased, n>0 required) |
| bounded(0) | Programmer error, 0 draws, explicit failure |
| chance(n,d) | **DEFERRED** (would be 1+ draws if via bounded) |
| Stream count | One canonical authoritative stream |
| Crystal formulas | Preserved exactly |
| Crystal call ordering | Preserved exactly |
| Crystal RNG values | NOT preserved (native PCG, not Crystal DIV) |
| Gen 2 accuracy fix | Preserved (threshold==255 → 0 draws) |
| Serialization | Little-endian uint64 via restore_state() |
| Mod RNG | Must use canonical API |
| Presentation RNG | Optional separate non-authoritative |

---

## Corrections from Review

1. **Accuracy check consumption**: Fixed from "1, Always" to "0 or 1" based on `effect_commands.asm` verification. Crystal's `cp -1; jr z, .Hit` explicitly skips `BattleRandom` when threshold==255.

2. **PCG seeding**: Changed from direct assignment to O'Neill canonical initialization (`state=0; step(); state+=seed; step();`) per reference implementation.

3. **Wild DV/item draws**: Verified against `core.asm` lines ~6030-6100:
   - Wild DVs: **2 draws** (exactly 2 `BattleRandom` calls, one per DV byte)
   - Wild Item: **1 or 2 draws** (1 for 75% no-item check; +1 if item assigned)

4. **Secondary effect 1/256 bug**: Verified in `effect_commands.asm` that `BattleCommand_EffectChance` does NOT have the threshold==255 skip. Source comment explicitly states: `; BUG: Moves with a 100% secondary effect chance will not trigger it in 1/256 uses`. This bug IS real and correctly preserved (distinct from accuracy check which was patched).

5. **chance() contract**: Fixed from "consumes exactly one draw" to **DEFERRED**. If implemented via `bounded()`, it would consume 1+ draws. Crystal-derived mechanics use `next_u8()` directly.

6. **bounded(0)**: Defined as programmer error, 0 draws consumed, explicit failure required.

7. **next_u64() sequencing**: Added explicit required implementation shape with first draw = high bits, second draw = low bits.

8. **API naming**: Changed `set_state()` to `restore_state()` to clarify it's for save/load restoration, not re-seeding.

9. **State hash**: Removed hand-written `state_hash()` recommendation. Deferred to serialization milestone; canonical hash should derive from serialized state.

10. **Crystal compatibility terminology**: Clarified that "Crystal-faithful" means formula/consumption preservation, NOT RNG sequence reproduction.

---

## Implementation Checklist

The following tasks are required when implementing this design:

- [ ] Replace Numerical Recipes LCG with PCG-XSH-RR
- [ ] Migrate all authoritative RNG callers to new API
- [ ] Remove inaccurate "pokecrystal RNG" comments from code
- [ ] Preserve GameState ownership of RNG
- [ ] Update save schema if required (version increment)
- [ ] Migrate legacy serialized RNG state explicitly (old saves)
- [ ] Add PCG known-answer test vectors
- [ ] Test next_u64() ordering (first=high, second=low)
- [ ] Test bounded(0) explicit failure
- [ ] Test bounded(1) returns 0 always
- [ ] Test serialize/restore_state continuation
- [ ] Test deterministic Pokémon creation (DVs, item)
- [ ] Run canonical verifier (golden fixtures)

---

**End of Design Audit — Implementation Ready**
