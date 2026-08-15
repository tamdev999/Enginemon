// engine/core/game_state.cpp
// Game state serialization
//
// Minimal binary format for save/load testing.
// Not the final save format - just proving ownership boundaries.

#include "engine/core/game_state.hpp"
#include <cstring>

namespace enginemon {

//=============================================================================
// SERIALIZATION HELPERS
//=============================================================================

namespace {

// Write string: length (4 bytes) + data
void write_string(std::vector<uint8_t>& out, const std::string& str) {
    uint32_t len = static_cast<uint32_t>(str.size());
    out.push_back(static_cast<uint8_t>(len & 0xFF));
    out.push_back(static_cast<uint8_t>((len >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>((len >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((len >> 24) & 0xFF));
    for (char c : str) {
        out.push_back(static_cast<uint8_t>(c));
    }
}

// Read string
bool read_string(const uint8_t*& ptr, const uint8_t* end, std::string& out) {
    if (ptr + 4 > end) return false;
    uint32_t len = static_cast<uint32_t>(ptr[0]) |
                   (static_cast<uint32_t>(ptr[1]) << 8) |
                   (static_cast<uint32_t>(ptr[2]) << 16) |
                   (static_cast<uint32_t>(ptr[3]) << 24);
    ptr += 4;
    if (ptr + len > end) return false;
    out.assign(reinterpret_cast<const char*>(ptr), len);
    ptr += len;
    return true;
}

// Write int32
void write_int32(std::vector<uint8_t>& out, int32_t val) {
    out.push_back(static_cast<uint8_t>(val & 0xFF));
    out.push_back(static_cast<uint8_t>((val >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>((val >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((val >> 24) & 0xFF));
}

// Read int32
bool read_int32(const uint8_t*& ptr, const uint8_t* end, int32_t& out) {
    if (ptr + 4 > end) return false;
    out = static_cast<int32_t>(ptr[0]) |
          (static_cast<int32_t>(ptr[1]) << 8) |
          (static_cast<int32_t>(ptr[2]) << 16) |
          (static_cast<int32_t>(ptr[3]) << 24);
    ptr += 4;
    return true;
}

// Write uint64
void write_uint64(std::vector<uint8_t>& out, uint64_t val) {
    for (int i = 0; i < 8; i++) {
        out.push_back(static_cast<uint8_t>((val >> (i * 8)) & 0xFF));
    }
}

// Read uint64
bool read_uint64(const uint8_t*& ptr, const uint8_t* end, uint64_t& out) {
    if (ptr + 8 > end) return false;
    out = 0;
    for (int i = 0; i < 8; i++) {
        out |= static_cast<uint64_t>(ptr[i]) << (i * 8);
    }
    ptr += 8;
    return true;
}

// Write uint8
void write_uint8(std::vector<uint8_t>& out, uint8_t val) {
    out.push_back(val);
}

// Read uint8
bool read_uint8(const uint8_t*& ptr, const uint8_t* end, uint8_t& out) {
    if (ptr >= end) return false;
    out = *ptr++;
    return true;
}

// Magic number for save format
constexpr uint32_t SAVE_MAGIC = 0x454E474D;  // "ENGM"
constexpr uint32_t SAVE_VERSION = 2;  // Bumped for NPC state addition

// Write uint16
void write_uint16(std::vector<uint8_t>& out, uint16_t val) {
    out.push_back(static_cast<uint8_t>(val & 0xFF));
    out.push_back(static_cast<uint8_t>((val >> 8) & 0xFF));
}

// Read uint16
bool read_uint16(const uint8_t*& ptr, const uint8_t* end, uint16_t& out) {
    if (ptr + 2 > end) return false;
    out = static_cast<uint16_t>(ptr[0]) |
          (static_cast<uint16_t>(ptr[1]) << 8);
    ptr += 2;
    return true;
}

// Write bool
void write_bool(std::vector<uint8_t>& out, bool val) {
    out.push_back(val ? 1 : 0);
}

// Read bool
bool read_bool(const uint8_t*& ptr, const uint8_t* end, bool& out) {
    if (ptr >= end) return false;
    out = (*ptr++) != 0;
    return true;
}

} // anonymous namespace

//=============================================================================
// SERIALIZATION
//=============================================================================

std::vector<uint8_t> GameState::serialize() const {
    std::vector<uint8_t> out;
    out.reserve(1024);  // Reasonable starting size
    
    // Header
    write_int32(out, static_cast<int32_t>(SAVE_MAGIC));
    write_int32(out, static_cast<int32_t>(SAVE_VERSION));
    
    // Player state
    write_string(out, player.current_map_id);
    write_int32(out, player.x);
    write_int32(out, player.y);
    write_uint8(out, static_cast<uint8_t>(player.facing));
    write_uint8(out, player.surfing ? 1 : 0);
    write_uint8(out, player.on_bike ? 1 : 0);
    
    // Warp memory
    write_string(out, warp_memory.map_id);
    write_int32(out, warp_memory.x);
    write_int32(out, warp_memory.y);
    write_string(out, warp_memory.backup_map_id);
    write_int32(out, warp_memory.backup_x);
    write_int32(out, warp_memory.backup_y);
    
    // Flags
    write_int32(out, static_cast<int32_t>(flags.size()));
    for (const auto& flag : flags) {
        write_string(out, flag);
    }
    
    // Variables
    write_int32(out, static_cast<int32_t>(variables.size()));
    for (const auto& [key, value] : variables) {
        write_string(out, key);
        write_int32(out, value);
    }
    
    // RNG state
    write_uint64(out, rng.seed);
    write_uint64(out, rng.state);
    
    // Playtime
    write_uint64(out, playtime_frames);
    
    // NPC states per map
    write_int32(out, static_cast<int32_t>(npc_states.size()));
    for (const auto& [map_id, states] : npc_states) {
        write_string(out, map_id);
        write_int32(out, static_cast<int32_t>(states.size()));
        for (const auto& npc : states) {
            write_uint16(out, npc.id);
            write_int32(out, npc.x);
            write_int32(out, npc.y);
            write_uint8(out, static_cast<uint8_t>(npc.facing));
            write_bool(out, npc.is_moving);
            write_int32(out, npc.idle_timer);
            write_int32(out, npc.target_x);
            write_int32(out, npc.target_y);
            write_int32(out, npc.move_progress);
            write_bool(out, npc.frozen);
            write_bool(out, npc.visible);
        }
    }
    
    return out;
}

GameState GameState::deserialize(const std::vector<uint8_t>& data) {
    GameState state;
    
    if (data.size() < 8) return state;  // Invalid
    
    const uint8_t* ptr = data.data();
    const uint8_t* end = data.data() + data.size();
    
    // Header
    int32_t magic = 0, version = 0;
    if (!read_int32(ptr, end, magic)) return state;
    if (!read_int32(ptr, end, version)) return state;
    
    if (static_cast<uint32_t>(magic) != SAVE_MAGIC) return state;
    if (static_cast<uint32_t>(version) != SAVE_VERSION) return state;
    
    // Player state
    if (!read_string(ptr, end, state.player.current_map_id)) return state;
    if (!read_int32(ptr, end, state.player.x)) return state;
    if (!read_int32(ptr, end, state.player.y)) return state;
    
    uint8_t facing = 0;
    if (!read_uint8(ptr, end, facing)) return state;
    state.player.facing = static_cast<Direction>(facing);
    
    uint8_t surfing = 0, on_bike = 0;
    if (!read_uint8(ptr, end, surfing)) return state;
    if (!read_uint8(ptr, end, on_bike)) return state;
    state.player.surfing = surfing != 0;
    state.player.on_bike = on_bike != 0;
    
    // Warp memory
    if (!read_string(ptr, end, state.warp_memory.map_id)) return state;
    if (!read_int32(ptr, end, state.warp_memory.x)) return state;
    if (!read_int32(ptr, end, state.warp_memory.y)) return state;
    if (!read_string(ptr, end, state.warp_memory.backup_map_id)) return state;
    if (!read_int32(ptr, end, state.warp_memory.backup_x)) return state;
    if (!read_int32(ptr, end, state.warp_memory.backup_y)) return state;
    
    // Flags
    int32_t flag_count = 0;
    if (!read_int32(ptr, end, flag_count)) return state;
    for (int32_t i = 0; i < flag_count; i++) {
        std::string flag;
        if (!read_string(ptr, end, flag)) return state;
        state.flags.insert(flag);
    }
    
    // Variables
    int32_t var_count = 0;
    if (!read_int32(ptr, end, var_count)) return state;
    for (int32_t i = 0; i < var_count; i++) {
        std::string key;
        int32_t value = 0;
        if (!read_string(ptr, end, key)) return state;
        if (!read_int32(ptr, end, value)) return state;
        state.variables[key] = value;
    }
    
    // RNG state
    if (!read_uint64(ptr, end, state.rng.seed)) return state;
    if (!read_uint64(ptr, end, state.rng.state)) return state;
    
    // Playtime
    if (!read_uint64(ptr, end, state.playtime_frames)) return state;
    
    // NPC states per map (only present in version 2+)
    int32_t map_count = 0;
    if (read_int32(ptr, end, map_count)) {
        for (int32_t m = 0; m < map_count; m++) {
            std::string map_id;
            if (!read_string(ptr, end, map_id)) return state;
            
            int32_t npc_count = 0;
            if (!read_int32(ptr, end, npc_count)) return state;
            
            std::vector<NpcSaveState> npcs;
            npcs.reserve(npc_count);
            
            for (int32_t n = 0; n < npc_count; n++) {
                NpcSaveState npc;
                if (!read_uint16(ptr, end, npc.id)) return state;
                if (!read_int32(ptr, end, npc.x)) return state;
                if (!read_int32(ptr, end, npc.y)) return state;
                
                uint8_t facing = 0;
                if (!read_uint8(ptr, end, facing)) return state;
                npc.facing = static_cast<Direction>(facing);
                
                if (!read_bool(ptr, end, npc.is_moving)) return state;
                if (!read_int32(ptr, end, npc.idle_timer)) return state;
                if (!read_int32(ptr, end, npc.target_x)) return state;
                if (!read_int32(ptr, end, npc.target_y)) return state;
                if (!read_int32(ptr, end, npc.move_progress)) return state;
                if (!read_bool(ptr, end, npc.frozen)) return state;
                if (!read_bool(ptr, end, npc.visible)) return state;
                
                npcs.push_back(npc);
            }
            
            state.npc_states[map_id] = std::move(npcs);
        }
    }
    
    return state;
}

bool GameState::is_valid() const {
    // Must have a current map
    return !player.current_map_id.empty();
}

} // namespace enginemon
