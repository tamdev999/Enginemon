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

// NOTE: No #pragma warning(disable: 4996) here.
// Production code always sets rules_ before execute_turn() and uses the
// BattleRules& overloads.  Unit tests that omit set_battle_rules() must
// suppress deprecated warnings locally in their own TU.
// A missing rules_ in production is an initialization error, not a fallback.

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
    const Registries& reg,
    uint8_t dv_atk = 9, uint8_t dv_def = 8,
    uint8_t dv_spd = 8, uint8_t dv_spc = 8)
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

        // Stats: use per-pokemon DVs from TrainerData (materialized by frontend extractor).
        // Gen 2 HP DV is derived from the low bits of all four DVs.
        // Source: Crystal CalcMonStatC / GetHPIV (move_mon.asm):
        //   DV_HP = (DV_ATK & 1) << 3 | (DV_DEF & 1) << 2 | (DV_SPD & 1) << 1 | (DV_SPC & 1)
        const auto& bs = sd->base_stats;
        const uint8_t dv_hp = static_cast<uint8_t>(
            ((dv_atk & 1u) << 3) | ((dv_def & 1u) << 2) |
            ((dv_spd & 1u) << 1) |  (dv_spc & 1u));
        bp.stats.max_hp          = static_cast<int16_t>(calc_hp(bs.hp, dv_hp, 0, level));
        bp.stats.hp              = bp.stats.max_hp;
        bp.stats.attack          = static_cast<int16_t>(calc_stat(bs.attack,          dv_atk, 0, level));
        bp.stats.defense         = static_cast<int16_t>(calc_stat(bs.defense,         dv_def, 0, level));
        bp.stats.speed           = static_cast<int16_t>(calc_stat(bs.speed,           dv_spd, 0, level));
        bp.stats.special_attack  = static_cast<int16_t>(calc_stat(bs.special_attack,  dv_spc, 0, level));
        bp.stats.special_defense = static_cast<int16_t>(calc_stat(bs.special_defense, dv_spc, 0, level));
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

// Production constructor — BattleRules required at construction.
Battle::Battle(BattleType type, Party& player_party, const Registries& reg,
               const BattleRules& rules)
    : type_(type)
    , player_party_(player_party)
    , registries_(reg)
    , rules_(&rules)
{}

