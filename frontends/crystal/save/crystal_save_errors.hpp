#pragma once
// frontends/crystal/save/crystal_save_errors.hpp
// Shared exception types for the Crystal save codec.
// Both reader and writer use these — having them in a common header breaks
// the circular include dependency between crystal_save_reader.hpp and
// crystal_save_writer.hpp.

#include <stdexcept>
#include <string>

namespace crystal {

/// Thrown when the raw bytes are not a valid Crystal save image.
struct SaveImportError : std::runtime_error {
    explicit SaveImportError(std::string msg)
        : std::runtime_error(std::move(msg)) {}
};

/// Thrown when export cannot proceed:
///   - snapshot contains values Crystal cannot represent
///   - shadow identity does not match expected identity
///   - character not representable in Crystal charmap
struct SaveExportError : std::runtime_error {
    explicit SaveExportError(std::string msg)
        : std::runtime_error(std::move(msg)) {}
};

}  // namespace crystal
