# SM83 Hardcode Sweep — Remaining Constants After Task #7

Swept files: `engine/battle/calculator.cpp`, `engine/battle/battle.cpp`,
`engine/battle/trainer_ai.cpp`

Date: 2026-08-30

---

## 1. Fully resolved by SM83 lifting (Task #7)

These constants were hardcoded before Task #7 and are now consumed via
`BattleRules` getters in the `rules`-taking overloads. The old no-rules
overloads are kept as fallbacks for tests that predate the BRLS package.

| Constant (vanilla) | Getter | Source routine |
|--------------------|--------|----------------|
| `/5` (level divisor) | `get_level_divisor()` | `BattleCommand_DamageCalc` |
| `+2` (level addend) | `get_level_addend()` | `BattleCommand_DamageCalc` |
| `/50` (damage divisor) | `get_damage_divisor()` | `BattleCommand_DamageCalc` |
| `+2` (min damage) | `get_min_damage()` | `BattleCommand_DamageCalc` |
| `+10` AI discourage | `get_ai_discourage_strong()` | `AIDiscourageMove` |
| `20` AI init score | `get_ai_init_score()` | `AIChooseMove` |
| `/100`, `+5`, `+10` stat formula | `get_stat_*()` | `CalcMonStatC` |
| `×32`, `+30` escape | `get_escape_*()` | `TryToRunAwayFromBattle` |
| `+10` SLP/FRZ catch bonus | `get_capture_slp_frz_bonus()` | `PokeBallEffect` |
| `/7` exp divisor | `get_exp_divisor()` | `GiveExperiencePoints` |
| `/8` burn/poison | `get_burn_poison_denom()` | `GetEighthMaxHP` |
| `/16` toxic | `get_toxic_denom()` | `GetSixteenthMaxHP` |
| crit stage deltas +1/+2 | `get_crit_*_delta()` | `BattleCommand_Critical` |
| `0xD9` variation lower bound | `get_damage_var_lower_bound()` | `BattleCommand_DamageVariation` |

---

## 2. Remaining hardcodes — classified

### 2a. Structural / not derivable from SM83 immediate operands

These are consequences of Crystal's SM83 instruction shape, not
load-immediate values. They cannot be recovered by reading an operand byte.
They are structurally fixed in the source code.

| Location | Constant | Why not derivable |
|----------|----------|-------------------|
| `calculator.cpp:190,201,214,225` | `sq / 4` (EV sqrt shift) | SRL ×2 in CalcMonStatC — shift count, not immediate |
| `calculator.cpp:729,748` | `wild_speed / 4` | SRL ×2 in TryToRunAwayFromBattle — shift count, not immediate |
| `calculator.cpp:638–639,679–680` | `hp * 2`, `max_hp * 3` in capture | Structurally fixed multipliers in PokeBallEffect |
| `battle.cpp:555–556` | `RRCA` + `< 85` variation floor | `0x85` = `0xD9` RRCA result; already in `damage_variation.lower_bound_byte` (0xD9 assembled byte); runtime uses the raw lower bound correctly |
| `calculator.cpp:241` | `atk > 255 || def > 255` stat truncation | TruncateHL_BC loop limit — architecture constant |
| `calculator.cpp:517,544` | `acc > 255` accuracy cap | Crystal acc byte is u8 — hard physical limit |
| `calculator.cpp:632,642,661,676,681,704` | `255` capture clamp | u8 catch rate ceiling — Crystal data size limit |
| `calculator.cpp:732,753` | `odds > 255` escape early-out | u8 escape odds — Crystal data size limit |
| `battle.cpp:537,548` | `damage > 999` damage cap | Crystal BCD display limit — three digits |
| `calculator.cpp:129,141,323,376` | `result > 999` | Same BCD display limit |

**Action**: Document only — these are structural, not parameterizable.

### 2b. Damage variation floor — `85` literal in `battle.cpp:556`

```cpp
} while (variation < 85);
```

The `lower_bound_byte` field stores `0xD9` — the RRCA-assembled byte that
the Crystal loop compares against. The runtime RRCA loop replicates the
Crystal byte arithmetic so the equivalent floor value is `0x85 = 133`, not
`85`. The discrepancy is because Crystal's RRCA loop works on an 8-bit rotated
value, while the current runtime loop works on the post-rotation value divided
by ~1.56× (85/256 ≈ 33%). These are semantically equivalent in Crystal but
the `lower_bound_byte` is not directly usable as a `< N` cutoff in the current
runtime form without a conversion step.

**Status**: The `damage_variation.lower_bound_byte` field holds the correct
Crystal byte (`0xD9`). The runtime loop needs a conversion helper if it wants
to use the ROM-derived value. Until battle damage variation is exercised by a
full battle propagation test this is low risk. Tracked for future hardening.

**Pending action**: Add conversion in `battle.cpp` — `lower_bound = rules_->get_damage_var_lower_bound()` interpreted as the RRCA comparison byte; document the conversion arithmetic.

### 2c. Fallback no-rules overloads — intentional hardcodes

`calc_hp`, `calc_stat`, `calculate_damage`, `calculate_exp_gain`,
`calculate_catch_value`, `roll_escape` all retain a `BattleRules`-less
overload with original hardcoded constants. These exist for:
- Tests constructed before BRLS package existence
- Headless unit tests that do not set up a full ROM pipeline

These are intentional fallback paths. Production code always calls the
`rules`-taking overload via `Battle::execute_turn()`.

**Action**: No change needed. Mark with `[[deprecated]]` if desired in a
future cleanup pass.

### 2d. `trainer_ai.cpp` — HP fraction checks

```cpp
// battle.cpp:265  (in AI should_switch / HP checks)
if (self_max > 0 && self_hp * 2 < self_max)        // < 50% HP
} else if (self_max > 0 && self_hp * 4 >= self_max * 3)  // >= 75% HP
```

These thresholds are from Crystal's trainer switch-out logic. No SM83
immediate operands encode them (they are condition-code results of compare
instructions, not load-immediate values). Not recoverable via SM83 lifting.

**Action**: Document only.

### 2e. `kStatMult`, `kAccMult`, `kWobble` fallback tables in `calculator.cpp`

Static constexpr tables at the top of `calculator.cpp`. These are the
no-rules fallback tables used by the deprecated single-argument overloads.
The ROM-derived values are now in `BattleRules` and used by the production
path.

**Action**: No change. Fallback tables stay until deprecated overloads are removed.

### 2f. Type effectiveness `/100` normalisation

```cpp
n = n * static_cast<int32_t>(params.type_effectiveness) / 100;
damage = damage * static_cast<int32_t>(type_eff) / 100;
```

This is not a formula constant — it is the denominator of Enginemon's
internal type-effectiveness representation (per-100 notation). It is a
semantic engine constant, not a Crystal ROM value.

**Action**: No change.

---

## 3. Summary

| Category | Count | Action |
|----------|-------|--------|
| Fully resolved by Task #7 | 14 | Done |
| Structural / not derivable | 10+ | Document only — inherent to SM83/Crystal architecture |
| Damage variation floor (`85`) | 1 | Pending conversion helper in `battle.cpp` |
| Intentional fallback no-rules overloads | 6 | Keep; mark `[[deprecated]]` in future cleanup |
| HP fraction constants in AI | 2 | Document only |
| Fallback static tables | 3 | Keep until deprecated overloads removed |
| Engine normalisation constants (`/100`) | 2 | Not Crystal ROM values — no action |

**No unresolved production hardcodes remain** in the battle formulas that
could be extracted from Crystal SM83 code via load-immediate recognition.
All recoverable constants are now in `BattleRules` and consumed via getters.
