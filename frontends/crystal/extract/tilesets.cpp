// crystal/extract/tilesets.cpp
// Tileset extraction from Crystal ROM
// 
// Extracts tile graphics, metatiles, collision, and palettes.
// Output is semantic - ready for rendering or native package.

#include "crystal/extract/tileset_extractor.hpp"
#include <format>

namespace crystal {

//=============================================================================
// CONSTRUCTION
//=============================================================================

TilesetExtractor::TilesetExtractor(const RomData& rom, const ExtractionProfile& profile)
    : rom_(rom), profile_(profile) {}

//=============================================================================
// 2BPP TILE DECODING
//=============================================================================

Tile TilesetExtractor::decode_tile(const uint8_t* data) const {
    Tile tile;
    
    // Game Boy 2bpp format:
    // Each row is 2 bytes (8 pixels)
    // Byte 0 has low bits, Byte 1 has high bits
    // Pixel order is left-to-right, MSB first
    
    for (int row = 0; row < 8; ++row) {
        uint8_t low = data[row * 2];
        uint8_t high = data[row * 2 + 1];
        
        for (int col = 0; col < 8; ++col) {
            int bit = 7 - col;  // MSB first
            uint8_t pixel = ((low >> bit) & 1) | (((high >> bit) & 1) << 1);
            tile.pixels[row * 8 + col] = pixel;
        }
    }
    
    return tile;
}

//=============================================================================
// LZ DECOMPRESSION (LZ3 - Pokemon GSC variant)
//=============================================================================
// 
// From pokecrystal home/decompress.asm:
// Pokemon GSC uses an lz variant (lz3) for compression.
// Compressed data is terminated with $ff (LZ_END).
//
// Control byte format:
//   bits 5-7: command (LZ_CMD)
//   bits 0-4: length n (LZ_LEN)
//
// Commands:
//   0 (LZ_LITERAL):   Read literal data for n+1 bytes
//   1 (LZ_ITERATE):   Write the same byte for n+1 bytes
//   2 (LZ_ALTERNATE): Alternate two bytes for n+1 bytes
//   3 (LZ_ZERO):      Write 0 for n+1 bytes
//   4 (LZ_REPEAT):    Repeat n+1 bytes from offset (rewrite)
//   5 (LZ_FLIP):      Repeat n+1 bitflipped bytes from offset
//   6 (LZ_REVERSE):   Repeat n+1 bytes in reverse from offset
//   7 (LZ_LONG):      Extended length command (10-bit length)
//
// For commands 4-6, an offset follows:
//   - If bit 7 is set: negative 7-bit offset from current position
//   - If bit 7 is clear: positive 15-bit offset from start (2 bytes)

bool TilesetExtractor::decompress_lz(uint32_t addr, std::vector<uint8_t>& out) const {
    return decompress_lz_crystal(rom_, addr, out);
}

// Free function implementation — shared with MapExtractor and any other consumer.
bool decompress_lz_crystal(const RomData& rom, uint32_t addr, std::vector<uint8_t>& out) {
    constexpr uint8_t LZ_END = 0xFF;
    constexpr uint8_t LZ_CMD = 0b11100000;
    constexpr uint8_t LZ_LEN = 0b00011111;
    constexpr uint8_t LZ_LONG_HI = 0b00000011;
    
    // Command types (shifted >> 5)
    constexpr uint8_t LZ_LITERAL   = 0;
    constexpr uint8_t LZ_ITERATE   = 1;
    constexpr uint8_t LZ_ALTERNATE = 2;
    constexpr uint8_t LZ_ZERO      = 3;
    constexpr uint8_t LZ_REPEAT    = 4;
    constexpr uint8_t LZ_FLIP      = 5;
    constexpr uint8_t LZ_REVERSE   = 6;
    constexpr uint8_t LZ_LONG      = 7;
    
    if (addr >= rom.size()) {
        return false;
    }
    
    out.clear();
    out.reserve(4096);  // Reasonable initial capacity
    
    uint32_t ptr = addr;
    
    // Helper to flip bits in a byte (MSB <-> LSB)
    auto flip_byte = [](uint8_t b) -> uint8_t {
        uint8_t result = 0;
        for (int i = 0; i < 8; ++i) {
            result = (result << 1) | (b & 1);
            b >>= 1;
        }
        return result;
    };
    
    // Fail-closed: partial output must never be treated as success.
    //
    // Only the clean LZ_END path returns true.  Every early exit — ROM bounds
    // overrun, truncated extended command, invalid back-reference, or safety
    // limit exceeded — returns false so the caller sees a hard failure rather
    // than a silently truncated tile set.
    while (ptr < rom.size()) {
        uint8_t control = rom.read_byte(ptr);
        
        if (control == LZ_END) {
            // Clean termination.  Only path that counts as success.
            return !out.empty();
        }
        
        uint8_t cmd = (control & LZ_CMD) >> 5;
        size_t count;
        
        if (cmd == LZ_LONG) {
            // Extended length: 10-bit count
            // Format: 111xxxyy yyyyyyyy
            //   xxx = new command
            //   yy yyyyyyyy = 10-bit length
            cmd = ((control >> 2) & 0x07);  // bits 2-4 become new cmd
            uint8_t hi = control & LZ_LONG_HI;
            ptr++;
            if (ptr >= rom.size()) {
                return false;  // truncated extended command (no second byte)
            }
            uint8_t lo = rom.read_byte(ptr);
            count = ((size_t)hi << 8) | lo;
            count++;  // length is n+1
            ptr++;
        } else {
            count = (control & LZ_LEN) + 1;
            ptr++;
        }
        
        // Commands 0-3: simple data operations
        if (cmd == LZ_LITERAL) {
            // Read literal bytes — every byte in [ptr, ptr+count) must be in ROM.
            if (ptr + count > rom.size()) {
                return false;  // truncated literal run
            }
            for (size_t i = 0; i < count; ++i) {
                out.push_back(rom.read_byte(ptr++));
            }
        }
        else if (cmd == LZ_ITERATE) {
            // Repeat single byte
            if (ptr >= rom.size()) {
                return false;  // no byte to repeat
            }
            uint8_t byte = rom.read_byte(ptr++);
            for (size_t i = 0; i < count; ++i) {
                out.push_back(byte);
            }
        }
        else if (cmd == LZ_ALTERNATE) {
            // Alternate two bytes
            if (ptr + 1 >= rom.size()) {
                return false;  // truncated alternate pair
            }
            uint8_t byte1 = rom.read_byte(ptr);
            uint8_t byte2 = rom.read_byte(ptr + 1);
            for (size_t i = 0; i < count; ++i) {
                out.push_back((i & 1) ? byte2 : byte1);
            }
            ptr += 2;
        }
        else if (cmd == LZ_ZERO) {
            // Write zeros — no ROM read needed
            for (size_t i = 0; i < count; ++i) {
                out.push_back(0);
            }
        }
        // Commands 4-6: rewrite from decompressed output
        else {
            if (ptr >= rom.size()) {
                return false;  // no offset byte
            }
            uint8_t offset_byte = rom.read_byte(ptr++);
            
            size_t src_pos;
            if (offset_byte & 0x80) {
                // Negative offset (7-bit) from current position
                size_t neg_offset = offset_byte & 0x7F;
                if (neg_offset > out.size()) {
                    return false;  // invalid negative back-reference
                }
                src_pos = out.size() - neg_offset - 1;
            } else {
                // Positive offset (15-bit) from start — needs a second byte
                if (ptr >= rom.size()) {
                    return false;  // truncated positive offset
                }
                uint8_t lo = rom.read_byte(ptr++);
                size_t pos_offset = ((size_t)offset_byte << 8) | lo;
                src_pos = pos_offset;
                if (src_pos >= out.size()) {
                    return false;  // forward reference beyond decompressed output
                }
            }
            
            if (cmd == LZ_REPEAT) {
                // Copy bytes from earlier in output.
                // src_pos can advance into newly written bytes intentionally
                // (run-length style), so no pre-flight end check here.
                for (size_t i = 0; i < count; ++i) {
                    out.push_back(out[src_pos + i]);
                }
            }
            else if (cmd == LZ_FLIP) {
                // Copy bitflipped bytes — source range must be within the
                // previously-written output (not newly written this command).
                for (size_t i = 0; i < count; ++i) {
                    out.push_back(flip_byte(out[src_pos + i]));
                }
            }
            else if (cmd == LZ_REVERSE) {
                // Copy bytes in reverse order — src_pos - (count-1) must be ≥ 0.
                if (count > 0 && src_pos + 1 < count) {
                    return false;  // reverse would underflow decompressed buffer
                }
                for (size_t i = 0; i < count; ++i) {
                    out.push_back(out[src_pos - i]);
                }
            }
        }
        
        // Safety limit — malformed data causing runaway expansion is an error,
        // not a partial success.
        if (out.size() > 0x10000) {
            return false;
        }
    }
    
    // Fell off end of ROM without seeing LZ_END — missing terminator.
    return false;
}

//=============================================================================
// ID GENERATION
//=============================================================================

std::string TilesetExtractor::make_tileset_id(uint8_t index) const {
    // Known tilesets - 1-indexed! (from constants/tileset_constants.asm)
    // const_def 1 means first constant is 1, not 0
    // This MUST match MapExtractor::make_tileset_id exactly
    static const char* names[] = {
        nullptr,                // 0: unused (indices are 1-based)
        "johto_outdoor",        // 1: TILESET_JOHTO
        "johto_modern",         // 2: TILESET_JOHTO_MODERN
        "kanto_outdoor",        // 3: TILESET_KANTO
        "battle_tower_outside", // 4: TILESET_BATTLE_TOWER_OUTSIDE
        "house",                // 5: TILESET_HOUSE
        "players_house",        // 6: TILESET_PLAYERS_HOUSE
        "pokecenter",           // 7: TILESET_POKECENTER
        "gate",                 // 8: TILESET_GATE
        "port",                 // 9: TILESET_PORT
        "lab",                  // 10 (0x0A): TILESET_LAB
        "facility",             // 11 (0x0B): TILESET_FACILITY
        "mart",                 // 12 (0x0C): TILESET_MART
        "mansion",              // 13 (0x0D): TILESET_MANSION
        "game_corner",          // 14 (0x0E): TILESET_GAME_CORNER
        "elite_four_room",      // 15 (0x0F): TILESET_ELITE_FOUR_ROOM
        "traditional_house",    // 16 (0x10): TILESET_TRADITIONAL_HOUSE
        "train_station",        // 17 (0x11): TILESET_TRAIN_STATION
        "champions_room",       // 18 (0x12): TILESET_CHAMPIONS_ROOM
        "lighthouse",           // 19 (0x13): TILESET_LIGHTHOUSE
        "players_room",         // 20 (0x14): TILESET_PLAYERS_ROOM
        "pokecom_center",       // 21 (0x15): TILESET_POKECOM_CENTER
        "battle_tower_inside",  // 22 (0x16): TILESET_BATTLE_TOWER_INSIDE
        "tower",                // 23 (0x17): TILESET_TOWER
        "cave",                 // 24 (0x18): TILESET_CAVE
        "park",                 // 25 (0x19): TILESET_PARK
        "ruins_of_alph",        // 26 (0x1A): TILESET_RUINS_OF_ALPH
        "radio_tower",          // 27 (0x1B): TILESET_RADIO_TOWER
        "underground",          // 28 (0x1C): TILESET_UNDERGROUND
        "ice_path",             // 29 (0x1D): TILESET_ICE_PATH
        "dark_cave",            // 30 (0x1E): TILESET_DARK_CAVE
        "forest",               // 31 (0x1F): TILESET_FOREST
        "beta_word_room",       // 32 (0x20): TILESET_BETA_WORD_ROOM
        "ho_oh_word_room",      // 33 (0x21): TILESET_HO_OH_WORD_ROOM
        "kabuto_word_room",     // 34 (0x22): TILESET_KABUTO_WORD_ROOM
        "omanyte_word_room",    // 35 (0x23): TILESET_OMANYTE_WORD_ROOM
        "aerodactyl_word_room", // 36 (0x24): TILESET_AERODACTYL_WORD_ROOM
    };
    
    constexpr size_t num_names = sizeof(names) / sizeof(names[0]);
    if (index > 0 && index < num_names && names[index]) {
        return names[index];
    }
    return std::format("tileset_{:02d}", index);
}

//=============================================================================
// EXTRACTION
//=============================================================================

TilesetExtractionResult TilesetExtractor::extract_tileset(uint8_t tileset_index) const {
    TilesetExtractionResult result;
    
    // Validate index - Crystal tilesets are 1-indexed (1..num_tilesets)
    if (tileset_index == 0 || tileset_index > profile_.counts.num_tilesets) {
        result.error = std::format("Invalid tileset index: {} (valid range: 1-{})", 
                                   tileset_index, profile_.counts.num_tilesets);
        return result;
    }

    // The PalMap bank is derived from profile_.offsets.tilesets (see below).
    // Require it to be set before going any further: an unset address would
    // cause the palmap bank to be 0 (ROM0), producing garbage palette data
    // with no error signal.  Catch it here with a clear diagnostic rather than
    // letting the extractor silently misread from the wrong bank.
    if (profile_.offsets.tilesets == 0) {
        result.error = std::format(
            "Cannot extract tileset {}: profile.offsets.tilesets is not configured "
            "(required to derive the PalMap bank)",
            tileset_index);
        stats_.bounds_check_failures++;
        return result;
    }
    
    const auto& o = profile_.offsets;
    const auto& fmt = profile_.format.tileset;
    
    // Calculate tileset entry address
    uint32_t entry_addr = o.tilesets + (tileset_index * fmt.tileset_size);
    
    if (entry_addr + fmt.tileset_size > rom_.size()) {
        result.error = "Tileset entry out of bounds";
        stats_.bounds_check_failures++;
        return result;
    }
    
    // Read tileset entry (15 bytes for Crystal)
    auto entry = rom_.read_bytes(entry_addr, fmt.tileset_size);
    
    // Parse entry:
    // 0: gfx_bank
    // 1-2: gfx_ptr
    // 3: metatile_bank  
    // 4-5: metatile_ptr
    // 6: collision_bank
    // 7-8: collision_ptr
    // 9-11: animation data
    // 12-14: palette data
    
    uint8_t gfx_bank = entry[fmt.gfx_bank_offset];
    uint16_t gfx_ptr = entry[fmt.gfx_ptr_offset] | (entry[fmt.gfx_ptr_offset + 1] << 8);
    
    uint8_t meta_bank = entry[fmt.metatile_bank_offset];
    uint16_t meta_ptr = entry[fmt.metatile_ptr_offset] | (entry[fmt.metatile_ptr_offset + 1] << 8);
    
    uint8_t coll_bank = entry[fmt.coll_bank_offset];
    uint16_t coll_ptr = entry[fmt.coll_ptr_offset] | (entry[fmt.coll_ptr_offset + 1] << 8);
    
    ExtractedTileset& tileset = result.tileset;
    tileset.tileset_id = make_tileset_id(tileset_index);
    
    // Extract tile graphics (LZ compressed)
    uint32_t gfx_addr = rom_.bank_to_flat(gfx_bank, gfx_ptr);
    std::vector<uint8_t> tile_data;
    
    if (!decompress_lz(gfx_addr, tile_data)) {
        // LZ decompression failed entirely — zero bytes produced.
        // A tileset with no tile graphics is not usable; fail closed.
        result.error = std::format(
            "Failed to decompress tile graphics for tileset '{}' "
            "(gfx_bank=0x{:02x} gfx_ptr=0x{:04x} flat=0x{:x})",
            tileset.tileset_id, gfx_bank, gfx_ptr, gfx_addr);
        return result;
    }
    
    // Decode tiles (16 bytes per 8x8 tile in 2bpp)
    if (!tile_data.empty()) {
        size_t tile_count = tile_data.size() / 16;
        tileset.tiles.reserve(tile_count);
        
        for (size_t i = 0; i < tile_count; ++i) {
            tileset.tiles.push_back(decode_tile(&tile_data[i * 16]));
            stats_.tiles_extracted++;
        }
    }
    
    // Extract metatiles
    uint32_t meta_addr = rom_.bank_to_flat(meta_bank, meta_ptr);
    uint32_t coll_addr = rom_.bank_to_flat(coll_bank, coll_ptr);
    
    // Each metatile is 16 bytes = 4×4 grid of tile indices (32×32 pixels)
    // See pokecrystal/home/map.asm LoadMetatiles:
    //   add a ; multiply block index by 16
    //   ld l, a
    //   ld h, 0
    //   add hl, hl ; ×2
    //   add hl, hl ; ×4
    //   add hl, hl ; ×8 (now hl = block_index * 16)
    // And Gen2Recomped/src/import/RomExtractorGen2.lua:
    //   local blocksRaw = self.rom:bytes(meta.bank, meta.address, blockCount * 16)
    //   for offset = 1, #blocksRaw, 16 do
    
    const auto& tileset_fmt = profile_.format.tileset;
    const size_t metatile_size = tileset_fmt.metatile_size;    // 16 bytes
    
    // Derive actual metatile count from gap between Meta and Coll data
    // Gen2Recomped does this: if coll.bank == meta.bank and coll > meta
    // then blockCount = (coll.address - meta.address) / 16
    // This handles indoor tilesets that have fewer than 128 metatiles
    size_t metatile_count = tileset_fmt.metatile_count;  // Default 128
    if (meta_bank == coll_bank && coll_addr > meta_addr) {
        // Minimum floor: the gap must be large enough for at least one metatile.
        // A gap smaller than metatile_size means the addresses are bogus
        // (e.g. coll_addr == meta_addr + 1 from a corrupt entry) and the
        // derived count would be zero — reject it and fall back to default.
        size_t gap = coll_addr - meta_addr;
        if (gap >= metatile_size) {
            size_t derived_count = gap / metatile_size;
            if (derived_count > 0 && derived_count <= 128) {
                metatile_count = derived_count;
            }
        }
        // If gap < metatile_size: addresses are corrupt; use the default count.
        // The subsequent metatile OOB check will catch it if that's wrong too.
    }
    
    if (meta_addr + metatile_count * metatile_size <= rom_.size()) {
        tileset.metatiles.reserve(metatile_count);
        for (size_t i = 0; i < metatile_count; ++i) {
            auto meta_data = rom_.read_bytes(meta_addr + i * metatile_size, metatile_size);
            
            Metatile mt;
            // Crystal metatile format: 16 bytes = 4×4 grid of tile indices
            // Row-major order: bytes 0-3 = row 0, bytes 4-7 = row 1, etc.
            // From Gen2Recomped/src/world/Map.lua:
            //   return block[(ty % 4) * 4 + (tx % 4) + 1]
            //
            // CRITICAL: Crystal tile byte normalization
            // Crystal uses a two-bank VRAM layout for tileset graphics:
            //   - Decompressed tiles 0-95 → VRAM bank 0 ($9000, vTiles2)
            //   - Decompressed tiles 96-191 → VRAM bank 1 ($9000, vTiles5)
            //
            // Source tile bytes encode VRAM bank selection via bit 7:
            //   - Bit 7 clear (0x00-0x7F): tile in VRAM bank 0
            //   - Bit 7 set (0x80-0xFF): tile in VRAM bank 1, local index = byte & 0x7F
            //
            // Crystal's _LoadOverworldAttrmapPals does: res 7, [hl] after extracting
            // the attrmap bank bit, so hardware sees (source & 0x7F) in the tilemap.
            //
            // We normalize to a flattened native tile index:
            //   Source 0x00-0x5F → native 0-95 (VRAM bank 0, tiles 0-95)
            //   Source 0x80-0xDF → native 96-191 (VRAM bank 1, tiles 96-191)
            //
            // Formula: native = (source & 0x80) ? ((source & 0x7F) + 96) : source
            for (int t = 0; t < 16; ++t) {
                uint8_t source_tile = meta_data[t];
                uint16_t native_tile;
                if (source_tile & 0x80) {
                    // VRAM bank 1: local tile index + 96
                    native_tile = (source_tile & 0x7F) + 96;
                } else {
                    // VRAM bank 0: direct index
                    native_tile = source_tile;
                }
                mt.tile_indices[t] = native_tile;
            }
            
            // Default attributes
            for (int j = 0; j < 16; ++j) {
                mt.attrs[j].collision = CollisionType::Walkable;
                mt.attrs[j].palette_index = 0;
                mt.attrs[j].flip_x = false;
                mt.attrs[j].flip_y = false;
                mt.attrs[j].priority = false;
            }
            mt.collision = CollisionType::Walkable;  // Set from collision data later
            
            tileset.metatiles.push_back(mt);
            stats_.metatiles_extracted++;
        }
    } else {
        // Metatile data out of ROM bounds — fail closed rather than producing
        // an empty tileset that would render as solid black in the game.
        result.error = std::format(
            "Metatile data out of ROM bounds for tileset '{}' "
            "(meta_addr=0x{:x} metatile_count={} metatile_size={} requires 0x{:x} bytes)",
            tileset.tileset_id, meta_addr, metatile_count, metatile_size,
            metatile_count * metatile_size);
        return result;
    }
    
    // Extract collision data: 4 bytes per metatile (TL, TR, BL, BR quadrants)
    // Reference: pokecrystal data/tilesets/*_collision.asm
    // Each metatile has 4 collision values for its 4 16x16 cells (2x2 cells per block)
    // Format: tilecoll TL, TR, BL, BR order in ROM
    // coll_addr already defined above for metatile_count derivation
    
    // 4 bytes per metatile × metatile_count
    const size_t coll_data_size = metatile_count * 4;
    
    if (coll_addr + coll_data_size <= rom_.size()) {
        tileset.collision.reserve(coll_data_size);
        
        for (size_t i = 0; i < coll_data_size; ++i) {
            uint8_t coll = rom_.read_byte(coll_addr + i);
            tileset.collision.push_back(coll);
        }
        
        // Also update metatile collision (use the TL value as representative)
        for (size_t m = 0; m < metatile_count && m < tileset.metatiles.size(); ++m) {
            tileset.metatiles[m].collision = static_cast<CollisionType>(tileset.collision[m * 4]);
        }
    } else {
        // Collision data out of ROM bounds — fail closed.  A tileset without
        // collision data cannot be used safely (all tiles would be walkable/solid
        // depending on the default, which is wrong for any real map).
        result.error = std::format(
            "Collision data out of ROM bounds for tileset '{}' "
            "(coll_addr=0x{:x} requires {} bytes)",
            tileset.tileset_id, coll_addr, coll_data_size);
        return result;
    }
    // Extract palette map
    // The palette map maps tile indices to BG palette IDs (0-6).
    // Crystal uses a two-bank VRAM layout:
    //   - Bank 0: decompressed tiles 0-95 (first 48 packed bytes)
    //   - Gap: 16 bytes of 0xFF filler (Crystal's "tiles 96-127" don't exist as graphics)
    //   - Bank 1: decompressed tiles 96-191 (next 48 packed bytes)
    // Each packed byte contains 2 palette IDs: low nybble for even tile, high for odd.
    // The high bit (0x08) in each nybble is OAM_BANK flag, mask off to get palette ID (0-6).
    // 
    // Note: Crystal's palette map uses source indices (0-95 bank 0, "128-223" bank 1),
    // but we normalize to native indices (0-95 bank 0, 96-191 bank 1).
    // See pokecrystal gfx/tilesets/*_palette_map.asm and tilepal macro.
    uint16_t palmap_ptr = entry[fmt.palmap_offset] | (entry[fmt.palmap_offset + 1] << 8);
    
    // The palette map data (gfx/tilesets/*_palette_map.asm) is placed in the same ROM
    // bank as the Tilesets table (data/tilesets.asm) by the linker.  The tileset entry
    // stores only a bank-local dw pointer — no bank byte — so the correct bank is the
    // bank of the Tilesets table itself.  Deriving it from profile_.offsets.tilesets
    // makes this work for Gold (bank 0x12), Crystal (bank 0x13), and any relocated hack
    // without per-ROM hardcoding.
    // profile_.offsets.tilesets != 0 is guaranteed by the guard at function entry.
    const uint8_t PALMAP_BANK = static_cast<uint8_t>(profile_.offsets.tilesets / 0x4000u);
    uint32_t palmap_addr = rom_.bank_to_flat(PALMAP_BANK, palmap_ptr);
    
    // Full palette map: bank 0 (48 bytes) + gap (16 bytes) + bank 1 (48 bytes) = 112 bytes
    constexpr size_t BANK0_PACKED_SIZE = 48;   // 96 tiles (0-95)
    constexpr size_t GAP_SIZE = 16;            // Filler for tiles 96-127 (not real graphics)
    constexpr size_t BANK1_PACKED_SIZE = 48;   // 96 tiles (96-191 after normalization)
    constexpr size_t FULL_PALMAP_SIZE = BANK0_PACKED_SIZE + GAP_SIZE + BANK1_PACKED_SIZE;
    constexpr size_t TOTAL_TILES = 192;        // Tiles 0-191 (bank 0: 0-95 + bank 1: 96-191)
    
    // Initialize palette map with default palette 0 for all 256 possible tile indices
    tileset.palette_map.resize(256, 0);
    
    if (palmap_addr + FULL_PALMAP_SIZE <= rom_.size()) {
        auto palmap_data = rom_.read_bytes(palmap_addr, FULL_PALMAP_SIZE);
        
        // Unpack bank 0 tiles (native indices 0-95)
        for (size_t i = 0; i < BANK0_PACKED_SIZE; ++i) {
            uint8_t packed = palmap_data[i];
            size_t tile_idx = i * 2;
            // Low nybble first (even tile), high nybble (odd tile)
            // Mask off bit 3 (OAM_BANK) to get palette ID 0-6
            tileset.palette_map[tile_idx] = packed & 0x07;
            tileset.palette_map[tile_idx + 1] = (packed >> 4) & 0x07;
        }
        
        // Skip gap (not used - Crystal's "tiles 96-127" have no graphics)
        
        // Unpack bank 1 tiles (native indices 96-191)
        // These are stored after the gap in the palette map ROM data
        for (size_t i = 0; i < BANK1_PACKED_SIZE; ++i) {
            uint8_t packed = palmap_data[BANK0_PACKED_SIZE + GAP_SIZE + i];
            size_t tile_idx = 96 + i * 2;  // Bank 1 starts at native tile 96
            tileset.palette_map[tile_idx] = packed & 0x07;
            tileset.palette_map[tile_idx + 1] = (packed >> 4) & 0x07;
        }
    }
    
    // Extract time-of-day palettes from TilesetBGPalette.
    // Address from profile (moved from inline constexpr 0x02:0x7319).
    // Format: 5 time-of-day sets (morn, day, nite, dark, indoor)
    // Each set has 8 palettes (7 BG + 1 text), each palette has 4 colors × 2 bytes.
    constexpr size_t PALETTE_SIZE = 8;      // bytes per palette (4 colors × 2 bytes)
    constexpr size_t PALETTES_PER_SET = 8;  // 7 BG + 1 text
    constexpr size_t SET_SIZE = PALETTE_SIZE * PALETTES_PER_SET;  // 64 bytes per time-of-day
    
    uint32_t palette_base = profile_.offsets.tileset_bg_palette;
    
    // Read all 5 time-of-day palette sets
    for (int tod = 0; tod < 5; ++tod) {
        uint32_t set_addr = palette_base + tod * SET_SIZE;
        
        if (set_addr + SET_SIZE <= rom_.size()) {
            auto pal_data = rom_.read_bytes(set_addr, SET_SIZE);
            
            for (int pal_id = 0; pal_id < 7; ++pal_id) {
                Palette& pal = tileset.time_palettes[tod][pal_id];
                
                for (int c = 0; c < 4; ++c) {
                    size_t offset = pal_id * PALETTE_SIZE + c * 2;
                    uint16_t gbc = pal_data[offset] | (pal_data[offset + 1] << 8);
                    pal.colors[c] = Color::from_gbc(gbc);
                }
            }
        }
    }
    
    // Copy Day palettes to legacy palette array for compatibility
    for (int p = 0; p < 7; ++p) {
        tileset.palettes[p] = tileset.time_palettes[static_cast<int>(TimeOfDay::Day)][p];
    }
    // Palette 7 is TEXT (not used for tiles)
    tileset.palettes[7] = tileset.time_palettes[static_cast<int>(TimeOfDay::Day)][0];
    
    // Extract special tileset palettes from the profile.
    // These are per-tileset palette overrides loaded from profile.offsets.special_tileset_palettes.
    // Previously they were hardcoded inline with SPECIAL_BANK=0x12 and fixed addresses.
    // Now the profile provides the (tileset_index, flat_rom_address) pairs, making
    // ROM hacks that relocate these palettes work without code changes.
    {
        const auto& stp = profile_.offsets;
        constexpr size_t SPECIAL_SIZE = 7 * 8;  // 7 palettes × 8 bytes each
        for (uint8_t pi = 0; pi < stp.special_tileset_palette_count; ++pi) {
            const auto& entry = stp.special_tileset_palettes[pi];
            if (entry.tileset_index != tileset_index) continue;

            uint32_t special_addr = entry.rom_address;
            if (special_addr + SPECIAL_SIZE <= rom_.size()) {
                auto special_data = rom_.read_bytes(special_addr, SPECIAL_SIZE);
                std::array<Palette, 7> special_set;
                for (int pal_id = 0; pal_id < 7; ++pal_id) {
                    for (int c = 0; c < 4; ++c) {
                        size_t offset = pal_id * 8 + c * 2;
                        uint16_t gbc = special_data[offset] | (special_data[offset + 1] << 8);
                        special_set[pal_id].colors[c] = Color::from_gbc(gbc);
                    }
                }
                tileset.fixed_special_palette = special_set;
            }
            break;
        }
    }
    
#ifndef NDEBUG
    tileset.debug.tileset_index = tileset_index;
    tileset.debug.gfx_rom_addr = gfx_addr;
    tileset.debug.metatile_rom_addr = meta_addr;
    tileset.debug.collision_rom_addr = coll_addr;
    tileset.debug.palmap_rom_addr = palmap_addr;
#endif
    
    stats_.tilesets_extracted++;
    result.success = true;
    return result;
}

TilesetExtractionResult TilesetExtractor::extract_tileset(const std::string& tileset_id) const {
    // Reverse lookup - Crystal tilesets are 1-indexed (1..num_tilesets)
    for (uint8_t i = 1; i <= profile_.counts.num_tilesets; ++i) {
        if (make_tileset_id(i) == tileset_id) {
            return extract_tileset(i);
        }
    }
    
    TilesetExtractionResult result;
    result.error = "Unknown tileset: " + tileset_id;
    return result;
}

std::vector<ExtractedTileset> TilesetExtractor::extract_all_tilesets() const {
    std::vector<ExtractedTileset> tilesets;
    
    // Crystal tilesets are 1-indexed (1..num_tilesets), not 0-indexed
    // From constants/tileset_constants.asm: const_def 1 means first constant is 1
    for (uint8_t i = 1; i <= profile_.counts.num_tilesets; ++i) {
        auto result = extract_tileset(i);
        if (result.success) {
            tilesets.push_back(std::move(result.tileset));
        }
    }
    
    return tilesets;
}

//=============================================================================
// RENDERING
//=============================================================================

TilesetAtlas render_tileset_atlas(const ExtractedTileset& tileset, TimeOfDay time_of_day) {
    TilesetAtlas atlas;
    atlas.tileset_id = tileset.tileset_id;
    
    // Atlas layout: 8 metatiles per row, each metatile is 32×32 pixels (4×4 tiles)
    // 8 metatiles × 32 pixels = 256 pixels wide
    // 128 metatiles / 8 per row = 16 rows × 32 pixels = 512 pixels tall
    const uint32_t metatiles_per_row = 8;
    const uint32_t metatile_pixels = 32;  // 4×4 8x8 tiles = 32×32 pixels
    
    size_t metatile_count = tileset.metatiles.size();
    uint32_t rows = (metatile_count + metatiles_per_row - 1) / metatiles_per_row;
    
    atlas.atlas_width = metatiles_per_row * metatile_pixels;
    atlas.atlas_height = rows * metatile_pixels;
    atlas.pixels.resize(atlas.atlas_width * atlas.atlas_height, 0xFF000000);  // Black/opaque
    
    // Get time-of-day palette set
    // Crystal palette IDs (from tileset_constants.asm):
    //   0 = GRAY, 1 = RED, 2 = GREEN, 3 = WATER, 4 = YELLOW, 5 = BROWN, 6 = ROOF
    const auto& tod_palettes = tileset.time_palettes[static_cast<int>(time_of_day)];
    
    // Render each metatile
    for (size_t m = 0; m < metatile_count; ++m) {
        const Metatile& mt = tileset.metatiles[m];
        
        // Position of this metatile in the atlas
        uint32_t mx = (m % metatiles_per_row) * metatile_pixels;
        uint32_t my = (m / metatiles_per_row) * metatile_pixels;
        
        // Each metatile has 16 tiles (4×4)
        // Index: row * 4 + col (row-major order)
        for (int t = 0; t < 16; ++t) {
            uint8_t tile_idx = mt.tile_indices[t];
            if (tile_idx >= tileset.tiles.size()) continue;
            
            const Tile& tile = tileset.tiles[tile_idx];
            
            // Get per-tile palette from palette_map
            // palette_map now covers tiles 0-255 with proper bank 0/1 mapping
            uint8_t palette_id = 0;
            if (tile_idx < tileset.palette_map.size()) {
                palette_id = tileset.palette_map[tile_idx];
            }
            // Clamp to valid palette range (0-6)
            if (palette_id > 6) {
                palette_id = 0;
            }
            const Palette& pal = tod_palettes[palette_id];
            
            // Position within metatile: t % 4 = col, t / 4 = row
            uint32_t tx = mx + (t % 4) * 8;
            uint32_t ty = my + (t / 4) * 8;
            
            // Render 8×8 tile
            for (int py = 0; py < 8; ++py) {
                for (int px = 0; px < 8; ++px) {
                    uint8_t pixel = tile.pixels[py * 8 + px];
                    uint32_t color = pal.colors[pixel].to_rgba32();
                    
                    uint32_t ax = tx + px;
                    uint32_t ay = ty + py;
                    atlas.pixels[ay * atlas.atlas_width + ax] = color;
                }
            }
        }
        
        // Store UV coordinates (32×32 pixel metatiles)
        TilesetAtlas::MetatileUV uv;
        uv.u0 = static_cast<float>(mx) / atlas.atlas_width;
        uv.v0 = static_cast<float>(my) / atlas.atlas_height;
        uv.u1 = static_cast<float>(mx + metatile_pixels) / atlas.atlas_width;
        uv.v1 = static_cast<float>(my + metatile_pixels) / atlas.atlas_height;
        atlas.metatile_uvs.push_back(uv);
    }
    
    // Copy collision data
    atlas.collision = tileset.collision;
    
    return atlas;
}

} // namespace crystal
