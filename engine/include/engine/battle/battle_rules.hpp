#pragma once
// engine/include/engine/battle/battle_rules.hpp
// ROM-derived battle rule tables — extracted by the Crystal frontend and
// packaged in the BRLS chunk.  The generic runtime uses these in place of
// the stock-Crystal-hardcoded tables that were previously in calculator.cpp
// and trainer_ai.cpp.
//
// Pipeline:
//   Crystal ROM/profile
//   → BattleRulesExtractor (frontends/crystal/extract/battle_rules.cpp)
//   → BattleRules (this struct)
//   → BRLS chunk (EMON package, via PackageWriter::add_battle_rules())
//   → BattleRules (loaded by PackageReader::load_battle_rules())
//   → FrozenGameData / HeadlessRuntime
//   → calculator functions / VanillaCrystalAI
//
// Every table in this struct corresponds to a named data table in the Crystal
// ROM.  No values are hardcoded in generic runtime code.
//
// Source references (Crystal v1.1):
//   StatLevelMultipliers_Applied  0f:6d2b  data/battle/stat_multipliers.asm
//   AccuracyLevelMultipliers      0d:4eb2  data/battle/accuracy_multipliers.asm
//   CriticalHitChances            0d:46ab  data/battle/critical_hit_chances.asm
//   WobbleProbabilities           03:79ba  data/battle/wobble_probabilities.asm
//   WeatherTypeModifiers          3e:7e13  data/battle/weather_modifiers.asm
//   WeatherMoveModifiers          3e:7e20  data/battle/weather_modifiers.asm
//   CriticalHitMoves              0d:46a3  data/moves/critical_hit_moves.asm
//   MoveEffectPriorities          0f:45df  data/moves/effects_priorities.asm
//   StatusOnlyEffects             0e:45db  data/battle/ai/status_only_effects.asm
//   RiskyEffects                  0e:54ff  data/battle/ai/risky_effects.asm
//   StallMoves                    0e:5348  data/battle/ai/stall_moves.asm
//   UsefulMoves                   0e:5301  data/battle/ai/useful_moves.asm
//   ResidualMoves                 0e:5446  data/battle/ai/residual_moves.asm
//   EncoreMoves                   0e:4c85  data/battle/ai/encore_moves.asm
//   TrainerClassAttributes        0e:559c  data/trainers/attributes.asm

#include "engine/core/types.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

namespace enginemon {

// ============================================================================
// Stage multiplier entry: {numerator, denominator}.
// Applied as: floor(base_stat * numerator / denominator), clamped [1, 999].
// ============================================================================
struct StageMultiplierEntry {
    uint8_t numerator;
    uint8_t denominator;
};

// 13 entries covering stages -6 through +6 (index = stage + 6).
using StageMult13 = std::array<StageMultiplierEntry, 13>;

// ============================================================================
// Weather modifier entry: weather condition × move type → multiplier.
// Multiplier is in Crystal's per-10 notation (MORE_EFFECTIVE=15, NVE=5).
// A sentinel entry has weather_id == 0xFF.
// ============================================================================
struct WeatherModifierEntry {
    uint8_t weather_id;    // Crystal WEATHER_* constant (0=None, 1=Rain, 2=Sun, …)
    uint8_t type_id;       // Type or move-effect ID depending on table
    uint8_t multiplier;    // 15=1.5×, 5=0.5×
};

// ============================================================================
// Move effect priority entry: effect_id → priority value.
// Crystal: priority 2 = Quick Attack tier, 1 = normal, 0 = last.
// A sentinel entry has effect_id == 0xFF.
// ============================================================================
struct MoveEffectPriorityEntry {
    uint8_t effect_id;
    uint8_t priority;   // Crystal: BASE_PRIORITY=1; 2=fast, 0=last, 3=Protect/Endure
};

// ============================================================================
// Trainer class AI flags entry: one per trainer class (0-indexed).
// ai_passes: semantic AI behavior set — decoded from ROM bitmask by the Crystal frontend.
// ai_item_flags: TRNATTR_AI_ITEM_SWITCH bitmask (raw — item use AI not yet semantic).
// ============================================================================
struct TrainerClassAIEntry {
    uint8_t  item1;          // Default held item 1 (NO_ITEM=0)
    uint8_t  item2;          // Default held item 2 (NO_ITEM=0)
    uint8_t  base_reward;    // Base prize money (prize = base_reward * level * 4)
    AIPassSet ai_passes;     // Semantic AI pass set (decoded from TRNATTR_AI_MOVE_WEIGHTS)
    uint16_t ai_item_flags;  // TRNATTR_AI_ITEM_SWITCH bitmask (LE, raw — item AI pending)
};

// ============================================================================
// BattleRules — the complete set of ROM-derived battle tables.
//
// All vectors/arrays are populated from the BRLS package chunk.
// An empty or default-constructed BattleRules is NOT valid for battle use;
// it must be loaded from a package.
//
// Validity: use is_valid() to test before passing to battle functions.
// ============================================================================
struct BattleRules {
    // Stat stage multipliers [stage+6], for all stats except accuracy/evasion.
    // Source: StatLevelMultipliers_Applied (0f:6d2b)
    StageMult13 stat_stage_mult{};

    // Accuracy/evasion stage multipliers [stage+6].
    // Source: AccuracyLevelMultipliers (0d:4eb2)
    StageMult13 acc_stage_mult{};

