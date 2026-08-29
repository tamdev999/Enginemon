# Enginemon Save Architecture

## Overview

Enginemon uses its own native save format as the source of truth. Crystal `.sav`
compatibility is an optional frontend codec — a two-way bridge between the native
format and the real 32 KB SRAM image — implemented entirely inside
`frontends/crystal/save/`.

```
Enginemon Native Save (.emon_save)
        ↑↓
  Crystal Save Codec               ← frontends/crystal/save/ only
        ↑↓
Crystal .sav (32 KB SRAM)
```

The engine never sees raw SRAM bytes. The Crystal frontend never touches the
native save format except through the codec's defined interface.

---

## Part 1 — Native Enginemon Save

### Purpose

The native save is the canonical persistent state for every Enginemon runtime
instance. It is format-independent: it does not know or care whether the game
was originally a Crystal ROM, a mod, or a later frontend. It is also
platform-independent: it serializes as a flat byte sequence with a versioned
header, suitable for any backing store (file, cloud, embedded flash, etc.).

### Current format version: `v6` (magic `"ENGM"`, version field `6`)

The native save is defined in `engine/core/game_state.hpp` and serialized in
`engine/core/game_state.cpp`. All fields are little-endian. Strings are
length-prefixed (4-byte LE length followed by UTF-8 bytes). Unordered containers
(flags, variables, NPC states) are sorted canonically before serialization so
that byte-identical saves are reproducible.

### Wire layout (v6)

```
Header
  [u32]  magic = 0x454E474D ("ENGM")
  [u32]  version = 6

Player
  [str]  current_map_id           — semantic map string ID (e.g. "new_bark_town")
  [i32]  x                        — tile column
  [i32]  y                        — tile row
  [u8]   facing                   — Direction (0=Down 1=Up 2=Left 3=Right)
  [u8]   surfing                  — bool
  [u8]   on_bike                  — bool

Warp memory  (for LAST_MAP / LAST_WARP exits)
  [str]  map_id                   — last outdoor map
  [i32]  x
  [i32]  y
  [str]  backup_map_id            — wBackupWarp equivalent
  [i32]  backup_x
  [i32]  backup_y

Event flags  (sorted)
  [i32]  count
  [str×count] flag_id strings     — "flag_XXXX" (EventFlag) / "eflag_XXXX" (EngineFlag)

Variables  (sorted by key)
  [i32]  count
  [(str key, i32 value) × count]  — includes money, scene, state_vars, etc.
      "money_player"              — wMoney
      "money_mom"                 — wMomsMoney
      "coins"                     — wCoins (Game Corner)
      "scene_<map_string_id>"     — per-map scene (setscene/checkscene)
      "map_scene_XXXX"            — per-map scene by hex numeric MapId (setmapscene)
      "state_var_N"               — well-known state vars (N = WellKnownStateVar id)
      "var_N"                     — wScriptVar slots

Variable sprites  (sorted by slot name)
  [i32]  count
  [(str slot, str sprite_id) × count]

RNG state
  [u64]  PCG-XSH-RR internal state (restore_state, not re-seed)

Day Care occupancy
  [i32]  slot1_species            — 0 = empty; 1–N = SpeciesId
  [i32]  slot2_species

Playtime
  [u64]  playtime_frames

NPC states per map  (sorted by map_id)
  [i32]  map_count
  per map:
    [str]  map_id
    [i32]  npc_count
    per NPC:
      [u16] id
      [i32] x, y
      [u8]  facing
      [bool] is_moving
      [i32] idle_timer, target_x, target_y, move_progress
      [bool] frozen, visible

Item bag  (sorted by ItemId)
  [i32]  item_count
  [(u32 item_id, i32 qty) × item_count]   — qty validated 1–99

RTC
  [i64]  rtc_offset_seconds       — effective_time = system_now + offset
  [bool] rtc_dst_enabled
```

### Version migration

| Version | Changes | Migration path |
|---------|---------|---------------|
| v4 | LCG RNG (two u64: seed + state) | `seed(legacy_state)` → PCG init |
| v5 | PCG state (one u64); no items/RTC | items = empty, RTC = 0 |
| v6 | Current: adds items bag + RTC fields | — |

Versions older than v4 are rejected. Unknown versions are rejected. A save with
trailing bytes is rejected. Corrupt or truncated saves never silently become a
new-game state; `try_deserialize()` returns an explicit `DeserializeError`.

### What the native save currently covers