// Test constructor — no BattleRules; execute_turn() will throw in release.
Battle::Battle(BattleType type, Party& player_party, const Registries& reg)
    : type_(type)
    , player_party_(player_party)
    , registries_(reg)
    , rules_(nullptr)
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
    trainer_id_          = trainer;
    trainer_class_index_ = data.trainer_class;
    // Crystal's ComputeTrainerReward uses wCurPartyLevel, which holds the level of the
    // LAST pokemon parsed from the trainer party stream (not the highest level).
    // Source: read_trainer_party.asm — wCurPartyLevel set in the parsing loop;
    // after the loop ends, it holds the last value set, which is the last party member.
    last_trainer_party_level_ = data.party.empty() ? 0
                              : data.party.back().level;

    opponent_party_.clear();
    for (size_t i = 0; i < data.party.size(); ++i) {
        const auto& tp = data.party[i];
        opponent_party_.push_back(
            make_battle_pokemon(tp.species, tp.level, tp.held_item, tp.moves, i, registries_,
                                tp.dv_atk, tp.dv_def, tp.dv_spd, tp.dv_spc));
    }

    if (!opponent_party_.empty()) {
        opponent_active_index_ = 0;
        opponent_pokemon_ = opponent_party_[0];
    }

    // Determine AI behavior from trainer class via BattleRules.
    // get_trainer_ai_passes() returns a semantic AIPassSet decoded by the Crystal frontend.
    // No Crystal ROM bit positions in this code.
    if (rules_) {
        const AIPassSet passes = rules_->get_trainer_ai_passes(data.trainer_class);
        trainer_ai_ = std::make_unique<VanillaCrystalAI>(passes);
    } else {
        trainer_ai_ = std::make_unique<VanillaCrystalAI>(VanillaAI::BASIC);
    }
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
    // rules_ is asserted non-null before any call to execute_turn().
    // apply_stat_stages is only called from execute_move and execute_turn paths.
    if (rules_) {
        bp.stats.attack          = static_cast<int16_t>(apply_stat_stage(bp.base_stats.attack,          bp.stages.attack,          *rules_));
        bp.stats.defense         = static_cast<int16_t>(apply_stat_stage(bp.base_stats.defense,         bp.stages.defense,         *rules_));
        bp.stats.speed           = static_cast<int16_t>(apply_stat_stage(bp.base_stats.speed,           bp.stages.speed,           *rules_));
        bp.stats.special_attack  = static_cast<int16_t>(apply_stat_stage(bp.base_stats.special_attack,  bp.stages.special_attack,  *rules_));
        bp.stats.special_defense = static_cast<int16_t>(apply_stat_stage(bp.base_stats.special_defense, bp.stages.special_defense, *rules_));
    } else {
        // Test-only path (rules_ == nullptr, no execute_turn assert fired yet).
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

    // BattleRules must be set before execute_turn() in any production path.
    // Enforced in both debug and release builds: throws if rules_ is null.
    // Use the production constructor Battle(type, party, reg, rules) to guarantee this.
    if (rules_ == nullptr) {
        throw std::runtime_error(
            "Battle::execute_turn(): BattleRules not set. "
            "Use Battle(type, party, reg, rules) constructor or call set_battle_rules() first.");
    }

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

    turn_halted_ = false;  // Reset at turn start
    execute_action(first, second, fa, first_is_player);
    check_fainted();
    if (result_ != BattleResult::InProgress) { finalize_outcome(); return; }

    if (!turn_halted_ && !second.is_fainted()) {
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
    turn_halted_ = false;
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
        const auto res = execute_move(user, target, mid, af.move_slot, is_player);
        // UnsupportedSemantic and InvalidData: no valid move execution; flag turn as
        // halted so the opponent does not act on this turn.  Battle state remains coherent.
        if (res == MoveExecutionResult::UnsupportedSemantic ||
            res == MoveExecutionResult::InvalidData) {
            turn_halted_ = true;
        }

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

MoveExecutionResult Battle::execute_move(BattlePokemon& user, BattlePokemon& target,
                          MoveId move_id, size_t move_slot, bool user_is_player) {
    const MoveData* md = registries_.moves.get(move_id);
    if (!md) { message("Unknown move!"); return MoveExecutionResult::NoTarget; }

    message((user_is_player ? "Player used " : "Opponent used ") + md->name + "!");

    if (md->category == MoveCategory::Status) {
        // Status move effects are not implemented in this pass.
        // PP NOT deducted. Return UnsupportedSemantic to halt turn continuation.
        // Source: Crystal dispatches each status effect via BattleCommand handlers.
        message(md->name + " — status effect not yet supported (deferred).");
        return MoveExecutionResult::UnsupportedSemantic;
    }

    // Deduct PP (only for damaging moves where execution proceeds)
    if (move_slot < 4 && user.moves[move_slot].pp > 0)
        user.moves[move_slot].pp--;

    // Accuracy check
    // 0xFF = always hit (Crystal encoding for never-miss moves like Swift).
    // 0    = missing/unset data — explicit invalid-data failure; undo PP deduct.
    if (md->accuracy == 0) {
        // Undo PP deduction: this is a data error, not a gameplay action.
        if (move_slot < 4 && user.moves[move_slot].pp < 63)
            user.moves[move_slot].pp++;
        message("Move data error: accuracy not set for " + md->name);
        return MoveExecutionResult::InvalidData;
    }
    if (md->accuracy != 0xFF) {
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
            return MoveExecutionResult::Miss;
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
        return MoveExecutionResult::Immune;
    }

    // STAB
    const bool stab = (md->type == user.type1 || md->type == user.type2);

    // Attack / defense stats (crit ignores negative atk stages and positive def stages)
    // Source: suiCune effect_commands.c CheckDamageStatsCritical
    const int8_t eff_atk_stage  = (is_crit && user.stages.attack < 0)            ? 0 : user.stages.attack;
    const int8_t eff_def_stage  = (is_crit && target.stages.defense > 0)          ? 0 : target.stages.defense;
    const int8_t eff_satk_stage = (is_crit && user.stages.special_attack < 0)    ? 0 : user.stages.special_attack;
    const int8_t eff_sdef_stage = (is_crit && target.stages.special_defense > 0) ? 0 : target.stages.special_defense;

    int32_t atk_stat, def_stat;
    const bool physical = (md->category == MoveCategory::Physical);

    // Helper lambda: applies stat stage using BattleRules if available
    auto stat_stage = [this](int32_t base, int8_t stage) -> int32_t {
        return rules_ ? apply_stat_stage(base, stage, *rules_)
                      : apply_stat_stage(base, stage);
    };

    if (physical) {
        atk_stat = stat_stage(user.base_stats.attack,    eff_atk_stage);
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

    // Crystal damage formula matching BattleCommand_DamageCalc + BattleCommand_Stab ordering:
    //   DamageCalc: base = (level×2/5+2) × power × atk/def/50  → crit×2 → burn>>1 → +2 floor
    //   BattleCommand_Stab: weather → STAB → type matchup loop
    //
    // Step 1: compute base damage (no type_eff, no STAB — those come after weather)
    DamageParams dp{};
    dp.attacker_level     = user.level;
    dp.attack_stat        = atk_stat;
    dp.defense_stat       = def_stat;
    dp.move_power         = md->power;
    dp.type_effectiveness = 100;  // Neutral — type applied manually after weather (see Step 4)
    dp.stab               = false; // Applied manually after weather (see Step 3)
    dp.critical           = is_crit;
    dp.burned             = burned;
    dp.weather            = field_.weather;
    dp.move_type          = md->type;

    int32_t damage = enginemon::calculate_damage(dp);
    if (damage == 0) return MoveExecutionResult::Immune;

    // Step 2: apply weather modifier
    // Source: DoWeatherModifiers runs FIRST in BattleCommand_Stab, before STAB and type loop.
    if (rules_ && field_.weather != Weather::None) {
        const uint8_t weather_id = static_cast<uint8_t>(field_.weather);
        const uint8_t type_id    = static_cast<uint8_t>(md->type);
        const uint8_t effect_id  = md->effect_id;
        damage = apply_weather_modifier(damage, weather_id, type_id, effect_id, *rules_);
    }

    // Step 3: STAB — applied after weather, before type loop.
    // Source: BattleCommand_Stab checks STAB before .TypesLoop.
    if (stab) {
        damage += damage / 2;   // +50% integer: floor(n × 3/2) via shift+add
        if (damage > 999) damage = 999;
        if (damage < 2)   damage = 2;
    }

    // Step 4: type effectiveness multiplier.
    // Source: BattleCommand_Stab .TypesLoop applies after STAB.
    // type_eff is in per-100 notation (100=neutral, 200=2×, 50=0.5×, 0=immune).
    // Immunity was already checked above and returned early.
    if (type_eff != 100) {
        damage = damage * static_cast<int32_t>(type_eff) / 100;
        if (damage < 1) damage = 1;
        if (damage > 999) damage = 999;
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
    if (is_crit)        message("A critical hit!");
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
    return MoveExecutionResult::Success;
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

    // Denominators from SM83 lifting of GetEighthMaxHP / GetSixteenthMaxHP.
    // Vanilla: burn/poison = /8, toxic = /16 (per turn, multiplied by status_turns).
    const int32_t burn_denom  = rules_ ? static_cast<int32_t>(rules_->get_burn_poison_denom())  : 8;
    const int32_t toxic_denom = rules_ ? static_cast<int32_t>(rules_->get_toxic_denom())         : 16;

    if (bp.status == Status::Burn) {
        const int32_t d = (burn_denom > 0) ? (bp.stats.max_hp / burn_denom) : 1;
        dmg = static_cast<int16_t>(std::max(1, d));
        message(std::string(side) + " is hurt by its burn!");
    } else if (bp.status == Status::Poison) {
        const int32_t d = (burn_denom > 0) ? (bp.stats.max_hp / burn_denom) : 1;
        dmg = static_cast<int16_t>(std::max(1, d));
        message(std::string(side) + " is hurt by poison!");
    } else if (bp.status == Status::BadPoison) {
        bp.status_turns++;
        const int32_t d = (toxic_denom > 0)
            ? (bp.stats.max_hp * bp.status_turns / toxic_denom) : 1;
        dmg = static_cast<int16_t>(std::max(1, d));
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
        // Crystal formula: ComputeTrainerReward = base_reward × wCurPartyLevel
        // Source: engine/battle/read_trainer_party.asm ComputeTrainerReward
        // wCurPartyLevel holds the level of the LAST-PARSED pokemon in the party
        // (set sequentially in the parsing loop; last iteration wins).
        // base_reward comes from TrainerClassAttributes::TRNATTR_BASEMONEY.
        uint8_t base_reward = 0;
        if (rules_) {
            base_reward = rules_->get_trainer_class_base_reward(trainer_class_index_);
        }
        outcome_.money_gained = static_cast<uint32_t>(base_reward)
                              * static_cast<uint32_t>(last_trainer_party_level_);
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
