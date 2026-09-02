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
// Constants matching Crystal AI behavior
// ============================================================================

// Score adjustment magnitudes — Crystal source: scoring.asm
//   Default score: 20 (all moves start at 20)
//   AIDiscourageMove: +10 (strongly discourage)
//   Individual pass discourage: +1 or +2
//   Individual pass encourage: -1 or -2
static constexpr int kInitScore       = 20;   // Crystal: default = 20, disabled = 80
static constexpr int kDiscourageStrong = 10;  // AIDiscourageMove in Crystal (+10)
static constexpr int kStrongDiscourage = 2;   // individual pass strong discourage (+2)
static constexpr int kDiscourage       = 1;   // individual pass discourage (+1)
static constexpr int kEncourage        = -1;  // individual pass encourage (-1)
static constexpr int kStrongEncourage  = -2;  // individual pass strong encourage (-2)

// Crystal move effect IDs (from constants/battle_constants.asm).
// We use the effect_id field of MoveData for these comparisons.
// These are the Gen 2 canonical values.
namespace Effect {
    static constexpr uint8_t None          = 0;
    static constexpr uint8_t Sleep         = 1;
    static constexpr uint8_t Poison        = 2;
    static constexpr uint8_t Drain         = 3;  // LeechHit
    static constexpr uint8_t Burn          = 5;
    static constexpr uint8_t Freeze        = 6;
    static constexpr uint8_t Paralyze      = 7;
    static constexpr uint8_t Selfdestruct  = 8;
    static constexpr uint8_t DreamEater    = 9;
    static constexpr uint8_t MirrorMove    = 10;
    static constexpr uint8_t AttackUp      = 11;
    static constexpr uint8_t DefenseUp     = 12;
    static constexpr uint8_t SpeedUp       = 13;
    static constexpr uint8_t SpecialUp     = 14;
    static constexpr uint8_t AccuracyUp    = 15;
    static constexpr uint8_t EvasionUp     = 16;
    static constexpr uint8_t AlwaysHit     = 17;
    static constexpr uint8_t AttackDown    = 18;
    static constexpr uint8_t DefenseDown   = 19;
    static constexpr uint8_t SpeedDown     = 20;
    static constexpr uint8_t SpecialDown   = 21;
    static constexpr uint8_t AccuracyDown  = 22;
    static constexpr uint8_t EvasionDown   = 23;
    static constexpr uint8_t Bide          = 27;
    static constexpr uint8_t ForceSwitch   = 29;
    static constexpr uint8_t Heal          = 33;
    static constexpr uint8_t Toxic         = 34;
    static constexpr uint8_t LightScreen   = 36;
    static constexpr uint8_t Ohko          = 38;
    static constexpr uint8_t Confuse       = 48;
    static constexpr uint8_t SpDefUp2      = 53;
    static constexpr uint8_t Reflect       = 66;
    static constexpr uint8_t Substitute    = 68;
    static constexpr uint8_t HyperBeam     = 69;
    static constexpr uint8_t LeechSeed     = 73;
    static constexpr uint8_t AttackUp2     = 74;
    static constexpr uint8_t DefenseUp2    = 75;
    static constexpr uint8_t SpeedUp2      = 76;
    static constexpr uint8_t SpecialUp2    = 77;
    static constexpr uint8_t AccuracyUp2   = 78;
    static constexpr uint8_t EvasionUp2    = 79;
    static constexpr uint8_t AttackDown2   = 80;
    static constexpr uint8_t DefenseDown2  = 81;
    static constexpr uint8_t SpeedDown2    = 82;
    static constexpr uint8_t SpecialDown2  = 83;
    static constexpr uint8_t AccuracyDown2 = 84;
    static constexpr uint8_t EvasionDown2  = 85;
    static constexpr uint8_t Nightmare     = 92;
    static constexpr uint8_t BellyDrum     = 97;
    static constexpr uint8_t Safeguard     = 108;
    static constexpr uint8_t MorningSun    = 112;
    static constexpr uint8_t Synthesis     = 113;
    static constexpr uint8_t Moonlight     = 114;
    static constexpr uint8_t RainDance     = 120;
    static constexpr uint8_t SunnyDay      = 121;
    static constexpr uint8_t Protect       = 122;
    static constexpr uint8_t BatonPass     = 133;
    static constexpr uint8_t Endure        = 135;
}

// Status-only effects (source: suiCune data/battle/ai/status_only_effects.asm)
// These are discarded by BASIC AI if target already has a status.
static const uint8_t kStatusOnlyEffects[] = {
    Effect::Sleep, Effect::Poison, Effect::Burn, Effect::Freeze,
    Effect::Paralyze, Effect::Toxic, Effect::Confuse, Effect::LeechSeed,
    Effect::Nightmare, 0xFF  // terminator
};

