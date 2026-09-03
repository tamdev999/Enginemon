#pragma once
// engine/core/game_state.hpp
// Minimal game state for save/load and continuity
//
// Serializes/restores enough to prove ownership boundaries:
// - Current map (semantic ID)
// - Player position/facing
// - Event flags
// - RNG state (if present)
//
// Reference: Gen2Recomped save system, pokecrystal SRAM layout

#include "engine/core/types.hpp"
#include "engine/core/rtc.hpp"
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>

namespace enginemon {

//=============================================================================
// PLAYER SAVE STATE
//=============================================================================

struct PlayerSaveState {
    std::string current_map_id;     // Semantic map ID
    int32_t x = 0;                  // Tile position
    int32_t y = 0;
    Direction facing = Direction::Down;
    bool surfing = false;
    bool on_bike = false;
};

//=============================================================================
// WARP MEMORY
// For LAST_MAP/LAST_WARP style exits (pokered wLastMap, GSC wBackupWarp)
//=============================================================================

struct WarpMemory {
    std::string map_id;             // Last outdoor map (for LAST_MAP exits)
    int32_t x = 0;
    int32_t y = 0;
    
    // Backup warp for LAST_WARP (GSC)
    std::string backup_map_id;
    int32_t backup_x = 0;
    int32_t backup_y = 0;
};

//=============================================================================
// GAMEPLAY RNG — CANONICAL AUTHORITATIVE STREAM
//
// PCG-XSH-RR (64-bit state, 32-bit output)
// O'Neill reference implementation, Jan 2014
//
// OWNERSHIP: GameState owns exactly one instance. All gameplay-affecting
// randomness must draw from this stream. Presentation RNG uses RngState
// (map_rng_) which is never serialized.
//
// SEEDING:
//   seed(value)         — O'Neill canonical init (new game / deterministic test)
//   restore_state(s)    — direct state restore for save/load (NOT re-seeding)
//
// DRAW COUNTS (contractual):
//   next_u32()          — exactly 1 draw
//   next_u8()           — exactly 1 draw
//   next_u64()          — exactly 2 draws (high=first, low=second)
//   bounded(n)          — 1+ draws (Lemire unbiased); n==0 is programmer error
//
// Source: docs/NATIVE_RNG_ARCHITECTURE.md
//=============================================================================

class GameplayRng {
public:
    static constexpr uint64_t MULTIPLIER = 6364136223846793005ULL;
    static constexpr uint64_t INCREMENT  = 1442695040888963407ULL;

    // O'Neill canonical seeding (new game / deterministic tests)
    void seed(uint64_t seed_value) noexcept {
        state_ = 0;
        step();
        state_ += seed_value;
        step();
    }

    // Direct state restoration — save/load ONLY, NOT re-seeding
    void restore_state(uint64_t s) noexcept { state_ = s; }

    // Core PCG-XSH-RR advance — 1 draw
    uint32_t next_u32() noexcept {
        uint64_t old = state_;
        state_ = old * MULTIPLIER + INCREMENT;
        uint32_t xorshifted = static_cast<uint32_t>(((old >> 18u) ^ old) >> 27u);
        uint32_t rot = static_cast<uint32_t>(old >> 59u);
        return (xorshifted >> rot) | (xorshifted << ((~rot + 1u) & 31u));
    }

    // 8-bit draw for Crystal-derived mechanics — 1 draw
    uint8_t next_u8() noexcept { return static_cast<uint8_t>(next_u32()); }

    // 64-bit draw with defined sequencing — 2 draws
    // First draw → high 32 bits; second draw → low 32 bits
    uint64_t next_u64() noexcept {
        uint64_t hi = next_u32();   // Draw 1 → high
        uint64_t lo = next_u32();   // Draw 2 → low
        return (hi << 32u) | lo;
    }

    // Unbiased bounded sample via Lemire's method — 1+ draws
    // PRECONDITION: range > 0 (programmer error if zero)
    uint32_t bounded(uint32_t range) {
        if (range == 0) {
            // Programmer error — 0 draws consumed, explicit failure
            throw std::invalid_argument("GameplayRng::bounded(0): range must be > 0");
        }
        uint64_t r = next_u32();
        uint64_t product = r * static_cast<uint64_t>(range);
        uint32_t low = static_cast<uint32_t>(product);
        if (low < range) {
            uint32_t threshold = static_cast<uint32_t>(-static_cast<int32_t>(range) % static_cast<int32_t>(range));
            while (low < threshold) {
                r = next_u32();
                product = r * static_cast<uint64_t>(range);
                low = static_cast<uint32_t>(product);
            }
        }
        return static_cast<uint32_t>(product >> 32u);
    }

