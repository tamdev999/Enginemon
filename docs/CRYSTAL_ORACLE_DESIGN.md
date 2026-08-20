# Crystal Frontend Oracle Design

Design only. No production code changes. No RNG work.

---

## 1. Threat model

The oracle exists to catch corruption classes that same-implementation round-trip tests
structurally cannot see:

| # | Class | Why round-trip misses it |
|---|-------|--------------------------|
| 1 | Decoder/encoder share a wrong assumption | Both sides agree with each other, not with Crystal |
| 2 | Cross-layer seam bugs (extractor ✅, package writer ❌, runtime ✅) | Each layer's isolated test is green; only the composed path is wrong |
| 3 | Operand order/width/signedness/truncation | Symmetric bugs are invisible to a symmetric test |
| 4 | Namespace collision (EventFlag{5} vs EngineFlag{5}) | Nothing forces two domains with overlapping integers to stay distinct |
| 5 | Pointer-kind confusion (raw16 bank-relative treated as flat) | Only visible if the fixture address is chosen so the two interpretations diverge |
| 6 | Silent fallback/degradation (unknown → StepEnd/Read/0/"?") | Looks like success; produces wrong behavior instead of a loud failure |
| 7 | Text-as-code / code-as-text confusion | Text is a command language interleaved with character data — ambiguity is the normal case |
| 8 | Enum variant drift across the package boundary | New variant added on one side, other side silently defaults |
| 9 | Oracle drift (fixtures quietly rewritten to match the decoder instead of Crystal) | Meta-bug — the oracle stops being independent over time |

Everything below exists to make these nine specifically hard to (re)introduce.

---

## 2. Oracle architecture

**Binary-layout path** (proves layers: binary-layout → typed-decoder → semantic-lowering):

```
pokecrystal source/macros
  → RGBDS (pinned version)
  → checked-in binary fixture blob
  → Enginemon decoder
  → typed IR
  → hand-authored semantic assertion (NOT generated from the decoder/encoder)
```

**Package-seam path** (proves: package preservation → runtime interpretation):

```
hand-constructed compiler-side semantic object
  → real package writer   (production code, under test)
  → real package reader   (production code, under test)
  → runtime object
  → hand-authored expected runtime object (deep comparison)
```

Note the asymmetry: the binary-layout path gets external ground truth from RGBDS. The
package-seam path has no external ground truth — Crystal's ROM format says nothing about
Enginemon's internal package format. Independence there means something narrower and just
as important: the expected runtime object is written by a human reasoning about what should
survive, before/without reading what the writer or reader currently do. Never derive
"expected" by running the real writer+reader once and snapshotting the output — that proves
self-consistency, which is exactly the failure mode from the confirmed
BgEventType::IfSet → RuntimeBgEventType::Read bug (both components were individually
correct; nothing checked the seam against an independently-stated expectation).

A fixture proves only the layers it actually traverses. A Tier A fixture proves
binary-layout + decode; it says nothing about package/runtime. Tag fixtures with which
layers they cover so "we have tests" never gets conflated with "we have coverage."

---

## 3. Source-of-truth hierarchy

1. **pokecrystal source + RGBDS-assembled bytes** — authoritative for binary layout and
   operand semantics. Pinned by commit hash + RGBDS version (§7).

2. **Human-authored semantic assertions**, written by reading pokecrystal source directly —
   authoritative for "what this opcode/field means." Must not be reverse-engineered from
   Enginemon's current decoder output.

3. **Known real-ROM offsets / assembled vanilla builds** — authoritative for Tier D
   integration slices, used only when an isolated fixture would be artificial (e.g. proving
   a specific real script actually plays correctly end-to-end).

4. **Enginemon decoder/encoder/package/runtime** — never authoritative. Always the thing
   being checked, never the thing checking.

---

## 4. Fixture structure (repo layout)

v1 simplification: one `.asm`/`.bin` pair per fixture, not shared per-domain files. No
`.sym` parsing, no offset manifest, no batching machinery — the whole `.bin` is the
fixture. Revisit batching (shared domain file + symbol-table-derived offsets) only
if/when Tier E's ~170-opcode scale actually makes one-file-per-fixture slow to build, not
before.

```
tests/oracle/
  README.md                    # rules + one line of provenance:
                               #   pokecrystal commit, RGBDS version
  tools/
    regen_fixtures.sh          # rgbasm+rgblink each fixture .asm → .bin, run manually
  fixtures/
    event_operand_order.asm / .bin
    event_flag_vs_engine_flag.asm / .bin
    text_tx_ram_mixed.asm / .bin
    text_tx_decimal.asm / .bin
    movement_step_dig.asm / .bin
    movement_skyfall_top.asm / .bin
    pointer_sdefer.asm / .bin
    connections_offset.asm / .bin
  negative/
    corrupted/                 # hand-crafted raw bytes, NOT RGBDS output (see §9)
      truncated_operand.bin
      invalid_opcode.bin
  package_seam/
    bg_event_ifset_test.cpp    # no .asm/.bin — starts from a hand-built compiler object
    sprite_id_boundary_test.cpp
  golden/                      # Tier D, added later — real assembled/ROM-derived slices
  oracle_event_test.cpp
  oracle_movement_test.cpp
  oracle_text_test.cpp
  oracle_pointer_test.cpp
  oracle_namespace_test.cpp
```

