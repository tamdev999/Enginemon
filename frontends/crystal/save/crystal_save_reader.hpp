#pragma once
// frontends/crystal/save/crystal_save_reader.hpp
//
// crystal_save_reader — import a raw Crystal .sav file.
//
// Responsibilities:
//   1. Accept raw bytes (32 KB minimum, optional trailer).
//   2. Validate size.
//   3. Check sentinels on both save copies.
//   4. Verify checksums on surviving copies.
//   5. Choose which copy to decode (prefer primary when both valid).
//   6. Decode Phase-1 fields into CrystalSaveSnapshot.
//   7. Bind SramIdentity to the shadow.
//   8. Return CrystalImport{snapshot, shadow}.
//
// Failures are explicit exceptions — never silent defaults.

#include "crystal_sram.hpp"
#include "crystal_save_snapshot.hpp"
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace crystal {

/// Holds the semantic decode and the verbatim 32 KB shadow.
struct CrystalImport {
    CrystalSaveSnapshot snapshot;
    Sram                shadow;    // verbatim 32 KB + emulator trailer + identity
};

/// Thrown when the raw bytes are not a valid Crystal save image.
struct SaveImportError : std::runtime_error {
    explicit SaveImportError(std::string msg)
        : std::runtime_error(std::move(msg)) {}
};

/// Import a raw .sav byte sequence.
///
/// @param bytes          Pointer to the raw data (at least SRAM_SIZE bytes required).
/// @param size           Total length in bytes.  May exceed SRAM_SIZE (emulator trailer).
/// @param profile_sha1   SHA-1 of the ExtractionProfile used for this save.
///                       Bound into shadow.identity.  Pass empty string for
///                       synthetic/test saves that have no associated profile.
/// @param rom_sha1       SHA-1 of the source ROM bytes, if known.  Informational only.
///
/// @throws SaveImportError on any validation failure.
/// @throws std::invalid_argument if bytes is null.
[[nodiscard]] CrystalImport import_save(
    const uint8_t* bytes,
    std::size_t    size,
    std::string    profile_sha1 = "",
    std::string    rom_sha1     = "");

/// Convenience overload for std::vector (no identity).
[[nodiscard]] inline CrystalImport import_save(const std::vector<uint8_t>& bytes) {
    return import_save(bytes.data(), bytes.size(), "", "");
}

/// Convenience overload for std::vector with identity.
[[nodiscard]] inline CrystalImport import_save(
    const std::vector<uint8_t>& bytes,
    std::string                 profile_sha1,
    std::string                 rom_sha1 = "")
{
    return import_save(bytes.data(), bytes.size(),
                       std::move(profile_sha1), std::move(rom_sha1));
}

}  // namespace crystal
