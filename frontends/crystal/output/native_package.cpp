// crystal/output/native_package.cpp
// Native package format implementation
//
// Writes extracted Crystal data to a ROM-independent package file.
// The package becomes the runtime's only data source after extraction.

#include "crystal/output/native_package.hpp"
#include "crystal/extract/map_extractor.hpp"
#include "crystal/extract/tileset_extractor.hpp"
#include "crystal/extract/font_extractor.hpp"
#include "crystal/world/collision_classifier.hpp"

#include <fstream>
#include <sstream>
#include <cstring>
#include <bit>
#include <format>
#include <stdexcept>
#include <numeric>
#include <array>
#include <iostream>
#include <stdexcept>
#include <unordered_set>

namespace crystal {

//=============================================================================
// CRC32 IMPLEMENTATION
//=============================================================================

// CRC32 lookup table (polynomial 0xEDB88320)
static constexpr uint32_t make_crc_table_entry(uint32_t n) {
    uint32_t c = n;
    for (int k = 0; k < 8; ++k) {
        c = (c & 1) ? (0xEDB88320 ^ (c >> 1)) : (c >> 1);
    }
    return c;
}

static constexpr std::array<uint32_t, 256> make_crc_table() {
    std::array<uint32_t, 256> table{};
    for (uint32_t n = 0; n < 256; ++n) {
        table[n] = make_crc_table_entry(n);
    }
    return table;
}

static constexpr auto crc_table = make_crc_table();

uint32_t calculate_crc32(const void* data, size_t size) {
    const auto* bytes = static_cast<const uint8_t*>(data);
    uint32_t crc = 0xFFFFFFFF;
    
    for (size_t i = 0; i < size; ++i) {
        crc = crc_table[(crc ^ bytes[i]) & 0xFF] ^ (crc >> 8);
    }
    
    return crc ^ 0xFFFFFFFF;
}

//=============================================================================
// SERIALIZATION HELPERS
//=============================================================================

// Write value as little-endian
template<typename T>
static void write_le(std::ostream& out, T value) {
    for (size_t i = 0; i < sizeof(T); ++i) {
        out.put(static_cast<char>(value & 0xFF));
        value >>= 8;
    }
}

// Write string with fixed size (null-padded)
static void write_fixed_string(std::ostream& out, const std::string& str, size_t size) {
    out.write(str.c_str(), std::min(str.size(), size - 1));
    size_t written = std::min(str.size(), size - 1);
    for (size_t i = written; i < size; ++i) {
        out.put('\0');
    }
}

// Write a count-prefixed array
template<typename T>
static void write_counted_array(std::ostream& out, const std::vector<T>& arr,
                                 void (*write_item)(std::ostream&, const T&)) {
    write_le(out, static_cast<uint32_t>(arr.size()));
    for (const auto& item : arr) {
        write_item(out, item);
    }
}

// Write a length-prefixed string as uint16_t length + raw bytes.
// The serialized length field is uint16_t (max 65535).  Any semantic ID
// or resource name that exceeds this is a compiler bug — fail hard rather
// than silently truncating the serialized bytes.
static void write_length_string(std::ostream& out, const std::string& s) {
    if (s.size() > 0xFFFF) {
        throw std::runtime_error(
            std::format("write_length_string: string length {} exceeds uint16_t max (65535). "
                        "String begins: '{}'",
                        s.size(), s.substr(0, 40)));
    }
    write_le(out, static_cast<uint16_t>(s.size()));
    out.write(s.data(), static_cast<std::streamsize>(s.size()));
}

// Write a chunk index key (string ID) with a checked uint16_t length prefix.
// Used in every chunk index entry:  uint16_t id_len, id bytes, uint32_t data_size.
// Same overflow contract as write_length_string — throws on ID > 65535 bytes.
static void write_chunk_id(std::ostream& out, const std::string& id) {
    if (id.size() > 0xFFFF) {
        throw std::runtime_error(
            std::format("write_chunk_id: resource ID length {} exceeds uint16_t max (65535). "
                        "ID begins: '{}'",
                        id.size(), id.substr(0, 40)));
    }
    write_le(out, static_cast<uint16_t>(id.size()));
    out.write(id.data(), static_cast<std::streamsize>(id.size()));
}

// Read value as little-endian
template<typename T>
static T read_le(std::istream& in) {
    T value = 0;
    for (size_t i = 0; i < sizeof(T); ++i) {
        value |= static_cast<T>(static_cast<uint8_t>(in.get())) << (i * 8);
    }
    return value;
}

// Read fixed-size string
static std::string read_fixed_string(std::istream& in, size_t size) {
    std::string str(size, '\0');
    in.read(str.data(), size);
    // Trim to null terminator
    auto null_pos = str.find('\0');
    if (null_pos != std::string::npos) {
        str.resize(null_pos);
    }
    return str;
}

//=============================================================================
// MAP SERIALIZATION
//=============================================================================

static void write_warp(std::ostream& out, const WarpPoint& warp) {
    out.put(warp.x);
    out.put(warp.y);
    out.put(warp.target_warp_index);
    write_length_string(out, warp.target_map_id);
}

static void write_coord_event(std::ostream& out, const CoordEvent& evt) {
    out.put(evt.x);
    out.put(evt.y);
    write_le(out, evt.scene_id);
    write_length_string(out, evt.script_id);
}

static void write_bg_event(std::ostream& out, const BgEvent& evt) {
    out.put(evt.x);
    out.put(evt.y);
    out.put(static_cast<uint8_t>(evt.type));
    out.put(evt.quantity);
    write_length_string(out, evt.script_id);
    write_length_string(out, evt.item_id);
    write_length_string(out, evt.condition_flag);
}

static void write_object(std::ostream& out, const ObjectEvent& obj) {
    out.put(obj.local_id);
    out.put(obj.x);
    out.put(obj.y);
    out.put(obj.movement_type);
    out.put(obj.movement_radius_x);
    out.put(obj.movement_radius_y);
    out.put(obj.hour_start);
    out.put(obj.hour_end);
    out.put(obj.palette);
    out.put(obj.is_trainer ? 1 : 0);
    out.put(obj.trainer_sight_range);
    
    write_length_string(out, obj.sprite_id);
    write_length_string(out, obj.script_id);
    write_length_string(out, obj.visibility_flag);
}

static void write_connection(std::ostream& out, const MapConnection& conn) {
    out.put(static_cast<uint8_t>(conn.direction));
    write_le(out, conn.src_skip_blocks);
    out.put(conn.strip_length_blocks);
    write_le(out, conn.coord_adjust_tiles);
    write_length_string(out, conn.target_map_id);
}

static std::vector<uint8_t> serialize_map(const ExtractedMap& map) {
    std::ostringstream out(std::ios::binary);
    
    // Fixed fields
    write_fixed_string(out, map.map_id, 64);
    write_fixed_string(out, map.display_name, 64);
    write_fixed_string(out, map.tileset_id, 32);
    write_fixed_string(out, map.music_id, 32);
    write_fixed_string(out, map.landmark_id, 32);
    write_fixed_string(out, map.map_script_id, 64);
    write_fixed_string(out, map.fish_group_id, 32);
    
    out.put(map.width);
    out.put(map.height);
    out.put(map.border_block);
    out.put(map.environment_type);
    
    // Flags packed into a byte
    uint8_t flags = 0;
    if (map.is_outdoor) flags |= 0x01;
    if (map.phone_service_disabled) flags |= 0x02;
    out.put(flags);
    
    out.put(map.lighting);
    out.put(0);  // padding
    out.put(0);  // padding
    
    // Block data
    write_le(out, static_cast<uint32_t>(map.blocks.size()));
    out.write(reinterpret_cast<const char*>(map.blocks.data()), map.blocks.size());
    
    // Events
    write_counted_array(out, map.warps, write_warp);
    write_counted_array(out, map.coord_events, write_coord_event);
    write_counted_array(out, map.bg_events, write_bg_event);
    write_counted_array(out, map.objects, write_object);
    write_counted_array(out, map.connections, write_connection);
    
    std::string data = out.str();
    return std::vector<uint8_t>(data.begin(), data.end());
}

//=============================================================================
// MAP DESERIALIZATION
//=============================================================================

static WarpPoint read_warp(std::istream& in) {
    WarpPoint warp;
    warp.x = in.get();
    warp.y = in.get();
    warp.target_warp_index = in.get();
    uint16_t id_len = read_le<uint16_t>(in);
    warp.target_map_id.resize(id_len);
    in.read(warp.target_map_id.data(), id_len);
    return warp;
}

static CoordEvent read_coord_event(std::istream& in) {
    CoordEvent evt;
    evt.x = in.get();
    evt.y = in.get();
    evt.scene_id = read_le<uint16_t>(in);
    uint16_t id_len = read_le<uint16_t>(in);
    evt.script_id.resize(id_len);
    in.read(evt.script_id.data(), id_len);
    return evt;
}

static BgEvent read_bg_event(std::istream& in) {
    BgEvent evt;
    evt.x = in.get();
    evt.y = in.get();
    evt.type = static_cast<BgEventType>(in.get());
    evt.quantity = in.get();
    uint16_t script_len = read_le<uint16_t>(in);
    evt.script_id.resize(script_len);
    in.read(evt.script_id.data(), script_len);
    uint16_t item_len = read_le<uint16_t>(in);
    evt.item_id.resize(item_len);
    in.read(evt.item_id.data(), item_len);
    uint16_t flag_len = read_le<uint16_t>(in);
    evt.condition_flag.resize(flag_len);
    in.read(evt.condition_flag.data(), flag_len);
    return evt;
}

static ObjectEvent read_object(std::istream& in) {
    ObjectEvent obj;
    obj.local_id = in.get();
    obj.x = in.get();
    obj.y = in.get();
    obj.movement_type = in.get();
    obj.movement_radius_x = in.get();
    obj.movement_radius_y = in.get();
    obj.hour_start = in.get();
    obj.hour_end = in.get();
    obj.palette = in.get();
    obj.is_trainer = (in.get() != 0);
    obj.trainer_sight_range = in.get();
    
    uint16_t sprite_len = read_le<uint16_t>(in);
    obj.sprite_id.resize(sprite_len);
    in.read(obj.sprite_id.data(), sprite_len);
    uint16_t script_len = read_le<uint16_t>(in);
    obj.script_id.resize(script_len);
    in.read(obj.script_id.data(), script_len);
    uint16_t flag_len = read_le<uint16_t>(in);
    obj.visibility_flag.resize(flag_len);
    in.read(obj.visibility_flag.data(), flag_len);
    
    return obj;
}

static MapConnection read_connection(std::istream& in) {
    MapConnection conn;
    conn.direction = static_cast<Direction>(in.get());
    conn.src_skip_blocks = read_le<int32_t>(in);
    conn.strip_length_blocks = in.get();
    conn.coord_adjust_tiles = read_le<int32_t>(in);
    uint16_t id_len = read_le<uint16_t>(in);
    conn.target_map_id.resize(id_len);
    in.read(conn.target_map_id.data(), id_len);
    return conn;
}

template<typename T>
static std::vector<T> read_counted_array(std::istream& in, T (*read_item)(std::istream&)) {
    uint32_t count = read_le<uint32_t>(in);
    std::vector<T> arr;
    arr.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        arr.push_back(read_item(in));
    }
    return arr;
}