    uint64_t state() const noexcept { return state_; }

private:
    uint64_t state_ = 0;

    void step() noexcept {
        state_ = state_ * MULTIPLIER + INCREMENT;
    }
};

//=============================================================================
// NPC SAVE STATE
// Per-NPC runtime state that affects gameplay determinism
// Must be saved/restored for deterministic simulation resume
//=============================================================================

struct NpcSaveState {
    uint16_t id = 0;            // Object local ID (1-indexed)
    int32_t x = 0;              // Current tile position
    int32_t y = 0;
    Direction facing = Direction::Down;
    bool is_moving = false;
    int32_t idle_timer = 0;     // Frames until next movement attempt - CRITICAL for determinism
    int32_t target_x = 0;       // Movement target X (during move)
    int32_t target_y = 0;       // Movement target Y
    int32_t move_progress = 0;  // Ticks into current movement
    bool frozen = false;        // Script is interacting with this NPC
    bool visible = true;        // Visibility state
};

//=============================================================================
// DESERIALIZATION RESULT TYPES (Audit 5)
// Explicit error handling for save loading - corrupt saves must not silently
// become default/new-game states
//=============================================================================

enum class DeserializeError {
    Success,
    TruncatedData,          // Data too short
    InvalidMagic,           // Wrong magic number
    UnsupportedVersion,     // Schema version mismatch
    CorruptedPayload,       // Parse error within payload
};

// Forward declaration - DeserializeResult is defined after GameState
struct DeserializeResult;

//=============================================================================
// GAME STATE
// Complete saveable state
//=============================================================================

struct GameState {
    // Player
    PlayerSaveState player;
    
    // Warp memory
    WarpMemory warp_memory;
    
    // Event flags (semantic IDs)
    std::unordered_set<std::string> flags;
    
    // Variables (semantic ID -> integer value)
    std::unordered_map<std::string, int32_t> variables;
    
    // Variable sprite assignments (slot_name → assigned sprite_id string).
    // ...existing doc...
    std::unordered_map<std::string, std::string> variable_sprites;

    // Day Care species occupancy.
    // Source: Crystal wBreedMon1Species / wBreedMon2Species (WRAM bank 1).
    // SpeciesId 0 = slot is empty (no Pokémon deposited).
    // SpeciesId 1-251 = Pokémon species currently in the Day Care slot.
    // This state drives the overworld sprite for "daycare:1" / "daycare:2" objects
    // (Route 34, outdoor_sprites.asm).
    //
    // Visibility of the Day Care Pokémon objects is separately controlled by the
    // event flags EVENT_DAY_CARE_MON_1 / EVENT_DAY_CARE_MON_2 (in the flags set).
    // When the flag is SET the object is hidden; when CLEARED it is visible.
    // The Route34EggCheckCallback synchronizes flags with the actual occupancy.
    //
    // daycare_slot[0] = slot 1 (wBreedMon1Species equivalent)
    // daycare_slot[1] = slot 2 (wBreedMon2Species equivalent)
    std::array<SpeciesId, 2> daycare_slot = {0, 0};
    
    // RNG — canonical authoritative gameplay stream (PCG-XSH-RR)
    GameplayRng rng;
    
    // Item bag — canonical item inventory.
    // ItemId -> quantity (quantity 0 means item is absent; entries may be absent).
    // Source: Crystal wNumItems / wItems (PC item list not tracked yet).
    // Crystal bag is capped at 20 items / 99 per slot.  We do not enforce the
    // 20-slot cap here — that is a presentation constraint, not a semantic one.
    // Quantity cap of 99 per slot matches Crystal Gen2 bag semantics.
    // NOTE: The per-slot cap is enforced in give_item() using BattleRules::frontend_limits
    // when a package is loaded, or the hardcoded vanilla default of 99 otherwise.
    static constexpr int32_t ITEM_QUANTITY_MAX = 99;  // vanilla default; see BattleRules::frontend_limits
    std::unordered_map<ItemId, int32_t> items;

