#pragma once
// frontends/crystal/save/crystal_sram.hpp
//
// Sram — raw 32 KB SRAM image plus any emulator trailer bytes.
//
// The data array is an exact verbatim copy of the SRAM chip contents.
// Unknown bytes are never interpreted or modified except through the
// explicit codec patch steps.
//
// SramIdentity binds an imported shadow to the profile/ROM it came from.
// export_save() validates this identity before patching any bytes, so a
// shadow imported under one ROM layout can never be silently patched with
// offsets from a different layout.

#include "crystal_sram_layout.hpp"
#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace crystal {

// ─── SRAM identity ───────────────────────────────────────────────────────────

/// The profile/ROM identity that was in effect when this SRAM image was imported.
///
/// Bound at import time and checked at export time.  Prevents a shadow imported
/// under one ROM/profile layout from being silently patched with offsets from
/// a different layout.
///
/// - `profile_sha1`:    The ExtractionProfile::sha1 that describes this save's
///                      SRAM layout (e.g. "f2f52230..." for Crystal v1.1).
/// - `rom_sha1`:        SHA-1 of the actual source ROM bytes, if known.  May be
///                      empty for synthetic/test saves that have no source ROM.
/// - `codec_version`:   Save codec version string (e.g. "crystal-save-1.0").
///                      Bumped when the Phase-1 owned field set or encoding changes.
///
/// An empty `profile_sha1` means "no identity bound" — the shadow was created
/// synthetically (e.g. in tests or from a blank template) and no identity check
/// is performed on export.

struct SramIdentity {
    std::string profile_sha1;   // ExtractionProfile::sha1 at import time
    std::string rom_sha1;       // actual source ROM hash (may be empty)
    std::string codec_version;  // e.g. "crystal-save-1.0"

    bool empty() const { return profile_sha1.empty(); }

    bool operator==(const SramIdentity& o) const {
        return profile_sha1   == o.profile_sha1
            && codec_version  == o.codec_version;
        // rom_sha1 is informational; two saves from compatible ROM hacks
        // sharing the same profile_sha1 are treated as compatible.
    }
    bool operator!=(const SramIdentity& o) const { return !(*this == o); }
};

/// Codec version string embedded in every imported shadow.
static constexpr const char* SRAM_CODEC_VERSION = "crystal-save-1.0";

// ─── Sram buffer ─────────────────────────────────────────────────────────────

struct Sram {
    /// The exact 32 KB SRAM image.
    std::array<uint8_t, sram_layout::SRAM_SIZE> data{};

    /// Emulator trailer (RTC bytes, etc.) — may be empty for cart saves.
    std::vector<uint8_t> trailer;

    /// Identity bound at import time.  Empty for synthetic/test saves.
    /// Checked against the expected identity before export.
    SramIdentity identity;

    // ── Bounded read helpers ──────────────────────────────────────────────────

    [[nodiscard]] uint8_t read_u8(uint32_t offset) const {
        if (offset >= sram_layout::SRAM_SIZE)
            throw std::out_of_range("Sram::read_u8 offset out of range");
        return data[offset];
    }

    [[nodiscard]] uint16_t read_u16_be(uint32_t offset) const {
        if (offset + 1 >= sram_layout::SRAM_SIZE)
            throw std::out_of_range("Sram::read_u16_be offset out of range");
        return static_cast<uint16_t>(
            (static_cast<uint32_t>(data[offset]) << 8) |
             static_cast<uint32_t>(data[offset + 1]));
    }

    [[nodiscard]] uint16_t read_u16_le(uint32_t offset) const {
        if (offset + 1 >= sram_layout::SRAM_SIZE)
            throw std::out_of_range("Sram::read_u16_le offset out of range");
        return static_cast<uint16_t>(
            static_cast<uint32_t>(data[offset]) |
            (static_cast<uint32_t>(data[offset + 1]) << 8));
    }

    // ── Bounded write helpers ─────────────────────────────────────────────────

    void write_u8(uint32_t offset, uint8_t value) {
        if (offset >= sram_layout::SRAM_SIZE)
            throw std::out_of_range("Sram::write_u8 offset out of range");
        data[offset] = value;
    }

    void write_u16_be(uint32_t offset, uint16_t value) {
        if (offset + 1 >= sram_layout::SRAM_SIZE)
            throw std::out_of_range("Sram::write_u16_be offset out of range");
        data[offset]     = static_cast<uint8_t>(value >> 8);
        data[offset + 1] = static_cast<uint8_t>(value & 0xFF);
    }

    void write_u16_le(uint32_t offset, uint16_t value) {
        if (offset + 1 >= sram_layout::SRAM_SIZE)
            throw std::out_of_range("Sram::write_u16_le offset out of range");
        data[offset]     = static_cast<uint8_t>(value & 0xFF);
        data[offset + 1] = static_cast<uint8_t>(value >> 8);
    }

    void write_bytes(uint32_t offset, const uint8_t* src, uint32_t count) {
        if (count == 0) return;
        if (offset + count > sram_layout::SRAM_SIZE)
            throw std::out_of_range("Sram::write_bytes range out of SRAM");
        std::copy(src, src + count, data.begin() + offset);
    }
};

}  // namespace crystal
