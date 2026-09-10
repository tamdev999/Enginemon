// engine/battle/battle.cpp
// Gen 2 battle system Î“Ã‡Ã¶ turn-based Pokemon battles
//
// Architecture note:
//   Battle owns no renderer, no Lua, no ROM references.
//   RNG comes from GameState::rng via the callback set by the caller.
//   If no callback is set (unit test mode), a seeded mt19937 fallback is used
//   that does NOT touch GameState::rng.
//
// Turn flow (source: suiCune core.c):
//   1. Determine turn order (priority Î“Ã¥Ã† speed Î“Ã¥Ã† random tie)
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
    return (volatile_status & static_cast<uint32_t>(vs)) != 0;
}

void BattlePokemon::set_volatile(VolatileStatus vs) {
    volatile_status |= static_cast<uint32_t>(vs);
}

void BattlePokemon::clear_volatile(VolatileStatus vs) {
    volatile_status &= ~static_cast<uint32_t>(vs);
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

// Production constructor Î“Ã‡Ã¶ BattleRules required at construction.
Battle::Battle(BattleType type, Party& player_party, const Registries& reg,
               const BattleRules& rules)
    : type_(type)
    , player_party_(player_party)
    , registries_(reg)
    , rules_(&rules)
{}

// Test constructor Î“Ã‡Ã¶ no BattleRules; execute_turn() will throw in release.
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
    // Source: read_trainer_party.asm Î“Ã‡Ã¶ wCurPartyLevel set in the parsing loop;
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
        // Source: Crystal AIChooseMove Î“Ã‡Ã¶ wild uses Random() % num_usable_moves,
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

    // Architecture B: Rampage lock.
    auto enforce_rampage = [&](BattleAction& act, BattlePokemon& mon) {
        if (!mon.has_volatile(VolatileStatus::Rampage) || mon.last_move_used == MOVE_NONE) return;
        for (size_t s = 0; s < 4; ++s) {
            if (mon.moves[s].move == mon.last_move_used) { act = ActionFight{s, 0}; return; }
        }
    };
    enforce_rampage(player_action_,   player_pokemon_);
    enforce_rampage(opponent_action_, opponent_pokemon_);

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
        // Architecture B: Pursuit interception.
        // Crystal model: if the second actor is switching, check whether the first actor
        // has Pursuit. If so, fire it (2x power) before the switch resolves.
        if (std::holds_alternative<ActionSwitch>(sa) && !first.is_fainted()) {
            hook_pursuit_check(first, second, first_is_player);
            second.is_switching = false;
            check_fainted();
            if (result_ != BattleResult::InProgress) { finalize_outcome(); return; }
        }
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
        // Architecture B: Encore override.
        size_t effective_slot = af.move_slot;
        if (user.encore_turns > 0 && user.encored_move != MOVE_NONE) {
            for (size_t s = 0; s < 4; ++s) {
                if (user.moves[s].move == user.encored_move) { effective_slot = s; break; }
            }
        }
        const MoveId mid = (effective_slot < 4) ? user.moves[effective_slot].move : MOVE_NONE;
        if (mid == MOVE_NONE) {
            message(is_player ? "Player has no usable move!" : "Opponent has no usable move!");
            return;
        }
        // Architecture B: pre-move clear and check.
        hook_pre_move_clear_protect(user);
        hook_pre_move_clear_destiny_bond(user);
        if (hook_pre_move_check(user, target, effective_slot, is_player)) return;
        const auto res = execute_move(user, target, mid, effective_slot, is_player);
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

    // Ã¢â€â‚¬Ã¢â€â‚¬ Recharge gate (Hyper Beam) Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
    if (user.recharge_turns > 0) {
        --user.recharge_turns;
        message(md->name + " Ã¢â‚¬â€ must recharge!");
        return MoveExecutionResult::UnsupportedSemantic;  // turn skipped, no halt
    }

    // -- Support gate - read from SemanticEffectDescription ------------------
    // Execution dispatch is entirely driven by effect_desc.
    // effect_id (SemEffect::X) is retained for AI classification only.
    // There is no synthesis from effect_id at runtime: missing or unsupported
    // descriptions fail closed with UnsupportedSemantic.
    const auto& effective_desc = md->effect_desc;

    // -- B-dispatch: checked BEFORE is_supported because B-path moves intentionally
    // have effect_desc.is_supported=false (Architecture A never executes them).
    // execute_program handles its own PP deduction and recharge gate.
    if (md->has_program && md->effect_program.is_compiled) {
        return execute_program(user, target, *md, move_slot, user_is_player);
    }

    // Hard fail for unsupported or missing descriptions.
    // Production packages always carry a compiled SemanticEffectDescription.
    // A zero-init desc (is_supported=false) means the move was never compiled
    // through the semanticizer -- treat as unsupported, never synthesise.
    if (!effective_desc.is_supported) {
        message(md->name + " -- effect not yet implemented (deferred).");
        return MoveExecutionResult::UnsupportedSemantic;
    }

    // Ã¢â€â‚¬Ã¢â€â‚¬ PP deduction Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
    if (move_slot < 4 && user.moves[move_slot].pp > 0)
        user.moves[move_slot].pp--;

    // Ã¢â€â‚¬Ã¢â€â‚¬ Safeguard check for status moves Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
    // Source: BattleCommand_CheckSafeguard Ã¢â‚¬â€ returns fail if opponent has Safeguard.
    const bool target_has_safeguard =
        user_is_player ? (field_.safeguard_opponent > 0) : (field_.safeguard_player > 0);

    // Ã¢â€â‚¬Ã¢â€â‚¬ OHKO special path Ã¢â‚¬â€ no standard pipeline Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
    if (effective_desc.is_ohko) {
        // Auto-fail if target level > user level (BattleCommand_OHKO).
        if (target.level > user.level) {
            message("The attack missed!");
            return MoveExecutionResult::Miss;
        }
        // Accuracy: base_acc + (user_level - target_level) Ãƒâ€” multiplier
        const uint8_t mult = rules_ ? rules_->get_ohko_level_mult() : uint8_t{2};
        const int32_t level_bonus = (static_cast<int32_t>(user.level) -
                                     static_cast<int32_t>(target.level)) * mult;
        const int32_t eff_acc = std::min(255, static_cast<int32_t>(md->accuracy) + level_bonus);
        const bool hit = (rng_.next_byte() < eff_acc);
        if (!hit) {
            message("The attack missed!");
            return MoveExecutionResult::Miss;
        }
        // P0-1: Protect check for OHKO (Crystal's ohko opcode calls CheckHit which checks Protect).
        if (target.has_volatile(VolatileStatus::Protect)) {
            message((user_is_player ? std::string("Opponent") : std::string("Player"))
                    + " protected itself!");
            return MoveExecutionResult::Miss;
        }
        // OHKO always faints the target.
        message("It's a one-hit KO!");
        const int16_t old_hp = target.stats.hp;
        // P0-1: Substitute absorbs OHKO (Substitute has HP, OHKO fades against it).
        if (target.has_volatile(VolatileStatus::Substitute)) {
            // OHKO: destroy the Substitute entirely (damage exceeds any sub HP).
            target.clear_volatile(VolatileStatus::Substitute);
            target.substitute_hp = 0;
            message((user_is_player ? std::string("Opponent") : std::string("Player"))
                    + "'s substitute faded!");
            // ROOT_OHKO_BOOKKEEPING: when Substitute absorbs OHKO, real target HP is
            // unchanged. Do NOT record target's old real HP as damage_dealt, and do NOT
            // call hook_on_damage_received with the real-HP value. That would falsely
            // populate Counter/Bide/Rage damage history for damage the target never took.
            // Crystal source: OHKO that hits Substitute does not update wCurDamage for
            // real-target accumulation — the sub just fades.
            return MoveExecutionResult::Success;
        }
        target.stats.hp = 0;
        // P0-2: Endure 1-HP floor for OHKO.
        if (target.has_volatile(VolatileStatus::Endure)) {
            target.stats.hp = 1;
            message((user_is_player ? std::string("Opponent") : std::string("Player"))
                    + " endured the hit!");
        }
        hp_change(user_is_player ? 1u : 0u, old_hp, target.stats.hp);
        outcome_.damage_dealt += static_cast<uint16_t>(old_hp);
        // P0-4: Damage history for OHKO.
        hook_on_damage_received(target, static_cast<int32_t>(old_hp),
                                static_cast<uint8_t>(md->category));
        return MoveExecutionResult::Success;
    }

    // Ã¢â€â‚¬Ã¢â€â‚¬ Pure status-only path (no damage) Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
    if (md->category == MoveCategory::Status && !effective_desc.has_standard_damage
        && effective_desc.constant_damage_source == ConstantDamageSource::None) {
        // Healing Ã¢â‚¬â€ user heals itself.
        if (effective_desc.heal_source != HealSource::None) {
            int32_t heal_amt = 0;
            if (effective_desc.heal_source == HealSource::HalfMaxHP) {
                heal_amt = std::max(1, static_cast<int32_t>(user.stats.max_hp) / 2);
            } else if (effective_desc.heal_source == HealSource::WeatherHealing) {
                const uint8_t sun_div = rules_ ? rules_->weather_heal.sun_divisor      : 2;
                const uint8_t neu_div = rules_ ? rules_->weather_heal.neutral_divisor  : 2;
                const uint8_t oth_div = rules_ ? rules_->weather_heal.other_divisor    : 4;
                const uint8_t divisor = (field_.weather == Weather::Sun)  ? sun_div :
                                        (field_.weather == Weather::None) ? neu_div : oth_div;
                heal_amt = std::max(1, static_cast<int32_t>(user.stats.max_hp) / divisor);
            }
            if (user.stats.hp >= user.stats.max_hp) {
                message(md->name + " Ã¢â‚¬â€ HP is full!");
            } else {
                const int16_t old_hp = user.stats.hp;
                user.stats.hp = static_cast<int16_t>(
                    std::min(static_cast<int32_t>(user.stats.max_hp),
                             static_cast<int32_t>(user.stats.hp) + heal_amt));
                hp_change(user_is_player ? 0u : 1u, old_hp, user.stats.hp);
                message((user_is_player ? std::string("Player") : std::string("Opponent"))
                        + " restored HP!");
            }
            return MoveExecutionResult::Success;
        }

        // Screen setup.
        if (effective_desc.set_screen != ScreenType::None) {
            uint8_t duration = 5;  // Crystal: Reflect/Light Screen last 5 turns
            if (effective_desc.set_screen == ScreenType::Reflect) {
                if (user_is_player) { field_.reflect_player   = duration; }
                else                { field_.reflect_opponent = duration; }
                message("Reflect raised the Defense of the team!");
            } else {
                if (user_is_player) { field_.light_screen_player   = duration; }
                else                { field_.light_screen_opponent = duration; }
                message("Light Screen raised the Sp. Def. of the team!");
            }
            return MoveExecutionResult::Success;
        }

        // Weather setup.
        if (effective_desc.set_weather != WeatherSetType::None) {
            const uint8_t weather_turns = 5;
            switch (effective_desc.set_weather) {
                case WeatherSetType::Rain:
                    field_.weather = Weather::Rain;
                    field_.weather_turns = weather_turns;
                    message("Rain started to fall!");
                    break;
                case WeatherSetType::Sun:
                    field_.weather = Weather::Sun;
                    field_.weather_turns = weather_turns;
                    message("The sun shone harshly!");
                    break;
                case WeatherSetType::Sandstorm:
                    field_.weather = Weather::Sandstorm;
                    field_.weather_turns = weather_turns;
                    message("A sandstorm brewed!");
                    break;
                default: break;
            }
            return MoveExecutionResult::Success;
        }

        // Primary status application.
        if (effective_desc.primary_status != PrimaryStatusType::None) {
            if (target_has_safeguard) {
                message("Safeguard protected the target!");
                return MoveExecutionResult::Miss;
            }
            if (target.status != Status::None) {
                message("It didn't workÃ¢â‚¬Â¦");
                return MoveExecutionResult::Miss;
            }
            // Accuracy check
            if (md->accuracy != 0xFF) {
                if (!roll_accuracy(md->accuracy, user.stages.accuracy,
                                   target.stages.evasion, rng_.next_byte(),
                                   *rules_)) {
                    message("The attack missed!");
                    return MoveExecutionResult::Miss;
                }
            }
            switch (effective_desc.primary_status) {
                case PrimaryStatusType::Sleep:
                    target.status = Status::Sleep;
                    // Crystal SleepEffect: status_turns = (BattleRandom & 6) + 1 = 1..7.
                    // Source: pokecrystal engine/battle/effect_commands.asm SleepEffect.
                    {
                        const uint8_t raw = rng_.next_byte();
                        target.status_turns = static_cast<uint8_t>((raw & 6u) + 1u);
                    }
                    break;
                case PrimaryStatusType::Poison:   target.status = Status::Poison;   break;
                case PrimaryStatusType::Toxic:    target.status = Status::BadPoison; target.status_turns = 0; break;
                case PrimaryStatusType::Paralysis:target.status = Status::Paralysis; break;
                case PrimaryStatusType::Confusion:
                    target.set_volatile(VolatileStatus::Confusion);
                    message((user_is_player ? std::string("Opponent") : std::string("Player"))
                            + " became confused!");
                    return MoveExecutionResult::Success;
                default: break;
            }
            message((user_is_player ? std::string("Opponent") : std::string("Player"))
                    + " was inflicted with a status condition!");
            return MoveExecutionResult::Success;
        }

        // Stat change (status move path).
        // NOTE: swagger_stat_change is handled separately below; skip here if set.
        if (effective_desc.stat_change != StatChangeTarget::None
                && !effective_desc.swagger_stat_change) {
            apply_stat_change(user, target, effective_desc.stat_change, user_is_player);
            // Minimize: set Minimized volatile so Stomp deals double damage.
            // Source: suiCune MinimizeDropSub — called from RaiseStat only when
            // the move animation is MINIMIZE. sets_minimize is set only for move ID 107.
            // Double Team shares EvasionUp but does NOT set this flag.
            if (effective_desc.sets_minimize) {
                user.set_volatile(VolatileStatus::Minimized);
            }
            return MoveExecutionResult::Success;
        }

        // Spikes.
        if (effective_desc.sets_spikes) {
            // Second use fails if spikes already active on that side (Gen 2: single layer only).
            const bool already_set = user_is_player ? field_.spikes_opponent : field_.spikes_player;
            if (already_set) {
                message(md->name + " -- failed!");
                return MoveExecutionResult::Miss;
            }
            if (user_is_player) field_.spikes_opponent = true;
            else                field_.spikes_player   = true;
            message("Spikes were scattered!");
            return MoveExecutionResult::Success;
        }

        // New A semantics
        if (effective_desc.requires_user_asleep && user.status != Status::Sleep) {
            message(md->name + " -- failed!"); return MoveExecutionResult::Miss;
        }
        if (effective_desc.is_splash) { message("But nothing happened!"); return MoveExecutionResult::Success; }
        if (effective_desc.sets_focus_energy) {
            if (user.has_volatile(VolatileStatus::FocusEnergy)) { message(md->name + " -- failed!"); return MoveExecutionResult::Miss; }
            user.set_volatile(VolatileStatus::FocusEnergy); message(md->name + " -- getting pumped!"); return MoveExecutionResult::Success;
        }
        if (effective_desc.sets_mist) {
            if (user.has_volatile(VolatileStatus::Mist)) { message(md->name + " -- failed!"); return MoveExecutionResult::Miss; }
            user.set_volatile(VolatileStatus::Mist); message(md->name + " -- shrouded in mist!"); return MoveExecutionResult::Success;
        }
        if (effective_desc.sets_safeguard) {
            uint8_t& sg = user_is_player ? field_.safeguard_player : field_.safeguard_opponent;
            if (sg > 0) { message(md->name + " -- failed!"); return MoveExecutionResult::Miss; }
            sg = 5; message(md->name + " -- covered by a veil!"); return MoveExecutionResult::Success;
        }
        if (effective_desc.changes_user_type) {
            std::vector<uint8_t> cands;
            for (size_t s = 0; s < 4; ++s) {
                if (user.moves[s].move == MOVE_NONE) continue;
                const MoveData* mm = registries_.moves.get(user.moves[s].move);
                if (mm && static_cast<uint8_t>(mm->type) != 19u) cands.push_back(static_cast<uint8_t>(mm->type));
            }
            if (cands.empty()) { message(md->name + " -- failed!"); return MoveExecutionResult::Miss; }
            user.type1 = user.type2 = static_cast<TypeId>(cands[rng_.next_byte() % cands.size()]);
            message(md->name + " -- type changed!"); return MoveExecutionResult::Success;
        }
        if (effective_desc.changes_user_type_resist) {
            if (target.last_move_used == MOVE_NONE) { message(md->name + " -- failed!"); return MoveExecutionResult::Miss; }
            const MoveData* lm = registries_.moves.get(target.last_move_used);
            if (!lm) { message(md->name + " -- failed!"); return MoveExecutionResult::Miss; }
            for (uint32_t tr = 0; tr < 500u; ++tr) {
                const uint8_t c = rng_.next_byte() % 20u;
                if (c == 19u) continue;
                if (get_combined_effectiveness(lm->type, static_cast<TypeId>(c), static_cast<TypeId>(c), registries_.type_chart) < 100u) {
                    user.type1 = user.type2 = static_cast<TypeId>(c);
                    message(md->name + " -- type changed!"); return MoveExecutionResult::Success;
                }
            }
            message(md->name + " -- failed!"); return MoveExecutionResult::Miss;
        }
        if (effective_desc.traps_opponent) {
            if (target.has_volatile(VolatileStatus::Flying) || target.has_volatile(VolatileStatus::Underground) || target.has_volatile(VolatileStatus::CantRun)) { message(md->name + " -- failed!"); return MoveExecutionResult::Miss; }
            target.set_volatile(VolatileStatus::CantRun); message(md->name + " -- can't escape!"); return MoveExecutionResult::Success;
        }
        if (effective_desc.identifies_opponent) {
            if (target.has_volatile(VolatileStatus::Flying) || target.has_volatile(VolatileStatus::Underground) || target.has_volatile(VolatileStatus::Identified)) { message(md->name + " -- failed!"); return MoveExecutionResult::Miss; }
            target.set_volatile(VolatileStatus::Identified); message(md->name + " -- identified!"); return MoveExecutionResult::Success;
        }
        if (effective_desc.reduces_pp) {
            if (target.last_move_used == MOVE_NONE) { message(md->name + " -- failed!"); return MoveExecutionResult::Miss; }
            uint8_t fs = 0xFF;
            for (uint8_t s = 0; s < 4; ++s) { if (target.moves[s].move == target.last_move_used) { fs=s; break; } }
            if (fs == 0xFF || target.moves[fs].pp == 0) { message(md->name + " -- failed!"); return MoveExecutionResult::Miss; }
            const uint8_t red = static_cast<uint8_t>((rng_.next_byte() & 3u) + 2u);
            target.moves[fs].pp = static_cast<uint8_t>(target.moves[fs].pp > red ? target.moves[fs].pp - red : 0u);
            message(md->name + " -- reduced PP!"); return MoveExecutionResult::Success;
        }
        if (effective_desc.equalizes_hp) {
            if (target.has_volatile(VolatileStatus::Substitute)) { message(md->name + " -- failed!"); return MoveExecutionResult::Miss; }
            const int32_t avg = (static_cast<int32_t>(user.stats.hp) + static_cast<int32_t>(target.stats.hp)) / 2;
            const int16_t nu = static_cast<int16_t>(std::min(avg, static_cast<int32_t>(user.stats.max_hp)));
            const int16_t nt = static_cast<int16_t>(std::min(avg, static_cast<int32_t>(target.stats.max_hp)));
            const int16_t ou = user.stats.hp, ot = target.stats.hp;
            user.stats.hp = nu; target.stats.hp = nt;
            if (nu != ou) hp_change(user_is_player ? 0u : 1u, ou, nu);
            if (nt != ot) hp_change(user_is_player ? 1u : 0u, ot, nt);
            message(md->name + " -- HP shared!"); return MoveExecutionResult::Success;
        }
        if (effective_desc.ends_wild_battle) {
            if (type_ != BattleType::Wild || target.has_volatile(VolatileStatus::CantRun)) { message(md->name + " -- failed!"); return MoveExecutionResult::Miss; }
            result_ = BattleResult::PlayerRan; message(md->name + " -- teleported!"); return MoveExecutionResult::Success;
        }
        if (effective_desc.swagger_stat_change) {
            // Swagger: raise TARGET's Attack +2 (not user's), then confuse target.
            // Source: Crystal -- switchturn makes attackup2 apply to opponent.
            apply_one_stage_change(target, 0, +2);
            if (!target_has_safeguard && !target.has_volatile(VolatileStatus::Confusion)) {
                target.set_volatile(VolatileStatus::Confusion);
                target.confusion_turns = static_cast<uint8_t>((rng_.next_byte() & 3u) + 2u);
                message((user_is_player ? std::string("Opponent") : std::string("Player")) + " became confused!");
            }
            return MoveExecutionResult::Success;
        }
        if (effective_desc.has_payday) {
            // Pay Day coins are accumulated in the damaging path (after damage is dealt).
            // Fall through to the damaging path.
        } else {
            // Unrecognised status-only move -- fail closed.
            message(md->name + " -- effect not implemented.");
            return MoveExecutionResult::UnsupportedSemantic;
        }
    }

    // Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
    // DAMAGING PATH
    // Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬

    // Accuracy check.
    if (md->accuracy == 0) {
        if (move_slot < 4 && user.moves[move_slot].pp < 63)
            user.moves[move_slot].pp++;  // undo PP deduct Ã¢â‚¬â€ data error
        message("Move data error: accuracy not set for " + md->name);
        return MoveExecutionResult::InvalidData;
    }
    if (md->accuracy != 0xFF) {
        // Lock-On / Mind Reader: bypass accuracy entirely.
        // Source: Crystal BattleCommand_CheckHit -- always-hit when LockOn volatile set on target.
        const bool lock_on_active = target.has_volatile(VolatileStatus::LockOn);
        if (lock_on_active) {
            target.clear_volatile(VolatileStatus::LockOn);
        } else {
            const bool hit = rules_
                ? roll_accuracy(md->accuracy, user.stages.accuracy,
                                target.stages.evasion, rng_.next_byte(), *rules_)
                : roll_accuracy(md->accuracy, user.stages.accuracy,
                                target.stages.evasion, rng_.next_byte());
            if (!hit) {
                // Jump Kick / Hi Jump Kick: crash damage on accuracy miss ONLY when target
                // is not type-immune.
                // Source: pokecrystal GetFailureResultText -- checks wTypeModifier != 0
                // before crash. wTypeModifier is 0 on type immunity.
                // We pre-check type_eff here to match Crystal's ordering.
                if (effective_desc.crash_on_miss) {
                    // Compute type effectiveness now to gate crash (mirrors Crystal's
                    // wTypeModifier check in GetFailureResultText).
                    const uint16_t crash_type_eff = get_combined_effectiveness(
                        md->type, target.type1, target.type2, registries_.type_chart);
                    if (crash_type_eff != 0) {
                        // Non-immune: crash fires.
                        // crash = max(1, computed_hit_damage >> 3)
                        // Source: pokecrystal effect_commands.asm -- srl/rr x3 on wCurDamage.
                        const bool physical = (md->category == MoveCategory::Physical);
                        auto ss2 = [this](int32_t b, int8_t s) {
                            return rules_ ? apply_stat_stage(b,s,*rules_) : apply_stat_stage(b,s);
                        };
                        const int32_t atk2 = physical
                            ? ss2(user.base_stats.attack, user.stages.attack)
                            : ss2(user.base_stats.special_attack, user.stages.special_attack);
                        const int32_t def2 = physical
                            ? ss2(target.base_stats.defense, target.stages.defense)
                            : ss2(target.base_stats.special_defense, target.stages.special_defense);
                        DamageParams crashdp{};
                        crashdp.attacker_level = user.level;
                        crashdp.attack_stat    = std::max(1, atk2);
                        crashdp.defense_stat   = std::max(1, def2);
                        crashdp.move_power     = md->power;
                        crashdp.type_effectiveness = 100;
                        crashdp.stab = false; crashdp.critical = false;
                        crashdp.burned = false; crashdp.weather = Weather::None;
                        crashdp.move_type = md->type;
                        const int32_t ref_dmg = rules_
                            ? enginemon::calculate_damage(crashdp, *rules_)
                            : enginemon::calculate_damage(crashdp);
                        const int32_t crash_dmg = std::max(1, ref_dmg >> 3);
                        const int16_t old_user_hp = user.stats.hp;
                        user.stats.hp = static_cast<int16_t>(
                            std::max(0, static_cast<int32_t>(user.stats.hp) - crash_dmg));
                        hp_change(user_is_player ? 0u : 1u, old_user_hp, user.stats.hp);
                        message(md->name + " -- the user crashed!");
                    }
                    // Immune target: no crash. Fall through to Miss return.
                }
                message("The attack missed!");
                return MoveExecutionResult::Miss;
            }
        }
    }

    // Type effectiveness.
    // Foresight (Identified): Normal and Fighting moves bypass Ghost immunity.
    // Source: Crystal BattleCommand_CheckTypeMatchup -- skip type_chart for Identified target.
    TypeId effective_move_type = md->type;
    uint16_t type_eff;
    {
        const bool identified = target.has_volatile(VolatileStatus::Identified);
        const uint8_t mt = static_cast<uint8_t>(effective_move_type);
        if (identified && (mt == 0u || mt == 1u)) {
            // Normal(0) or Fighting(1) vs Identified Ghost: treat as neutral, not immune.
            type_eff = get_combined_effectiveness(
                effective_move_type, target.type1, target.type2, registries_.type_chart);
            if (type_eff == 0) type_eff = 100;  // Foresight removes immunity
        } else {
            type_eff = get_combined_effectiveness(
                effective_move_type, target.type1, target.type2, registries_.type_chart);
        }
    }
    if (type_eff == 0) {
        message("It doesn't affect the opposing PokÃƒÂ©monÃ¢â‚¬Â¦");
        return MoveExecutionResult::Immune;
    }

    // Ã¢â€â‚¬Ã¢â€â‚¬ Constant damage path (Super Fang, Dragon Rage, etc.) Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
    if (effective_desc.constant_damage_source != ConstantDamageSource::None) {
        int32_t const_dmg = 0;
        switch (effective_desc.constant_damage_source) {
            case ConstantDamageSource::MoveFixed:
                const_dmg = static_cast<int32_t>(md->power);
                break;
            case ConstantDamageSource::UserLevel:
                const_dmg = static_cast<int32_t>(user.level);
                break;
            case ConstantDamageSource::HalfTargetHP:
                // Super Fang: floor(target_hp / 2), minimum 1
                const_dmg = std::max(1, static_cast<int32_t>(target.stats.hp) / 2);
                break;
            case ConstantDamageSource::Psywave: {
                // BattleCommand_ConstantDamage Psywave path:
                //   b = floor(user_level Ãƒâ€” 1.5)
                //   random non-zero in [1, b)
                const int32_t max_dmg = std::max(1,
                    static_cast<int32_t>(user.level) * 3 / 2);
                do {
                    const_dmg = static_cast<int32_t>(rng_.next_byte() % max_dmg);
                } while (const_dmg == 0);
                break;
            }
            case ConstantDamageSource::ReversalFlail: {
                // hp_bar_pixels = floor(current_hp Ãƒâ€” HP_BAR_MULT / max_hp)
                const uint8_t mult = rules_ ? rules_->get_reversal_hp_bar_mult() : uint8_t{48};
                const int32_t hp_pixels = (user.stats.hp > 0)
                    ? static_cast<int32_t>(user.stats.hp) * mult / user.stats.max_hp
                    : 0;
                const uint8_t hp_px = static_cast<uint8_t>(std::clamp(hp_pixels, 0, 255));
                const uint8_t power = rules_ ? rules_->get_reversal_power(hp_px) : uint8_t{20};
                // Run through the full damage formula with the derived power.
                {
                    const bool stab_r = (md->type == user.type1 || md->type == user.type2);
                    uint8_t crit_stage = 0;
                    if (rules_) crit_stage = build_crit_stage(user, *md, *rules_);
                    const bool is_crit_r = rules_
                        ? roll_critical(crit_stage, rng_.next_byte(), *rules_)
                        : roll_critical(crit_stage, rng_.next_byte());
                    const int8_t eff_atk_r = (is_crit_r && user.stages.attack < 0) ? 0 : user.stages.attack;
                    const int8_t eff_def_r = (is_crit_r && target.stages.defense > 0) ? 0 : target.stages.defense;
                    auto ss = [this](int32_t b, int8_t s) {
                        return rules_ ? apply_stat_stage(b,s,*rules_) : apply_stat_stage(b,s);
                    };
                    int32_t atk_r = ss(user.base_stats.attack,   eff_atk_r);
                    int32_t def_r = ss(target.base_stats.defense, eff_def_r);
                    const bool burned_r = (user.status == Status::Burn);
                    DamageParams dp_r{};
                    dp_r.attacker_level = user.level; dp_r.attack_stat = atk_r;
                    dp_r.defense_stat = def_r; dp_r.move_power = power;
                    dp_r.type_effectiveness = 100; dp_r.stab = false;
                    dp_r.critical = is_crit_r; dp_r.burned = burned_r;
                    dp_r.weather = field_.weather; dp_r.move_type = md->type;
                    int32_t dmg_r = rules_ ? enginemon::calculate_damage(dp_r, *rules_) : enginemon::calculate_damage(dp_r);
                    if (stab_r) { dmg_r += dmg_r/2; dmg_r = std::clamp(dmg_r, 2, 999); }
                    if (type_eff != 100) { dmg_r = dmg_r * type_eff / 100; dmg_r = std::clamp(dmg_r, 1, 999); }
                    // Variation
                    const uint8_t vt = rules_ ? rules_->get_damage_var_lower_bound() : uint8_t{0xD9};
                    const int32_t vd = rules_ ? static_cast<int32_t>(rules_->get_damage_var_divisor()) : 255;
                    uint8_t var_r;
                    do { uint8_t rr=rng_.next_byte(); var_r=(rr>>1)|(rr<<7); } while(var_r < vt);
                    dmg_r = dmg_r * var_r / (vd > 0 ? vd : 255);
                    if (dmg_r < 2) dmg_r = 2;
                    if (is_crit_r) message("A critical hit!");
                    if (type_eff > 100) message("It's super effective!");
                    else if (type_eff < 100) message("It's not very effectiveÃ¢â‚¬Â¦");
                    animate(md->animation_id, user_is_player ? 0u:1u, user_is_player ? 1u:0u);
                    // P0-1: Protect check for Reversal/Flail.
                    if (target.has_volatile(VolatileStatus::Protect)) {
                        message((user_is_player ? std::string("Opponent") : std::string("Player"))
                                + " protected itself!");
                        return MoveExecutionResult::Miss;
                    }
                    // P0-3: Substitute routing for Reversal/Flail.
                    if (target.has_volatile(VolatileStatus::Substitute) && target.substitute_hp > 0) {
                        if (dmg_r >= static_cast<int32_t>(target.substitute_hp)) {
                            target.substitute_hp = 0;
                            target.clear_volatile(VolatileStatus::Substitute);
                            message((user_is_player ? std::string("Opponent") : std::string("Player"))
                                    + "'s substitute broke!");
                        } else {
                            target.substitute_hp = static_cast<uint16_t>(
                                target.substitute_hp - static_cast<uint16_t>(dmg_r));
                        }
                        outcome_.damage_dealt += static_cast<uint16_t>(dmg_r);
                        // No hook_on_damage_received: Crystal skips accumulation for Substitute hits.
                        return MoveExecutionResult::Success;
                    }
                    const int16_t old_hp_r = target.stats.hp;
                    target.stats.hp = static_cast<int16_t>(std::max(0, static_cast<int32_t>(target.stats.hp) - dmg_r));
                    // P0-2: Endure 1-HP floor for Reversal/Flail.
                    if (target.has_volatile(VolatileStatus::Endure) && target.stats.hp <= 0) {
                        target.stats.hp = 1;
                        message((user_is_player ? std::string("Opponent") : std::string("Player"))
                                + " endured the hit!");
                    }
                    hp_change(user_is_player ? 1u:0u, old_hp_r, target.stats.hp);
                    outcome_.damage_dealt += static_cast<uint16_t>(dmg_r);
                    // P0-4: Damage history for Reversal/Flail.
                    hook_on_damage_received(target, dmg_r, static_cast<uint8_t>(md->category));
                }
                return MoveExecutionResult::Success;
            }
            default:
                const_dmg = static_cast<int32_t>(md->power);
                break;
        }
        // Apply constant damage (type matchup resets for most constant-damage moves).
        const_dmg = std::max(1, const_dmg);
        // P0-1: Protect check (Crystal: constantdamage â†’ checkhit which checks Protect).
        if (target.has_volatile(VolatileStatus::Protect)) {
            message((user_is_player ? std::string("Opponent") : std::string("Player"))
                    + " protected itself!");
            return MoveExecutionResult::Miss;
        }
        animate(md->animation_id, user_is_player ? 0u:1u, user_is_player ? 1u:0u);
        const int16_t old_hp_c = target.stats.hp;
        // P0-1: Substitute absorbs constant-damage moves.
        if (target.has_volatile(VolatileStatus::Substitute)) {
            const uint16_t sub_dmg = std::min(
                static_cast<uint16_t>(const_dmg),
                static_cast<uint16_t>(target.substitute_hp));
            target.substitute_hp = static_cast<uint16_t>(target.substitute_hp - sub_dmg);
            if (target.substitute_hp == 0) {
                target.clear_volatile(VolatileStatus::Substitute);
                message((user_is_player ? std::string("Opponent") : std::string("Player"))
                        + "'s substitute faded!");
            }
            outcome_.damage_dealt += static_cast<uint16_t>(const_dmg);
            return MoveExecutionResult::Success;
        }
        target.stats.hp = static_cast<int16_t>(std::max(0, static_cast<int32_t>(target.stats.hp) - const_dmg));
        // P0-2: Endure 1-HP floor for constant-damage moves.
        if (target.has_volatile(VolatileStatus::Endure) && target.stats.hp <= 0) {
            target.stats.hp = 1;
            message((user_is_player ? std::string("Opponent") : std::string("Player"))
                    + " endured the hit!");
        }
        hp_change(user_is_player ? 1u:0u, old_hp_c, target.stats.hp);
        outcome_.damage_dealt += static_cast<uint16_t>(const_dmg);
        // P0-4: Damage history for constant-damage moves.
        hook_on_damage_received(target, const_dmg, static_cast<uint8_t>(md->category));
        return MoveExecutionResult::Success;
    }

    // Ã¢â€â‚¬Ã¢â€â‚¬ Set-power computation (Magnitude, Present, Return, Frustration) Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
    uint8_t computed_power = md->power;  // default: use move's power field
    bool present_is_heal = false;

    if (effective_desc.set_power_source != SetPowerSource::None) {
        switch (effective_desc.set_power_source) {
            case SetPowerSource::MagnitudeTable: {
                const uint8_t rng_byte = rng_.next_byte();
                computed_power = rules_ ? rules_->get_magnitude_power(rng_byte) : uint8_t{70};
                message("Magnitude " + std::to_string(computed_power) + "!");
                break;
            }
            case SetPowerSource::PresentTable: {
                const uint8_t rng_byte = rng_.next_byte();
                if (rules_) {
                    const auto& entry = rules_->get_present_outcome(rng_byte);
                    if (entry.is_heal) {
                        present_is_heal = true;
                    } else {
                        computed_power = entry.power;
                    }
                } else {
                    // Fallback: 40% chance heal, 60% chance power 40
                    if (rng_byte < 51) { present_is_heal = true; }
                    else { computed_power = 40; }
                }
                if (present_is_heal) {
                    // Present heal: max(1, max_hp >> present_heal_shift)
                    const uint8_t shift = rules_ ? rules_->get_present_heal_shift() : uint8_t{2};
                    const int32_t heal = std::max(1, static_cast<int32_t>(target.stats.max_hp) >> shift);
                    const int16_t old_hp_p = target.stats.hp;
                    target.stats.hp = static_cast<int16_t>(
                        std::min(static_cast<int32_t>(target.stats.max_hp),
                                 static_cast<int32_t>(target.stats.hp) + heal));
                    if (target.stats.hp != old_hp_p) {
                        hp_change(user_is_player ? 1u:0u, old_hp_p, target.stats.hp);
                        message("Present healed the target!");
                    }
                    return MoveExecutionResult::Success;
                }
                break;
            }
            case SetPowerSource::HappinessReturn:
                // floor(happiness Ãƒâ€” 10 / 25), max 102
                computed_power = static_cast<uint8_t>(std::min(102,
                    static_cast<int32_t>(user.happiness) * 10 / 25));
                if (computed_power == 0) computed_power = 1;
                break;
            case SetPowerSource::HappinessFrustration:
                computed_power = static_cast<uint8_t>(std::min(102,
                    static_cast<int32_t>(255 - user.happiness) * 10 / 25));
                if (computed_power == 0) computed_power = 1;
                break;
            case SetPowerSource::HiddenPower: {
                // Derive type and power from DVs.
                // Source: suiCune engine/battle/hidden_power.c HiddenPowerDamage.
                // Source: pokecrystal engine/battle/hidden_power.asm HiddenPowerDamage.
                //
                // TYPE:
                //   type_raw = (def_dv & 3) | ((atk_dv & 3) << 2)  [4-bit, 0-15]
                //   type = type_raw + 1       (skip Normal=0)
                //   if type >= BIRD(6): type += 1      (skip Bird)
                //   if type >= UNUSED_TYPES_START(10): type += 10   (skip unused 10-19)
                //   Result sequence: 1,2,3,4,5,7,8,9,20,21,22,23,24,25,26,27
                //
                // POWER:
                //   p_bits = (atk&8) | ((def&8)>>1) | ((spd&8)>>2) | ((spc&8)>>3)  [bit 3 of each DV]
                //   power = ((p_bits * 5 + (spc_dv & 3)) >> 1) + 31   [range 31-70]
                {
                    // Type: uses low 2 bits of Attack and Defense only.
                    uint8_t hp_type = static_cast<uint8_t>(
                        (user.dv_def & 3u) | ((user.dv_atk & 3u) << 2u));
                    hp_type += 1u;               // skip Normal (0)
                    if (hp_type >= 6u)  hp_type += 1u;  // skip Bird (6)
                    if (hp_type >= 10u) hp_type += 10u; // skip unused (10-19)
                    effective_move_type = static_cast<TypeId>(hp_type);

                    // Power: uses bit 3 of each DV.
                    const uint8_t p_bits = static_cast<uint8_t>(
                          (user.dv_atk & 8u)
                        | ((user.dv_def & 8u) >> 1u)
                        | ((user.dv_spd & 8u) >> 2u)
                        | ((user.dv_spc & 8u) >> 3u));
                    const uint8_t spc_low2 = static_cast<uint8_t>(user.dv_spc & 3u);
                    computed_power = static_cast<uint8_t>(
                        ((static_cast<uint32_t>(p_bits) * 5u + spc_low2) >> 1u) + 31u);
                }
                break;
            }
            default:
                break;
        }
    }

    // After set_power_source: recompute type_eff if effective_move_type changed (Hidden Power).
    if (effective_move_type != md->type) {
        const bool identified = target.has_volatile(VolatileStatus::Identified);
        const uint8_t mt = static_cast<uint8_t>(effective_move_type);
        if (identified && (mt == 0u || mt == 1u)) {
            type_eff = get_combined_effectiveness(
                effective_move_type, target.type1, target.type2, registries_.type_chart);
            if (type_eff == 0) type_eff = 100;
        } else {
            type_eff = get_combined_effectiveness(
                effective_move_type, target.type1, target.type2, registries_.type_chart);
        }
        if (type_eff == 0) {
            message("It doesn't affect the opposing Pokemon...");
            return MoveExecutionResult::Immune;
        }
    }

    // Snore: fails when user is not asleep (requires_user_asleep).
    // Must be checked in the damaging path (Snore has has_standard_damage=true, not Status-only).
    // Source: Crystal BattleCommand_Snore -- fail if user != asleep.
    if (effective_desc.requires_user_asleep && user.status != Status::Sleep) {
        message(md->name + " -- failed! User isn't asleep.");
        return MoveExecutionResult::Miss;
    }

    // Delegate the damaging path to execute_move_damaging to keep execute_move within
    // MSVC function-size limits (avoids ICE on large functions).
    return execute_move_damaging(user, target, md, move_slot, user_is_player,
                                 effective_desc, effective_move_type, type_eff,
                                 computed_power);
}

