// crystal/extract/battle_rules.cpp
// Crystal frontend: battle rule table extractor implementation.
//
// Reference: pokecrystal.sym (Crystal v1.1) + pokecrystal source:
//   data/battle/stat_multipliers.asm       — StatLevelMultipliers_Applied
//   data/battle/accuracy_multipliers.asm   — AccuracyLevelMultipliers
//   data/battle/critical_hit_chances.asm   — CriticalHitChances
//   data/battle/wobble_probabilities.asm   — WobbleProbabilities
//   data/battle/weather_modifiers.asm      — WeatherTypeModifiers / WeatherMoveModifiers
//   data/moves/critical_hit_moves.asm      — CriticalHitMoves
//   data/moves/effects_priorities.asm      — MoveEffectPriorities
//   data/battle/ai/status_only_effects.asm — StatusOnlyEffects
//   data/battle/ai/risky_effects.asm       — RiskyEffects
//   data/battle/ai/stall_moves.asm         — StallMoves
//   data/battle/ai/useful_moves.asm        — UsefulMoves
//   data/battle/ai/residual_moves.asm      — ResidualMoves
//   data/battle/ai/encore_moves.asm        — EncoreMoves
//   data/trainers/attributes.asm           — TrainerClassAttributes

#include "crystal/extract/battle_rules_extractor.hpp"
#include "crystal/battle/crystal_effects.hpp"
#include <format>
#include <algorithm>