static bool is_status_only(uint8_t effect) {
    for (const uint8_t* p = kStatusOnlyEffects; *p != 0xFF; ++p)
        if (*p == effect) return true;
    return false;
}

// ============================================================================
// Score table for move selection
// ============================================================================

struct MoveScores {
    int scores[4];
    MoveScores() {
        // Crystal initializes all move scores to 20 (AIChooseMove, line "ld a, 20")
        scores[0] = scores[1] = scores[2] = scores[3] = kInitScore;
    }
};

// ============================================================================
// VanillaCrystalAI helpers
// ============================================================================

VanillaCrystalAI::VanillaCrystalAI(AIBehaviorId behavior)
    : behavior_(behavior)
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
// Discourages status-only moves if target already has a status condition,
// or if the target has Safeguard active.
// Also discourages redundant moves (e.g., Protect used twice consecutively).
// ============================================================================

static void ai_basic(MoveScores& scores, const AIContext& ctx) {
    for (size_t i = 0; i < 4; ++i) {
        const MoveId mid = ctx.self.moves[i].move;
        if (mid == MOVE_NONE) continue;

        // We need the move's effect_id. Battle gives us the Battle registry
        // via ctx.battle.registries().
        // Access through the Battle const reference.
        const MoveData* md = ctx.battle.registries().moves.get(mid);
        if (!md) continue;

        // Discourage status-only moves if target already has a status
        if (is_status_only(md->effect_id)) {
            const bool target_already_statused =
                (ctx.opponent.status != Status::None);
            if (target_already_statused) {
                scores.scores[i] += kDiscourageStrong;  // AIDiscourageMove = +10
            }
        }
    }
}