| Domain | Status | Notes |
|--------|--------|-------|
| Map / player position | ✓ | semantic string map ID |
| Warp memory | ✓ | LAST_MAP + backup warp |
| Event flags | ✓ | `flag_XXXX` / `eflag_XXXX` namespaced |
| Script variables | ✓ | `var_N` + well-known state vars |
| Money (player/mom/coins) | ✓ | three separate keys |
| Per-map scene state | ✓ | `scene_<string_id>` |
| Item bag (player pocket) | ✓ | v6+ |
| NPC runtime state | ✓ | position/facing/visibility per map |
| RTC offset | ✓ | v6+ |
| Day Care occupancy | ✓ | species IDs only |
| Party Pokémon | ✗ | **prerequisite for .sav codec** |
| PC boxes | ✗ | **prerequisite for .sav codec** |
| Pokédex owned/seen | ✗ | **prerequisite for .sav codec** |
| Trainer ID / player name | ✗ | **prerequisite for .sav codec** |
| Rival name | ✗ | |
| Badges | ✗ | |
| TMs / HMs | ✗ | |
| Hall of Fame | ✗ | |

The missing fields are the primary prerequisite for a useful Crystal `.sav`
codec. The architecture below is designed so you can add fields to `GameState`
incrementally — each addition automatically becomes part of the `.emon_save`
wire format and feeds the codec.

---

## Part 2 — Crystal `.sav` Codec

### What a Crystal `.sav` is

A Crystal save is exactly 32,768 bytes (32 KiB) of battery-backed SRAM.
An emulator `.sav` file is this image verbatim. The file is divided into four
8 KiB banks (SRAM banks 0–3):

```
Bank 0  0x0000–0x1FFF   PC box 1–2 (plus secondary save copy 0x1209–0x1D82)
Bank 1  0x2000–0x3FFF   Primary save data (the checksummed region)
Bank 2  0x4000–0x5FFF   PC boxes 3–8
Bank 3  0x6000–0x7FFF   PC boxes 9–14 (+ secondary checksum at 0x7F0D)
```

The file contains **two identical copies** of the primary save data for
reliability (if one is corrupt, the game uses the other):

| Copy | Primary data | Checksum stored at |
|------|-------------|-------------------|
| Primary | 0x2009–0x2B82 | 0x2D0D (u16 LE) |
| Secondary | 0x1209–0x1D82 | 0x1F0D (u16 LE) |

Both checksums are the same algorithm: sum all bytes in the respective range as
unsigned 8-bit values, accumulated into a u16, stored little-endian.

Key offsets within the primary copy (Crystal English, relative to start of
`.sav` file):

```
0x2000  Options
0x2008  Player Trainer ID (2 bytes)
0x200B  Player name (11 bytes, Crystal charmap, 0x50-terminated)
0x2021  Rival name (11 bytes)
0x2042  Daylight savings time flag (bit 7 = DST active)
0x2053  Time played: hours(1) minutes(1) seconds(1) frames(1)
0x206A  Player palette / gender hint (0x00 = boy/red, 0x01 = girl/blue)
0x23DC  Money: 3 bytes BCD (wMoney)
0x23E2  Game Corner coins: 2 bytes big-endian (wCoins)
0x23E3  Johto Badges (1 byte bitmask)
0x23E4  Kanto Badges (1 byte bitmask)
0x23E5  TM/HM pocket (57 bytes: 50 TMs + 7 HMs, each = quantity byte)
0x2420  Item pocket (capacity 20, item-list format)
0x244A  Key item pocket (capacity 26)
0x2465  Ball pocket (capacity 12)
0x247F  PC items (capacity 50)
0x2700  Current PC Box index (low nibble = box 0-indexed)
0x2703  PC box names (14 × 9 bytes = 126 bytes)
0x2843  Player location: [map_bank][map_id][player_x][player_y] (4 bytes)
0x2865  Party Pokémon list (428 bytes: 6 Pokémon at 48 bytes each + species list)
0x2A27  Pokédex owned (32 bytes bitfield, Bulbasaur=bit0)
0x2A47  Pokédex seen  (32 bytes bitfield)
0x2D0D  Checksum 1 (u16 LE, sum of 0x2009–0x2B82)
0x1F0D  Checksum 2 (u16 LE, sum of 0x1209–0x1D82)
```

Item list format: `[count:u8] [item_id:u8, qty:u8]×capacity [0xFF terminator]`

Party Pokémon list format:
`[count:u8] [species_ids:u8×7, last=0xFF] [pokemon_data:48×6] [ot_names:11×6] [nicknames:11×6]`

