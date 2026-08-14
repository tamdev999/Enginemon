# Reference Projects for Development

These projects are development references. Clone them **separately** from Enginemon - they are not dependencies, but provide documentation, translation patterns, and parity sources.

## Reference Code Policy

**Enginemon must not require any of these at runtime.**

Before directly copying implementation code, verify its license/provenance. Prefer clean native implementations based on understood semantics.

---

## Clone Locally

### pret/pokecrystal
**Purpose:** Primary Crystal semantic specification

```bash
git clone https://github.com/pret/pokecrystal.git
cd pokecrystal
make  # Generates .sym files
```

**What we use:**
- `pokecrystal.sym` - Symbol table mapping names to ROM addresses
- `macros/scripts/events.asm` - Script opcode definitions
- `data/` - Data table layouts (Pokemon, moves, items, maps)
- `engine/` - Game logic reference

---

### DanZC/suiCune
**Purpose:** Current ASM→C/mechanics translation reference

```bash
git clone https://github.com/DanZC/suiCune.git
```

**What we use:**
- C99 translations of Crystal routines
- Battle mechanics implementations
- Verified translations against original behavior

This is the actively maintained C port using SDL2.

---

### froggestspirit/suiCune
**Purpose:** Historical translation/tooling reference

```bash
git clone https://github.com/froggestspirit/suiCune.git
```

The older suiCune work. Useful for historical context and alternative approaches.

---

### bryanthaboi/gen1recomp
**Purpose:** Lua scripting/modding and ROM-content-source reference

```bash
git clone https://github.com/bryanthaboi/gen1recomp.git
```

**What we study:**
- ROM import and cache generation
- Lua orchestration patterns
- How to release ROM after import
- Scripting/modding architecture

**Key distinction:**
- Gen1Recomp: Hand-written Lua engine + ROM data
- Enginemon: Compiled ROM scripts + native C++ engine

---

### artyrambles/DRAMALESS_SHAPE
**Purpose:** Voxel/extrusion/3D presentation reference

```bash
git clone https://github.com/artyrambles/DRAMALESS_SHAPE.git
```

**What we study:**
- Voxel diorama presentation
- Depth-buffered occlusion
- Leaning sprite slabs
- Shadow mapping
- Tilt-shift / world curvature
- Camera modes (overhead, over-the-shoulder)

Based on the earlier Dramatic Shape work. Reference only - Enginemon implements independently in C++/Vulkan.

---

## Documentation (Web Fetch)

- **RGBDS** - https://rgbds.gbdev.io/docs/ - Symbol file format, assembler docs
- **Vulkan** - https://vulkan.lunarg.com/doc/sdk - Graphics API
- **SDL3** - https://wiki.libsdl.org/SDL3 - Platform abstraction

---

## Directory Structure

Reference projects are in `references/` inside Enginemon (gitignored):

```
Enginemon/
├── references/             # Gitignored - not part of Enginemon
│   ├── pokecrystal/
│   ├── suiCune/            # DanZC version
│   ├── gen1recomp/
│   └── DRAMALESS_SHAPE/
├── engine/
├── frontends/
└── ...
```

The `references/` folder is in `.gitignore` so these repos don't get committed to Enginemon.
