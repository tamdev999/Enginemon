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
#include "engine/battle/battle_rules.hpp"
#include "engine/battle/semantic_program.hpp"
#include <memory>
#include <vector>
#include <optional>
#include <functional>
#include <variant>
#include <random>
#include <stdexcept>

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
    uint32_t volatile_status = 0;   // Bitmask of VolatileStatus (widened for Architecture B)

    // Held item
    ItemId held_item = ITEM_NONE;

    // Battle-specific state (Architecture A)
    MoveId last_move_used = MOVE_NONE;
    uint8_t protect_counter = 0;
    uint8_t disable_turns = 0;
    MoveId disabled_move = MOVE_NONE;
    uint8_t encore_turns = 0;
    MoveId encored_move = MOVE_NONE;
    uint16_t substitute_hp = 0;
    uint8_t perish_count = 0;
    bool is_transformed = false;
    uint8_t happiness = 255;    // Gen 2 friendship (0–255). Populated from Pokemon::friendship
                                // at make_battle_pokemon time. Default=255 for wild Pokémon.
                                // Used by Return (power = happiness×10/25) / Frustration.
    uint8_t recharge_turns = 0; // Hyper Beam recharge turns remaining

    // DVs — stored for Hidden Power type/power calculation and Transform backup.
    // Populated from Pokemon::dvs at make_battle_pokemon / force_switch_player time.
    // Wild Pokémon receive random DVs; trainer Pokémon use TrainerClassDVs.
    uint8_t dv_atk = 9;
    uint8_t dv_def = 8;
    uint8_t dv_spd = 8;
    uint8_t dv_spc = 8;

    // ── Architecture B persistent per-combatant state ─────────────────────────
    // These fields are set/read by execute_program() and the engine hooks.
    // They do not affect Architecture A execution paths.

    // Multi-turn counter — shared across Bide, Rampage (mutually exclusive via volatile bits).
    // Trap uses trap_turns separately since Trap can coexist with Rampage/Bide on the victim.
    uint8_t  turn_counter       = 0;

    // Bide accumulated damage (incoming damage while SUBSTATUS_BIDE is set).
    uint16_t bide_stored        = 0;

    // Future Sight scheduled damage (stored at cast time, fired after 3 turns).
    // Note: Future Sight state is per-side, not per-combatant — stored on Battle.
    // This field is unused; see Battle::player_future_sight / opponent_future_sight.

    // Escalating power chain counters (separate: both can coexist on same combatant
    // if a ROM hack has a move using both; Crystal vanilla they are mutually exclusive,
    // but sharing one field risks a ROM-hack regression).
    uint8_t  rollout_count      = 0;   // Rollout: 1..5, resets on miss
    uint8_t  fury_cutter_count  = 0;   // FuryCutter: 1..5, resets on miss

    // Trapping state (on the victim).
    // A Pokémon can be trapped while also rampaging (different combinats).
    uint8_t  trap_turns         = 0;
    MoveId   trapping_move      = MOVE_NONE;

    // Damage received this turn — reset at turn start.
    // Feeds Counter (Physical filter) and MirrorCoat (Special filter).
    uint16_t damage_received_this_turn = 0;
    uint8_t  damage_category_received  = 0;  // 0=Physical, 1=Special, 2=None

    // Rage accumulator — incremented each time this combatant is hit while RAGE volatile.
    // Drives ScalePower(BDamageSource::EscalatingChain) for Rage move.
    uint8_t  rage_accumulator   = 0;

    // Hit loop remaining count — set by InitCounter(HitLoop) at start of multi-hit loop.
    uint8_t  hit_loop_remaining = 0;
    uint8_t  beat_up_index      = 0;  // BeatUp: which party member is currently hitting

    // Pursuit interception flag — set by ActionSwitch resolution, read by PursuitCheck hook.
    bool     is_switching       = false;

    // Transform backup DVs — stored when Transform fires, restored on switch-out.
    uint16_t backup_dvs         = 0;  // packs {atk:4|def:4} in byte 0, {spd:4|spc:4} in byte 1

    // Mimic: store the original move for the mimicked slot so it can be restored.
    uint8_t  mimic_slot         = 0xFF;  // 0xFF = no active Mimic
    MoveId   mimic_original_move = MOVE_NONE;
    uint8_t  mimic_original_pp   = 0;

    // Gender ratio — used by Attract to determine gender compatibility.
    // Copied from SpeciesData::gender_ratio at make_battle_pokemon time.
    // 255 = genderless; 0 = always male; 254 = always female; other = threshold.
    // Female if gender_dv <= gender_ratio, where gender_dv = (dv_atk<<4)|dv_spd.
    uint8_t  gender_ratio       = 255;  // default genderless

    // Per-turn state for new A/B mechanics (not already in the existing fields above)
    uint8_t  protect_consecutive = 0;   // consecutive Protect/Endure uses (halves success each time)
    uint32_t payday_coins        = 0;   // Pay Day coins accumulated this battle (per-attacker side)

    // Confusion turn counter — counts down from initial value each turn the mon is confused.
    // Initialized to (BattleRandom & 3) + 2 = 2–5 when confusion is applied.
    // Decremented at the start of the confused mon's turn; cleared when it reaches 0.
    uint8_t  confusion_turns     = 0;

    // Freeze guard: set to true when this mon is frozen this turn (same-turn freeze).
    // Prevents natural thaw (HandleDefrost) from triggering on the same turn the freeze was
    // applied. Cleared at the start of apply_end_of_turn_effects (HandleDefrost runs there).
    // Source: wPlayerJustGotFrozen / wEnemyJustGotFrozen in Crystal core.asm.
    bool     freeze_guard        = false;

    // Destiny Bond: cleared at start of user's NEXT turn (before any move executes).
    // The VolatileStatus::DestinyBond bit on BattlePokemon IS the state.
    // EndUserDestinyBond clears it at turn start; CheckFaint-path fires it on direct-damage KO.

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

    // Architecture B: Future Sight per-side state.
    // In Crystal, wPlayerFutureSightCount/Damage are side-scoped, not mon-scoped.
    // They persist when the caster switches or faints.
    struct FutureSightState {
        uint8_t  turns  = 0;   // Countdown: 3 → 2 → 1 → fire (0 = inactive)
        uint16_t damage = 0;   // Pre-computed damage stored at cast time
    };
    FutureSightState player_future_sight;
    FutureSightState opponent_future_sight;
};