// ============================================================================
// execute_move_damaging: the standard damaging path after type and power setup
// ============================================================================
MoveExecutionResult Battle::execute_move_damaging(
    BattlePokemon& user, BattlePokemon& target,
    const MoveData* md, size_t move_slot, bool user_is_player,
    const SemanticEffectDescription& effective_desc,
    TypeId effective_move_type, uint16_t type_eff,
    uint8_t computed_power) {

    // Ã¢â€â‚¬Ã¢â€â‚¬ Dream Eater: requires target asleep Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
    if (effective_desc.drain_requires_sleep && target.status != Status::Sleep) {
        message(md->name + " Ã¢â‚¬â€ the target isn't asleep!");
        return MoveExecutionResult::Miss;
    }

    // Ã¢â€â‚¬Ã¢â€â‚¬ Critical hit Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
    uint8_t crit_stage = 0;
    if (rules_) crit_stage = build_crit_stage(user, *md, *rules_);
    const bool is_crit = rules_
        ? roll_critical(crit_stage, rng_.next_byte(), *rules_)
        : roll_critical(crit_stage, rng_.next_byte());

    // Ã¢â€â‚¬Ã¢â€â‚¬ STAB Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
    const bool stab = (effective_move_type == user.type1 || effective_move_type == user.type2);

    // Ã¢â€â‚¬Ã¢â€â‚¬ Stat selection Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
    const int8_t eff_atk_stage  = (is_crit && user.stages.attack < 0)            ? 0 : user.stages.attack;
    const int8_t eff_def_stage  = (is_crit && target.stages.defense > 0)          ? 0 : target.stages.defense;
    const int8_t eff_satk_stage = (is_crit && user.stages.special_attack < 0)    ? 0 : user.stages.special_attack;
    const int8_t eff_sdef_stage = (is_crit && target.stages.special_defense > 0) ? 0 : target.stages.special_defense;

    const bool physical = (md->category == MoveCategory::Physical);
    auto ss = [this](int32_t b, int8_t s) {
        return rules_ ? apply_stat_stage(b, s, *rules_) : apply_stat_stage(b, s);
    };

    int32_t atk_stat, def_stat;
    if (physical) {
        atk_stat = ss(user.base_stats.attack,    eff_atk_stage);
        def_stat = ss(target.base_stats.defense, eff_def_stage);
        if ( user_is_player && field_.reflect_opponent > 0) def_stat *= 2;
        if (!user_is_player && field_.reflect_player   > 0) def_stat *= 2;
    } else {
        atk_stat = ss(user.base_stats.special_attack,   eff_satk_stage);
        def_stat = ss(target.base_stats.special_defense, eff_sdef_stage);
        if ( user_is_player && field_.light_screen_opponent > 0) def_stat *= 2;
        if (!user_is_player && field_.light_screen_player   > 0) def_stat *= 2;
    }
    const bool burned = physical && (user.status == Status::Burn);

    // Ã¢â€â‚¬Ã¢â€â‚¬ Selfdestruct defense halving Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
    // Source: BattleCommand_DamageCalc Ã¢â‚¬â€ `srl c` halves defender's defense.
    // defense_shift=1 Ã¢â€ â€™ def_stat = max(1, def_stat >> 1)
    if (effective_desc.user_faints && rules_) {
        const uint8_t ds = rules_->get_selfdestruct_def_shift();
        if (ds > 0) def_stat = std::max(1, def_stat >> ds);
    } else if (effective_desc.user_faints) {
        def_stat = std::max(1, def_stat / 2);
    }

    // Ã¢â€â‚¬Ã¢â€â‚¬ Damage calculation Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
    DamageParams dp{};
    dp.attacker_level     = user.level;
    dp.attack_stat        = atk_stat;
    dp.defense_stat       = def_stat;
    dp.move_power         = computed_power;
    dp.type_effectiveness = 100;
    dp.stab               = false;
    dp.critical           = is_crit;
    dp.burned             = burned;
    dp.weather            = field_.weather;
    dp.move_type          = effective_move_type;

    int32_t damage = rules_ ? enginemon::calculate_damage(dp, *rules_) : enginemon::calculate_damage(dp);
    if (damage == 0) return MoveExecutionResult::Immune;

    // Weather modifier
    // NOTE: apply_weather_modifier takes md->effect_id for the 'weather Ã— move effect'
    // table lookup (WeatherMoveModifiers). The one existing entry in that table is
    // {weather=Rain, effect=SolarBeam_Crystal_raw=0x97, mult=0.5x}, which maps to
    // ai_classification=SemEffect::Unknown=0.  The lookup effect_id==0x97 will never
    // match SemEffect::Unknown, so the SolarBeam rain penalty is currently dead.
    // MIGRATION DEBT: this table entry must be migrated to a semantic identity
    // (e.g. a dedicated SemEffect::SolarBeam) before Solar Beam support is added.
    // It cannot affect currently unsupported Solar Beam execution because Solar Beam
    // has is_supported=false in the compiled package (charge mechanic is deferred).
    if (rules_ && field_.weather != Weather::None) {
        damage = apply_weather_modifier(damage,
            static_cast<uint8_t>(field_.weather),
            static_cast<uint8_t>(effective_move_type),
            md->effect_id, *rules_);
    }

    // STAB
    if (stab) {
        damage += damage / 2;
        if (damage > 999) damage = 999;
        if (damage < 2)   damage = 2;
    }

    // Type effectiveness
    if (type_eff != 100) {
        damage = damage * static_cast<int32_t>(type_eff) / 100;
        if (damage < 1) damage = 1;
        if (damage > 999) damage = 999;
    }

    // Ã¢â€â‚¬Ã¢â€â‚¬ Conditional double damage Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
    if (effective_desc.conditional_double != ConditionalDoubleCondition::None) {
        bool double_it = false;
        switch (effective_desc.conditional_double) {
            case ConditionalDoubleCondition::TargetFlying:
                // Source: suiCune BattleCommand_DoubleFlyingDamage — checks SUBSTATUS_FLYING on opponent.
                // VolatileStatus::Flying is set by Fly/Sky Attack charge; cleared on fire turn.
                double_it = target.has_volatile(VolatileStatus::Flying);
                break;
            case ConditionalDoubleCondition::TargetUnderground:
                // Source: suiCune BattleCommand_DoubleUndergroundDamage — checks SUBSTATUS_UNDERGROUND.
                // VolatileStatus::Underground is set by Dig charge; cleared on fire turn.
                double_it = target.has_volatile(VolatileStatus::Underground);
                break;
            case ConditionalDoubleCondition::TargetMinimized:
                // Source: suiCune BattleCommand_DoubleMinimizeDamage — checks wPlayerMinimized/wEnemyMinimized.
                // VolatileStatus::Minimized is set by Minimize; NOT set by Double Team.
                double_it = target.has_volatile(VolatileStatus::Minimized);
                break;
            default: break;
        }
        if (double_it) {
            damage = std::min(999, damage * 2);
        }
    }

    // Ã¢â€â‚¬Ã¢â€â‚¬ Cannot KO (False Swipe) Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
    if (effective_desc.cannot_ko) {
        const int32_t max_dmg = static_cast<int32_t>(target.stats.hp) - 1;
        if (max_dmg < 1) {
            // Target already at 1 HP Ã¢â‚¬â€ move fails to do damage.
            message(md->name + " Ã¢â‚¬â€ the target barely hung on!");
            return MoveExecutionResult::Miss;
        }
        damage = std::min(damage, max_dmg);
    }

    // Damage variation
    const uint8_t var_threshold = rules_ ? rules_->get_damage_var_lower_bound() : uint8_t{0xD9};
    const int32_t var_divisor   = rules_ ? static_cast<int32_t>(rules_->get_damage_var_divisor()) : int32_t{255};
    uint8_t variation;
    do {
        uint8_t r = rng_.next_byte();
        variation = (r >> 1) | (r << 7);
    } while (variation < var_threshold);
    const int32_t safe_div = (var_divisor > 0) ? var_divisor : 255;
    damage = damage * static_cast<int32_t>(variation) / safe_div;
    if (damage < 2) damage = 2;

    if (is_crit)        message("A critical hit!");
    if (type_eff > 100) message("It's super effective!");
    else if (type_eff < 100) message("It's not very effectiveÃ¢â‚¬Â¦");

    animate(md->animation_id, user_is_player ? 0u:1u, user_is_player ? 1u:0u);

    // Ã¢â€â‚¬Ã¢â€â‚¬ Apply damage Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
    // -- P0-1: Protect interception (A-path) ----------------------------------
    // Crystal: BattleCommand_CheckHit returns miss if target has Protect.
    if (target.has_volatile(VolatileStatus::Protect)) {
        message((user_is_player ? std::string("Opponent") : std::string("Player"))
                + " protected itself!");
        return MoveExecutionResult::Miss;
    }

    // -- P0-3: Substitute routing (A-path) ------------------------------------
    // Crystal: if target has Substitute, damage goes to substitute_hp, not HP.
    if (target.has_volatile(VolatileStatus::Substitute) && target.substitute_hp > 0) {
        if (damage >= static_cast<int32_t>(target.substitute_hp)) {
            target.substitute_hp = 0;
            target.clear_volatile(VolatileStatus::Substitute);
            message((user_is_player ? std::string("Opponent") : std::string("Player"))
                    + "'s substitute broke!");
        } else {
            target.substitute_hp = static_cast<uint16_t>(
                target.substitute_hp - static_cast<uint16_t>(damage));
        }
        // Damage absorbed by Substitute -- real HP unchanged.
        outcome_.damage_dealt += static_cast<uint16_t>(damage);
        // Do NOT call hook_on_damage_received: Crystal skips accumulation for Substitute hits.
        return MoveExecutionResult::Success;
    }

    // -- Apply damage ---------------------------------------------------------
    const int16_t old_hp = target.stats.hp;
    target.stats.hp = static_cast<int16_t>(
        std::max(0, static_cast<int32_t>(target.stats.hp) - damage));

    // -- P0-2: Endure 1-HP floor (A-path) -------------------------------------
    if (target.has_volatile(VolatileStatus::Endure) && target.stats.hp <= 0) {
        target.stats.hp = 1;
        message((user_is_player ? std::string("Opponent") : std::string("Player"))
                + " endured the hit!");
    }

    hp_change(user_is_player ? 1u : 0u, old_hp, target.stats.hp);
    outcome_.damage_dealt += static_cast<uint16_t>(damage);

    // -- Pay Day coin scatter (A-path) -----------------------------------------
    // Crystal BattleCommand_PayDay: coins = user_level * 2, added to running total.
    if (effective_desc.has_payday) {
        const uint32_t coins = static_cast<uint32_t>(user.level) * 2u;
        if (user_is_player) player_payday_coins_ += coins; else opponent_payday_coins_ += coins;
        user.payday_coins += coins;
    }

    // -- P0-4: Damage history (A-path) ----------------------------------------
    hook_on_damage_received(target, damage, static_cast<uint8_t>(md->category));

    // DestinyBond: if target just fainted from this A-path damage hit.
    if (target.stats.hp <= 0 && target.has_volatile(VolatileStatus::DestinyBond)) {
        hook_destiny_bond_check(target, user, !user_is_player);
    }

    // Ã¢â€â‚¬Ã¢â€â‚¬ Recoil Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
    if (effective_desc.has_recoil && !user.is_fainted()) {
        const uint8_t shift = rules_ ? rules_->get_recoil_shift() : uint8_t{2};
        const int32_t recoil_dmg = (shift > 0) ? std::max(1, damage >> shift) : 1;
        const int16_t old_user_hp = user.stats.hp;
        user.stats.hp = static_cast<int16_t>(std::max(0, static_cast<int32_t>(user.stats.hp) - recoil_dmg));
        hp_change(user_is_player ? 0u : 1u, old_user_hp, user.stats.hp);
        message((user_is_player ? std::string("Player") : std::string("Opponent")) + " is hurt by recoil!");
    }

    // Ã¢â€â‚¬Ã¢â€â‚¬ Drain / Dream Eater Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
    if (effective_desc.has_drain && !user.is_fainted()) {
        const uint8_t shift = rules_ ? rules_->get_drain_shift() : uint8_t{1};
        const int32_t heal_amt = (shift > 0) ? std::max(1, damage >> shift) : 1;
        const int16_t old_user_hp = user.stats.hp;
        user.stats.hp = static_cast<int16_t>(
            std::min(static_cast<int32_t>(user.stats.max_hp),
                     static_cast<int32_t>(user.stats.hp) + heal_amt));
        if (user.stats.hp != old_user_hp) {
            hp_change(user_is_player ? 0u : 1u, old_user_hp, user.stats.hp);
            message((user_is_player ? std::string("Player") : std::string("Opponent")) + " drained HP!");
        }
    }

    // Ã¢â€â‚¬Ã¢â€â‚¬ Selfdestruct / Explosion Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
    if (effective_desc.user_faints && !user.is_fainted()) {
        // Clear user status, remove Leech Seed and Destiny Bond substatuses.
        user.status = Status::None;
        user.status_turns = 0;
        user.clear_volatile(VolatileStatus::Seeded);
        // Faint user.
        const int16_t old_user_hp2 = user.stats.hp;
        user.stats.hp = 0;
        if (old_user_hp2 != 0) {
            hp_change(user_is_player ? 0u : 1u, old_user_hp2, user.stats.hp);
        }
        message((user_is_player ? std::string("Player") : std::string("Opponent")) + " fainted from its own attack!");
    }

    // Ã¢â€â‚¬Ã¢â€â‚¬ Hyper Beam recharge Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
    if (effective_desc.sets_recharge && !user.is_fainted()) {
        user.recharge_turns = 1;
        message((user_is_player ? std::string("Player") : std::string("Opponent"))
                + " must recharge!");
    }

    // -- Secondary effect (effectchance roll) -------------------------------------------------
    if (effective_desc.secondary_effect != SecondaryEffectType::None
        && !target.is_fainted()
        && md->effect_chance > 0) {
        // Crystal: call BattleRandom; cp [hl] (MOVE_CHANCE); ret c
        // fires iff BattleRandom < MOVE_CHANCE.
        // effect_chance is the raw Crystal ROM byte (0-255), already pre-converted by the
        // percent RGBDS macro (e.g. "20 percent" assembles to 51). Direct comparison; no scaling.
        const bool fires = (rng_.next_byte() < static_cast<uint32_t>(md->effect_chance));
        if (fires) {
            apply_secondary_effect(user, target, effective_desc.secondary_effect, user_is_player);
        }
    }
    // Ã¢â€â‚¬Ã¢â€â‚¬ Stat change on hit (DefenseUpHit, AttackDownHit, etc.) Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
    if (effective_desc.stat_change != StatChangeTarget::None) {
        // For hit-effect stat changes (applied to target unconditionally on hit).
        apply_stat_change(user, target, effective_desc.stat_change, user_is_player);
    }

    // Ã¢â€â‚¬Ã¢â€â‚¬ Hazard clearing (Rapid Spin) Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
    if (effective_desc.clears_hazards) {
        if (user_is_player) field_.spikes_player = false;
        else                field_.spikes_opponent = false;
    }

    return MoveExecutionResult::Success;
}