Each fixture is two files and one build step:

```sh
rgbasm -o /tmp/f.o fixtures/event_operand_order.asm
rgblink -o fixtures/event_operand_order.bin /tmp/f.o
```

and one test function reading that `.bin` directly. No offset extraction, no manifest,
nothing else in between.

Deliberate non-decisions: no YAML/DSL for expected values, no generated opcode table, no
`.sym` parsing. Each fixture's expectation is an ordinary unit-test function asserting
typed-IR fields directly. A hand-written assertion per fixture we chose to write is not
"a second giant opcode table" — a parallel byte-layout table covering all opcodes up front
would be. Writing per-fixture assertions is fine.

---

## 5. Initial fixture matrix (minimum, maximum breadth)

One fixture per named bug-class, not per opcode. Ten fixtures, five files.

| Fixture | File | Layers proven | Asymmetric design |
|---------|------|---------------|-------------------|
| Operand-order bug | `event_operand_order.asm` | binary+decode | `gettrainername 3, 17, 201` — three distinct values, no field can silently swap with another and pass |
| EventFlag{5} vs EngineFlag{5} | `event_flag_vs_engine_flag.asm` | decode+semantic | same numeric ID (5) in two different opcodes; assert different values on different IR fields/variant tags |
| TX_RAM mixed text | `text_tx_ram_mixed.asm` | decode+semantic | literal → command byte → literal, chosen so a byte that would be a valid ASCII character is also a valid command opcode, forcing correct boundary tracking |
| TX_DECIMAL | `text_tx_decimal.asm` | decode+semantic | operand width/count checked with non-trivial digit count |
| Movement parameter command (step_dig) | `movement_step_dig.asm` | binary+decode | parameter byte given a value that is not 0/1 and not equal to any nearby opcode number |
| skyfall_top | `movement_skyfall_top.asm` | decode+semantic | historically-fragile; regression fixture |
| sdefer bank-relative pointer | `pointer_sdefer.asm` | decode+semantic | placed so raw16 ≠ flat_address_low16 (see §pointer below) — known-clean regression per task spec |
| BG IfSet package seam | `package_seam/bg_event_ifset_test.cpp` | package+runtime | no RGBDS; hand-built BgEventType::IfSet{condition_flag} → real writer → real reader → hand-built expected runtime object |
| Connection semantic fixture | `connections_offset.asm` | decode+semantic | offset map connection, asymmetric offset value |
| Sprite ID > 58 seam | `package_seam/sprite_id_boundary_test.cpp` | boundary-dependent | value exactly at and one past the boundary, both asserted |

### Namespace/type oracle, generalized

The EventFlag/EngineFlag fixture is one instance of a pattern, not a one-off. For every
typed-reference domain (EventFlag, EngineFlag, Species, Item, Move, Map, Script,
TrainerGroup, TrainerId, Text, Sprite): pick one raw integer, encode it in two different
domains inside one fixture, and assert both decode to the correct value in the correct
field/variant without collapsing into each other. For Phase 1 this is a runtime assertion
only (different fields/variant tags hold different values) — no type-system change required
to write it. A strong-typedef/newtype pattern that makes EventFlag{5} == EngineFlag{5}
fail to compile is a strictly stronger, independent follow-up (§ADD INCREMENTALLY): it
turns the guarantee from "this fixture would catch it" into "the compiler catches it," but
it's a separate refactor, not a precondition for writing the fixture now.

### Pointer/address oracle

The ambiguity has to be forced: choose sdefer's bank and in-bank offset so that if the
decoder incorrectly treated the raw 16-bit bank-relative pointer as a flat ROM address, it
would land somewhere detectably wrong (ideally out of any valid bank, or inside a different
known symbol) rather than by-coincidence-correct. Concretely: avoid bank 0 for the fixture
(bank 0 is where raw16-as-flat and true-flat are most likely to coincide for early
addresses), and avoid placing the fixture at a low in-bank offset where
`0x4000 + offset` could coincidentally match the intended flat address arithmetic
elsewhere. This has to be checked against the actual pointer scheme in pokecrystal during
implementation, not assumed from this doc.

---

## 6. Scaling strategy

Do not batch-generate all ~170 event opcodes or all movement/text commands up front. Two
parallel tracks:

