// engine/core/game_state.cpp
// Game state serialization
//
// Minimal binary format for save/load testing.
// Not the final save format - just proving ownership boundaries.
//
// NOTE (Audit 8): Iteration order of unordered containers is implementation-defined.
// Serialization MUST use canonical (sorted) ordering for deterministic output.

#include "engine/core/game_state.hpp"
#include <cstring>
#include <algorithm>
#include <stdexcept>

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
// Version history:
//   v4 — added daycare_slot; RNG stored as two uint64_t (LCG seed + state)
//   v5 — PCG-XSH-RR replaces LCG; RNG stored as single uint64_t (PCG state)
constexpr uint32_t SAVE_VERSION   = 5;   // Current version
constexpr uint32_t SAVE_VERSION_4 = 4;   // Legacy LCG version (migrate on load)

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

// Read bool — only accepts exactly 0 or 1 as valid boolean encodings
bool read_bool(const uint8_t*& ptr, const uint8_t* end, bool& out) {
    if (ptr >= end) return false;
    uint8_t val = *ptr++;
    if (val > 1) return false;  // Only 0 or 1 are valid boolean encodings
    out = (val != 0);
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
    
    // Flags - sorted for canonical ordering (Audit 8)
    std::vector<std::string> sorted_flags(flags.begin(), flags.end());
    std::sort(sorted_flags.begin(), sorted_flags.end());
    write_int32(out, static_cast<int32_t>(sorted_flags.size()));
    for (const auto& flag : sorted_flags) {
        write_string(out, flag);
    }
    
    // Variables - sorted for canonical ordering (Audit 8)
    std::vector<std::pair<std::string, int32_t>> sorted_vars(variables.begin(), variables.end());
    std::sort(sorted_vars.begin(), sorted_vars.end(), 
              [](const auto& a, const auto& b) { return a.first < b.first; });
    write_int32(out, static_cast<int32_t>(sorted_vars.size()));
    for (const auto& [key, value] : sorted_vars) {
        write_string(out, key);
        write_int32(out, value);
    }

    // Variable sprite assignments — sorted for canonical ordering.
    // Key: slot_name (e.g., "copycat"), Value: assigned sprite_id (e.g., "fixed:lass").
    std::vector<std::pair<std::string, std::string>> sorted_var_sprites(
        variable_sprites.begin(), variable_sprites.end());
    std::sort(sorted_var_sprites.begin(), sorted_var_sprites.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    write_int32(out, static_cast<int32_t>(sorted_var_sprites.size()));
    for (const auto& [slot, sprite_id] : sorted_var_sprites) {
        write_string(out, slot);
        write_string(out, sprite_id);
    }
    
    // RNG state — v5: single uint64_t PCG internal state
    write_uint64(out, rng.state());

    // Day Care occupancy (slot 1 and slot 2 species IDs; 0 = empty)
    // Source: Crystal wBreedMon1Species / wBreedMon2Species
    write_int32(out, static_cast<int32_t>(daycare_slot[0]));
    write_int32(out, static_cast<int32_t>(daycare_slot[1]));
    
    // Playtime
    write_uint64(out, playtime_frames);
    
    // NPC states per map - sorted by map_id for canonical ordering (Audit 8)
    std::vector<std::pair<std::string, std::vector<NpcSaveState>>> sorted_npc_states(
        npc_states.begin(), npc_states.end());
    std::sort(sorted_npc_states.begin(), sorted_npc_states.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    write_int32(out, static_cast<int32_t>(sorted_npc_states.size()));
    for (const auto& [map_id, states] : sorted_npc_states) {
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

DeserializeResult GameState::try_deserialize(const std::vector<uint8_t>& data) {
    DeserializeResult result;
    
    if (data.size() < 8) {
        result.error = DeserializeError::TruncatedData;
        return result;
    }
    
    const uint8_t* ptr = data.data();
    const uint8_t* end = data.data() + data.size();
    
    // Header
    int32_t magic = 0, version = 0;
    if (!read_int32(ptr, end, magic)) {
        result.error = DeserializeError::TruncatedData;
        return result;
    }
    if (!read_int32(ptr, end, version)) {
        result.error = DeserializeError::TruncatedData;
        return result;
    }
    
    if (static_cast<uint32_t>(magic) != SAVE_MAGIC) {
        result.error = DeserializeError::InvalidMagic;
        return result;
    }
    // Accept current version (v5, PCG) and legacy version (v4, LCG)
    const bool is_v4 = (static_cast<uint32_t>(version) == SAVE_VERSION_4);
    const bool is_v5 = (static_cast<uint32_t>(version) == SAVE_VERSION);
    if (!is_v4 && !is_v5) {
        result.error = DeserializeError::UnsupportedVersion;
        return result;
    }
    
    GameState& state = result.state;
    
    // Player state
    if (!read_string(ptr, end, state.player.current_map_id)) {
        result.error = DeserializeError::CorruptedPayload;
        return result;
    }
    if (!read_int32(ptr, end, state.player.x)) {
        result.error = DeserializeError::CorruptedPayload;
        return result;
    }
    if (!read_int32(ptr, end, state.player.y)) {
        result.error = DeserializeError::CorruptedPayload;
        return result;
    }
    
    uint8_t facing = 0;
    if (!read_uint8(ptr, end, facing)) {
        result.error = DeserializeError::CorruptedPayload;
        return result;
    }
    state.player.facing = static_cast<Direction>(facing);
    // Validate Direction is within domain (0-3: Down=0, Up=1, Left=2, Right=3)
    if (static_cast<uint8_t>(state.player.facing) > 3) {
        result.error = DeserializeError::CorruptedPayload;
        return result;
    }
    
    uint8_t surfing = 0, on_bike = 0;
    if (!read_uint8(ptr, end, surfing)) {
        result.error = DeserializeError::CorruptedPayload;
        return result;
    }
    if (!read_uint8(ptr, end, on_bike)) {
        result.error = DeserializeError::CorruptedPayload;
        return result;
    }
    state.player.surfing = surfing != 0;
    state.player.on_bike = on_bike != 0;
    
    // Warp memory
    if (!read_string(ptr, end, state.warp_memory.map_id)) {
        result.error = DeserializeError::CorruptedPayload;
        return result;
    }
    if (!read_int32(ptr, end, state.warp_memory.x)) {
        result.error = DeserializeError::CorruptedPayload;
        return result;
    }
    if (!read_int32(ptr, end, state.warp_memory.y)) {
        result.error = DeserializeError::CorruptedPayload;
        return result;
    }
    if (!read_string(ptr, end, state.warp_memory.backup_map_id)) {
        result.error = DeserializeError::CorruptedPayload;
        return result;
    }
    if (!read_int32(ptr, end, state.warp_memory.backup_x)) {
        result.error = DeserializeError::CorruptedPayload;
        return result;
    }
    if (!read_int32(ptr, end, state.warp_memory.backup_y)) {
        result.error = DeserializeError::CorruptedPayload;
        return result;
    }
    
    // Flags
    int32_t flag_count = 0;
    if (!read_int32(ptr, end, flag_count)) {
        result.error = DeserializeError::CorruptedPayload;
        return result;
    }
    // Bounds check (Audit 4)
    if (flag_count < 0 || flag_count > 1000000) {
        result.error = DeserializeError::CorruptedPayload;
        return result;
    }
    for (int32_t i = 0; i < flag_count; i++) {
        std::string flag;
        if (!read_string(ptr, end, flag)) {
            result.error = DeserializeError::CorruptedPayload;
            return result;
        }
        state.flags.insert(flag);
    }
    
    // Variables
    int32_t var_count = 0;
    if (!read_int32(ptr, end, var_count)) {
        result.error = DeserializeError::CorruptedPayload;
        return result;
    }
    // Bounds check (Audit 4)
    if (var_count < 0 || var_count > 1000000) {
        result.error = DeserializeError::CorruptedPayload;
        return result;
    }
    for (int32_t i = 0; i < var_count; i++) {
        std::string key;
        int32_t value = 0;
        if (!read_string(ptr, end, key)) {
            result.error = DeserializeError::CorruptedPayload;
            return result;
        }
        if (!read_int32(ptr, end, value)) {
            result.error = DeserializeError::CorruptedPayload;
            return result;
        }
        state.variables[key] = value;
    }

    // Variable sprite assignments
    int32_t var_sprite_count = 0;
    if (!read_int32(ptr, end, var_sprite_count)) {
        result.error = DeserializeError::CorruptedPayload;
        return result;
    }
    if (var_sprite_count < 0 || var_sprite_count > 1000000) {
        result.error = DeserializeError::CorruptedPayload;
        return result;
    }
    for (int32_t i = 0; i < var_sprite_count; i++) {
        std::string slot, sprite_id;
        if (!read_string(ptr, end, slot)) {
            result.error = DeserializeError::CorruptedPayload;
            return result;
        }
        if (!read_string(ptr, end, sprite_id)) {
            result.error = DeserializeError::CorruptedPayload;
            return result;
        }
        state.variable_sprites[slot] = sprite_id;
    }
    
    // RNG state — version-dependent deserialization
    if (is_v4) {
        // v4 stored two uint64_t: legacy LCG seed + state.
        // Migrate: use old state value as seed input into O'Neill canonical init.
        uint64_t legacy_seed = 0, legacy_state = 0;
        if (!read_uint64(ptr, end, legacy_seed)) {
            result.error = DeserializeError::CorruptedPayload;
            return result;
        }
        if (!read_uint64(ptr, end, legacy_state)) {
            result.error = DeserializeError::CorruptedPayload;
            return result;
        }
        // Deterministic migration: seed PCG from legacy state value
        state.rng.seed(legacy_state);
    } else {
        // v5: single uint64_t PCG internal state — restore directly
        uint64_t pcg_state = 0;
        if (!read_uint64(ptr, end, pcg_state)) {
            result.error = DeserializeError::CorruptedPayload;
            return result;
        }
        state.rng.restore_state(pcg_state);
    }

    // Day Care occupancy
    {
        int32_t s1 = 0, s2 = 0;
        if (!read_int32(ptr, end, s1) || !read_int32(ptr, end, s2)) {
            result.error = DeserializeError::CorruptedPayload;
            return result;
        }
        // Validate: SpeciesId must be 0 (empty/SPECIES_NONE) or a non-zero uint16_t.
        // The hardcoded > 251 ceiling is intentionally removed here.
        // Actual domain validation is by registry membership at runtime;
        // the save format supports any profile's species ceiling.
        // Guard only against negative values (corrupt int32 sign extension) and
        // implausibly large values that indicate data corruption.
        if ((s1 != 0 && (s1 < 1 || s1 > 65534)) ||
            (s2 != 0 && (s2 < 1 || s2 > 65534))) {
            result.error = DeserializeError::CorruptedPayload;
            return result;
        }
        state.daycare_slot[0] = static_cast<SpeciesId>(s1);
        state.daycare_slot[1] = static_cast<SpeciesId>(s2);
    }
    
    // Playtime
    if (!read_uint64(ptr, end, state.playtime_frames)) {
        result.error = DeserializeError::CorruptedPayload;
        return result;
    }
    
    // NPC states per map — MANDATORY in version 2 (never optional).
    // A v2 save that truncates before map_count is corrupted, not merely
    // missing optional data.  Return CorruptedPayload, not Success.
    int32_t map_count = 0;
    if (!read_int32(ptr, end, map_count)) {
        result.error = DeserializeError::CorruptedPayload;
        return result;
    }
    // Bounds check
    if (map_count < 0 || map_count > 10000) {
        result.error = DeserializeError::CorruptedPayload;
        return result;
    }
    for (int32_t m = 0; m < map_count; m++) {
            std::string map_id;
            if (!read_string(ptr, end, map_id)) {
                result.error = DeserializeError::CorruptedPayload;
                return result;
            }
            
            int32_t npc_count = 0;
            if (!read_int32(ptr, end, npc_count)) {
                result.error = DeserializeError::CorruptedPayload;
                return result;
            }
            // Bounds check (Audit 4)
            if (npc_count < 0 || npc_count > 1000) {
                result.error = DeserializeError::CorruptedPayload;
                return result;
            }
            
            std::vector<NpcSaveState> npcs;
            npcs.reserve(npc_count);
            
            for (int32_t n = 0; n < npc_count; n++) {
                NpcSaveState npc;
                if (!read_uint16(ptr, end, npc.id)) {
                    result.error = DeserializeError::CorruptedPayload;
                    return result;
                }
                if (!read_int32(ptr, end, npc.x)) {
                    result.error = DeserializeError::CorruptedPayload;
                    return result;
                }
                if (!read_int32(ptr, end, npc.y)) {
                    result.error = DeserializeError::CorruptedPayload;
                    return result;
                }
                
                uint8_t facing = 0;
                if (!read_uint8(ptr, end, facing)) {
                    result.error = DeserializeError::CorruptedPayload;
                    return result;
                }
                npc.facing = static_cast<Direction>(facing);
                // Validate Direction is within domain (0-3)
                if (static_cast<uint8_t>(npc.facing) > 3) {
                    result.error = DeserializeError::CorruptedPayload;
                    return result;
                }
                
                if (!read_bool(ptr, end, npc.is_moving)) {
                    result.error = DeserializeError::CorruptedPayload;
                    return result;
                }
                if (!read_int32(ptr, end, npc.idle_timer)) {
                    result.error = DeserializeError::CorruptedPayload;
                    return result;
                }
                if (!read_int32(ptr, end, npc.target_x)) {
                    result.error = DeserializeError::CorruptedPayload;
                    return result;
                }
                if (!read_int32(ptr, end, npc.target_y)) {
                    result.error = DeserializeError::CorruptedPayload;
                    return result;
                }
                if (!read_int32(ptr, end, npc.move_progress)) {
                    result.error = DeserializeError::CorruptedPayload;
                    return result;
                }
                if (!read_bool(ptr, end, npc.frozen)) {
                    result.error = DeserializeError::CorruptedPayload;
                    return result;
                }
                if (!read_bool(ptr, end, npc.visible)) {
                    result.error = DeserializeError::CorruptedPayload;
                    return result;
                }
                
                npcs.push_back(npc);
            }
            
            state.npc_states[map_id] = std::move(npcs);
        }
    
    // Require exact payload consumption — no trailing bytes allowed
    if (ptr != end) {
        result.error = DeserializeError::CorruptedPayload;
        return result;
    }
    
    result.error = DeserializeError::Success;
    return result;
}

bool GameState::is_valid() const {
    // Must have a current map
    return !player.current_map_id.empty();
}

} // namespace enginemon
