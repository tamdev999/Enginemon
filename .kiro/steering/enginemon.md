# Enginemon Project Steering

## Project Overview

Enginemon is a native C++23 Pokémon runtime/compiler platform.

The first frontend targets Pokémon Crystal, but Crystal is content input and compatibility authority, not the runtime architecture.

```
ROM (Crystal / Gold / Silver / RBY / Emerald / hack / ...)
→ SHA-1 identification → exact profile selection
→ frontend compiler (source-specific)
→ semantic native game definitions
→ EMON package
→ generic native runtime
→ Vulkan 2D / 3D / enhanced 3D / RT-assisted rendering
```

Long-term:

```
Crystal retail profile ─┐
Crystal hack A profile  ─┤
Crystal hack B profile  ─┤
Gold/Silver profile     ─┼→ common Enginemon semantic model → EMON → native runtime
RBY profile             ─┤
Emerald profile         ─┘
Mods ──────────────────┘
```

The shipped game must run without:
- Game Boy emulation
- SM83 execution
- ROM access
- banked-memory abstractions
- OAM/VRAM abstractions
- Crystal pointer/address identity

---

## Compiler Input Contract

The Crystal frontend accepts ONLY raw ROM bytes as input.

**No auxiliary files:**
- no .sym symbol files
- no pokecrystal source tree at compile time
- no external metadata files
- no manually selected hack profiles

**ROM identity is always the actual input ROM's SHA-1 hash.**

The compiler pipeline begins with strict ROM identification:

```
raw ROM bytes
→ SHA-1 hash
→ exact profile lookup (registered SHA → exact profile)
→ OR explicit fallback with strong layout validation
→ frontend compiler
```

An unrecognized SHA-1 may only proceed with an explicitly supplied fallback profile if that profile passes a full layout validation pass (not just superficial spot checks). If compatibility cannot be proven, the compiler fails explicitly.

This means:
- Crystal retail → Crystal v1.1 profile
- Crystal retail v1.0 → Crystal v1.0 profile
- Crystal hack A → registered hack-A profile OR validated-compatible profile
- Unknown ROM → hard failure (no silent guessing)

The cache identity is the actual input ROM's SHA-1. A package compiled from ROM A can never satisfy a cache lookup for ROM B, even if they share the same profile layout.

**No hardcoded assumptions:**
- species count must be derived from ROM structure, not hardcoded 251
- table locations may be discovered or validated from ROM patterns
- domain sizes come from ROM evidence, not profile overrides

The frontend may embed knowledge of Crystal data-structure formats (record sizes, field offsets, encoding rules), but counts and locations must be proven from the ROM being compiled.

This enables:
- vanilla Crystal (251 species)
- format-compatible ROM hacks (245, 274, or other species counts)
- relocated tables (common in ROM hacks)

without per-hack profile maintenance — provided the selected profile truthfully describes the ROM's actual layout.

---

## Build Instructions

**CRITICAL**: Use Windows CMake:
```
C:\Program Files\CMake\bin\cmake.exe
```
Do not use devkitPro/MSYS2 CMake from PATH.

### Initial Setup

```powershell
Remove-Item -Path build -Recurse -Force -ErrorAction SilentlyContinue
& "C:\Program Files\CMake\bin\cmake.exe" `
  -B build `
  -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake `
  -DENGINEMON_ENABLE_ENGINE=ON `
  -DENGINEMON_ENABLE_VULKAN=ON
```

### Build Runtime

```powershell
& "C:\Program Files\CMake\bin\cmake.exe" --build build --target enginemon_tiles --config Release
```

### Build Tests

```powershell
& "C:\Program Files\CMake\bin\cmake.exe" --build build --target golden_test --config Release
& "C:\Program Files\CMake\bin\cmake.exe" --build build --target runtime_test --config Release
```

### Run

```powershell
Start-Process `
  -FilePath ".\build\runtime\Release\enginemon_tiles.exe" `
  -ArgumentList "crystal.emon" `
  -WorkingDirectory (Get-Location)
