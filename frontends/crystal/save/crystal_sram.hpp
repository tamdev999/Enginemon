#pragma once
// frontends/crystal/save/crystal_sram.hpp
//
// Sram — raw 32 KB SRAM image plus any emulator trailer bytes.
//
// The data array is an exact verbatim copy of the SRAM chip contents.
// Unknown bytes are never interpreted or modified except through the
// explicit codec patch steps.
//
// The trailer holds emulator-specific RTC state (typically 44–48 bytes from
// mGBA, BGB, etc.) and is emitted unchanged at the end of every export.

#include "crystal_sram_layout.hpp"
#include <array>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace crystal {

struct Sram {
    /// The exact 32 KB SRAM image.
    std::array<uint8_t, sram_layout::SRAM_SIZE> data{};

    /// Emulator trailer (RTC bytes, etc.) — may be empty for cart saves.
    std::vector<uint8_t> trailer;

    // ── Bounded read helpers ──────────────────────────────────────────────────
    // All reads go through these helpers.  They enforce the [0, SRAM_SIZE)
    // boundary at call-time so callers cannot silently go out of range.

    [[nodiscard]] uint8_t read_u8(uint32_t offset) const {
        if (offset >= sram_layout::SRAM_SIZE)
            throw std::out_of_range("Sram::read_u8 offset out of range");
        return data[offset];
    }

    /// Read 2-byte big-endian unsigned value.
    [[nodiscard]] uint16_t read_u16_be(uint32_t offset) const {
        if (offset + 1 >= sram_layout::SRAM_SIZE)
            throw std::out_of_range("Sram::read_u16_be offset out of range");
        return static_cast<uint16_t>(
            (static_cast<uint32_t>(data[offset]) << 8) |
             static_cast<uint32_t>(data[offset + 1]));
    }

    /// Read 2-byte little-endian unsigned value.
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

    /// Write `count` bytes from `src` starting at `offset`.
    void write_bytes(uint32_t offset, const uint8_t* src, uint32_t count) {
        if (count == 0) return;
        if (offset + count > sram_layout::SRAM_SIZE)
            throw std::out_of_range("Sram::write_bytes range out of SRAM");
        std::copy(src, src + count, data.begin() + offset);
    }
};

}  // namespace crystal
