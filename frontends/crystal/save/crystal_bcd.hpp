#pragma once
// frontends/crystal/save/crystal_bcd.hpp
//
// Encode/decode Crystal's 3-byte packed BCD money representation.
//
// Format: 3 bytes, big-endian BCD.
//   Byte[0]: hundreds-of-thousands and ten-thousands digits.
//   Byte[1]: thousands and hundreds digits.
//   Byte[2]: tens and units digits.
//
//   Value 999999 → bytes {0x99, 0x99, 0x99}.
//   Value 0      → bytes {0x00, 0x00, 0x00}.
//   Value 12345  → bytes {0x01, 0x23, 0x45}.
//
// Maximum representable value: 999 999.
// EngineFlags have no presence in Crystal SRAM — EngineFlags are Enginemon-only.

#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace crystal {

/// Maximum money value Crystal can represent in 3-byte BCD.
static constexpr uint32_t BCD3_MAX = 999'999;

/// Maximum coin value Crystal can represent in 2-byte big-endian.
static constexpr uint16_t COINS_MAX = 9'999;

// ── Decode ────────────────────────────────────────────────────────────────────

/// Decode 3 BCD bytes (big-endian) → integer.
/// Throws std::invalid_argument if any nibble > 9 (invalid BCD).
[[nodiscard]] inline uint32_t bcd3_decode(const uint8_t bytes[3]) {
    uint32_t result = 0;
    for (int i = 0; i < 3; ++i) {
        uint8_t hi = (bytes[i] >> 4) & 0x0F;
        uint8_t lo =  bytes[i]       & 0x0F;
        if (hi > 9 || lo > 9) {
            throw std::invalid_argument(
                "bcd3_decode: invalid BCD nibble in byte " + std::to_string(i));
        }
        result = result * 100 + hi * 10 + lo;
    }
    return result;
}

/// Decode 3 BCD bytes from a const array.
[[nodiscard]] inline uint32_t bcd3_decode(const std::array<uint8_t, 3>& bytes) {
    return bcd3_decode(bytes.data());
}

// ── Encode ────────────────────────────────────────────────────────────────────

/// Encode integer → 3 BCD bytes (big-endian).
/// Throws std::out_of_range if value > BCD3_MAX.
[[nodiscard]] inline std::array<uint8_t, 3> bcd3_encode(uint32_t value) {
    if (value > BCD3_MAX) {
        throw std::out_of_range(
            "bcd3_encode: value " + std::to_string(value) +
            " exceeds Crystal BCD3 maximum (" + std::to_string(BCD3_MAX) + ")");
    }
    std::array<uint8_t, 3> out{};
    for (int i = 2; i >= 0; --i) {
        uint32_t pair = value % 100;
        value /= 100;
        out[i] = static_cast<uint8_t>(((pair / 10) << 4) | (pair % 10));
    }
    return out;
}

}  // namespace crystal
