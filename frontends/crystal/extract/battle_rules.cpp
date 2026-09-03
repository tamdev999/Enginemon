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
#include "crystal/extract/sm83_lifter.hpp"
#include "crystal/battle/crystal_effects.hpp"
#include <cstdio>
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
    // 6b. TypeMatchups — 3 bytes/entry {atk_type, def_type, multiplier}, 0xFF sentinel
    // Source: TypeMatchups (0d:4bb1) — only non-neutral entries are stored.
    // Multiplier: 0=immune, 5=NVE, 20=SE (neutral=10 is the TypeChart default, absent).
    // This populates BattleRules::type_matchups which the runtime uses to build
    // registries_.type_chart via rules.apply_to(chart).
    // ------------------------------------------------------------------
    {
        const char* tname = "TypeMatchups";
        if (o.type_matchups == 0) {
            err = std::format("{}: address is zero — not configured in profile", tname);
            return result;
        }
        rules.type_matchups.clear();
        uint32_t ptr = o.type_matchups;
        constexpr uint32_t ENTRY_SIZE = 3;
        constexpr uint32_t MAX_ENTRIES = 512;  // safety cap
        for (uint32_t i = 0; i < MAX_ENTRIES; ++i) {
            if (ptr + ENTRY_SIZE > rom.size()) {
                err = std::format("{}: sentinel not found before ROM end (offset 0x{:05x})",
                                  tname, ptr);
                return result;
            }
            uint8_t atk = rom.read_byte(ptr);
            if (atk == 0xFF) break;  // sentinel
            // 0xFE = separator between "always was this" and "added in Gen 2" sections.
            // Skip this byte and continue parsing.
            if (atk == 0xFE) { ptr += 1; continue; }
            uint8_t def  = rom.read_byte(ptr + 1);
            uint8_t mult = rom.read_byte(ptr + 2);
            // Validate multiplier: Crystal uses 0=immune, 5=NVE, 20=SE (10 never stored)
            if (mult != 0 && mult != 5 && mult != 20) {
                err = std::format("{}: unexpected multiplier byte {} at offset 0x{:05x}",
                                  tname, mult, ptr);
                return result;
            }
            // Sanity check on type IDs: Crystal type IDs are in 0x00–0x1F range in vanilla,
            // with Polished and other hacks adding types up to ~0x25 or so.
            // Reject any type ID >= 0x40 (64) as almost certainly wrong-address data.
            // This catches the Polished Crystal case where the wrong address produces
            // type IDs in the range 0x40–0xFF (plainly not type matchup data).
            constexpr uint8_t MAX_REASONABLE_TYPE_ID = 0x3F;  // generous for all known hacks
            if (atk > MAX_REASONABLE_TYPE_ID || def > MAX_REASONABLE_TYPE_ID) {
                err = std::format("{}: implausible type ID: atk=0x{:02x} def=0x{:02x} — "
                                  "likely wrong address for TypeMatchups table in this ROM; "
                                  "update profile.offsets.type_matchups",
                                  tname, atk, def);
                return result;
            }
            enginemon::BattleRules::TypeMatchupEntry entry;
            entry.attacking = atk;
            entry.defending = def;
            entry.multiplier = mult;
            rules.type_matchups.push_back(entry);
            ptr += ENTRY_SIZE;
        }
        if (rules.type_matchups.empty()) {
            err = std::format("{}: extracted 0 entries — ROM data wrong or address incorrect",
                              tname);
            return result;
        }
    }

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

    // ------------------------------------------------------------------
    // 18. SM83 static lifting — formula parameter extraction.
    //
    // Each recognizer is called with a ROM span covering the routine.
    // Failure is non-fatal: the struct default (vanilla-correct) value
    // remains in place.  A failure is logged to stderr.
    //
    // Recognizer success is recorded in rules.sm83_lifted_mask so callers
    // can distinguish ROM-extracted values from struct defaults.
    // ------------------------------------------------------------------
    {
        auto lift = [&](uint32_t flat_addr, uint32_t span_len, const char* name,
                        uint16_t mask_bit,
                        auto recognizer_fn, auto apply_fn) {
            if (flat_addr == 0) return;  // address not configured in profile
            auto span = rom_span_at(rom, flat_addr, span_len);
            if (!span.data) {
                // OOB address — emit warning but do not hard-fail.
                std::fprintf(stderr, "SM83 lift %s: address 0x%05X out of ROM bounds\n",
                             name, flat_addr);
                return;
            }
            auto r = recognizer_fn(span);
            if (!r.ok) {
                // Recognizer rejected the routine shape.  Default values remain.
                std::fprintf(stderr, "SM83 lift %s warning: %s\n", name, r.error.c_str());
                return;
            }
            apply_fn(r);
            rules.sm83_lifted_mask |= mask_bit;  // record successful extraction
        };

        // AIDiscourageMove — p[0] = discouragement delta
        lift(o.sm83_ai_discourage_move, ProfileOffsets::SM83_SPAN_AI_DISCOURAGE,
             "AIDiscourageMove", enginemon::BattleRules::SM83_LIFTED_AI_SCORES,
             [](const RomSpan& s) { return lift_ai_discourage_move(s); },
             [&](const LiftResult& r) {
                 rules.ai_scores.discourage_strong = r.p[0];
             });

        // AIChooseMove — p[0] = init score
        lift(o.sm83_ai_choose_move, ProfileOffsets::SM83_SPAN_AI_CHOOSE_MOVE,
             "AIChooseMove", enginemon::BattleRules::SM83_LIFTED_AI_SCORES,
             [](const RomSpan& s) { return lift_ai_choose_move_scores(s); },
             [&](const LiftResult& r) {
                 rules.ai_scores.init_score = r.p[0];
             });

        // GiveExperiencePoints — p[0] = exp base divisor
        lift(o.sm83_give_exp_points, ProfileOffsets::SM83_SPAN_GIVE_EXP,
             "GiveExperiencePoints", enginemon::BattleRules::SM83_LIFTED_EXP,
             [](const RomSpan& s) { return lift_exp_divisor(s); },
             [&](const LiftResult& r) {
                 rules.exp_formula.base_divisor = r.p[0];
             });

        // BattleCommand_DamageVariation — p[0] = RRCA lower bound byte
        lift(o.sm83_damage_variation, ProfileOffsets::SM83_SPAN_DAMAGE_VARIATION,
             "BattleCommand_DamageVariation", enginemon::BattleRules::SM83_LIFTED_DAMAGE_VAR,
             [](const RomSpan& s) { return lift_damage_variation(s); },
             [&](const LiftResult& r) {
                 rules.damage_variation.lower_bound_byte = r.p[0];
             });

        // PokeBallEffect — p[0]=SLP/FRZ bonus, p[1]=BRN/PSN/PAR (bug), p[2]=none
        lift(o.sm83_poke_ball_effect, ProfileOffsets::SM83_SPAN_POKE_BALL,
             "PokeBallEffect", enginemon::BattleRules::SM83_LIFTED_CAPTURE_STATUS,
             [](const RomSpan& s) { return lift_capture_status_bonus(s); },
             [&](const LiftResult& r) {
                 rules.capture_status.slp_frz_bonus     = r.p[0];
                 rules.capture_status.brn_psn_par_bonus = r.p[1];
             });

        // TryToRunAwayFromBattle — p[0]=speed multiplier, p[1]=per-attempt addend
        lift(o.sm83_try_to_run_away, ProfileOffsets::SM83_SPAN_TRY_TO_RUN,
             "TryToRunAwayFromBattle", enginemon::BattleRules::SM83_LIFTED_ESCAPE,
             [](const RomSpan& s) { return lift_escape_constants(s); },
             [&](const LiftResult& r) {
                 rules.escape.speed_multiplier = r.p[0];
                 rules.escape.attempt_addend   = r.p[1];
             });

        // CalcMonStatC — p[0]=/100, p[1]=non_hp_offset, p[2]=hp_offset
        lift(o.sm83_calc_mon_stat_c, ProfileOffsets::SM83_SPAN_CALC_MON_STAT_C,
             "CalcMonStatC", enginemon::BattleRules::SM83_LIFTED_STAT_FORMULA,
             [](const RomSpan& s) { return lift_stat_formula_offsets(s); },
             [&](const LiftResult& r) {
                 rules.stat_formula.level_divisor  = r.p[0];
                 rules.stat_formula.non_hp_offset  = r.p[1];
                 rules.stat_formula.hp_offset      = r.p[2];
             });

        // BattleCommand_DamageCalc — p[0]=level_div, p[1]=level_add, p[2]=dmg_div, p[3]=min_dmg
        lift(o.sm83_damage_calc, ProfileOffsets::SM83_SPAN_DAMAGE_CALC,
             "BattleCommand_DamageCalc", enginemon::BattleRules::SM83_LIFTED_DAMAGE_FORMULA,
             [](const RomSpan& s) { return lift_damage_calc_constants(s); },
             [&](const LiftResult& r) {
                 rules.damage_formula.level_divisor  = r.p[0];
                 rules.damage_formula.level_addend   = r.p[1];
                 rules.damage_formula.damage_divisor = r.p[2];
                 rules.damage_formula.min_damage     = r.p[3];
             });

        // GetEighthMaxHP — p[0] = denominator (8)
        lift(o.sm83_get_eighth_max_hp, ProfileOffsets::SM83_SPAN_GET_EIGHTH_HP,
             "GetEighthMaxHP", enginemon::BattleRules::SM83_LIFTED_RESIDUAL,
             [](const RomSpan& s) { return lift_residual_fraction(s); },
             [&](const LiftResult& r) {
                 rules.residual.burn_poison_denom = r.p[0];
             });

        // GetSixteenthMaxHP — p[0] = denominator (16)
        lift(o.sm83_get_sixteenth_max_hp, ProfileOffsets::SM83_SPAN_GET_SIXTEENTH_HP,
             "GetSixteenthMaxHP", enginemon::BattleRules::SM83_LIFTED_RESIDUAL,
             [](const RomSpan& s) { return lift_residual_fraction(s); },
             [&](const LiftResult& r) {
                 rules.residual.toxic_denom = r.p[0];
             });

        // BattleCommand_Critical — p[0]=held_item, p[1]=scope_lens, p[2]=focus_energy
        lift(o.sm83_critical, ProfileOffsets::SM83_SPAN_CRITICAL,
             "BattleCommand_Critical", enginemon::BattleRules::SM83_LIFTED_CRIT_DELTAS,
             [](const RomSpan& s) { return lift_crit_stage_deltas(s); },
             [&](const LiftResult& r) {
                 rules.crit_deltas.held_item_delta    = r.p[0];
                 rules.crit_deltas.scope_lens_delta   = r.p[1];
                 rules.crit_deltas.focus_energy_delta = r.p[2];
             });
    }

    result.success = true;
    return result;
}

} // namespace crystal
