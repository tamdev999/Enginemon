// frontends/crystal/save/crystal_save_reader.cpp
//
// Crystal .sav import — validation + Phase-1+2 semantic decode.

#include "crystal_save_reader.hpp"
#include "crystal_bcd.hpp"
#include "crystal_save_errors.hpp"
#include "crystal_sram_layout.hpp"
#include "crystal_party_codec.hpp"

#include <array>
#include <cstring>
#include <stdexcept>
#include <string>

namespace crystal {

// ─── Checksum computation ────────────────────────────────────────────────────

static uint16_t compute_checksum(const uint8_t* data, uint32_t begin, uint32_t end) {
    uint16_t sum = 0;
    for (uint32_t i = begin; i <= end; ++i) sum += data[i];
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
        v.checksum_ok = (stored == compute_checksum(data, PRIMARY_GAME_DATA, PRIMARY_CHECKSUM_END));
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
        v.checksum_ok = (stored == compute_checksum(data, BACKUP_GAME_DATA, BACKUP_CHECKSUM_END));
    }
    return v;
}

// ─── Crystal charmap decoder ─────────────────────────────────────────────────
// Decodes a null (0x50) terminated Crystal charmap string to UTF-8.
// Stops at the 0x50 terminator or at max_bytes.

static std::string decode_crystal_string(const uint8_t* bytes, uint32_t max_bytes) {
    std::string result;
    for (uint32_t i = 0; i < max_bytes; ++i) {
        uint8_t c = bytes[i];
        if (c == 0x50) break;  // string terminator '@'

        // Map Crystal charmap codes to UTF-8 (from fonts.cpp charmap table)
        // Uppercase A-Z: 0x80–0x99
        if (c >= 0x80 && c <= 0x99) { result += static_cast<char>('A' + (c - 0x80)); continue; }
        // Punctuation: 0x9A–0x9F
        switch (c) {
            case 0x9A: result += '(';  continue;
            case 0x9B: result += ')';  continue;
            case 0x9C: result += ':';  continue;
            case 0x9D: result += ';';  continue;
            case 0x9E: result += '[';  continue;
            case 0x9F: result += ']';  continue;
        }
        // Lowercase a-z: 0xA0–0xB9
        if (c >= 0xA0 && c <= 0xB9) { result += static_cast<char>('a' + (c - 0xA0)); continue; }
        // German umlauts: 0xC0–0xC5
        if (c == 0xC0) { result += "\xC3\x84"; continue; }  // Ä
        if (c == 0xC1) { result += "\xC3\x96"; continue; }  // Ö
        if (c == 0xC2) { result += "\xC3\x9C"; continue; }  // Ü
        if (c == 0xC3) { result += "\xC3\xA4"; continue; }  // ä
        if (c == 0xC4) { result += "\xC3\xB6"; continue; }  // ö
        if (c == 0xC5) { result += "\xC3\xBC"; continue; }  // ü
        // Contractions: 0xD0–0xD6
        if (c == 0xD0) { result += "'d"; continue; }
        if (c == 0xD1) { result += "'l"; continue; }
        if (c == 0xD2) { result += "'m"; continue; }
        if (c == 0xD3) { result += "'r"; continue; }
        if (c == 0xD4) { result += "'s"; continue; }
        if (c == 0xD5) { result += "'t"; continue; }
        if (c == 0xD6) { result += "'v"; continue; }
        // Symbols
        if (c == 0xE0) { result += '\''; continue; }
        if (c == 0xE3) { result += '-';  continue; }
        if (c == 0xE6) { result += '?';  continue; }
        if (c == 0xE7) { result += '!';  continue; }
        if (c == 0xE8) { result += '.';  continue; }
        if (c == 0xE9) { result += '&';  continue; }
        if (c == 0xEA) { result += "\xC3\xA9"; continue; }  // é
        if (c == 0xF0) { result += "\xC2\xA5"; continue; }  // ¥
        if (c == 0xF1) { result += "\xC3\x97"; continue; }  // ×
        if (c == 0xF3) { result += '/';  continue; }
        if (c == 0xF4) { result += ',';  continue; }
        // Numbers: 0xF6–0xFF
        if (c >= 0xF6 && c <= 0xFF) { result += static_cast<char>('0' + (c - 0xF6)); continue; }
        // Space: 0x7F
        if (c == 0x7F) { result += ' '; continue; }
        // Unrecognized — emit '?'
        result += '?';
    }
    return result;
}

// ─── Crystal charmap encoder ─────────────────────────────────────────────────
// Encodes a UTF-8 string to Crystal charmap bytes, writing into `out` (which
// must be max_bytes long).  Fills remaining space with 0x50 terminators.
// Throws SaveExportError if any character is not representable.

