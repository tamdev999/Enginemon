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
Crystal .sav (raw SRAM + optional emulator trailer)
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

### v7 — planned (Crystal codec prerequisite)

When party Pokémon, PC boxes, Pokédex, trainer identity, and badges are added to
`GameState`, the version bumps to 7. The migration for v6→v7 defaults all new
fields to empty/zero. The SRAM shadow chunk (see Part 2) will be added as an
optional `XSRM` chunk appended after the core v7 payload so that pre-codec saves
remain valid.

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
codec. The architecture is designed so you can add fields to `GameState`
incrementally — each addition automatically becomes part of the `.emon_save`
wire format and feeds the codec.

---

## Part 2 — Crystal `.sav` Codec

### What a Crystal save file is

The raw SRAM chip on a Crystal cartridge is exactly **32,768 bytes** (0x8000).
Emulator `.sav` files begin with this 32 KB image verbatim, but may append
additional bytes — typically 44–48 bytes of real-time clock state in a
format specific to the emulator (mGBA, BGB, etc.). The codec must accept and
preserve any such trailer; it must not assert the file is exactly 32,768 bytes.

The 32 KB image is divided into SRAM sections by the Crystal linker
(`ram/sram.asm`). The sections that matter for the codec:

```
SRAM Bank 0  0x0000–0x1FFF   Party mail, mailboxes, Mystery Gift, RTC status,
                              Lucky Number. Also houses the backup save copy
                              (sBackupGameData: 0x1209–0x1D82).

SRAM Bank 1  0x2000–0x3FFF   Primary save data.
               0x2000          sOptions
               0x2001          sCheckValue1  (must equal SAVE_CHECK_VALUE_1 = 99)
               0x2002          sGameData / sPlayerData  ← start of checksummed region
               ...
               0x2D0D          sChecksum (u16 LE)  ← sum of 0x2009–0x2B82
               0x2D0F          sCheckValue2  (must equal SAVE_CHECK_VALUE_2 = 127)
               0x2D10–0x3FFF   Active box, link battle stats, Hall of Fame,
                               Crystal Data, Battle Tower data.

SRAM Boxes   0x4000–0x5E2F  PC Boxes 1–7  (sBox1..sBox7)
             0x6000–0x7E2F  PC Boxes 8–14 (sBox8..sBox14)
```

### The two save copies and integrity flags

Crystal uses a three-level integrity system. The codec must handle all three:

**1. Check values** (`SAVE_CHECK_VALUE_1 = 99`, `SAVE_CHECK_VALUE_2 = 127`)

Two sentinel bytes — `sCheckValue1` immediately before `sGameData` and
`sCheckValue2` immediately after `sChecksum` — are loaded with magic constants
at save time. Crystal checks these on load *before* verifying checksums. If
either sentinel is wrong, Crystal treats the save as absent (not corrupted —
absent; it offers a new game).

The backup copy has equivalent sentinels: `sBackupCheckValue1` and
`sBackupCheckValue2` at corresponding positions.

The writer must set all four sentinel bytes. The reader must check them.

**2. Checksums**

After confirming the sentinels, Crystal computes and verifies 16-bit checksums.

| Copy | Checksummed region | Checksum stored at |
|------|-------------------|-------------------|
| Primary | 0x2009–0x2B82 | 0x2D0D (u16 LE) |
| Backup | 0x1209–0x1D82 | 0x1F0D (u16 LE) |

Algorithm: unsigned 8-bit sum of all bytes in the range, accumulated into a u16,
stored little-endian.

**3. Fallback logic**

- Both copies valid → use primary.
- One copy valid → use the valid copy; overwrite the other with it on next save.
- Both copies invalid → error screen; player must start a new game.

The writer must always produce both copies with matching content and correct
sentinels and checksums. The reader must be prepared to use either copy.

### Key SRAM offsets (Crystal English, primary copy)

Offsets are absolute within the `.sav` file (byte 0 = start of file).
Source-verified against `ram/sram.asm` and `ram/wram.asm` in pret/pokecrystal.

