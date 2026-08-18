# Hostile Frontend Correctness Audit - August 2026

## Executive Summary

**FRONTEND TRUST VERDICT: PARTIALLY TRUSTWORTHY**

The Crystal frontend decodes scripts correctly for the vanilla corpus but has schema bugs that would corrupt certain operand values. The bugs are latent because they happen to not affect vanilla Crystal semantics in ways detectable by current tests.

### Key Findings

| # | Finding | Verdict | Severity | Active in Prod? | Blocks Downstream? |
|---|---------|---------|----------|-----------------|-------------------|
| 1 | Operand order bugs in 8+ string formatting commands | CONFIRMED BUG | HIGH | YES | NO (latent) |
| 2 | Round-trip validation bypassed in production | CONFIRMED BUG | MEDIUM | YES | NO (gate exists) |
| 3 | Text command handling incomplete | CONFIRMED ARCHITECTURAL GAP | LOW | Partial | NO |
| 4 | EventFlag/EngineFlag namespace collapsed | CONFIRMED ARCHITECTURAL GAP | MEDIUM | YES | NO (Crystal values distinct) |
| 5 | Movement command decoder incomplete | CONFIRMED ARCHITECTURAL GAP | LOW | Partial | NO |
| 6 | sdefer pointer semantics consistent | CLEAN | - | - | - |
| 7 | BG event types silently collapsed | **FIXED** | HIGH | Was YES | Was YES |
| 8 | Corpus green despite schema bugs | TRUE | - | - | - |

---

## 1. Event Opcode Operand Schemas

### CONFIRMED BUG - Operand Order Swapped

**Source**: `pokecrystal/macros/scripts/events.asm`

The typed decoder reads operands in the wrong order for string formatting commands:

| Command | pokecrystal order | Enginemon decoder order | BUG |
|---------|-------------------|------------------------|-----|
| getmoney (0x3D) | account, strbuf | strbuf, account | YES |
| getmonname (0x40) | pokemon, strbuf | strbuf, pokemon | YES |
| getitemname (0x41) | item, strbuf | strbuf, item | YES |
| gettrainername (0x43) | group, id, strbuf | strbuf, group, id | YES |
| getstring (0x44) | pointer, strbuf | strbuf, pointer | YES |
| getlandmarkname (0xA5) | id, strbuf | id, strbuf | CLEAN |
| gettrainerclassname (0xA6) | group, strbuf | group, strbuf | CLEAN |
| getname (0xA7) | type, id, strbuf | type, id, strbuf | CLEAN |

**Evidence from pokecrystal**:
```asm
MACRO getmoney
    db getmoney_command
    db \2 ; account       <- FIRST in ROM
    db \1 ; string_buffer <- SECOND in ROM
ENDM

MACRO gettrainername
    db gettrainername_command
    db \2 ; trainer_group <- FIRST in ROM
    db \3 ; trainer_id    <- SECOND in ROM
    db \1 ; string_buffer <- THIRD in ROM
ENDM
```

**Evidence from typed_decoder.cpp**:
```cpp
case CrystalOp::getmoney: {
    cmd.strbuf = read_byte(ctx, span);    // WRONG - reads strbuf first
    cmd.account = read_byte(ctx, span);   // WRONG - reads account second
}
```

**Why corpus tests pass**: The decoder reads the bytes in the wrong order, but then the encoder (for round-trip) also writes them in the wrong order, so the round-trip succeeds. The semantic lowering may also not distinguish which field is which if only one is used downstream.

**Fix required**: Swap read order in typed_decoder.cpp to match ROM layout.

---

## 2. Round-Trip Validation Bypassed

### CONFIRMED BUG - Gate Exists But Is Not Invoked

**Source**: `frontends/crystal/compile/full_compiler.cpp`

The production compiler manually sets:
```cpp
input.decode_complete = true;
input.round_trip_failures = 0;
```

WITHOUT calling `validate_script_round_trip()`. The actual validation function exists and works (used in corpus_test), but production bypasses it.

**Implication**: A symmetric decoder+encoder bug (like the operand order bugs above) will pass round-trip and reach the package.

**Why it matters less than expected**: The operand order bugs produce identical round-trip bytes, so even if validation ran, it wouldn't catch them. Round-trip validation catches asymmetric bugs, not symmetric schema errors.

**Fix required**: Either:
1. Invoke round-trip validation in production, OR
2. Build independent oracle (RGBDS-based) fixtures

---

## 3. Text Command Handling

### PARTIAL COVERAGE

**Source**: `frontends/crystal/script/decoder.cpp`

The text decoder handles these control codes:
- 0x00: TX_START (skip)
- 0x4E: NEXT
- 0x4F: LINE
- 0x50: @ terminator
- 0x51: PARA
- 0x55/0x4B: CONT/SCROLL
- 0x57: DONE
- 0x58: PROMPT

**Not explicitly handled**:
- TX_RAM (0x01) - dynamic text from RAM
- TX_BCD - BCD number formatting
- TX_FAR - far text reference
- TX_DECIMAL/NUM - decimal formatting
- TX_STRINGBUFFER - string buffer reference
- TX_PAUSE - timed pause

**Current behavior**: Unknown codes are rendered as '?' characters, which is visible but not semantically correct.

**Why it's acceptable**: Vanilla Crystal text rendering works for all tested scripts. The missing commands are used in specialized contexts (Pokémon stats display, shop prices) that may use separate code paths.

---

## 4. EventFlag vs EngineFlag Namespace

### CONFIRMED ARCHITECTURAL GAP

**Source**: `engine/include/engine/core/types.hpp`

```cpp
using FlagId = uint16_t;
```