// ============================================================================
// Helper: apply a secondary effect to the target
// Source: BattleCommand_PoisonTarget/BurnTarget/etc. + effectchance framework
// ============================================================================
void Battle::apply_secondary_effect(BattlePokemon& user, BattlePokemon& target,
                                    SecondaryEffectType effect, bool user_is_player) {
    const bool target_has_safeguard =
        user_is_player ? (field_.safeguard_opponent > 0) : (field_.safeguard_player > 0);

    switch (effect) {
        case SecondaryEffectType::Burn:
            if (target.status == Status::None && !target_has_safeguard) {
                target.status = Status::Burn;
                message((user_is_player ? std::string("Opponent") : std::string("Player"))
                        + " was burned!");
            }
            break;
        case SecondaryEffectType::Freeze:
            if (target.status == Status::None && !target_has_safeguard) {
                target.status = Status::Freeze;
                message((user_is_player ? std::string("Opponent") : std::string("Player"))
                        + " was frozen!");
            }
            break;
        case SecondaryEffectType::Paralysis:
            if (target.status == Status::None && !target_has_safeguard) {
                target.status = Status::Paralysis;
                message((user_is_player ? std::string("Opponent") : std::string("Player"))
                        + " was paralyzed!");
            }
            break;
        case SecondaryEffectType::Poison:
            if (target.status == Status::None && !target_has_safeguard) {
                target.status = Status::Poison;
                message((user_is_player ? std::string("Opponent") : std::string("Player"))
                        + " was poisoned!");
            }
            break;
        case SecondaryEffectType::Flinch:
            // Flinch causes the target to miss its turn this turn.
            target.set_volatile(VolatileStatus::Flinch);
            break;
        case SecondaryEffectType::Confusion:
            if (!target.has_volatile(VolatileStatus::Confusion) && !target_has_safeguard) {
                target.set_volatile(VolatileStatus::Confusion);
                message((user_is_player ? std::string("Opponent") : std::string("Player"))
                        + " became confused!");
            }
            break;
        case SecondaryEffectType::AttackDown:
            apply_one_stage_change(target, 0, -1);
            break;
        case SecondaryEffectType::DefenseDown:
            apply_one_stage_change(target, 1, -1);
            break;
        case SecondaryEffectType::SpeedDown:
            apply_one_stage_change(target, 2, -1);
            break;
        case SecondaryEffectType::SpAtkDown:
            apply_one_stage_change(target, 3, -1);
            break;
        case SecondaryEffectType::SpDefDown:
            apply_one_stage_change(target, 4, -1);
            break;
        case SecondaryEffectType::AccuracyDown:
            apply_one_stage_change(target, 5, -1);
            break;
        case SecondaryEffectType::EvasionDown:
            apply_one_stage_change(target, 6, -1);
            break;
        case SecondaryEffectType::AttackUp:
            apply_one_stage_change(user, 0, +1);
            break;
        case SecondaryEffectType::DefenseUp:
            apply_one_stage_change(user, 1, +1);
            break;
        case SecondaryEffectType::AllStatsUp:
            // All stats up by 1 Ã¢â‚¬â€ AncientPower, Silver Wind, Ominous Wind
            for (int s = 0; s < 5; ++s) apply_one_stage_change(user, s, +1);
            break;
        case SecondaryEffectType::Defrost:
            // FlameWheel / SacredFire: thaw user if frozen before burn proc
            if (user.status == Status::Freeze) {
                user.status = Status::None;
                message((user_is_player ? std::string("Player") : std::string("Opponent"))
                        + " thawed out!");
            }
            break;
        case SecondaryEffectType::TriAttack:
            // TriAttack: 1/3 chance each burn/freeze/paralysis
            if (target.status == Status::None && !target_has_safeguard) {
                const uint8_t which = rng_.next_byte() % 3;
                if (which == 0) { target.status = Status::Burn;     message("Burn!"); }
                else if (which == 1) { target.status = Status::Freeze; message("Freeze!"); }
                else { target.status = Status::Paralysis;           message("Paralysis!"); }
            }
            break;
        default:
            break;
    }
    (void)user;
}