PC box format: same but capacity=20, entry size=32 (level and current stats
not stored, recalculated on withdrawal — the "Box trick").

### Codec design

All Crystal SRAM knowledge is confined to `frontends/crystal/save/`. The engine
never imports from this package.

```
frontends/crystal/save/
  crystal_save_reader.hpp/.cpp    — read .sav → SaveSnapshot
  crystal_save_writer.hpp/.cpp    — SaveSnapshot + SRAM shadow → .sav
  crystal_save_codec.hpp/.cpp     — top-level import/export API
  crystal_sram_layout.hpp         — named SRAM offsets (no packed structs)
  crystal_sram_offsets.hpp        — version-specific offset tables (Crystal EN/JP)
```

Do not use packed structs (`#pragma pack` / `__attribute__((packed))`). Use
explicit byte readers at named offsets. This makes it impossible for a compiler
to silently misalign a field.

### SaveSnapshot — the codec's exchange type

`SaveSnapshot` is a plain C++ struct that carries only what the codec can
reliably decode from and encode to Crystal SRAM. It is **not** `GameState`. It
lives in `frontends/crystal/save/` and is converted to/from `GameState` by the
codec layer.

```cpp
// frontends/crystal/save/save_snapshot.hpp  (sketch — not yet implemented)

struct CrystalPartyMon {
    SpeciesId species;
    uint16_t trainer_id;
    // ... level, stats, moves, EVs, DVs, held item, nickname, OT name, etc.
};

struct CrystalSaveSnapshot {
    // --- Player identity ---
    uint16_t trainer_id;
    std::string player_name;       // decoded from Crystal charmap
    std::string rival_name;
    uint8_t  player_gender;        // 0 = boy, 1 = girl

    // --- Location ---
    uint8_t  map_bank;
    uint8_t  map_id;
    int32_t  player_x;
    int32_t  player_y;

    // --- Progression ---
    uint8_t  johto_badges;         // bitmask
    uint8_t  kanto_badges;         // bitmask
    uint32_t money;                // decoded from 3-byte BCD
    uint16_t coins;

    // --- Time ---
    uint16_t hours;
    uint8_t  minutes;
    uint8_t  seconds;
    bool     dst_active;

    // --- Party ---
    uint8_t party_count;
    CrystalPartyMon party[6];

    // --- PC boxes ---
    // (omit until party is done)

    // --- Pokédex ---
    std::bitset<256> owned;
    std::bitset<256> seen;

    // --- Inventory ---
    std::vector<std::pair<uint8_t,uint8_t>> item_pocket;     // [(id,qty)]
    std::vector<std::pair<uint8_t,uint8_t>> key_item_pocket;
    std::vector<std::pair<uint8_t,uint8_t>> ball_pocket;
    std::vector<std::pair<uint8_t,uint8_t>> pc_items;
    std::array<uint8_t, 50> tms;   // quantity per TM01-TM50
    std::array<uint8_t, 7>  hms;   // quantity per HM01-HM07

    // --- Original SRAM shadow ---
    // 32 KB opaque copy of the source .sav.
    // Preserved verbatim; the writer patches only known fields into this buffer
    // rather than constructing a new .sav from scratch.
    std::array<uint8_t, 0x8000> sram_shadow;
};
```

### Import path: `.sav` → native save

```
                          .sav file (32 KB)
                               │
                  crystal_save_reader.cpp
                               │
         1. Validate both save copies (checksum 1 & 2)
            - If one is bad: use the good one
            - If both are bad: return ImportError::BothCopiesInvalid
                               │
         2. Decode supported fields into CrystalSaveSnapshot
            - Use named offset constants from crystal_sram_layout.hpp
            - Decode BCD money, Crystal charmap names, bitfield Pokédex, etc.
            - Do not guess at unknown bytes — leave them in sram_shadow
                               │
         3. Convert CrystalSaveSnapshot → GameState
            - Map Crystal bank+id to Enginemon string map ID via
              compiled_game_data_.map_string_ids (or a static table)
            - Translate Crystal item IDs → Enginemon ItemId
            - Translate Pokémon data → party/box structures (not yet in GameState)
            - Set money_player, coins, flags, variables, etc.
                               │
         4. Attach sram_shadow to the result for round-trip fidelity
                               │
                      GameState + sram_shadow
                               │
                    Serialize to .emon_save
```

**Rejection policy:** If a decoded value cannot be represented in `GameState`
(e.g. a species ID not in the compiled game data), reject the import with an
explicit error rather than silently truncating or defaulting. The user must be
told what was incompatible.

