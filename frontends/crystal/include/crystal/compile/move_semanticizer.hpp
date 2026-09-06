#pragma once
// crystal/compile/move_semanticizer.hpp
//
// Thin interface for the move semanticization step in Stage 9 of the
// full-game compiler.
//
// Declared separately from full_compiler.cpp to avoid blowing MSVC's TU
// heap limit — effect_script_decoder.hpp and effect_semanticizer.hpp are
// heavy template headers; keeping them in their own .cpp prevents C1060.
//
// CONTRACT
//   - Decodes all Crystal effect scripts from ROM using EffectScriptDecoder.
//   - Calls EffectSemanticizer::semanticize() for each move entry.
//   - Stores the resulting SemanticEffectDescription in entry.effect_desc.
//   - Returns false (with error on stderr) if the corpus cannot be decoded.
//   - Moves with out-of-range effect IDs get a zero-init (is_supported=false)
//     description — fails closed, never silently defaults.

#include "crystal/output/native_package.hpp"  // PackageWriter::MoveDataEntry
#include "crystal/rom/loader.hpp"
#include "crystal/rom/profile.hpp"
#include <vector>

namespace crystal {

// Decode all effect scripts and populate effect_desc for each move entry.
// Returns true on success, false on fatal decode failure.
bool semanticize_move_entries(
    const RomData&                            rom,
    const ExtractionProfile&                  profile,
    std::vector<PackageWriter::MoveDataEntry>& entries);

} // namespace crystal