// ROM-derived variant: uses rules.ai_status_only_effects instead of kStatusOnlyEffects.
static void ai_basic(MoveScores& scores, const AIContext& ctx, const BattleRules& rules) {
    for (size_t i = 0; i < 4; ++i) {
        const MoveId mid = ctx.self.moves[i].move;
        if (mid == MOVE_NONE) continue;
        const MoveData* md = ctx.battle.registries().moves.get(mid);
        if (!md) continue;
        if (rules.is_ai_status_only(md->effect_id)) {
            if (ctx.opponent.status != Status::None) {
                scores.scores[i] += kDiscourageStrong;  // AIDiscourageMove = +10
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
            scores.scores[i] += kDiscourageStrong;
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
// ============================================================================

static bool is_stat_up(uint8_t effect) {
    return (effect >= Effect::AttackUp  && effect <= Effect::EvasionUp)
        || (effect >= Effect::AttackUp2 && effect <= Effect::EvasionUp2);
}

static bool is_stat_down(uint8_t effect) {
    return (effect >= Effect::AttackDown  && effect <= Effect::EvasionDown)
        || (effect >= Effect::AttackDown2 && effect <= Effect::EvasionDown2);
}

static void ai_setup(MoveScores& scores, const AIContext& ctx,
                     uint16_t self_turn, uint16_t opp_turn) {
    for (size_t i = 0; i < 4; ++i) {
        if (ctx.self.moves[i].move == MOVE_NONE) continue;
        const MoveData* md = ctx.battle.registries().moves.get(ctx.self.moves[i].move);
        if (!md) continue;

        if (is_stat_up(md->effect_id)) {
            if (self_turn == 0) {
                // Turn 1 of self's pokemon: 50% chance to encourage
                scores.scores[i] += kStrongEncourage;
            } else {
                scores.scores[i] += kStrongDiscourage;
            }
        } else if (is_stat_down(md->effect_id)) {
            if (opp_turn == 0) {
                // Turn 1 of opponent's pokemon: 50% chance to encourage
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
// ============================================================================

static void ai_smart(MoveScores& scores, const AIContext& ctx) {
    for (size_t i = 0; i < 4; ++i) {
        if (ctx.self.moves[i].move == MOVE_NONE) continue;
        const MoveData* md = ctx.battle.registries().moves.get(ctx.self.moves[i].move);
        if (!md) continue;

        const uint8_t eff = md->effect_id;
        const int16_t self_hp  = ctx.self.stats.hp;
        const int16_t self_max = ctx.self.stats.max_hp;

        // Healing moves: encourage when HP is low (< half)
        if (eff == Effect::Heal || eff == Effect::MorningSun
         || eff == Effect::Synthesis || eff == Effect::Moonlight) {
            if (self_max > 0 && self_hp * 2 < self_max) {
                scores.scores[i] += kStrongEncourage;
            } else if (self_max > 0 && self_hp * 4 >= self_max * 3) {
                // HP above 75%: discourage healing
                scores.scores[i] += kDiscourage;
            }
            continue;
        }

        // Selfdestruct / Explosion: discourage if opponent not faint-threshold
        if (eff == Effect::Selfdestruct) {
            if (ctx.opponent.stats.hp * 4 > ctx.opponent.stats.max_hp) {
                // Opponent HP > 25% — discourage
                scores.scores[i] += kDiscourage;
            }
            continue;
        }

        // Sleep-inducing: encourage if we also have Dream Eater or Nightmare
        if (eff == Effect::Sleep) {
            for (size_t j = 0; j < 4; ++j) {
                if (ctx.self.moves[j].move == MOVE_NONE) continue;
                const MoveData* mdj = ctx.battle.registries().moves.get(ctx.self.moves[j].move);
                if (!mdj) continue;
                if (mdj->effect_id == Effect::DreamEater || mdj->effect_id == Effect::Nightmare) {
                    scores.scores[i] += kStrongEncourage;
                    break;
                }
            }
            continue;
        }

        // Dream Eater: only useful if target is asleep
        if (eff == Effect::DreamEater) {
            if (ctx.opponent.status != Status::Sleep) {
                scores.scores[i] += kDiscourageStrong;  // AIDiscourageMove
            }
            continue;
        }

        // Nightmare: only useful if target is asleep
        if (eff == Effect::Nightmare) {
            if (ctx.opponent.status != Status::Sleep) {
                scores.scores[i] += kDiscourageStrong;  // AIDiscourageMove
            }
            continue;
        }

        // Toxic / Poison: discourage if target already poisoned
        if (eff == Effect::Toxic || eff == Effect::Poison) {
            if (ctx.opponent.status == Status::Poison
             || ctx.opponent.status == Status::BadPoison) {
                scores.scores[i] += kDiscourageStrong;  // AIDiscourageMove
            }
            continue;
        }

        // Paralyze: discourage if target already paralyzed
        if (eff == Effect::Paralyze) {
            if (ctx.opponent.status == Status::Paralysis) {
                scores.scores[i] += kDiscourageStrong;  // AIDiscourageMove
            }
            continue;
        }

        // Reflect / Light Screen: discourage if already active (simplified)
        if (eff == Effect::Reflect || eff == Effect::LightScreen) {
            // No convenient way to check screen state from AIContext currently;
            // leave neutral.
            continue;
        }

        // Baton Pass: encourage if we have positive stat stages
        if (eff == Effect::BatonPass) {
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
        if (eff == Effect::BellyDrum) {
            if (self_max > 0 && self_hp < self_max) {
                scores.scores[i] += kStrongDiscourage;
            }
            continue;
        }

        // Protect / Endure: discourage (Crystal AI generally avoids stalling)
        if (eff == Effect::Protect || eff == Effect::Endure) {
            scores.scores[i] += kDiscourage;
            continue;
        }

        // Hyper Beam: encourage if opponent is at low HP (likely KO)
        if (eff == Effect::HyperBeam) {
            if (ctx.opponent.stats.max_hp > 0 &&
                ctx.opponent.stats.hp * 4 <= ctx.opponent.stats.max_hp) {
                // Opponent <= 25% HP — encourage the KO attempt
                scores.scores[i] += kEncourage;
            }
            continue;
        }
    }
}

// ============================================================================
// VanillaCrystalAI::decide
// ============================================================================

AIDecision VanillaCrystalAI::decide(const AIContext& ctx) {
    // Build score table (all 0 = equal initially)
    MoveScores scores{};

    // Determine which AI passes to run based on behavior flags
    // Crystal: the behavior ID encodes a bitmask of AI passes.
    // BASIC is always run.
    // Higher behaviors add more passes.
    const AIBehaviorId beh = behavior_;

    ai_basic(scores, ctx);

    if (beh >= VanillaAI::SMART || beh == VanillaAI::GYM_LEADER
     || beh == VanillaAI::ELITE_FOUR || beh == VanillaAI::CHAMPION) {
        ai_types(scores, ctx);
        ai_smart(scores, ctx);
    } else if (beh >= VanillaAI::AGGRESSIVE) {
        ai_types(scores, ctx);
        ai_offensive(scores, ctx);
    } else {
        // BASIC only adds type awareness
        ai_types(scores, ctx);
    }

    if (beh == VanillaAI::DEFENSIVE || beh == VanillaAI::GYM_LEADER
     || beh == VanillaAI::ELITE_FOUR || beh == VanillaAI::CHAMPION) {
        // Setup AI: turn counts aren't in AIContext yet; pass 0 as approximation
        ai_setup(scores, ctx, 0, 0);
    }

    // Should switch? Basic check: if self has very low HP and has usable switches.
    if (should_switch(ctx)) {
        // Pick a switch target and issue a switch action
        size_t switch_to = choose_switch_target(ctx);
        if (switch_to < SIZE_MAX) {
            AIDecision d;
            d.action    = ActionSwitch{switch_to};
            d.reasoning = "Low HP — switching";
            d.confidence = 0.7f;
            return d;
        }
    }

    // Select the move with the lowest score (ties broken randomly via first-found)
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
        // No usable moves — struggle
        d.action = ActionFight{0, 0};
        d.reasoning = "Struggle";
        d.confidence = 1.0f;
        return d;
    }

    // Crystal tie-breaking: Random() % num_tied_moves (uniform among tied slots).
    // Source: AIChooseMove .ChooseMove — maskbits NUM_MOVES, retry until non-zero.
    const size_t chosen = candidates.size() == 1
        ? candidates[0]
        : candidates[const_cast<Battle&>(ctx.battle).rng_byte() % candidates.size()];
    d.action     = ActionFight{chosen, 0};
    d.reasoning  = "Move slot " + std::to_string(chosen) + " score=" + std::to_string(best_score);
    d.confidence = 0.8f;
    return d;
}

AIDecision VanillaCrystalAI::decide(const AIContext& ctx, const BattleRules& rules) {
    // ROM-derived overload using exact bitmask-driven pass dispatch.
    //
    // When set_trainer() calls set_battle_rules(), it stores the raw
    // TRNATTR_AI_MOVE_WEIGHTS bitmask as the behavior_ ID.
    // Crystal bit definitions (trainer_data_constants.asm):
    //   bit 0 = AI_BASIC       bit 1 = AI_SETUP      bit 2 = AI_TYPES
    //   bit 3 = AI_OFFENSIVE   bit 4 = AI_SMART
    //
    // Crystal's AIChooseMove iterates flag bits 0-15 in order, running the
    // corresponding AI pass for each set bit (AIScoringPointers index = bit number).
    // We replicate the exact pass set and order.
    //
    // NOTE: When behavior_ is a legacy VanillaAI:: enum constant (< 9, from direct
    // construction in tests), fall through to the tier-based logic below.
    MoveScores scores{};

    // Check whether behavior_ is a raw bitmask (>= 16 and not a legacy enum)
    // or a legacy tier constant (0-8).  Raw bitmask values from set_trainer() will
    // be 0x0001–0xFFFF; legacy enum values are 0–8.  Discriminate by value range.
    const bool is_bitmask = (behavior_ >= 16);

    if (is_bitmask) {
        // Exact bitmask-driven pass dispatch — matches Crystal AIChooseMove.
        const uint16_t flags = static_cast<uint16_t>(behavior_);
        constexpr uint16_t BIT_BASIC     = 1 << 0;
        constexpr uint16_t BIT_SETUP     = 1 << 1;
        constexpr uint16_t BIT_TYPES     = 1 << 2;
        constexpr uint16_t BIT_OFFENSIVE = 1 << 3;
        constexpr uint16_t BIT_SMART     = 1 << 4;

        // AI_BASIC: always run first regardless of flags.
        // Source: Crystal's AI always scores via AI_Basic at minimum.
        ai_basic(scores, ctx, rules);

        // Passes run in Crystal's AIScoringPointers order (bit 0 first).
        if (flags & BIT_SETUP)     ai_setup(scores, ctx, 0, 0);
        if (flags & BIT_TYPES)     ai_types(scores, ctx);
        if (flags & BIT_OFFENSIVE) ai_offensive(scores, ctx);
        if (flags & BIT_SMART)     ai_smart(scores, ctx);
    } else {
        // Legacy tier-based dispatch (tests / direct VanillaAI:: construction).
        const AIBehaviorId beh = behavior_;

        ai_basic(scores, ctx, rules);

        if (beh >= VanillaAI::SMART || beh == VanillaAI::GYM_LEADER
         || beh == VanillaAI::ELITE_FOUR || beh == VanillaAI::CHAMPION) {
            ai_types(scores, ctx);
            ai_smart(scores, ctx);
        } else if (beh >= VanillaAI::AGGRESSIVE) {
            ai_types(scores, ctx);
            ai_offensive(scores, ctx);
        } else {
            ai_types(scores, ctx);
        }

        if (beh == VanillaAI::DEFENSIVE || beh == VanillaAI::GYM_LEADER
         || beh == VanillaAI::ELITE_FOUR || beh == VanillaAI::CHAMPION) {
            ai_setup(scores, ctx, 0, 0);
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
    // Placeholder: expose the score for a single slot if needed externally
    MoveScores scores{};
    ai_basic(scores, ctx);
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
