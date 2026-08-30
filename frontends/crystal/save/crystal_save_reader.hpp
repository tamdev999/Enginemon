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
//   7. Return CrystalImport{snapshot, shadow}.
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
    Sram                shadow;    // verbatim 32 KB + emulator trailer
};

/// Thrown when the raw bytes are not a valid Crystal save image.
struct SaveImportError : std::runtime_error {
    explicit SaveImportError(std::string msg)
        : std::runtime_error(std::move(msg)) {}
};

/// Import a raw .sav byte sequence.
///
/// @param bytes  Pointer to the raw data (at least SRAM_SIZE bytes required).
/// @param size   Total length in bytes.  May exceed SRAM_SIZE (emulator trailer).
///
/// @throws SaveImportError on any validation failure.
/// @throws std::invalid_argument if bytes is null.
[[nodiscard]] CrystalImport import_save(const uint8_t* bytes, std::size_t size);

/// Convenience overload for std::vector.
[[nodiscard]] inline CrystalImport import_save(const std::vector<uint8_t>& bytes) {
    return import_save(bytes.data(), bytes.size());
}

}  // namespace crystal
