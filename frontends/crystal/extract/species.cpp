// crystal/extract/species.cpp
// Crystal frontend: BaseData table extractor implementation.
//
// The BaseData table is a flat array of fixed-size records:
//   base_data + (species_index_0based * base_data_size)
//
// Crystal indexes species 1..num_pokemon (SPECIES_NONE = 0 has no record).
// Entry for species S is at: base_data + (S-1) * base_data_size.
//
// The count (num_pokemon) is authoritative from the profile.
// The ROM has no length prefix or terminator for this table.
//
// Reference: pokecrystal/data/pokemon/base_stats/*.asm → BaseData

#include "crystal/extract/species_extractor.hpp"
#include <format>

namespace crystal {

SpeciesExtractResult extract_all_species(
    const RomData& rom,
    const ExtractionProfile& profile)
{
    SpeciesExtractResult result;

    const auto& o   = profile.offsets;
    const auto& fmt = profile.format.pokemon;
    const auto& c   = profile.counts;

    if (o.base_data == 0) {
        result.error = "profile.offsets.base_data is zero — not configured";
        return result;
    }

    if (c.num_pokemon == 0) {
        result.error = "profile.counts.num_pokemon is zero — nothing to extract";
        return result;
    }

    // Validate that the entire BaseData table fits inside the ROM.
    // All record accesses are guarded by this single upfront check.
    uint64_t table_bytes =
        static_cast<uint64_t>(c.num_pokemon) * fmt.base_data_size;
    if (o.base_data + table_bytes > rom.size()) {
        result.error = std::format(
            "BaseData table (base={:#x}, count={}, record_size={}) "
            "extends past ROM size {:#x}",
            o.base_data, c.num_pokemon, fmt.base_data_size, rom.size());
        return result;
    }

    result.species.reserve(c.num_pokemon);
    result.ordered_ids.reserve(c.num_pokemon);

    for (uint16_t i = 1; i <= c.num_pokemon; ++i) {
        uint32_t entry_addr = o.base_data +
            static_cast<uint32_t>(i - 1) * fmt.base_data_size;

        // Bounds already guaranteed by the upfront check above.
        auto rec = rom.read_bytes(entry_addr, fmt.base_data_size);

        SpeciesDefinition def;
        def.id         = static_cast<enginemon::SpeciesId>(i);
        def.dex_number = rec[fmt.dex_num_offset];
        def.hp         = rec[fmt.hp_offset];
        def.attack     = rec[fmt.atk_offset];
        def.defense    = rec[fmt.def_offset];
        def.speed      = rec[fmt.spd_offset];
        def.sp_atk     = rec[fmt.satk_offset];
        def.sp_def     = rec[fmt.sdef_offset];
        def.type1      = rec[fmt.type1_offset];
        def.type2      = rec[fmt.type2_offset];
        def.catch_rate = rec[fmt.catch_rate_offset];
        def.base_exp   = rec[fmt.base_exp_offset];
        def.gender     = rec[fmt.gender_offset];
        def.egg_cycles = rec[fmt.egg_cycles_offset];
        def.growth_rate= rec[fmt.growth_rate_offset];
        def.egg_groups = rec[fmt.egg_groups_offset];

        // Held items: two consecutive bytes at items_offset
        if (fmt.items_offset + 1 < fmt.base_data_size) {
            def.held_item1 = rec[fmt.items_offset];
            def.held_item2 = rec[fmt.items_offset + 1];
        }

        // TM/HM compatibility: 8 bytes at tmhm_offset
        if (fmt.tmhm_offset + 8 <= fmt.base_data_size) {
            for (int b = 0; b < 8; ++b) {
                def.tmhm_compat[b] = rec[fmt.tmhm_offset + b];
            }
        }

        result.species[def.id] = def;
        result.ordered_ids.push_back(def.id);
    }

    result.success = true;
    return result;
}

} // namespace crystal