    // Critical hit chance thresholds per crit stage (index 0..6).
    // A crit fires when random(0-255) < threshold.
    // Source: CriticalHitChances (0d:46ab)
    std::array<uint8_t, 7> crit_chances{};

    // Wobble probability table for capture animation.
    // Each entry: {catch_rate_threshold, wobble_chance/255}.
    // Walked linearly; first entry where [0] >= final_catch_rate is used.
    // Source: WobbleProbabilities (03:79ba)
    std::vector<std::array<uint8_t, 2>> wobble_probabilities;

    // Weather × type modifiers (sentinel-terminated).
    // Source: WeatherTypeModifiers (3e:7e13)
    std::vector<WeatherModifierEntry> weather_type_modifiers;

    // Weather × move-effect modifiers (sentinel-terminated).
    // Source: WeatherMoveModifiers (3e:7e20)
    std::vector<WeatherModifierEntry> weather_move_modifiers;

    // Move IDs that grant +2 crit stage.
    // Source: CriticalHitMoves (0d:46a3)
    // Crystal stores these as MOVE_ANIM byte values (= MoveId for standard Crystal moves).
    // The frontend extractor maps each ROM byte to semantic MoveId at extraction time.
    // Runtime compares move.id directly.
    std::vector<MoveId> high_crit_moves;

    // Move effect → priority value (sentinel-terminated).
    // Source: MoveEffectPriorities (0f:45df)
    std::vector<MoveEffectPriorityEntry> effect_priorities;

    // AI lists: effect IDs for each behavioral category.
    // These are the move-effect ID lists used by the AI scoring passes.
    // Source: data/battle/ai/*.asm
    std::vector<uint8_t> ai_status_only_effects;  // StatusOnlyEffects (0e:45db)
    std::vector<uint8_t> ai_risky_effects;         // RiskyEffects      (0e:54ff)
    std::vector<uint8_t> ai_stall_move_ids;        // StallMoves        (0e:5348) — move IDs
    std::vector<uint8_t> ai_useful_move_ids;       // UsefulMoves       (0e:5301) — move IDs
    std::vector<uint8_t> ai_residual_move_ids;     // ResidualMoves     (0e:5446) — move IDs
    std::vector<uint8_t> ai_encore_move_ids;       // EncoreMoves       (0e:4c85) — move IDs

    // Per-trainer-class AI flags.
    // Index = trainer class index (0-based, corresponding to Crystal trainer class order).
    // Source: TrainerClassAttributes (0e:559c)
    std::vector<TrainerClassAIEntry> trainer_class_ai;

    // ========================================================================
    // Validity
    // ========================================================================

    // Returns true if this BattleRules was loaded from a package and all
    // required tables are populated.
    bool is_valid() const {
        return !wobble_probabilities.empty()
            && !trainer_class_ai.empty()
            && crit_chances[0] != 0;  // 0 would mean every move crits — obviously wrong
    }

    // ========================================================================
    // Lookup helpers (used by calculator and AI — replaces hardcoded tables)
    // ========================================================================

    // Stat stage multiplier.  stage clamped to [-6, +6].
    StageMultiplierEntry get_stat_mult(int8_t stage) const {
        const int idx = std::clamp(static_cast<int>(stage), -6, 6) + 6;
        return stat_stage_mult[static_cast<size_t>(idx)];
    }

    // Accuracy/evasion stage multiplier.  net_stage = acc_stage - eva_stage, clamped.
    StageMultiplierEntry get_acc_mult(int8_t net_stage) const {
        const int idx = std::clamp(static_cast<int>(net_stage), -6, 6) + 6;
        return acc_stage_mult[static_cast<size_t>(idx)];
    }

    // Critical hit threshold for a given crit stage (clamped 0..6).
    uint8_t get_crit_chance(uint8_t stage) const {
        return crit_chances[std::min<uint8_t>(stage, 6)];
    }

    // True if move_id is in the high-crit move list.
    bool is_high_crit_move(MoveId move_id) const {
        for (MoveId m : high_crit_moves)
            if (m == move_id) return true;
        return false;
    }

    // True if effect_id is a status-only AI effect.
    bool is_ai_status_only(uint8_t effect_id) const {
        for (uint8_t e : ai_status_only_effects)
            if (e == effect_id) return true;
        return false;
    }

    // True if effect_id is a risky AI effect.
    bool is_ai_risky(uint8_t effect_id) const {
        for (uint8_t e : ai_risky_effects)
            if (e == effect_id) return true;
        return false;
    }

    // AI pass set for a trainer class (0-indexed).  Returns basic-only if out of range.
    AIPassSet get_trainer_ai_passes(size_t trainer_class_index) const {
        if (trainer_class_index >= trainer_class_ai.size()) return AIPassSet::basic_only();
        return trainer_class_ai[trainer_class_index].ai_passes;
    }

    // Priority for a given move effect_id.  Returns BASE_PRIORITY (1) if not found.
    uint8_t get_effect_priority(uint8_t effect_id) const {
        for (const auto& e : effect_priorities)
            if (e.effect_id == effect_id) return e.priority;
        return 1;  // BASE_PRIORITY
    }

    // Wobble chance/255 for a given final catch rate.
    // Returns 255 if rate exceeds all table entries.
    uint8_t get_wobble_chance(uint8_t final_catch_rate) const {
        for (const auto& entry : wobble_probabilities)
            if (entry[0] >= final_catch_rate) return entry[1];
        return 255;
    }
};

} // namespace enginemon
