#pragma once
// crystal/extract/item_extractor.hpp
//
// Crystal frontend: ItemAttributes ROM table extractor.
//
// Reads the Crystal item_attributes table (ITEMATTR_STRUCT_LENGTH = 7 bytes per
// item) and semanticizes each entry into engine-semantic ItemData fields.
//
// The semanticization step maps Crystal HELD_* raw bytes → HeldItemEffectType,
// resolves type booster mappings, and encodes species restrictions for
// Stick (Farfetch'd), Lucky Punch (Chansey), and Metal Powder (Ditto).
//
// The generic engine runtime never sees raw Crystal HELD_* values or raw item/
// species IDs used as dispatch constants.

#include "crystal/rom/loader.hpp"
#include "crystal/rom/profile.hpp"
#include "crystal/output/native_package.hpp"
#include <string>
#include <vector>

namespace crystal {

struct ItemExtractResult {
    bool success = false;
    std::string error;
    std::vector<PackageWriter::ItemDataEntry> items;
};

// Extract and semanticize all items from the Crystal ROM.
// Reads the item_attributes table at profile.offsets.item_attributes
// (7 bytes per entry, profile.counts.num_items - 1 entries: items 1..num_items-1).
// Item 0 (NO_ITEM) is a sentinel with no table entry; it is emitted as a
// zero-effect entry.
//
// Fail-closed: ROM bounds failure or profile misconfiguration → error returned.
ItemExtractResult extract_all_items(
    const RomData& rom,
    const ExtractionProfile& profile);

} // namespace crystal
