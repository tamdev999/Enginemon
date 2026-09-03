// engine/package/package_reader.cpp
// Runtime package reader implementation
//
// Deserializes EMON package data directly into runtime-native types.
// No frontend types (ExtractedMap, etc.) are used - this is the clean boundary.

#include "engine/package/package_reader.hpp"
#include <fstream>
#include <sstream>
#include <cstring>
#include <array>
#include <iostream>
#include <format>
#include <stdexcept>

namespace enginemon {

//=============================================================================
// CRC32 IMPLEMENTATION
//=============================================================================

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
// SERIALIZATION HELPERS - with bounds validation (Audit 4)
//=============================================================================

// Maximum reasonable limits for package data (defense-in-depth, not primary safety)
namespace PackageLimits {
    constexpr uint32_t MAX_STRING_LENGTH = 64 * 1024;      // 64KB per string
    constexpr uint32_t MAX_ARRAY_COUNT = 1024 * 1024;      // 1M elements max
    constexpr uint32_t MAX_BLOCK_COUNT = 4 * 1024 * 1024;  // 4M blocks max
    constexpr uint32_t MAX_CHUNK_SIZE = 256 * 1024 * 1024; // 256MB per chunk
    constexpr uint32_t MAX_TOC_ENTRIES = 65536;            // 64K TOC entries max
}

// Bounds-checked read helper - validates remaining bytes BEFORE reading
class BoundsReader {
public:
    BoundsReader(std::istream& in, size_t total_size)
        : in_(in), total_size_(total_size), current_pos_(0) {}
    
    bool has_bytes(size_t count) const {
        return current_pos_ + count <= total_size_;
    }
    
    template<typename T>
    bool read_le(T& out) {
        if (!has_bytes(sizeof(T))) return false;
        out = 0;
        for (size_t i = 0; i < sizeof(T); ++i) {
            int byte = in_.get();
            if (byte == EOF) return false;
            out |= static_cast<T>(static_cast<uint8_t>(byte)) << (i * 8);
        }
        current_pos_ += sizeof(T);
        return true;
    }
    
    bool read_bytes(void* dst, size_t count) {
        if (!has_bytes(count)) return false;
        in_.read(static_cast<char*>(dst), count);
        if (!in_.good() && !in_.eof()) return false;
        current_pos_ += count;
        return true;
    }
    
    bool skip(size_t count) {
        if (!has_bytes(count)) return false;
        in_.seekg(count, std::ios::cur);
        current_pos_ += count;
        return in_.good();
    }
    