```
0x2000  sOptions             (8 bytes)
0x2008  sCheckValue1         = 99  (1 byte sentinel — NOT Trainer ID)
0x2009  sGameData begins     ← start of checksummed region

  Within sGameData / sPlayerData:
  0x2009  Player Trainer ID  (2 bytes, big-endian)
  0x200B  Player name        (11 bytes, Crystal charmap, 0x50-terminated)
  0x2016  Unused / mom name  (11 bytes)
  0x2021  Rival name         (11 bytes)
  0x202C  Unused / Red name  (11 bytes)
  0x2042  Daylight savings   (bit 7 = DST active; lower 7 bits: unknown)
  0x2053  Time played        hours(1) minutes(1) seconds(1) frames(1)
  0x206A  Player palette     0x00 = boy/red, 0x01 = girl/blue
          NOTE: this is NOT the authoritative gender field.
          Authoritative gender is at 0x3E3D (outside checksum region, Crystal only).

  0x23DC  Money              3 bytes BCD, big-endian (wMoney; max 999999)
  0x23E3  Game Corner coins  2 bytes big-endian (wCoins; max 9999)
  0x23E5  Johto Badges       1 byte bitmask (bit7=Zephyr … bit0=Rising)
  0x23E6  Kanto Badges       1 byte bitmask
  0x23E7  TM/HM pocket       57 bytes: TM01–TM50 quantities (50), HM01–HM07 (7)

  0x2420  Item pocket        item-list, capacity 20  (42 bytes total)
  0x244A  Key item pocket    item-list, capacity 26  (27 bytes total: 26 items + count)
  0x2465  Ball pocket        item-list, capacity 12  (26 bytes total)
  0x247F  PC items           item-list, capacity 50  (102 bytes total)

  0x2700  Current PC Box     (1 byte; low nibble = box index 0-based)
  0x2703  PC box names       14 × 9 bytes = 126 bytes

  0x2780  Saved map header   (28 bytes)
  0x2843  Player location    [map_group:1][map_number:1][y:1][x:1]
          NOTE: wWarpNumber (warp index within the current map, 1 byte) sits
          immediately before this group at 0x2842 and is a distinct field.
          "map_bank"/"map_id" are Gen I naming; Crystal uses group+number.
  0x2847  Saved map tiles    (30 bytes)
  0x2865  Party Pokémon list (428 bytes)
  0x2A27  Pokédex owned      (32 bytes bitfield; bit0 = Bulbasaur)
  0x2A47  Pokédex seen       (32 bytes bitfield)

  — sGameData ends at 0x2B82 —

0x2D0D  sChecksum            u16 LE, sum of bytes 0x2009–0x2B82
0x2D0F  sCheckValue2         = 127  (1 byte sentinel)

0x3E3D  Player gender        0x00 = boy, 0x01 = girl  (outside checksum region)

PC boxes (outside checksum region):
0x4000–0x5E2F  Boxes 1–7    (7 × sBox, each box = 1104 bytes)
0x6000–0x7E2F  Boxes 8–14   (7 × sBox, each box = 1104 bytes)

Backup copy (mirrors primary, different addresses):
0x1209–0x1D82  sBackupGameData
0x1F0D  sBackupChecksum      u16 LE, sum of bytes 0x1209–0x1D82
```

Item list format: `[count:u8] [(item_id:u8, qty:u8) × capacity] [0xFF terminator]`

Party Pokémon list format:
`[count:u8] [species_ids:u8 × 7, last entry=0xFF] [pokemon_data:48 × 6] [ot_names:11 × 6] [nicknames:11 × 6]`

PC box format: capacity=20, entry size=32 (current stats omitted, recalculated
on withdrawal — the Gen II "Box trick"). Each box is 1104 bytes total.

### Codec types

The codec uses three separate types to keep semantic, layout, and raw-bytes
concerns properly separated.

**`Sram`** — the raw 32 KB buffer plus trailer:
```cpp
// frontends/crystal/save/crystal_sram.hpp
struct Sram {
    std::array<uint8_t, 0x8000> data;   // The 32 KB SRAM image
    std::vector<uint8_t>        trailer; // Emulator RTC bytes or other appended data
};
```