// Result of executing a single move — allows callers to distinguish outcomes.
enum class MoveExecutionResult {
    Success,             // Move executed normally (damage dealt or effect applied)
    Miss,                // Move failed accuracy check
    Immune,              // Target immune to move type
    NoTarget,            // No valid target
    NoPP,                // Out of PP (struggle not yet implemented)
    ActorSkipped,        // Actor's turn consumed without executing a move (recharge, etc.);
                         // battle turn continues — second actor still acts
    UnsupportedSemantic, // Move effect not implemented; PP NOT deducted; turn halted
    InvalidData          // Malformed/missing move data (accuracy==0, etc.); turn halted
};

// Battle context
class Battle {
public:    // Production constructor: BattleRules are mandatory and non-nullable.
    // This is the only constructor that permits execute_turn() to run.
    Battle(BattleType type, Party& player_party, const Registries& reg,
           const BattleRules& rules);

    // Test constructor: no BattleRules provided.
    // execute_turn() will still assert rules_ != nullptr in debug builds;
    // in release builds execute_turn() throws if rules_ is null.
    // Tests that don't need full execution (calculator unit tests) can use this.
    [[deprecated("use Battle(type, party, reg, rules) in production")]]
    Battle(BattleType type, Party& player_party, const Registries& reg);

    // Kept for test compatibility: override rules after construction.
    // In production the constructor-supplied rules are authoritative.
    void set_battle_rules(const BattleRules* rules) { rules_ = rules; }

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

    // Draw one byte from the battle RNG — used by AI and test harnesses.
    uint8_t rng_byte() { return rng_.next_byte(); }