    size_t remaining() const { return total_size_ - current_pos_; }
    
private:
    std::istream& in_;
    size_t total_size_;
    size_t current_pos_;
};

template<typename T>
static T read_le(std::istream& in) {
    T value = 0;
    for (size_t i = 0; i < sizeof(T); ++i) {
        int byte = in.get();
        if (byte == EOF) return value;  // Partial read on truncated data
        value |= static_cast<T>(static_cast<uint8_t>(byte)) << (i * 8);
    }
    return value;
}

static std::string read_fixed_string(std::istream& in, size_t size) {
    std::string str(size, '\0');
    in.read(str.data(), size);
    if (!in.good() && !in.eof()) {
        return "";  // Read error
    }
    auto null_pos = str.find('\0');
    if (null_pos != std::string::npos) {
        str.resize(null_pos);
    }
    return str;
}

// Read a length-prefixed string with bounds validation
static bool read_length_string(std::istream& in, std::string& out) {
    uint16_t len = read_le<uint16_t>(in);
    if (!in.good()) return false;
    
    // Bounds check: reject unreasonably large strings
    if (len > PackageLimits::MAX_STRING_LENGTH) {
        return false;
    }
    
    out.resize(len);
    if (len > 0) {
        in.read(out.data(), len);
        if (!in.good() && !in.eof()) return false;
    }
    return true;
}

//=============================================================================
// MAP DESERIALIZATION - DIRECTLY TO RUNTIME TYPES
//=============================================================================

static RuntimeWarp read_warp(std::istream& in) {
    RuntimeWarp warp;
    warp.x = in.get();
    warp.y = in.get();
    warp.target_warp_index = in.get();
    if (!read_length_string(in, warp.target_map_id)) {
        throw std::runtime_error(
            "read_warp: truncated target_map_id string — malformed package payload");
    }
    return warp;
}

static RuntimeCoordEvent read_coord_event(std::istream& in) {
    RuntimeCoordEvent evt;
    evt.x = in.get();
    evt.y = in.get();
    evt.scene_id = read_le<uint16_t>(in);
    if (!read_length_string(in, evt.script_id)) {
        throw std::runtime_error(
            "read_coord_event: truncated script_id string — malformed package payload");
    }
    return evt;
}

static RuntimeBgEvent read_bg_event(std::istream& in) {
    RuntimeBgEvent evt;
    evt.x = in.get();
    evt.y = in.get();

    // Read the raw type byte and validate domain before cast.
    // RuntimeBgEventType is defined 0–8; bytes outside this range are
    // a structural package error — throw rather than producing a garbage enum.
    uint8_t raw_type = in.get();
    constexpr uint8_t BGEVENT_MAX_VALID = 8;  // Copy (= RuntimeBgEventType::Copy)
    if (raw_type > BGEVENT_MAX_VALID) {
        throw std::runtime_error(
            std::format("read_bg_event: invalid BgEventType byte {} (max {})"
                        " — malformed or wrong-schema package",
                        static_cast<int>(raw_type), static_cast<int>(BGEVENT_MAX_VALID)));
    }
    evt.type = static_cast<RuntimeBgEventType>(raw_type);

    evt.quantity = in.get();
    if (!read_length_string(in, evt.script_id)) {
        throw std::runtime_error(
            "read_bg_event: truncated script_id string — malformed package payload");
    }
    if (!read_length_string(in, evt.item_id)) {
        throw std::runtime_error(
            "read_bg_event: truncated item_id string — malformed package payload");
    }
    if (!read_length_string(in, evt.condition_flag)) {
        throw std::runtime_error(
            "read_bg_event: truncated condition_flag string — malformed package payload");
    }
    return evt;
}

static RuntimeObject read_object(std::istream& in) {
    RuntimeObject obj;
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

    if (!read_length_string(in, obj.sprite_id)) {
        throw std::runtime_error(
            "read_object: truncated sprite_id string — malformed package payload");
    }
    if (!read_length_string(in, obj.script_id)) {
        throw std::runtime_error(
            "read_object: truncated script_id string — malformed package payload");
    }
    if (!read_length_string(in, obj.visibility_flag)) {
        throw std::runtime_error(
            "read_object: truncated visibility_flag string — malformed package payload");
    }

    return obj;
}

static RuntimeConnection read_connection(std::istream& in) {
    RuntimeConnection conn;
    
    // Read raw direction byte and convert to runtime type.
    // An unrecognized direction byte is a structural package error — throw
    // rather than silently defaulting to North.
    uint8_t raw_dir = in.get();
    switch (raw_dir) {
        case 0: conn.direction = ConnectionDirection::North; break;
        case 1: conn.direction = ConnectionDirection::South; break;
        case 2: conn.direction = ConnectionDirection::East; break;
        case 3: conn.direction = ConnectionDirection::West; break;
        default:
            throw std::runtime_error(
                std::format("read_connection: invalid direction byte {} "
                            "— malformed or wrong-schema package",
                            static_cast<int>(raw_dir)));
    }
    
    conn.src_skip_blocks    = read_le<int32_t>(in);
    conn.strip_length_blocks = in.get();
    conn.coord_adjust_tiles = read_le<int32_t>(in);
    if (!read_length_string(in, conn.target_map_id)) {
        throw std::runtime_error(
            "read_connection: truncated target_map_id string — malformed package payload");
    }
    return conn;
}

// Read counted array with hard failure on structural anomalies.
// - count > MAX_ARRAY_COUNT: structural corruption → throw (not empty return)
// - stream failure mid-array: truncated package → throw (not partial prefix)
template<typename T>
static std::vector<T> read_counted_array(std::istream& in, T (*read_item)(std::istream&)) {
    uint32_t count = read_le<uint32_t>(in);
    
    if (count > PackageLimits::MAX_ARRAY_COUNT) {
        throw std::runtime_error(
            std::format("read_counted_array: declared count {} exceeds MAX_ARRAY_COUNT {} "
                        "— malformed or wrong-schema package",
                        count, PackageLimits::MAX_ARRAY_COUNT));
    }
    
    std::vector<T> arr;
    arr.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        if (!in.good()) {
            throw std::runtime_error(
                std::format("read_counted_array: stream failure after reading {}/{} items "
                            "— truncated package payload",
                            i, count));
        }
        arr.push_back(read_item(in));
    }
    return arr;
}