**`CrystalSaveSnapshot`** — the semantic layer: decoded, human-readable fields.
No raw bytes. No SRAM layout knowledge outside the codec.
```cpp
// frontends/crystal/save/save_snapshot.hpp  (sketch — not yet implemented)

struct CrystalPartyMon {
    SpeciesId species;
    uint16_t trainer_id;
    std::string trainer_name;  // OT name, decoded from Crystal charmap
    std::string nickname;
    uint8_t  level;
    uint32_t experience;
    uint16_t hp_current;
    uint8_t  status_condition;
    uint8_t  held_item;
    uint8_t  move_ids[4];
    uint8_t  move_pp[4];
    uint8_t  move_pp_up[4];
    uint8_t  dvs[4];            // Attack/Defense/Speed/Special, 4 bits each
    uint16_t evs[5];            // HP/Atk/Def/Spd/Spc
    uint8_t  friendship;
    bool     is_egg;
    uint16_t caught_data;       // level | location | time-of-day | game
};

struct CrystalSaveSnapshot {
    // Player identity
    uint16_t    trainer_id;
    std::string player_name;
    std::string rival_name;
    uint8_t     player_gender;   // 0 = boy, 1 = girl  (from 0x3E3D, not palette)

    // Location
    uint8_t map_bank;
    uint8_t map_id;
    int32_t player_x;
    int32_t player_y;

    // Progression
    uint8_t  johto_badges;
    uint8_t  kanto_badges;
    uint32_t money;              // decoded from 3-byte BCD
    uint16_t coins;              // decoded from 2-byte big-endian

    // Time
    uint16_t hours;
    uint8_t  minutes;
    uint8_t  seconds;
    bool     dst_active;

    // Party
    uint8_t       party_count;
    CrystalPartyMon party[6];

    // Pokédex
    std::bitset<256> pokedex_owned;
    std::bitset<256> pokedex_seen;

    // Inventory
    std::vector<std::pair<uint8_t,uint8_t>> item_pocket;      // [(id, qty)]
    std::vector<std::pair<uint8_t,uint8_t>> key_item_pocket;
    std::vector<std::pair<uint8_t,uint8_t>> ball_pocket;
    std::vector<std::pair<uint8_t,uint8_t>> pc_items;
    std::array<uint8_t, 50> tms;
    std::array<uint8_t,  7> hms;

    // PC boxes — omit until party is stable
};
```

**`CrystalImport`** — the result of reading a `.sav`. Holds the snapshot and
the raw materials needed to round-trip back to `.sav`:
```cpp
// frontends/crystal/save/crystal_save_reader.hpp
struct CrystalImport {
    CrystalSaveSnapshot snapshot;
    Sram                shadow;   // The full 32 KB + trailer, preserved verbatim
};
```

The shadow is **not** inside the snapshot. The snapshot is semantic; the shadow
is raw. Keeping them at arm's length prevents the snapshot from accidentally
acquiring layout knowledge.

### Persisting the shadow in `.emon_save`

The shadow must survive the native save round-trip so that export can patch it.
It is stored as an optional tagged chunk appended after the core `GameState`
payload in the `.emon_save` file. The chunk is only written when a Crystal
import is active:

```
[core GameState payload — versioned as always]
[optional XSRM chunk:]
  [u32]  chunk_tag = 0x5853524D ("XSRM")
  [u32]  sram_size = 0x8000
  [u8 × 0x8000]  sram_data
  [u32]  trailer_size
  [u8 × trailer_size]  trailer_data
```

`try_deserialize()` reads the core payload first, then checks if an `XSRM`
chunk follows. If present, it populates a `std::optional<Sram> sram_shadow`
field on the result. If absent, `sram_shadow` is empty and export falls back
to the blank template.

The XSRM chunk does not affect the v6 checksum scheme — `try_deserialize()`
already requires exact payload consumption, so `XSRM` must be handled as an
extension point, not parsed as trailing garbage. This is the `XSRM` design
contract: present = import shadow available; absent = use blank template.

The v7 bump (Party/Pokédex/TrainerID additions) is separate from XSRM; both
can land independently.

### Files in `frontends/crystal/save/`

All Crystal SRAM knowledge lives here. The engine never imports from this
package.

