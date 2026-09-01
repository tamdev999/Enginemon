#pragma once
// engine/battle/battle.hpp
// Battle system - turn-based Pokemon battles
// 
// MECHANICS SCOPE:
// Vanilla Crystal implements Gen 2 mechanics only (no abilities).
// However, the architecture does NOT hard-code this limitation.
// Future mods/frontends can register additional mechanics (abilities,
// new status types, new weather, etc.) through the data/behavior systems
// without requiring engine changes.

#include "engine/core/types.hpp"
#include "engine/core/registry.hpp"
#include <memory>
#include <vector>
#include <optional>
#include <functional>
#include <variant>
#include <random>

namespace enginemon {

// Forward declarations
class Party;
class Inventory;
class ITrainerAI;

// ============================================================================
// Battle Pokemon (runtime state during battle)
// ============================================================================

struct BattlePokemon {
    // Reference to party pokemon (for HP/PP/status sync)
    size_t party_index;
    
    // Species info
    SpeciesId species;
    TypeId type1;
    TypeId type2;
    
    // Level (needed for damage formula)
    uint8_t level = 1;
    
    // Current stats (with stat stages applied)
    struct Stats {
        int16_t hp;
        int16_t max_hp;
        int16_t attack;
        int16_t defense;
        int16_t speed;
        int16_t special_attack;
        int16_t special_defense;
    } stats;
    
    // Base stats (unmodified)
    Stats base_stats;
    
    // Stat stages (-6 to +6)
    struct Stages {
        int8_t attack = 0;
        int8_t defense = 0;
        int8_t speed = 0;
        int8_t special_attack = 0;
        int8_t special_defense = 0;
        int8_t accuracy = 0;
        int8_t evasion = 0;
    } stages;
    
    // Moves
    struct MoveSlot {
        MoveId move;
        uint8_t pp;
        uint8_t max_pp;
    };
    std::array<MoveSlot, 4> moves;
    
    // Status
    Status status = Status::None;
    uint8_t status_turns = 0;       // Sleep counter, toxic counter
    uint16_t volatile_status = 0;   // Bitmask of VolatileStatus
    
    // Held item
    ItemId held_item = ITEM_NONE;
    
    // Battle-specific state
    MoveId last_move_used = MOVE_NONE;
    uint8_t protect_counter = 0;
    uint8_t disable_turns = 0;
    MoveId disabled_move = MOVE_NONE;
    uint8_t encore_turns = 0;
    MoveId encored_move = MOVE_NONE;
    uint16_t substitute_hp = 0;
    uint8_t perish_count = 0;
    bool is_transformed = false;
    
    // Helpers
    bool is_fainted() const { return stats.hp <= 0; }
    bool can_use_move(size_t slot) const;
    bool has_volatile(VolatileStatus vs) const;
    void set_volatile(VolatileStatus vs);
    void clear_volatile(VolatileStatus vs);
};

// ============================================================================
// Battle Actions
// ============================================================================

enum class BattleActionType {
    Fight,
    Item,
    Switch,
    Run
};

struct ActionFight {
    size_t move_slot;
    size_t target;      // Target pokemon index
};

struct ActionItem {
    ItemId item;
    size_t target;      // Target pokemon (for healing items) or slot
};

struct ActionSwitch {
    size_t party_slot;
};

struct ActionRun {};

using BattleAction = std::variant<ActionFight, ActionItem, ActionSwitch, ActionRun>;

// ============================================================================
// Battle Result
// ============================================================================

enum class BattleResult {
    InProgress,
    PlayerWin,
    PlayerLose,
    PlayerRan,
    Draw,           // Rare (both faint same turn)
    Captured        // Wild pokemon caught
};

// Detailed outcome
struct BattleOutcome {
    BattleResult result;
    
    // Experience/rewards
    uint32_t exp_gained = 0;
    uint32_t money_gained = 0;
    std::vector<std::pair<size_t, uint32_t>> exp_per_pokemon;  // party_index -> exp
    
    // Capture info (if applicable)
    SpeciesId captured_species = SPECIES_NONE;
    
    // Stats
    uint16_t turns_taken = 0;
    uint16_t damage_dealt = 0;
    uint16_t damage_received = 0;
};

// ============================================================================
// Battle State
// ============================================================================

enum class BattleType {
    Wild,
    Trainer,
    // Safari,      // Not in Crystal
    // Double,      // Not in Gen 2
};

// Field effects
struct FieldState {
    Weather weather = Weather::None;
    uint8_t weather_turns = 0;      // 0 = indefinite
    
    // Entry hazards (Gen 2 only has Spikes)
    bool spikes_player = false;
    bool spikes_opponent = false;
    
    // Reflect/Light Screen
    uint8_t reflect_player = 0;     // Turns remaining
    uint8_t light_screen_player = 0;
    uint8_t reflect_opponent = 0;
    uint8_t light_screen_opponent = 0;
    
    // Safeguard
    uint8_t safeguard_player = 0;
    uint8_t safeguard_opponent = 0;
};

// Battle context
class Battle {
public:
    Battle(BattleType type, Party& player_party, const Registries& reg);
    ~Battle();
    
    // Setup
    void set_wild_pokemon(SpeciesId species, uint8_t level);
    void set_trainer(TrainerId trainer, const TrainerData& data);
    
    // Get current pokemon
    BattlePokemon& player_pokemon();
    BattlePokemon& opponent_pokemon();
    const BattlePokemon& player_pokemon() const;
    const BattlePokemon& opponent_pokemon() const;
    
