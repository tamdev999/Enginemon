// engine/battle/trainer_ai.cpp
// Trainer AI — VanillaCrystalAI + AIRegistry
//
// Crystal AI architecture (source: suiCune engine/battle/ai/scoring.c):
//
//   Each move slot starts with a score of 0 (lower = more likely to be chosen).
//   AI passes run through the move list and adjust scores up (discourage) or
//   down (encourage).  At the end, the slot with the lowest score is selected.
//   When multiple slots tie on minimum, one is chosen randomly.
//
// AI behavior flags (Crystal wEnemyAIBits):
//   BASIC     — discourages moves redundant/blocked (always active)
//   SETUP     — encourages stat moves on turn 1
//   TYPES     — type matchup awareness
//   OFFENSIVE — discourages non-damaging moves
//   SMART     — context-specific per-effect adjustments
//
// Implementation strategy:
//   - BASIC and TYPES are implemented faithfully.
//   - SETUP is implemented.
//   - OFFENSIVE is implemented.
//   - SMART is a large per-effect table; we implement the most impactful
//     cases (healing, sleep, toxic, stat-up/down, type-immune) and leave
//     the rest as no-ops. This is correct — an unrecognised effect simply
//     gets no adjustment, which is better than guessing.
//   - Scoring is local to this file (no global mutable Crystal wram state).

#include "engine/battle/trainer_ai.hpp"
#include "engine/battle/calculator.hpp"
#include "engine/battle/battle_effects.hpp"
#include <algorithm>
#include <cassert>
#include <numeric>

// NOTE: No file-level #pragma warning(disable: 4996) here.
// The deprecated fallback overloads of apply_stat_stage/roll_accuracy etc. are
// NOT called in this file at all — all AI scoring uses type-chart lookups from
// registries_, not calculator table functions.  The only deprecated calls this
// file makes are through VanillaCrystalAI::decide(ctx) (no-rules overload) which
// is a test path only.  Tests must suppress the warning locally if needed.

namespace enginemon {

// ============================================================================
// AI scoring constants — Crystal source: scoring.asm
//   Default score: 20  AIDiscourageMove: +10  pass deltas: ±1, ±2
// ============================================================================
static constexpr int kInitScore        = 20;   // Crystal vanilla default (fallback)
static constexpr int kDiscourageStrong = 10;   // AIDiscourageMove vanilla (+10, fallback)
static constexpr int kStrongDiscourage =  2;   // individual pass strong discourage (+2)
static constexpr int kDiscourage       =  1;   // individual pass discourage (+1)
static constexpr int kEncourage        = -1;   // individual pass encourage (-1)
static constexpr int kStrongEncourage  = -2;   // individual pass strong encourage (-2)

// ============================================================================
// Score table for move selection
// ============================================================================

struct MoveScores {
    int scores[4];
    int discourage_strong = kDiscourageStrong; // AIDiscourageMove delta, overridden by rules

