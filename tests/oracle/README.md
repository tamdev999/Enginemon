# Crystal Frontend Oracle — Phase 1

## Purpose

Independent source-fidelity tests for the Enginemon Crystal frontend.

These tests exist to catch corruption classes that same-implementation round-trip
tests structurally cannot detect: symmetric decoder/encoder bugs, cross-layer seam
failures, operand-order confusion, namespace collapse, pointer-kind errors, and
silent fallback/degradation.

**Independence rule**: expected values in this suite are authored from pokecrystal
semantics + Crystal macro documentation + human reasoning.  They are NEVER derived
by running Enginemon's own encoder, decoder, identity_string(), or lowering output
and snapshotting the result.

---

## Provenance

```
pokecrystal reference: https://github.com/pret/pokecrystal
  commit: 8e8f7e200 (HEAD at time of Phase 1.5 — see git log in references/pokecrystal)
RGBDS version: 1.0.3 (pinned — see regen_fixtures.sh)
Fixture bytes: assembled by RGBDS 1.0.3 from hand-authored .asm sources
               All 8 Phase 1+1.5 fixtures verified byte-identical to RGBDS 1.0.3 output.
```

The `.bin` files in `fixtures/` are checked in and are the normal CI inputs.
The `.asm` files are the authoritative source for what the bytes mean.
Running `regen_fixtures.sh` reproduces byte-identical `.bin` files.

---

## Fixture inventory

### Binary-layout fixtures (`fixtures/`)

Each `.asm` + `.bin` pair exercises one historically-fragile axis.

| Fixture | Axis | Historical bug killed |
|---------|------|-----------------------|
| `event_operand_order` | Operand order (gettrainername) | gettrainername operands decoded in wrong order — trainer_group, trainer_id, strbuf reversed from macro argument order |
| `event_flag_vs_engine_flag` | Flag namespace (EventFlag ≠ EngineFlag) | checkevent and checkflag collapsed to same FlagId type, losing namespace distinction |
| `text_tx_ram_mixed` | TX_RAM in literal text stream | TX_RAM opcode (0x01) boundary detection — byte value 0x01 is also a valid character code in some charsets |
| `text_tx_decimal` | TX_DECIMAL operand layout | TX_DECIMAL (0x09) dw+dn operand — bytes|digits nibble pack format |
| `movement_step_dig` | step_dig parameter byte | step_dig (0x4F) consumes a length parameter byte that must be preserved, not treated as next command |
| `movement_skyfall_top` | skyfall_top terminal | skyfall_top (0x59) is a terminal — previously silently degraded to StepEnd or caused decoder overrun |
| `sdefer_bank_resolution` | sdefer bank-relative pointer → flat | sdefer resolved pointer using wrong bank, or used raw 16-bit as flat address |
| `connection_offset_direction` | Connection direction-dependent offset byte | MapExtractor selected wrong offset byte for connection direction: N/S must use data[9] (X), E/W must use data[8] (Y) |

### Package seam fixtures (`package_seam/`)

Pure C++ tests.  No `.asm` / `.bin`.  Use real production writer + reader.

| Fixture | Axis | Historical bug killed |
|---------|------|-----------------------|
| `bg_event_ifset_test.cpp` | BgEvent IfSet + condition_flag across package seam | BgEventType::IfSet → RuntimeBgEventType::Read collapse; condition_flag string dropped |
| `sprite_id_boundary_test.cpp` | Sprite ID 1..102 boundary mapping | sprite index 0 / 103+ producing wrong or empty IDs |
| `map_connection_seam_test.cpp` | MapConnection direction + strip_offset round-trip | Connection direction/offset wrong for E/W vs N/S (different offset byte selected) |

### Negative fixtures (`negative/corrupted/`)

Hand-crafted invalid byte sequences — NOT RGBDS output.

| Fixture | Expected failure |
|---------|-----------------|
| `truncated_operand.bin` | Decoder must fail explicitly (not return partial/default result) |
| `invalid_movement_opcode.bin` | Invalid movement opcode (0x5A) must produce hard error |

---

## How to regenerate fixtures

```bash
# From the Enginemon workspace root (verify mode — default):
./tests/oracle/tools/regen_fixtures.sh

# To update checked-in .bin files after intentionally changing a .asm:
./tests/oracle/tools/regen_fixtures.sh --update
```

Requires RGBDS 1.0.3 on PATH (or set `RGBASM=/path/to/rgbasm`).
The script assembles each `.asm` into a flat binary and compares against checked-in.
In **verify mode** (default): exits nonzero on any mismatch — never updates files.
In **update mode**: overwrites `.bin` files — only use when a fixture change is intentional.

A mismatch means either the `.asm` fixture changed, the checked-in `.bin` is wrong,
or the RGBDS version has drifted. Investigate before updating.

---

## Mutation check convention

When a fixture guards a specific historical bug, the test must include a
mutation check that intentionally applies the old wrong behavior locally and
confirms the test fails.  This is done in the C++ test body — no production
code is modified.
