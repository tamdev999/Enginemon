// engine/battle/battle.cpp
// Gen 2 battle system — turn-based Pokemon battles
//
// Architecture note:
//   Battle owns no renderer, no Lua, no ROM references.
//   RNG comes from GameState::rng via the callback set by the caller.
//   If no callback is set (unit test mode), a seeded mt19937 fallback is used
//   that does NOT touch GameState::rng.
//
// Turn flow (source: suiCune core.c):
//   1. Determine turn order (priority → speed → random tie)
//   2. First actor executes action
//   3. Check faint after first action
//   4. If both still alive: second actor executes action
//   5. Check faint after second action
//   6. End-of-turn effects (weather, screens, residual)
//   7. Repeat next turn

#include "engine/battle/battle.hpp"
#include "engine/battle/calculator.hpp"
#include "engine/battle/trainer_ai.hpp"
#include "engine/core/registry.hpp"
#include "engine/party/party.hpp"
#include <algorithm>
#include <cassert>
#include <stdexcept>

// Suppress [[deprecated]] warnings in this file: battle.cpp calls the fallback
// (no-rules) overloads only when rules_ == nullptr (unit test path).
// Production path always has rules_ set and calls the BattleRules overloads.
#pragma warning(push)
#pragma warning(disable: 4996)   // MSVC: suppress deprecated function warnings

