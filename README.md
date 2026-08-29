# Enginemon

Native C++23 Pokémon compiler and runtime. Compiles a source ROM into a clean semantic EMON package, then runs entirely without Game Boy emulation.

---

## What it is

Enginemon is a compiler pipeline and native runtime platform.

The compiler frontend reads a raw ROM, identifies it by SHA-1 hash against a registered profile (Crystal retail, Gold/Silver, ROM hacks, eventually RBY and Emerald), and compiles it into a format-independent EMON package. The runtime loads that package and runs the game natively — no SM83 execution, no banked memory, no VRAM simulation.

```
ROM (any supported source)
  ↓
SHA-1 → exact profile selection
  ↓
frontend compiler (Crystal / G/S / RBY / Emerald / hack profile)
  ↓
semantic EMON package
  ↓
native runtime
  ↓
Vulkan 2D / 3D / RT rendering
```

The Crystal frontend is the first and currently operational target.

---

## Architecture

### Compiler / Runtime boundary

Everything source-game-specific lives in `frontends/`. The runtime (`engine/`, `runtime/`) knows nothing about ROM addresses, bank numbers, Crystal opcodes, RGBDS symbols, or GBC hardware.

```
frontends/crystal/    ← ROM knowledge, all of it
  rom/                  SHA-1 identification, profile registry, validation
  extract/              Data table extraction (maps, tilesets, sprites, species)
  script/               Typed decode → CFG → SemanticScriptIR → legality gate
  compile/              Full compiler pipeline, corpus discovery, linker
  output/               PackageWriter → EMON

engine/               ← No ROM knowledge
  core/                 GameState, HeadlessGameLoop, RTC, save format
  world/                Maps, collision, WorldManager, interaction
  scripting/            LuaRuntime, Lua API bindings (ctx.*)
  build/                PackageCache, BuildIdentity

runtime/              ← Vulkan renderer, SDL3, presentation
  main_tiles.cpp        Entry point, GPU-backed transition_to_map
  render/               TileRenderer, SpriteRenderer, TextboxRenderer
```

### Script pipeline

Crystal event scripts go through a staged compiler:

```
Crystal bytecode
  → typed lossless CrystalCommand IR
  → CFG
  → NativeCallRegistry + RamAddressRegistry classification
  → SemanticScriptIR lowering
  → hard per-script legality gate
  → corpus-wide typed-reference linker
  → Lua emission
  → runtime (LuaRuntime + ctx.* C++ API)
```

Generated Lua and mod Lua use the same `ctx.*` API. The legality gate is hard — no degraded output, no fallback escape hatches.

### Native save format

GameState is serialized to a versioned binary `.emon_save` (current: v6, magic `"ENGM"`). Fields include player position, flags, variables, items, money, coins, NPC states per map, RNG state (PCG-XSH-RR), RTC offset, day care, and more. Not yet implemented: party Pokémon, PC boxes, Pokédex (these are required before Crystal `.sav` import/export works).

Crystal `.sav` compatibility is a planned frontend codec in `frontends/crystal/save/`. The design uses a shadow-patch model: import the raw 32 KB SRAM, decode known fields into native GameState, preserve unknown bytes verbatim for round-trip fidelity. See [docs/SAVE_ARCHITECTURE.md](docs/SAVE_ARCHITECTURE.md).

### Rendering

Backend-agnostic renderer interface. Vulkan is the primary desktop backend. The interface is abstracted so OpenGL ES can target low-end Linux handhelds and Metal can target Apple platforms. Current target is native 2D; the path progresses toward 3D, enhanced 3D, and RT-assisted rendering.

### Simulation

Gameplay simulation runs at fixed 60 Hz independent of render rate. RNG is PCG-XSH-RR, owned by GameState, serialized and restored exactly. RTC is a persistent offset over the system clock — fast-forward does not advance calendar time. Multiple runtime instances can coexist in one process with no shared mutable state.

---

## Current status (Crystal frontend)

- **1788 scripts** compiled, legality-gated, and linked
- Full overworld: collision, NPC movement and visibility, warps, map connections
- Scripted warps with atomic prepare/commit transactions
- NPC save state snapshot/restore across map transitions
- Item bag, money (player/mom/coins), flags, variables, per-map scene state
- RTC with persistent offset and DST
- Vulkan 2D renderer: tilesets, sprites, textbox, time-of-day palettes
- Persistent package cache keyed by actual ROM SHA-1

---

## Building

**Windows (primary):**

```powershell
# Use the bundled CMake, not devkitPro/MSYS2 cmake
& "C:\Program Files\CMake\bin\cmake.exe" `
  -B build `
  -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake `
  -DENGINEMON_ENABLE_ENGINE=ON `
  -DENGINEMON_ENABLE_VULKAN=ON

& "C:\Program Files\CMake\bin\cmake.exe" --build build --target enginemon_tiles --config Release
```

**Run (requires a compiled EMON package):**

```powershell
# Compile ROM to package
.\build\tools\Release\emon_compile.exe "crystal.gbc" "crystal.emon"

# Run
.\build\runtime\Release\enginemon_tiles.exe "crystal.emon"
```

---

## Tests

```powershell
$rom = "references\Pokemon - Crystal Version (UE) (V1.1) [C][!].gbc"

.\build\tests\Release\runtime_test.exe $rom          # 587 tests
.\build\tests\Release\golden_test.exe $rom            # 56 golden fixtures
.\build\tests\Release\legality_gate_test.exe $rom     # 14 adversarial negative tests
.\build\tests\Release\corpus_test.exe $rom            # 1300 scripts hard legality
.\build\tests\Release\linker_test.exe $rom            # 1788 linked, InvalidOwnership=0
.\build\tools\Release\corpus_lowering_audit.exe $rom  # 1788 bodies
.\build\tests\Release\oracle_test.exe $rom            # 111 oracle/behavioral tests
.\build\tests\Release\compiler_integrity_test.exe $rom # 25 fail-closed adversarial tests
```

---

## References

| Repository | Role |
|------------|------|
| [pret/pokecrystal](https://github.com/pret/pokecrystal) | Authoritative Crystal source / semantic spec |
| [UNDERdecoded/Gen2Recomped](https://github.com/UNDERdecoded/Gen2Recomped) | Modern Gen2 reference implementation |
| [DanZC/suiCune](https://github.com/DanZC/suiCune) | ASM→C mechanics reference |
| [bryanthaboi/gen1recomp](https://github.com/bryanthaboi/gen1recomp) | Gen1 recompilation reference |

Clone alongside this repo as siblings. None are runtime dependencies.

---

## License

MIT