// Deserialize map data directly to RuntimeMap.
// Returns nullopt on any structural failure — callers can distinguish a
// malformed payload (nullopt) from a valid but empty/default resource.
static std::optional<RuntimeMap> deserialize_map(const std::vector<uint8_t>& data) {
    std::istringstream in(std::string(data.begin(), data.end()), std::ios::binary);
    
    RuntimeMap map;
    
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
    
    if (!in.good()) return std::nullopt;  // Fixed-header truncated
    
    // Derive semantic Environment from raw Crystal byte
    map.environment = environment_from_crystal(map.environment_type);
    
    uint8_t flags = in.get();
    map.is_outdoor = (flags & 0x01) != 0;
    map.phone_service_disabled = (flags & 0x02) != 0;
    
    map.lighting = in.get();
    
    // Derive time_policy from lighting byte
    if (map.lighting <= 4) {
        map.time_policy = static_cast<PalettePolicy>(map.lighting);
    } else {
        map.time_policy = PalettePolicy::Auto;
    }
    
    in.get();  // padding
    in.get();  // padding
    
    if (!in.good()) return std::nullopt;
    
    // Block data — oversized block_count is a structural error, not a silent skip
    uint32_t block_count = read_le<uint32_t>(in);
    if (block_count > PackageLimits::MAX_BLOCK_COUNT) {
        return std::nullopt;  // Malformed: block count exceeds physical limit
    }
    map.blocks.resize(block_count);
    in.read(reinterpret_cast<char*>(map.blocks.data()), block_count);
    if (!in.good() && !in.eof()) return std::nullopt;  // Truncated block data
    
    // Events — read_counted_array and read_connection both throw on structural failure;
    // catch and convert to nullopt so the whole map decode is a clean failure.
    try {
        map.warps = read_counted_array(in, read_warp);
        map.coord_events = read_counted_array(in, read_coord_event);
        map.bg_events = read_counted_array(in, read_bg_event);
        map.objects = read_counted_array(in, read_object);
        map.connections = read_counted_array(in, read_connection);
    } catch (const std::exception& e) {
        return std::nullopt;  // Structural failure in event arrays
    }
    
    return map;
}

//=============================================================================
// PACKAGE READER IMPLEMENTATION
//=============================================================================

std::unique_ptr<PackageReader> PackageReader::open(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return nullptr;
    
    // Get file size for bounds validation
    in.seekg(0, std::ios::end);
    size_t file_size = static_cast<size_t>(in.tellg());
    in.seekg(0, std::ios::beg);
    
    // Validate minimum header size
    if (file_size < sizeof(PackageHeader)) {
        return nullptr;  // Truncated: header doesn't fit
    }
    
    auto reader = std::unique_ptr<PackageReader>(new PackageReader());
    reader->path_ = path;
    reader->file_size_ = file_size;
    
    // Read header
    in.read(reinterpret_cast<char*>(&reader->header_), sizeof(PackageHeader));
    
    // Validate magic
    if (reader->header_.magic != PackageHeader::MAGIC) {
        return nullptr;
    }
    
    // Validate format version before interpreting any version-dependent payloads.
    // An older or newer version must fail explicitly rather than silently
    // decoding fields under wrong-schema assumptions.
    if (reader->header_.version != PackageHeader::VERSION) {
        std::cerr << "PackageReader: incompatible format version "
                  << reader->header_.version
                  << " (expected " << PackageHeader::VERSION << ")\n";
        return nullptr;
    }
    
    // Validate TOC bounds: offset + size must be within file
    if (reader->header_.toc_offset > file_size ||
        reader->header_.toc_size > file_size ||
        reader->header_.toc_offset + reader->header_.toc_size > file_size) {
        return nullptr;  // TOC extends beyond file
    }
    
    // Read TOC
    in.seekg(reader->header_.toc_offset);
    uint32_t toc_entries = reader->header_.toc_size / (sizeof(uint32_t) * 5);
    
    // Validate TOC entry count
    if (toc_entries > PackageLimits::MAX_TOC_ENTRIES) {
        return nullptr;  // Unreasonable TOC size
    }
    
    reader->toc_.reserve(toc_entries);
    
    for (uint32_t i = 0; i < toc_entries; ++i) {
        TocEntry entry;
        entry.type = static_cast<ChunkType>(read_le<uint32_t>(in));
        entry.offset = read_le<uint32_t>(in);
        entry.size = read_le<uint32_t>(in);
        entry.count = read_le<uint32_t>(in);
        entry.crc32 = read_le<uint32_t>(in);
        
        // Validate each chunk's bounds BEFORE using
        if (entry.offset > file_size ||
            entry.size > file_size ||
            entry.offset + entry.size > file_size) {
            return nullptr;  // Chunk extends beyond file
        }
        
        // Validate chunk size against limits
        if (entry.size > PackageLimits::MAX_CHUNK_SIZE) {
            return nullptr;  // Chunk too large
        }
        
        reader->toc_.push_back(entry);
    }
    
    // Build indices for all chunk types
    for (size_t i = 0; i < reader->toc_.size(); ++i) {
        const auto& entry = reader->toc_[i];
        in.seekg(entry.offset);
        
        std::unordered_map<std::string, size_t>* target_index = nullptr;
        
        switch (entry.type) {
            case ChunkType::Maps:
                target_index = &reader->map_index_;
                break;
            case ChunkType::TilesetAtlases:
                target_index = &reader->tileset_index_;
                break;
            case ChunkType::Scripts:
                target_index = &reader->script_index_;
                break;
            case ChunkType::Sprites:
                target_index = &reader->sprite_index_;
                break;
            case ChunkType::Fonts:
                target_index = &reader->font_index_;
                break;
            default:
                continue;
        }
        
        // Validate count against limits
        if (entry.count > PackageLimits::MAX_ARRAY_COUNT) {
            return nullptr;  // Too many entries in chunk
        }
        
        // Track consumed bytes within chunk for bounds checking
        size_t consumed = 0;
        
        for (uint32_t j = 0; j < entry.count; ++j) {
            // Check we have room for length prefix
            if (consumed + 2 > entry.size) {
                return nullptr;  // Truncated index entry
            }
            
            uint16_t id_len = read_le<uint16_t>(in);
            consumed += 2;
            
            // Validate string length
            if (id_len > PackageLimits::MAX_STRING_LENGTH ||
                consumed + id_len > entry.size) {
                return nullptr;  // String extends beyond chunk
            }
            
            std::string id(id_len, '\0');
            in.read(id.data(), id_len);
            consumed += id_len;
            
            // Check we have room for data_size
            if (consumed + 4 > entry.size) {
                return nullptr;  // Truncated index entry
            }
            
            uint32_t data_size = read_le<uint32_t>(in);
            consumed += 4;
            
            // Don't need to validate data_size fits here since we're just reading index
            // The actual data read will validate against remaining chunk size
            
            // Reject duplicate IDs: a duplicate in the index means the writer
            // emitted an invalid package.  Reject rather than silently accepting
            // last-wins (which would desync the index from the sequential data scan).
            if (target_index->contains(id)) {
                return nullptr;  // Duplicate ID in chunk index — malformed package
            }
            (*target_index)[id] = j;
            (void)data_size;
        }
    }
    
    return reader;
}

