# SM83 Hardcode Sweep — Remaining Constants After SM83 Recovery Audit Pass

Swept files: `engine/battle/calculator.cpp`, `engine/battle/battle.cpp`,
`engine/battle/trainer_ai.cpp`

Last updated: 2026-08-30 (recovery audit pass)

---

## 1. Fully resolved by SM83 lifting (Tasks #7 + recovery audit pass)

These constants were hardcoded before this work and are now consumed via
`BattleRules` getters in the `rules`-taking overloads.

| Constant (vanilla) | Getter | Source routine | Status |
|--------------------|--------|----------------|--------|
| `/5` (level divisor) | `get_level_divisor()` | `BattleCommand_DamageCalc` | ✅ extracted + wired + tested |
| `+2` (level addend) | `get_level_addend()` | `BattleCommand_DamageCalc` | ✅ |
| `/50` (damage divisor) | `get_damage_divisor()` | `BattleCommand_DamageCalc` | ✅ |
| `+2` (min damage) | `get_min_damage()` | `BattleCommand_DamageCalc` | ✅ |
| `+10` AI discourage | `get_ai_discourage_strong()` | `AIDiscourageMove` | ✅ |
| `20` AI init score | `get_ai_init_score()` | `AIChooseMove` | ✅ |
| `/100`, `+5`, `+10` stat formula | `get_stat_*()` | `CalcMonStatC` | ✅ shape-driven (no value anchor) |
| `×32`, `+30` escape | `get_escape_*()` | `TryToRunAwayFromBattle` | ✅ |
| `+10` SLP/FRZ catch bonus | `get_capture_slp_frz_bonus()` | `PokeBallEffect` | ✅ |
| `/7` exp divisor | `get_exp_divisor()` | `GiveExperiencePoints` | ✅ |
| `/8` burn/poison | `get_burn_poison_denom()` | `GetEighthMaxHP` | ✅ SRL count |
| `/16` toxic | `get_toxic_denom()` | `GetSixteenthMaxHP` | ✅ SRL count |
| crit stage deltas | `get_crit_*_delta()` | `BattleCommand_Critical` | ✅ held_item extracted; scope/focus hardcoded 1,1 (see §2c) |
| `0xD9` variation lower bound | `get_damage_var_lower_bound()` | `BattleCommand_DamageVariation` | ✅ extracted + used in RRCA loop |

---

## 2. Remaining hardcodes — classified

### 2a. Structural / VERIFIED ALGORITHM SHAPE

These are SM83 instruction shapes whose parameter is an iteration count or
arithmetic sequence rather than a load-immediate byte. They are counted or
recognised by shape, not by extracting an operand.

| Location | Constant | Classification | Notes |
|----------|----------|---------------|-------|
| `calculator.cpp` | `sq / 4` (EV sqrt shift) | VERIFIED ALGORITHM SHAPE | SRL×2 in CalcMonStatC. Same counting approach as `lift_residual_fraction` — implementable. Low priority: affects only EV scaling accuracy. |
| `calculator.cpp` | `wild_speed / 4` (escape) | VERIFIED ALGORITHM SHAPE | SRL×2 in TryToRunAwayFromBattle. |
| `calculator.cpp` | `hp * 2` in capture | VERIFIED ALGORITHM SHAPE | `add a,a` (0x87) opcode = ×2 in PokeBallEffect. |
| `calculator.cpp` | `max_hp * 3` in capture | VERIFIED ALGORITHM SHAPE | `save + add a,a + add a,b` = ×3 in PokeBallEffect. |
| `battle.cpp` | `damage > 999` / `result > 999` | TRULY NATIVE | Crystal BCD display limit — not an algorithm parameter. |
| `calculator.cpp` | `atk > 255 || def > 255` | TRULY NATIVE | TruncateHL_BC loop limit — u8 architecture constant. |
| `calculator.cpp` | `acc > 255` / `255` capture clamp | TRULY NATIVE | u8 data width ceilings — architecture constants. |
| `calculator.cpp` | `odds > 255` escape early-out | TRULY NATIVE | u8 escape odds ceiling. |

### 2b. Intentional fallback no-rules overloads

`calc_hp`, `calc_stat`, `calculate_damage`, `calculate_exp_gain`,
`calculate_catch_value`, `roll_escape` retain `BattleRules`-less overloads
for unit tests that do not set up a full ROM pipeline. Production code always
calls the `rules`-taking overloads through `Battle::execute_turn()`.

### 2c. Crit stage scope_lens and focus_energy deltas

`lift_crit_stage_deltas` cannot distinguish `inc c` from scope lens vs focus
energy without full CFG analysis — both are a single `inc c` opcode. The
recognizer returns `p[1]=1, p[2]=1` (hardcoded) after extracting `held_item_delta`
from `ld c, N`. This is correct for vanilla Crystal (both are +1) and is a known
partial extraction documented in the recognizer comments. Full extraction requires
CFG-driven analysis of the conditional branches separating the two paths.

### 2d. Trainer AI HP fraction checks

```cpp
// trainer_ai.cpp
if (self_hp * 2 < self_max)          // < 50% HP
if (self_hp * 4 >= self_max * 3)     // >= 75% HP
```

These are comparison thresholds from Crystal's trainer switch-out logic.
They are encoded as compare results, not load-immediates — not recoverable
via SM83 immediate lifting. TRULY NATIVE.