### Export path: native save → `.sav`

```
                    GameState + sram_shadow
                               │
                  crystal_save_writer.cpp
                               │
         1. Start with sram_shadow as the write buffer
            — all unknown bytes are preserved verbatim
                               │
         2. Convert GameState → CrystalSaveSnapshot
            - Map string map ID → Crystal bank+id
            - Encode money as 3-byte BCD
            - Encode player name via Crystal charmap
            - Write item quantities, flags, etc.
                               │
         3. Patch only known fields into the write buffer at their offsets
            - Write primary copy  (0x2009–0x2B82)
            - Write secondary copy (0x1209–0x1D82) — byte-for-byte copy of primary
                               │
         4. Recompute both checksums
            - checksum1 = sum(buffer[0x2009..0x2B82]) → buffer[0x2D0D]
            - checksum2 = sum(buffer[0x1209..0x1D82]) → buffer[0x1F0D]
                               │
         5. Validate the result (re-read both checksums)
                               │
         6. Emit 32 KB .sav file
```

**Rejection policy:** If `GameState` contains state that Crystal cannot
represent (e.g. a map ID that has no Crystal bank+id, or an item count
exceeding 99), reject the export with an explicit error. Do not silently
clamp or discard.

### The sram_shadow as lossless bridge

The shadow is the core of the design. It means:

- You do not need to understand every byte of Crystal SRAM to produce a valid
  `.sav`. Unknown data stays untouched.
- Round-tripping a `.sav` through import → export without changing any
  gameplay state produces a byte-identical file (modulo the checksum fields,
  which must be recomputed).
- Fields that Enginemon does not model yet (Hall of Fame, RTC data, mobile
  features, etc.) remain correct for Crystal because the shadow preserves them.

The tradeoff: a shadow-based export requires that you have a source `.sav` to
start from. Starting a fresh Enginemon game on Crystal hardware requires a
**blank template** (see below).

### Fresh game from a blank template

For players who never had a Crystal cartridge, or for starting a new game from
Enginemon rather than importing:

```
frontends/crystal/save/crystal_blank_template.hpp
```

This file provides a deterministic 32 KB buffer representing a valid "just
started a new game" Crystal SRAM state — the exact bytes Crystal writes during
its first-save initialization. The template is extracted once from the ROM at
compile time (or hardcoded from a known-good reference) and checked into the
repository.

The writer uses this template as the starting shadow when no import shadow is
available. All known fields are patched in on top of it. Unknown bytes retain
their new-game values.

Generating the template requires running Crystal through its new-game save
sequence on a known ROM and capturing the SRAM state. This is a one-time
offline process; the result is a static byte array.

---

## Part 3 — Prerequisites

The `.sav` codec cannot be useful without first adding the missing domains to
`GameState`. Priority order:

### 1. Party Pokémon  *(highest priority)*

Crystal's party is the central gameplay artifact. Without it, import/export
loses every Pokémon.

`GameState` needs a `party` field containing up to 6 `PartyMon` structs:

```cpp
struct PartyMon {
    SpeciesId species;
    uint16_t  trainer_id;
    std::string trainer_name;   // OT name
    std::string nickname;
    uint8_t   level;
    uint32_t  experience;
    uint16_t  hp_current;
    uint8_t   status_condition;
    HeldItemId held_item;
    uint8_t   move_ids[4];
    uint8_t   move_pp[4];
    uint8_t   move_pp_up[4];
    // DVs (Defense, Attack, Speed, Special) — 4-bit each
    // EVs (HP, Attack, Defense, Speed, Special) — 16-bit each
    // Friendship
    bool      is_egg;
    uint8_t   catch_rate_or_held_item;  // Generation II encoding
    uint16_t  caught_data;              // level | location | time-of-day | original game
};
```

This must be serialized into `game_state.cpp` (new section after items, before NPC states,
requiring a v7 save version bump).

### 2. PC Boxes

14 boxes × 20 Pokémon × 32-byte structs (box-format Pokémon omit current stats,
which are recomputed on withdrawal). The `PartyMon` struct covers the box format
if you add an `is_in_box` flag or use a separate `BoxMon` struct with the shared
fields.

### 3. Trainer identity

```cpp
struct TrainerIdentity {
    uint16_t id;
    std::string name;   // decoded player name
    std::string rival_name;
    uint8_t gender;     // 0=boy, 1=girl
};
```

Add to `GameState`. Serialize in a new section.

### 4. Pokédex

```cpp
std::bitset<256> pokedex_owned;
std::bitset<256> pokedex_seen;
```

