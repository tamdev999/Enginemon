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

**Tracked revision:** `8e8f7e20` (master, Aug 2026)  
**Latest upstream:** `7a7881d0` — `Sync the make_patch tool with poketcg` (tooling only, no semantic changes)  
**Status:** Up to date for Enginemon purposes. The `kanzure/fix-buena-password` branch is unmerged.

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

**Tracked revision:** `201d7002` (main, Aug 2026)  
**Latest upstream:** `2d9149c5`

**Upstream findings since `201d7002`:**

- `wGameTimeHours` is **big-endian u16** (`8619bd0b`, confirmed Aug 2026).  
  `util/serialize.c` was fixed from `TY_U16LE` → `TY_U16BE`.  
  Crystal stores `wGameTimeHours` big-endian in WRAM (consistent with `wCoins`,
  `wMoney` big-endian patterns). The save codec must decode it as big-endian
  when Phase 2 adds play-time to the snapshot.

- TM/HM movepool bit array has an off-by-one in JSON loading (`2d9149c5`).  
  The `wTMsHMs` bit array field (57 bytes in the checksummed SRAM region) is
  affected. Relevant when Phase 2 save work encodes TM/HM data.

- Variable sprite recursion fix (`6c36f344`): `v_DoesSpriteHaveFacings()` must
  recurse into `variableSprites[]` for IDs ≥ SPRITE_VARS.

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

### UNDERdecoded/Gen2Recomped
**Purpose:** Modern Gen2 implementation/reference

```bash
git clone https://github.com/UNDERdecoded/Gen2Recomped.git
```

**What we use:**
- ROM extraction patterns (`src/import/RomExtractorGen2.lua`)
- World/map/warp semantics cross-reference
- Save layout cross-reference
- Script VM as behavioral reference

**Tracked revision:** `401bc25` (tag: 0.5.1b-hotfix, main, Aug 2026)  
**Latest upstream:** `fd1ba38` (tag: v0.7.35)

**Upstream findings since `401bc25`:**

- **Warp-carpet direction gate** (`524666c`, v0.7.31): `Warp.onArrive()` now
  implements Crystal's `CheckDirectionalWarp` — carpet tiles (`COLL_WARP_CARPET_*`)
  only fire when the player walks in the carpet's own direction; they do not fire
  as instant trapdoors on any step. Enginemon already implements this correctly
  via `CollisionClass` carpet detection. The Gen2Recomped commit has the best
  inline commentary on Crystal's `CheckWarpTile` / `CheckDirectionalWarp` split
  (`src/world/Warp.lua`).

- **Roaming legendaries** (`524666c`, v0.7.31): `src/world/RoamMons.lua` (new,
  147 lines) implements `CheckEncounterRoamMon` including the 100/256 encounter
  rate and beast-staging logic with Crystal assembly cross-references. Reference
  material for when Enginemon implements roaming encounters.

- **Crystal EngineFlag numbering** (`2ebdf2c`, v0.7.32): Crystal's EngineFlag
  table has one more row than Gold's from index 16 up. `Gen2Save.lua` was fixed
  to use `scriptFlag` naming for Crystal (not `engineFlag`) to reflect this.
  Relevant to `FlagNamespace::Engine` exact bit offsets when Crystal save Phase 2
  decodes engine flags from SRAM.

- **Crystal support** (`8f7de1f`, `ae144db`): Large-scale Crystal ROM import
  added. `RomExtractorGen2.lua` has substantially more Crystal-aware extraction.
  Useful as a cross-check when auditing Enginemon's own Crystal extraction.

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
