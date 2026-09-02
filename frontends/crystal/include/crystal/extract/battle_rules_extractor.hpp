#pragma once
// crystal/extract/battle_rules_extractor.hpp
// Crystal frontend: battle rule table extractor.
//
// Reads all ROM-resident battle rule tables from the Crystal ROM and produces
// an enginemon::BattleRules struct ready for serialization into the BRLS
// EMON package chunk.
//
// Every table address comes from profile.offsets.* — no addresses are
// hardcoded in this file.  A ROM hack that relocates any of these tables only
// needs updated profile offsets.
//
// Tables extracted:
//   Fixed-length (profile-count-driven):
//     StatLevelMultipliers_Applied  — 13 × 2 bytes  (stat_stage_mult)
//     AccuracyLevelMultipliers      — 13 × 2 bytes  (acc_stage_mult)
//     CriticalHitChances            — 7  × 1 byte   (crit_chances)
//     WobbleProbabilities           — num_wobble_entries × 2 bytes
//     TrainerClassAttributes        — num_trainer_classes × 7 bytes
//
//   Sentinel-terminated (scanned until 0xFF):
//     WeatherTypeModifiers          — 3 bytes/entry
//     WeatherMoveModifiers          — 3 bytes/entry
//     CriticalHitMoves              — 1 byte/entry
//     MoveEffectPriorities          — 2 bytes/entry
//     StatusOnlyEffects             — 1 byte/entry
//     RiskyEffects                  — 1 byte/entry
//     StallMoves                    — 1 byte/entry  (move IDs)
//     UsefulMoves                   — 1 byte/entry  (move IDs)
//     ResidualMoves                 — 1 byte/entry  (move IDs)
//     EncoreMoves                   — 1 byte/entry  (move IDs)
//
// Fail-closed: any address that is zero or any table that extends past the
// ROM boundary causes the extraction to fail with a descriptive error string.
// Partial results are never returned.

#include "crystal/rom/loader.hpp"
#include "crystal/rom/profile.hpp"
#include "engine/battle/battle_rules.hpp"
#include <string>

namespace crystal {

struct BattleRulesExtractResult {
    bool success = false;
    std::string error;

    // Populated only when success == true.
    enginemon::BattleRules rules;
};

// Extract all battle rule tables from the ROM.
// Uses profile.offsets for addresses and profile.counts for table lengths.
// Returns success=false with a descriptive error on any bounds or address failure.
BattleRulesExtractResult extract_battle_rules(
    const RomData&           rom,
    const ExtractionProfile& profile);

} // namespace crystal