bool PackageReader::validate() const {
    std::ifstream in(path_, std::ios::binary);
    if (!in) return false;
    
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

std::vector<std::string> PackageReader::list_tilesets() const {
    std::vector<std::string> result;
    for (const auto& [id, idx] : tileset_index_) {
        result.push_back(id);
    }
    return result;
}

std::vector<std::string> PackageReader::list_scripts() const {
    std::vector<std::string> result;
    result.reserve(script_index_.size());
    for (const auto& [id, idx] : script_index_) {
        result.push_back(id);
    }
    return result;
}

std::vector<std::string> PackageReader::list_sprites() const {
    std::vector<std::string> result;
    result.reserve(sprite_index_.size());
    for (const auto& [id, idx] : sprite_index_) {
        result.push_back(id);
    }
    return result;
}

// Generic helper to read indexed chunk data
std::optional<std::vector<uint8_t>> PackageReader::read_indexed_chunk(
    ChunkType type,
    const std::string& target_id,
    const std::unordered_map<std::string, size_t>& index) const {
    
    auto it = index.find(target_id);
    if (it == index.end()) {
        return std::nullopt;
    }
    
    // Find the chunk
    const TocEntry* chunk = nullptr;
    for (const auto& entry : toc_) {
        if (entry.type == type) {
            chunk = &entry;
            break;
        }
    }
    if (!chunk || chunk->count == 0) return std::nullopt;
    
    std::ifstream in(path_, std::ios::binary);
    if (!in) return std::nullopt;
    
    // Read index to find offsets
    in.seekg(chunk->offset);
    
    std::vector<std::pair<std::string, uint32_t>> chunk_index;
    for (uint32_t i = 0; i < chunk->count; ++i) {
        uint16_t id_len = read_le<uint16_t>(in);
        std::string id(id_len, '\0');
        in.read(id.data(), id_len);
        uint32_t data_size = read_le<uint32_t>(in);
        chunk_index.push_back({id, data_size});
    }
    
    // Find target
    uint32_t data_offset = 0;
    uint32_t target_size = 0;
    bool found = false;
    
    for (const auto& [id, size] : chunk_index) {
        if (id == target_id) {
            target_size = size;
            found = true;
            break;
        }
        data_offset += size;
    }
    
    if (!found) return std::nullopt;
    
    // Calculate index size
    uint32_t index_size = 0;
    for (const auto& [id, size] : chunk_index) {
        index_size += 2 + static_cast<uint32_t>(id.size()) + 4;
    }
    
    // Read data
    in.seekg(chunk->offset + index_size + data_offset);
    std::vector<uint8_t> data(target_size);
    in.read(reinterpret_cast<char*>(data.data()), target_size);
    
    if (!in.good()) return std::nullopt;
    
    return data;
}

std::optional<RuntimeMap> PackageReader::load_map(const std::string& map_id) const {
    auto data = read_indexed_chunk(ChunkType::Maps, map_id, map_index_);
    if (!data) return std::nullopt;
    
    // deserialize_map returns nullopt on any structural failure —
    // a malformed payload is distinguishable from "map not found" (data = nullopt).
    return deserialize_map(*data);
}

std::optional<std::vector<uint8_t>> PackageReader::load_tileset_data(
    const std::string& tileset_id) const {
    return read_indexed_chunk(ChunkType::TilesetAtlases, tileset_id, tileset_index_);
}

std::optional<std::vector<uint8_t>> PackageReader::load_font_atlas(
    const std::string& font_id) const {
    return read_indexed_chunk(ChunkType::Fonts, font_id, font_index_);
}

std::optional<std::string> PackageReader::load_script(const std::string& script_id) const {
    auto data = read_indexed_chunk(ChunkType::Scripts, script_id, script_index_);
    if (!data) return std::nullopt;
    
    return std::string(data->begin(), data->end());
}

std::optional<RuntimeSprite> PackageReader::load_sprite(const std::string& sprite_id) const {
    auto data = read_indexed_chunk(ChunkType::Sprites, sprite_id, sprite_index_);
    if (!data) return std::nullopt;
    
    // Deserialize sprite with stream-health checks at each structural read.
    // Returns nullopt on any truncation rather than a partially-built sprite.
    std::istringstream in(std::string(data->begin(), data->end()), std::ios::binary);
    
    RuntimeSprite sprite;
    
    // Read sprite_id (length-prefixed)
    uint16_t id_len = read_le<uint16_t>(in);
    if (!in.good()) return std::nullopt;
    sprite.sprite_id.resize(id_len);
    in.read(sprite.sprite_id.data(), id_len);
    if (!in.good() && !in.eof()) return std::nullopt;
    
    // Read type and palette
    int type_byte = in.get();
    int palette_byte = in.get();
    if (!in.good() && !in.eof()) return std::nullopt;
    sprite.type = static_cast<SpriteType>(type_byte);
    sprite.default_palette = static_cast<SpritePalette>(palette_byte);
    
    // Read frames
    uint32_t frame_count = read_le<uint32_t>(in);
    if (!in.good()) return std::nullopt;
    sprite.frames.resize(frame_count);
    for (uint32_t i = 0; i < frame_count; ++i) {
        in.read(reinterpret_cast<char*>(sprite.frames[i].pixels.data()), 256);
        if (!in.good() && !in.eof()) return std::nullopt;
    }
    
    return sprite;
}

std::optional<SpriteObjPalettes> PackageReader::load_obj_palettes() const {
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
    
    SpriteObjPalettes palettes;
    
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

std::unordered_map<SpeciesId, std::string>
PackageReader::load_species_icon_map() const {
    std::unordered_map<SpeciesId, std::string> result;

    // Find the SpeciesIconMap chunk
    const TocEntry* chunk = nullptr;
    for (const auto& entry : toc_) {
        if (entry.type == ChunkType::SpeciesIconMap) {
            chunk = &entry;
            break;
        }
    }
    if (!chunk || chunk->size == 0) return result;  // absent = empty map (old package)

    std::ifstream in(path_, std::ios::binary);
    if (!in) return result;
    in.seekg(chunk->offset);

    // Format: uint32_t count, then [uint16_t species, uint16_t name_len, name_bytes]
    uint32_t count = read_le<uint32_t>(in);
    if (!in.good() || count > 512) return result;

    for (uint32_t i = 0; i < count; ++i) {
        uint16_t species = read_le<uint16_t>(in);
        uint16_t name_len = read_le<uint16_t>(in);
        if (!in.good() || name_len > 256) break;
        std::string icon_id(name_len, '\0');
        in.read(icon_id.data(), name_len);
        if (!in.good() && !in.eof()) break;
        result[static_cast<SpeciesId>(species)] = std::move(icon_id);
    }

    return result;
}

//=============================================================================
// REGISTRY LOADING — BaseStats and MoveData chunks
//
// Both loaders are fail-closed: any structural read failure or duplicate ID
// returns nullopt rather than a partially-populated registry.  Callers must
// treat nullopt as a hard failure (chunk absent or corrupt).
//=============================================================================

std::optional<Registry<SpeciesId, SpeciesData>>
PackageReader::load_base_stats_registry() const {
    // Locate the BaseStats chunk in the TOC.
    const TocEntry* chunk = nullptr;
    for (const auto& entry : toc_) {
        if (entry.type == ChunkType::BaseStats) {
            chunk = &entry;
            break;
        }
    }
    if (!chunk || chunk->size < 4) return std::nullopt;  // absent or too small

    std::ifstream in(path_, std::ios::binary);
    if (!in) return std::nullopt;
    in.seekg(chunk->offset);

    // Wire format: u32 count, then per-entry (14 bytes each):
    //   u16 species_id, u8 hp, atk, def, spd, satk, sdef, type1, type2,
    //   catch_rate, base_exp, gender_ratio, u8 reserved
    uint32_t count = read_le<uint32_t>(in);
    if (!in.good()) return std::nullopt;

    constexpr uint32_t ENTRY_SIZE = 14;
    // Validate that the declared count fits within the chunk.
    if (static_cast<uint64_t>(count) * ENTRY_SIZE + 4 > chunk->size) {
        return std::nullopt;  // count/size mismatch — corrupt chunk
    }
    // Reject implausibly large counts.
    if (count > 65535u) return std::nullopt;

    Registry<SpeciesId, SpeciesData> reg;
    for (uint32_t i = 0; i < count; ++i) {
        uint16_t species_id_raw = read_le<uint16_t>(in);
        if (!in.good()) return std::nullopt;

        uint8_t hp    = static_cast<uint8_t>(in.get());
        uint8_t atk   = static_cast<uint8_t>(in.get());
        uint8_t def   = static_cast<uint8_t>(in.get());
        uint8_t spd   = static_cast<uint8_t>(in.get());
        uint8_t satk  = static_cast<uint8_t>(in.get());
        uint8_t sdef  = static_cast<uint8_t>(in.get());
        uint8_t type1 = static_cast<uint8_t>(in.get());
        uint8_t type2 = static_cast<uint8_t>(in.get());
        uint8_t catch_rate   = static_cast<uint8_t>(in.get());
        uint8_t base_exp     = static_cast<uint8_t>(in.get());
        uint8_t gender_ratio = static_cast<uint8_t>(in.get());
        in.get();  // reserved
        if (!in.good() && !in.eof()) return std::nullopt;

        SpeciesId sid = static_cast<SpeciesId>(species_id_raw);

        // Reject duplicate SpeciesIds — a duplicate means the package is malformed.
        if (reg.get(sid) != nullptr) {
            return std::nullopt;  // duplicate id — corrupt chunk
        }

        SpeciesData sd;
        sd.id   = sid;
        sd.base_stats.hp             = hp;
        sd.base_stats.attack         = atk;
        sd.base_stats.defense        = def;
        sd.base_stats.speed          = spd;
        sd.base_stats.special_attack  = satk;
        sd.base_stats.special_defense = sdef;
        sd.type1        = static_cast<TypeId>(type1);
        sd.type2        = static_cast<TypeId>(type2);
        sd.catch_rate   = catch_rate;
        sd.base_exp     = base_exp;
        sd.gender_ratio = gender_ratio;

        reg.register_entry(sid, std::move(sd));
    }

    reg.freeze();
    return reg;
}

std::optional<Registry<MoveId, MoveData>>
PackageReader::load_move_registry() const {
    // Locate the MoveData chunk in the TOC.
    const TocEntry* chunk = nullptr;
    for (const auto& entry : toc_) {
        if (entry.type == ChunkType::MoveData) {
            chunk = &entry;
            break;
        }
    }
    if (!chunk || chunk->size < 4) return std::nullopt;

    std::ifstream in(path_, std::ios::binary);
    if (!in) return std::nullopt;
    in.seekg(chunk->offset);

    // Wire format: u32 count, then per-entry (9 bytes each):
    //   u16 move_id, u8 type_id, power, accuracy, pp, effect_id, effect_chance, reserved
    uint32_t count = read_le<uint32_t>(in);
    if (!in.good()) return std::nullopt;

    constexpr uint32_t ENTRY_SIZE = 9;
    if (static_cast<uint64_t>(count) * ENTRY_SIZE + 4 > chunk->size) {
        return std::nullopt;
    }
    if (count > 65535u) return std::nullopt;

    Registry<MoveId, MoveData> reg;
    for (uint32_t i = 0; i < count; ++i) {
        uint16_t move_id_raw    = read_le<uint16_t>(in);
        if (!in.good()) return std::nullopt;

        uint8_t type_id       = static_cast<uint8_t>(in.get());
        uint8_t power         = static_cast<uint8_t>(in.get());
        uint8_t accuracy      = static_cast<uint8_t>(in.get());
        uint8_t pp            = static_cast<uint8_t>(in.get());
        uint8_t effect_id     = static_cast<uint8_t>(in.get());
        uint8_t effect_chance = static_cast<uint8_t>(in.get());
        in.get();  // reserved
        if (!in.good() && !in.eof()) return std::nullopt;

        MoveId mid = static_cast<MoveId>(move_id_raw);

        if (reg.get(mid) != nullptr) {
            return std::nullopt;  // duplicate move id — corrupt chunk
        }

        MoveData md;
        md.id             = mid;
        md.type           = static_cast<TypeId>(type_id);
        md.power          = power;
        md.accuracy       = accuracy;
        md.pp             = pp;
        md.effect_id      = effect_id;
        md.effect_chance  = effect_chance;
        // Fields not in the package chunk are left at their zero-initialised defaults:
        // category, target, priority, makes_contact, is_sound_based, animation_id, name.

        reg.register_entry(mid, std::move(md));
    }

    reg.freeze();
    return reg;
}

std::optional<BattleRules>
PackageReader::load_battle_rules() const {
    // Locate the BattleRules chunk in the TOC.
    const TocEntry* chunk = nullptr;
    for (const auto& entry : toc_) {
        if (entry.type == ChunkType::BattleRules) {
            chunk = &entry;
            break;
        }
    }
    if (!chunk || chunk->size < 64) return std::nullopt;  // 64 = minimum sane size

    std::ifstream in(path_, std::ios::binary);
    if (!in) return std::nullopt;
    in.seekg(chunk->offset);
    if (!in) return std::nullopt;

    BoundsReader r(in, chunk->size);
    BattleRules rules;

    // -----------------------------------------------------------------------
    // Helper lambdas — all return false on read failure
    // -----------------------------------------------------------------------
    auto read_u8 = [&](uint8_t& out) { return r.read_le(out); };

    auto read_stage_mult = [&](StageMult13& tbl) -> bool {
        for (auto& e : tbl) {
            if (!r.read_le(e.numerator))   return false;
            if (!r.read_le(e.denominator)) return false;
            if (e.denominator == 0)        return false;  // would div-by-zero
        }
        return true;
    };

    auto read_byte_list = [&](std::vector<uint8_t>& out) -> bool {
        uint8_t count = 0;
        if (!r.read_le(count)) return false;
        out.resize(count);
        for (uint8_t i = 0; i < count; ++i)
            if (!r.read_le(out[i])) return false;
        return true;
    };

    // Read a u16 list from wire and store each entry as MoveId.
    // high_crit_moves wire format: u16 LE count + count × u16 LE entries.
    // Supports the full MoveId range (0..65535) including semantic IDs > 255.
    auto read_u16_list_as_move_ids = [&](std::vector<enginemon::MoveId>& out) -> bool {
        uint16_t count = 0;
        if (!r.read_le(count)) return false;
        out.resize(count);
        for (uint16_t i = 0; i < count; ++i) {
            uint16_t v = 0;
            if (!r.read_le(v)) return false;
            out[i] = static_cast<enginemon::MoveId>(v);
        }
        return true;
    };

    // -----------------------------------------------------------------------
    // Deserialise (must match wire format in PackageWriter::add_battle_rules)
    // -----------------------------------------------------------------------

    // stat_stage_mult: 13 × {num, den}
    if (!read_stage_mult(rules.stat_stage_mult)) return std::nullopt;

    // acc_stage_mult: 13 × {num, den}
    if (!read_stage_mult(rules.acc_stage_mult)) return std::nullopt;

    // crit_chances: 7 × u8
    for (uint8_t& c : rules.crit_chances)
        if (!r.read_le(c)) return std::nullopt;
    if (rules.crit_chances[0] == 0) return std::nullopt;  // stage-0 crit of 0 → corrupt

    // wobble_probabilities: u8 count + count × {thr, wob}
    {
        uint8_t n = 0;
        if (!r.read_le(n)) return std::nullopt;
        rules.wobble_probabilities.resize(n);
        for (uint8_t i = 0; i < n; ++i) {
            if (!r.read_le(rules.wobble_probabilities[i][0])) return std::nullopt;
            if (!r.read_le(rules.wobble_probabilities[i][1])) return std::nullopt;
        }
    }

    // weather_type_modifiers: u8 count + count × {wid, tid, mul}
    {
        uint8_t n = 0;
        if (!r.read_le(n)) return std::nullopt;
        rules.weather_type_modifiers.resize(n);
        for (uint8_t i = 0; i < n; ++i) {
            auto& e = rules.weather_type_modifiers[i];
            if (!r.read_le(e.weather_id)) return std::nullopt;
            if (!r.read_le(e.type_id))    return std::nullopt;
            if (!r.read_le(e.multiplier)) return std::nullopt;
        }
    }

    // weather_move_modifiers: u8 count + count × {wid, eid, mul}
    {
        uint8_t n = 0;
        if (!r.read_le(n)) return std::nullopt;
        rules.weather_move_modifiers.resize(n);
        for (uint8_t i = 0; i < n; ++i) {
            auto& e = rules.weather_move_modifiers[i];
            if (!r.read_le(e.weather_id)) return std::nullopt;
            if (!r.read_le(e.type_id))    return std::nullopt;
            if (!r.read_le(e.multiplier)) return std::nullopt;
        }
    }

    // high_crit_moves: u16 count + count × u16 LE (full MoveId range)
    if (!read_u16_list_as_move_ids(rules.high_crit_moves)) return std::nullopt;

    // effect_priorities: u8 count + count × {eid, priority}
    {
        uint8_t n = 0;
        if (!r.read_le(n)) return std::nullopt;
        rules.effect_priorities.resize(n);
        for (uint8_t i = 0; i < n; ++i) {
            if (!r.read_le(rules.effect_priorities[i].effect_id)) return std::nullopt;
            if (!r.read_le(rules.effect_priorities[i].priority))  return std::nullopt;
        }
    }

    // AI byte lists
    if (!read_byte_list(rules.ai_status_only_effects)) return std::nullopt;
    if (!read_byte_list(rules.ai_risky_effects))       return std::nullopt;
    if (!read_byte_list(rules.ai_stall_move_ids))      return std::nullopt;
    if (!read_byte_list(rules.ai_useful_move_ids))        return std::nullopt;
    if (!read_byte_list(rules.ai_residual_move_ids))      return std::nullopt;
    if (!read_byte_list(rules.ai_encore_move_ids))        return std::nullopt;
    if (!read_byte_list(rules.ai_rain_dance_move_ids))    return std::nullopt;
    if (!read_byte_list(rules.ai_sunny_day_move_ids))     return std::nullopt;
    if (!read_byte_list(rules.ai_stat_up_effects))        return std::nullopt;
    if (!read_byte_list(rules.ai_stat_down_effects))      return std::nullopt;

    // trainer_class_ai: u16 LE count + count × 8 bytes
    {
        uint16_t n = 0;
        if (!r.read_le(n)) return std::nullopt;
        if (n > 256u)      return std::nullopt;  // sanity: Crystal has 67 classes
        rules.trainer_class_ai.resize(n);
        for (uint16_t i = 0; i < n; ++i) {
            auto& t = rules.trainer_class_ai[i];
            if (!r.read_le(t.item1))       return std::nullopt;
            if (!r.read_le(t.item2))       return std::nullopt;
            if (!r.read_le(t.base_reward)) return std::nullopt;
            // Deserialize EMON-owned ai_passes flags byte.
            // Bit positions are EMON BRLS wire spec — no Crystal ROM ABI knowledge:
            //   bit 0=run_basic  bit 1=run_setup  bit 2=run_types
            //   bit 3=run_offensive  bit 4=run_smart
            uint8_t ai_passes_byte = 0;
            if (!r.read_le(ai_passes_byte)) return std::nullopt;
            t.ai_passes.run_basic     = (ai_passes_byte & (1u << 0)) != 0;
            t.ai_passes.run_setup     = (ai_passes_byte & (1u << 1)) != 0;
            t.ai_passes.run_types     = (ai_passes_byte & (1u << 2)) != 0;
            t.ai_passes.run_offensive = (ai_passes_byte & (1u << 3)) != 0;
            t.ai_passes.run_smart     = (ai_passes_byte & (1u << 4)) != 0;
            if (!r.read_le(t.ai_item_flags)) return std::nullopt;
            // DV nibble pairs: {atk<<4|def, spd<<4|spc}
            uint8_t dv0 = 0, dv1 = 0;
            if (!r.read_le(dv0)) return std::nullopt;
            if (!r.read_le(dv1)) return std::nullopt;
            t.dv_atk = (dv0 >> 4) & 0x0F;
            t.dv_def = (dv0)      & 0x0F;
            t.dv_spd = (dv1 >> 4) & 0x0F;
            t.dv_spc = (dv1)      & 0x0F;
        }
    }

    if (!rules.is_valid()) return std::nullopt;

    // =========================================================================
    // SM83-lifted formula parameters — read if bytes remain in the chunk.
    // These are appended after the existing fields and are optional:
    // older packages without them retain the struct defaults (vanilla values).
    // Each field is exactly 1 byte.
    // =========================================================================
    auto read_sm83_u8 = [&](uint8_t& out) -> bool {
        if (!r.has_bytes(1)) return false;  // end of old package — keep default
        return r.read_le(out);
    };

    // sm83_damage_formula: 4 bytes
    read_sm83_u8(rules.damage_formula.level_divisor);
    read_sm83_u8(rules.damage_formula.level_addend);
    read_sm83_u8(rules.damage_formula.damage_divisor);
    read_sm83_u8(rules.damage_formula.min_damage);
    // sm83_ai_scores: 2 bytes
    read_sm83_u8(rules.ai_scores.init_score);
    read_sm83_u8(rules.ai_scores.discourage_strong);
    // sm83_stat_formula: 3 bytes
    read_sm83_u8(rules.stat_formula.level_divisor);
    read_sm83_u8(rules.stat_formula.non_hp_offset);
    read_sm83_u8(rules.stat_formula.hp_offset);
    // sm83_escape: 2 bytes
    read_sm83_u8(rules.escape.speed_multiplier);
    read_sm83_u8(rules.escape.attempt_addend);
    // sm83_capture_status: 2 bytes
    read_sm83_u8(rules.capture_status.slp_frz_bonus);
    read_sm83_u8(rules.capture_status.brn_psn_par_bonus);
    // sm83_exp: 1 byte
    read_sm83_u8(rules.exp_formula.base_divisor);
    // sm83_residual: 2 bytes
    read_sm83_u8(rules.residual.burn_poison_denom);
    read_sm83_u8(rules.residual.toxic_denom);
    // sm83_crit_deltas: 3 bytes
    read_sm83_u8(rules.crit_deltas.held_item_delta);
    read_sm83_u8(rules.crit_deltas.scope_lens_delta);
    read_sm83_u8(rules.crit_deltas.focus_energy_delta);
    // sm83_damage_variation: 1 byte
    read_sm83_u8(rules.damage_variation.lower_bound_byte);

    return rules;
}

} // namespace enginemon