// ============================================================================
// Helper: apply a single stat stage change
// ============================================================================
void Battle::apply_one_stage_change(BattlePokemon& mon, int stat_idx, int8_t delta) {
    // Mist: blocks opponent-inflicted stat drops (negative delta).
    // Source: Crystal BattleCommand_LowerStat -- checks SUBSTATUS_MIST before applying.
    if (delta < 0 && mon.has_volatile(VolatileStatus::Mist)) {
        message("Mist protected the Pokemon from stat changes!");
        return;
    }
    auto clamp6 = [](int8_t v) { return static_cast<int8_t>(std::clamp(static_cast<int>(v), -6, 6)); };
    switch (stat_idx) {
        case 0: mon.stages.attack         = clamp6(mon.stages.attack         + delta); break;
        case 1: mon.stages.defense        = clamp6(mon.stages.defense        + delta); break;
        case 2: mon.stages.speed          = clamp6(mon.stages.speed          + delta); break;
        case 3: mon.stages.special_attack = clamp6(mon.stages.special_attack + delta); break;
        case 4: mon.stages.special_defense= clamp6(mon.stages.special_defense+ delta); break;
        case 5: mon.stages.accuracy       = clamp6(mon.stages.accuracy       + delta); break;
        case 6: mon.stages.evasion        = clamp6(mon.stages.evasion        + delta); break;
    }
    apply_stat_stages(mon);
}