```
frontends/crystal/save/
  crystal_sram.hpp             — Sram struct (raw bytes + trailer)
  save_snapshot.hpp            — CrystalSaveSnapshot, CrystalPartyMon
  crystal_save_reader.hpp/.cpp — .sav → CrystalImport
  crystal_save_writer.hpp/.cpp — CrystalImport + GameState → .sav
  crystal_save_codec.hpp/.cpp  — top-level import/export API
  crystal_sram_layout.hpp      — named SRAM offset constants (no packed structs)
  crystal_bcd.hpp              — encode/decode 3-byte BCD money
  crystal_charmap.hpp/.cpp     — Crystal charmap ↔ UTF-8
  crystal_blank_template.hpp   — static 32 KB new-game SRAM array
```

Do not use packed structs. Use `constexpr uint32_t SRAM_MONEY_OFFSET = 0x23DC`
and read `sram.data[SRAM_MONEY_OFFSET]` explicitly.

### Import path: `.sav` → native save

```
                     .sav file (32 KB + optional trailer)
                                │
                   crystal_save_reader.cpp
                                │
         1. Read file → Sram{data[0x8000], trailer}
                                │
         2. Check sentinels on both copies:
            - Primary:  data[0x2008] == 99  AND  data[0x2D0F] == 127
            - Backup:   backup equivalents
            If a copy fails either sentinel: treat it as absent (not corrupt)
                                │
         3. Verify checksums on surviving copies:
            - Primary checksum:  sum(data[0x2009..0x2B82]) == data[0x2D0D:2]
            - Backup checksum:   sum(data[0x1209..0x1D82]) == data[0x1F0D:2]
            Choose which copy to decode (prefer primary if both valid)
                                │
         4. Decode selected copy into CrystalSaveSnapshot
            - Read player_gender from 0x3E3D (not 0x206A)
            - Decode BCD money, Crystal charmap names, bitfield Pokédex, etc.
            - Do not guess at unknown bytes — leave them in shadow
                                │
         5. Convert CrystalSaveSnapshot → GameState
            - Map Crystal map_bank+map_id → Enginemon string map ID
            - Translate Crystal item IDs → Enginemon ItemId
            - Set money_player, coins, flags, variables, etc.
                                │
         6. Return CrystalImport{snapshot, shadow}
                                │
         7. Serialize GameState to .emon_save, appending XSRM chunk
```

**Rejection policy:** If a decoded value cannot be represented in `GameState`
(e.g. a species not in the compiled game data), reject the import with an
explicit diagnostic. Do not silently truncate or default.

### Export path: native save → `.sav`

```
                      .emon_save (GameState + optional XSRM)
                                │
                   crystal_save_writer.cpp
                                │
         1. Load GameState from .emon_save
         2. Load Sram shadow from XSRM chunk, or use blank template
                                │
         3. Convert GameState → CrystalSaveSnapshot
            - Map string map ID → Crystal map_bank+map_id
            - Encode money as 3-byte BCD
            - Encode names via Crystal charmap
                                │
         4. Patch known fields into shadow.data at their offsets
            (unknown bytes remain from the shadow or blank template)
                                │
         5. Copy primary region → backup region:
            shadow.data[0x1209..0x1D82] = shadow.data[0x2009..0x2B82]
                                │
         6. Write sentinel bytes (all four):
            shadow.data[0x2008] = 99    (sCheckValue1)
            shadow.data[0x2D0F] = 127   (sCheckValue2)
            shadow.data[backup_cv1_offset] = 99
            shadow.data[backup_cv2_offset] = 127
                                │
         7. Recompute both checksums:
            primary_sum = sum(shadow.data[0x2009..0x2B82])
            shadow.data[0x2D0D:2] = primary_sum  (LE)
            backup_sum  = sum(shadow.data[0x1209..0x1D82])
            shadow.data[0x1F0D:2] = backup_sum   (LE)
                                │
         8. Self-validate: re-read sentinels + checksums; fail if wrong
                                │
         9. Emit: shadow.data (32 KB) + shadow.trailer
```

**Rejection policy:** State that Crystal cannot represent (map with no Crystal
bank+id, item count > 99) fails the export with a clear diagnostic. No silent
clamping or discarding.

