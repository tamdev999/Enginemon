// frontends/crystal/save/crystal_save_writer.cpp
//
// Crystal .sav export — patch shadow, rebuild copies/checksums, self-validate, emit.

#include "crystal_save_writer.hpp"
#include "crystal_bcd.hpp"
#include "crystal_save_reader.hpp"   // SaveImportError (re-used for self-validation)
#include "crystal_sram_layout.hpp"

#include <array>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace crystal {

namespace {

// ─── Checksum computation (shared with reader) ───────────────────────────────

static uint16_t compute_checksum(const uint8_t* data, uint32_t begin, uint32_t end) {
    uint16_t sum = 0;
    for (uint32_t i = begin; i <= end; ++i) {
        sum += data[i];
    }
    return sum;
}

// ─── Validation helpers used in self-check step ──────────────────────────────

static bool sentinel_ok(const uint8_t* data, uint32_t cv1_off, uint32_t cv2_off) {
    using namespace sram_layout;
    return data[cv1_off] == SAVE_CHECK_VALUE_1 &&
           data[cv2_off] == SAVE_CHECK_VALUE_2;
}

static bool checksum_ok(const uint8_t* data,
                        uint32_t begin, uint32_t end,
                        uint32_t checksum_off) {
    uint16_t stored = static_cast<uint16_t>(
        data[checksum_off] | (static_cast<uint32_t>(data[checksum_off + 1]) << 8));
    uint16_t computed = compute_checksum(data, begin, end);
    return stored == computed;
}

}  // namespace

// ─── Public export ───────────────────────────────────────────────────────────

std::vector<uint8_t> export_save(
    const CrystalSaveSnapshot& snapshot,
    const Sram&                shadow)
{
    using namespace sram_layout;

    // ── Step 1: Validate representability before touching anything. ───────────

    if (snapshot.money_player > BCD3_MAX) {
        throw SaveExportError(
            "export_save: money_player " + std::to_string(snapshot.money_player) +
            " exceeds Crystal maximum of " + std::to_string(BCD3_MAX));
    }
    if (snapshot.money_mom > BCD3_MAX) {
        throw SaveExportError(
            "export_save: money_mom " + std::to_string(snapshot.money_mom) +
            " exceeds Crystal maximum of " + std::to_string(BCD3_MAX));
    }
    if (snapshot.coins > COINS_MAX) {
        throw SaveExportError(
            "export_save: coins " + std::to_string(snapshot.coins) +
            " exceeds Crystal maximum of " + std::to_string(COINS_MAX));
    }

    // ── Step 2: Copy shadow into working buffer. ──────────────────────────────
    // We work on a local copy of the 32 KB data — the shadow is never mutated.
    std::array<uint8_t, SRAM_SIZE> buf;
    std::copy(shadow.data.begin(), shadow.data.end(), buf.begin());
    uint8_t* data = buf.data();

    // ── Step 3: Patch owned fields into primary region. ───────────────────────

    // wMoney (3 bytes BCD)
    {
        auto enc = bcd3_encode(snapshot.money_player);
        std::copy(enc.begin(), enc.end(), &data[MONEY]);
    }
    // wMomsMoney (3 bytes BCD)
    {
        auto enc = bcd3_encode(snapshot.money_mom);
        std::copy(enc.begin(), enc.end(), &data[MOMS_MONEY]);
    }
    // wCoins (2 bytes big-endian)
    {
        data[COINS]     = static_cast<uint8_t>(snapshot.coins >> 8);
        data[COINS + 1] = static_cast<uint8_t>(snapshot.coins & 0xFF);
    }
    // wEventFlags (100 bytes bitfield)
    {
        static_assert(snapshot.event_flags.size() == EVENT_FLAGS_SIZE,
                      "event_flags size mismatch");
        std::copy(snapshot.event_flags.begin(), snapshot.event_flags.end(),
                  &data[EVENT_FLAGS]);
    }

    // ── Step 4: Copy primary region → backup region (byte-for-byte). ─────────
    //
    //   data[BACKUP_GAME_DATA .. BACKUP_CHECKSUM_END] =
    //       data[PRIMARY_GAME_DATA .. PRIMARY_CHECKSUM_END]
    //
    // CHECKSUM_REGION_SIZE == BACKUP_CHECKSUM_END - BACKUP_GAME_DATA + 1 ✓
    std::copy(&data[PRIMARY_GAME_DATA],
              &data[PRIMARY_GAME_DATA] + CHECKSUM_REGION_SIZE,
              &data[BACKUP_GAME_DATA]);

    // Also mirror the options block (not checksummed but Crystal copies it).
    std::copy(&data[PRIMARY_OPTIONS],
              &data[PRIMARY_OPTIONS] + PRIMARY_OPTIONS_SIZE,
              &data[BACKUP_OPTIONS]);

    // ── Step 5: Write all four sentinel bytes. ────────────────────────────────
    data[PRIMARY_CHECK_VALUE_1] = SAVE_CHECK_VALUE_1;
    data[PRIMARY_CHECK_VALUE_2] = SAVE_CHECK_VALUE_2;
    data[BACKUP_CHECK_VALUE_1]  = SAVE_CHECK_VALUE_1;
    data[BACKUP_CHECK_VALUE_2]  = SAVE_CHECK_VALUE_2;

    // ── Step 6: Recompute both checksums. ─────────────────────────────────────
    {
        uint16_t cs = compute_checksum(data, PRIMARY_GAME_DATA, PRIMARY_CHECKSUM_END);
        data[PRIMARY_CHECKSUM]     = static_cast<uint8_t>(cs & 0xFF);
        data[PRIMARY_CHECKSUM + 1] = static_cast<uint8_t>(cs >> 8);
    }
    {
        uint16_t cs = compute_checksum(data, BACKUP_GAME_DATA, BACKUP_CHECKSUM_END);
        data[BACKUP_CHECKSUM]     = static_cast<uint8_t>(cs & 0xFF);
        data[BACKUP_CHECKSUM + 1] = static_cast<uint8_t>(cs >> 8);
    }

    // ── Step 7: Self-validate. ────────────────────────────────────────────────
    // A checksum error here is an internal bug — the export code is wrong.
    if (!sentinel_ok(data, PRIMARY_CHECK_VALUE_1, PRIMARY_CHECK_VALUE_2)) {
        throw std::logic_error("export_save: primary sentinels not set correctly (bug)");
    }
    if (!sentinel_ok(data, BACKUP_CHECK_VALUE_1, BACKUP_CHECK_VALUE_2)) {
        throw std::logic_error("export_save: backup sentinels not set correctly (bug)");
    }
    if (!checksum_ok(data, PRIMARY_GAME_DATA, PRIMARY_CHECKSUM_END, PRIMARY_CHECKSUM)) {
        throw std::logic_error("export_save: primary checksum self-validation failed (bug)");
    }
    if (!checksum_ok(data, BACKUP_GAME_DATA, BACKUP_CHECKSUM_END, BACKUP_CHECKSUM)) {
        throw std::logic_error("export_save: backup checksum self-validation failed (bug)");
    }

    // ── Step 8: Emit 32 KB + trailer. ─────────────────────────────────────────
    std::vector<uint8_t> out;
    out.reserve(SRAM_SIZE + shadow.trailer.size());
    out.insert(out.end(), buf.begin(), buf.end());
    out.insert(out.end(), shadow.trailer.begin(), shadow.trailer.end());

    return out;
}

}  // namespace crystal