    // ── Test helpers ─────────────────────────────────────────────────────────
    // These are used only in battle_rom_test.cpp and oracle tests.
    // They are NOT part of the runtime API.

    // Push a BattlePokemon as an additional slot in the opponent party.
    // Used to set up multi-mon opponent parties for Spikes/switch tests.
    void push_opponent_party_slot(const BattlePokemon& bp) {
        opponent_party_.push_back(bp);
    }

    // Set the entry hazard (Spikes) flags directly.
    // Used to test entry-hazard application without executing a full battle setup.
    void set_field_spikes(bool player_side, bool opponent_side) {
        field_.spikes_player   = player_side;
        field_.spikes_opponent = opponent_side;
    }

    // Set field weather directly.
    // Used to test weather-conditional moves (Morning Sun, Synthesis, Moonlight)
    // without executing a full weather-setup turn.
    void set_field_weather(Weather w) {
        field_.weather = w;
    }

    // Registry access for AI and other consumers
    const Registries& registries() const { return registries_; }

private:
    BattleType type_;
    BattleResult result_ = BattleResult::InProgress;
    BattleOutcome outcome_;

    // References
    Party& player_party_;
    const Registries& registries_;
    const BattleRules* rules_ = nullptr;  // Non-owning; set via set_battle_rules()

    // Active pokemon
    BattlePokemon player_pokemon_;
    BattlePokemon opponent_pokemon_;

    // Opponent party (trainer battles)
    std::vector<BattlePokemon> opponent_party_;
    size_t opponent_active_index_ = 0;

    // Trainer info
    std::optional<TrainerId> trainer_id_;
    size_t trainer_class_index_ = 0;   // Index into rules_->trainer_class_ai for this trainer
    uint8_t last_trainer_party_level_ = 0;  // Level of last-parsed party mon (Crystal ComputeTrainerReward)
    std::unique_ptr<ITrainerAI> trainer_ai_;

    // Field state
    FieldState field_;

    // Turn state
    uint16_t turn_number_ = 0;
    BattleAction player_action_;
    BattleAction opponent_action_;
    uint8_t run_attempts_ = 0;
    bool turn_halted_ = false;  // Set on UnsupportedSemantic to skip second actor

    // Architecture B: transient battle-scoped state
    uint32_t player_payday_coins_ = 0;   // accumulated Pay Day coins for player this battle
    uint32_t opponent_payday_coins_ = 0; // accumulated Pay Day coins for opponent this battle

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
    MoveExecutionResult execute_move(BattlePokemon& user, BattlePokemon& target, MoveId move,
                      size_t move_slot, bool user_is_player);
    // Continuation of execute_move: handles all logic after type-immunity check.
    // Split out to avoid MSVC ICE on large functions.
    // pre_crit: whether the critical hit was pre-rolled in execute_move (Crystal order).
    // pre_variation: the accepted variation byte, already rrca-accepted (0 = not pre-rolled).
    MoveExecutionResult execute_move_damaging(BattlePokemon& user, BattlePokemon& target,
                      const MoveData* md, size_t move_slot, bool user_is_player,
                      const SemanticEffectDescription& effective_desc,
                      TypeId initial_effective_move_type, uint16_t type_eff,
                      uint8_t computed_power,
                      bool pre_crit, uint8_t pre_variation);
    void apply_end_of_turn_effects();
    void apply_residual(BattlePokemon& bp, bool is_player);
    void check_fainted();
    void finalize_outcome();
    void apply_stat_stages(BattlePokemon& bp);
    // Secondary-effect helpers (used by execute_move)
    void apply_secondary_effect(BattlePokemon& user, BattlePokemon& target,
                                enginemon::SecondaryEffectType effect, bool user_is_player);
    void apply_one_stage_change(BattlePokemon& mon, int stat_idx, int8_t delta);
    void apply_stat_change(BattlePokemon& user, BattlePokemon& target,
                           enginemon::StatChangeTarget change, bool user_is_player);
    // build_ai_context() is defined in battle.cpp (returns AIContext from trainer_ai.hpp)

