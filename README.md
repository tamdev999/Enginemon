# Enginemon

A native C++23 Pokémon Crystal runtime that compiles a user-supplied ROM into a clean native format, then runs entirely without Game Boy emulation.

## Architecture

```
Crystal ROM → Crystal Frontend → GameDefinition → Native Engine → 2D/3D/3D+ Rendering
                    ↓
             Generated Lua Scripts
                    ↓
         Same API as Mod Scripts
```

### Key Design Principles

1. **Crystal is a compiler frontend, not runtime** - All ROM offsets, RGBDS symbols, banking, and Crystal-specific formats live in `frontends/crystal/`. The shipped runtime only understands native semantic data.

2. **Scripts compile to Lua** - Crystal's event scripts are decoded into a small IR, then emitted as Lua. Generated scripts and mod scripts use the same `ctx.*` API.

3. **Semantic boundaries everywhere** - Translate intent, not hardware operations. `Crystal movement → Lua ctx.world:move_actor() → native World::move_actor()`, not OAM writes.

4. **Modding via startup composition** - Mods modify a mutable GameDefinition at startup, then freeze. No expensive layered lookups at runtime.

5. **Connected exterior maps form continuous world** - Crystal map connections are resolved into shared world coordinates. 3D mode supports very long sightlines.

## Directory Structure

```
enginemon/
├── CMakeLists.txt           # Root build
├── frontends/
│   └── crystal/             # Crystal ROM compiler (all ROM knowledge here)
│       ├── rom/             # ROM loading, symbols, validation
│       ├── extract/         # Data table extraction
│       ├── script/          # Script decoder → IR → Lua emitter
│       ├── world/           # Map connection resolver
│       └── output/          # GameDefinition writer
├── engine/                  # Native runtime (no ROM knowledge)
│   ├── core/                # GameDefinition, registries, types
│   ├── world/               # Movement, collision, maps
│   ├── battle/              # Battle system (Gen 2 mechanics)
│   ├── party/               # Party, PC, Pokemon instances
│   ├── save/                # Native save format
│   ├── ui/                  # Menu system, dialog
│   ├── audio/               # AudioSequence, synthesis, mixing
│   ├── scripting/           # Lua runtime, C++ API bindings
│   └── mod/                 # Mod loader, composition
├── runtime/                 # Executable
│   ├── app/                 # Application lifecycle
│   ├── render/              # Vulkan 2D/3D/3D+
│   └── platform/            # SDL3 windowing, input, audio device
├── tools/                   # Standalone utilities
└── tests/                   # Unit and parity tests
```

## Building

### Prerequisites

- CMake 3.25+
- C++23 compiler (MSVC 2022, GCC 13+, Clang 16+)
- Vulkan SDK
- SDL3
- Lua 5.4

### Build Commands

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build .
```

### Build Options

- `ENGINEMON_BUILD_TOOLS` - Build standalone tools (default: ON)
- `ENGINEMON_BUILD_TESTS` - Build test suite (default: ON)
- `ENGINEMON_ENABLE_RT` - Enable raytracing support (default: OFF)

## Usage

### Compile a ROM

```bash
./crystal_compile path/to/crystal.gbc -o compiled_game/
```

This produces:
- `compiled_game/game.def` - GameDefinition binary
- `compiled_game/assets/` - Sprites, tiles, audio
- `compiled_game/scripts/` - Generated Lua scripts

### Run the Game

```bash
./enginemon compiled_game/
```

### Mods

Place mods in `compiled_game/mods/`. Each mod is a directory with:
- `mod.json` - Metadata, dependencies
- `scripts/` - Lua scripts
- `assets/` - Custom assets
- `data/` - Data overrides

## Reference Projects

These are development references - clone separately, don't include in this repo:

| Project | Purpose | Usage |
|---------|---------|-------|
| [pret/pokecrystal](https://github.com/pret/pokecrystal) | Semantic specification | Build for `.sym` files, reference ASM for data layouts |
| [suiCune](https://github.com/mid-kid/suiCune) | ASM→C translation patterns | Reference for mechanics translation |
| [Dramaless Shape](https://github.com/DramaticShape/gen1recomp-voxel-mod) | 3D voxel technique | Study approach, implement independently |
| [Gen1Recomp](https://github.com/APokemon/Gen1Recomp) | ROM-as-content-source | Reference for ROM import, Lua patterns |

None become runtime dependencies - they're reference material only.

## Timing & Presentation

Independent clocks for simulation, rendering, and audio:

```
Simulation → 1× / 2× / 4× / 8× (gameplay, scripts, battles, movement)
Rendering  → VSync / uncapped / frame cap / VRR
Audio      → Always real-time 1× (music doesn't speed up)
```

**Fast-forward** accelerates gameplay without forcing music speedup.

**Presentation features:**
- VSync, low-latency present modes, VRR-friendly
- Frame caps / uncapped rendering
- Nearest-neighbor sampling for pixel integrity
- Aspect-ratio preservation
- 2D: Full-scene integer scaling
- 3D: Native resolution geometry, original texel grid on textures/sprites/UI

## Trainer AI Extensibility

Registry/behavior-driven AI system:

```
Trainer → AI BehaviorId → Implementation
                           ├── Vanilla native C++ (default Crystal)
                           ├── Lua mod AI (scripted)
                           └── Custom native AI (C++ plugin)
```

Mods can:
- Replace vanilla AI entirely
- Assign different AI to specific trainers/classes
- Register new AI implementations

## Mechanics Extensibility

**Vanilla scope:** Gen 2 only (no abilities, etc.)

**Architecture:** NOT hard-coded to Gen 2 limitations. Future mods/frontends can register:
- New status types
- New weather types
- Abilities (via behavior registry)
- New move effects
- New item effects

All through the existing data/behavior systems without engine changes.

## Accuracy Approach

This is a port/compiler coverage problem, not reverse engineering. We use:
- pokecrystal as the semantic specification
- suiCune as translation reference
- Reference execution for differential/parity testing

Test coverage includes: scripts, battle calculations, RNG, status, AI, movement, collision, inventory, flags, encounters, saves.

## External Dependencies

Runtime dependencies (must be installed):
- **Vulkan SDK** - Graphics
- **SDL3** - Windowing, input, audio device
- **Lua 5.4** - Scripting
- **C++23 compiler** - MSVC 2022, GCC 13+, or Clang 16+

Build dependencies:
- **CMake 3.25+**
- **GTest** (optional, for tests)

## License

MIT