// ============================================================================
// Helper: apply a SemanticStatChangeTarget to the correct mon
// ============================================================================
void Battle::apply_stat_change(BattlePokemon& user, BattlePokemon& target,
                               StatChangeTarget change, bool user_is_player) {
    using SC = StatChangeTarget;
    // Determine who is affected (user vs opponent) and by how much.
    // Stat-up: affects user. Stat-down: affects target.
    // Special cases: Reset, CopyOpponent, MaxAttack.
    switch (change) {
        case SC::AttackUp1:   apply_one_stage_change(user,  0, +1); break;
        case SC::AttackUp2:   apply_one_stage_change(user,  0, +2); break;
        case SC::DefenseUp1:  apply_one_stage_change(user,  1, +1); break;
        case SC::DefenseUp2:  apply_one_stage_change(user,  1, +2); break;
        case SC::SpeedUp1:    apply_one_stage_change(user,  2, +1); break;
        case SC::SpeedUp2:    apply_one_stage_change(user,  2, +2); break;
        case SC::SpAtkUp1:    apply_one_stage_change(user,  3, +1); break;
        case SC::SpAtkUp2:    apply_one_stage_change(user,  3, +2); break;
        case SC::SpDefUp1:    apply_one_stage_change(user,  4, +1); break;
        case SC::SpDefUp2:    apply_one_stage_change(user,  4, +2); break;
        case SC::AccuracyUp1: apply_one_stage_change(user,  5, +1); break;
        case SC::AccuracyUp2: apply_one_stage_change(user,  5, +2); break;
        case SC::EvasionUp1:  apply_one_stage_change(user,  6, +1); break;
        case SC::EvasionUp2:  apply_one_stage_change(user,  6, +2); break;
        case SC::AllUp1:
            for (int s = 0; s < 5; ++s) apply_one_stage_change(user, s, +1);
            break;
        case SC::AttackDown1:   apply_one_stage_change(target, 0, -1); break;
        case SC::AttackDown2:   apply_one_stage_change(target, 0, -2); break;
        case SC::DefenseDown1:  apply_one_stage_change(target, 1, -1); break;
        case SC::DefenseDown2:  apply_one_stage_change(target, 1, -2); break;
        case SC::SpeedDown1:    apply_one_stage_change(target, 2, -1); break;
        case SC::SpeedDown2:    apply_one_stage_change(target, 2, -2); break;
        case SC::SpAtkDown1:    apply_one_stage_change(target, 3, -1); break;
        case SC::SpAtkDown2:    apply_one_stage_change(target, 3, -2); break;
        case SC::SpDefDown1:    apply_one_stage_change(target, 4, -1); break;
        case SC::SpDefDown2:    apply_one_stage_change(target, 4, -2); break;
        case SC::AccuracyDown1: apply_one_stage_change(target, 5, -1); break;
        case SC::AccuracyDown2: apply_one_stage_change(target, 5, -2); break;
        case SC::EvasionDown1:  apply_one_stage_change(target, 6, -1); break;
        case SC::EvasionDown2:  apply_one_stage_change(target, 6, -2); break;
        case SC::Reset:
            // Haze: reset all stages on both PokÃƒÂ©mon.
            user.stages   = {};
            target.stages = {};
            apply_stat_stages(user);
            apply_stat_stages(target);
            message("All stat changes were eliminated!");
            break;
        case SC::CopyOpponent:
            // Psych Up: copy all opponent stat stages.
            user.stages = target.stages;
            apply_stat_stages(user);
            message((user_is_player ? std::string("Player") : std::string("Opponent"))
                    + " copied stat changes!");
            break;
        case SC::MaxAttack:
            // Belly Drum: Crystal vanilla bug â€” AttackUp2 fires BEFORE the HP check.
            // Source: pokecrystal engine/battle/effect_commands.asm BattleCommand_BellyDrum.
            // Step 1: Raise Attack to +6 unconditionally.
            user.stages.attack = 6;
            apply_stat_stages(user);
            // Step 2: HP cost -- only if HP > half max (the check comes AFTER the raise).
            {
                const int32_t cost = std::max(1, static_cast<int32_t>(user.stats.max_hp) / 2);
                if (user.stats.hp > cost) {
                    const int16_t old_hp_b = user.stats.hp;
                    user.stats.hp = static_cast<int16_t>(user.stats.hp - cost);
                    hp_change(user_is_player ? 0u : 1u, old_hp_b, user.stats.hp);
                    message((user_is_player ? std::string("Player") : std::string("Opponent"))
                            + " cut its own HP to max out its Attack!");
                } else {
                    // HP <= half: attack was already raised (vanilla bug), HP not deducted.
                    message((user_is_player ? std::string("Player") : std::string("Opponent"))
                            + " maxed out its Attack!");
                }
            }
            break;
        default:
            break;
    }
    (void)user_is_player;
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

    // Natural thaw: P1-Freeze. 25/256 chance per turn (after freeze_guard is consumed).
    // Source: Crystal HandleDefrost in core.asm.
    hook_end_of_turn_natural_thaw(player_pokemon_,   true);
    hook_end_of_turn_natural_thaw(opponent_pokemon_, false);

    // Architecture B: end-of-turn hooks.
    hook_end_of_turn_leech_seed();
    hook_end_of_turn_nightmare();
    hook_end_of_turn_curse();
    hook_end_of_turn_perish_song();
    hook_future_sight_tick();
    hook_trap_damage_tick();
    // NOTE: hook_rampage_end_check is NOT called here.
    // Rampage counter decrement and volatile clearing are handled by the Rampage gate
    // at the start of execute_program() on continuation turns. Calling the hook here
    // would double-decrement and clear Rampage one turn too early.
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

    // Sandstorm residual: 1/8 max HP to non-Rock, non-Ground, non-Steel types.
    // Source: Crystal HandleSandstorm -- checks type matchup.
    // Rock=5, Ground=4, Steel=9 are immune (Crystal type_constants.asm).
    if (field_.weather == Weather::Sandstorm && !bp.is_fainted()) {
        const TypeId t1 = bp.type1, t2 = bp.type2;
        const bool immune = (t1 == 4 || t1 == 5 || t1 == 9 ||
                             t2 == 4 || t2 == 5 || t2 == 9);
        if (!immune) {
            const int32_t sand_denom = 8;
            const int16_t sand_dmg = static_cast<int16_t>(
                std::max(1, static_cast<int32_t>(bp.stats.max_hp) / sand_denom));
            const int16_t old_hp_s = bp.stats.hp;
            bp.stats.hp = static_cast<int16_t>(
                std::max(0, static_cast<int32_t>(bp.stats.hp) - sand_dmg));
            hp_change(is_player ? 0u : 1u, old_hp_s, bp.stats.hp);
            message(std::string(side) + " is buffeted by the sandstorm!");
            return;  // Sandstorm damage replaces / occurs independently of status damage
        }
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
        // Crystal formula: ComputeTrainerReward = base_reward â”œÃ¹ wCurPartyLevel
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

    // Write back outgoing Pokemon's battle state to the party slot before switching.
    // Source: Crystal SavePlayerMon / WritebackPlayerMon -- HP/PP/status written back.
    Pokemon* outgoing = player_party_.get(old_slot);
    if (outgoing) {
        outgoing->current_hp  = static_cast<uint16_t>(std::max(0, static_cast<int32_t>(player_pokemon_.stats.hp)));
        outgoing->status      = player_pokemon_.status;
        outgoing->held_item   = player_pokemon_.held_item;
        for (size_t i = 0; i < 4; ++i) {
            outgoing->moves[i].pp = player_pokemon_.moves[i].pp;
        }
    }

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

    // Spikes entry damage for player switching in.
    // Source: Crystal CheckEntryHazards.
    if (field_.spikes_player && !player_pokemon_.is_fainted()) {
        const TypeId t1 = player_pokemon_.type1, t2 = player_pokemon_.type2;
        const bool flying = (t1 == 2 || t2 == 2);
        if (!flying) {
            const int16_t old_hp_spk = player_pokemon_.stats.hp;
            const int16_t spike_dmg  = static_cast<int16_t>(
                std::max(1, static_cast<int32_t>(player_pokemon_.stats.max_hp) / 8));
            player_pokemon_.stats.hp = static_cast<int16_t>(
                std::max(0, static_cast<int32_t>(player_pokemon_.stats.hp) - spike_dmg));
            hp_change(0u, old_hp_spk, player_pokemon_.stats.hp);
            message("Player was hurt by Spikes!");
        }
    }
}

void Battle::force_switch_opponent(size_t party_slot) {
    if (party_slot >= opponent_party_.size()) return;
    const size_t old_slot = opponent_active_index_;
    opponent_party_[old_slot] = opponent_pokemon_;
    opponent_active_index_    = party_slot;
    opponent_pokemon_         = opponent_party_[party_slot];
    opponent_pokemon_.volatile_status = 0;  // Clear volatile on switch
    switched(1u, old_slot, party_slot);
    message("Opponent sent out a new Pok\u00e9mon!");

    // Spikes entry damage (Gen 2: one layer only, 1/8 max HP, Flying immune).
    // Source: Crystal CheckEntryHazards -- SUBSTATUS_SPIKES check.
    // Flying type (type ID 2) is immune.
    if (field_.spikes_opponent && !opponent_pokemon_.is_fainted()) {
        const TypeId t1 = opponent_pokemon_.type1, t2 = opponent_pokemon_.type2;
        const bool flying = (t1 == 2 || t2 == 2);
        if (!flying) {
            const int16_t old_hp_spk = opponent_pokemon_.stats.hp;
            const int16_t spike_dmg  = static_cast<int16_t>(
                std::max(1, static_cast<int32_t>(opponent_pokemon_.stats.max_hp) / 8));
            opponent_pokemon_.stats.hp = static_cast<int16_t>(
                std::max(0, static_cast<int32_t>(opponent_pokemon_.stats.hp) - spike_dmg));
            hp_change(1u, old_hp_spk, opponent_pokemon_.stats.hp);
            message("Opponent was hurt by Spikes!");
        }
    }
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
                      : capture_wobble_chance(final_rate));  // wobble count for animation Î“Ã‡Ã¶ not yet rendered
        result_ = BattleResult::Captured;
        outcome_.captured_species = opponent_pokemon_.species;
        message("Gotcha! Pokâ”œâŒmon was caught!");
        return true;
    }
    message("Oh no! The Pokâ”œâŒmon broke free!");
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