    // NPC states per map (map_id -> NPC states)
    // This captures all gameplay-relevant NPC runtime state for deterministic resume
    std::unordered_map<std::string, std::vector<NpcSaveState>> npc_states;
    
    // Playtime (frames or seconds)
    uint64_t playtime_frames = 0;

    // Real-Time Clock offset (seconds).
    // effective_time = system_clock::now() + rtc_offset_seconds
    // Setting the in-game clock recomputes this offset; it does not tick
    // with game frames, turbo mode, or pause.
    // Serialized in save format v6.
    int64_t rtc_offset_seconds = 0;

    // DST preference flag (player-set).
    // When true, effective RTC is offset by +3600 (one hour ahead of standard).
    // Implementation: adjust rtc_offset_seconds by ±3600 when toggled.
    // The offset already absorbs DST; this flag records the player's preference
    // so it can be displayed and toggled correctly.
    bool rtc_dst_enabled = false;
    
    //=========================================================================
    // FLAG OPERATIONS
    //=========================================================================
    
    void set_flag(const std::string& flag_id) {
        flags.insert(flag_id);
    }
    
    void clear_flag(const std::string& flag_id) {
        flags.erase(flag_id);
    }
    
    bool check_flag(const std::string& flag_id) const {
        return flags.find(flag_id) != flags.end();
    }
    
    //=========================================================================
    // VARIABLE OPERATIONS
    //=========================================================================
    
    void set_var(const std::string& var_id, int32_t value) {
        variables[var_id] = value;
    }
    
    int32_t get_var(const std::string& var_id) const {
        auto it = variables.find(var_id);
        return it != variables.end() ? it->second : 0;
    }
    
    //=========================================================================
    // ITEM BAG OPERATIONS
    // Source: Crystal Gen2 bag semantics (wNumItems, wItems).
    // Quantity cap: 99 per slot (Crystal ITEM_QUANTITY_MAX).
    //=========================================================================

    // Give count of item_id.  Returns true if the item was added (always true
    // unless count <= 0).  Quantity is clamped at ITEM_QUANTITY_MAX (99).
    bool give_item(ItemId item_id, int32_t count) {
        if (count <= 0) return false;
        auto& qty = items[item_id];
        qty = std::min(qty + count, ITEM_QUANTITY_MAX);
        return true;
    }

    // Take count of item_id.  Returns true if the player had enough.
    bool take_item(ItemId item_id, int32_t count) {
        if (count <= 0) return false;
        auto it = items.find(item_id);
        if (it == items.end() || it->second < count) return false;
        it->second -= count;
        if (it->second == 0) items.erase(it);
        return true;
    }

    // Returns true if the player has at least count of item_id.
    bool has_item(ItemId item_id, int32_t count = 1) const {
        if (count <= 0) return true;
        auto it = items.find(item_id);
        return it != items.end() && it->second >= count;
    }

    // Returns the current quantity of item_id in the bag (0 if absent).
    int32_t item_count(ItemId item_id) const {
        auto it = items.find(item_id);
        return it != items.end() ? it->second : 0;
    }
    
    // Serialize to bytes
    std::vector<uint8_t> serialize() const;
    
    // Deserialize from bytes with explicit error handling (Audit 5)
    // Returns DeserializeResult with error code - corrupt saves never silently
    // become valid default states
    static DeserializeResult try_deserialize(const std::vector<uint8_t>& data);
    
    // Validate deserialized state
    bool is_valid() const;
};

//=============================================================================
// DESERIALIZATION RESULT (Audit 5)
// Must be defined after GameState since it contains a GameState member
//=============================================================================

struct DeserializeResult {
    DeserializeError error = DeserializeError::Success;
    GameState state;
    
    bool ok() const { return error == DeserializeError::Success; }
    explicit operator bool() const { return ok(); }
};

//=============================================================================
// SAVE SLOT
// Wrapper with metadata
//=============================================================================

struct SaveSlot {
    bool occupied = false;
    std::string player_name;        // For display
    std::string map_display_name;   // Current location for display
    uint64_t playtime_frames = 0;
    uint32_t save_timestamp = 0;    // Unix timestamp
    
    GameState state;
};

} // namespace enginemon