### The shadow as lossless bridge

The shadow is the core of the design:

- You do not need to understand every SRAM byte to produce a valid `.sav`.
  Unknown data is never touched.
- Round-tripping a `.sav` through import → export without any gameplay changes
  produces a byte-identical file (modulo checksum fields, which are always
  recomputed from scratch).
- Fields Enginemon does not model yet (Hall of Fame, RTC hardware bytes, mobile
  features, Trainer Rankings) remain correct because the shadow preserves them.
- The emulator trailer (RTC state) is carried in `Sram::trailer` and emitted
  unchanged at the end of the export.

### Fresh game from a blank template

When no import shadow is available (new Enginemon game, never imported a `.sav`):

`frontends/crystal/save/crystal_blank_template.hpp` provides a deterministic
32 KB buffer representing a valid Crystal SRAM state immediately after the
game's first-save initialization. Both save copies are present with correct
sentinels and checksums. All fields have new-game values.

Generating the template: boot Crystal through its new-game save sequence on the
known ROM, dump SRAM, commit as a static byte array. This is a one-time offline
step. The result is the starting point for all exports that have no import shadow.

---

## Part 3 — Prerequisites

The `.sav` codec is not useful without first adding the missing domains to
`GameState`. Priority order:

### 1. Party Pokémon  *(highest priority)*

Without it, every import and export loses all Pokémon.

Add to `GameState`:
```cpp
struct PartyMon {
    SpeciesId   species;
    uint16_t    trainer_id;
    std::string trainer_name;  // OT name
    std::string nickname;
    uint8_t     level;
    uint32_t    experience;
    uint16_t    hp_current;
    uint8_t     status_condition;
    uint16_t    held_item_id;
    uint8_t     move_ids[4];
    uint8_t     move_pp[4];
    uint8_t     move_pp_up[4];
    uint8_t     dvs[4];        // Attack, Defense, Speed, Special — 4 bits each
    uint16_t    evs[5];        // HP, Atk, Def, Spd, SpC — 16 bits each
    uint8_t     friendship;
    bool        is_egg;
    uint16_t    caught_data;   // packed: level | location | time-of-day | game
};

// In GameState:
uint8_t  party_count = 0;
std::array<PartyMon, 6> party;
```

Serialize as a new section in `game_state.cpp`, requiring a v7 bump.

### 2. PC Boxes

14 boxes × 20 Pokémon × 32 bytes (box-format; current stats recalculated on
withdrawal). `PartyMon` covers box Pokémon if you use a union or flag.

### 3. Trainer identity

```cpp
struct TrainerIdentity {
    uint16_t    id;
    std::string player_name;
    std::string rival_name;
    uint8_t     gender;   // 0=boy, 1=girl
};
// In GameState:
TrainerIdentity trainer;
```

### 4. Pokédex

```cpp
// In GameState:
std::array<uint8_t, 32> pokedex_owned = {};   // bitfield, bit0=Bulbasaur
std::array<uint8_t, 32> pokedex_seen  = {};
```

### 5. Badges

```cpp
// In GameState:
uint8_t johto_badges = 0;   // bitmask: bit7=Zephyr, bit0=Rising
uint8_t kanto_badges = 0;
```

Dedicated fields rather than `variables` entries, for clarity.

### 6. TMs/HMs

Once the full item-domain extractor is complete, TMs/HMs map naturally to
`ItemId` in the existing item bag. No separate field needed.

---

## Part 4 — Implementation Plan

