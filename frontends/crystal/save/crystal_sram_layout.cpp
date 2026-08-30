// frontends/crystal/save/crystal_sram_layout.cpp
//
// Runtime validation of sram_layout constants against the assembled sym file.

#include "crystal_sram_layout.hpp"

#include <fstream>
#include <regex>
#include <sstream>
#include <unordered_map>
#include <format>

namespace crystal {
namespace sram_layout {

namespace {

// Parse one line of a pokecrystal RGBDS .sym file.
// Format: BB:AAAA name
// Returns {name, sav_offset} where sav_offset uses the SRAM formula:
//   bank * 0x2000 + (addr - 0xA000)   for SRAM symbols (addr >= 0xA000)
//   bank * 0x2000 + addr               for bank-0 absolute (addr < 0xA000, bank == 0)
//   0 + addr                           for WRAM/ROM (bank == 1..N, addr 0xC000+)
//
// We store two offsets:
//   sram_sav_offset — only valid for SRAM symbols (addr 0xA000–0xBFFF)
//   wram_addr       — raw 16-bit address for WRAM symbols

struct ParsedSym {
    uint8_t  bank   = 0;
    uint16_t addr   = 0;

    // SRAM file offset (valid only when 0xA000 <= addr <= 0xBFFF)
    uint32_t sav_offset() const {
        return static_cast<uint32_t>(bank) * 0x2000u +
               (static_cast<uint32_t>(addr) - 0xA000u);
    }

    // Raw WRAM address (the addr field)
    uint16_t wram() const { return addr; }

    bool is_sram() const { return addr >= 0xA000 && addr <= 0xBFFF; }
    bool is_wram() const { return addr >= 0xC000; }
};

std::unordered_map<std::string, ParsedSym> load_sym(const std::filesystem::path& path) {
    std::unordered_map<std::string, ParsedSym> result;
    std::ifstream f(path);
    if (!f) return result;

    std::regex re(R"(([0-9A-Fa-f]{1,2}):([0-9A-Fa-f]{4})\s+(\S+))");
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == ';') continue;
        std::smatch m;
        if (!std::regex_search(line, m, re)) continue;
        ParsedSym s;
        s.bank = static_cast<uint8_t>(std::stoul(m[1].str(), nullptr, 16));
        s.addr = static_cast<uint16_t>(std::stoul(m[2].str(), nullptr, 16));
        result[m[3].str()] = s;
    }
    return result;
}

// WRAM → SRAM primary sav offset
// sav = PRIMARY_GAME_DATA + (wram_addr - wGameData_wram)
// wGameData is at WRAM 0xD47B (sym: 01:d47b)
constexpr uint16_t W_GAME_DATA_WRAM = 0xD47B;

uint32_t wram_to_primary_sav(uint16_t wram_addr) {
    return PRIMARY_GAME_DATA + (static_cast<uint32_t>(wram_addr) -
                                static_cast<uint32_t>(W_GAME_DATA_WRAM));
}

}  // namespace

