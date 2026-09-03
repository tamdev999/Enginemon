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
//   RainDanceMoves                0e:50e7  data/battle/ai/rain_dance_moves.asm
//   SunnyDayMoves                 0e:5134  data/battle/ai/sunny_day_moves.asm
//   TrainerClassAttributes        0e:559c  data/trainers/attributes.asm
//   TrainerClassDVs               09:70d6  data/trainers/dvs.asm
//
// SM83-lifted routine parameters (extracted by frontends/crystal/extract/sm83_lifter.cpp):
//   BattleCommand_DamageCalc      0d:5612  damage formula constants (/5,+2,/50,MIN_DAMAGE=2)
//   BattleCommand_DamageVariation 0d:4cfd  variation lower bound byte (0xD9)
//   AIDiscourageMove              0e:5503  AI discouragement delta (+10)
//   AIChooseMove                  11:40ce  AI initial score (20)
//   GiveExperiencePoints          0f:6e3b  experience base divisor (/7)
//   PokeBallEffect                03:68a2  capture status bonuses (+10 SLP/FRZ)
//   TryToRunAwayFromBattle        0f:58b3  escape constants (×32, +30/attempt)
//   CalcMonStatC                  03:617b  stat formula offsets (/100, +5, +10)
//   GetEighthMaxHP                0f:4c83  burn/poison residual (/8 from shift count)
//   GetSixteenthMaxHP             0f:4c76  toxic residual (/16 from shift count)
//   BattleCommand_Critical        0d:4631  crit stage deltas (+2 held, +1 focus/scope)

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
    uint8_t  base_reward;    // Base prize money (prize = base_reward * level)
    AIPassSet ai_passes;     // Semantic AI pass set (decoded from TRNATTR_AI_MOVE_WEIGHTS)
    uint16_t ai_item_flags;  // TRNATTR_AI_ITEM_SWITCH bitmask (LE, raw — item AI pending)
    // DVs for all Pokémon of this trainer class.
    // Crystal's TrainerClassDVs table (09:70d6) stores one 2-byte entry per class.
    // Format: {atk<<4 | def, spd<<4 | spc} — each nibble is a 0–15 DV value.
    // Decoded into 4-bit fields for direct use by make_battle_pokemon().
    uint8_t dv_atk = 9;      // Attack DV  (0–15; vanilla default 9)
    uint8_t dv_def = 8;      // Defense DV (0–15; vanilla default 8)
    uint8_t dv_spd = 8;      // Speed DV   (0–15; vanilla default 8)
    uint8_t dv_spc = 8;      // Special DV (0–15; vanilla default 8)
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

    // AI weather synergy move lists: move IDs that benefit from weather.
    // AI_Smart encourages weather-setting moves when the AI knows one of these.
    // Source: data/battle/ai/rain_dance_moves.asm (0e:50e7)
    //         data/battle/ai/sunny_day_moves.asm  (0e:5134)
    std::vector<uint8_t> ai_rain_dance_move_ids;   // RainDanceMoves — move IDs that benefit from Rain
    std::vector<uint8_t> ai_sunny_day_move_ids;    // SunnyDayMoves  — move IDs that benefit from Sun

    // AI stat-effect lists for ai_setup().
    // These encode Crystal's contiguous effect-ID ranges as explicit lists,
    // extracted by the Crystal frontend.  The engine AI uses these lists
    // rather than range-checking Crystal's source layout directly.
    // Stat-up: ATTACK_UP(11)..EVASION_UP(16) + ATTACK_UP_2(74)..EVASION_UP_2(79)
    // Stat-down: ATTACK_DOWN(18)..EVASION_DOWN(23) + ATTACK_DOWN_2(80)..EVASION_DOWN_2(85)
    std::vector<uint8_t> ai_stat_up_effects;       // effect IDs that are +stat moves
    std::vector<uint8_t> ai_stat_down_effects;     // effect IDs that are -stat moves (to opponent)

    // Per-trainer-class AI flags.
    // Index = trainer class index (0-based, corresponding to Crystal trainer class order).
    // Source: TrainerClassAttributes (0e:559c)
    std::vector<TrainerClassAIEntry> trainer_class_ai;

    // ============================================================================
    // SM83-lifted formula parameters.
    // Extracted at package-build time from known Crystal routine shapes.
    // The generic runtime uses these instead of inline hardcoded constants.
    // Source routines verified in frontends/crystal/extract/sm83_lifter.cpp.
    // ============================================================================

    // Damage formula constants — BattleCommand_DamageCalc (0d:5612)
    // Core formula: ((level × 2 / level_divisor) + level_addend) × power × atk / def / damage_divisor
    // Then: clamped to [floor, cap], +floor addend applied after cap.
    struct DamageFormulaParams {
        uint8_t level_divisor   = 5;    // ÷5 in level factor (vanilla)
        uint8_t level_addend    = 2;    // +2 in level factor (count of inc [hl])
        uint8_t damage_divisor  = 50;   // final /50 divisor (vanilla)
        uint8_t min_damage      = 2;    // MIN_DAMAGE floor addend (vanilla: add a,2)
    } damage_formula{};

    // AI score constants — AIChooseMove (11:40ce) + AIDiscourageMove (0e:5503)
    struct AIScoreParams {
        uint8_t init_score         = 20;  // Initial score for all move slots (ld a,20)
        uint8_t discourage_strong  = 10;  // AIDiscourageMove delta (add a,10)
    } ai_scores{};

    // Stat formula offsets — CalcMonStatC (03:617b)
    // Formula: ((base+DV)×2 + sqrt(StatExp)/4) × level / level_divisor + offset
    struct StatFormulaParams {
        uint8_t level_divisor = 100;  // /100 divisor (ld a,100)
        uint8_t non_hp_offset =   5;  // STAT_MIN_NORMAL: non-HP stat +5 (ld a,5)
        uint8_t hp_offset     =  10;  // STAT_MIN_HP: HP stat +10 (ld a,10)
    } stat_formula{};

    // Escape formula constants — TryToRunAwayFromBattle (0f:58b3)
    // odds = (player_speed × speed_multiplier) / (wild_speed / 4) + (attempts-1) × attempt_addend
    struct EscapeParams {
        uint8_t speed_multiplier = 32;  // player_speed × N (ld a,32)
        uint8_t attempt_addend   = 30;  // per-attempt addition (ld b,30)
    } escape{};

    // Capture status bonus — PokeBallEffect (03:68a2)
    // Catch rate += bonus depending on target status.
    // Crystal bug: brn_psn_par_bonus is intended +5 but path is unreachable → effectively 0.
    struct CaptureStatusBonus {
        uint8_t slp_frz_bonus       = 10;  // Sleep/Freeze bonus (ld c,10)
        uint8_t brn_psn_par_bonus   =  5;  // Burn/Poison/Paralysis (ld c,5; BUG: unreachable)
    } capture_status{};

    // Experience formula constants — GiveExperiencePoints (0f:6e3b)
    struct ExpParams {
        uint8_t base_divisor = 7;  // exp = base_exp × level / divisor (ld a,7)
    } exp_formula{};

    // Residual damage fractions — GetEighthMaxHP / GetSixteenthMaxHP (0f:4c83/0f:4c76)
    // Derived from shift-count (GetQuarterMaxHP + K srl c → 1/(4×2^K)).
    struct ResidualFractionParams {
        uint8_t burn_poison_denom = 8;   // Burn/Poison: max_hp / 8 (1 srl c after /4)
        uint8_t toxic_denom       = 16;  // Toxic: max_hp / 16 (2 srl c after /4)
    } residual{};

    // Critical hit stage deltas — BattleCommand_Critical (0d:4631)
    struct CritStageDeltaParams {
        uint8_t held_item_delta       = 2;  // Lucky Punch / Stick (ld c, 2)
        uint8_t scope_lens_delta      = 1;  // Scope Lens (inc c)
        uint8_t focus_energy_delta    = 1;  // Focus Energy (inc c)
        // high_crit_move_delta = held_item_delta (same ld c, n instruction)
    } crit_deltas{};

    // Damage variation bounds — BattleCommand_DamageVariation (0d:4cfd)
    // The random byte is rotated right once (RRCA), then must be >= lower_bound_byte.
    // lower_bound_byte is the assembled immediate of `cp 85 percent + 1`:
    //   85 * 255 / 100 + 1 = 217 (0xD9)
    // Semantic: values [0xD9..0xFF] map to damage ×(value/0xFF) ≈ 85%..100%.
    struct DamageVariationParams {
        uint8_t lower_bound_byte = 0xD9;  // `cp 85 percent + 1` assembled → 0xD9
    } damage_variation{};

    // ========================================================================
    // Frontend-derived economy limits.
    // These are format constants derived from the source frontend's SRAM/BCD layout,
    // not SM83 immediate parameters.  They belong here (not in generic GameState)
    // so ROM hacks or alternate frontends can override them through the package.
    //
    // Crystal SRAM BCD widths:
    //   wPlayerMoney / wMomsMoney: 3 packed-BCD bytes → max 999,999
    //   wCoins: 2 packed-BCD bytes → max 9,999
    //   wNumItems / wItems bag: 20 item slots × 99 per slot
    //
    // Source: pokecrystal/constants/wram_constants.asm + wram.asm
    struct FrontendLimits {
        int32_t money_max  = 999999;  // BCD 3-byte max for player/mom money
        int32_t coin_max   =   9999;  // BCD 2-byte max for Game Corner coins
        int32_t item_qty_max  =   99; // Crystal bag semantics: 99 per slot
    } frontend_limits{};

    // Convenience getters — fall back to struct defaults (vanilla-correct).
    int32_t get_money_max()    const { return frontend_limits.money_max; }
    int32_t get_coin_max()     const { return frontend_limits.coin_max; }
    int32_t get_item_qty_max() const { return frontend_limits.item_qty_max; }

    // ======================================================================
    // SM83 lift status — bitmask recording which sub-structs were actually
    // extracted from ROM bytes vs fell back to the in-struct vanilla defaults.
    //
    // A set bit means the recognizer RAN and succeeded for that routine.
    // A clear bit means the address was zero, the span was OOB, or the
    // recognizer returned LiftResult::fail() — the sub-struct default is used.
    //
    // This is the ONLY authoritative indicator of lift success.  Code that
    // needs to distinguish "ROM-derived" from "assumed vanilla" must check
    // this mask rather than comparing field values to known vanilla constants.
    //
    // Bit assignments (stable — part of the BRLS wire format v2):
    enum : uint16_t {
        SM83_LIFTED_DAMAGE_FORMULA = 1u << 0,   // DamageFormulaParams
        SM83_LIFTED_AI_SCORES      = 1u << 1,   // AIScoreParams
        SM83_LIFTED_STAT_FORMULA   = 1u << 2,   // StatFormulaParams
        SM83_LIFTED_ESCAPE         = 1u << 3,   // EscapeParams
        SM83_LIFTED_CAPTURE_STATUS = 1u << 4,   // CaptureStatusBonus
        SM83_LIFTED_EXP            = 1u << 5,   // ExpParams
        SM83_LIFTED_RESIDUAL       = 1u << 6,   // ResidualFractionParams (both routines)
        SM83_LIFTED_CRIT_DELTAS    = 1u << 7,   // CritStageDeltaParams
        SM83_LIFTED_DAMAGE_VAR     = 1u << 8,   // DamageVariationParams
    };
    uint16_t sm83_lifted_mask = 0;  // initially: nothing lifted (all defaults)

    bool sm83_is_lifted(uint16_t bit) const { return (sm83_lifted_mask & bit) != 0; }

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

    // True if move_id is in the AI rain-dance synergy list.
    bool is_ai_rain_dance_move(uint8_t move_id) const {
        for (uint8_t m : ai_rain_dance_move_ids)
            if (m == move_id) return true;
        return false;
    }

    // True if move_id is in the AI sunny-day synergy list.
    bool is_ai_sunny_day_move(uint8_t move_id) const {
        for (uint8_t m : ai_sunny_day_move_ids)
            if (m == move_id) return true;
        return false;
    }

    // AI pass set for a trainer class (0-indexed).  Returns basic-only if out of range.
    AIPassSet get_trainer_ai_passes(size_t trainer_class_index) const {
        if (trainer_class_index >= trainer_class_ai.size()) return AIPassSet::basic_only();
        return trainer_class_ai[trainer_class_index].ai_passes;
    }

    // Trainer class DV values (0-indexed).  Returns vanilla default {9,8,8,8} if out of range.
    // dvs[0]=atk, dvs[1]=def, dvs[2]=spd, dvs[3]=spc
    std::array<uint8_t, 4> get_trainer_dvs(size_t trainer_class_index) const {
        if (trainer_class_index < trainer_class_ai.size()) {
            const auto& e = trainer_class_ai[trainer_class_index];
            return {e.dv_atk, e.dv_def, e.dv_spd, e.dv_spc};
        }
        return {9, 8, 8, 8};  // Vanilla default for trainers not in the table
    }

    // Base reward for a trainer class (0-indexed).  Returns 0 if out of range.
    // Used by ComputeTrainerReward: prize = base_reward × highest_party_level.
    uint8_t get_trainer_class_base_reward(size_t trainer_class_index) const {
        if (trainer_class_index >= trainer_class_ai.size()) return 0;
        return trainer_class_ai[trainer_class_index].base_reward;
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

    // Convenience helpers for SM83-lifted formula parameters.
    // These return the SM83-extracted value if available, or the struct default.
    uint8_t  get_level_divisor()          const { return damage_formula.level_divisor; }
    uint8_t  get_level_addend()           const { return damage_formula.level_addend; }
    uint8_t  get_damage_divisor()         const { return damage_formula.damage_divisor; }
    uint8_t  get_min_damage()             const { return damage_formula.min_damage; }
    uint8_t  get_ai_init_score()          const { return ai_scores.init_score; }
    uint8_t  get_ai_discourage_strong()   const { return ai_scores.discourage_strong; }
    uint8_t  get_stat_level_divisor()     const { return stat_formula.level_divisor; }
    uint8_t  get_stat_non_hp_offset()     const { return stat_formula.non_hp_offset; }
    uint8_t  get_stat_hp_offset()         const { return stat_formula.hp_offset; }
    uint8_t  get_escape_speed_mult()      const { return escape.speed_multiplier; }
    uint8_t  get_escape_attempt_add()     const { return escape.attempt_addend; }
    uint8_t  get_capture_slp_frz_bonus()  const { return capture_status.slp_frz_bonus; }
    uint8_t  get_exp_divisor()            const { return exp_formula.base_divisor; }
    uint8_t  get_burn_poison_denom()      const { return residual.burn_poison_denom; }
    uint8_t  get_toxic_denom()            const { return residual.toxic_denom; }
    uint8_t  get_crit_held_item_delta()   const { return crit_deltas.held_item_delta; }
    uint8_t  get_crit_scope_lens_delta()  const { return crit_deltas.scope_lens_delta; }
    uint8_t  get_crit_focus_energy_delta()const { return crit_deltas.focus_energy_delta; }
    uint8_t  get_damage_var_lower_bound() const { return damage_variation.lower_bound_byte; }
};

} // namespace enginemon
