#pragma once
// crystal/extract/species_extractor.hpp
// Crystal frontend: BaseData table extractor.
//
// Reads every species definition from the ROM's BaseData table using the
// profile's authoritative extent (profile.counts.num_pokemon) and record
// layout (profile.format.pokemon). The count is profile metadata — it is
// not derived from any ROM-internal self-describing length; the ROM table
// has no header or terminator.
//
// A non-251 profile (e.g. an expanded Crystal hack) uses exactly the same
// extraction path. Only the profile values differ.
//
// Output: SpeciesExtractResult containing one SpeciesDefinition per valid
// species (IDs 1..num_pokemon). Species 0 is SPECIES_NONE and is never
// extracted.

#include "crystal/rom/loader.hpp"
#include "crystal/rom/profile.hpp"
#include "engine/core/types.hpp"
#include <string>
#include <unordered_map>
#include <vector>

namespace crystal {

// Extracted base stats for one species.
// Populated directly from the BaseData ROM record using profile field offsets.
struct SpeciesDefinition {
    enginemon::SpeciesId id = 0;   // 1-indexed, matching Crystal's species numbering
    uint8_t dex_number = 0;        // Internal Crystal dex entry number (same as id for vanilla)

    // Base stats
    uint8_t hp       = 0;
    uint8_t attack   = 0;
    uint8_t defense  = 0;
    uint8_t speed    = 0;
    uint8_t sp_atk   = 0;
    uint8_t sp_def   = 0;

    // Type identifiers (Crystal type indices)
    uint8_t type1 = 0;
    uint8_t type2 = 0;  // == type1 for single-type species

    // Misc
    uint8_t catch_rate  = 0;
    uint8_t base_exp    = 0;
    uint8_t gender      = 0;   // gender byte at profile.format.pokemon.gender_offset
    uint8_t egg_cycles  = 0;
    uint8_t growth_rate = 0;
    uint8_t egg_groups  = 0;   // packed byte at profile.format.pokemon.egg_groups_offset

    // Held items (Crystal: 2 possible held items)
    uint8_t held_item1 = 0;
    uint8_t held_item2 = 0;

    // TM/HM compatibility bitfield (8 bytes)
    std::array<uint8_t, 8> tmhm_compat{};
};

// Result of extracting the full species table.
struct SpeciesExtractResult {
    bool success = false;
    std::string error;

    // One entry per valid species, keyed by SpeciesId (1..num_pokemon).
    // SPECIES_NONE (0) is never present.
    std::unordered_map<enginemon::SpeciesId, SpeciesDefinition> species;

    // Ordered list of extracted IDs (for deterministic iteration)
    std::vector<enginemon::SpeciesId> ordered_ids;
};

// Extract all species definitions from ROM.
// Reads profile.counts.num_pokemon records at profile.offsets.base_data.
// Fails if any record's bounds exceed the ROM.
SpeciesExtractResult extract_all_species(
    const RomData& rom,
    const ExtractionProfile& profile);

} // namespace crystal
