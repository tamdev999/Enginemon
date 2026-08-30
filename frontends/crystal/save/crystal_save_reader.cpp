// frontends/crystal/save/crystal_save_reader.cpp
//
// Crystal .sav import — validation + Phase-1 semantic decode.
//
// Validation order follows pokecrystal's own boot sequence:
//   1. Size ≥ 32 KB.
//   2. Check sentinel bytes on both copies.
//   3. Verify checksums on copies that passed sentinels.
//   4. Select authoritative copy (primary preferred).
//   5. Decode Phase-1 fields.
//   6. Bind SramIdentity.

#include "crystal_save_reader.hpp"
#include "crystal_bcd.hpp"
#include "crystal_sram_layout.hpp"

#include <array>
#include <cstring>
#include <stdexcept>
#include <string>

namespace crystal {

// ─── Checksum computation ────────────────────────────────────────────────────

static uint16_t compute_checksum(const uint8_t* data, uint32_t begin, uint32_t end) {
    uint16_t sum = 0;
    for (uint32_t i = begin; i <= end; ++i) {
        sum += data[i];
    }
    return sum;
}

// ─── Per-copy validity ───────────────────────────────────────────────────────

struct CopyValidity {
    bool sentinel_ok = false;
    bool checksum_ok = false;
    [[nodiscard]] bool fully_valid() const { return sentinel_ok && checksum_ok; }
};

static CopyValidity check_primary(const uint8_t* data) {
    using namespace sram_layout;
    CopyValidity v;

    v.sentinel_ok =
        (data[PRIMARY_CHECK_VALUE_1] == SAVE_CHECK_VALUE_1) &&
        (data[PRIMARY_CHECK_VALUE_2] == SAVE_CHECK_VALUE_2);

    if (v.sentinel_ok) {
        uint16_t stored   = static_cast<uint16_t>(
            data[PRIMARY_CHECKSUM] | (static_cast<uint32_t>(data[PRIMARY_CHECKSUM + 1]) << 8));
        uint16_t computed = compute_checksum(data, PRIMARY_GAME_DATA, PRIMARY_CHECKSUM_END);
        v.checksum_ok = (stored == computed);
    }

    return v;
}

static CopyValidity check_backup(const uint8_t* data) {
    using namespace sram_layout;
    CopyValidity v;

    v.sentinel_ok =
        (data[BACKUP_CHECK_VALUE_1] == SAVE_CHECK_VALUE_1) &&
        (data[BACKUP_CHECK_VALUE_2] == SAVE_CHECK_VALUE_2);

    if (v.sentinel_ok) {
        uint16_t stored   = static_cast<uint16_t>(
            data[BACKUP_CHECKSUM] | (static_cast<uint32_t>(data[BACKUP_CHECKSUM + 1]) << 8));
        uint16_t computed = compute_checksum(data, BACKUP_GAME_DATA, BACKUP_CHECKSUM_END);
        v.checksum_ok = (stored == computed);
    }

    return v;
}

// ─── Snapshot decode (Phase 1) ───────────────────────────────────────────────

// copy_offset_adj == 0 for primary, BACKUP_OFFSET for backup.
// primary_addr - copy_offset_adj gives the actual byte address in the buffer.
static CrystalSaveSnapshot decode_phase1(const uint8_t* data, uint32_t copy_offset_adj) {
    using namespace sram_layout;

    CrystalSaveSnapshot snap;

    // wMoney — 3-byte BCD big-endian
    {
        const uint8_t* m = &data[MONEY - copy_offset_adj];
        snap.money_player = bcd3_decode(m);
    }
    // wMomsMoney — 3-byte BCD big-endian
    {
        const uint8_t* m = &data[MOMS_MONEY - copy_offset_adj];
        snap.money_mom = bcd3_decode(m);
    }
    // wCoins — 2-byte big-endian (high byte first, confirmed from Crystal source)
    {
        uint32_t off = COINS - copy_offset_adj;
        snap.coins = static_cast<uint16_t>(
            (static_cast<uint32_t>(data[off]) << 8) | data[off + 1]);
    }
    // wEventFlags — 100 bytes verbatim
    {
        uint32_t off = EVENT_FLAGS - copy_offset_adj;
        std::copy(&data[off], &data[off + EVENT_FLAGS_SIZE], snap.event_flags.data());
    }

    return snap;
}

// ─── Public API ──────────────────────────────────────────────────────────────

CrystalImport import_save(
    const uint8_t* bytes,
    std::size_t    size,
    std::string    profile_sha1,
    std::string    rom_sha1)
{
    using namespace sram_layout;

    // 1. Size validation — before null check so MSVC empty-vector data() is caught.
    if (size < SRAM_SIZE) {
        throw SaveImportError(
            "import_save: file too small (" + std::to_string(size) +
            " bytes; expected at least " + std::to_string(SRAM_SIZE) + ")");
    }

    if (bytes == nullptr) {
        throw std::invalid_argument("import_save: bytes must not be null");
    }

    // 2. Validate both copies.
    CopyValidity primary_v = check_primary(bytes);
    CopyValidity backup_v  = check_backup(bytes);

    // 3. Select authoritative copy.
    bool use_primary;
    if (primary_v.fully_valid()) {
        use_primary = true;
    } else if (backup_v.fully_valid()) {
        use_primary = false;
    } else {
        std::string diag = "import_save: no valid save copy found. ";
        diag += "Primary: sentinel=" + std::string(primary_v.sentinel_ok ? "OK" : "FAIL");
        diag += " checksum=" + std::string(primary_v.checksum_ok ? "OK" : "FAIL");
        diag += ".  Backup: sentinel=" + std::string(backup_v.sentinel_ok ? "OK" : "FAIL");
        diag += " checksum=" + std::string(backup_v.checksum_ok ? "OK" : "FAIL") + ".";
        throw SaveImportError(diag);
    }

    // 4. Build the Sram shadow — verbatim copy of all 32 KB + trailer.
    CrystalImport result;
    std::copy(bytes, bytes + SRAM_SIZE, result.shadow.data.begin());
    if (size > SRAM_SIZE) {
        result.shadow.trailer.assign(bytes + SRAM_SIZE, bytes + size);
    }

    // 5. Decode Phase-1 fields from the selected copy.
    uint32_t adj = use_primary ? 0 : BACKUP_OFFSET;
    result.snapshot = decode_phase1(bytes, adj);

    // 6. Bind identity to shadow.
    result.shadow.identity.profile_sha1  = std::move(profile_sha1);
    result.shadow.identity.rom_sha1      = std::move(rom_sha1);
    result.shadow.identity.codec_version = SRAM_CODEC_VERSION;

    return result;
}

}  // namespace crystal