static ExtractedMap deserialize_map(const std::vector<uint8_t>& data) {
    std::istringstream in(std::string(data.begin(), data.end()), std::ios::binary);
    
    ExtractedMap map;
    
    // Fixed fields
    map.map_id = read_fixed_string(in, 64);
    map.display_name = read_fixed_string(in, 64);
    map.tileset_id = read_fixed_string(in, 32);
    map.music_id = read_fixed_string(in, 32);
    map.landmark_id = read_fixed_string(in, 32);
    map.map_script_id = read_fixed_string(in, 64);
    map.fish_group_id = read_fixed_string(in, 32);
    
    map.width = in.get();
    map.height = in.get();
    map.border_block = in.get();
    map.environment_type = in.get();
    
    uint8_t flags = in.get();
    map.is_outdoor = (flags & 0x01) != 0;
    map.phone_service_disabled = (flags & 0x02) != 0;
    
    map.lighting = in.get();
    in.get();  // padding
    in.get();  // padding
    
    // Block data
    uint32_t block_count = read_le<uint32_t>(in);
    map.blocks.resize(block_count);
    in.read(reinterpret_cast<char*>(map.blocks.data()), block_count);
    
    // Events
    map.warps = read_counted_array(in, read_warp);
    map.coord_events = read_counted_array(in, read_coord_event);
    map.bg_events = read_counted_array(in, read_bg_event);
    map.objects = read_counted_array(in, read_object);
    map.connections = read_counted_array(in, read_connection);
    
    return map;
}

//=============================================================================
// PACKAGE WRITER
//=============================================================================

PackageWriter::PackageWriter() {
    std::memset(&header_, 0, sizeof(header_));
    header_.magic = PackageHeader::MAGIC;
    header_.version = PackageHeader::VERSION;
}

void PackageWriter::add_map(const ExtractedMap& map) {
    // Reject duplicate map IDs — inconsistent first-wins/last-wins reader behaviour
    // makes duplicates structurally invalid rather than merely undesirable.
    for (const auto& [existing_id, _] : map_data_) {
        if (existing_id == map.map_id) {
            throw std::runtime_error(
                std::format("PackageWriter::add_map: duplicate map ID '{}' — "
                            "each resource ID must be unique within its namespace",
                            map.map_id));
        }
    }
    auto data = serialize_map(map);
    
    // Store serialized data with map_id as key
    SerializedMap entry;
    std::memset(&entry, 0, sizeof(entry));
    std::strncpy(entry.map_id, map.map_id.c_str(), sizeof(entry.map_id) - 1);
    std::strncpy(entry.display_name, map.display_name.c_str(), sizeof(entry.display_name) - 1);
    std::strncpy(entry.tileset_id, map.tileset_id.c_str(), sizeof(entry.tileset_id) - 1);
    std::strncpy(entry.music_id, map.music_id.c_str(), sizeof(entry.music_id) - 1);
    entry.width = map.width;
    entry.height = map.height;
    entry.border_block = map.border_block;
    entry.environment_type = map.environment_type;
    entry.flags = 0;
    if (map.is_outdoor) entry.flags |= 0x01;
    if (map.phone_service_disabled) entry.flags |= 0x02;
    entry.lighting = map.lighting;
    
    maps_.push_back(entry);
    
    // Store full serialized data
    map_data_.push_back({map.map_id, std::move(data)});
    
    stats_.maps_written++;
}

void PackageWriter::add_tileset_atlas(const TilesetAtlas& atlas) {
    for (const auto& [existing_id, _] : tileset_data_) {
        if (existing_id == atlas.tileset_id) {
            throw std::runtime_error(
                std::format("PackageWriter::add_tileset_atlas: duplicate tileset ID '{}'",
                            atlas.tileset_id));
        }
    }
    std::ostringstream out(std::ios::binary);
    
    // Header
    write_le(out, atlas.atlas_width);
    write_le(out, atlas.atlas_height);
    write_le(out, static_cast<uint32_t>(atlas.pixels.size()));
    
    // Pixel data (RGBA32)
    out.write(reinterpret_cast<const char*>(atlas.pixels.data()), 
              atlas.pixels.size() * sizeof(uint32_t));
    
    // Metatile UV count
    write_le(out, static_cast<uint32_t>(atlas.metatile_uvs.size()));
    
    // UV data
    for (const auto& uv : atlas.metatile_uvs) {
        write_le(out, std::bit_cast<uint32_t>(uv.u0));
        write_le(out, std::bit_cast<uint32_t>(uv.v0));
        write_le(out, std::bit_cast<uint32_t>(uv.u1));
        write_le(out, std::bit_cast<uint32_t>(uv.v1));
    }
    
    // Collision data
    write_le(out, static_cast<uint32_t>(atlas.collision.size()));
    for (auto coll : atlas.collision) {
        out.put(static_cast<uint8_t>(coll));
    }
    
    std::string data = out.str();
    tileset_data_.push_back({atlas.tileset_id, std::vector<uint8_t>(data.begin(), data.end())});
    stats_.tilesets_written++;
}