static void encode_crystal_string(const std::string& utf8, uint8_t* out, uint32_t max_bytes) {
    uint32_t pos = 0;
    size_t i = 0;
    while (i < utf8.size() && pos < max_bytes - 1) {
        unsigned char c = static_cast<unsigned char>(utf8[i]);
        uint8_t code = 0x50;  // default: terminator (error sentinel)

        if (c >= 'A' && c <= 'Z') { code = 0x80 + (c - 'A'); i++; }
        else if (c >= 'a' && c <= 'z') { code = 0xA0 + (c - 'a'); i++; }
        else if (c >= '0' && c <= '9') { code = 0xF6 + (c - '0'); i++; }
        else if (c == '(') { code = 0x9A; i++; }
        else if (c == ')') { code = 0x9B; i++; }
        else if (c == ':') { code = 0x9C; i++; }
        else if (c == ';') { code = 0x9D; i++; }
        else if (c == '[') { code = 0x9E; i++; }
        else if (c == ']') { code = 0x9F; i++; }
        else if (c == '\'') { code = 0xE0; i++; }
        else if (c == '-')  { code = 0xE3; i++; }
        else if (c == '?')  { code = 0xE6; i++; }
        else if (c == '!')  { code = 0xE7; i++; }
        else if (c == '.')  { code = 0xE8; i++; }
        else if (c == '&')  { code = 0xE9; i++; }
        else if (c == '/')  { code = 0xF3; i++; }
        else if (c == ',')  { code = 0xF4; i++; }
        else if (c == ' ')  { code = 0x7F; i++; }
        else if (c == 0xC3 && i + 1 < utf8.size()) {
            // Multi-byte UTF-8
            unsigned char c2 = static_cast<unsigned char>(utf8[i + 1]);
            if (c2 == 0x84) { code = 0xC0; i += 2; }       // Ä
            else if (c2 == 0x96) { code = 0xC1; i += 2; }  // Ö
            else if (c2 == 0x9C) { code = 0xC2; i += 2; }  // Ü
            else if (c2 == 0xA4) { code = 0xC3; i += 2; }  // ä
            else if (c2 == 0xB6) { code = 0xC4; i += 2; }  // ö
            else if (c2 == 0xBC) { code = 0xC5; i += 2; }  // ü
            else if (c2 == 0xA9) { code = 0xEA; i += 2; }  // é
            else if (c2 == 0x97) { code = 0xF1; i += 2; }  // ×
            else goto unrepresentable;
        }
        else if (c == 0xC2 && i + 1 < utf8.size()) {
            unsigned char c2 = static_cast<unsigned char>(utf8[i + 1]);
            if (c2 == 0xA5) { code = 0xF0; i += 2; }  // ¥
            else goto unrepresentable;
        }
        else {
            unrepresentable:
            throw SaveExportError(
                "encode_crystal_string: character not representable in Crystal charmap at UTF-8 byte "
                + std::to_string(i) + " (0x" + [c]{ char buf[4]{}; snprintf(buf,4,"%02X",c); return std::string(buf); }() + ")");
        }
        out[pos++] = code;
    }
    // Fill remainder with terminator
    while (pos < max_bytes) out[pos++] = 0x50;
}

// ─── Snapshot decode (Phase 1+2) ─────────────────────────────────────────────