namespace crystal {

namespace {

// ============================================================================
// Helper: extract a fixed-length table of (num, den) pairs.
// Used for stat/accuracy stage multiplier tables.
// Returns false and sets error on bounds failure.
// ============================================================================
static bool extract_stage_mult_table(
    const RomData& rom,
    uint32_t addr,
    const char* table_name,
    enginemon::StageMult13& out,
    std::string& error)
{
    if (addr == 0) {
        error = std::format("{}: address is zero — not configured in profile", table_name);
        return false;
    }
    // 13 entries × 2 bytes each = 26 bytes
    constexpr uint32_t TABLE_BYTES = 13 * 2;
    if (static_cast<uint64_t>(addr) + TABLE_BYTES > rom.size()) {
        error = std::format(
            "{}: table at 0x{:05x} ({} bytes) extends past ROM size 0x{:05x}",
            table_name, addr, TABLE_BYTES, rom.size());
        return false;
    }
    auto bytes = rom.read_bytes(addr, TABLE_BYTES);
    for (int i = 0; i < 13; ++i) {
        out[i].numerator   = bytes[static_cast<size_t>(i * 2)];
        out[i].denominator = bytes[static_cast<size_t>(i * 2 + 1)];
        // Sanity: denominator of 0 would cause division-by-zero at runtime.
        if (out[i].denominator == 0) {
            error = std::format(
                "{}: entry [{}] has denominator=0 — ROM data corrupt or wrong address",
                table_name, i);
            return false;
        }
    }
    return true;
}

// ============================================================================
// Helper: extract a 1-byte-per-entry sentinel-terminated list into a vector.
// Sentinel is 0xFF.  Safety limit prevents runaway scans on corrupt ROM data.
// ============================================================================
static bool extract_byte_list(
    const RomData& rom,
    uint32_t addr,
    const char* table_name,
    std::vector<uint8_t>& out,
    std::string& error,
    uint32_t safety_limit = 256)
{
    if (addr == 0) {
        error = std::format("{}: address is zero — not configured in profile", table_name);
        return false;
    }
    if (addr >= rom.size()) {
        error = std::format("{}: address 0x{:05x} is past ROM size 0x{:05x}",
                            table_name, addr, rom.size());
        return false;
    }
    out.clear();
    for (uint32_t i = 0; i < safety_limit; ++i) {
        uint32_t cur = addr + i;
        if (cur >= rom.size()) {
            error = std::format(
                "{}: reached ROM boundary at 0x{:05x} without 0xFF sentinel",
                table_name, cur);
            return false;
        }
        uint8_t b = rom.read_byte(cur);
        if (b == 0xFF) break;
        out.push_back(b);
    }
    return true;
}

// ============================================================================
// Helper: extract a 2-byte-per-entry sentinel-terminated list.
// Sentinel: first byte of entry == 0xFF.
// ============================================================================
template<typename T>
static bool extract_two_byte_list(
    const RomData& rom,
    uint32_t addr,
    const char* table_name,
    std::vector<T>& out,
    std::string& error,
    uint32_t safety_limit = 128)
{
    if (addr == 0) {
        error = std::format("{}: address is zero — not configured in profile", table_name);
        return false;
    }
    if (addr >= rom.size()) {
        error = std::format("{}: address 0x{:05x} is past ROM size 0x{:05x}",
                            table_name, addr, rom.size());
        return false;
    }
    out.clear();
    for (uint32_t i = 0; i < safety_limit; ++i) {
        uint32_t cur = addr + i * 2;
        if (cur + 2 > rom.size()) {
            error = std::format(
                "{}: reached ROM boundary at 0x{:05x} without 0xFF sentinel",
                table_name, cur);
            return false;
        }
        uint8_t b0 = rom.read_byte(cur);
        if (b0 == 0xFF) break;
        uint8_t b1 = rom.read_byte(cur + 1);
        T entry;
        entry.effect_id = b0;
        entry.priority  = b1;
        out.push_back(entry);
    }
    return true;
}

// Specialisation for WeatherModifierEntry (field names differ from MoveEffectPriorityEntry).
// Template above uses .effect_id/.priority — add a non-template overload for weather.
static bool extract_weather_table(
    const RomData& rom,
    uint32_t addr,
    const char* table_name,
    std::vector<enginemon::WeatherModifierEntry>& out,
    std::string& error,
    uint32_t safety_limit = 32)
{
    if (addr == 0) {
        error = std::format("{}: address is zero — not configured in profile", table_name);
        return false;
    }
    if (addr >= rom.size()) {
        error = std::format("{}: address 0x{:05x} is past ROM size 0x{:05x}",
                            table_name, addr, rom.size());
        return false;
    }
    out.clear();
    for (uint32_t i = 0; i < safety_limit; ++i) {
        uint32_t cur = addr + i * 3;
        if (cur + 3 > rom.size()) {
            error = std::format(
                "{}: reached ROM boundary at 0x{:05x} without 0xFF sentinel",
                table_name, cur);
            return false;
        }
        uint8_t b0 = rom.read_byte(cur);
        if (b0 == 0xFF) break;
        enginemon::WeatherModifierEntry e;
        e.weather_id = b0;
        e.type_id    = rom.read_byte(cur + 1);
        e.multiplier = rom.read_byte(cur + 2);
        out.push_back(e);
    }
    return true;
}

} // anonymous namespace

// ============================================================================
// Main extraction entry point
// ============================================================================

BattleRulesExtractResult extract_battle_rules(
    const RomData&           rom,
    const ExtractionProfile& profile)
{
    BattleRulesExtractResult result;
    const auto& o = profile.offsets;
    const auto& c = profile.counts;
    auto& rules   = result.rules;
    std::string& err = result.error;

    // ------------------------------------------------------------------
    // 1. StatLevelMultipliers_Applied — 13 × {num, den}
    // ------------------------------------------------------------------
    if (!extract_stage_mult_table(rom, o.stat_level_multipliers,
                                  "StatLevelMultipliers_Applied",
                                  rules.stat_stage_mult, err))
        return result;

    // ------------------------------------------------------------------
    // 2. AccuracyLevelMultipliers — 13 × {num, den}
    // ------------------------------------------------------------------
    if (!extract_stage_mult_table(rom, o.accuracy_level_multipliers,
                                  "AccuracyLevelMultipliers",
                                  rules.acc_stage_mult, err))
        return result;

    // ------------------------------------------------------------------
    // 3. CriticalHitChances — exactly 7 bytes
    // ------------------------------------------------------------------
    {
        const char* tname = "CriticalHitChances";
        if (o.critical_hit_chances == 0) {
            err = std::format("{}: address is zero — not configured in profile", tname);
            return result;
        }
        constexpr uint32_t TABLE_BYTES = 7;
        if (static_cast<uint64_t>(o.critical_hit_chances) + TABLE_BYTES > rom.size()) {
            err = std::format(
                "{}: table at 0x{:05x} ({} bytes) extends past ROM size 0x{:05x}",
                tname, o.critical_hit_chances, TABLE_BYTES, rom.size());
            return result;
        }
        auto bytes = rom.read_bytes(o.critical_hit_chances, TABLE_BYTES);
        for (int i = 0; i < 7; ++i)
            rules.crit_chances[i] = bytes[i];
        // Sanity: stage-0 crit chance of 0 means every move would crit.
        if (rules.crit_chances[0] == 0) {
            err = std::format("{}: stage-0 threshold is 0 — ROM data corrupt or wrong address",
                              tname);
            return result;
        }
    }

    // ------------------------------------------------------------------
    // 4. WobbleProbabilities — profile.offsets.num_wobble_entries × 2 bytes
    // ------------------------------------------------------------------
    {
        const char* tname = "WobbleProbabilities";
        const uint8_t n = o.num_wobble_entries;
        if (o.wobble_probabilities == 0) {
            err = std::format("{}: address is zero — not configured in profile", tname);
            return result;
        }
        if (n == 0) {
            err = std::format("{}: num_wobble_entries is 0 — not configured in profile", tname);
            return result;
        }
        uint32_t table_bytes = static_cast<uint32_t>(n) * 2u;
        if (static_cast<uint64_t>(o.wobble_probabilities) + table_bytes > rom.size()) {
            err = std::format(
                "{}: table at 0x{:05x} ({} bytes) extends past ROM size 0x{:05x}",
                tname, o.wobble_probabilities, table_bytes, rom.size());
            return result;
        }
        auto bytes = rom.read_bytes(o.wobble_probabilities, table_bytes);
        rules.wobble_probabilities.clear();
        rules.wobble_probabilities.reserve(n);
        for (uint8_t i = 0; i < n; ++i) {
            std::array<uint8_t, 2> entry{
                bytes[static_cast<size_t>(i * 2)],
                bytes[static_cast<size_t>(i * 2 + 1)]
            };
            rules.wobble_probabilities.push_back(entry);
        }
    }

    // ------------------------------------------------------------------
    // 5. WeatherTypeModifiers — 3 bytes/entry, 0xFF sentinel
    // ------------------------------------------------------------------
    if (!extract_weather_table(rom, o.weather_type_modifiers,
                               "WeatherTypeModifiers",
                               rules.weather_type_modifiers, err))
        return result;

    // ------------------------------------------------------------------
    // 6. WeatherMoveModifiers — 3 bytes/entry, 0xFF sentinel
    // ------------------------------------------------------------------
    if (!extract_weather_table(rom, o.weather_move_modifiers,
                               "WeatherMoveModifiers",
                               rules.weather_move_modifiers, err))
        return result;

    // ------------------------------------------------------------------
    // 7. CriticalHitMoves — 1 byte/entry (move constants = MoveId ≤ 255), 0xFF sentinel
    // ------------------------------------------------------------------
    {
        std::vector<uint8_t> raw_ids;
        if (!extract_byte_list(rom, o.critical_hit_moves,
                               "CriticalHitMoves", raw_ids, err))
            return result;
        // Widen each byte to semantic MoveId: Crystal MOVE_ANIM = MoveId for standard moves.
        rules.high_crit_moves.clear();
        rules.high_crit_moves.reserve(raw_ids.size());
        for (uint8_t b : raw_ids)
            rules.high_crit_moves.push_back(static_cast<enginemon::MoveId>(b));
    }

    // ------------------------------------------------------------------
    // 8. MoveEffectPriorities — 2 bytes/entry {effect_id, priority}, 0xFF sentinel
    // ------------------------------------------------------------------
    if (!extract_two_byte_list(rom, o.move_effect_priorities,
                               "MoveEffectPriorities",
                               rules.effect_priorities, err))
        return result;

    // ------------------------------------------------------------------
    // 9–14. AI move/effect lists — 1 byte/entry, 0xFF sentinel
    // ------------------------------------------------------------------
    // Effect-ID lists (status_only, risky): raw Crystal EFFECT_* bytes → semantic EffectId
    // Move-ID lists (stall, useful, residual, encore): raw move IDs stay as-is
    if (!extract_byte_list(rom, o.ai_status_only_effects,
                           "StatusOnlyEffects",
                           rules.ai_status_only_effects, err))
        return result;
    // Convert status_only effect IDs: Crystal raw bytes → semantic EffectId
    for (auto& b : rules.ai_status_only_effects)
        b = crystal::to_semantic_effect(b);

    if (!extract_byte_list(rom, o.ai_risky_effects,
                           "RiskyEffects",
                           rules.ai_risky_effects, err))
        return result;
    // Convert risky effect IDs: Crystal raw bytes → semantic EffectId
    for (auto& b : rules.ai_risky_effects)
        b = crystal::to_semantic_effect(b);

    if (!extract_byte_list(rom, o.ai_stall_moves,
                           "StallMoves",
                           rules.ai_stall_move_ids, err))
        return result;

    if (!extract_byte_list(rom, o.ai_useful_moves,
                           "UsefulMoves",
                           rules.ai_useful_move_ids, err))
        return result;

    if (!extract_byte_list(rom, o.ai_residual_moves,
                           "ResidualMoves",
                           rules.ai_residual_move_ids, err))
        return result;

    if (!extract_byte_list(rom, o.ai_encore_moves,
                           "EncoreMoves",
                           rules.ai_encore_move_ids, err))
        return result;

    // ------------------------------------------------------------------
    // 15. AI weather synergy move lists — 1 byte/entry, 0xFF sentinel
    // ------------------------------------------------------------------
    if (!extract_byte_list(rom, o.ai_rain_dance_moves,
                           "RainDanceMoves",
                           rules.ai_rain_dance_move_ids, err))
        return result;

    if (!extract_byte_list(rom, o.ai_sunny_day_moves,
                           "SunnyDayMoves",
                           rules.ai_sunny_day_move_ids, err))
        return result;

    // ------------------------------------------------------------------
    // 15b. AI stat-effect lists — synthesized from Crystal's effect-ID ranges.
    // These use correct Crystal EFFECT_* values (from move_effect_constants.asm):
    //   Stat-up 1:  ATTACK_UP(10) .. EVASION_UP(16)    — 7 entries
    //   Stat-up 2:  ATTACK_UP_2(50) .. EVASION_UP_2(56) — 7 entries
    //   Stat-down 1: ATTACK_DOWN(18) .. EVASION_DOWN(24)   — 7 entries
    //   Stat-down 2: ATTACK_DOWN_2(58) .. EVASION_DOWN_2(64) — 7 entries
    // The engine AI uses these lists (not hardcoded Crystal ranges) so it stays
    // frontend-neutral: a future frontend can populate different values.
    // ------------------------------------------------------------------
    {
        rules.ai_stat_up_effects.clear();
        for (uint8_t e = crystal::EffectId::STAT_UP_MIN;  e <= crystal::EffectId::STAT_UP_MAX;  ++e)
            rules.ai_stat_up_effects.push_back(e);
        for (uint8_t e = crystal::EffectId::STAT_UP2_MIN; e <= crystal::EffectId::STAT_UP2_MAX; ++e)
            rules.ai_stat_up_effects.push_back(e);
    }
    {
        rules.ai_stat_down_effects.clear();
        for (uint8_t e = crystal::EffectId::STAT_DOWN_MIN;  e <= crystal::EffectId::STAT_DOWN_MAX;  ++e)
            rules.ai_stat_down_effects.push_back(e);
        for (uint8_t e = crystal::EffectId::STAT_DOWN2_MIN; e <= crystal::EffectId::STAT_DOWN2_MAX; ++e)
            rules.ai_stat_down_effects.push_back(e);
    }

    // ------------------------------------------------------------------
    // 16. TrainerClassAttributes — num_trainer_classes × 7 bytes
    //
    // Each entry (from trainer_data_constants.asm):
    //   byte 0:   TRNATTR_ITEM1           (item1)
    //   byte 1:   TRNATTR_ITEM2           (item2)
    //   byte 2:   TRNATTR_BASEMONEY       (base_reward)
    //   bytes 3-4: TRNATTR_AI_MOVE_WEIGHTS (ai_passes, decoded to AIPassSet)
    //   bytes 5-6: TRNATTR_AI_ITEM_SWITCH  (ai_item_flags, LE word)
    // ------------------------------------------------------------------
    {
        const char* tname = "TrainerClassAttributes";
        const uint16_t n_classes = c.num_trainer_classes;
        if (o.trainer_class_attributes == 0) {
            err = std::format("{}: address is zero — not configured in profile", tname);
            return result;
        }
        if (n_classes == 0) {
            err = std::format("{}: profile.counts.num_trainer_classes is 0", tname);
            return result;
        }
        constexpr uint32_t ENTRY_SIZE = 7;
        uint64_t table_bytes = static_cast<uint64_t>(n_classes) * ENTRY_SIZE;
        if (static_cast<uint64_t>(o.trainer_class_attributes) + table_bytes > rom.size()) {
            err = std::format(
                "{}: table at 0x{:05x} ({} bytes) extends past ROM size 0x{:05x}",
                tname, o.trainer_class_attributes,
                static_cast<uint32_t>(table_bytes), rom.size());
            return result;
        }
        rules.trainer_class_ai.clear();
        rules.trainer_class_ai.reserve(n_classes);
        for (uint16_t i = 0; i < n_classes; ++i) {
            uint32_t entry_addr = o.trainer_class_attributes +
                                  static_cast<uint32_t>(i) * ENTRY_SIZE;
            auto rec = rom.read_bytes(entry_addr, ENTRY_SIZE);
            enginemon::TrainerClassAIEntry e;
            e.item1       = rec[0];
            e.item2       = rec[1];
            e.base_reward = rec[2];
            // Crystal TRNATTR_AI_MOVE_WEIGHTS bit positions (trainer_data_constants.asm):
            //   bit 0 = AI_BASIC      bit 1 = AI_SETUP    bit 2 = AI_TYPES
            //   bit 3 = AI_OFFENSIVE  bit 4 = AI_SMART
            // These bit positions are Crystal-specific and belong only in this extractor.
            const uint16_t raw_move = static_cast<uint16_t>(rec[3] | (rec[4] << 8));
            const uint16_t flags = (raw_move != 0) ? raw_move : 0x0001u; // default: BASIC
            e.ai_passes.run_basic     = true;                            // always active
            e.ai_passes.run_setup     = (flags & (1u << 1)) != 0;
            e.ai_passes.run_types     = (flags & (1u << 2)) != 0;
            e.ai_passes.run_offensive = (flags & (1u << 3)) != 0;
            e.ai_passes.run_smart     = (flags & (1u << 4)) != 0;
            e.ai_item_flags = static_cast<uint16_t>(rec[5] | (rec[6] << 8));
            rules.trainer_class_ai.push_back(e);
        }
    }

    // ------------------------------------------------------------------
    // 17. TrainerClassDVs — num_trainer_classes × 2 bytes
    //
    // Each entry is two bytes encoding 4×4-bit DV values:
    //   byte 0: {atk<<4 | def}   byte 1: {spd<<4 | spc}
    // Source: data/trainers/dvs.asm (09:70d6)
    // Note: table has exactly num_trainer_classes entries; parallel with trainer_class_ai.
    // ------------------------------------------------------------------
    {
        const char* tname = "TrainerClassDVs";
        const uint16_t n_classes = c.num_trainer_classes;
        if (o.trainer_class_dvs != 0) {
            // Table must cover exactly num_trainer_classes × 2 bytes
            const uint32_t table_bytes = static_cast<uint32_t>(n_classes) * 2u;
            if (static_cast<uint64_t>(o.trainer_class_dvs) + table_bytes > rom.size()) {
                err = std::format(
                    "{}: table at 0x{:05x} ({} bytes) extends past ROM size 0x{:05x}",
                    tname, o.trainer_class_dvs, table_bytes, rom.size());
                return result;
            }
            auto bytes = rom.read_bytes(o.trainer_class_dvs, table_bytes);
            for (uint16_t i = 0; i < n_classes && i < static_cast<uint16_t>(rules.trainer_class_ai.size()); ++i) {
                uint8_t b0 = bytes[static_cast<size_t>(i * 2)];      // atk<<4 | def
                uint8_t b1 = bytes[static_cast<size_t>(i * 2 + 1)];  // spd<<4 | spc
                auto& e = rules.trainer_class_ai[i];
                e.dv_atk = (b0 >> 4) & 0x0F;
                e.dv_def = (b0)      & 0x0F;
                e.dv_spd = (b1 >> 4) & 0x0F;
                e.dv_spc = (b1)      & 0x0F;
            }
        } else {
            // Profile does not provide TrainerClassDVs address — use vanilla default 9/8/8/8
            // This is a valid state for profiles that don't supply the address.
            // make_battle_pokemon falls back to the default {9,8,8,8} via get_trainer_dvs().
        }
    }

    result.success = true;
    return result;
}

} // namespace crystal
