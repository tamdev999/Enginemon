#pragma once
// frontends/crystal/save/crystal_save_writer.hpp
//
// crystal_save_writer — export a Crystal .sav from a snapshot + SRAM shadow.
//
// Export path:
//   1. Start from the SRAM shadow (preserves all unowned bytes verbatim).
//   2. Patch owned fields from the snapshot.
//   3. Copy primary region → backup region.
//   4. Write all four sentinel bytes.
//   5. Recompute both checksums from scratch.
//   6. Self-validate: re-read sentinels + checksums.
//   7. Emit 32 KB + trailer.
//
// Owned fields are only money, mom-money, coins, and event flags.
// All other SRAM bytes are carried unchanged from the shadow.
//
// Values that Crystal cannot represent are rejected with SaveExportError.

#include "crystal_save_snapshot.hpp"
#include "crystal_sram.hpp"
#include <stdexcept>
#include <string>
#include <vector>

namespace crystal {

/// Thrown when the snapshot contains values Crystal cannot represent.
struct SaveExportError : std::runtime_error {
    explicit SaveExportError(std::string msg)
        : std::runtime_error(std::move(msg)) {}
};

/// Export a .sav byte sequence.
///
/// @param snapshot  Semantic fields to patch into the shadow.
/// @param shadow    The verbatim 32 KB image (from import, or blank template).
/// @returns         Exactly SRAM_SIZE bytes (32 KB) followed by the trailer.
///
/// @throws SaveExportError on any unrepresentable field value.
/// @throws std::out_of_range if an internal patch address is out of bounds (bug).
[[nodiscard]] std::vector<uint8_t> export_save(
    const CrystalSaveSnapshot& snapshot,
    const Sram&                shadow);

}  // namespace crystal