namespace enginemon {

// ============================================================================
// BattlePokemon helpers
// ============================================================================

bool BattlePokemon::can_use_move(size_t slot) const {
    if (slot >= 4) return false;
    const auto& m = moves[slot];
    if (m.move == MOVE_NONE) return false;
    if (m.pp == 0) return false;
    return true;
}

bool BattlePokemon::has_volatile(VolatileStatus vs) const {
    return (volatile_status & static_cast<uint16_t>(vs)) != 0;
}

void BattlePokemon::set_volatile(VolatileStatus vs) {
    volatile_status |= static_cast<uint16_t>(vs);
}

void BattlePokemon::clear_volatile(VolatileStatus vs) {
    volatile_status &= ~static_cast<uint16_t>(vs);
}

// ============================================================================
// Helper: build a BattlePokemon from species/level
// ============================================================================

static BattlePokemon make_battle_pokemon(
    SpeciesId species, uint8_t level,
    ItemId held, const std::array<MoveId, 4>& move_ids,
    size_t party_index,
    const Registries& reg)
{
    BattlePokemon bp{};
    bp.party_index = party_index;
    bp.species     = species;
    bp.level       = level;
    bp.held_item   = held;

    const SpeciesData* sd = reg.species.get(species);
    if (sd) {
        bp.type1 = sd->type1;
        bp.type2 = sd->type2;

        // Stats: use trainer default IV=9 for trainer mons
        const auto& bs = sd->base_stats;
        const uint8_t iv = 9;
        bp.stats.max_hp          = static_cast<int16_t>(calc_hp(bs.hp, iv, 0, level));
        bp.stats.hp              = bp.stats.max_hp;
        bp.stats.attack          = static_cast<int16_t>(calc_stat(bs.attack, iv, 0, level));
        bp.stats.defense         = static_cast<int16_t>(calc_stat(bs.defense, iv, 0, level));
        bp.stats.speed           = static_cast<int16_t>(calc_stat(bs.speed, iv, 0, level));
        bp.stats.special_attack  = static_cast<int16_t>(calc_stat(bs.special_attack, iv, 0, level));
        bp.stats.special_defense = static_cast<int16_t>(calc_stat(bs.special_defense, iv, 0, level));
        bp.base_stats            = bp.stats;
    }

    for (size_t i = 0; i < 4; ++i) {
        bp.moves[i].move = move_ids[i];
        if (move_ids[i] != MOVE_NONE) {
            const MoveData* md = reg.moves.get(move_ids[i]);
            bp.moves[i].max_pp = md ? md->pp : 0;
            bp.moves[i].pp     = bp.moves[i].max_pp;
        }
    }
    return bp;
}

// ============================================================================
// Battle construction / destruction
// ============================================================================

Battle::Battle(BattleType type, Party& player_party, const Registries& reg)
    : type_(type)
    , player_party_(player_party)
    , registries_(reg)
{}

Battle::~Battle() = default;

// ============================================================================
// Setup
// ============================================================================

void Battle::set_wild_pokemon(SpeciesId species, uint8_t level) {
    assert(type_ == BattleType::Wild);

    std::array<MoveId, 4> moves{MOVE_NONE, MOVE_NONE, MOVE_NONE, MOVE_NONE};
    const SpeciesData* sd = registries_.species.get(species);
    if (sd) {
        size_t slot = 0;
        // Assign the most-recently-learned moves up to this level (Crystal behavior)
        for (auto it = sd->learnset.rbegin(); it != sd->learnset.rend() && slot < 4; ++it) {
            if (it->first <= level) {
                moves[slot++] = it->second;
            }
        }
    }

    opponent_pokemon_ = make_battle_pokemon(species, level, ITEM_NONE, moves, 0, registries_);
}

void Battle::set_trainer(TrainerId trainer, const TrainerData& data) {
    assert(type_ == BattleType::Trainer);
    trainer_id_ = trainer;

    opponent_party_.clear();
    for (size_t i = 0; i < data.party.size(); ++i) {
        const auto& tp = data.party[i];
        opponent_party_.push_back(
            make_battle_pokemon(tp.species, tp.level, tp.held_item, tp.moves, i, registries_));
    }

    if (!opponent_party_.empty()) {
        opponent_active_index_ = 0;
        opponent_pokemon_ = opponent_party_[0];
    }

    // Determine AI behavior from trainer class via BattleRules.
    // Source: AIChooseMove — TrainerClassAttributes bitmask → AI layer selection.
    // ai_move_flags bits: 0=BASIC 1=SETUP 2=TYPES 3=OFFENSIVE 4=SMART 5..=higher
    // Map to VanillaAI behavior IDs (which encode multi-pass combinations).
    AIBehaviorId ai_behavior = VanillaAI::BASIC;
    if (rules_) {
        const uint16_t flags = rules_->get_trainer_ai_flags(data.trainer_class);
        // Bit 4 (SMART) → GYM_LEADER tier (BASIC+TYPES+SMART+SETUP)
        // Bit 3 (OFFENSIVE) → AGGRESSIVE tier (BASIC+TYPES+OFFENSIVE)
        // Bit 1 (SETUP) → DEFENSIVE tier (BASIC+TYPES+SETUP) if no offensive
        // Bit 0 (BASIC only) → BASIC
        // Use highest-tier flag present.
        constexpr uint16_t AI_BASIC     = 1 << 0;
        constexpr uint16_t AI_SETUP     = 1 << 1;
        constexpr uint16_t AI_OFFENSIVE = 1 << 3;
        constexpr uint16_t AI_SMART     = 1 << 4;
        if (flags & AI_SMART)          ai_behavior = VanillaAI::GYM_LEADER;
        else if (flags & AI_OFFENSIVE) ai_behavior = VanillaAI::AGGRESSIVE;
        else if (flags & AI_SETUP)     ai_behavior = VanillaAI::DEFENSIVE;
        else if (flags & AI_BASIC)     ai_behavior = VanillaAI::BASIC;
    }
    trainer_ai_ = std::make_unique<VanillaCrystalAI>(ai_behavior);
}

// ============================================================================
// Accessors
// ============================================================================

BattlePokemon&       Battle::player_pokemon()        { return player_pokemon_; }
BattlePokemon&       Battle::opponent_pokemon()       { return opponent_pokemon_; }
const BattlePokemon& Battle::player_pokemon()  const  { return player_pokemon_; }
const BattlePokemon& Battle::opponent_pokemon() const { return opponent_pokemon_; }

// ============================================================================
// Actions
// ============================================================================

void Battle::set_player_action(BattleAction action)   { player_action_   = std::move(action); }
void Battle::set_opponent_action(BattleAction action)  { opponent_action_  = std::move(action); }

// ============================================================================
// Stat stage application
// ============================================================================

void Battle::apply_stat_stages(BattlePokemon& bp) {
    if (rules_) {
        bp.stats.attack          = static_cast<int16_t>(apply_stat_stage(bp.base_stats.attack,          bp.stages.attack,          *rules_));
        bp.stats.defense         = static_cast<int16_t>(apply_stat_stage(bp.base_stats.defense,         bp.stages.defense,         *rules_));
        bp.stats.speed           = static_cast<int16_t>(apply_stat_stage(bp.base_stats.speed,           bp.stages.speed,           *rules_));
        bp.stats.special_attack  = static_cast<int16_t>(apply_stat_stage(bp.base_stats.special_attack,  bp.stages.special_attack,  *rules_));
        bp.stats.special_defense = static_cast<int16_t>(apply_stat_stage(bp.base_stats.special_defense, bp.stages.special_defense, *rules_));
    } else {
        bp.stats.attack          = static_cast<int16_t>(apply_stat_stage(bp.base_stats.attack,          bp.stages.attack));
        bp.stats.defense         = static_cast<int16_t>(apply_stat_stage(bp.base_stats.defense,         bp.stages.defense));
        bp.stats.speed           = static_cast<int16_t>(apply_stat_stage(bp.base_stats.speed,           bp.stages.speed));
        bp.stats.special_attack  = static_cast<int16_t>(apply_stat_stage(bp.base_stats.special_attack,  bp.stages.special_attack));
        bp.stats.special_defense = static_cast<int16_t>(apply_stat_stage(bp.base_stats.special_defense, bp.stages.special_defense));
    }
    // HP is never stage-modified
}

// ============================================================================
// Turn order determination
// Source: suiCune core.c DetermineMoveOrder
// ============================================================================

void Battle::determine_turn_order() {
    player_goes_first_ = true;

    bool p_switch = std::holds_alternative<ActionSwitch>(player_action_);
    bool o_switch = std::holds_alternative<ActionSwitch>(opponent_action_);

    // Switches before Fight actions
    if (p_switch && !o_switch) { player_goes_first_ = true;  return; }
    if (!p_switch && o_switch) { player_goes_first_ = false; return; }

    if (!std::holds_alternative<ActionFight>(player_action_) ||
        !std::holds_alternative<ActionFight>(opponent_action_)) {
        return; // Non-fight/non-switch actions; player first by default
    }

    const ActionFight& pf = std::get<ActionFight>(player_action_);
    const ActionFight& of = std::get<ActionFight>(opponent_action_);

    // Move priority
    int8_t p_prio = 0, o_prio = 0;
    if (pf.move_slot < 4 && player_pokemon_.moves[pf.move_slot].move != MOVE_NONE) {
        const MoveData* md = registries_.moves.get(player_pokemon_.moves[pf.move_slot].move);
        if (md) p_prio = md->priority;
    }
    if (of.move_slot < 4 && opponent_pokemon_.moves[of.move_slot].move != MOVE_NONE) {
        const MoveData* md = registries_.moves.get(opponent_pokemon_.moves[of.move_slot].move);
        if (md) o_prio = md->priority;
    }
    if (p_prio != o_prio) { player_goes_first_ = (p_prio > o_prio); return; }

    // Speed
    int32_t p_spd, o_spd;
    if (rules_) {
        p_spd = apply_stat_stage(player_pokemon_.base_stats.speed,   player_pokemon_.stages.speed,   *rules_);
        o_spd = apply_stat_stage(opponent_pokemon_.base_stats.speed, opponent_pokemon_.stages.speed, *rules_);
    } else {
        p_spd = apply_stat_stage(player_pokemon_.base_stats.speed,   player_pokemon_.stages.speed);
        o_spd = apply_stat_stage(opponent_pokemon_.base_stats.speed, opponent_pokemon_.stages.speed);
    }
    if (p_spd != o_spd) { player_goes_first_ = (p_spd > o_spd); return; }

    // Tie: random 50/50
    player_goes_first_ = (rng_.next_byte() < 128);
}

// ============================================================================
// Turn execution
// ============================================================================

void Battle::execute_turn() {
    if (result_ != BattleResult::InProgress) return;

    ++turn_number_;

    // AI decides for opponent
    if (type_ == BattleType::Trainer && trainer_ai_) {
        if (!std::holds_alternative<ActionFight>(opponent_action_) &&
            !std::holds_alternative<ActionSwitch>(opponent_action_)) {
            // Build AI context inline
            AIContext ctx{*this, opponent_pokemon_, player_pokemon_,
                true, can_switch_opponent(), false, false, 0, {}, {}};
            for (size_t i = 0; i < opponent_party_.size(); ++i) {
                if (!opponent_party_[i].is_fainted() && i != opponent_active_index_) {
                    ctx.usable_party_count++;
                    ctx.available_switches.push_back(i);
                }
            }
            // Use ROM-derived AI if BattleRules available; fallback otherwise
            AIDecision decision = rules_
                ? static_cast<VanillaCrystalAI*>(trainer_ai_.get())->decide(ctx, *rules_)
                : trainer_ai_->decide(ctx);
            opponent_action_ = decision.action;
        }
    } else if (type_ == BattleType::Wild) {
        // Wild: uniform random selection among usable moves.
        // Source: Crystal AIChooseMove — wild uses Random() % num_usable_moves,
        // not per-slot biased selection.
        ActionFight af; af.target = 0; af.move_slot = 0;
        std::vector<size_t> usable;
        for (size_t i = 0; i < 4; ++i) {
            if (opponent_pokemon_.can_use_move(i)) usable.push_back(i);
        }
        if (!usable.empty()) {
            // Uniform selection: draw a byte, take modulo over usable count.
            // For count <= 4 the bias is negligible (max 1/256 error per slot).
            af.move_slot = usable[rng_.next_byte() % usable.size()];
        }
        opponent_action_ = af;
    }

    determine_turn_order();

    BattlePokemon& first  = player_goes_first_ ? player_pokemon_  : opponent_pokemon_;
    BattlePokemon& second = player_goes_first_ ? opponent_pokemon_ : player_pokemon_;
    BattleAction& fa      = player_goes_first_ ? player_action_    : opponent_action_;
    BattleAction& sa      = player_goes_first_ ? opponent_action_  : player_action_;
    const bool first_is_player = player_goes_first_;

    execute_action(first, second, fa, first_is_player);
    check_fainted();
    if (result_ != BattleResult::InProgress) { finalize_outcome(); return; }

    if (!second.is_fainted()) {
        execute_action(second, first, sa, !first_is_player);
        check_fainted();
        if (result_ != BattleResult::InProgress) { finalize_outcome(); return; }
    }

    apply_end_of_turn_effects();
    check_fainted();
    if (result_ != BattleResult::InProgress) finalize_outcome();

    // Clear per-turn actions
    player_action_   = ActionFight{};
    opponent_action_ = ActionFight{};
}

// ============================================================================
// Action execution
// ============================================================================

void Battle::execute_action(BattlePokemon& user, BattlePokemon& target,
                            const BattleAction& action, bool is_player) {
    if (user.is_fainted()) return;

    if (std::holds_alternative<ActionFight>(action)) {
        const ActionFight& af = std::get<ActionFight>(action);
        const MoveId mid = (af.move_slot < 4) ? user.moves[af.move_slot].move : MOVE_NONE;
        if (mid == MOVE_NONE) {
            message(is_player ? "Player has no usable move!" : "Opponent has no usable move!");
            return;
        }
        execute_move(user, target, mid, af.move_slot, is_player);

    } else if (std::holds_alternative<ActionSwitch>(action)) {
        const ActionSwitch& as = std::get<ActionSwitch>(action);
        if (is_player) force_switch_player(as.party_slot);
        else           force_switch_opponent(as.party_slot);

    } else if (std::holds_alternative<ActionItem>(action)) {
        const ActionItem& ai_action = std::get<ActionItem>(action);
        if (is_player) use_item(ai_action.item, ai_action.target);

    } else if (std::holds_alternative<ActionRun>(action)) {
        if (is_player) attempt_run();
    }
}

// ============================================================================
// Move execution
// Source: suiCune effect_commands.c DamageCalc + BattleCommand_Stab + etc.
// ============================================================================

void Battle::execute_move(BattlePokemon& user, BattlePokemon& target,
                          MoveId move_id, size_t move_slot, bool user_is_player) {
    const MoveData* md = registries_.moves.get(move_id);
    if (!md) { message("Unknown move!"); return; }

    // Deduct PP
    if (move_slot < 4 && user.moves[move_slot].pp > 0)
        user.moves[move_slot].pp--;

    message((user_is_player ? "Player used " : "Opponent used ") + md->name + "!");

    if (md->category == MoveCategory::Status) {
        // Status move effects are dispatched separately (future milestone).
        // Explicitly flag that this move had no effect rather than silently no-op.
        message((user_is_player ? "Player used " : "Opponent used ") + md->name +
                " — status effect not yet implemented.");
        return;
    }

    // Accuracy check
    // 0xFF = always hit (Crystal encoding); 0 = always hit (Enginemon normalisation)
    if (md->accuracy != 0xFF && md->accuracy != 0) {
        const int8_t acc_stage = user.stages.accuracy;
        const int8_t eva_stage = target.stages.evasion;
        bool hit;
        if (rules_) {
            hit = roll_accuracy(md->accuracy, acc_stage, eva_stage, rng_.next_byte(), *rules_);
        } else {
            hit = roll_accuracy(md->accuracy, acc_stage, eva_stage, rng_.next_byte());
        }
        if (!hit) {
            message("The attack missed!");
            return;
        }
    }

    // Critical hit — build stage from BattleRules + volatile state
    uint8_t crit_stage = 0;
    if (rules_) {
        crit_stage = build_crit_stage(user, *md, *rules_);
    }
    const bool is_crit = rules_
        ? roll_critical(crit_stage, rng_.next_byte(), *rules_)
        : roll_critical(crit_stage, rng_.next_byte());

    // Type effectiveness
    const uint16_t type_eff = get_combined_effectiveness(
        md->type, target.type1, target.type2, registries_.type_chart);
    if (type_eff == 0) {
        message("It doesn't affect the opposing Pokémon…");
        return;
    }

    // STAB
    const bool stab = (md->type == user.type1 || md->type == user.type2);

    // Attack / defense stats (crit ignores negative atk stages and positive def stages)
    // Source: suiCune effect_commands.c CheckDamageStatsCritical
    const int8_t eff_atk_stage = (is_crit && user.stages.attack < 0)   ? 0 : user.stages.attack;
    const int8_t eff_def_stage = (is_crit && target.stages.defense > 0) ? 0 : target.stages.defense;
    const int8_t eff_satk_stage = (is_crit && user.stages.special_attack < 0)   ? 0 : user.stages.special_attack;
    const int8_t eff_sdef_stage = (is_crit && target.stages.special_defense > 0) ? 0 : target.stages.special_defense;

    int32_t atk_stat, def_stat;
    const bool physical = (md->category == MoveCategory::Physical);

    // Helper lambda: applies stat stage using BattleRules if available
    auto stat_stage = [this](int32_t base, int8_t stage) -> int32_t {
        return rules_ ? apply_stat_stage(base, stage, *rules_)
                      : apply_stat_stage(base, stage);
    };

    if (physical) {
        atk_stat = stat_stage(user.base_stats.attack,   eff_atk_stage);
        def_stat = stat_stage(target.base_stats.defense, eff_def_stage);
        // Reflect doubles defender's defense
        if ( user_is_player && field_.reflect_opponent > 0) def_stat *= 2;
        if (!user_is_player && field_.reflect_player   > 0) def_stat *= 2;
    } else {
        atk_stat = stat_stage(user.base_stats.special_attack,   eff_satk_stage);
        def_stat = stat_stage(target.base_stats.special_defense, eff_sdef_stage);
        // Light Screen doubles defender's special defense
        if ( user_is_player && field_.light_screen_opponent > 0) def_stat *= 2;
        if (!user_is_player && field_.light_screen_player   > 0) def_stat *= 2;
    }

    // Burn penalty on physical moves
    const bool burned = physical && (user.status == Status::Burn);

    DamageParams dp{};
    dp.attacker_level    = user.level;
    dp.attack_stat       = atk_stat;
    dp.defense_stat      = def_stat;
    dp.move_power        = md->power;
    dp.type_effectiveness = type_eff;
    dp.stab              = stab;
    dp.critical          = is_crit;
    dp.burned            = burned;
    dp.weather           = field_.weather;
    dp.move_type         = md->type;

    int32_t damage = enginemon::calculate_damage(dp);
    if (damage == 0) return;  // Immune or power=0 status move edge case

    // Weather modifier (Crystal: DoWeatherModifiers, applied after STAB)
    // Source: misc.asm DoWeatherModifiers — type and move-effect checks.
    if (rules_ && field_.weather != Weather::None) {
        const uint8_t weather_id = static_cast<uint8_t>(field_.weather);
        const uint8_t type_id    = static_cast<uint8_t>(md->type);
        const uint8_t effect_id  = md->effect_id;
        damage = apply_weather_modifier(damage, weather_id, type_id, effect_id, *rules_);
    }

    // Random variation 85-100% (suiCune BattleCommand_DamageVariation: RRCA loop)
    uint8_t variation;
    do {
        uint8_t r = rng_.next_byte();
        variation = (r >> 1) | (r << 7);  // RRCA
    } while (variation < 85);
    damage = damage * variation / 100;
    if (damage < 2) damage = 2;

    // Messages
    if (is_crit)       message("A critical hit!");
    if (type_eff > 100) message("It's super effective!");
    else if (type_eff < 100) message("It's not very effective…");

    animate(md->animation_id,
            user_is_player ? 0u : 1u,
            user_is_player ? 1u : 0u);

    const int16_t old_hp = target.stats.hp;
    target.stats.hp = static_cast<int16_t>(
        std::max(0, static_cast<int32_t>(target.stats.hp) - damage));
    hp_change(user_is_player ? 1u : 0u, old_hp, target.stats.hp);

    outcome_.damage_dealt += static_cast<uint16_t>(damage);
}

// ============================================================================
// End-of-turn effects
// ============================================================================

void Battle::apply_end_of_turn_effects() {
    // Tick down field timers
    if (field_.weather_turns > 0 && --field_.weather_turns == 0) {
        field_.weather = Weather::None;
        message("The weather cleared up!");
    }
    if (field_.reflect_player      > 0) --field_.reflect_player;
    if (field_.reflect_opponent    > 0) --field_.reflect_opponent;
    if (field_.light_screen_player  > 0) --field_.light_screen_player;
    if (field_.light_screen_opponent > 0) --field_.light_screen_opponent;
    if (field_.safeguard_player    > 0) --field_.safeguard_player;
    if (field_.safeguard_opponent  > 0) --field_.safeguard_opponent;

    apply_residual(player_pokemon_,   true);
    apply_residual(opponent_pokemon_, false);
}

void Battle::apply_residual(BattlePokemon& bp, bool is_player) {
    if (bp.is_fainted()) return;

    const char* side = is_player ? "Player" : "Opponent";
    int16_t dmg = 0;

    if (bp.status == Status::Burn) {
        dmg = static_cast<int16_t>(std::max(1, bp.stats.max_hp / 8));
        message(std::string(side) + " is hurt by its burn!");
    } else if (bp.status == Status::Poison) {
        dmg = static_cast<int16_t>(std::max(1, bp.stats.max_hp / 8));
        message(std::string(side) + " is hurt by poison!");
    } else if (bp.status == Status::BadPoison) {
        bp.status_turns++;
        dmg = static_cast<int16_t>(std::max(1, bp.stats.max_hp * bp.status_turns / 16));
        message(std::string(side) + " is badly poisoned!");
    }

    if (dmg > 0) {
        const int16_t old_hp = bp.stats.hp;
        bp.stats.hp = static_cast<int16_t>(std::max(0, static_cast<int32_t>(bp.stats.hp) - dmg));
        hp_change(is_player ? 0u : 1u, old_hp, bp.stats.hp);
    }
}

// ============================================================================
// Faint check
// ============================================================================

void Battle::check_fainted() {
    const bool p_fainted = player_pokemon_.is_fainted();
    const bool o_fainted = opponent_pokemon_.is_fainted();

    if (p_fainted && o_fainted) { result_ = BattleResult::Draw; return; }

    if (o_fainted) {
        fainted(1u);
        const SpeciesData* sd = registries_.species.get(opponent_pokemon_.species);
        if (sd) {
            outcome_.exp_gained += calculate_exp_gain(
                sd->base_exp, opponent_pokemon_.level,
                type_ == BattleType::Trainer, 1);
        }

        if (type_ == BattleType::Wild) {
            result_ = BattleResult::PlayerWin;
        } else {
            // Trainer: auto-switch to next available
            bool more = false;
            for (size_t i = 0; i < opponent_party_.size(); ++i) {
                if (i != opponent_active_index_ && !opponent_party_[i].is_fainted()) {
                    force_switch_opponent(i);
                    more = true;
                    break;
                }
            }
            if (!more) result_ = BattleResult::PlayerWin;
        }
        return;
    }

    if (p_fainted) {
        fainted(0u);
        if (available_switches_player().empty())
            result_ = BattleResult::PlayerLose;
        // Otherwise caller must provide switch action
    }
}

void Battle::finalize_outcome() {
    outcome_.result      = result_;
    outcome_.turns_taken = turn_number_;
    if (result_ == BattleResult::PlayerWin && type_ == BattleType::Trainer) {
        outcome_.money_gained = 100;  // Placeholder
    }
}

// ============================================================================
// Switching
// ============================================================================

bool Battle::can_switch_player() const { return !available_switches_player().empty(); }

bool Battle::can_switch_opponent() const {
    for (size_t i = 0; i < opponent_party_.size(); ++i) {
        if (i != opponent_active_index_ && !opponent_party_[i].is_fainted()) return true;
    }
    return false;
}

std::vector<size_t> Battle::available_switches_player() const {
    std::vector<size_t> result;
    for (size_t i = 0; i < player_party_.size(); ++i) {
        const Pokemon* mon = player_party_.get(i);
        if (!mon) continue;
        if (i != player_pokemon_.party_index && mon->current_hp > 0)
            result.push_back(i);
    }
    return result;
}

void Battle::force_switch_player(size_t party_slot) {
    const Pokemon* mon = player_party_.get(party_slot);
    if (!mon) return;

    const size_t old_slot = player_pokemon_.party_index;

    BattlePokemon bp{};
    bp.party_index = party_slot;
    bp.species     = mon->species;
    bp.level       = mon->level;
    bp.held_item   = mon->held_item;
    bp.status      = mon->status;

    const SpeciesData* sd = registries_.species.get(bp.species);
    if (sd) { bp.type1 = sd->type1; bp.type2 = sd->type2; }

    bp.stats.max_hp          = static_cast<int16_t>(mon->max_hp);
    bp.stats.hp              = static_cast<int16_t>(mon->current_hp);
    bp.stats.attack          = static_cast<int16_t>(mon->attack);
    bp.stats.defense         = static_cast<int16_t>(mon->defense);
    bp.stats.speed           = static_cast<int16_t>(mon->speed);
    bp.stats.special_attack  = static_cast<int16_t>(mon->special_attack);
    bp.stats.special_defense = static_cast<int16_t>(mon->special_defense);
    bp.base_stats = bp.stats;

    for (size_t i = 0; i < 4; ++i) {
        bp.moves[i].move = mon->moves[i].id;
        bp.moves[i].pp   = mon->moves[i].pp;
        const MoveData* md = registries_.moves.get(mon->moves[i].id);
        bp.moves[i].max_pp = md ? md->pp : 0;
    }

    player_pokemon_ = bp;
    switched(0u, old_slot, party_slot);
}

void Battle::force_switch_opponent(size_t party_slot) {
    if (party_slot >= opponent_party_.size()) return;
    const size_t old_slot = opponent_active_index_;
    opponent_party_[old_slot] = opponent_pokemon_;
    opponent_active_index_    = party_slot;
    opponent_pokemon_         = opponent_party_[party_slot];
    opponent_pokemon_.volatile_status = 0;  // Clear volatile on switch
    switched(1u, old_slot, party_slot);
    message("Opponent sent out a new Pokémon!");
}

// ============================================================================
// Running
// ============================================================================

bool Battle::can_run() const { return type_ == BattleType::Wild; }

bool Battle::attempt_run() {
    if (!can_run()) { message("There's no running from a trainer battle!"); return false; }
    run_attempts_++;
    int32_t p_spd, o_spd;
    if (rules_) {
        p_spd = apply_stat_stage(player_pokemon_.base_stats.speed,   player_pokemon_.stages.speed,   *rules_);
        o_spd = apply_stat_stage(opponent_pokemon_.base_stats.speed, opponent_pokemon_.stages.speed, *rules_);
    } else {
        p_spd = apply_stat_stage(player_pokemon_.base_stats.speed,   player_pokemon_.stages.speed);
        o_spd = apply_stat_stage(opponent_pokemon_.base_stats.speed, opponent_pokemon_.stages.speed);
    }
    if (roll_escape(p_spd, o_spd, run_attempts_, rng_.next_byte())) {
        result_ = BattleResult::PlayerRan;
        message("Got away safely!");
        return true;
    }
    message("Can't escape!");
    return false;
}

// ============================================================================
// Capture
// ============================================================================

bool Battle::can_capture() const {
    return type_ == BattleType::Wild && result_ == BattleResult::InProgress;
}

bool Battle::attempt_capture(ItemId ball) {
    if (!can_capture()) return false;

    uint8_t ball_mod = 10;
    const ItemData* id = registries_.items.get(ball);
    if (id && id->held_param > 0) ball_mod = id->held_param;

    CaptureParams cp{};
    cp.catch_rate    = 45;
    cp.ball_modifier = ball_mod;
    cp.max_hp        = opponent_pokemon_.stats.max_hp;
    cp.current_hp    = opponent_pokemon_.stats.hp;
    cp.status        = opponent_pokemon_.status;

    const SpeciesData* sd = registries_.species.get(opponent_pokemon_.species);
    if (sd) cp.catch_rate = sd->catch_rate;

    if (roll_capture(cp, rng_.next_byte(), rng_.next_byte())) {
        const uint16_t final_rate = calculate_catch_value(cp);
        (void)(rules_ ? capture_wobble_chance(final_rate, *rules_)
                      : capture_wobble_chance(final_rate));  // wobble count for animation — not yet rendered
        result_ = BattleResult::Captured;
        outcome_.captured_species = opponent_pokemon_.species;
        message("Gotcha! Pokémon was caught!");
        return true;
    }
    message("Oh no! The Pokémon broke free!");
    return false;
}

// ============================================================================
// Items
// ============================================================================

bool Battle::can_use_item(ItemId item) const {
    return registries_.items.get(item) != nullptr;
}

void Battle::use_item(ItemId item, size_t target) {
    const ItemData* id = registries_.items.get(item);
    if (!id) return;
    message("Player used " + id->name + "!");
    if (id->pocket == ItemPocket::Balls && type_ == BattleType::Wild) {
        attempt_capture(item);
        return;
    }
    (void)target;
    // Generic item effects (healing, etc.) will be dispatched in a future milestone
}

// ============================================================================
// RNG wiring
// ============================================================================

void Battle::set_rng_callback(std::function<uint32_t()> fn) {
    rng_.callback = std::move(fn);
}

// ============================================================================
// Callback helpers
// ============================================================================

void Battle::message(const std::string& msg)                                        { if (on_message_)       on_message_(msg); }
void Battle::animate(uint8_t id, size_t u, size_t t)                               { if (on_animation_)     on_animation_(id, u, t); }
void Battle::hp_change(size_t pokemon, int16_t old_hp, int16_t new_hp)            { if (on_hp_change_)     on_hp_change_(pokemon, old_hp, new_hp); }
void Battle::fainted(size_t pokemon)                                               { if (on_faint_)         on_faint_(pokemon); }
void Battle::switched(size_t side, size_t old_slot, size_t new_slot)              { if (on_switch_)        on_switch_(side, old_slot, new_slot); }

// Private calculate_damage(attacker, defender, move) is declared in battle.hpp for
// potential future override (e.g., custom damage hooks). The current implementation
// delegates entirely to the free-function version via execute_move.
int32_t Battle::calculate_damage(const BattlePokemon& /*atk*/,
                                  const BattlePokemon& /*def*/,
                                  const MoveData&      /*move*/) {
    return 0;  // Not called directly; execute_move uses the free function
}

uint32_t Battle::calculate_exp(const BattlePokemon& defeated, bool is_trainer) const {
    const SpeciesData* sd = registries_.species.get(defeated.species);
    if (!sd) return 0;
    return calculate_exp_gain(sd->base_exp, defeated.level, is_trainer, 1);
}

} // namespace enginemon

#pragma warning(pop)  // Re-enable deprecated warnings after battle.cpp
