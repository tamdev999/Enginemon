// frontends/crystal/save/crystal_save_writer.cpp
//
// Crystal .sav export — patch shadow, rebuild copies/checksums, self-validate, emit.

#include "crystal_save_writer.hpp"
#include "crystal_bcd.hpp"
#include "crystal_save_reader.hpp"
#include "crystal_sram_layout.hpp"
#include "crystal_party_codec.hpp"

#include <array>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace crystal {

namespace {

static uint16_t compute_checksum(const uint8_t* data, uint32_t begin, uint32_t end) {
    uint16_t sum = 0;
    for (uint32_t i = begin; i <= end; ++i) {
        sum += data[i];
    }
    return sum;
}

static bool sentinel_ok(const uint8_t* data, uint32_t cv1_off, uint32_t cv2_off) {
    using namespace sram_layout;
    return data[cv1_off] == SAVE_CHECK_VALUE_1 &&
           data[cv2_off] == SAVE_CHECK_VALUE_2;
}

static bool checksum_ok(const uint8_t* data,
                        uint32_t begin, uint32_t end,
                        uint32_t checksum_off) {
    uint16_t stored   = static_cast<uint16_t>(
        data[checksum_off] | (static_cast<uint32_t>(data[checksum_off + 1]) << 8));
    uint16_t computed = compute_checksum(data, begin, end);
    return stored == computed;
}

}  // namespace