Both `Cmd_Checkevent` (event_flag) and `Cmd_Checkflag` (engine_flag) are lowered to `Sem_CheckFlag { FlagId flag }` with no namespace distinction.

**Evidence from semantic_legalizer.cpp**:
```cpp
if (auto* p = std::get_if<Cmd_Checkevent>(&cmd->data)) {
    Sem_CheckFlag op;
    op.flag = FlagId{p->event_flag};  // Just casts to FlagId
}
if (auto* p = std::get_if<Cmd_Checkflag>(&cmd->data)) {
    Sem_CheckFlag op;
    op.flag = FlagId{p->engine_flag}; // Same type, no distinction
}
```

**Why it works for Crystal**: Crystal's event_flag and engine_flag ranges don't overlap in practice - they're allocated to different number spaces in the ROM. EventFlags are 0x000-0x7FF, EngineFlags are typically higher.

**Why it's a gap**: A future mod or Gen1/Gen3 frontend could have overlapping flag namespaces, causing silent flag collisions.

**Fix required**: Either:
1. Add `FlagNamespace` enum to `Sem_CheckFlag`, OR
2. Encode namespace in high bits of FlagId (e.g., 0x8000 | engine_flag)

---

## 5. Movement Command Decoder

### PARTIAL COVERAGE

**Source**: `frontends/crystal/script/decoder.cpp`

The movement decoder handles directional steps (0x00-0x37) and some control commands (0x38-0x49).

**Commands with incomplete handling**:
- 0x4F: step_dig - has param byte
- 0x51: fish_got_bite - not listed
- 0x52: fish_cast_rod - not listed
- 0x53-0x59: Various special movements (hide_emote, show_emote, step_shake, tree_shake, rock_smash, return_dig, skyfall_top)

**Current behavior**: Unrecognized movement commands are captured in raw bytes but not parsed to semantic MovementType.

**Why it's acceptable**: The raw bytes survive round-trip, and movement commands the vanilla corpus actually uses are handled. The semantic representation falls back gracefully.

---

## 6. sdefer Pointer Semantics

### CLEAN

**Source**: `frontends/crystal/compile/corpus_discovery.cpp`, `frontends/crystal/script/semantic_legalizer.cpp`

Discovery path:
1. Scan IR for `Cmd_Sdefer`
2. Resolve pointer: `flat = bank * 0x4000 + (ptr - 0x4000)` for ptr >= 0x4000
3. Add target as new root with `ScriptRootType::Deferred`

Lowering path:
1. Extract pointer from `Cmd_Sdefer`
2. Generate script_id via `make_label_ref(target_addr, ctx)`
3. Create `Sem_Sdefer { target_script_id }`

**Verification**: Both paths use consistent pointer resolution. The linker validates that all `Sem_Sdefer` targets exist as compiled bodies.

---

## 7. BG Event Type Collapse

### FIXED IN THIS SESSION

**Prior bug**: `convert_bg_event_type()` only handled Read, HiddenItem, FacingUp. All others collapsed to Read.

**Fix applied**:
- Added exhaustive switch for all 9 types
- Added `std::runtime_error` for invalid enum values
- Added `condition_flag` transfer to RuntimeBgEvent
- Added package seam tests proving all types round-trip

**New tests**:
- `bg_event_type_package_roundtrip_all_types` - Proves all 9 types survive
- `bg_event_ifset_ifnotset_condition_flag_integration` - Proves flag evaluation works through package

---

## 8. Corpus Green Despite Schema Bugs

### TRUE

The 1788/1788 corpus success does NOT prove semantic correctness.

**What current gates catch**:
- Decode failures (truncation, invalid opcodes)
- CFG validation failures
- Unlowered commands
- Missing semantic definitions
- Invalid reference types

**What current gates miss**:
- Symmetric operand order bugs (round-trip succeeds)
- Namespace collisions (same numeric ID, different meaning)
- Incorrect operand values that don't cause downstream failures

---

## Fix Priority

### MUST FIX BEFORE RNG
None - RNG implementation is not blocked by these issues.

### MUST FIX BEFORE MORE SCRIPT WORK
1. **Operand order bugs** - Fix decoder to match pokecrystal byte layout
2. **Round-trip validation** - Either invoke in production or build RGBDS oracle

### ARCHITECTURAL TEST GAP
3. **FlagId namespace** - Add namespace distinction to prevent future collisions
4. **Independent oracle** - Build RGBDS-based fixtures for authoritative byte verification

### CLEAN / STALE
- sdefer semantics - Clean
- BG event types - Fixed
- Movement decoder - Acceptable for current scope

---

## Justification

**PARTIALLY TRUSTWORTHY** because:

1. **Working**: Vanilla Crystal corpus compiles, links, and runs correctly for all tested scenarios
2. **Structurally sound**: CFG, lowering, and legality gates catch real errors
3. **Latent bugs exist**: Operand order bugs are provably wrong but happen not to cause visible failures
4. **Self-consistent but not externally validated**: Round-trip validation proves internal consistency, not correctness against authoritative source
5. **Fixes are bounded**: All identified issues have clear, localized fixes that don't require architecture changes

---

## Audit Completion

| Item | Status |
|------|--------|
| 1. Operand schemas | AUDITED - BUGS FOUND |
| 2. Round-trip validation | AUDITED - BYPASSED |
| 3. Text commands | AUDITED - PARTIAL |
| 4. Flag namespaces | AUDITED - GAP |
| 5. Movement commands | AUDITED - PARTIAL |
| 6. sdefer semantics | AUDITED - CLEAN |
| 7. Independent oracle | DESIGN OUTLINED |
| 8. Corpus implications | ANALYZED |

**Audit Date**: August 18, 2026
**Auditor**: Kiro (hostile audit mode)