```
Phase 1 — GameState expansion (v7)
  - Add TrainerIdentity, party[6], pokedex_owned/seen, johto/kanto_badges
  - Bump to save format v7; migrate v6→v7 with zero defaults
  - Add serialization + tests

Phase 2 — XSRM chunk in .emon_save
  - Add optional XSRM append to GameState::serialize()
  - Add XSRM parsing to try_deserialize()
  - No version bump needed (optional chunk, unknown chunks = skip)

Phase 3 — Crystal charmap and BCD
  frontends/crystal/save/crystal_charmap.hpp/.cpp
    - Crystal charmap ↔ UTF-8 (reuse existing frontend text decoder)
  frontends/crystal/save/crystal_bcd.hpp
    - encode_bcd3(uint32_t money) → 3 bytes
    - decode_bcd3(const uint8_t[3]) → uint32_t

Phase 4 — SRAM layout constants
  frontends/crystal/save/crystal_sram_layout.hpp
    - All named offset constants, sizes, capacities for Crystal EN
    - References: ram/sram.asm, constants/misc_constants.asm (pret/pokecrystal)

Phase 5 — Reader (import)
  frontends/crystal/save/crystal_save_reader.hpp/.cpp
    - Validate sentinels + checksums
    - Decode into CrystalSaveSnapshot
    - Return CrystalImport{snapshot, shadow}

Phase 6 — Writer (export)
  frontends/crystal/save/crystal_save_writer.hpp/.cpp
    - GameState → CrystalSaveSnapshot → patch shadow
    - Set sentinels, copy primary→backup, recompute checksums
    - Self-validate, emit

Phase 7 — Blank template
  frontends/crystal/save/crystal_blank_template.hpp
    - Deterministic 32 KB new-game SRAM (offline capture, checked in)

Phase 8 — CLI tools
  tools/crystal_save_import.cpp   — .sav → .emon_save
  tools/crystal_save_export.cpp   — .emon_save → .sav
```

---

## Part 5 — Design Constraints

**No packed structs.** Use `constexpr uint32_t SRAM_MONEY_OFFSET = 0x23DC` and
`sram.data[SRAM_MONEY_OFFSET]` explicitly. Packed struct alignment is
compiler-defined; explicit offset reads are not.

**Crystal offsets stay in the frontend.** The engine has no dependency on
`crystal_sram_layout.hpp`. The codec converts between domains at the boundary.

**Check sentinels before checksums.** Crystal does it in that order. A missing
sentinel means the save slot is absent, not corrupt — the game writes a blank
save rather than erroring. A failed checksum with a good sentinel means
corruption. The reader must distinguish these.

**Always write all four sentinel bytes.** Primary and backup each have two
(`sCheckValue1`, `sCheckValue2`). Missing any one of them causes Crystal to
ignore that copy.

**Checksums must be recomputed, never copied.** Even if the shadow has a
checksum from the import, always recalculate from the patched bytes. The
shadow's checksum is the old value; the patched file's checksum reflects the
new content.

**Reject rather than mangle.** If a field can't round-trip (an item with no
Crystal ID, a map Crystal can't represent), the import or export fails with
a clear diagnostic. No silent clamping or discarding.

**The native save is always authoritative.** The `.sav` codec is a compatibility
layer. If a native save and a `.sav` exist for the same slot, the native save
wins.

**Accept and preserve emulator trailers.** The file may be longer than 32 KB;
the extra bytes must pass through unchanged in `Sram::trailer`.

**The shadow is beside the snapshot, not inside it.** `CrystalSaveSnapshot`
is semantic — named, decoded fields. `Sram` is raw bytes. `CrystalImport`
holds both. This prevents the snapshot from accidentally carrying layout
knowledge.

---

## Part 6 — Reference Material

| Source | URL / Path |
|--------|-----------|
| pokecrystal SRAM layout | `ram/sram.asm` in pret/pokecrystal |
| pokecrystal save constants | `constants/misc_constants.asm` in pret/pokecrystal |
| Bulbapedia Gen II save structure | [Save data structure (Generation II)](https://bulbapedia.bulbagarden.net/wiki/Save_data_structure_in_Generation_II) |
| Pokémon data structure Gen II | [Pokémon data structure (Generation II)](https://bulbapedia.bulbagarden.net/wiki/Pok%C3%A9mon_data_structure_in_Generation_II) |
| Crystal charmap | `data/text/char_map.asm` in pret/pokecrystal |
| Crystal item IDs | `constants/item_constants.asm` in pret/pokecrystal |
| Crystal map bank+id | `constants/map_constants.asm` and `data/maps/maps.asm` |
| Engine native save | `engine/core/game_state.hpp`, `engine/core/game_state.cpp` |
| EMON package format | `engine/include/engine/package/package_format.hpp` |

Content was paraphrased for compliance with licensing restrictions where applicable.