std::vector<uint8_t> export_save(
    const CrystalSaveSnapshot& snapshot,
    const Sram&                shadow,
    const SramIdentity&        expected_identity)
{
    using namespace sram_layout;

    // ── Step 0: Identity check. ───────────────────────────────────────────────
    // Both fields must agree: profile_sha1, codec_version, and rom_sha1 (when
    // both sides carry a non-empty ROM SHA).  The default policy is SHA-driven:
    // one ROM SHA → one profile.  A shadow imported under a different ROM may
    // not be patched by this export.  If cross-ROM compatibility is ever needed
    // it must be modelled explicitly rather than silently ignoring rom_sha1.
    if (!expected_identity.empty() && !shadow.identity.empty()) {
        if (shadow.identity != expected_identity) {
            std::string msg = "export_save: SRAM identity mismatch — "
                              "shadow was imported under a different layout.\n"
                              "  shadow profile_sha1   = '" + shadow.identity.profile_sha1 + "'\n"
                              "  expected profile_sha1 = '" + expected_identity.profile_sha1 + "'\n"
                              "  shadow rom_sha1       = '" + shadow.identity.rom_sha1 + "'\n"
                              "  expected rom_sha1     = '" + expected_identity.rom_sha1 + "'\n"
                              "  shadow codec_version  = '" + shadow.identity.codec_version + "'\n"
                              "  expected codec_version= '" + expected_identity.codec_version + "'";
            throw SaveExportError(msg);
        }
    }

    // ── Step 1: Validate representability. ───────────────────────────────────
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
    std::array<uint8_t, SRAM_SIZE> buf;
    std::copy(shadow.data.begin(), shadow.data.end(), buf.begin());
    uint8_t* data = buf.data();

    // ── Step 3: Patch owned fields into primary region. ───────────────────────

    // ── Phase 1: Money, coins, event flags ───────────────────────────────────

    // wMoney (3 bytes BCD big-endian)
    {
        auto enc = bcd3_encode(snapshot.money_player);
        std::copy(enc.begin(), enc.end(), &data[MONEY]);
    }
    // wMomsMoney (3 bytes BCD big-endian)
    {
        auto enc = bcd3_encode(snapshot.money_mom);
        std::copy(enc.begin(), enc.end(), &data[MOMS_MONEY]);
    }
    // wCoins (2 bytes big-endian)
    {
        data[COINS]     = static_cast<uint8_t>(snapshot.coins >> 8);
        data[COINS + 1] = static_cast<uint8_t>(snapshot.coins & 0xFF);
    }
    // wEventFlags (100 bytes verbatim)
    {
        static_assert(snapshot.event_flags.size() == EVENT_FLAGS_SIZE,
                      "event_flags size mismatch");
        std::copy(snapshot.event_flags.begin(), snapshot.event_flags.end(),
                  &data[EVENT_FLAGS]);
    }

    // ── Phase 2: Player identity ─────────────────────────────────────────────

    // wPlayerID (2 bytes LE)
    {
        data[PLAYER_ID]     = static_cast<uint8_t>(snapshot.player_id & 0xFF);
        data[PLAYER_ID + 1] = static_cast<uint8_t>(snapshot.player_id >> 8);
    }
    // wSecretID (2 bytes LE)
    {
        data[SECRET_ID]     = static_cast<uint8_t>(snapshot.secret_id & 0xFF);
        data[SECRET_ID + 1] = static_cast<uint8_t>(snapshot.secret_id >> 8);
    }
    // Names (Crystal charmap, 0x50-terminated, fixed length)
    encode_crystal_string_to(snapshot.player_name, &data[PLAYER_NAME], PLAYER_NAME_SIZE);
    encode_crystal_string_to(snapshot.moms_name,   &data[MOMS_NAME],   MOMS_NAME_SIZE);
    encode_crystal_string_to(snapshot.rival_name,  &data[RIVAL_NAME],  RIVAL_NAME_SIZE);

    // wPlayerGender in sOptions block (bit 0 of byte 0).
    // sOptions is at PRIMARY_OPTIONS (0x2000), outside the checksummed region.
    // Keep all other bits of sOptions byte 0 intact.
    {
        uint8_t existing = data[PRIMARY_OPTIONS];
        data[PRIMARY_OPTIONS] = (existing & 0xFE) | (snapshot.player_gender & 0x01);
    }

    // ── Phase 2: Map position ────────────────────────────────────────────────

    data[WARP_NUMBER] = snapshot.location.warp;
    data[MAP_GROUP]   = snapshot.location.group;
    data[MAP_NUMBER]  = snapshot.location.number;
    data[PLAYER_Y]    = snapshot.location.y;
    data[PLAYER_X]    = snapshot.location.x;

    // ── Phase 2: Scene state ─────────────────────────────────────────────────

    {
        static_assert(snapshot.scene_ids.size() == SCENE_IDS_COUNT,
                      "scene_ids size mismatch");
        std::copy(snapshot.scene_ids.begin(), snapshot.scene_ids.end(),
                  &data[SCENE_IDS_BASE]);
    }

    // ── Phase 2: Play time ───────────────────────────────────────────────────

    // wGameTimeCap: preserve other bits, set/clear bit 7
    {
        // wGameTimeCap: bit 0 = capped flag (GAME_TIME_CAPPED EQU 0, ram_constants.asm)
        uint8_t cap = data[GAME_TIME_CAP] & 0xFE;
        if (snapshot.playtime_capped) cap |= 0x01;
        data[GAME_TIME_CAP] = cap;
    }
    // wGameTimeHours: 2 bytes BIG-ENDIAN
    {
        data[GAME_TIME_HOURS]     = static_cast<uint8_t>(snapshot.playtime_hours >> 8);
        data[GAME_TIME_HOURS + 1] = static_cast<uint8_t>(snapshot.playtime_hours & 0xFF);
    }
    data[GAME_TIME_MINUTES] = snapshot.playtime_minutes;
    data[GAME_TIME_SECONDS] = snapshot.playtime_seconds;

    // ── Phase 2: RTC / time context ─────────────────────────────────────────

    {
        uint32_t off = RTC_BYTES;
        data[off + 0] = snapshot.rtc.day;
        data[off + 1] = snapshot.rtc.hour;
        data[off + 2] = snapshot.rtc.minute;
        data[off + 3] = snapshot.rtc.second;
    }
    {
        uint8_t dst = data[DST] & 0x7F;
        if (snapshot.rtc.dst) dst |= 0x80;
        data[DST] = dst;
    }

    // ── Phase 3A: Party ──────────────────────────────────────────────────────
    encode_party(snapshot.party, data, PartyCodecDomain{});

    // ── Step 4: Copy primary region → backup region (byte-for-byte). ─────────
    std::copy(&data[PRIMARY_GAME_DATA],
              &data[PRIMARY_GAME_DATA] + CHECKSUM_REGION_SIZE,
              &data[BACKUP_GAME_DATA]);

    // Mirror the options block (not checksummed, but Crystal copies it).
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
    if (!sentinel_ok(data, PRIMARY_CHECK_VALUE_1, PRIMARY_CHECK_VALUE_2))
        throw std::logic_error("export_save: primary sentinels not set correctly (bug)");
    if (!sentinel_ok(data, BACKUP_CHECK_VALUE_1, BACKUP_CHECK_VALUE_2))
        throw std::logic_error("export_save: backup sentinels not set correctly (bug)");
    if (!checksum_ok(data, PRIMARY_GAME_DATA, PRIMARY_CHECKSUM_END, PRIMARY_CHECKSUM))
        throw std::logic_error("export_save: primary checksum self-validation failed (bug)");
    if (!checksum_ok(data, BACKUP_GAME_DATA, BACKUP_CHECKSUM_END, BACKUP_CHECKSUM))
        throw std::logic_error("export_save: backup checksum self-validation failed (bug)");

    // ── Step 8: Emit 32 KB + trailer. ─────────────────────────────────────────
    std::vector<uint8_t> out;
    out.reserve(SRAM_SIZE + shadow.trailer.size());
    out.insert(out.end(), buf.begin(), buf.end());
    out.insert(out.end(), shadow.trailer.begin(), shadow.trailer.end());

    return out;
}

}  // namespace crystal