void PackageWriter::add_tileset(const ExtractedTileset& tileset, TimeOfDay tod) {
    for (const auto& [existing_id, _] : tileset_data_) {
        if (existing_id == tileset.tileset_id) {
            throw std::runtime_error(
                std::format("PackageWriter::add_tileset: duplicate tileset ID '{}'",
                            tileset.tileset_id));
        }
    }
    // Serialize native indexed tiles + palette data
    // Format:
    //   tile_count (u32)
    //   tiles[tile_count] - each tile is 64 indexed pixels (64 bytes)
    //   block_count (u32)
    //   blocks[block_count] - each block is 16 tile IDs (32 bytes, u16 each)
    //   collision_count (u32)
    //   collision[collision_count] - raw collision bytes
    //   palette_map_size (u32)
    //   palette_map[palette_map_size] - tile_id → palette_id (1 byte each)
    //   standard_palette_rows[5] - 5 rows × 7 palettes × 4 colors × 4 bytes (RGBA32)
    //   has_fixed_special_palette (u8)
    //   [if has_fixed] fixed_special_palette - 7 palettes × 4 colors × 4 bytes (RGBA32)
    
    std::ostringstream out(std::ios::binary);
    
    // Write tile count
    write_le(out, static_cast<uint32_t>(tileset.tiles.size()));
    
    // Write each tile as 64 indexed pixels (NOT pre-colored RGBA)
    for (const auto& tile : tileset.tiles) {
        out.write(reinterpret_cast<const char*>(tile.pixels.data()), 64);
    }
    
    // Write block count
    write_le(out, static_cast<uint32_t>(tileset.metatiles.size()));
    
    // Write each block's 16 tile IDs
    for (const auto& block : tileset.metatiles) {
        for (int i = 0; i < 16; ++i) {
            write_le(out, static_cast<uint16_t>(block.tile_indices[i]));
        }
    }
    
    // Write collision count and data (CLASSIFIED to semantic CollisionClass)
    // The Crystal classifier converts raw bytes to semantic values at packaging time
    // Runtime never sees raw Crystal collision bytes
    write_le(out, static_cast<uint32_t>(tileset.collision.size()));
    for (auto coll : tileset.collision) {
        // Classify Crystal raw byte to semantic CollisionClass
        auto coll_class = crystal::classify_crystal_collision(coll);
        out.put(static_cast<uint8_t>(coll_class));
    }
    
    // Write palette map
    write_le(out, static_cast<uint32_t>(tileset.palette_map.size()));
    for (auto pal_id : tileset.palette_map) {
        out.put(pal_id);
    }
    
    // Helper to write a palette set (7 palettes × 4 colors × RGBA32)
    auto write_palette_set = [&out](const std::array<Palette, 7>& palettes) {
        for (int pal_id = 0; pal_id < 7; ++pal_id) {
            for (int c = 0; c < 4; ++c) {
                uint32_t rgba = palettes[pal_id].colors[c].to_rgba32();
                write_le(out, rgba);
            }
        }
    };
    
    // Write all 5 standard palette rows
    for (int row = 0; row < 5; ++row) {
        write_palette_set(tileset.time_palettes[row]);
    }
    
    // Write fixed special palette (if present)
    if (tileset.fixed_special_palette.has_value()) {
        out.put(1);  // has_fixed = true
        write_palette_set(*tileset.fixed_special_palette);
    } else {
        out.put(0);  // has_fixed = false
    }
    
    std::string data = out.str();
    tileset_data_.push_back({tileset.tileset_id, std::vector<uint8_t>(data.begin(), data.end())});
    stats_.tilesets_written++;
    
    std::cout << "[PACKAGE] Tileset " << tileset.tileset_id << ": "
              << tileset.tiles.size() << " indexed tiles, "
              << tileset.metatiles.size() << " blocks, "
              << tileset.collision.size() << " collision bytes, "
              << tileset.palette_map.size() << " palette map entries"
              << (tileset.fixed_special_palette ? " [SPECIAL PALETTE]" : "")
              << "\n";
}

void PackageWriter::add_font_atlas(const FontAtlas& atlas) {
    for (const auto& [existing_id, _] : font_data_) {
        if (existing_id == atlas.font_id) {
            throw std::runtime_error(
                std::format("PackageWriter::add_font_atlas: duplicate font ID '{}'",
                            atlas.font_id));
        }
    }
    std::ostringstream out(std::ios::binary);
    
    // Header: dimensions
    write_le(out, atlas.atlas_width);
    write_le(out, atlas.atlas_height);
    
    // Pixel data (RGBA32)
    write_le(out, static_cast<uint32_t>(atlas.pixels.size()));
    out.write(reinterpret_cast<const char*>(atlas.pixels.data()), 
              atlas.pixels.size() * sizeof(uint32_t));
    
    // Glyph UV count and data
    write_le(out, static_cast<uint32_t>(atlas.glyph_uvs.size()));
    for (const auto& uv : atlas.glyph_uvs) {
        write_le(out, std::bit_cast<uint32_t>(uv.u0));
        write_le(out, std::bit_cast<uint32_t>(uv.v0));
        write_le(out, std::bit_cast<uint32_t>(uv.u1));
        write_le(out, std::bit_cast<uint32_t>(uv.v1));
    }
    
    // Charmap entries (native format: UTF-8 → GlyphId)
    // NOTE: crystal_code is NOT serialized - it's frontend/compiler provenance only
    write_le(out, static_cast<uint32_t>(atlas.charmap.size()));
    for (const auto& entry : atlas.charmap) {
        // Native charmap entry format (v2):
        //   glyph_index: u16
        //   is_control: u8
        //   control_name_len: u16, control_name: bytes
        //   utf8_len: u16, utf8_char: bytes
        write_le(out, entry.glyph_index);
        out.put(entry.is_control ? 1 : 0);
        write_length_string(out, entry.control_name);
        write_length_string(out, entry.utf8_char);
    }
    
    // Special glyph indices
    write_le(out, atlas.border_top_left);
    write_le(out, atlas.border_top);
    write_le(out, atlas.border_top_right);
    write_le(out, atlas.border_left);
    write_le(out, atlas.border_bottom_left);
    write_le(out, atlas.border_bottom_right);
    write_le(out, atlas.space_glyph);
    write_le(out, atlas.cursor_glyph);
    
    std::string data = out.str();
    font_data_.push_back({atlas.font_id, std::vector<uint8_t>(data.begin(), data.end())});
}

void PackageWriter::add_script(const std::string& script_id, const std::string& lua_code) {
    for (const auto& [existing_id, _] : script_data_) {
        if (existing_id == script_id) {
            throw std::runtime_error(
                std::format("PackageWriter::add_script: duplicate script ID '{}'",
                            script_id));
        }
    }
    script_data_.push_back({script_id, lua_code});
    stats_.scripts_written++;
}

void PackageWriter::add_sprite(const RuntimeSprite& sprite) {
    for (const auto& [existing_id, _] : sprite_data_) {
        if (existing_id == sprite.sprite_id) {
            throw std::runtime_error(
                std::format("PackageWriter::add_sprite: duplicate sprite ID '{}'",
                            sprite.sprite_id));
        }
    }
    std::ostringstream out(std::ios::binary);
    
    // Header: sprite_id, type, palette
    write_length_string(out, sprite.sprite_id);
    out.put(static_cast<uint8_t>(sprite.type));
    out.put(static_cast<uint8_t>(sprite.default_palette));
    
    // Frame count and frames
    write_le(out, static_cast<uint32_t>(sprite.frames.size()));
    for (const auto& frame : sprite.frames) {
        // Each frame is 256 bytes (16x16 pixels, values 0-3)
        out.write(reinterpret_cast<const char*>(frame.pixels.data()), 256);
    }

    // Icon frames (SpriteType::Icon only — 32×32 animation frames)
    write_le(out, static_cast<uint32_t>(sprite.icon_frames.size()));
    for (const auto& iframe : sprite.icon_frames) {
        // Each icon frame is 1024 bytes (32×32 pixels, values 0-3)
        out.write(reinterpret_cast<const char*>(iframe.pixels.data()), 1024);
    }
    
    std::string data = out.str();
    sprite_data_.push_back({sprite.sprite_id, std::vector<uint8_t>(data.begin(), data.end())});
}

void PackageWriter::add_obj_palettes(const SpriteObjPalettes& palettes) {
    std::ostringstream out(std::ios::binary);
    
    // 4 time-of-day variants × 8 palettes × 4 colors × 2 bytes (RGB555)
    for (int tod = 0; tod < 4; ++tod) {
        for (int pal = 0; pal < 8; ++pal) {
            const auto& sprite_pal = palettes.time_palettes[tod][pal];
            for (int c = 0; c < 4; ++c) {
                write_le(out, sprite_pal.colors_gbc[c]);
            }
        }
    }
    
    std::string data = out.str();
    obj_palettes_data_ = std::vector<uint8_t>(data.begin(), data.end());
}

