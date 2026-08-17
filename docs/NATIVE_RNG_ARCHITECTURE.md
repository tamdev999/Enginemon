# Enginemon Native Deterministic RNG Architecture

**Status**: Design Audit Complete — No Implementation  
**Date**: 2026-08-17  
**Prerequisite**: Crystal RNG Fidelity Audit (completed)

---

## Executive Summary

Enginemon adopts a single canonical deterministic PRNG that replaces Crystal's hardware entropy while preserving all gameplay probability mechanics and RNG consumption semantics.

**Recommended PRNG**: PCG-XSH-RR (32-bit output, 64-bit state)

**Core Principle**: Crystal formulas and call ordering are authoritative; the entropy source is modernized.

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

### Primitive Operations

```cpp
class GameplayRng {
public:
    // Core advance - always consumes one draw
    uint32_t next_u32();
    
    // Derived primitives - consume exactly one draw each
    uint64_t next_u64();      // Two next_u32() calls
    uint8_t next_u8();        // next_u32() masked to 8 bits
    
    // Unbiased bounded sampling - consumes 1+ draws (rejection)
    uint32_t bounded(uint32_t exclusive_max);
    
    // Probability check - consumes exactly one draw
    bool chance(uint32_t numerator, uint32_t denominator);
    
    // State management
    uint64_t get_state() const;
    void set_state(uint64_t state);
    void seed(uint64_t seed);      // Uses O'Neill initialization
    void seed_raw(uint64_t state); // Direct state assignment (for deserialization)
};
```

### Unbiased Bounded Sampling

Crystal uses modulo for bounded sampling, which has slight bias. Enginemon preserves Crystal's exact formulas where they exist, but provides an unbiased primitive for new mechanics:

```cpp
// Lemire's fast unbiased bounded random (2019)
uint32_t GameplayRng::bounded(uint32_t range) {
    uint64_t random = next_u32();
    uint64_t product = random * static_cast<uint64_t>(range);
    uint32_t low = static_cast<uint32_t>(product);
    
    if (low < range) {
        uint32_t threshold = (-range) % range;  // 2^32 mod range
        while (low < threshold) {
            random = next_u32();
            product = random * static_cast<uint64_t>(range);
            low = static_cast<uint32_t>(product);
        }
    }
    return static_cast<uint32_t>(product >> 32);
}
```

### Crystal Compatibility Mode

For exact Crystal formula translation, mechanics use raw byte output:

```cpp
// Crystal: call BattleRandom, compare against threshold
uint8_t roll = rng.next_u8();
bool crit = (roll < crit_threshold);

// Crystal: damage = damage * (217..255) / 255
uint8_t variation;
do {
    variation = rng.next_u8();
} while (variation < 217);
damage = damage * variation / 255;
```

The `next_u8()` call maps directly to Crystal's `BattleRandom` return value.


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
    RandomChance,         // Consume 1 draw, return bool
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

### Seeding vs. Evolution

- **Seeding**: Choosing the initial RNG state (one-time, may use entropy)
- **Evolution**: Advancing RNG state during gameplay (deterministic)

These are strictly separated.

### Seeding Policies by Context

| Context | Seeding Policy |
|---------|----------------|
| Normal play | std::random_device or system entropy once at new game |
| Deterministic tests | Explicit fixed seed (e.g., 0xDEADBEEF) |
| Replay | Recorded seed from save/replay file |
| Multiplayer | Agreed session seed (host generates, clients receive) |
| Save resume | Deserialize saved RNG state |


### Implementation

