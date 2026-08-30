#pragma once
// frontends/crystal/save/crystal_party_codec.hpp
//
// Low-level decode/encode of the Crystal party region in an SRAM image.
//
// Called by crystal_save_reader.cpp (decode) and crystal_save_writer.cpp (encode).
// All SRAM layout knowledge is in crystal_sram_layout.hpp; this file owns
// only the field-level bit manipulation.

#include "crystal_save_snapshot.hpp"
#include "crystal_save_errors.hpp"
#include <cstdint>
#include <string>

namespace crystal {

// ─── Profile-domain limits needed for validation ────────────────────────────
// Passed in from the import call so the codec never hardcodes species/move/item
// counts — they come from the ExtractionProfile selected for this ROM.

struct PartyCodecDomain {
    uint16_t num_pokemon = 251;  // max valid species ID  (Crystal v1.1: 251 + EGG=254)
    uint16_t num_moves   = 251;  // max valid move ID
    uint16_t num_items   = 256;  // item IDs 0–255
};

// ─── Decode (SRAM → snapshot) ────────────────────────────────────────────────

/// Decode the party region of an SRAM image.
/// @param data        raw 32 KB SRAM bytes
/// @param adj         0 for primary copy, BACKUP_OFFSET for backup copy
/// @param domain      species/move/item validity ranges from the active profile
/// @throws SaveImportError on malformed party data
[[nodiscard]] CrystalParty decode_party(
    const uint8_t*       data,
    uint32_t             adj,
    const PartyCodecDomain& domain);

// ─── Encode (snapshot → SRAM) ────────────────────────────────────────────────

/// Encode a CrystalParty back into the primary party region of the working buffer.
/// Caller is responsible for re-mirroring primary→backup and recomputing checksums.
/// @param party       semantic party to encode
/// @param data        mutable 32 KB working buffer (primary region patched in-place)
/// @param domain      used to validate representability before writing any bytes
/// @throws SaveExportError on any unrepresentable field
void encode_party(
    const CrystalParty&  party,
    uint8_t*             data,
    const PartyCodecDomain& domain);

}  // namespace crystal