    // Actions
    void set_player_action(BattleAction action);
    void set_opponent_action(BattleAction action);  // Or let AI decide
    
    // Turn execution
    void execute_turn();
    
    // State queries
    BattleType type() const { return type_; }
    BattleResult result() const { return result_; }
    const BattleOutcome& outcome() const { return outcome_; }
    const FieldState& field() const { return field_; }
    uint16_t turn_number() const { return turn_number_; }
    
    // Switching
    bool can_switch_player() const;
    bool can_switch_opponent() const;
    std::vector<size_t> available_switches_player() const;
    void force_switch_player(size_t party_slot);
    void force_switch_opponent(size_t party_slot);
    
    // Running
    bool can_run() const;
    bool attempt_run();
    
    // Capture (wild only)
    bool can_capture() const;
    bool attempt_capture(ItemId ball);
    
    // Item usage
    bool can_use_item(ItemId item) const;
    void use_item(ItemId item, size_t target);
    
    // Callbacks for UI/animation
    using MessageCallback   = std::function<void(const std::string&)>;
    using AnimationCallback = std::function<void(uint8_t anim_id, size_t user, size_t target)>;
    using HpChangeCallback  = std::function<void(size_t pokemon, int16_t old_hp, int16_t new_hp)>;
    using StatusCallback    = std::function<void(size_t pokemon, Status old_status, Status new_status)>;
    using FaintCallback     = std::function<void(size_t pokemon)>;
    using SwitchCallback    = std::function<void(size_t side, size_t old_slot, size_t new_slot)>;
    
    void set_message_callback(MessageCallback cb)   { on_message_    = std::move(cb); }
    void set_animation_callback(AnimationCallback cb){ on_animation_  = std::move(cb); }
    void set_hp_change_callback(HpChangeCallback cb) { on_hp_change_  = std::move(cb); }
    void set_status_callback(StatusCallback cb)      { on_status_change_ = std::move(cb); }
    void set_faint_callback(FaintCallback cb)        { on_faint_      = std::move(cb); }
    void set_switch_callback(SwitchCallback cb)      { on_switch_     = std::move(cb); }
    
    // RNG wiring: production code supplies draws from GameState::rng.
    // Without a callback, a fallback seeded mt19937 is used (unit tests only).
    void set_rng_callback(std::function<uint32_t()> rng_fn);

    // Registry access for AI and other consumers
    const Registries& registries() const { return registries_; }

private:
    BattleType type_;
    BattleResult result_ = BattleResult::InProgress;
    BattleOutcome outcome_;
    
    // References
    Party& player_party_;
    const Registries& registries_;
    
    // Active pokemon
    BattlePokemon player_pokemon_;
    BattlePokemon opponent_pokemon_;
    
    // Opponent party (trainer battles)
    std::vector<BattlePokemon> opponent_party_;
    size_t opponent_active_index_ = 0;
    
    // Trainer info
    std::optional<TrainerId> trainer_id_;
    std::unique_ptr<ITrainerAI> trainer_ai_;
    
    // Field state
    FieldState field_;
    
    // Turn state
    uint16_t turn_number_ = 0;
    BattleAction player_action_;
    BattleAction opponent_action_;
    uint8_t run_attempts_ = 0;
    
    // NOTE: When battle system is implemented, RNG must be consumed from
    // GameState::rng to maintain deterministic save/restore.
    // Do NOT add std::mt19937 rng_ here - it violates Audit 7 determinism.
    
    // Callbacks
    MessageCallback on_message_;
    AnimationCallback on_animation_;
    HpChangeCallback on_hp_change_;
    StatusCallback on_status_change_;
    FaintCallback on_faint_;
    SwitchCallback on_switch_;
    
    // Turn execution helpers
    void determine_turn_order();
    void execute_action(BattlePokemon& user, BattlePokemon& target, 
                       const BattleAction& action, bool is_player);
    void execute_move(BattlePokemon& user, BattlePokemon& target, MoveId move,
                      size_t move_slot, bool user_is_player);
    void apply_end_of_turn_effects();
    void apply_residual(BattlePokemon& bp, bool is_player);
    void check_fainted();
    void finalize_outcome();
    void apply_stat_stages(BattlePokemon& bp);
    // build_ai_context() is defined in battle.cpp (returns AIContext from trainer_ai.hpp)
    
    // Damage calculation
    int32_t calculate_damage(const BattlePokemon& attacker, 
                            const BattlePokemon& defender,
                            const MoveData& move);
    
    // Experience calculation
    uint32_t calculate_exp(const BattlePokemon& defeated, bool is_trainer) const;
    
    // Internal event notification helpers
    void message(const std::string& msg);
    void animate(uint8_t anim_id, size_t user, size_t target);
    void hp_change(size_t pokemon, int16_t old_hp, int16_t new_hp);
    void fainted(size_t pokemon);
    void switched(size_t side, size_t old_slot, size_t new_slot);
    
    // Internal RNG wrapper.
    // Production: caller sets rng_.callback from GameState::rng before each turn.
    // Tests: fallback mt19937 is used (does NOT touch GameState).
    struct BattleRng {
        std::mt19937 fallback{12345u};
        std::function<uint32_t()> callback;
        uint32_t next() { return callback ? callback() : fallback(); }
        uint8_t  next_byte() { return static_cast<uint8_t>(next() & 0xFF); }
    } rng_;

    // Turn state
    bool player_goes_first_ = true;
};

} // namespace enginemon