```cpp
void GameplayRng::seed_from_entropy() {
    // Use system entropy ONCE to generate initial seed
    std::random_device rd;
    uint64_t entropy = (static_cast<uint64_t>(rd()) << 32) | rd();
    seed(entropy);  // Uses O'Neill canonical initialization
}

void GameplayRng::seed_deterministic(uint64_t seed_value) {
    seed(seed_value);  // Uses O'Neill canonical initialization
}

void GameplayRng::seed_from_save(const uint8_t* data) {
    deserialize(data);  // Direct state restoration, no re-seeding
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
    return services.rng.bounded(max_value);  // Uses canonical RNG
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
-- ctx.util.random() uses canonical RNG
local roll = ctx.util.random(1, 100)  -- Consumes 1 draw

-- ctx.util.random_chance(n, d) uses canonical RNG
if ctx.util:random_chance(30, 100) then
    -- 30% chance, consumes 1 draw
end
```

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
    // O'Neill canonical seeding
    void seed(uint64_t s) {
        state_ = 0;
        step();
        state_ += s;
        step();
    }
    
    // Direct state restoration (for deserialization only)
    void set_state(uint64_t s) { state_ = s; }
    
    void seed_from_entropy();
    
    // Core advance (PCG-XSH-RR)
    uint32_t next_u32() {
        uint64_t old = state_;
        state_ = old * 6364136223846793005ULL + 1442695040888963407ULL;
        
        uint32_t xorshifted = static_cast<uint32_t>(((old >> 18) ^ old) >> 27);
        uint32_t rot = static_cast<uint32_t>(old >> 59);
        return (xorshifted >> rot) | (xorshifted << ((-rot) & 31));
    }
    
    // Crystal-compatible byte (most common usage)
    uint8_t next_u8() { return static_cast<uint8_t>(next_u32()); }
    
    // Unbiased bounded (Lemire's method)
    uint32_t bounded(uint32_t range);
    
    // Probability check
    bool chance(uint32_t numerator, uint32_t denominator) {
        return bounded(denominator) < numerator;
    }
    
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

### State Hash Integration

RNG state should contribute to canonical simulation hash:

```cpp
uint64_t GameState::state_hash() const {
    uint64_t hash = 0;
    
    // Include RNG state
    hash ^= rng.state();
    hash = hash * 0x9E3779B97F4A7C15ULL;  // Mix
    
    // Include other state...
    hash ^= static_cast<uint64_t>(player.x) << 0;
    hash ^= static_cast<uint64_t>(player.y) << 8;
    // ...
    
    return hash;
}
```


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
    ├── Serialized: 8 bytes (little-endian uint64)
    └── API:
        ├── next_u32()                    // Core advance
        ├── next_u8()                     // Crystal-compatible byte
        ├── bounded(uint32_t)             // Unbiased range
        ├── chance(num, denom)            // Probability
        ├── state() / set_state()         // Direct access
        └── serialize() / deserialize()   // Persistence

RuntimeServices
├── gameplay_rng → GameState.rng         // Authoritative, serialized
└── presentation_rng                      // Optional, non-authoritative

Crystal Mechanics
├── Source-faithful formulas             // Thresholds, ranges, branching
├── Source-faithful call ordering        // Consumption per branch
├── Gen 2 accuracy fix                   // threshold==255 → 0 draws
└── Modern entropy source                // PCG instead of DIV

Mod API
└── ctx.util:random() → gameplay_rng     // All mods use canonical RNG

Battle Compiler
├── Crystal bytecode → MechanicsIR       // Preserves random operations
├── Sem_AccuracyCheck { threshold }      // 0 or 1 draw based on threshold
├── Sem_DamageVariation { }              // Rejection loop
└── Runtime → gameplay_rng.next_u8()     // Modernized execution
```


---

## Summary Table

| Aspect | Decision |
|--------|----------|
| PRNG | PCG-XSH-RR (64-bit state, 32-bit output) |
| Seeding | O'Neill canonical initialization |
| State size | 8 bytes |
| Primary output | next_u8() for Crystal compatibility |
| Bounded sampling | Lemire's unbiased method |
| Stream count | One canonical stream |
| Crystal formulas | Preserved exactly |
| Crystal call ordering | Preserved exactly |
| Crystal hardware entropy | Not reproduced |
| Gen 2 accuracy fix | **Preserved** (threshold==255 → 0 draws) |
| Serialization | Little-endian uint64 |
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

---

**End of Design Audit**