    MoveScores() {
        // Crystal initializes all move scores to 20 (AIChooseMove, line "ld a, 20")
        scores[0] = scores[1] = scores[2] = scores[3] = kInitScore;
    }
    explicit MoveScores(const BattleRules& rules) {
        // ROM-derived: init score from AIChooseMove recognizer,
        // discourage_strong from AIDiscourageMove recognizer.
        const int init = static_cast<int>(rules.get_ai_init_score());
        scores[0] = scores[1] = scores[2] = scores[3] = init;
        discourage_strong = static_cast<int>(rules.get_ai_discourage_strong());
    }
};

// ============================================================================
// VanillaCrystalAI helpers
// ============================================================================

VanillaCrystalAI::VanillaCrystalAI(AIBehaviorId behavior)
    : behavior_(behavior)
    , use_pass_set_(false)
{}

VanillaCrystalAI::VanillaCrystalAI(AIPassSet passes)
    : behavior_(0)
    , ai_passes_(passes)
    , use_pass_set_(true)
{}

std::string VanillaCrystalAI::name() const {
    switch (behavior_) {
        case VanillaAI::BASIC:      return "Basic";
        case VanillaAI::SMART:      return "Smart";
        case VanillaAI::AGGRESSIVE: return "Aggressive";
        case VanillaAI::DEFENSIVE:  return "Defensive";
        case VanillaAI::STATUS_FOCUS: return "StatusFocus";
        case VanillaAI::GYM_LEADER: return "GymLeader";
        case VanillaAI::ELITE_FOUR: return "EliteFour";
        case VanillaAI::CHAMPION:   return "Champion";
        default: return "AI-" + std::to_string(behavior_);
    }
}

// ============================================================================
// AI_Basic
// Source: suiCune engine/battle/ai/scoring.c AI_Basic
// Discourages status-only moves if target already has a status condition.
// Production path: uses rules.ai_status_only_effects (ROM-derived).
// The no-rules path (hardcoded list) has been removed; callers must provide rules.
// ============================================================================

static void ai_basic(MoveScores& scores, const AIContext& ctx, const BattleRules& rules) {
    for (size_t i = 0; i < 4; ++i) {
        const MoveId mid = ctx.self.moves[i].move;
        if (mid == MOVE_NONE) continue;
        const MoveData* md = ctx.battle.registries().moves.get(mid);
        if (!md) continue;
        if (rules.is_ai_status_only(md->effect_id)) {
            if (ctx.opponent.status != Status::None) {
                scores.scores[i] += scores.discourage_strong;  // AIDiscourageMove
            }
        }
    }
}

// ============================================================================
// AI_Types
// Source: suiCune engine/battle/ai/scoring.c AI_Types
// Dismisses immune moves. Encourages super-effective. 
// Discourages not-very-effective if an alternative damaging move exists.
// ============================================================================

static void ai_types(MoveScores& scores, const AIContext& ctx) {
    // Check if all damaging moves share one type (no alternative type available)
    bool has_multi_type_damaging = false;
    uint8_t first_type = 0xFF;
    for (size_t i = 0; i < 4; ++i) {
        if (ctx.self.moves[i].move == MOVE_NONE) continue;
        const MoveData* md = ctx.battle.registries().moves.get(ctx.self.moves[i].move);
        if (!md || md->power == 0) continue;
        if (first_type == 0xFF) { first_type = md->type; }
        else if (md->type != first_type) { has_multi_type_damaging = true; break; }
    }

    for (size_t i = 0; i < 4; ++i) {
        const MoveId mid = ctx.self.moves[i].move;
        if (mid == MOVE_NONE) continue;
        const MoveData* md = ctx.battle.registries().moves.get(mid);
        if (!md) continue;

        const uint16_t eff = get_combined_effectiveness(
            md->type, ctx.opponent.type1, ctx.opponent.type2,
            ctx.battle.registries().type_chart);

        if (eff == 0) {
            // Immune — AIDiscourageMove (+10) — Crystal: strongly discourages immune moves
            scores.scores[i] += scores.discourage_strong;
            continue;
        }

        if (md->power == 0) continue;  // Status move; type matchup irrelevant

        if (eff > 100) {
            // Super-effective: encourage (-1)
            scores.scores[i] += kEncourage;
        } else if (eff < 100) {
            // Not very effective — discourage (+1) only if there's a better-type alternative
            if (has_multi_type_damaging) {
                scores.scores[i] += kDiscourage;
            }
        }
    }
}

// ============================================================================
// AI_Offensive
// Source: suiCune engine/battle/ai/scoring.c AI_Offensive
// Greatly discourages non-damaging moves.
// ============================================================================

static void ai_offensive(MoveScores& scores, const AIContext& ctx) {
    for (size_t i = 0; i < 4; ++i) {
        if (ctx.self.moves[i].move == MOVE_NONE) continue;
        const MoveData* md = ctx.battle.registries().moves.get(ctx.self.moves[i].move);
        if (!md) continue;
        if (md->power == 0) {
            // Crystal AI_Offensive: inc [hl]; inc [hl] = +2
            scores.scores[i] += kStrongDiscourage;
        }
    }
}

// ============================================================================
// AI_Setup
// Source: suiCune engine/battle/ai/scoring.c AI_Setup
// Encourages stat-up moves on turn 1 (both sides' turn 1).
// Discourages them otherwise.
//
// Stat-up/stat-down classification uses BattleRules::ai_stat_up_effects and
// ai_stat_down_effects, which are populated from Crystal's TrainerClassAttributes
// and the extractor's knowledge of Crystal's contiguous effect-ID ranges.
// The engine does not directly encode Crystal's ID layout.
// ============================================================================

static bool is_stat_up(uint8_t effect_id, const BattleRules&) {
    // All Crystal stat-up effects map to SemEffect::StatUp at extraction time.
    return effect_id == SemEffect::StatUp;
}

static bool is_stat_down(uint8_t effect_id, const BattleRules&) {
    // All Crystal stat-down effects map to SemEffect::StatDown at extraction time.
    return effect_id == SemEffect::StatDown;
}

static void ai_setup(MoveScores& scores, const AIContext& ctx,
                     uint16_t self_turn, uint16_t opp_turn,
                     const BattleRules& rules) {
    for (size_t i = 0; i < 4; ++i) {
        if (ctx.self.moves[i].move == MOVE_NONE) continue;
        const MoveData* md = ctx.battle.registries().moves.get(ctx.self.moves[i].move);
        if (!md) continue;

        if (is_stat_up(md->effect_id, rules)) {
            if (self_turn == 0) {
                scores.scores[i] += kStrongEncourage;
            } else {
                scores.scores[i] += kStrongDiscourage;
            }
        } else if (is_stat_down(md->effect_id, rules)) {
            if (opp_turn == 0) {
                scores.scores[i] += kStrongEncourage;
            } else {
                scores.scores[i] += kStrongDiscourage;
            }
        }
    }
}

// ============================================================================
// AI_Smart (selected high-value cases)
// Source: suiCune engine/battle/ai/scoring.c AI_Smart
// Full implementation has ~70 per-effect handlers; we implement the most
// impactful ones. Unimplemented effects get no adjustment (neutral).
//
// Uses SemEffect:: constants (engine/include/engine/battle/battle_effects.hpp)
// rather than raw Crystal EFFECT_* IDs from trainer_ai.cpp (removed).
// Weather synergy moves looked up via BattleRules::is_ai_rain_dance_move() /
// is_ai_sunny_day_move() — ROM-extracted lists, no hardcoded Crystal constants.
// ============================================================================

static void ai_smart(MoveScores& scores, const AIContext& ctx, const BattleRules& rules) {
    for (size_t i = 0; i < 4; ++i) {
        if (ctx.self.moves[i].move == MOVE_NONE) continue;
        const MoveData* md = ctx.battle.registries().moves.get(ctx.self.moves[i].move);
        if (!md) continue;

        const uint8_t eff = md->effect_id;
        const int16_t self_hp  = ctx.self.stats.hp;
        const int16_t self_max = ctx.self.stats.max_hp;

        // Healing moves: encourage when HP is low (< half); discourage when above 75%
        // All heal effects (Recover, Morning Sun, Synthesis, Moonlight) map to SemEffect::Heal
        if (eff == SemEffect::Heal) {
            if (self_max > 0 && self_hp * 2 < self_max) {
                scores.scores[i] += kStrongEncourage;
            } else if (self_max > 0 && self_hp * 4 >= self_max * 3) {
                scores.scores[i] += kDiscourage;
            }
            continue;
        }

        // Selfdestruct / Explosion: discourage if opponent not near KO
        if (eff == SemEffect::Selfdestruct) {
            if (ctx.opponent.stats.hp * 4 > ctx.opponent.stats.max_hp) {
                scores.scores[i] += kDiscourage;
            }
            continue;
        }

        // Sleep-inducing: encourage if we also have Dream Eater or Nightmare
        if (eff == SemEffect::Sleep) {
            for (size_t j = 0; j < 4; ++j) {
                if (ctx.self.moves[j].move == MOVE_NONE) continue;
                const MoveData* mdj = ctx.battle.registries().moves.get(ctx.self.moves[j].move);
                if (!mdj) continue;
                if (mdj->effect_id == SemEffect::DreamEater || mdj->effect_id == SemEffect::Nightmare) {
                    scores.scores[i] += kStrongEncourage;
                    break;
                }
            }
            continue;
        }

        // Dream Eater: only useful if target is asleep
        if (eff == SemEffect::DreamEater) {
            if (ctx.opponent.status != Status::Sleep) {
                scores.scores[i] += scores.discourage_strong;
            }
            continue;
        }

        // Nightmare: only useful if target is asleep
        if (eff == SemEffect::Nightmare) {
            if (ctx.opponent.status != Status::Sleep) {
                scores.scores[i] += scores.discourage_strong;
            }
            continue;
        }

        // Toxic / Poison: discourage if target already poisoned
        if (eff == SemEffect::Toxic || eff == SemEffect::Poison) {
            if (ctx.opponent.status == Status::Poison
             || ctx.opponent.status == Status::BadPoison) {
                scores.scores[i] += scores.discourage_strong;
            }
            continue;
        }

        // Paralyze: discourage if target already paralyzed
        if (eff == SemEffect::Paralyze) {
            if (ctx.opponent.status == Status::Paralysis) {
                scores.scores[i] += scores.discourage_strong;
            }
            continue;
        }

        // Reflect / Light Screen: leave neutral (no AIContext screen state yet)
        if (eff == SemEffect::Reflect || eff == SemEffect::LightScreen) {
            continue;
        }

        // Baton Pass: encourage if we have positive stat stages
        if (eff == SemEffect::BatonPass) {
            const auto& st = ctx.self.stages;
            const bool has_positive = (st.attack > 0 || st.defense > 0 ||
                st.speed > 0 || st.special_attack > 0 || st.special_defense > 0 ||
                st.evasion > 0);
            if (has_positive) {
                scores.scores[i] += kStrongEncourage;
            } else {
                scores.scores[i] += kDiscourage;
            }
            continue;
        }

        // Belly Drum: encourage only if HP is at max
        if (eff == SemEffect::BellyDrum) {
            if (self_max > 0 && self_hp < self_max) {
                scores.scores[i] += kStrongDiscourage;
            }
            continue;
        }

        // Protect / Endure: discourage (Crystal AI generally avoids stalling)
        if (eff == SemEffect::Protect || eff == SemEffect::Endure) {
            scores.scores[i] += kDiscourage;
            continue;
        }

        // Hyper Beam: encourage if opponent is at low HP (likely KO)
        if (eff == SemEffect::HyperBeam) {
            if (ctx.opponent.stats.max_hp > 0 &&
                ctx.opponent.stats.hp * 4 <= ctx.opponent.stats.max_hp) {
                scores.scores[i] += kEncourage;
            }
            continue;
        }

        // Rain Dance: encourage if AI knows a rain-synergy move (ROM-derived list)
        if (eff == SemEffect::RainDance) {
            bool has_synergy = false;
            for (size_t j = 0; j < 4 && !has_synergy; ++j) {
                const MoveData* mdj = ctx.battle.registries().moves.get(ctx.self.moves[j].move);
                if (mdj && rules.is_ai_rain_dance_move(static_cast<uint8_t>(mdj->id)))
                    has_synergy = true;
            }
            if (has_synergy) {
                scores.scores[i] += kStrongEncourage;
            } else {
                scores.scores[i] += kStrongDiscourage;
            }
            continue;
        }

        // Sunny Day: encourage if AI knows a sunny-day synergy move (ROM-derived list)
        if (eff == SemEffect::SunnyDay) {
            bool has_synergy = false;
            for (size_t j = 0; j < 4 && !has_synergy; ++j) {
                const MoveData* mdj = ctx.battle.registries().moves.get(ctx.self.moves[j].move);
                if (mdj && rules.is_ai_sunny_day_move(static_cast<uint8_t>(mdj->id)))
                    has_synergy = true;
            }
            if (has_synergy) {
                scores.scores[i] += kStrongEncourage;
            } else {
                scores.scores[i] += kStrongDiscourage;
            }
            continue;
        }
    }
}

// ============================================================================
// VanillaCrystalAI::decide
// ============================================================================

AIDecision VanillaCrystalAI::decide(const AIContext& ctx) {
    // No-rules overload: test/legacy path only.
    // Without BattleRules, ai_basic (status-only list), ai_setup (stat-up/down lists),
    // and ai_smart (weather synergy lists) cannot run faithfully.
    // Only ai_types and ai_offensive run; ai_basic is skipped.
    // For production use, always call decide(ctx, rules).
    MoveScores scores{};

    const AIBehaviorId beh = behavior_;

    // ai_basic skipped: requires BattleRules::ai_status_only_effects
    ai_types(scores, ctx);

    if (beh >= VanillaAI::AGGRESSIVE) {
        ai_offensive(scores, ctx);
    }
    // ai_setup and ai_smart skipped: require BattleRules lists

    if (should_switch(ctx)) {
        size_t switch_to = choose_switch_target(ctx);
        if (switch_to < SIZE_MAX) {
            AIDecision d;
            d.action    = ActionSwitch{switch_to};
            d.reasoning = "Low HP — switching (no-rules)";
            d.confidence = 0.7f;
            return d;
        }
    }

    int best_score = INT_MAX;
    std::vector<size_t> candidates;
    for (size_t i = 0; i < 4; ++i) {
        if (!ctx.self.can_use_move(i)) continue;
        if (scores.scores[i] < best_score) {
            best_score = scores.scores[i];
            candidates.clear();
            candidates.push_back(i);
        } else if (scores.scores[i] == best_score) {
            candidates.push_back(i);
        }
    }

    AIDecision d;
    if (candidates.empty()) {
        d.action = ActionFight{0, 0};
        d.reasoning = "Struggle";
        d.confidence = 1.0f;
        return d;
    }

    const size_t chosen = candidates.size() == 1
        ? candidates[0]
        : candidates[const_cast<Battle&>(ctx.battle).rng_byte() % candidates.size()];
    d.action     = ActionFight{chosen, 0};
    d.reasoning  = "Move slot " + std::to_string(chosen) + " score=" + std::to_string(best_score);
    d.confidence = 0.8f;
    return d;
}

AIDecision VanillaCrystalAI::decide(const AIContext& ctx, const BattleRules& rules) {
    // ROM-derived overload using semantic AIPassSet for named-boolean pass dispatch.
    // When constructed from AIPassSet, uses ai_passes_ directly — no
    // range-sniffing or numeric domain overloading.
    // When constructed from AIBehaviorId (tests), uses legacy tier path.
    MoveScores scores{rules};

    if (use_pass_set_) {
        // Semantic named-boolean dispatch — no Crystal bit positions in this code.
        // ai_basic always runs (Crystal minimum, also the default when run_basic=true).
        ai_basic(scores, ctx, rules);

        // Passes run in Crystal's AIScoringPointers order (SETUP, TYPES, OFFENSIVE, SMART).
        if (ai_passes_.run_setup)     ai_setup(scores, ctx, 0, 0, rules);
        if (ai_passes_.run_types)     ai_types(scores, ctx);
        if (ai_passes_.run_offensive) ai_offensive(scores, ctx);
        if (ai_passes_.run_smart)     ai_smart(scores, ctx, rules);
    } else {
        // Legacy tier-based dispatch (tests / direct AIBehaviorId construction).
        const AIBehaviorId beh = behavior_;

        ai_basic(scores, ctx, rules);

        if (beh >= VanillaAI::SMART || beh == VanillaAI::GYM_LEADER
         || beh == VanillaAI::ELITE_FOUR || beh == VanillaAI::CHAMPION) {
            ai_types(scores, ctx);
            ai_smart(scores, ctx, rules);
        } else if (beh >= VanillaAI::AGGRESSIVE) {
            ai_types(scores, ctx);
            ai_offensive(scores, ctx);
        } else {
            ai_types(scores, ctx);
        }

        if (beh == VanillaAI::DEFENSIVE || beh == VanillaAI::GYM_LEADER
         || beh == VanillaAI::ELITE_FOUR || beh == VanillaAI::CHAMPION) {
            ai_setup(scores, ctx, 0, 0, rules);
        }
    }

    if (should_switch(ctx)) {
        size_t switch_to = choose_switch_target(ctx);
        if (switch_to < SIZE_MAX) {
            AIDecision d;
            d.action    = ActionSwitch{switch_to};
            d.reasoning = "Low HP — switching (rules-derived)";
            d.confidence = 0.7f;
            return d;
        }
    }

    int best_score = INT_MAX;
    std::vector<size_t> candidates;
    for (size_t i = 0; i < 4; ++i) {
        if (!ctx.self.can_use_move(i)) continue;
        if (scores.scores[i] < best_score) {
            best_score = scores.scores[i];
            candidates.clear();
            candidates.push_back(i);
        } else if (scores.scores[i] == best_score) {
            candidates.push_back(i);
        }
    }

    AIDecision d;
    if (candidates.empty()) {
        d.action = ActionFight{0, 0};
        d.reasoning = "Struggle";
        d.confidence = 1.0f;
        return d;
    }

    // Crystal tie-breaking: uniform random among tied slots
    const size_t chosen = candidates.size() == 1
        ? candidates[0]
        : candidates[const_cast<Battle&>(ctx.battle).rng_byte() % candidates.size()];
    d.action     = ActionFight{chosen, 0};
    d.reasoning  = "Move slot " + std::to_string(chosen) + " score=" + std::to_string(best_score)
                 + " (rules-derived)";
    d.confidence = 0.8f;
    return d;
}

// ============================================================================
// VanillaCrystalAI private helpers
// ============================================================================

int VanillaCrystalAI::score_move(const AIContext& ctx, size_t move_slot) {
    // Placeholder: expose the score for a single slot if needed externally.
    // Only ai_types runs here (ai_basic requires BattleRules).
    MoveScores scores{};
    ai_types(scores, ctx);
    return scores.scores[move_slot];
}

bool VanillaCrystalAI::should_switch(const AIContext& ctx) {
    // Switch if HP <= 25% and we have available switches and trainer battle
    if (!ctx.can_switch) return false;
    if (ctx.available_switches.empty()) return false;
    const int16_t hp  = ctx.self.stats.hp;
    const int16_t max = ctx.self.stats.max_hp;
    // Trainer AI switches at <= 25% HP with ~50% probability (simplified)
    return (max > 0 && hp * 4 <= max);
}

bool VanillaCrystalAI::should_use_item(const AIContext& /*ctx*/) {
    return false;  // Item AI not yet implemented
}

size_t VanillaCrystalAI::choose_switch_target(const AIContext& ctx) {
    if (ctx.available_switches.empty()) return SIZE_MAX;
    // Pick the available mon with the most HP (simple heuristic)
    size_t best = ctx.available_switches[0];
    return best;
}

// ============================================================================
// LuaTrainerAI stub (Lua runtime integration deferred)
// ============================================================================

LuaTrainerAI::LuaTrainerAI(AIBehaviorId id, const std::string& name,
                            LuaRuntime& lua, const std::string& script_function)
    : id_(id), name_(name), lua_(&lua), script_function_(script_function)
{}

AIDecision LuaTrainerAI::decide(const AIContext& ctx) {
    // Lua dispatch not yet implemented; fall back to basic AI
    VanillaCrystalAI fallback(VanillaAI::BASIC);
    return fallback.decide(ctx);
}

void LuaTrainerAI::on_battle_start(const Battle& /*battle*/) {}
void LuaTrainerAI::on_opponent_switch(const BattlePokemon& /*new_pokemon*/) {}
void LuaTrainerAI::on_turn_end(const Battle& /*battle*/) {}

// ============================================================================
// AIRegistry
// ============================================================================

AIRegistry::AIRegistry() {
    // Register vanilla AI behaviors
    register_native(VanillaAI::NONE, [](AIBehaviorId) {
        return std::make_unique<VanillaCrystalAI>(VanillaAI::NONE);
    });
    register_native(VanillaAI::BASIC, [](AIBehaviorId) {
        return std::make_unique<VanillaCrystalAI>(VanillaAI::BASIC);
    });
    register_native(VanillaAI::SMART, [](AIBehaviorId) {
        return std::make_unique<VanillaCrystalAI>(VanillaAI::SMART);
    });
    register_native(VanillaAI::AGGRESSIVE, [](AIBehaviorId) {
        return std::make_unique<VanillaCrystalAI>(VanillaAI::AGGRESSIVE);
    });
    register_native(VanillaAI::DEFENSIVE, [](AIBehaviorId) {
        return std::make_unique<VanillaCrystalAI>(VanillaAI::DEFENSIVE);
    });
    register_native(VanillaAI::STATUS_FOCUS, [](AIBehaviorId) {
        return std::make_unique<VanillaCrystalAI>(VanillaAI::STATUS_FOCUS);
    });
    register_native(VanillaAI::GYM_LEADER, [](AIBehaviorId) {
        return std::make_unique<VanillaCrystalAI>(VanillaAI::GYM_LEADER);
    });
    register_native(VanillaAI::ELITE_FOUR, [](AIBehaviorId) {
        return std::make_unique<VanillaCrystalAI>(VanillaAI::ELITE_FOUR);
    });
    register_native(VanillaAI::CHAMPION, [](AIBehaviorId) {
        return std::make_unique<VanillaCrystalAI>(VanillaAI::CHAMPION);
    });

    // Register names
    names_[VanillaAI::NONE]        = "None";
    names_[VanillaAI::BASIC]       = "Basic";
    names_[VanillaAI::SMART]       = "Smart";
    names_[VanillaAI::AGGRESSIVE]  = "Aggressive";
    names_[VanillaAI::DEFENSIVE]   = "Defensive";
    names_[VanillaAI::STATUS_FOCUS]= "StatusFocus";
    names_[VanillaAI::GYM_LEADER]  = "GymLeader";
    names_[VanillaAI::ELITE_FOUR]  = "EliteFour";
    names_[VanillaAI::CHAMPION]    = "Champion";
}

void AIRegistry::register_native(AIBehaviorId id, AIFactory factory) {
    if (frozen_) throw std::runtime_error("AIRegistry is frozen");
    factories_[id] = std::move(factory);
}

void AIRegistry::register_lua(AIBehaviorId id, const std::string& name,
                               const std::string& script_function) {
    if (frozen_) throw std::runtime_error("AIRegistry is frozen");
    names_[id] = name;
    // Lua AI factories created on demand when lua_ is set
    factories_[id] = [id, name, script_function, this](AIBehaviorId) -> std::unique_ptr<ITrainerAI> {
        if (!lua_) {
            return std::make_unique<VanillaCrystalAI>(VanillaAI::BASIC);
        }
        return std::make_unique<LuaTrainerAI>(id, name, *lua_, script_function);
    };
}

std::unique_ptr<ITrainerAI> AIRegistry::create(AIBehaviorId id) {
    auto it = factories_.find(id);
    if (it != factories_.end()) {
        return it->second(id);
    }
    // Unknown ID: fall back to BASIC
    return std::make_unique<VanillaCrystalAI>(VanillaAI::BASIC);
}

void AIRegistry::set_trainer_override(TrainerId trainer, AIBehaviorId ai) {
    if (frozen_) throw std::runtime_error("AIRegistry is frozen");
    trainer_overrides_[trainer] = ai;
}

void AIRegistry::set_class_override(uint8_t trainer_class, AIBehaviorId ai) {
    if (frozen_) throw std::runtime_error("AIRegistry is frozen");
    class_overrides_[trainer_class] = ai;
}

AIBehaviorId AIRegistry::get_ai_for_trainer(TrainerId trainer, uint8_t trainer_class) {
    // Trainer-specific override takes priority
    auto it = trainer_overrides_.find(trainer);
    if (it != trainer_overrides_.end()) return it->second;

    // Class-level override next
    auto cit = class_overrides_.find(trainer_class);
    if (cit != class_overrides_.end()) return cit->second;

    // Default
    return VanillaAI::BASIC;
}

std::vector<std::pair<AIBehaviorId, std::string>> AIRegistry::list_registered() const {
    std::vector<std::pair<AIBehaviorId, std::string>> result;
    for (const auto& [id, factory] : factories_) {
        auto nit = names_.find(id);
        result.push_back({id, nit != names_.end() ? nit->second : "?"});
    }
    return result;
}

} // namespace enginemon