**Tier B, incremental**: when an opcode/command gets implemented, its oracle fixture
(RGBDS block + hand-written assertion) is added in the same PR. This is a process rule,
not tooling — put it in `tests/oracle/README.md`.

**Tier E, one-shot breadth net**: a script parses pokecrystal's own opcode table (not a
hand-copied Enginemon table) to enumerate all opcodes, generates one minimal `.asm` line
per opcode with asymmetric filler operands, assembles once, and asserts only the cheap
corpus invariant: no currently-supported opcode decodes to fallback/unknown. This gives
broad structural coverage immediately without hand-authoring 170 semantic assertions — it
catches "silently degraded" but not "wrong but plausible," which is what Tier B is for.
Explicitly supplementary.

Same two-track pattern applies to movement (0x00–0x59) and text commands.

**Package-seam tests**: prioritize by risk, not alphabetically — types with enums, optional
fields, or flags first (highest silent-default risk), typed IDs/pointers second, everything
else last.

---

## 7. CI/build integration

Recommended model: `.asm` sources checked in, `.bin` blobs checked in, a manual regen
script uses pinned RGBDS, normal test runs consume only the checked-in blobs.

**Rejected alternatives and why**:

- *RGBDS required at every local test run* — slow, fragile on Windows dev machines, couples
  "can I run unit tests" to "do I have a Game Boy toolchain installed."
- *Generate during CMake configure* — same coupling, just moved earlier; breaks clean builds
  for contributors without RGBDS.

Provenance for v1 is one line in `tests/oracle/README.md`
(`pokecrystal @ <commit>, RGBDS <version>`), not a provenance.json schema file —
machine-checked provenance is easy to add later if the fixture count grows enough to want
it. Same for CI: for now, run `regen_fixtures.sh` by hand before committing a changed
fixture; wire an actual CI diff job once there are enough fixtures that "did someone forget
to regen" stops being a hypothetical and starts being a real risk.

---

## 8. Oracle drift prevention

- Binary fixtures can only change via `regen_fixtures.sh` against pinned RGBDS — never
  hand-edited, never regenerated by "run the current decoder and diff."

- For v1 this is manual discipline, not a CI gate: regenerate before committing a changed
  fixture, and a reviewer can spot-check by rerunning the script on a PR that touches
  `.asm` files. Promote this to an actual CI diff job once fixture count makes manual
  discipline unreliable — the check itself doesn't change, only whether a human or a
  pipeline runs it.

