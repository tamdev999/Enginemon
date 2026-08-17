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
        warp.target_map_id.clear();
    }
    return warp;
}

static RuntimeCoordEvent read_coord_event(std::istream& in) {
    RuntimeCoordEvent evt;
    evt.x = in.get();
    evt.y = in.get();
    evt.scene_id = read_le<uint16_t>(in);
    if (!read_length_string(in, evt.script_id)) {
        evt.script_id.clear();
    }
    return evt;
}

static RuntimeBgEvent read_bg_event(std::istream& in) {
    RuntimeBgEvent evt;
    evt.x = in.get();
    evt.y = in.get();
    
    // Read the raw type byte and convert to runtime type
    uint8_t raw_type = in.get();
    // Map: 0=Read, 7=HiddenItem, others map as-is for now
    switch (raw_type) {
        case 0: evt.type = RuntimeBgEventType::Read; break;
        case 7: evt.type = RuntimeBgEventType::HiddenItem; break;
        default: evt.type = RuntimeBgEventType::Read; break;
    }
    
    evt.quantity = in.get();
    if (!read_length_string(in, evt.script_id)) {
        evt.script_id.clear();
    }
    if (!read_length_string(in, evt.item_id)) {
        evt.item_id.clear();
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
    obj.time_of_day = in.get();
    obj.is_trainer = (in.get() != 0);
    obj.trainer_sight_range = in.get();
    
    if (!read_length_string(in, obj.sprite_id)) {
        obj.sprite_id.clear();
    }
    if (!read_length_string(in, obj.script_id)) {
        obj.script_id.clear();
    }
    if (!read_length_string(in, obj.visibility_flag)) {
        obj.visibility_flag.clear();
    }
    
    return obj;
}

static RuntimeConnection read_connection(std::istream& in) {
    RuntimeConnection conn;
    
    // Read raw direction byte and convert to runtime type
    uint8_t raw_dir = in.get();
    switch (raw_dir) {
        case 0: conn.direction = ConnectionDirection::North; break;
        case 1: conn.direction = ConnectionDirection::South; break;
        case 2: conn.direction = ConnectionDirection::East; break;
        case 3: conn.direction = ConnectionDirection::West; break;
        default: conn.direction = ConnectionDirection::North; break;
    }
    
    conn.strip_offset = read_le<int32_t>(in);
    conn.strip_length = in.get();
    if (!read_length_string(in, conn.target_map_id)) {
        conn.target_map_id.clear();
    }
    return conn;
}

// Read counted array with bounds validation (Audit 4)
template<typename T>
static std::vector<T> read_counted_array(std::istream& in, T (*read_item)(std::istream&)) {
    uint32_t count = read_le<uint32_t>(in);
    
    // Bounds check: reject unreasonably large counts
    if (count > PackageLimits::MAX_ARRAY_COUNT) {
        return {};  // Return empty on malformed data
    }
    
    std::vector<T> arr;
    arr.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        if (!in.good()) break;  // Stop on stream error
        arr.push_back(read_item(in));
    }
    return arr;
}

// Deserialize map data directly to RuntimeMap
static RuntimeMap deserialize_map(const std::vector<uint8_t>& data) {
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
    
    // Derive semantic Environment from raw Crystal byte
    map.environment = environment_from_crystal(map.environment_type);
    
    uint8_t flags = in.get();
    map.is_outdoor = (flags & 0x01) != 0;
    map.phone_service_disabled = (flags & 0x02) != 0;
    
    map.lighting = in.get();
    
    // Derive time_policy from lighting byte
    // Crystal's lighting byte encodes the palette policy:
    //   0 = PALETTE_AUTO (follow RTC)
    //   1 = PALETTE_DAY (always Day)
    //   2 = PALETTE_NITE (always Night)
    //   3 = PALETTE_MORN (always Morning)
    //   4 = PALETTE_DARK (dark cave)
    if (map.lighting <= 4) {
        map.time_policy = static_cast<PalettePolicy>(map.lighting);
    } else {
        map.time_policy = PalettePolicy::Auto;
    }
    
    in.get();  // padding
    in.get();  // padding
    
    // Block data with bounds validation (Audit 4)
    uint32_t block_count = read_le<uint32_t>(in);
    if (block_count > PackageLimits::MAX_BLOCK_COUNT) {
        return map;  // Return partial map on malformed data
    }
    map.blocks.resize(block_count);
    in.read(reinterpret_cast<char*>(map.blocks.data()), block_count);
    
    // Events - directly to runtime types
    map.warps = read_counted_array(in, read_warp);
    map.coord_events = read_counted_array(in, read_coord_event);
    map.bg_events = read_counted_array(in, read_bg_event);
    map.objects = read_counted_array(in, read_object);
    map.connections = read_counted_array(in, read_connection);
    
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
    
    // Deserialize directly to RuntimeMap
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
    
    // Deserialize sprite
    std::istringstream in(std::string(data->begin(), data->end()), std::ios::binary);
    
    RuntimeSprite sprite;
    
    // Read sprite_id
    uint16_t id_len = read_le<uint16_t>(in);
    sprite.sprite_id.resize(id_len);
    in.read(sprite.sprite_id.data(), id_len);
    
    // Read type and palette
    sprite.type = static_cast<SpriteType>(in.get());
    sprite.default_palette = static_cast<SpritePalette>(in.get());
    
    // Read frames
    uint32_t frame_count = read_le<uint32_t>(in);
    sprite.frames.resize(frame_count);
    for (uint32_t i = 0; i < frame_count; ++i) {
        in.read(reinterpret_cast<char*>(sprite.frames[i].pixels.data()), 256);
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

} // namespace enginemon