void PackageWriter::add_base_stats(const std::vector<SpeciesBaseStatsEntry>& entries) {
    if (!base_stats_data_.empty()) {
        throw std::runtime_error("PackageWriter::add_base_stats: called more than once");
    }
    std::unordered_set<enginemon::SpeciesId> seen_species;
    for (const auto& e : entries) {
        if (!seen_species.insert(e.id).second) {
            throw std::runtime_error(
                std::format("PackageWriter::add_base_stats: duplicate SpeciesId {}", e.id));
        }
    }
    // Serialize directly into a vector<uint8_t> — no stringstream
    auto count32 = static_cast<uint32_t>(entries.size());
    std::vector<uint8_t> buf;
    buf.reserve(4 + entries.size() * 14);
    // u32 count LE
    buf.push_back(static_cast<uint8_t>(count32 & 0xFF));
    buf.push_back(static_cast<uint8_t>((count32 >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>((count32 >> 16) & 0xFF));
    buf.push_back(static_cast<uint8_t>((count32 >> 24) & 0xFF));
    for (const auto& e : entries) {
        auto sid = static_cast<uint16_t>(e.id);
        buf.push_back(static_cast<uint8_t>(sid & 0xFF));
        buf.push_back(static_cast<uint8_t>((sid >> 8) & 0xFF));
        buf.push_back(e.hp);
        buf.push_back(e.attack);
        buf.push_back(e.defense);
        buf.push_back(e.speed);
        buf.push_back(e.sp_atk);
        buf.push_back(e.sp_def);
        buf.push_back(e.type1);
        buf.push_back(e.type2);
        buf.push_back(e.catch_rate);
        buf.push_back(e.base_exp);
        buf.push_back(e.gender_ratio);
        buf.push_back(0);  // reserved
    }
    base_stats_data_ = std::move(buf);
}

void PackageWriter::add_move_data(const std::vector<MoveDataEntry>& entries) {
    if (!move_data_data_.empty()) {
        throw std::runtime_error("PackageWriter::add_move_data: called more than once");
    }
    std::unordered_set<enginemon::MoveId> seen_moves;
    for (const auto& e : entries) {
        if (!seen_moves.insert(e.id).second) {
            throw std::runtime_error(
                std::format("PackageWriter::add_move_data: duplicate MoveId {}", e.id));
        }
    }
    auto count32 = static_cast<uint32_t>(entries.size());
    std::vector<uint8_t> buf;
    buf.reserve(4 + entries.size() * 9);
    buf.push_back(static_cast<uint8_t>(count32 & 0xFF));
    buf.push_back(static_cast<uint8_t>((count32 >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>((count32 >> 16) & 0xFF));
    buf.push_back(static_cast<uint8_t>((count32 >> 24) & 0xFF));
    for (const auto& e : entries) {
        auto mid = static_cast<uint16_t>(e.id);
        buf.push_back(static_cast<uint8_t>(mid & 0xFF));
        buf.push_back(static_cast<uint8_t>((mid >> 8) & 0xFF));
        buf.push_back(e.type_id);
        buf.push_back(e.power);
        buf.push_back(e.accuracy);
        buf.push_back(e.pp);
        buf.push_back(e.effect_id);
        buf.push_back(e.effect_chance);
        buf.push_back(0);  // reserved
    }
    move_data_data_ = std::move(buf);
}

void PackageWriter::add_battle_rules(const enginemon::BattleRules& rules) {
    if (!battle_rules_data_.empty()) {
        throw std::runtime_error("PackageWriter::add_battle_rules: called more than once");
    }
    if (!rules.is_valid()) {
        throw std::runtime_error("PackageWriter::add_battle_rules: rules.is_valid() is false — "
                                 "extract_battle_rules must succeed before calling this");
    }

    // Wire format (all little-endian where multi-byte):
    //
    //  [stat_stage_mult]      13 × {u8 num, u8 den}          = 26 bytes
    //  [acc_stage_mult]       13 × {u8 num, u8 den}          = 26 bytes
    //  [crit_chances]         7  × u8                        =  7 bytes
    //  [wobble_count]         u8                             =  1 byte
    //  [wobble_probabilities] wobble_count × {u8 thr, u8 wob}= N×2 bytes
    //  [weather_type_count]   u8                             =  1 byte
    //  [weather_type_mods]    count × {u8 wid, u8 tid, u8 mul}= N×3 bytes
    //  [weather_move_count]   u8                             =  1 byte
    //  [weather_move_mods]    count × {u8 wid, u8 eid, u8 mul}= N×3 bytes
    //  [high_crit_count]      u16 LE                         =  2 bytes
    //  [high_crit_moves]      count × u16 LE                 = N×2 bytes
    //  [eff_priority_count]   u8                             =  1 byte
    //  [eff_priorities]       count × {u8 eid, u8 pri}       = N×2 bytes
    //  [status_only_count]    u8                             =  1 byte
    //  [status_only_effects]  count × u8                     = N bytes
    //  [risky_count]          u8                             =  1 byte
    //  [risky_effects]        count × u8                     = N bytes
    //  [stall_count]          u8                             =  1 byte
    //  [stall_move_ids]       count × u8                     = N bytes
    //  [useful_count]         u8                             =  1 byte
    //  [useful_move_ids]      count × u8                     = N bytes
    //  [residual_count]       u8                             =  1 byte
    //  [residual_move_ids]    count × u8                     = N bytes
    //  [encore_count]         u8                             =  1 byte
    //  [encore_move_ids]      count × u8                     = N bytes
    //  [trainer_class_count]  u16 LE                         =  2 bytes
    //  [trainer_class_ai]     count × {u8 item1, u8 item2, u8 reward, u16 LE move_flags, u16 LE item_flags}
    //                                                        = count×7 bytes

    std::vector<uint8_t> buf;
    buf.reserve(512);

    auto push_u8  = [&](uint8_t v) { buf.push_back(v); };
    auto push_u16 = [&](uint16_t v) {
        buf.push_back(static_cast<uint8_t>(v & 0xFF));
        buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    };

    // stat_stage_mult: 13 × {num, den}
    for (const auto& e : rules.stat_stage_mult) {
        push_u8(e.numerator);
        push_u8(e.denominator);
    }

    // acc_stage_mult: 13 × {num, den}
    for (const auto& e : rules.acc_stage_mult) {
        push_u8(e.numerator);
        push_u8(e.denominator);
    }

    // crit_chances: 7 × u8
    for (uint8_t c : rules.crit_chances) push_u8(c);

    // wobble_probabilities: u8 count + count × {thr, wob}
    push_u8(static_cast<uint8_t>(rules.wobble_probabilities.size()));
    for (const auto& w : rules.wobble_probabilities) {
        push_u8(w[0]);
        push_u8(w[1]);
    }

    // weather_type_modifiers: u8 count + count × {wid, tid, mul}
    push_u8(static_cast<uint8_t>(rules.weather_type_modifiers.size()));
    for (const auto& w : rules.weather_type_modifiers) {
        push_u8(w.weather_id);
        push_u8(w.type_id);
        push_u8(w.multiplier);
    }

    // weather_move_modifiers: u8 count + count × {wid, eid, mul}
    push_u8(static_cast<uint8_t>(rules.weather_move_modifiers.size()));
    for (const auto& w : rules.weather_move_modifiers) {
        push_u8(w.weather_id);
        push_u8(w.type_id);
        push_u8(w.multiplier);
    }

    // high_crit_moves: u16 count + count × u16 LE (full MoveId range — supports >255)
    push_u16(static_cast<uint16_t>(rules.high_crit_moves.size()));
    for (enginemon::MoveId m : rules.high_crit_moves)
        push_u16(static_cast<uint16_t>(m));

    // effect_priorities: u8 count + count × {eid, priority}
    push_u8(static_cast<uint8_t>(rules.effect_priorities.size()));
    for (const auto& e : rules.effect_priorities) {
        push_u8(e.effect_id);
        push_u8(e.priority);
    }

    // ai lists: each is u8 count + count × u8
    auto push_byte_list = [&](const std::vector<uint8_t>& list) {
        push_u8(static_cast<uint8_t>(list.size()));
        for (uint8_t b : list) push_u8(b);
    };
    push_byte_list(rules.ai_status_only_effects);
    push_byte_list(rules.ai_risky_effects);
    push_byte_list(rules.ai_stall_move_ids);
    push_byte_list(rules.ai_useful_move_ids);
    push_byte_list(rules.ai_residual_move_ids);
    push_byte_list(rules.ai_encore_move_ids);

    // trainer_class_ai: u16 LE count + count × 7 bytes
    push_u16(static_cast<uint16_t>(rules.trainer_class_ai.size()));
    for (const auto& t : rules.trainer_class_ai) {
        push_u8(t.item1);
        push_u8(t.item2);
        push_u8(t.base_reward);
        // Repack AIPassSet back to Crystal TRNATTR_AI_MOVE_WEIGHTS bitmask for wire.
        // Crystal bit positions (trainer_data_constants.asm):
        //   bit 0=BASIC  bit 1=SETUP  bit 2=TYPES  bit 3=OFFENSIVE  bit 4=SMART
        uint16_t move_flags = 0u;
        if (t.ai_passes.run_basic)     move_flags |= (1u << 0);
        if (t.ai_passes.run_setup)     move_flags |= (1u << 1);
        if (t.ai_passes.run_types)     move_flags |= (1u << 2);
        if (t.ai_passes.run_offensive) move_flags |= (1u << 3);
        if (t.ai_passes.run_smart)     move_flags |= (1u << 4);
        push_u16(move_flags);
        push_u16(t.ai_item_flags);
    }

    battle_rules_data_ = std::move(buf);
}

void PackageWriter::add_species_icon_map(
    const std::vector<SpeciesIconEntry>& entries)
{
    // Validate: no duplicates, no empty icon IDs.
    std::unordered_set<enginemon::SpeciesId> seen_species;
    for (const auto& [species, icon_id] : entries) {
        if (icon_id.empty()) {
            throw std::runtime_error(std::format(
                "add_species_icon_map: species {} has empty icon_id", species));
        }
        if (!seen_species.insert(species).second) {
            throw std::runtime_error(std::format(
                "add_species_icon_map: duplicate species entry {}", species));
        }
        if (!icon_id.starts_with("pokemon_icon:")) {
            throw std::runtime_error(std::format(
                "add_species_icon_map: icon_id '{}' for species {} must start with "
                "'pokemon_icon:'", icon_id, species));
        }
    }

    // Serialize: uint32_t count, then [uint16_t species, uint16_t name_len, name_bytes]
    std::ostringstream out(std::ios::binary);
    write_le(out, static_cast<uint32_t>(entries.size()));
    for (const auto& [species, icon_id] : entries) {
        write_le(out, static_cast<uint16_t>(species));
        write_chunk_id(out, icon_id);  // uint16_t len + bytes
    }

    std::string data = out.str();
    species_icon_map_data_ = std::vector<uint8_t>(data.begin(), data.end());
}

void PackageWriter::set_source_rom(const std::string& sha1, const std::string& version) {
    std::strncpy(header_.source_sha1, sha1.c_str(), sizeof(header_.source_sha1) - 1);
    std::strncpy(header_.source_version, version.c_str(), sizeof(header_.source_version) - 1);
}

bool PackageWriter::write(const std::filesystem::path& path) const {
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    
    // Reserve space for header (will rewrite later)
    out.write(reinterpret_cast<const char*>(&header_), sizeof(header_));
    
    // Build and write chunks
    std::vector<TocEntry> toc;
    std::vector<uint8_t> all_data;
    
    // Maps chunk
    if (!map_data_.empty()) {
        TocEntry entry;
        entry.type = ChunkType::Maps;
        entry.offset = static_cast<uint32_t>(sizeof(PackageHeader) + all_data.size());
        entry.count = static_cast<uint32_t>(map_data_.size());
        
        std::ostringstream chunk(std::ios::binary);
        
        // Write map index (id -> offset within chunk)
        for (const auto& [id, data] : map_data_) {
            write_chunk_id(chunk, id);
            write_le(chunk, static_cast<uint32_t>(data.size()));
        }
        
        // Write map data
        for (const auto& [id, data] : map_data_) {
            chunk.write(reinterpret_cast<const char*>(data.data()), data.size());
        }
        
        std::string chunk_data = chunk.str();
        entry.size = static_cast<uint32_t>(chunk_data.size());
        entry.crc32 = calculate_crc32(chunk_data.data(), chunk_data.size());
        
        toc.push_back(entry);
        all_data.insert(all_data.end(), chunk_data.begin(), chunk_data.end());
    }
    
    // Tileset atlas chunk
    if (!tileset_data_.empty()) {
        TocEntry entry;
        entry.type = ChunkType::TilesetAtlases;
        entry.offset = static_cast<uint32_t>(sizeof(PackageHeader) + all_data.size());
        entry.count = static_cast<uint32_t>(tileset_data_.size());
        
        std::ostringstream chunk(std::ios::binary);
        
        // Write index (id -> size)
        for (const auto& [id, data] : tileset_data_) {
            write_chunk_id(chunk, id);
            write_le(chunk, static_cast<uint32_t>(data.size()));
        }
        
        // Write tileset data
        for (const auto& [id, data] : tileset_data_) {
            chunk.write(reinterpret_cast<const char*>(data.data()), data.size());
        }
        
        std::string chunk_data = chunk.str();
        entry.size = static_cast<uint32_t>(chunk_data.size());
        entry.crc32 = calculate_crc32(chunk_data.data(), chunk_data.size());
        
        toc.push_back(entry);
        all_data.insert(all_data.end(), chunk_data.begin(), chunk_data.end());
    }
    
    // Font atlas chunk
    if (!font_data_.empty()) {
        TocEntry entry;
        entry.type = ChunkType::Fonts;
        entry.offset = static_cast<uint32_t>(sizeof(PackageHeader) + all_data.size());
        entry.count = static_cast<uint32_t>(font_data_.size());
        
        std::ostringstream chunk(std::ios::binary);
        
        // Write index (id -> size)
        for (const auto& [id, data] : font_data_) {
            write_chunk_id(chunk, id);
            write_le(chunk, static_cast<uint32_t>(data.size()));
        }
        
        // Write font data
        for (const auto& [id, data] : font_data_) {
            chunk.write(reinterpret_cast<const char*>(data.data()), data.size());
        }
        
        std::string chunk_data = chunk.str();
        entry.size = static_cast<uint32_t>(chunk_data.size());
        entry.crc32 = calculate_crc32(chunk_data.data(), chunk_data.size());
        
        toc.push_back(entry);
        all_data.insert(all_data.end(), chunk_data.begin(), chunk_data.end());
    }
    
    // Scripts chunk (ScriptId → Lua code)
    if (!script_data_.empty()) {
        TocEntry entry;
        entry.type = ChunkType::Scripts;
        entry.offset = static_cast<uint32_t>(sizeof(PackageHeader) + all_data.size());
        entry.count = static_cast<uint32_t>(script_data_.size());
        
        std::ostringstream chunk(std::ios::binary);
        
        // Write index (script_id -> lua_code_size)
        for (const auto& [id, lua_code] : script_data_) {
            write_chunk_id(chunk, id);
            write_le(chunk, static_cast<uint32_t>(lua_code.size()));
        }
        
        // Write script Lua code
        for (const auto& [id, lua_code] : script_data_) {
            chunk.write(lua_code.data(), lua_code.size());
        }
        
        std::string chunk_data = chunk.str();
        entry.size = static_cast<uint32_t>(chunk_data.size());
        entry.crc32 = calculate_crc32(chunk_data.data(), chunk_data.size());
        
        toc.push_back(entry);
        all_data.insert(all_data.end(), chunk_data.begin(), chunk_data.end());
    }
    
    // Sprites chunk (sprite_id → RuntimeSprite data)
    if (!sprite_data_.empty()) {
        TocEntry entry;
        entry.type = ChunkType::Sprites;
        entry.offset = static_cast<uint32_t>(sizeof(PackageHeader) + all_data.size());
        entry.count = static_cast<uint32_t>(sprite_data_.size());
        
        std::ostringstream chunk(std::ios::binary);
        
        // Write index (sprite_id -> data_size)
        for (const auto& [id, data] : sprite_data_) {
            write_chunk_id(chunk, id);
            write_le(chunk, static_cast<uint32_t>(data.size()));
        }
        
        // Write sprite data
        for (const auto& [id, data] : sprite_data_) {
            chunk.write(reinterpret_cast<const char*>(data.data()), data.size());
        }
        
        std::string chunk_data = chunk.str();
        entry.size = static_cast<uint32_t>(chunk_data.size());
        entry.crc32 = calculate_crc32(chunk_data.data(), chunk_data.size());
        
        toc.push_back(entry);
        all_data.insert(all_data.end(), chunk_data.begin(), chunk_data.end());
    }
    
    // OBJ Palettes chunk (single blob, shared across all sprites)
    if (!obj_palettes_data_.empty()) {
        TocEntry entry;
        entry.type = ChunkType::ObjPalettes;
        entry.offset = static_cast<uint32_t>(sizeof(PackageHeader) + all_data.size());
        entry.count = 1;  // Single palettes blob
        entry.size = static_cast<uint32_t>(obj_palettes_data_.size());
        entry.crc32 = calculate_crc32(obj_palettes_data_.data(), obj_palettes_data_.size());
        
        toc.push_back(entry);
        all_data.insert(all_data.end(), obj_palettes_data_.begin(), obj_palettes_data_.end());
    }

    // SpeciesIconMap chunk — species→icon mapping compiled from Crystal MonMenuIcons
    if (!species_icon_map_data_.empty()) {
        TocEntry entry;
        entry.type = ChunkType::SpeciesIconMap;
        entry.offset = static_cast<uint32_t>(sizeof(PackageHeader) + all_data.size());
        entry.count = 1;
        entry.size = static_cast<uint32_t>(species_icon_map_data_.size());
        entry.crc32 = calculate_crc32(species_icon_map_data_.data(),
                                      species_icon_map_data_.size());
        toc.push_back(entry);
        all_data.insert(all_data.end(),
                        species_icon_map_data_.begin(), species_icon_map_data_.end());
    }

    // BaseStats chunk — SpeciesId → base stats, types, misc (single flat blob)
    if (!base_stats_data_.empty()) {
        TocEntry bss_entry;
        bss_entry.type = ChunkType::BaseStats;
        bss_entry.offset = static_cast<uint32_t>(sizeof(PackageHeader) + all_data.size());
        bss_entry.count = 1;
        bss_entry.size = static_cast<uint32_t>(base_stats_data_.size());
        bss_entry.crc32 = calculate_crc32(base_stats_data_.data(), base_stats_data_.size());
        toc.push_back(bss_entry);
        all_data.insert(all_data.end(), base_stats_data_.begin(), base_stats_data_.end());
    }

    // MoveData chunk — MoveId → power/type/accuracy/pp/effect (single flat blob)
    if (!move_data_data_.empty()) {
        TocEntry mvd_entry;
        mvd_entry.type = ChunkType::MoveData;
        mvd_entry.offset = static_cast<uint32_t>(sizeof(PackageHeader) + all_data.size());
        mvd_entry.count = 1;
        mvd_entry.size = static_cast<uint32_t>(move_data_data_.size());
        mvd_entry.crc32 = calculate_crc32(move_data_data_.data(), move_data_data_.size());
        toc.push_back(mvd_entry);
        all_data.insert(all_data.end(), move_data_data_.begin(), move_data_data_.end());
    }

    // BattleRules chunk — ROM-derived battle tables (single flat blob)
    if (!battle_rules_data_.empty()) {
        TocEntry brl_entry;
        brl_entry.type   = ChunkType::BattleRules;
        brl_entry.offset = static_cast<uint32_t>(sizeof(PackageHeader) + all_data.size());
        brl_entry.count  = 1;
        brl_entry.size   = static_cast<uint32_t>(battle_rules_data_.size());
        brl_entry.crc32  = calculate_crc32(battle_rules_data_.data(), battle_rules_data_.size());
        toc.push_back(brl_entry);
        all_data.insert(all_data.end(), battle_rules_data_.begin(), battle_rules_data_.end());
    }
    
    // Write data
    out.write(reinterpret_cast<const char*>(all_data.data()), all_data.size());
    
    // Write TOC
    uint32_t toc_offset = static_cast<uint32_t>(out.tellp());
    for (const auto& entry : toc) {
        write_le(out, static_cast<uint32_t>(entry.type));
        write_le(out, entry.offset);
        write_le(out, entry.size);
        write_le(out, entry.count);
        write_le(out, entry.crc32);
    }
    uint32_t toc_size = static_cast<uint32_t>(out.tellp()) - toc_offset;
    
    // Rewrite header with TOC info
    PackageHeader final_header = header_;
    final_header.toc_offset = toc_offset;
    final_header.toc_size = toc_size;
    final_header.data_crc32 = calculate_crc32(all_data.data(), all_data.size());
    
    out.seekp(0);
    out.write(reinterpret_cast<const char*>(&final_header), sizeof(final_header));
    
    return out.good();
}

//=============================================================================
// PACKAGE READER
//=============================================================================

std::unique_ptr<PackageReader> PackageReader::open(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return nullptr;
    
    auto reader = std::unique_ptr<PackageReader>(new PackageReader());
    reader->path_ = path;
    
    // Read header
    in.read(reinterpret_cast<char*>(&reader->header_), sizeof(PackageHeader));
    
    // Validate magic
    if (reader->header_.magic != PackageHeader::MAGIC) {
        return nullptr;
    }
    
    // Validate format version — do not decode under wrong-schema assumptions.
    if (reader->header_.version != PackageHeader::VERSION) {
        return nullptr;
    }
    
    // Read TOC
    in.seekg(reader->header_.toc_offset);
    uint32_t toc_entries = reader->header_.toc_size / (sizeof(uint32_t) * 5);
    reader->toc_.reserve(toc_entries);
    
    for (uint32_t i = 0; i < toc_entries; ++i) {
        TocEntry entry;
        entry.type = static_cast<ChunkType>(read_le<uint32_t>(in));
        entry.offset = read_le<uint32_t>(in);
        entry.size = read_le<uint32_t>(in);
        entry.count = read_le<uint32_t>(in);
        entry.crc32 = read_le<uint32_t>(in);
        reader->toc_.push_back(entry);
    }
    
    // Build map index
    for (size_t i = 0; i < reader->toc_.size(); ++i) {
        if (reader->toc_[i].type == ChunkType::Maps) {
            // Read map IDs from chunk
            in.seekg(reader->toc_[i].offset);
            for (uint32_t j = 0; j < reader->toc_[i].count; ++j) {
                uint16_t id_len = read_le<uint16_t>(in);
                std::string id(id_len, '\0');
                in.read(id.data(), id_len);
                uint32_t data_size = read_le<uint32_t>(in);
                reader->map_index_[id] = j;
                (void)data_size;  // Used later for seeking
            }
        }
        // Build script index
        else if (reader->toc_[i].type == ChunkType::Scripts) {
            in.seekg(reader->toc_[i].offset);
            for (uint32_t j = 0; j < reader->toc_[i].count; ++j) {
                uint16_t id_len = read_le<uint16_t>(in);
                std::string id(id_len, '\0');
                in.read(id.data(), id_len);
                uint32_t data_size = read_le<uint32_t>(in);
                reader->script_index_[id] = j;
                (void)data_size;
            }
        }
        // Build sprite index
        else if (reader->toc_[i].type == ChunkType::Sprites) {
            in.seekg(reader->toc_[i].offset);
            for (uint32_t j = 0; j < reader->toc_[i].count; ++j) {
                uint16_t id_len = read_le<uint16_t>(in);
                std::string id(id_len, '\0');
                in.read(id.data(), id_len);
                uint32_t data_size = read_le<uint32_t>(in);
                reader->sprite_index_[id] = j;
                (void)data_size;
            }
        }
    }
    
    return reader;
}

bool PackageReader::validate() const {
    std::ifstream in(path_, std::ios::binary);
    if (!in) return false;
    
    // Validate each chunk's CRC
    for (const auto& entry : toc_) {
        std::vector<uint8_t> data(entry.size);
        in.seekg(entry.offset);
        in.read(reinterpret_cast<char*>(data.data()), entry.size);
        
        uint32_t actual_crc = calculate_crc32(data.data(), data.size());
        if (actual_crc != entry.crc32) {
            return false;
        }
    }
    
    return true;
}

std::vector<std::string> PackageReader::list_maps() const {
    std::vector<std::string> result;
    result.reserve(map_index_.size());
    for (const auto& [id, idx] : map_index_) {
        result.push_back(id);
    }
    return result;
}

std::optional<SerializedMap> PackageReader::load_map(const std::string& map_id) const {
    auto it = map_index_.find(map_id);
    if (it == map_index_.end()) {
        return std::nullopt;
    }
    
    // Find maps chunk
    const TocEntry* maps_chunk = nullptr;
    for (const auto& entry : toc_) {
        if (entry.type == ChunkType::Maps) {
            maps_chunk = &entry;
            break;
        }
    }
    if (!maps_chunk) return std::nullopt;
    
    std::ifstream in(path_, std::ios::binary);
    if (!in) return std::nullopt;
    
    // Navigate to map data
    in.seekg(maps_chunk->offset);
    
    // Skip index entries to find data offset
    uint32_t data_offset = 0;
    for (uint32_t i = 0; i <= it->second; ++i) {
        uint16_t id_len = read_le<uint16_t>(in);
        in.seekg(id_len, std::ios::cur);
        uint32_t data_size = read_le<uint32_t>(in);
        if (i < it->second) {
            data_offset += data_size;
        }
    }
    
    // Calculate where map data starts (after all index entries)
    in.seekg(maps_chunk->offset);
    uint32_t index_size = 0;
    for (uint32_t i = 0; i < maps_chunk->count; ++i) {
        uint16_t id_len = read_le<uint16_t>(in);
        index_size += 2 + id_len + 4;
        in.seekg(id_len + 4, std::ios::cur);
    }
    
    // Seek to target map data
    in.seekg(maps_chunk->offset + index_size + data_offset);
    
    // Re-read to get actual size
    in.seekg(maps_chunk->offset);
    uint32_t target_size = 0;
    for (uint32_t i = 0; i <= it->second; ++i) {
        uint16_t id_len = read_le<uint16_t>(in);
        in.seekg(id_len, std::ios::cur);
        target_size = read_le<uint32_t>(in);
    }
    
    in.seekg(maps_chunk->offset + index_size + data_offset);
    std::vector<uint8_t> data(target_size);
    in.read(reinterpret_cast<char*>(data.data()), target_size);
    
    // Deserialize
    ExtractedMap map = deserialize_map(data);
    
    // Convert to SerializedMap
    SerializedMap result;
    std::memset(&result, 0, sizeof(result));
    std::strncpy(result.map_id, map.map_id.c_str(), sizeof(result.map_id) - 1);
    std::strncpy(result.display_name, map.display_name.c_str(), sizeof(result.display_name) - 1);
    std::strncpy(result.tileset_id, map.tileset_id.c_str(), sizeof(result.tileset_id) - 1);
    std::strncpy(result.music_id, map.music_id.c_str(), sizeof(result.music_id) - 1);
    result.width = map.width;
    result.height = map.height;
    result.border_block = map.border_block;
    result.environment_type = map.environment_type;
    result.flags = 0;
    if (map.is_outdoor) result.flags |= 0x01;
    if (map.phone_service_disabled) result.flags |= 0x02;
    result.lighting = map.lighting;
    
    return result;
}

std::vector<std::string> PackageReader::list_tilesets() const {
    std::vector<std::string> result;
    for (const auto& [id, idx] : tileset_index_) {
        result.push_back(id);
    }
    return result;
}

std::optional<std::vector<uint8_t>> PackageReader::load_tileset_atlas(
    const std::string& tileset_id) const {
    // Find tilesets chunk
    const TocEntry* tileset_chunk = nullptr;
    for (const auto& entry : toc_) {
        if (entry.type == ChunkType::TilesetAtlases) {
            tileset_chunk = &entry;
            break;
        }
    }
    if (!tileset_chunk || tileset_chunk->count == 0) return std::nullopt;
    
    std::ifstream in(path_, std::ios::binary);
    if (!in) return std::nullopt;
    
    // Read index to find target tileset
    in.seekg(tileset_chunk->offset);
    
    std::vector<std::pair<std::string, uint32_t>> index;
    for (uint32_t i = 0; i < tileset_chunk->count; ++i) {
        uint16_t id_len = read_le<uint16_t>(in);
        std::string id(id_len, '\0');
        in.read(id.data(), id_len);
        uint32_t data_size = read_le<uint32_t>(in);
        index.push_back({id, data_size});
    }
    
    // Find target
    uint32_t data_offset = 0;
    uint32_t target_size = 0;
    bool found = false;
    
    for (const auto& [id, size] : index) {
        if (id == tileset_id) {
            target_size = size;
            found = true;
            break;
        }
        data_offset += size;
    }
    
    if (!found) return std::nullopt;
    
    // Calculate index size
    uint32_t index_size = 0;
    for (const auto& [id, size] : index) {
        index_size += 2 + static_cast<uint32_t>(id.size()) + 4;
    }
    
    // Read tileset data
    in.seekg(tileset_chunk->offset + index_size + data_offset);
    std::vector<uint8_t> data(target_size);
    in.read(reinterpret_cast<char*>(data.data()), target_size);
    
    if (!in.good()) return std::nullopt;
    
    return data;
}

std::optional<std::vector<uint8_t>> PackageReader::load_font_atlas(
    const std::string& font_id) const {
    // Find fonts chunk
    const TocEntry* font_chunk = nullptr;
    for (const auto& entry : toc_) {
        if (entry.type == ChunkType::Fonts) {
            font_chunk = &entry;
            break;
        }
    }
    if (!font_chunk || font_chunk->count == 0) return std::nullopt;
    
    std::ifstream in(path_, std::ios::binary);
    if (!in) return std::nullopt;
    
    // Read index to find target font
    in.seekg(font_chunk->offset);
    
    std::vector<std::pair<std::string, uint32_t>> index;
    for (uint32_t i = 0; i < font_chunk->count; ++i) {
        uint16_t id_len = read_le<uint16_t>(in);
        std::string id(id_len, '\0');
        in.read(id.data(), id_len);
        uint32_t data_size = read_le<uint32_t>(in);
        index.push_back({id, data_size});
    }
    
    // Find target
    uint32_t data_offset = 0;
    uint32_t target_size = 0;
    bool found = false;
    
    for (const auto& [id, size] : index) {
        if (id == font_id) {
            target_size = size;
            found = true;
            break;
        }
        data_offset += size;
    }
    
    if (!found) return std::nullopt;
    
    // Calculate index size
    uint32_t index_size = 0;
    for (const auto& [id, size] : index) {
        index_size += 2 + static_cast<uint32_t>(id.size()) + 4;
    }
    
    // Read font data
    in.seekg(font_chunk->offset + index_size + data_offset);
    std::vector<uint8_t> data(target_size);
    in.read(reinterpret_cast<char*>(data.data()), target_size);
    
    if (!in.good()) return std::nullopt;
    
    return data;
}

std::optional<std::string> PackageReader::load_script(const std::string& script_id) const {
    auto it = script_index_.find(script_id);
    if (it == script_index_.end()) {
        return std::nullopt;
    }
    
    // Find scripts chunk
    const TocEntry* script_chunk = nullptr;
    for (const auto& entry : toc_) {
        if (entry.type == ChunkType::Scripts) {
            script_chunk = &entry;
            break;
        }
    }
    if (!script_chunk || script_chunk->count == 0) return std::nullopt;
    
    std::ifstream in(path_, std::ios::binary);
    if (!in) return std::nullopt;
    
    // Read index to find target script
    in.seekg(script_chunk->offset);
    
    std::vector<std::pair<std::string, uint32_t>> index;
    for (uint32_t i = 0; i < script_chunk->count; ++i) {
        uint16_t id_len = read_le<uint16_t>(in);
        std::string id(id_len, '\0');
        in.read(id.data(), id_len);
        uint32_t data_size = read_le<uint32_t>(in);
        index.push_back({id, data_size});
    }
    
    // Find target
    uint32_t data_offset = 0;
    uint32_t target_size = 0;
    bool found = false;
    
    for (const auto& [id, size] : index) {
        if (id == script_id) {
            target_size = size;
            found = true;
            break;
        }
        data_offset += size;
    }
    
    if (!found) return std::nullopt;
    
    // Calculate index size
    uint32_t index_size = 0;
    for (const auto& [id, size] : index) {
        index_size += 2 + static_cast<uint32_t>(id.size()) + 4;
    }
    
    // Read script Lua code
    in.seekg(script_chunk->offset + index_size + data_offset);
    std::string lua_code(target_size, '\0');
    in.read(lua_code.data(), target_size);
    
    if (!in.good()) return std::nullopt;
    
    return lua_code;
}

std::vector<std::string> PackageReader::list_scripts() const {
    std::vector<std::string> result;
    result.reserve(script_index_.size());
    for (const auto& [id, idx] : script_index_) {
        result.push_back(id);
    }
    return result;
}

// Helper to convert crystal::BgEventType to enginemon::RuntimeBgEventType
// EXHAUSTIVE SWITCH - no silent fallback to Read
static enginemon::RuntimeBgEventType convert_bg_event_type(BgEventType type) {
    switch (type) {
        case BgEventType::Read:       return enginemon::RuntimeBgEventType::Read;
        case BgEventType::FacingUp:   return enginemon::RuntimeBgEventType::Up;
        case BgEventType::FacingDown: return enginemon::RuntimeBgEventType::Down;
        case BgEventType::FacingRight:return enginemon::RuntimeBgEventType::Right;
        case BgEventType::FacingLeft: return enginemon::RuntimeBgEventType::Left;
        case BgEventType::IfSet:      return enginemon::RuntimeBgEventType::IfSet;
        case BgEventType::IfNotSet:   return enginemon::RuntimeBgEventType::IfNotSet;
        case BgEventType::HiddenItem: return enginemon::RuntimeBgEventType::HiddenItem;
        case BgEventType::Copy:       return enginemon::RuntimeBgEventType::Copy;
    }
    // Unhandled enum value - hard fail package construction
    throw std::runtime_error("convert_bg_event_type: invalid BgEventType value " + 
                             std::to_string(static_cast<int>(type)));
}

// Helper to convert crystal::Direction to enginemon::ConnectionDirection
static enginemon::ConnectionDirection convert_direction(Direction dir) {
    switch (dir) {
        case Direction::North: return enginemon::ConnectionDirection::North;
        case Direction::South: return enginemon::ConnectionDirection::South;
        case Direction::East: return enginemon::ConnectionDirection::East;
        case Direction::West: return enginemon::ConnectionDirection::West;
    }
    return enginemon::ConnectionDirection::North;
}

std::optional<enginemon::RuntimeMap> PackageReader::load_full_map(const std::string& map_id) const {
    auto it = map_index_.find(map_id);
    if (it == map_index_.end()) {
        return std::nullopt;
    }
    
    // Find maps chunk
    const TocEntry* maps_chunk = nullptr;
    for (const auto& entry : toc_) {
        if (entry.type == ChunkType::Maps) {
            maps_chunk = &entry;
            break;
        }
    }
    if (!maps_chunk) return std::nullopt;
    
    std::ifstream in(path_, std::ios::binary);
    if (!in) return std::nullopt;
    
    // Navigate to map data
    in.seekg(maps_chunk->offset);
    
    // Skip index entries to find data offset
    uint32_t data_offset = 0;
    for (uint32_t i = 0; i <= it->second; ++i) {
        uint16_t id_len = read_le<uint16_t>(in);
        in.seekg(id_len, std::ios::cur);
        uint32_t data_size = read_le<uint32_t>(in);
        if (i < it->second) {
            data_offset += data_size;
        }
    }
    
    // Calculate where map data starts (after all index entries)
    in.seekg(maps_chunk->offset);
    uint32_t index_size = 0;
    for (uint32_t i = 0; i < maps_chunk->count; ++i) {
        uint16_t id_len = read_le<uint16_t>(in);
        index_size += 2 + id_len + 4;
        in.seekg(id_len + 4, std::ios::cur);
    }
    
    // Re-read to get actual size
    in.seekg(maps_chunk->offset);
    uint32_t target_size = 0;
    for (uint32_t i = 0; i <= it->second; ++i) {
        uint16_t id_len = read_le<uint16_t>(in);
        in.seekg(id_len, std::ios::cur);
        target_size = read_le<uint32_t>(in);
    }
    
    // Seek to target map data
    in.seekg(maps_chunk->offset + index_size + data_offset);
    std::vector<uint8_t> data(target_size);
    in.read(reinterpret_cast<char*>(data.data()), target_size);
    
    if (!in.good()) {
        return std::nullopt;
    }
    
    // Deserialize to ExtractedMap first
    ExtractedMap extracted = deserialize_map(data);
    
    // Convert to runtime-native RuntimeMap
    enginemon::RuntimeMap result;
    
    // Basic properties
    result.map_id = std::move(extracted.map_id);
    result.display_name = std::move(extracted.display_name);
    result.width = extracted.width;
    result.height = extracted.height;
    result.tileset_id = std::move(extracted.tileset_id);
    result.blocks = std::move(extracted.blocks);
    result.border_block = extracted.border_block;
    result.environment_type = extracted.environment_type;
    result.is_outdoor = extracted.is_outdoor;
    result.phone_service_disabled = extracted.phone_service_disabled;
    result.lighting = extracted.lighting;
    result.music_id = std::move(extracted.music_id);
    result.fish_group_id = std::move(extracted.fish_group_id);
    result.landmark_id = std::move(extracted.landmark_id);
    result.map_script_id = std::move(extracted.map_script_id);
    
    // Convert warps
    result.warps.reserve(extracted.warps.size());
    for (auto& w : extracted.warps) {
        enginemon::RuntimeWarp rw;
        rw.x = w.x;
        rw.y = w.y;
        rw.target_map_id = std::move(w.target_map_id);
        rw.target_warp_index = w.target_warp_index;
        result.warps.push_back(std::move(rw));
    }
    
    // Convert coord events
    result.coord_events.reserve(extracted.coord_events.size());
    for (auto& ce : extracted.coord_events) {
        enginemon::RuntimeCoordEvent rce;
        rce.x = ce.x;
        rce.y = ce.y;
        rce.script_id = std::move(ce.script_id);
        rce.scene_id = ce.scene_id;
        result.coord_events.push_back(std::move(rce));
    }
    
    // Convert BG events
    result.bg_events.reserve(extracted.bg_events.size());
    for (auto& bg : extracted.bg_events) {
        enginemon::RuntimeBgEvent rbg;
        rbg.x = bg.x;
        rbg.y = bg.y;
        rbg.type = convert_bg_event_type(bg.type);
        rbg.script_id = std::move(bg.script_id);
        rbg.item_id = std::move(bg.item_id);
        rbg.quantity = bg.quantity;
        rbg.condition_flag = std::move(bg.condition_flag);
        result.bg_events.push_back(std::move(rbg));
    }
    
    // Convert objects
    result.objects.reserve(extracted.objects.size());
    for (auto& obj : extracted.objects) {
        enginemon::RuntimeObject ro;
        ro.local_id = obj.local_id;
        ro.x = obj.x;
        ro.y = obj.y;
        ro.sprite_id = std::move(obj.sprite_id);
        ro.movement_type = obj.movement_type;
        ro.movement_radius_x = obj.movement_radius_x;
        ro.movement_radius_y = obj.movement_radius_y;
        ro.hour_start = obj.hour_start;
        ro.hour_end = obj.hour_end;
        ro.palette = obj.palette;
        ro.is_trainer = obj.is_trainer;
        ro.trainer_sight_range = obj.trainer_sight_range;
        ro.script_id = std::move(obj.script_id);
        ro.visibility_flag = std::move(obj.visibility_flag);
        result.objects.push_back(std::move(ro));
    }
    
    // Convert connections
    result.connections.reserve(extracted.connections.size());
    for (auto& conn : extracted.connections) {
        enginemon::RuntimeConnection rc;
        rc.direction = convert_direction(conn.direction);
        rc.target_map_id = std::move(conn.target_map_id);
        rc.src_skip_blocks    = conn.src_skip_blocks;
        rc.strip_length_blocks = conn.strip_length_blocks;
        rc.coord_adjust_tiles = conn.coord_adjust_tiles;
        result.connections.push_back(std::move(rc));
    }
    
    return result;
}

std::optional<enginemon::RuntimeSprite> PackageReader::load_sprite(const std::string& sprite_id) const {
    auto it = sprite_index_.find(sprite_id);
    if (it == sprite_index_.end()) {
        return std::nullopt;
    }
    
    // Find sprites chunk
    const TocEntry* sprite_chunk = nullptr;
    for (const auto& entry : toc_) {
        if (entry.type == ChunkType::Sprites) {
            sprite_chunk = &entry;
            break;
        }
    }
    if (!sprite_chunk || sprite_chunk->count == 0) return std::nullopt;
    
    std::ifstream in(path_, std::ios::binary);
    if (!in) return std::nullopt;
    
    // Read index to find target sprite
    in.seekg(sprite_chunk->offset);
    
    std::vector<std::pair<std::string, uint32_t>> index;
    for (uint32_t i = 0; i < sprite_chunk->count; ++i) {
        uint16_t id_len = read_le<uint16_t>(in);
        std::string id(id_len, '\0');
        in.read(id.data(), id_len);
        uint32_t data_size = read_le<uint32_t>(in);
        index.push_back({id, data_size});
    }
    
    // Find target
    uint32_t data_offset = 0;
    uint32_t target_size = 0;
    bool found = false;
    
    for (const auto& [id, size] : index) {
        if (id == sprite_id) {
            target_size = size;
            found = true;
            break;
        }
        data_offset += size;
    }
    
    if (!found) return std::nullopt;
    
    // Calculate index size
    uint32_t index_size = 0;
    for (const auto& [id, size] : index) {
        index_size += 2 + static_cast<uint32_t>(id.size()) + 4;
    }
    
    // Read sprite data
    in.seekg(sprite_chunk->offset + index_size + data_offset);
    std::vector<uint8_t> data(target_size);
    in.read(reinterpret_cast<char*>(data.data()), target_size);
    
    if (!in.good()) return std::nullopt;
    
    // Deserialize sprite
    std::istringstream sin(std::string(data.begin(), data.end()), std::ios::binary);
    
    enginemon::RuntimeSprite sprite;
    
    // Read sprite_id (redundant but consistent with format)
    uint16_t id_len = read_le<uint16_t>(sin);
    sprite.sprite_id.resize(id_len);
    sin.read(sprite.sprite_id.data(), id_len);
    
    // Read type and palette
    sprite.type = static_cast<enginemon::SpriteType>(sin.get());
    sprite.default_palette = static_cast<enginemon::SpritePalette>(sin.get());
    
    // Read frames
    uint32_t frame_count = read_le<uint32_t>(sin);
    sprite.frames.resize(frame_count);
    for (uint32_t i = 0; i < frame_count; ++i) {
        sin.read(reinterpret_cast<char*>(sprite.frames[i].pixels.data()), 256);
    }

    // Read icon frames (SpriteType::Icon only)
    uint32_t icon_frame_count = read_le<uint32_t>(sin);
    if (icon_frame_count > 16) return std::nullopt;  // sanity bound
    sprite.icon_frames.resize(icon_frame_count);
    for (uint32_t i = 0; i < icon_frame_count; ++i) {
        sin.read(reinterpret_cast<char*>(sprite.icon_frames[i].pixels.data()), 1024);
    }
    
    return sprite;
}

std::vector<std::string> PackageReader::list_sprites() const {
    std::vector<std::string> result;
    result.reserve(sprite_index_.size());
    for (const auto& [id, idx] : sprite_index_) {
        result.push_back(id);
    }
    return result;
}

std::optional<enginemon::SpriteObjPalettes> PackageReader::load_obj_palettes() const {
    // Find OBJ palettes chunk
    const TocEntry* pal_chunk = nullptr;
    for (const auto& entry : toc_) {
        if (entry.type == ChunkType::ObjPalettes) {
            pal_chunk = &entry;
            break;
        }
    }
    if (!pal_chunk || pal_chunk->size == 0) return std::nullopt;
    
    std::ifstream in(path_, std::ios::binary);
    if (!in) return std::nullopt;
    
    // Read palette data
    in.seekg(pal_chunk->offset);
    std::vector<uint8_t> data(pal_chunk->size);
    in.read(reinterpret_cast<char*>(data.data()), pal_chunk->size);
    
    if (!in.good()) return std::nullopt;
    
    // Deserialize palettes
    std::istringstream sin(std::string(data.begin(), data.end()), std::ios::binary);
    
    enginemon::SpriteObjPalettes palettes;
    
    // 4 time-of-day variants × 8 palettes × 4 colors × 2 bytes (RGB555)
    for (int tod = 0; tod < 4; ++tod) {
        for (int pal = 0; pal < 8; ++pal) {
            for (int c = 0; c < 4; ++c) {
                palettes.time_palettes[tod][pal].colors_gbc[c] = read_le<uint16_t>(sin);
            }
        }
    }
    
    return palettes;
}

} // namespace crystal