```

### Run All Tests (Canonical Verifier)

```powershell
# Find ROM path and run canonical verifier
$romPath = (Get-ChildItem -Path "references" -Filter "*.gbc" | Select-Object -First 1).FullName
.\run_all_tests.ps1 -RomPath $romPath
```

This runs all 6 test suites and reports key invariants:
- corpus lowering = 1788/1788
- linker corpus = 1788/1788
- InvalidOwnership = 0
- decoder/CFG = PASS

### Run Individual Test Suites

```powershell
.\build\tests\Release\runtime_test.exe "references\Pokemon - Crystal Version (UE) (V1.1) [C][!].gbc"
.\build\tests\Release\golden_test.exe "references\Pokemon - Crystal Version (UE) (V1.1) [C][!].gbc"
.\build\tests\Release\legality_gate_test.exe "references\Pokemon - Crystal Version (UE) (V1.1) [C][!].gbc"
.\build\tests\Release\corpus_test.exe "references\Pokemon - Crystal Version (UE) (V1.1) [C][!].gbc"
.\build\tests\Release\linker_test.exe "references\Pokemon - Crystal Version (UE) (V1.1) [C][!].gbc"
.\build\tools\Release\corpus_lowering_audit.exe "references\Pokemon - Crystal Version (UE) (V1.1) [C][!].gbc"
.\build\tests\Release\compiler_integrity_test.exe "references\Pokemon - Crystal Version (UE) (V1.1) [C][!].gbc"
```

---

## Architectural North Star

Enginemon is not a Game Boy emulator with native rendering.
It is a compiler plus native game runtime.

The frontend translates source-game implementation details into stable semantic concepts.

```
source implementation
→ semantic intent
→ native runtime behavior
```

Never:
```
source implementation
→ renamed hardware abstraction
→ runtime imitation of original machine
```

Modernize:
- storage
- memory layout
- batching
- threading
- rendering
- audio synthesis
- asset representation
- mod composition
- tooling

Preserve:
- gameplay semantics
- script behavior
- timing where observable
- data relationships
- mechanics
- content identity

---

## Core Architecture Rules

### 1. Crystal Is a Compiler Frontend

All Crystal-specific concepts belong under the Crystal frontend/compiler boundary:
- ROM addresses
- bank numbers
- banked pointers
- RGBDS symbols
- Crystal table indices
- Crystal compression
- Crystal RAM addresses
- Crystal opcodes
- Crystal Special indices
- Crystal object encodings

They may exist as compiler evidence or source provenance.
They must not become runtime architecture.

After package creation/load:
- no ROM reads
- no source pointer chasing
- no Crystal decompression
- no RGBDS lookup
- no bank switching
- no SM83
- no simulated RAM
- no OAM
- no VRAM

### Runtime Ownership Model

Prefer a small explicit runtime:
- FrozenGameData
- BehaviorTable
- GameState
- RuntimeServices

Conceptually:

**FrozenGameData**
Immutable game/content definitions:
- maps
- species
- moves
- items
- trainers
- encounters
- scripts
- text
- graphics
- audio
- materials
- behaviors
- UI definitions

**BehaviorTable**
Stable semantic behavior identities mapped to native/mod implementations.

**GameState**
All mutable gameplay state:
- party (planned)
- inventory (items, money_player, money_mom, coins)
- flags and variables (event flags, engine flags, per-map scene state, script variables)
- map state (current map, player position, warp memory)
- actors (NPC positions, facing, visibility — snapshotted per map)
- battle state (planned)
- RNG (PCG-XSH-RR, canonical authoritative stream)
- RTC offset + DST flag
- script execution state
- field-move context

**RuntimeServices**
Non-game-state services:
- renderer
- audio
- input
- filesystem/package access
- RTC
- platform integration

Avoid:
- giant ECS frameworks
- service-locator sprawl
- global mutable state
- hardware-shaped memory models

---

## Script Compiler Architecture

Crystal scripts use a staged compiler pipeline.

The canonical pipeline is:

```
Crystal bytecode
→ typed lossless CrystalCommand IR
→ CFG
→ NativeCallRegistry + RamAddressRegistry
→ block-local SemanticScriptIR lowering
→ hard per-script legality gate
→ corpus-wide typed-reference linker
→ behavioral/golden conformance
→ Lua/package emission
→ runtime
```

This pipeline is authoritative.
Legacy script-decoder/IR/emitter architecture must not be reintroduced into production.

### Stage 1 — Typed Lossless Decode

Every valid Crystal opcode 0x00–0xA9 must decode into a typed command.

Requirements:
- exact operands
- correct widths/signedness
- exact source span
- raw source bytes retained compiler-side
- round-trip decode/encode capability

Known valid opcodes may never silently degrade to:
- Raw
- Unknown
- Opaque
- Fallback

### Stage 2 — Control Flow Graph

CFG construction is separate from bytecode decoding.

Control flow must represent explicit source semantics:
- fallthrough
- static jump
- conditional branch
- static call
- return
- terminal
- computed/native exit
- unresolved exit

Never invent an edge merely to make the graph close.
Native return/terminal behavior must come from source-proven registry classification.

### Stage 3 — Source Classification

Crystal-native concepts are classified before semantic lowering.

Examples:
- NativeCallRegistry
- RamAddressRegistry
- TrainerRegistry
- ElevatorRegistry
- StdScript authority

Registries must distinguish:
- source-proven
- ROM-extracted
- profile metadata
- speculative/unreferenced

Encountered behavior may not rely on speculative classification.

### Stage 4 — SemanticScriptIR

SemanticScriptIR is the semantic boundary.

Packageable semantic operations must not contain:
- ROM addresses
- bank numbers
- banked pointers
- RAM addresses
- Crystal opcode numbers
- Crystal Special table identity
- pointer-derived fake IDs
- address-derived strings
- magic sentinel modes
- Crystal symbol dispatch

A typedef does not make an identifier semantic.

This is invalid:
```cpp
using FooId = uint16_t;
FooId{rom_pointer}
```

A real semantic boundary requires:
```
source pointer/index
→ frontend resolution/extraction
→ semantic definition
→ stable semantic ID
```

### Semantic IDs

Stable typed IDs represent runtime concepts:
- MapId
- SpeciesId
- ItemId
- TrainerId
- TextId
- MusicId
- SfxId
- ElevatorId
- StdScriptId
- PokeMailId
- BehaviorId
- MaterialId
- ColorGradeId
- ...

Numeric coincidence with a source-game enum/index is acceptable only when accidental.
Authority must come from Enginemon's semantic registry, not the Crystal source index.

This is mandatory for future Gen1/Gen3 frontends.

### No Semantic Sentinels

Do not encode behavior with impossible magic values.

Bad:
- MapId{0} = backup warp
- species=0 = pending encounter
- object=0xFF = last talked
- value=-1 = script result

Use explicit types/variants:
- WarpToBackup
- LoadPendingEncounter
- MovementTarget::LastTalked
- ValueSource::ScriptResult

### Block-Local Legalization

Semantic pattern folding/absorption may operate only inside a basic block.

Never consume a pattern across:
- block boundary
- branch target
- alternate entry
- merge
- terminator

Every consumed source command must have proven semantic coverage.

### Command Accounting

Every decoded source command must be accounted for exactly once as:
- semantic
- or source-proven absorbed

The legality gate must reject:
- unlowered commands
- orphan commands
- accounting mismatch
- diagnostic residue
- invalid semantic operands

"Tests pass" is not proof that absorption is correct.
Every absorbed pattern must be source-proven as behaviorally lossless.

### Stage 5 — Hard Legality Gate

A script does not enter the packageable corpus unless legality succeeds.

No degraded output.
No warnings-as-runtime-fallback.

No:
- Sem_Unlowered
- Op_Raw fallback
- ASM interpreter fallback
- generic native call fallback
- generic RAM operation fallback

Failure means compilation failure.

### Stage 6 — Typed Reference Linking

Semantic references are validated corpus-wide.

Use these concepts:

**ExactResolved**
A real compiled semantic definition/artifact exists.

**PendingDefinition**
The semantic domain is authoritative but its producing subsystem has not yet emitted the corresponding artifact.

**InvalidDomain**
The reference is not valid in the authoritative semantic domain.

A numeric range check is never sufficient for ExactResolved.

Example:
```
SpeciesId 25
```
is ExactResolved only when semantic Species definition 25 actually exists in compiled game data.

### Package Closure

Script legality and package closure are different gates.

Script legality may allow PendingDefinition.

Before an EMON package is considered fully self-contained:
```
every ResourceRef
→ ExactResolved
```

No unresolved package dependency may survive final closure.

### Game-Specific Behaviors

Not every source-game action deserves a universal engine opcode.

Three outcomes exist:

**Generic engine semantic**
Example:
- warp
- give item
- play sound
- move actor
- fade screen

**Stable game behavior**
Use:
```
BehaviorId
→ BehaviorTable
```
for source-game behaviors that are legitimate game semantics but not universal engine primitives.

**Frontend implementation detail**
Absorb/eliminate only when source analysis proves it has no independent semantic effect.

Do not create one generic escape hatch carrying raw Crystal identity.

Specifically:
```
raw Crystal Special ID
```
is not a valid long-term runtime semantic.

### Lua Runtime Role

Do not build a custom general-purpose ScriptVM.

Lua is the runtime orchestration/codegen target after semantic compilation.

Conceptually:
```
SemanticScriptIR
→ generated Lua
→ typed ctx.* APIs
→ native C++ systems
```

Generated Lua is not compiler truth.
Never parse generated Lua back into game definitions.
Never make compiler correctness depend on emitted formatting.

Core mechanics remain native C++.

Lua orchestrates:
- scripts
- events
- dialog
- movement sequencing
- battle invocation
- game-specific behavior composition
- mods

Multi-frame operations may use coroutine/yield semantics.

### Script Execution State

Script runtime state is owned per runtime instance.
No global Crystal-style scratch state.

Use explicit semantic state such as:
- ScriptExecutionContext
- selected field actor
- pending field encounter
- script result/value

Session-persistent gameplay concepts must live at the correct lifecycle level rather than being accidentally cleared with transient script context.

---

## Mods

Mods compose once at startup:
```
base GameDefinition
→ apply mods
→ resolve dependencies
→ validate
→ freeze
→ run
```

Avoid layered override lookups during gameplay.

Mods may:
- add
- replace
- patch
- remove

semantic resources including:
- species
- types
- moves
- items
- behaviors
- scripts
- maps
- trainers
- encounters
- graphics
- materials
- audio
- animations
- UI
- color grades

Runtime sees the final frozen composition.

---

## Connected World Model

Crystal map connections compile into semantic spatial relationships.

Connected exterior maps may compose into a continuous world-space representation.
Interiors may remain independent spaces.

Do not retain original Game Boy screen/map hardware limitations as engine constraints.

For 3D rendering, avoid arbitrary short sightline limits inherited from the source platform.
Visibility/performance should be determined by modern scene management.

---

## Simulation Determinism

Gameplay simulation must not depend on:
- render FPS
- audio clock
- wall-clock frame duration
- GPU workload

RNG belongs to GameState.
It must be explicitly:
- seeded
- owned
- serialized
- restored

Gameplay timing uses simulation time.

Save/restore must capture every gameplay-relevant state variable.

This supports:
- fast-forward
- save/load integrity
- parity testing
- replays
- deterministic debugging

### Threading Model

Gameplay simulation is currently single-threaded.

This is an implementation simplification, not an architectural constraint.
Specifically:
- Gameplay state (GameState, ScriptExecutionContext) has no mutex protection
- HeadlessGameLoop::tick() assumes single-threaded access

Single-threaded simulation does NOT imply:
- Only one runtime instance may exist per process
- State must be process-global

Multiple runtime instances (Runtime A, Runtime B) may coexist in one process.
Each must own its own:
- GameState
- ScriptExecutionContext
- PackageContext (package reader + asset caches)
- HeadlessGameLoop
- LuaRuntime

No mutable gameplay state may be process-global.
Instance isolation must be guaranteed by ownership, not by global exclusion.

### Independent Clocks

```
Simulation → gameplay/scripts/battles/movement/animations
Rendering  → VSync / VRR / uncapped / frame capped
Audio      → independent real-time clock
RTC        → calendar/world time
```

Simulation may run:
- 1×
- 2×
- 4×
- 8×

Audio normally remains real-time during fast-forward, but this is policy rather than an invariant.

### RTC

RTC is independent from simulation speed.

Normal behavior:
```
system/calendar time + rtc_offset_seconds (from GameState)
→ effective game time
→ time of day
→ scheduled events
→ RTC-dependent encounters/content
```

Fast-forward does not advance calendar time.

`rtc_offset_seconds` is the persisted offset stored in `GameState`. Setting the in-game clock computes a new offset so that `effective_time = system_now + offset`. DST is absorbed into the offset (+3600 when enabled). Both fields are serialized in the native save format.

Supported modes:

| Mode | Behavior | Purpose |
|------|----------|---------|
| system | Real system time + stored offset | Normal play |
| fixed | Fixed timestamp | Tests/screenshots |
| offset | System time + offset | Testing/modding |

---

## Native Save Format

The canonical save format is the Enginemon native `.emon_save`. It is format-independent and does not know or care about the source frontend.

The current format is v6 (`magic = "ENGM"`, version = 6). Fields are little-endian with length-prefixed UTF-8 strings. All unordered containers are sorted canonically before serialization for byte-identical output.

Currently persisted:
- Player position, facing, map (semantic string ID), surfing/bike state
- Warp memory (last outdoor map + backup warp for LAST_MAP/LAST_WARP semantics)
- Event flags (`flag_XXXX` = EventFlag, `eflag_XXXX` = EngineFlag — namespaced)
- Variables: `money_player`, `money_mom`, `coins`, per-map scene state (`scene_<map_id>`), script variables (`var_N`), well-known state vars (`state_var_N`)
- Variable sprite assignments
- PCG-XSH-RR RNG state (restore_state — exact continuation, not re-seed)
- Day Care occupancy (species IDs)
- Playtime frames
- NPC states per map (position, facing, visibility, idle timer — restored after map load)
- Item bag (ItemId → quantity, sorted, quantity capped at 99)
- RTC offset + DST flag (v6+)

Not yet persisted (prerequisite for Crystal .sav codec):
- Party Pokémon
- PC boxes
- Pokédex owned/seen
- Trainer ID / player name
- Badges

### Crystal `.sav` Compatibility (Planned)

Crystal save compatibility is an optional frontend codec in `frontends/crystal/save/`. The native save is always authoritative. The `.sav` codec is a two-way bridge:

```
raw Crystal .sav (32 KB SRAM)
→ crystal_save_reader: validate sentinels + checksums, decode into CrystalSaveSnapshot
→ convert to GameState
→ attach SRAM shadow (opaque 32 KB buffer preserved for round-trip fidelity)
→ persist as .emon_save with optional XSRM chunk
```

Export is the reverse: start from the SRAM shadow (or a blank template for new games), patch known fields from GameState, recompute sentinels and checksums, emit `.sav`.

Unknown Crystal SRAM bytes (Hall of Fame, RTC hardware, mobile features) are never touched — they survive verbatim via the shadow. See `docs/SAVE_ARCHITECTURE.md` for the full Crystal SRAM layout, integrity scheme, and codec design.

---

## Mechanics Scope

### Crystal Frontend

Vanilla Crystal implements Gen 2 mechanics only:
- stats
- types
- moves
- move effects
- statuses
- weather
- held items
- trainer AI
- capture
- switching
- party state
- encounters
- items

Do not accidentally add later-generation behavior to vanilla Crystal semantics.

Examples:
- no Gen3 abilities
- no modern physical/special split
- no later-generation battle rules

### Mechanics Extensibility

Gen 2 mechanics must not be encoded as an architectural ceiling.

Prefer:
- registries
- typed definitions
- BehaviorId
- data-driven dispatch

Future frontends/mods may introduce:
- abilities
- new statuses
- new weather
- new move effects
- new item effects
- new battle behaviors

without replacing the runtime architecture.

Avoid giant hard-coded switches keyed to Crystal source IDs.

### Trainer AI

Trainer AI is behavior-driven:
```
Trainer
→ AI BehaviorId
→ native vanilla implementation / Lua mod behavior / registered implementation
```

Mods may replace or assign AI at trainer/class/definition level.

---

## Asset Philosophy

Graphics and audio follow the same principle:
```
source-faithful semantic/native data in package
→ modern runtime interpretation
```

Do not bake away useful source structure merely for convenience.

### Graphics Compiler Boundary

Crystal graphics are extracted and decoded at compile time.

```
Crystal compressed graphics
→ frontend decoder
→ native indexed assets
→ semantic asset IDs
→ EMON package
```

Runtime must never decode Crystal graphics formats.

Canonical 2D representation:
- indexed pixel data
- +separate palette definitions

Do not make pre-expanded RGBA the canonical asset representation.

Benefits include:
- small GPU footprint
- palette swapping
- palette animation
- recoloring
- modding

### Graphics Build Cache

Within a compilation, expensive extraction should be compute-once.

Cache source computation by semantic/source identity such as:
- asset kind
- canonical source symbol/resource
- decode parameters

Do not automatically merge unrelated semantic assets merely because their decoded bytes happen to match.

Persistent cross-build caching is optional and should be added only when profiling demonstrates value.

### Package Asset Representation

The package stores canonical native assets.

Prefer:
- flat immutable structures
- TOC
- CRC/integrity metadata
- stable typed IDs
- offset-based/package-relative references where required

No ROM pointer identity survives.

Avoid redundant representations such as storing both indexed and RGBA copies without a measured reason.

### Runtime CPU Asset Access

For Crystal-scale assets, prefer simple full availability over speculative streaming complexity.

Package layout should support efficient direct access and memory mapping where practical.

Conceptually:
```
package mapping
→ immutable asset view
```

Do not build a giant CPU asset cache merely because future 3D may eventually need streaming.
Keep APIs capable of evolving to streaming later without making current implementation complex.

### GPU Asset Residency

For current Crystal-scale content:
- upload once
- keep resident
- reuse

Prefer dense semantic-ID-indexed resource tables where IDs are appropriately dense.

Avoid hash lookup in hot per-frame rendering paths when a direct indexed lookup is available.

Do not build eviction machinery before real asset volume requires it.

### Vulkan Descriptor Architecture

Prefer bindless/descriptor-indexed Vulkan architecture from the start.

Avoid designing rendering around per-draw material descriptor rebinding if that would require a major rewrite for future 3D.

Renderer data should ultimately reference compact native resource/material handles.

### 2D Rendering

Canonical tile/sprite path:
```
indexed texture
→ palette LUT
→ final fragment color
```

Prefer:
- texture arrays where dimensions are uniform
- instanced batching
- compact per-instance data
- nearest-neighbor sampling

Potential per-instance data:
- position
- tile/layer
- palette
- material
- transform/flags

Do not bake giant metatile atlases merely because the source platform used metatiles.
Canonical architecture remains native 8×8 tile assets.

### Materials

MaterialId / MaterialDefinition are the long-term rendering abstraction.

For 2D, a material may be minimal:
- indexed texture resource
- palette resource
- sampling/blend configuration

Future 3D materials may add:
- albedo
- normal
- roughness
- metallic
- emissive
- opacity
- other physically based parameters

Do not prematurely make 2D sprites into 3D billboards.
The unification point is material/resource identity, not forced geometry semantics.

### Renderer Direction

Enginemon has a backend-agnostic renderer interface. Vulkan is the primary desktop backend. The rendering API is deliberately abstracted so OpenGL ES can target low-end Linux handhelds and Metal can target Apple platforms without changes to the engine simulation layer.

Target rendering progression:
```
native 2D (current)
→ native 3D
→ enhanced 3D
→ Vulkan RT-assisted / ray-traced rendering
```

The long-term renderer should exploit modern GPU capabilities aggressively where justified.

Performance priorities:
- minimal CPU render overhead
- aggressive batching
- compact draw data
- GPU-resident hot resources
- minimal synchronization
- minimal redundant copies
- bindless resource access
- modern Vulkan pipeline design

But do not implement speculative future machinery before the content requires it.

In particular:
- no BLAS/TLAS scaffolding before actual 3D geometry
- no RT cache before RT acceleration structures exist
- no premature texture streaming system

### Color Grading / LUTs

Color grading is a first-class future semantic asset.

```
ColorGradeId
→ ColorGradeDefinition
```

Authoring formats such as .cube are compiler inputs only.

```
.cube / supported authoring format
→ asset compiler
→ native ColorGradeDefinition
→ EMON/mod package
→ GPU LUT
```

Runtime does not parse authoring formats.

Mods may replace color grades through normal package composition:
```
base grade
→ mod override
→ validate
→ freeze
→ GPU upload
```

Color grading belongs in the modern post-processing pipeline.

Normally:
```
world render
→ lighting/RT
→ tone mapping
→ color grade LUT
→ world output
→ UI composition
```

UI should not be unintentionally color-graded with the world unless a presentation mode explicitly requests it.

Color-space behavior must be explicitly defined once HDR/advanced lighting exists.

---

## Presentation Requirements

Support:
- VSync
- low-latency present modes
- VRR-friendly presentation
- frame caps
- uncapped rendering
- nearest-neighbor sampling
- aspect-ratio preservation
- pixel integrity

2D:
- full-scene integer scaling

3D:
- native-resolution geometry
- original texel-grid fidelity for sprites/textures/UI where appropriate

Simulation must remain independent of all presentation timing.

### Presentation Freedom

Enginemon presentation is not constrained to Crystal's original viewport or display model. Native renderers may provide survey/zoomed world views, connected-map rendering, perspective/2.5D camera transforms, alternate palette presentation, widescreen/native UI layouts, LCD/GBC display simulation, color grading, and other shader-driven effects without altering authoritative gameplay semantics.

Shader customization, including KLang-authored effects where supported, belongs to the presentation/material pipeline. Shader programs must not become authoritative gameplay logic.

Camera projection, viewport aspect ratio, postprocessing, palette presentation, and UI layout are presentation policy. They must remain separable from map topology, battle state, script semantics, and deterministic simulation.

---

## Audio Architecture

Do not convert Crystal music into opaque GBS playback.
Do not make pre-rendered WAV/OGG stems the canonical representation.

Crystal's sound-engine data should compile into a semantic timing-faithful native representation:

```
Crystal sound data
→ frontend/compiler
→ AudioSequence / semantic events
→ EMON
→ native voice renderer
→ DSP/mixer
```

Preserve musically observable behavior:
- tempo
- timing/ticks
- note/rest duration
- pitch
- duty/instrument changes
- volume/envelopes
- pitch slides
- vibrato
- noise parameters
- stereo routing
- loops/repeats
- channel interaction
- SFX/music interaction

Do not reduce this to approximate MIDI if that loses Crystal behavior.

### Audio Rendering

The semantic sequence should support interchangeable voice banks:
- faithful GBC-style voices
- enhanced modern voices
- mod-provided voices

Same composition/event data, different rendering.

Modernization happens in:
- voice synthesis
- sampling
- anti-aliasing
- spatialization
- DSP
- mixing

not by altering the original composition semantics.

Potential DSP:
- reverb
- stereo width
- saturation
- EQ
- environmental effects

### Audio Buses

Provide semantic mixer buses such as:
- Master
- Overworld Music
- Battle Music
- Event Music
- SFX
- UI
- Ambience

Audio operates on its own real-time clock unless an explicit mode chooses otherwise.

---

## Cache Boundaries

Keep these concepts distinct:
- compiler compute cache
- EMON package storage
- runtime package/CPU view
- GPU residency
- Vulkan pipeline cache
- future streaming cache
- future RT acceleration-structure cache

Do not solve future cache problems before they exist.

Persisting Vulkan pipeline cache data is desirable where supported and correctly invalidated for device/driver compatibility.

---

## Package Boundary

The EMON package is the runtime source of truth.

Conceptually:

**Compiler:**
```
ROM
→ CrystalCompiler
→ semantic/native definitions
→ PackageWriter
→ EMON
```

**Runtime:**
```
EMON
→ PackageReader
→ FrozenGameData
→ runtime systems
```

Runtime must not include or depend upon Crystal frontend code.

Package content must be:
- native
- semantic
- validated
- self-contained after closure

### Package Format Discipline

Package IDs must be stable semantic identities.

Never use as runtime identity:
- ROM address
- ROM bank
- source table pointer
- source array order unless explicitly stabilized
- RGBDS symbol address

Package evolution should be explicit and versioned.

Integrity checks should use the existing EMON TOC/CRC infrastructure rather than inventing unrelated parallel formats.

### Build Cache Identity

The package cache key is derived from four fields:
- `rom_sha1`: the actual input ROM's SHA-1 (computed from raw ROM bytes — NOT the profile's hardcoded SHA)
- `compiler_version`: semantic compiler version string
- `format_version`: EMON package format version
- `options_hash`: hash of compilation options

Two different ROMs that share the same table layout (e.g., vanilla Crystal and a compatible ROM hack) will have different `rom_sha1` values and therefore different cache keys. A cached package from ROM A can never satisfy a lookup for ROM B.

---

## Reference Projects

Clone locally as siblings to Enginemon:

| Repository | Purpose |
|------------|---------|
| **pret/pokecrystal** | Primary authoritative Crystal source/semantic specification |
| **UNDERdecoded/Gen2Recomped** | Modern Gen2 implementation/reference |
| **DanZC/suiCune** | ASM→C/mechanics reference |
| **froggestspirit/suiCune** | Historical translation/tooling reference |
| **bryanthaboi/gen1recomp** | Gen1 recompilation/modding/content reference |
| **artyrambles/DRAMALESS_SHAPE** | 3D/voxel presentation reference |

Additional documentation as needed:
- RGBDS
- Vulkan
- SDL3
- Lua

---

## Reference-First Implementation — HARD RULE

Before manually implementing or interpreting any Crystal/Gen2 subsystem:

```
pokecrystal
→ Gen2Recomped
→ suiCune
→ actual ROM/package/golden evidence
→ Enginemon
```

Use references proactively.
Do not wait for tests to fail before checking known authoritative implementations.

### Reference Priority

**pokecrystal**
Authoritative source-equivalent semantics.
Use it to determine what Crystal actually does.

**Gen2Recomped**
Modern implementation/reference.
Use aggressively for semantic decomposition and already-understood systems.
Do not assume its runtime architecture is Enginemon's architecture.

**suiCune**
ASM→C/mechanics reference.
Useful for tracing lower-level Crystal behavior into understandable logic.

**Actual ROM / Golden Fixtures**
Resolve source/profile details and validate assumptions.

### Reference-First Does Not Mean Architecture Copying

Preserve proven semantic decomposition where useful.

Do not copy another project's:
- emulation architecture
- memory model
- VM
- runtime ownership
- hardware abstractions
- renderer architecture

unless it independently fits Enginemon's native semantic design.

Copy semantics, not baggage.

---

## Investigation Discipline

When behavior is uncertain:
- source/static trace first

Then:
- cross-reference implementations
- inspect actual ROM/package fixtures
- use focused instrumentation only if ambiguity remains

Do not start with:
- large logging systems
- random runtime probes
- speculative theories

Instrumentation should answer one concrete unresolved question and then disappear unless it has lasting diagnostic value.

---

## Production vs Test Code

Tests must exercise production implementations.

Do not maintain parallel test-only versions of:
- map discovery
- script discovery
- registries
- decoders
- linkers
- extractors
- semantic rules

A test implementation becoming more correct than production is an architectural failure.

---

## Runtime/Compiler Separation

Production runtime code must not include:
- `frontends/crystal/*`
- Crystal decoder structures
- ROM extraction helpers
- RGBDS lookup helpers

Compiler evidence/provenance belongs compiler-side only.

---

## No Giant Frameworks

Keep Enginemon purpose-built.

Avoid introducing:
- general ECS frameworks
- MLIR
- SSA infrastructure
- generic compiler frameworks
- rule-engine DSLs
- computed-jump solvers
- generic emulator compatibility layers
- giant asset frameworks

unless concrete requirements demonstrate that the simpler architecture cannot solve the problem.

---

## Accuracy Strategy

This is primarily a compiler/port coverage problem, not blind reverse engineering.

Use:
```
pokecrystal
+ Gen2Recomped
+ suiCune
+ actual ROM
+ golden/reference fixtures
→ differential/parity validation
```

Test at least:
- script semantics
- movement
- warps
- collision
- maps
- inventory
- flags/variables
- text
- encounters
- battle calculations
- RNG
- status
- trainer AI
- field moves
- audio sequencing
- save/load

---

## Legality Gate Must Be Negative-Tested

The legality gate itself is part of the trusted compiler boundary and must be tested adversarially.

It is not sufficient to show that valid corpus scripts pass.

Tests must deliberately construct invalid scripts/IR and prove the gate rejects them for each enforced invariant, including at minimum:
- unknown opcode decode/round-trip failure
- incomplete decode
- invalid CFG target
- unclosed/unresolved CFG
- orphan command
- command assigned inconsistently
- unresolved native classification
- unlowered semantic command
- source-command accounting mismatch
- diagnostic residue
- invalid semantic ID/reference

When a new legality invariant is added:
```
implementation → positive coverage → focused negative test proving rejection
```
must land together.

A legality check without a test demonstrating failure on a known-bad input is not considered proven.

---

## Agent Rules

Agents working on Enginemon must follow these rules:

1. Read this steering document before architectural work.

2. Inspect authoritative references before inventing Crystal behavior.

3. Do not claim something is semantic merely because its C++ type was renamed.

4. Do not create fallback runtime paths for compiler uncertainty.

5. Prefer explicit failure over degraded package output.

6. Never hide uncertain behavior behind generic-looking abstractions.

7. A visible unresolved compiler case is preferable to fake semantic correctness.

8. Reuse production implementations in tests.

9. Remove speculative/dead code created during abandoned approaches.

10. Do not broaden the current milestone without instruction.

11. Do not build future infrastructure merely because it might eventually be useful.

12. Do preserve architectural seams that would otherwise require major rewrites later.

13. When reporting success, provide structural/source evidence, not merely green test counts.

14. Never use raw source identity as a runtime semantic ID.

15. Keep the engine small, explicit, native, and inspectable.

16. Every hard compiler gate must have adversarial negative tests proving it rejects known-invalid input; happy-path corpus success is not sufficient.

---

## Current Development Discipline

Work in narrow milestones.

Typical cycle:
```
one bounded task
→ source/reference research
→ implementation
→ focused tests
→ fresh-agent/red-team audit
→ fix violations
→ freeze
→ next task
```

Do not advance because an implementation agent says "all tests pass."
Architecture conformity must be independently checked.

---

## Multiplayer Compatibility and Determinism

Multiplayer operates on the frozen composed game definition, not merely on locally meaningful numeric semantic IDs.

Two peers may only exchange semantic resource references when both sides agree on resource identity.

### Composition Compatibility

Numeric semantic IDs are local implementation identities and are not assumed to match between independently composed installations.

Before a multiplayer session begins, peers must establish compatibility through one or both of:
- frozen composition/content hash agreement
- stable authoring/resource keys resolved into each peer's local semantic IDs

For strict synchronized simulation, the preferred default is:
```
same frozen composition → same composition hash → session allowed
```

Do not serialize/transmit a local SpeciesId, ItemId, BehaviorId, etc. and assume the same integer has the same meaning on another independently composed runtime.

Cross-composition multiplayer, if supported later, requires an explicit semantic compatibility/mapping layer.

### Cross-Machine Determinism

Deterministic RNG alone is not sufficient for synchronized multiplayer.

All gameplay-authoritative calculations used by input-synchronized simulation must produce identical results across supported machines.

Prefer deterministic integer/fixed-point arithmetic for gameplay mechanics where practical.

Gameplay-authoritative state transitions must not depend on:
- CPU floating-point implementation differences
- SIMD reassociation
- compiler fast-math behavior
- renderer/GPU calculations
- platform-specific transcendental results

Floating point may be used freely for presentation where its result cannot affect simulation.

If gameplay floating-point is ever introduced, its cross-platform determinism contract must be explicitly proven before it participates in synchronized multiplayer.

### Network Session Simulation Rate

A synchronized multiplayer session uses one agreed simulation cadence.

Local rendering remains independent, but simulation fast-forward is not independently selectable by each peer.

| Clock | Ownership |
|-------|-----------|
| rendering | independent per client |
| audio | local policy |
| simulation tick rate | session-authoritative |
| RTC | synchronized/defined by session policy where gameplay-relevant |

Normal 1× / 2× / 4× / 8× local fast-forward must either:
- be disabled during synchronized multiplayer
- or change only through an agreed session-wide control

A client must never advance authoritative simulation faster than its peers merely because local fast-forward was enabled.

### Network Battle Reuse Condition

Native multiplayer battles reuse the same BattleState and mechanics implementation as local battles.

This is valid because vanilla Gen 2 battle input is discrete/turn-based and can be synchronized as semantic player decisions.

```
synchronized BattleState
+ deterministic mechanics
+ deterministic GameState RNG
+ ordered player decisions
→ identical battle result
```

Do not create a parallel network battle engine.

If a future frontend/mod introduces timing-sensitive or real-time battle mechanics, its synchronization model must be explicitly extended rather than assuming the vanilla turn-based protocol remains sufficient.

---

## Palette Resources and Modding

Palette customization uses the normal semantic resource/mod composition system.

```
PaletteId → PaletteDefinition → MaterialDefinition / indexed asset reference
```

Mods may:
- replace palettes
- add palettes
- patch palette bindings
- animate/select palettes
- define richer palettes than the source frontend

### Source Constraints Are Not Runtime Limits

Pokémon Crystal may emit source-faithful small indexed palettes, but Enginemon must not encode Crystal's palette size/count as a universal runtime limit.

A future frontend or mod may define:
- more palette entries
- more palette rows
- richer native color precision
- animated palette resources
- event/time-driven palette selection

without changing indexed pixel data.

### GPU Palette Validation

The GPU indexed-palette path must use the actual compiled PaletteDefinition capacity rather than assuming Crystal's original fixed palette dimensions.

At package/mod validation time:
- indexed texel range
- palette entry count
- material palette binding
- GPU palette resource capacity

must agree.

A mod defining an 8-color or larger palette must either be supported correctly by the active renderer path or rejected explicitly during validation.

Silent truncation/wrapping/misrendering is not acceptable.

### Palette Authoring

Human-facing formats such as:
- paletted PNG
- .gpl
- .pal
- other editor formats

are authoring/compiler inputs only.

They compile into native PaletteDefinitions before runtime.

No authoring palette format is parsed by the runtime.

---

## End Goal

Enginemon should ultimately make a Pokémon game's source platform almost irrelevant to the runtime.

The frontend understands the original game.
The package describes the game semantically.
The runtime executes that game natively.

```
Original ROM
↓
source-faithful compiler
↓
clean semantic game package
↓
tiny high-performance native runtime
↓
faithful 2D
or
modern 3D
or
enhanced / RT-assisted presentation
```

The gameplay should remain the game.
The machine underneath it does not have to remain 1999 hardware.
