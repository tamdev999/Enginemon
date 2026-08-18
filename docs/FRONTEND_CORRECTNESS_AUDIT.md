# Hostile Frontend Correctness Audit - August 2026

## Executive Summary

**FRONTEND TRUST VERDICT: TRUSTWORTHY** (upgraded from PARTIALLY TRUSTWORTHY)

Following the August 2026 semantic stabilization pass, all confirmed schema bugs have been fixed. The Crystal frontend now correctly decodes operand order, preserves flag namespaces, and validates round-trip encoding in production.

### Key Findings - UPDATED

| # | Finding | Verdict | Severity | Status |
|---|---------|---------|----------|--------|
| 1 | Operand order bugs in 8+ string formatting commands | **FIXED** | HIGH | ✓ Decoder/encoder match pokecrystal |
| 2 | Round-trip validation bypassed in production | **FIXED** | MEDIUM | ✓ Validation wired into production |
| 3 | Text command handling | COMPLETE | LOW | ✓ All TX_* commands decoded |
| 4 | EventFlag/EngineFlag namespace | **FIXED** | MEDIUM | ✓ FlagRef with namespace enum |
| 5 | Movement command decoder | COMPLETE | LOW | ✓ All 0x00-0x59 handled |
| 6 | sdefer pointer semantics | CLEAN | - | ✓ Consistent |
| 7 | BG event types silently collapsed | **FIXED** | HIGH | ✓ Exhaustive switch |
| 8 | Corpus green despite schema bugs | RESOLVED | - | ✓ Bugs fixed |

---

## 1. Event Opcode Operand Schemas - FIXED ✓

**Prior Bug**: Typed decoder read operands in wrong order for string formatting commands.

**Fix Applied (August 2026)**:
Updated `typed_decoder.cpp` to match exact ROM byte layout from `pokecrystal/macros/scripts/events.asm`:

| Command | pokecrystal order | Decoder (now correct) |
|---------|-------------------|----------------------|
| getmoney (0x3D) | account, strbuf | ✓ account, strbuf |
| getmonname (0x40) | pokemon, strbuf | ✓ pokemon, strbuf |
| getitemname (0x41) | item, strbuf | ✓ item, strbuf |
| gettrainername (0x43) | group, id, strbuf | ✓ group, id, strbuf |
| getstring (0x44) | pointer, strbuf | ✓ pointer, strbuf |
| getlandmarkname (0xA5) | id, strbuf | ✓ id, strbuf |
| gettrainerclassname (0xA6) | group, strbuf | ✓ group, strbuf |
| getname (0xA7) | type, id, strbuf | ✓ type, id, strbuf |

---

## 2. Round-Trip Validation - FIXED ✓

**Prior Gap**: Production compiler bypassed `validate_script_round_trip()`.

**Fix Applied (August 2026)**:
- `full_compiler.cpp` now calls `typed_decoder_->validate_script_round_trip(ir)`
- `corpus_lowering_audit.cpp` similarly updated
- Results flow to legality gate for enforcement

---

## 3. Text Command Handling - COMPLETE ✓

All Crystal text commands (TX_*) are now decoded to semantic `TextOp` values:

| Command | Opcode | TextOp | Status |
|---------|--------|--------|--------|
| TX_RAM | 0x01 | TextRam | ✓ |
| TX_BCD | 0x02 | TextBcd | ✓ |
| TX_MOVE | 0x03 | TextMove | ✓ |
| TX_BOX | 0x04 | TextBox | ✓ |
| TX_LOW | 0x05 | TextLow | ✓ |
| TX_PROMPT_BUTTON | 0x06 | TextPromptButton | ✓ |
| TX_SCROLL | 0x07 | TextScroll | ✓ |
| TX_START_ASM | 0x08 | TextAsm | ✓ |
| TX_DECIMAL | 0x09 | TextDecimal | ✓ |
| TX_PAUSE | 0x0a | TextPause | ✓ |
| TX_STRINGBUFFER | 0x14 | TextStringBuffer | ✓ |
| TX_DAY | 0x15 | TextDay | ✓ |
| TX_FAR | 0x16 | TextFar | ✓ |
| Sound commands | 0x0f-0x13 | TextSound* | ✓ |

---

## 4. EventFlag vs EngineFlag Namespace - FIXED ✓

**Prior Gap**: Both flag types lowered to same `FlagId` type.

**Fix Applied (August 2026)**:
Created `FlagRef` struct with `FlagNamespace` enum:

```cpp
enum class FlagNamespace : uint8_t {
    Event = 0,    // wEventFlags (2048 bits, 0-2047)
    Engine = 1,   // wEngineFlags (190 bits, 0-189)
};

struct FlagRef {
    FlagNamespace ns;
    uint16_t value;
};
```

- `Sem_SetFlag`, `Sem_ClearFlag`, `Sem_CheckFlag` now use `FlagRef`
- Linker validates ranges per namespace
- No silent namespace collision possible

---

## 5. Movement Command Decoder - COMPLETE ✓

All Crystal movement commands 0x00-0x59 are now handled:

| Range | Commands | Status |
|-------|----------|--------|
| 0x00-0x37 | Directional movements | ✓ |
| 0x38-0x46 | Control commands | ✓ |
| 0x47 | step_end | ✓ terminal |
| 0x48 | step_wait_end | ✓ terminal with param |
| 0x49-0x4B | remove_object, step_loop, step_stop | ✓ terminals |
| 0x4C-0x50 | teleport, skyfall, dig, bump | ✓ |
| 0x51-0x54 | fish, emote commands | ✓ |
| 0x55-0x57 | shake, tree, rock_smash | ✓ |
| 0x58 | return_dig | ✓ with param |
| 0x59 | skyfall_top | ✓ terminal |
| ≥0x5A | Invalid | ✓ throws error |

---

## 6. sdefer Pointer Semantics - CLEAN ✓

Unchanged - both discovery and lowering use consistent pointer resolution.

---

## 7. BG Event Type Collapse - FIXED ✓

**Prior Bug**: Only Read, HiddenItem, FacingUp handled; others collapsed to Read.

**Fix Applied**:
Exhaustive switch for all 9 BG event types with `condition_flag` propagation.

---

## Compiler Version

**CRYSTAL_COMPILER_VERSION**: `crystal-2.3.0`
- 2.3.0: Operand order fix, FlagRef namespaces, text decoder complete, movement decoder complete, round-trip wiring

---

## Audit Conclusion

| Gate | Status |
|------|--------|
| Operand order | FIXED ✓ |
| Round-trip validation | WIRED ✓ |
| Text commands | COMPLETE ✓ |
| Flag namespaces | FIXED ✓ |
| Movement decoder | COMPLETE ✓ |
| BG event types | FIXED ✓ |
| sdefer semantics | CLEAN ✓ |

**Audit Date**: August 18, 2026
**Auditor**: Kiro (semantic stabilization pass)