Add to `GameState`. Serialized as 32 bytes each.

### 5. Badges

```cpp
uint8_t johto_badges = 0;   // bitmask: Zephyr=bit7 ... Rising=bit0
uint8_t kanto_badges = 0;
```

These could go in `variables` or as dedicated fields. Dedicated fields are
preferable for clarity.

### 6. TMs/HMs

The TM/HM pocket is 57 bytes of quantity counts. These map to ItemId once the
full item-domain extractor is complete. For now, the item bag already covers
this if TMs/HMs are represented as regular items with ID-to-TM mapping.

---

## Part 4 — Implementation Plan

```
Phase 1 — GameState expansion
  - Add TrainerIdentity, party[6], pokedex, badges to GameState
  - Bump to save format v7
  - Write migration for v6→v7 (defaults for new fields)
  - Add tests

Phase 2 — Crystal charmap and BCD codecs
  frontends/crystal/save/crystal_charmap.hpp/.cpp
    - Crystal character encoding ↔ UTF-8
    - Already partially exists in frontend text decoder; extract and reuse
  frontends/crystal/save/crystal_bcd.hpp
    - Encode/decode 3-byte BCD money

Phase 3 — SRAM layout constants
  frontends/crystal/save/crystal_sram_layout.hpp
    - All named offsets, sizes, capacities for Crystal EN
    - Reference: pokecrystal/ram/sram.asm + Bulbapedia Gen II save structure

Phase 4 — Reader (import)
  frontends/crystal/save/crystal_save_reader.hpp/.cpp
    - Validate checksums
    - Decode into CrystalSaveSnapshot
    - Convert to GameState

Phase 5 — Writer (export)
  frontends/crystal/save/crystal_save_writer.hpp/.cpp
    - GameState → CrystalSaveSnapshot → patch sram_shadow
    - Recompute checksums
    - Emit

Phase 6 — Blank template
  frontends/crystal/save/crystal_blank_template.hpp
    - Static byte array of a known-good new-game Crystal SRAM

Phase 7 — CLI tool
  tools/crystal_save_import.cpp   — .sav + .emon → .emon_save
  tools/crystal_save_export.cpp   — .emon_save → .sav
```

---

## Part 5 — Design Constraints

**No packed structs.** Use `constexpr uint32_t SRAM_MONEY_OFFSET = 0x23DC` and
read `buffer[SRAM_MONEY_OFFSET]` explicitly. MSVC, GCC, and Clang all have
different alignment rules for packed structures; explicit offset reads have
no such ambiguity.

**Crystal offsets stay in the frontend.** The engine has no dependency on
`crystal_sram_layout.hpp`. The codec converts between domains at the boundary.

**Reject rather than mangle.** If a field can't round-trip (an item that
exists in Enginemon but has no Crystal ID, or a map that Crystal can't represent),
the import or export fails with a clear diagnostic. Silent data loss is not
acceptable.

**Checksums must be recomputed, never copied.** When writing a `.sav`, always
recalculate both checksums from the actual bytes in the output buffer. Do not
copy the checksum from the input `sram_shadow`.

**The native save is always authoritative.** The `.sav` codec is a compatibility
layer. If a native save and a `.sav` exist for the same slot, the native save
wins. The `.sav` is treated as import/export, not as the primary storage medium.

**Preserve the secondary copy.** Crystal's fault-tolerance depends on having
two matching copies. Always write both primary and secondary regions, always
with correct matching checksums.

---

## Part 6 — Reference Material

| Source | URL / Path |
|--------|-----------|
| pokecrystal SRAM layout | `ram/sram.asm` in pret/pokecrystal |
| Bulbapedia Gen II save structure | [Save data structure (Generation II)](https://bulbapedia.bulbagarden.net/wiki/Save_data_structure_in_Generation_II) |
| Pokémon data structure Gen II | [Pokémon data structure (Generation II)](https://bulbapedia.bulbagarden.net/wiki/Pok%C3%A9mon_data_structure_in_Generation_II) |
| Crystal charmap | `data/text/char_map.asm` in pret/pokecrystal |
| Crystal item IDs | `constants/item_constants.asm` in pret/pokecrystal |
| Crystal map bank+id | `constants/map_constants.asm` and `data/maps/maps.asm` |
| Engine native save | `engine/core/game_state.hpp`, `engine/core/game_state.cpp` |
| EMON package format | `engine/include/engine/package/package_format.hpp` |

Content was rephrased for compliance with licensing restrictions where applicable.