    // Architecture B: execute a SemanticEffectProgram (called from execute_move when
    // MoveData::has_program is true).
    MoveExecutionResult execute_program(BattlePokemon& user, BattlePokemon& target,
                                        const MoveData& md, size_t move_slot,
                                        bool user_is_player);

    // Architecture B: engine hooks — called from execute_turn infrastructure.
    void hook_on_damage_received(BattlePokemon& defender, int32_t damage,
                                  uint8_t move_category);  // category: 0=Phys 1=Spec
    void hook_future_sight_tick();
    void hook_trap_damage_tick();
    void hook_rampage_end_check(BattlePokemon& combatant, bool is_player);
    bool hook_bide_gate(BattlePokemon& combatant, bool is_player);  // true = suppress move
    void hook_chain_reset(BattlePokemon& combatant);
    MoveExecutionResult hook_pursuit_check(BattlePokemon& pursuer, BattlePokemon& switcher,
                                            bool pursuer_is_player);
    // New hooks added for extended mechanics:
    void hook_end_of_turn_leech_seed();   // drain 1/8 max_hp from Seeded mons
    void hook_end_of_turn_nightmare();    // drain 1/4 max_hp from Nightmare mons while asleep
    void hook_end_of_turn_curse();        // drain 1/4 max_hp from Cursed (Ghost) mons
    void hook_end_of_turn_perish_song();  // decrement perish counts; faint at 0
    void hook_pre_move_clear_protect(BattlePokemon& current_actor);  // clear Protect/Endure for current actor only
    void hook_pre_move_clear_destiny_bond(BattlePokemon& user);     // clear DestinyBond for current actor only
    // Check if PreMove hook blocks the combatant from acting (Disable, Encore, Attract 50%).
    // Returns true if the move is blocked, false if execution should proceed.
    bool hook_pre_move_check(BattlePokemon& user, BattlePokemon& target,
                              size_t move_slot, bool user_is_player);
    // Natural thaw: called from apply_end_of_turn_effects. Returns true if the combatant thawed.
    bool hook_end_of_turn_natural_thaw(BattlePokemon& bp, bool is_player);
    // DestinyBond CheckFaint path: if user has DestinyBond and target's HP just hit 0 from
    // direct damage, faint the DestinyBond user too. Called from execute_program Damage case.
    void hook_destiny_bond_check(BattlePokemon& destiny_bond_user, BattlePokemon& killer,
                                  bool killer_is_player);

    // SetVolatile handler extracted to keep execute_program within MSVC size limits.
    // Returns 0 = continue executing ops; 1 = return MoveExecutionResult::Success (deferred);
    // 2 = return MoveExecutionResult::Miss (failed).
    int execute_program_set_volatile(const BOp& op, BattlePokemon& user, BattlePokemon& target,
                                      const MoveData& md, bool user_is_player);
    // King's Rock shared helper: one roll per move, used by both A-path and B-path.
    // Source: Crystal BattleCommand_HeldFlinch (kingsrock command, 0x4D).
    // Fires when: move_desc.needs_kingsrock, user holds PostHitFlinch item,
    // and target is NOT behind an intact Substitute.
    // If the roll fires, sets VolatileStatus::Flinch on target.
    // Target faint does NOT suppress the roll (outside hit loops in Crystal scripts).
    void apply_kings_rock(const SemanticEffectDescription& move_desc,
                          BattlePokemon& user, BattlePokemon& target);
    // End-of-turn held item effects, split per Crystal HandleBetweenTurnEffects ordering.
    // Pre-thaw: Leftovers (EndTurnHealFraction), Mysteryberry (EndTurnRestorePP).
    // Runs before HandleDefrost (natural thaw).
    void apply_held_item_pre_thaw(BattlePokemon& bp, bool is_player);
    // Post-thaw: HP berries, status-cure berries, MiracleBerry, Bitter Berry.
    // Runs after HandleDefrost (natural thaw).
    void apply_held_item_post_thaw(BattlePokemon& bp, bool is_player);
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