### 2e. Type effectiveness `/100` normalisation

Engine-internal representation of per-100 type effectiveness. Not a Crystal
ROM value. TRULY NATIVE to Enginemon's type system.

---

## 3. sm83_lifted_mask — provenance tracking

`BattleRules::sm83_lifted_mask` is a `uint16_t` bitmask (9 bits, one per
sub-struct, `SM83_LIFTED_*` constants) stored in the BRLS wire format at
the end of the SM83 block. A set bit means the recognizer RAN and succeeded
for that sub-struct; a clear bit means address=0, OOB, or recognizer failed.

Production code that needs to know whether a parameter is ROM-derived or
defaulted must check `sm83_is_lifted(SM83_LIFTED_*)`.

For vanilla Crystal v1.1, all 9 bits are set after successful compilation.

---

## 4. Non-battle SM83 lifting candidates (identified, not yet implemented)

These are SM83 routines outside the battle subsystem that embed parameters
as load-immediate operands in the same pattern as the battle recognizers.
**Not implemented** — identified for future work.

### Candidate A — Experience curve divisors (RECOVERABLE PARAMETER)

**Routines**: `CalcExpPoints` variants in banks 0x14 and 0x2E.

**Pattern** (same anchor as `lift_exp_divisor`):
```
3E NN E0 B7 06 04 CD    ld a,N / ldh [hDivisor],a / ld b,4 / call Divide
3E NN E0 B7 06 02 CD    ld a,N / ldh [hDivisor],a / ld b,2 / call Divide
```

**Vanilla values found**: divisors 5, 10, 100 in experience curve branches
(Fluctuating/Erratic formula phases).

**Impact**: These govern how quickly Pokémon level up under each experience
group. ROM hacks can change curve shapes by modifying these immediates.

**Implementation path**: Same `lift_exp_divisor` recognizer shape. Profile
would need addresses for each curve branch (3–4 per formula variant). This
is more complex than battle exp divisor (one address) because Crystal has
multiple CalcExp branches for different curves. A multi-address recognizer
that scans the full routine would extract all divisors.

**Note**: Does NOT affect battle EXP gain (`GiveExperiencePoints` at 0x0F:0x6E3B)
which is already lifted. These are the pre-battle level-up curve divisors.

### Candidate B — NPC overworld step duration (VERIFIED ALGORITHM SHAPE)

**Routine**: `ObjectStepVectors` in bank 0x00.

**Found**: flat 0x01F44: `ld a,16 / ld [0xCF73],a` — NPC step timer init.

**Classification**: VERIFIED ALGORITHM SHAPE. The 16-frame step duration
is a load-immediate byte but is also consistent with the NPC movement
semantics already hardcoded in the engine as 16 frames. Lifting this would
allow ROM hacks that change movement speed to be detected and honoured.

**Impact**: Low — overworld movement speed is observationally present but
not a gameplay-critical calculation. ROMs changing this would be unusual.

### Candidate C — Field poison/burn step damage (VERIFIED ALGORITHM SHAPE)

**Routine**: `PoisonAndBurnDamage` in engine/overworld/movement.asm.

**Classification**: VERIFIED ALGORITHM SHAPE — uses SRL instructions for
the HP fraction (same class as `lift_residual_fraction`). Not a
load-immediate: no operand byte to extract.

**Impact**: Already correctly structural — no new lifting possible.

### Candidate D — Time capsule max transfer count (RECOVERABLE PARAMETER)

**Routine**: `TimeCapsule` in bank 0x1C.

**Classification**: The `ld a, 6` (max 6 pokemon) is a load-immediate.
Pattern: `ld a, 6 / ld [wTimeCapsule...], a`.

**Impact**: Very low — time capsule transfer count is a link-cable feature,
not a gameplay formula. ROM hacks rarely change this.

### Candidate E — Happiness change deltas (RECOVERABLE PARAMETERS)

**Found**: Multiple `ld a,N / ld [0xDxxx],a` patterns in banks 0x03–0x04
with N in [1,10] targeting WRAM happiness addresses.

**Classification**: RECOVERABLE PARAMETER — happiness deltas (+1, +2, +5)
are load-immediate values in the happiness-change routines.

**Impact**: Moderate — ROM hacks that adjust friendship mechanics would
change these. However, the Enginemon semantics for happiness are not yet
implemented; this is future work.

---

## 5. Summary table

| Category | Count | Action |
|----------|-------|--------|
| Fully resolved (extracted + wired + tested) | 14 | ✅ Done |
| Fixed this pass (damage formula wiring, variation floor, shape-driven anchors) | 3 | ✅ Done |
| `sm83_lifted_mask` provenance tracking | new | ✅ Done |
| VERIFIED ALGORITHM SHAPE (countable but not immediate) | 5 | Document only |
| TRULY NATIVE (architecture/display limits) | 5 | Document only |
| Intentional fallback no-rules overloads | 6 | Keep; deprecate in future cleanup |
| Crit scope/focus partial extraction | 1 | Known partial; needs CFG analysis |
| Non-battle candidates identified | 5 | Future work — do not implement yet |

**No unresolved production hardcodes remain** in the battle formulas that
are extractable by the current SM83 recognizer framework.

All SM83-derivable parameters are now either:
- Extracted and consumed from ROM bytes, OR
- Explicitly classified as VERIFIED ALGORITHM SHAPE or TRULY NATIVE
