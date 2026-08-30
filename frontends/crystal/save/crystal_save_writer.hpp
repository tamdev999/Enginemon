#pragma once
// frontends/crystal/save/crystal_save_writer.hpp
//
// crystal_save_writer — export a Crystal .sav from a snapshot + SRAM shadow.
//
// Export path:
//   0. Validate shadow identity against expected_identity (if both non-empty).
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
// Identity mismatch is rejected with SaveExportError.

#include "crystal_save_snapshot.hpp"
#include "crystal_sram.hpp"
#include "crystal_save_errors.hpp"
#include <string>
#include <vector>

namespace crystal {

/// Thrown when export cannot proceed — see crystal_save_errors.hpp.
// (defined in crystal_save_errors.hpp)

/// Export a .sav byte sequence.
///
/// @param snapshot           Semantic fields to patch into the shadow.
/// @param shadow             The verbatim 32 KB image (from import, or blank).
/// @param expected_identity  If non-empty, shadow.identity must match this before
///                           any patching occurs.  Pass {} to skip the check
///                           (e.g. for blank-template exports or tests).
///
/// @returns  Exactly SRAM_SIZE bytes (32 KB) followed by the trailer.
///
/// @throws SaveExportError   on unrepresentable field value or identity mismatch.
/// @throws std::logic_error  on internal self-validation failure (codec bug).
[[nodiscard]] std::vector<uint8_t> export_save(
    const CrystalSaveSnapshot& snapshot,
    const Sram&                shadow,
    const SramIdentity&        expected_identity = {});

}  // namespace crystal