// adj == 0 for primary, BACKUP_OFFSET for backup.
static CrystalSaveSnapshot decode_snapshot(const uint8_t* data, uint32_t adj) {
    using namespace sram_layout;

    CrystalSaveSnapshot snap;

    // ── Phase 1 ───────────────────────────────────────────────────────────────

    snap.money_player = bcd3_decode(&data[MONEY        - adj]);
    snap.money_mom    = bcd3_decode(&data[MOMS_MONEY   - adj]);
    {
        uint32_t off = COINS - adj;
        snap.coins = static_cast<uint16_t>(
            (static_cast<uint32_t>(data[off]) << 8) | data[off + 1]);
    }
    {
        uint32_t off = EVENT_FLAGS - adj;
        std::copy(&data[off], &data[off + EVENT_FLAGS_SIZE], snap.event_flags.data());
    }

    // ── Phase 2: Player identity ─────────────────────────────────────────────

    // wPlayerID: 2 bytes LE (Crystal stores trainer ID little-endian)
    {
        uint32_t off = PLAYER_ID - adj;
        snap.player_id = static_cast<uint16_t>(
            static_cast<uint32_t>(data[off]) | (static_cast<uint32_t>(data[off + 1]) << 8));
    }
    // wSecretID: 2 bytes LE
    {
        uint32_t off = SECRET_ID - adj;
        snap.secret_id = static_cast<uint16_t>(
            static_cast<uint32_t>(data[off]) | (static_cast<uint32_t>(data[off + 1]) << 8));
    }
    // Name strings (Crystal charmap → UTF-8)
    snap.player_name = decode_crystal_string(&data[PLAYER_NAME - adj], PLAYER_NAME_SIZE);
    snap.moms_name   = decode_crystal_string(&data[MOMS_NAME   - adj], MOMS_NAME_SIZE);
    snap.rival_name  = decode_crystal_string(&data[RIVAL_NAME  - adj], RIVAL_NAME_SIZE);

    // wPlayerGender: in sOptions block (adj does NOT apply — sOptions is a distinct region)
    // For both primary and backup copies the gender is at its respective Options block.
    // When adj==0 (primary) → PLAYER_GENDER == PRIMARY_OPTIONS == 0x2000
    // When adj==BACKUP_OFFSET (backup) → backup options = BACKUP_OPTIONS = 0x1200
    {
        uint32_t gender_sav = (adj == 0) ? PRIMARY_OPTIONS : BACKUP_OPTIONS;
        snap.player_gender = data[gender_sav] & 0x01;  // bit 0
    }

    // ── Phase 2: Map position ────────────────────────────────────────────────

    snap.location.warp   = data[WARP_NUMBER - adj];
    snap.location.group  = data[MAP_GROUP   - adj];
    snap.location.number = data[MAP_NUMBER  - adj];
    snap.location.y      = data[PLAYER_Y    - adj];
    snap.location.x      = data[PLAYER_X    - adj];

    // ── Phase 2: Scene state ─────────────────────────────────────────────────

    {
        uint32_t off = SCENE_IDS_BASE - adj;
        std::copy(&data[off], &data[off + SCENE_IDS_COUNT], snap.scene_ids.data());
    }

    // ── Phase 2: Play time ───────────────────────────────────────────────────

    // wGameTimeCap: bit 0 = capped flag (GAME_TIME_CAPPED EQU 0 from ram_constants.asm)
    snap.playtime_capped  = (data[GAME_TIME_CAP - adj] & 0x01) != 0;
    // wGameTimeHours: 2 bytes BIG-ENDIAN (suiCune fix 2026-08)
    {
        uint32_t off = GAME_TIME_HOURS - adj;
        snap.playtime_hours = static_cast<uint16_t>(
            (static_cast<uint32_t>(data[off]) << 8) | static_cast<uint32_t>(data[off + 1]));
    }
    snap.playtime_minutes = data[GAME_TIME_MINUTES - adj];
    snap.playtime_seconds = data[GAME_TIME_SECONDS - adj];

    // ── Phase 2: RTC / time context ─────────────────────────────────────────

    {
        uint32_t off = RTC_BYTES - adj;
        snap.rtc.day    = data[off + 0];
        snap.rtc.hour   = data[off + 1];
        snap.rtc.minute = data[off + 2];
        snap.rtc.second = data[off + 3];
    }
    snap.rtc.dst = (data[DST - adj] & 0x80) != 0;

    // ── Phase 3A: Party ──────────────────────────────────────────────────────
    // Domain uses defaults (Crystal v1.1): 251 species, 251 moves, 256 items.
    // The codec boundary enforces these bounds; callers may pass tighter bounds
    // via the import_save_with_domain() overload when available.
    snap.party = decode_party(data, adj, PartyCodecDomain{});

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

    if (size < SRAM_SIZE) {
        throw SaveImportError(
            "import_save: file too small (" + std::to_string(size) +
            " bytes; expected at least " + std::to_string(SRAM_SIZE) + ")");
    }
    if (bytes == nullptr) {
        throw std::invalid_argument("import_save: bytes must not be null");
    }

    CopyValidity primary_v = check_primary(bytes);
    CopyValidity backup_v  = check_backup(bytes);

    bool use_primary;
    if (primary_v.fully_valid()) {
        use_primary = true;
    } else if (backup_v.fully_valid()) {
        use_primary = false;
    } else {
        std::string diag = "import_save: no valid save copy found. ";
        diag += "Primary: sentinel=" + std::string(primary_v.sentinel_ok ? "OK" : "FAIL");
        diag += " checksum="         + std::string(primary_v.checksum_ok ? "OK" : "FAIL");
        diag += ".  Backup: sentinel=" + std::string(backup_v.sentinel_ok ? "OK" : "FAIL");
        diag += " checksum="          + std::string(backup_v.checksum_ok ? "OK" : "FAIL") + ".";
        throw SaveImportError(diag);
    }

    CrystalImport result;
    std::copy(bytes, bytes + SRAM_SIZE, result.shadow.data.begin());
    if (size > SRAM_SIZE) {
        result.shadow.trailer.assign(bytes + SRAM_SIZE, bytes + size);
    }

    uint32_t adj       = use_primary ? 0u : BACKUP_OFFSET;
    result.snapshot    = decode_snapshot(bytes, adj);

    result.shadow.identity.profile_sha1  = std::move(profile_sha1);
    result.shadow.identity.rom_sha1      = std::move(rom_sha1);
    result.shadow.identity.codec_version = SRAM_CODEC_VERSION;

    return result;
}

// ─── encode_crystal_string exposed for writer ─────────────────────────────────

void encode_crystal_string_to(const std::string& utf8, uint8_t* out, uint32_t max_bytes) {
    encode_crystal_string(utf8, out, max_bytes);
}

// ─── decode_crystal_string exposed for party codec ────────────────────────────

std::string decode_crystal_string_from(const uint8_t* bytes, uint32_t max_bytes) {
    return decode_crystal_string(bytes, max_bytes);
}

}  // namespace crystal