- Convention, not tooling, for the harder case ("oracle expectation changed alongside
  production code, coincidentally in the same direction as a bug"): require the PR
  description to state why an oracle assertion changed, separately from the production
  diff. This is a review discipline, not a CI gate.

- **Mutation check at fixture-authoring time, not ongoing**: when a fixture is written to
  guard a specific historical bug (operand-order, EventFlag/EngineFlag, IfSet→Read),
  temporarily reintroduce that exact bug locally and confirm the new fixture fails. If it
  doesn't fail, the fixture doesn't actually prove what its name claims. Do this once per
  fixture at authoring time; it's not a recurring CI step.

---

## 9. Negative test strategy

Three distinct categories, not one "negative tests" bucket:

**Source-valid** — RGBDS assembles it, Crystal considers it legal, Enginemon supports it →
exact semantic decode. (Everything in §5.)

**Enginemon-unsupported-but-source-valid** — RGBDS assembles it, Crystal considers it
legal, Enginemon hasn't implemented it yet → must decode to a lossless typed "unlowered"
representation, then fail legality explicitly. The oracle should test against that state
machine directly rather than inventing a parallel one.

**Source-invalid** — bytes that no real RGBDS/pokecrystal build could ever produce
(truncated operand, corrupted opcode byte). These cannot come from RGBDS by construction —
RGBDS's whole job is refusing to emit invalid output. So this category is necessarily
hand-crafted raw bytes, checked in directly under `negative/corrupted/`, not assembled.
Independence here means something different: the expected failure (which error, at which
byte offset) is written down by a human describing the corruption before looking at what
the current decoder actually does with those bytes — not derived by running the decoder
once and asserting whatever it happens to do.

For every hard compiler/decoder gate, at least one fixture from category 3 proving it
fails for the intended reason (not just "throws something").

---

## 10. Package seam strategy

Pattern (from §2): hand-built compiler-side object → real writer → real reader →
hand-built expected runtime object → deep comparison. No fixture file, no RGBDS — these
are pure C++ unit tests.

**Priority order** for which semantic types get exhaustive seam tests: enums first, optional
fields second, flags third, typed IDs/pointers/directions fourth — because a missing
`default:` case or an unhandled optional is the exact shape of the confirmed
IfSet → Read bug, and typed IDs/pointers are lower-risk because they don't have a
"silently fall back to something plausible" failure mode the way enums and optionals do.

**Complementary structural defense**: `switch` statements over the compiler-side and
runtime-side enums with no `default:` case, so adding a new BgEventType variant without
updating the package writer/reader is a build error, not a hoped-for test failure. This
doesn't replace seam tests — it catches the "forgot entirely" case at compile time; seam
tests catch "handled, but wrong" at test time. Cheap to add, worth doing for every enum
crossing the package boundary, not just the ones with dedicated seam tests.

---

## 11. Recommended implementation phases

**Phase 1** (§5's ten fixtures + skeleton): useful immediately, small enough to land in one
PR-sized unit of work. Everything in §4's repo layout, populated only with the §5 matrix.
No Tier E generator yet, no Tier D golden slices yet.

**Phase 2**: Tier E corpus-invariant generator (script over pokecrystal's opcode table,
"no fallback" assertion) for event opcodes only. Extends breadth cheaply without
hand-authoring more semantics.

**Phase 3**: same Tier E treatment for movement and text.

**Phase 4**: remaining namespace/type-distinctness fixtures (Species, Item, Move, Map,
Script, TrainerGroup, TrainerId, Text, Sprite) — same pattern as the EventFlag/EngineFlag
fixture, one at a time.

**Phase 5**: remaining package-seam types by the priority order in §10.

**Phase 6**: first Tier D golden slice (pick one — e.g. Cyndaquil `getmonname`) to validate
the real-ROM/assembled-build integration path before committing to more of them.

Each phase is independently useful and independently shippable; none require the next to
have value.

---

## 12. Acceptance criteria

A domain (event opcodes / movement / text / pointers / a given package type) has
independent source-fidelity coverage when, and only when:

1. Its fixture bytes are provably RGBDS-derived from pinned pokecrystal source (or, for
   package-only types with no ROM representation, from a hand-built compiler-side object) —
   never hand-typed, never copied from decoder output.

2. Expected semantic values were authored by a human reading pokecrystal semantics (or
   reasoning about intended package behavior) — never generated by running Enginemon's own
   encoder/decoder/writer/reader and snapshotting.

3. At least one asymmetric fixture exists per historically-fragile axis for that domain
   (order/width/signedness/pointer-kind/namespace), and each such fixture has been
   mutation-checked (§8) against the specific bug it's named for.

4. CI proves the checked-in bytes match fresh RGBDS regeneration, so independence can't
   silently erode over time.

Meeting these is what distinguishes "the frontend has independent source-fidelity coverage"
from "the compiler's own components agree with each other" — the latter is what round-trip
tests already prove, and is not the goal here.

Note: Phase 1 as scoped satisfies 1–3 immediately but defers 4 (CI proving regeneration is
byte-identical) to the manual-discipline stage in §7/§8. Phase 1 is a working, useful
oracle; the "independent source-fidelity coverage" claim in full is reached once the CI
diff job lands.

---

## Summary

### DO NOW

- `tests/oracle/` skeleton (§4) + one provenance line in README.md
- The ten §5 fixtures: eight `.asm`/`.bin` pairs + two pure-C++ package-seam tests
- `regen_fixtures.sh` (run manually, no CI yet)
- Mutation-check each fixture once against the bug it's named for (§8)

### ADD INCREMENTALLY

- `provenance.json` + a CI diff job, once fixture count makes manual regen discipline
  unreliable
- Shared per-domain `.asm` files + `.sym`-derived offsets, only if/when Tier E's
  ~170-opcode scale actually makes one-file-per-fixture slow to build
- Compile-time newtype/strong-typedef distinctness for typed-reference domains (Phase 1
  ships with a runtime-only version of this assertion)
- Tier B fixture per opcode/command, added in the same PR that implements it
- Tier E "no fallback" generator, event → movement → text, in that order
- Remaining namespace/type-distinctness fixtures, one domain at a time
- Remaining package-seam tests, prioritized enum > optional > flag > typed-ID
- Exhaustive-switch compile-time guards on every enum crossing the package boundary
- First Tier D golden slice, once Phase 1–5 patterns are proven

### DO NOT BUILD

- A second Crystal/pokecrystal compiler or AST
- A giant declarative opcode DSL or fixture-expectation language (YAML/etc.) — plain
  hand-written test functions are enough at this scale
- A parallel hand-maintained opcode byte-layout table (use RGBDS symbol output, and only
  once batching is actually needed)
- Symbolic execution or SMT-based semantic equivalence
- A whole-ROM behavioral emulator oracle
- Automatic regeneration of expected semantic assertions from decoder changes — this is the
  one thing that would silently destroy independence
- CI machinery or a provenance schema before there's more than a handful of fixtures to
  justify it — a README line and a manual script are enough at ten fixtures
- A fixture-change review bot or formal governance process — a provenance note, a manual
  regen check, and a PR-description convention (§8) are sufficient at this scale; add
  process only if drift actually happens in practice