std::string validate_layout_against_sym(const std::filesystem::path& sym_file) {
    if (!std::filesystem::exists(sym_file)) {
        // Sym file not present — skip silently (CI without references/).
        return "";
    }

    auto syms = load_sym(sym_file);
    if (syms.empty()) {
        return "validate_layout_against_sym: sym file loaded but contained no symbols: "
               + sym_file.string();
    }

    std::ostringstream err;
    uint32_t failures = 0;

    auto check_sram = [&](const char* name, uint32_t expected_sav) {
        auto it = syms.find(name);
        if (it == syms.end()) {
            err << "  MISSING sym '" << name << "'\n";
            ++failures;
            return;
        }
        if (!it->second.is_sram()) {
            err << std::format("  sym '{}' is not in SRAM range (addr=0x{:04X})\n",
                               name, it->second.addr);
            ++failures;
            return;
        }
        uint32_t sym_sav = it->second.sav_offset();
        if (sym_sav != expected_sav) {
            err << std::format(
                "  MISMATCH '{}': sym=0x{:04X}  expected=0x{:04X}  DIFF={:+d}\n",
                name, sym_sav, expected_sav,
                static_cast<int32_t>(sym_sav) - static_cast<int32_t>(expected_sav));
            ++failures;
        }
    };

    auto check_wram_to_primary = [&](const char* name, uint32_t expected_sav) {
        auto it = syms.find(name);
        if (it == syms.end()) {
            err << "  MISSING sym '" << name << "'\n";
            ++failures;
            return;
        }
        if (!it->second.is_wram()) {
            err << std::format("  sym '{}' is not in WRAM range (addr=0x{:04X})\n",
                               name, it->second.addr);
            ++failures;
            return;
        }
        uint32_t derived = wram_to_primary_sav(it->second.wram());
        if (derived != expected_sav) {
            err << std::format(
                "  MISMATCH '{}': wram=0x{:04X} → sav=0x{:04X}  expected=0x{:04X}  DIFF={:+d}\n",
                name, it->second.wram(), derived, expected_sav,
                static_cast<int32_t>(derived) - static_cast<int32_t>(expected_sav));
            ++failures;
        }
    };

    // Verify wGameData anchor (the base for all WRAM→SRAM derivations).
    {
        auto it = syms.find("wGameData");
        if (it == syms.end()) {
            err << "  MISSING sym 'wGameData' (anchor for WRAM→SRAM derivation)\n";
            ++failures;
        } else if (it->second.wram() != W_GAME_DATA_WRAM) {
            err << std::format(
                "  ANCHOR MISMATCH 'wGameData': sym wram=0x{:04X}  expected=0x{:04X}\n",
                it->second.wram(), W_GAME_DATA_WRAM);
            ++failures;
        }
    }

    // ── SRAM-symbol checks (sym formula: bank * 0x2000 + addr - 0xA000) ──────
    check_sram("sOptions",           PRIMARY_OPTIONS);
    check_sram("sCheckValue1",       PRIMARY_CHECK_VALUE_1);
    check_sram("sGameData",          PRIMARY_GAME_DATA);
    // sGameDataEnd is exclusive end; last checksummed byte = sGameDataEnd - 1
    {
        auto it = syms.find("sGameDataEnd");
        if (it == syms.end()) {
            err << "  MISSING sym 'sGameDataEnd'\n";
            ++failures;
        } else {
            uint32_t end_sav = it->second.sav_offset();
            uint32_t expected_end = PRIMARY_CHECKSUM_END + 1;  // exclusive
            if (end_sav != expected_end) {
                err << std::format(
                    "  MISMATCH 'sGameDataEnd': sym=0x{:04X}  expected=0x{:04X}\n",
                    end_sav, expected_end);
                ++failures;
            }
        }
    }
    check_sram("sChecksum",          PRIMARY_CHECKSUM);
    check_sram("sCheckValue2",       PRIMARY_CHECK_VALUE_2);

    check_sram("sBackupOptions",     BACKUP_OPTIONS);
    check_sram("sBackupCheckValue1", BACKUP_CHECK_VALUE_1);
    check_sram("sBackupGameData",    BACKUP_GAME_DATA);
    {
        auto it = syms.find("sBackupGameDataEnd");
        if (it == syms.end()) {
            err << "  MISSING sym 'sBackupGameDataEnd'\n";
            ++failures;
        } else {
            uint32_t end_sav = it->second.sav_offset();
            uint32_t expected_end = BACKUP_CHECKSUM_END + 1;
            if (end_sav != expected_end) {
                err << std::format(
                    "  MISMATCH 'sBackupGameDataEnd': sym=0x{:04X}  expected=0x{:04X}\n",
                    end_sav, expected_end);
                ++failures;
            }
        }
    }
    check_sram("sBackupChecksum",    BACKUP_CHECKSUM);
    check_sram("sBackupCheckValue2", BACKUP_CHECK_VALUE_2);

    // ── WRAM-derived field checks ─────────────────────────────────────────────
    check_wram_to_primary("wMoney",      MONEY);
    check_wram_to_primary("wMomsMoney",  MOMS_MONEY);
    check_wram_to_primary("wCoins",      COINS);
    check_wram_to_primary("wEventFlags", EVENT_FLAGS);
    // Phase 2 fields
    check_wram_to_primary("wPlayerID",           PLAYER_ID);
    check_wram_to_primary("wPlayerName",         PLAYER_NAME);
    check_wram_to_primary("wMomsName",           MOMS_NAME);
    check_wram_to_primary("wRivalName",          RIVAL_NAME);
    check_wram_to_primary("wSecretID",           SECRET_ID);
    check_wram_to_primary("wStartDay",           START_DAY);
    check_wram_to_primary("wRTC",                RTC_BYTES);
    check_wram_to_primary("wDST",                DST);
    check_wram_to_primary("wGameTimeCap",        GAME_TIME_CAP);
    check_wram_to_primary("wGameTimeHours",      GAME_TIME_HOURS);
    check_wram_to_primary("wGameTimeMinutes",    GAME_TIME_MINUTES);
    check_wram_to_primary("wGameTimeSeconds",    GAME_TIME_SECONDS);
    check_wram_to_primary("wGameTimeFrames",     GAME_TIME_FRAMES);
    check_wram_to_primary("wPokecenter2FSceneID", SCENE_IDS_BASE);
    check_wram_to_primary("wMapGroup",           MAP_GROUP);
    check_wram_to_primary("wMapNumber",          MAP_NUMBER);
    check_wram_to_primary("wYCoord",             PLAYER_Y);
    check_wram_to_primary("wXCoord",             PLAYER_X);

    if (failures == 0) return "";  // all good

    std::string prefix = std::format(
        "validate_layout_against_sym: {} offset mismatch(es) in '{}':\n",
        failures, sym_file.string());
    return prefix + err.str();
}

}  // namespace sram_layout
}  // namespace crystal
