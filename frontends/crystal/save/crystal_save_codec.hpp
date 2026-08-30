#pragma once
// frontends/crystal/save/crystal_save_codec.hpp
//
// Top-level Crystal .sav codec API.
//
// This header is the single include point for callers outside the save module.
// It re-exports the types and functions they need without exposing the
// internal layout or BCD helpers.

#include "crystal_save_reader.hpp"
#include "crystal_save_writer.hpp"
#include "crystal_save_snapshot.hpp"
#include "crystal_save_errors.hpp"
#include "crystal_sram.hpp"

// Usage pattern:
//
//   // Import
//   std::vector<uint8_t> raw = read_file("game.sav");
//   crystal::CrystalImport imp = crystal::import_save(raw);
//   // imp.snapshot  — semantic decoded fields
//   // imp.shadow    — verbatim 32 KB + trailer
//
//   // Edit imp.snapshot fields (money, coins, event flags) …
//
//   // Export
//   std::vector<uint8_t> out = crystal::export_save(imp.snapshot, imp.shadow);
//   write_file("game.sav", out);
