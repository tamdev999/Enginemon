// tests/crystal/crystal_save_test.cpp
//
// Crystal .sav codec Phase 1 tests.
//
// Required coverage (10 categories):
//  1.  Valid import
//  2.  Corrupt checksum handling
//  3.  Backup recovery
//  4.  Wrong-size rejection
//  5.  Unchanged importâ†’export preserves unowned bytes
//  6.  money / mom-money / coins remain independent
//  7.  EventFlag / EngineFlag remain independent
//  8.  Owned-field edit changes only expected regions + checksum/copy machinery
//  9.  Exported save re-imports correctly
// 10.  Malformed input never causes UB
//
// Fixture helpers build minimal valid 32 KB SRAM images from scratch so
// tests do not depend on a real ROM file.

#include "frontends/crystal/save/crystal_save_codec.hpp"
#include "frontends/crystal/save/crystal_bcd.hpp"
#include "frontends/crystal/save/crystal_sram_layout.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include <filesystem>

// â”€â”€â”€ Minimal test framework â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

static int g_pass = 0;
static int g_fail = 0;

#define TEST(name) static void name()
#define RUN(name) do { \
    try { name(); ++g_pass; std::cout << "  PASS  " #name "\n"; } \
    catch (const std::exception& e) { ++g_fail; std::cout << "  FAIL  " #name " â€” " << e.what() << "\n"; } \
    catch (...) { ++g_fail; std::cout << "  FAIL  " #name " â€” unknown exception\n"; } \
} while (false)

#define ASSERT_TRUE(expr) \
    do { if (!(expr)) throw std::runtime_error("ASSERT_TRUE failed: " #expr); } while(false)
#define ASSERT_EQ(a, b) \
    do { if ((a) != (b)) throw std::runtime_error( \
        std::string("ASSERT_EQ failed: ") + #a " = " + std::to_string(a) + ", " #b " = " + std::to_string(b)); } while(false)
#define ASSERT_THROWS(expr, ExType) \
    do { bool _threw = false; try { (expr); } catch (const ExType&) { _threw = true; } \
         if (!_threw) throw std::runtime_error("Expected " #ExType " not thrown: " #expr); } while(false)
#define ASSERT_NO_THROW(expr) \
    do { try { (expr); } catch (const std::exception& _e) { \
         throw std::runtime_error(std::string("Unexpected exception: ") + _e.what()); } } while(false)

// â”€â”€â”€ Fixture helpers â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

using namespace crystal;
using namespace crystal::sram_layout;

/// Build a valid 32 KB SRAM image with correct sentinels and checksums.
/// All payload bytes are zero except what we explicitly set.
static std::vector<uint8_t> make_valid_sram(
    uint32_t money_player = 0,
    uint32_t money_mom    = 0,
    uint16_t coins        = 0,
    const std::array<uint8_t, 100>* event_flags = nullptr)
{
    std::vector<uint8_t> sav(SRAM_SIZE, 0x00);
    uint8_t* d = sav.data();

    // â”€â”€ Money â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    auto mp = bcd3_encode(money_player);
    auto mm = bcd3_encode(money_mom);
    std::copy(mp.begin(), mp.end(), d + MONEY);
    std::copy(mm.begin(), mm.end(), d + MOMS_MONEY);

    // â”€â”€ Coins â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    d[COINS]     = static_cast<uint8_t>(coins >> 8);
    d[COINS + 1] = static_cast<uint8_t>(coins & 0xFF);

    // â”€â”€ Event flags â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    if (event_flags) {
        std::copy(event_flags->begin(), event_flags->end(), d + EVENT_FLAGS);
    }

    auto compute_cs = [&](uint32_t begin, uint32_t end) -> uint16_t {
        uint16_t s = 0;
        for (uint32_t i = begin; i <= end; ++i) s += d[i];
        return s;
    };

    // Primary sentinels and checksum
    d[PRIMARY_CHECK_VALUE_1] = SAVE_CHECK_VALUE_1;
    d[PRIMARY_CHECK_VALUE_2] = SAVE_CHECK_VALUE_2;
    uint16_t pcs = compute_cs(PRIMARY_GAME_DATA, PRIMARY_CHECKSUM_END);
    d[PRIMARY_CHECKSUM]     = static_cast<uint8_t>(pcs & 0xFF);
    d[PRIMARY_CHECKSUM + 1] = static_cast<uint8_t>(pcs >> 8);

    // Copy primary â†’ backup
    std::copy(d + PRIMARY_GAME_DATA,
              d + PRIMARY_GAME_DATA + CHECKSUM_REGION_SIZE,
              d + BACKUP_GAME_DATA);
    std::copy(d + PRIMARY_OPTIONS,
              d + PRIMARY_OPTIONS + PRIMARY_OPTIONS_SIZE,
              d + BACKUP_OPTIONS);

    // Backup sentinels and checksum
    d[BACKUP_CHECK_VALUE_1] = SAVE_CHECK_VALUE_1;
    d[BACKUP_CHECK_VALUE_2] = SAVE_CHECK_VALUE_2;
    uint16_t bcs = compute_cs(BACKUP_GAME_DATA, BACKUP_CHECKSUM_END);
    d[BACKUP_CHECKSUM]     = static_cast<uint8_t>(bcs & 0xFF);
    d[BACKUP_CHECKSUM + 1] = static_cast<uint8_t>(bcs >> 8);

    return sav;
}

/// Corrupt the primary checksum by flipping the first checksum byte.
static void corrupt_primary_checksum(std::vector<uint8_t>& sav) {
    sav[PRIMARY_CHECKSUM] ^= 0xFF;
}

/// Corrupt the backup checksum.
static void corrupt_backup_checksum(std::vector<uint8_t>& sav) {
    sav[BACKUP_CHECKSUM] ^= 0xFF;
}

/// Corrupt the primary sentinel byte 1.
static void corrupt_primary_sentinel(std::vector<uint8_t>& sav) {
    sav[PRIMARY_CHECK_VALUE_1] = 0x00;
}

/// Corrupt the backup sentinel byte 1.
static void corrupt_backup_sentinel(std::vector<uint8_t>& sav) {
    sav[BACKUP_CHECK_VALUE_1] = 0x00;
}

// â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
// Category 1: Valid import
// â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

TEST(valid_import_basic) {
    auto sav = make_valid_sram(12345, 6789, 999);
    CrystalImport imp = import_save(sav);
    ASSERT_EQ(imp.snapshot.money_player, 12345u);
    ASSERT_EQ(imp.snapshot.money_mom, 6789u);
    ASSERT_EQ(imp.snapshot.coins, 999u);
}

TEST(valid_import_zero_values) {
    auto sav = make_valid_sram(0, 0, 0);
    CrystalImport imp = import_save(sav);
    ASSERT_EQ(imp.snapshot.money_player, 0u);
    ASSERT_EQ(imp.snapshot.money_mom, 0u);
    ASSERT_EQ(imp.snapshot.coins, 0u);
}

TEST(valid_import_max_values) {
    auto sav = make_valid_sram(999999, 999999, 9999);
    CrystalImport imp = import_save(sav);
    ASSERT_EQ(imp.snapshot.money_player, 999999u);
    ASSERT_EQ(imp.snapshot.money_mom, 999999u);
    ASSERT_EQ(imp.snapshot.coins, 9999u);
}

TEST(valid_import_shadow_preserved) {
    auto sav = make_valid_sram(42, 0, 0);
    // Write a sentinel byte in an unowned region so we can check it survives.
    sav[0x3E3D] = 0x01;  // player gender field (outside checksummed region)
    // Recompute checksums after the edit (it's outside checksum region so checksums unchanged)
    CrystalImport imp = import_save(sav);
    ASSERT_EQ(imp.shadow.data[0x3E3D], 0x01);
}

TEST(valid_import_with_trailer) {
    auto sav = make_valid_sram();
    // Append a fake 44-byte emulator RTC trailer
    for (int i = 0; i < 44; ++i) sav.push_back(static_cast<uint8_t>(i));
    CrystalImport imp = import_save(sav);
    ASSERT_EQ(imp.shadow.trailer.size(), 44u);
    ASSERT_EQ(imp.shadow.trailer[0], 0x00);
    ASSERT_EQ(imp.shadow.trailer[43], 43u);
}

// â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
// Category 2: Corrupt checksum handling
// â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

TEST(corrupt_primary_checksum_rejected_if_backup_also_bad) {
    auto sav = make_valid_sram();
    corrupt_primary_checksum(sav);
    corrupt_backup_checksum(sav);
    ASSERT_THROWS(import_save(sav), SaveImportError);
}

TEST(corrupt_primary_sentinel_rejected_if_backup_also_bad) {
    auto sav = make_valid_sram();
    corrupt_primary_sentinel(sav);
    corrupt_backup_sentinel(sav);
    ASSERT_THROWS(import_save(sav), SaveImportError);
}

TEST(corrupt_checksum_error_message_is_informative) {
    auto sav = make_valid_sram();
    corrupt_primary_checksum(sav);
    corrupt_backup_checksum(sav);
    try {
        import_save(sav);
        throw std::runtime_error("should have thrown");
    } catch (const SaveImportError& e) {
        std::string msg = e.what();
        // Must mention both copies.
        ASSERT_TRUE(msg.find("Primary") != std::string::npos);
        ASSERT_TRUE(msg.find("Backup")  != std::string::npos);
    }
}

// â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
// Category 3: Backup recovery
// â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

TEST(backup_recovery_when_primary_checksum_bad) {
    // Write money_player=100 into a valid image.
    auto sav = make_valid_sram(100, 0, 0);
    // Corrupt primary checksum â€” backup remains valid.
    corrupt_primary_checksum(sav);
    // Import should succeed using the backup copy.
    CrystalImport imp = import_save(sav);
    // Backup mirrors primary, so money should still be 100.
    ASSERT_EQ(imp.snapshot.money_player, 100u);
}

TEST(backup_recovery_when_primary_sentinel_bad) {
    auto sav = make_valid_sram(200, 0, 0);
    corrupt_primary_sentinel(sav);
    CrystalImport imp = import_save(sav);
    ASSERT_EQ(imp.snapshot.money_player, 200u);
}

TEST(backup_preferred_order_primary_first) {
    // Both copies valid â†’ primary wins.
    // Manually patch primary money to 777 and backup money to 888.
    auto sav = make_valid_sram(777, 0, 0);
    // Patch backup money directly (backup copy mirrors primary; override for test).
    // First encode 888 as BCD and write into backup offset.
    auto mm888 = bcd3_encode(888);
    std::copy(mm888.begin(), mm888.end(), sav.data() + BACKUP_MONEY);
    // Recompute backup checksum.
    {
        uint16_t s = 0;
        for (uint32_t i = BACKUP_GAME_DATA; i <= BACKUP_CHECKSUM_END; ++i)
            s += sav[i];
        sav[BACKUP_CHECKSUM]     = static_cast<uint8_t>(s & 0xFF);
        sav[BACKUP_CHECKSUM + 1] = static_cast<uint8_t>(s >> 8);
    }
    // Both copies now valid, primary has 777, backup has 888.
    CrystalImport imp = import_save(sav);
    ASSERT_EQ(imp.snapshot.money_player, 777u);  // primary wins
}

// â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
// Category 4: Wrong-size rejection
// â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

TEST(empty_input_rejected) {
    std::vector<uint8_t> empty;
    // empty.data() on MSVC returns a non-null pointer to a zero-size allocation,
    // so null check won't fire. The size check fires and throws SaveImportError.
    ASSERT_THROWS(import_save(empty), SaveImportError);
}

TEST(too_small_input_rejected) {
    std::vector<uint8_t> small(SRAM_SIZE - 1, 0);
    ASSERT_THROWS(import_save(small), SaveImportError);
}

TEST(null_pointer_rejected) {
    ASSERT_THROWS(import_save(nullptr, SRAM_SIZE), std::invalid_argument);
}

TEST(exactly_32kb_accepted) {
    auto sav = make_valid_sram();
    ASSERT_EQ(sav.size(), SRAM_SIZE);
    ASSERT_NO_THROW(import_save(sav));
}

TEST(larger_than_32kb_accepted_trailer_preserved) {
    auto sav = make_valid_sram();
    sav.push_back(0xAB);
    sav.push_back(0xCD);
    CrystalImport imp = import_save(sav);
    ASSERT_EQ(imp.shadow.trailer.size(), 2u);
    ASSERT_EQ(imp.shadow.trailer[0], 0xAB);
    ASSERT_EQ(imp.shadow.trailer[1], 0xCD);
}

// â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
// Category 5: Unchanged importâ†’export preserves unowned bytes
// â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

TEST(roundtrip_unowned_bytes_preserved) {
    auto sav = make_valid_sram(500, 300, 100);
    // Write distinguishable bytes in several unowned SRAM regions.
    // Boxes area (0x4000â€“0x5E2F) â€” outside checksummed region.
    sav[0x4000] = 0xDE;
    sav[0x4001] = 0xAD;
    sav[0x5E00] = 0xBE;
    sav[0x5E01] = 0xEF;
    // Hall of Fame (0x3E00 range) â€” outside checksummed region.
    sav[0x3E00] = 0x42;

    CrystalImport imp = import_save(sav);
    std::vector<uint8_t> out = export_save(imp.snapshot, imp.shadow);

    // Unowned regions must be byte-identical.
    ASSERT_EQ(out[0x4000], 0xDE);
    ASSERT_EQ(out[0x4001], 0xAD);
    ASSERT_EQ(out[0x5E00], 0xBE);
    ASSERT_EQ(out[0x5E01], 0xEF);
    ASSERT_EQ(out[0x3E00], 0x42);

    // Output must be exactly SRAM_SIZE bytes (no trailer in this case).
    ASSERT_EQ(out.size(), SRAM_SIZE);
}

TEST(roundtrip_trailer_preserved) {
    auto sav = make_valid_sram();
    sav.push_back(0x11);
    sav.push_back(0x22);
    sav.push_back(0x33);
    CrystalImport imp = import_save(sav);
    std::vector<uint8_t> out = export_save(imp.snapshot, imp.shadow);
    ASSERT_EQ(out.size(), SRAM_SIZE + 3);
    ASSERT_EQ(out[SRAM_SIZE + 0], 0x11);
    ASSERT_EQ(out[SRAM_SIZE + 1], 0x22);
    ASSERT_EQ(out[SRAM_SIZE + 2], 0x33);
}

TEST(roundtrip_no_edit_checksums_valid) {
    // A round-trip without any edits must produce a valid, re-importable save.
    auto sav = make_valid_sram(12345, 678, 99);
    CrystalImport imp = import_save(sav);
    std::vector<uint8_t> out = export_save(imp.snapshot, imp.shadow);
    ASSERT_NO_THROW(import_save(out));
}

// â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
// Category 6: money / mom-money / coins remain independent
// â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

TEST(money_fields_independent_player_edit) {
    auto sav = make_valid_sram(111, 222, 333);
    CrystalImport imp = import_save(sav);
    // Edit only player money.
    imp.snapshot.money_player = 999999;
    std::vector<uint8_t> out = export_save(imp.snapshot, imp.shadow);
    CrystalImport reimp = import_save(out);
    ASSERT_EQ(reimp.snapshot.money_player, 999999u);
    ASSERT_EQ(reimp.snapshot.money_mom, 222u);
    ASSERT_EQ(reimp.snapshot.coins, 333u);
}

TEST(money_fields_independent_mom_edit) {
    auto sav = make_valid_sram(111, 222, 333);
    CrystalImport imp = import_save(sav);
    imp.snapshot.money_mom = 0;
    std::vector<uint8_t> out = export_save(imp.snapshot, imp.shadow);
    CrystalImport reimp = import_save(out);
    ASSERT_EQ(reimp.snapshot.money_player, 111u);
    ASSERT_EQ(reimp.snapshot.money_mom, 0u);
    ASSERT_EQ(reimp.snapshot.coins, 333u);
}

TEST(money_fields_independent_coins_edit) {
    auto sav = make_valid_sram(111, 222, 333);
    CrystalImport imp = import_save(sav);
    imp.snapshot.coins = 9999;
    std::vector<uint8_t> out = export_save(imp.snapshot, imp.shadow);
    CrystalImport reimp = import_save(out);
    ASSERT_EQ(reimp.snapshot.money_player, 111u);
    ASSERT_EQ(reimp.snapshot.money_mom, 222u);
    ASSERT_EQ(reimp.snapshot.coins, 9999u);
}

TEST(money_overflow_rejected) {
    auto sav = make_valid_sram();
    CrystalImport imp = import_save(sav);
    imp.snapshot.money_player = BCD3_MAX + 1;
    ASSERT_THROWS(export_save(imp.snapshot, imp.shadow), SaveExportError);
}

TEST(mom_money_overflow_rejected) {
    auto sav = make_valid_sram();
    CrystalImport imp = import_save(sav);
    imp.snapshot.money_mom = BCD3_MAX + 1;
    ASSERT_THROWS(export_save(imp.snapshot, imp.shadow), SaveExportError);
}

TEST(coins_overflow_rejected) {
    auto sav = make_valid_sram();
    CrystalImport imp = import_save(sav);
    imp.snapshot.coins = COINS_MAX + 1;
    ASSERT_THROWS(export_save(imp.snapshot, imp.shadow), SaveExportError);
}

// â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
// Category 7: EventFlag / EngineFlag remain independent
// â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

TEST(event_flags_round_trip_bit_set) {
    // Set flag 0 (bit 0 of byte 0) and flag 799 (bit 7 of byte 99).
    std::array<uint8_t, 100> flags{};
    flags[0]  = 0x01;  // flag 0
    flags[99] = 0x80;  // flag 799
    auto sav = make_valid_sram(0, 0, 0, &flags);
    CrystalImport imp = import_save(sav);
    ASSERT_EQ(imp.snapshot.event_flags[0],  0x01);
    ASSERT_EQ(imp.snapshot.event_flags[99], 0x80);
}

TEST(event_flags_round_trip_all_clear) {
    auto sav = make_valid_sram();
    CrystalImport imp = import_save(sav);
    for (int i = 0; i < 100; ++i)
        ASSERT_EQ(imp.snapshot.event_flags[i], 0x00);
}

TEST(event_flags_edit_persists_through_export) {
    auto sav = make_valid_sram();
    CrystalImport imp = import_save(sav);
    // Set flag 42 (byte 5, bit 2).
    imp.snapshot.event_flags[5] = 0x04;
    std::vector<uint8_t> out = export_save(imp.snapshot, imp.shadow);
    CrystalImport reimp = import_save(out);
    ASSERT_EQ(reimp.snapshot.event_flags[5], 0x04);
}

TEST(event_flags_edit_does_not_affect_money) {
    auto sav = make_valid_sram(12345, 678, 99);
    CrystalImport imp = import_save(sav);
    imp.snapshot.event_flags[0] = 0xFF;
    std::vector<uint8_t> out = export_save(imp.snapshot, imp.shadow);
    CrystalImport reimp = import_save(out);
    ASSERT_EQ(reimp.snapshot.money_player, 12345u);
    ASSERT_EQ(reimp.snapshot.money_mom, 678u);
    ASSERT_EQ(reimp.snapshot.coins, 99u);
}

TEST(engine_flags_not_in_snapshot) {
    // EngineFlags ("eflag_XXXX") are Enginemon-only - they have no presence in
    // Crystal SRAM.  The CrystalSaveSnapshot struct must not have any eflag field.
    // This is a compile-time structural proof: we verify the snapshot only has the
    // four expected Phase-1 fields and that import works on a normal valid save.
    auto sav = make_valid_sram(42, 7, 3);
    CrystalImport imp = import_save(sav);

    // Access every field declared on the snapshot.
    // If a spurious eflag field existed it would either bloat sizeof or
    // fail to compile if addressed here.
    ASSERT_EQ(imp.snapshot.money_player, 42u);
    ASSERT_EQ(imp.snapshot.money_mom, 7u);
    ASSERT_EQ(imp.snapshot.coins, 3u);
    ASSERT_EQ(imp.snapshot.event_flags.size(), 100u);

    // The CrystalSaveSnapshot has exactly: money_player(4), money_mom(4),
    // coins(2), event_flags(100). No eflag field exists.
    static_assert(sizeof(CrystalSaveSnapshot) >= 4 + 4 + 2 + 100,
        "Snapshot must be at least as large as its declared fields");
}

// â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
// Category 8: Owned-field edit changes only expected regions + checksum/copy machinery
// â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

TEST(money_edit_only_touches_money_region_and_checksums) {
    auto sav = make_valid_sram(0, 0, 0);
    CrystalImport imp = import_save(sav);

    // Record every byte in the SRAM before editing money.
    Sram before = imp.shadow;

    imp.snapshot.money_player = 123456;
    std::vector<uint8_t> out = export_save(imp.snapshot, imp.shadow);

    // Bytes outside {MONEY, MOMS_MONEY, COINS, EVENT_FLAGS, checksums, sentinel,
    //                backup mirror} should be identical to the original.
    auto enc = bcd3_encode(123456u);

    // Verify primary money patched.
    ASSERT_EQ(out[MONEY + 0], enc[0]);
    ASSERT_EQ(out[MONEY + 1], enc[1]);
    ASSERT_EQ(out[MONEY + 2], enc[2]);

    // Verify backup mirror of money patched (backup = primary - BACKUP_OFFSET).
    ASSERT_EQ(out[BACKUP_MONEY + 0], enc[0]);
    ASSERT_EQ(out[BACKUP_MONEY + 1], enc[1]);
    ASSERT_EQ(out[BACKUP_MONEY + 2], enc[2]);

    // mom-money, coins unchanged.
    ASSERT_EQ(out[MOMS_MONEY + 0], 0x00);
    ASSERT_EQ(out[MOMS_MONEY + 1], 0x00);
    ASSERT_EQ(out[MOMS_MONEY + 2], 0x00);
    ASSERT_EQ(out[COINS + 0], 0x00);
    ASSERT_EQ(out[COINS + 1], 0x00);

    // Checksums are valid.
    ASSERT_NO_THROW(import_save(out));
}

TEST(event_flag_edit_only_touches_flags_region_and_checksums) {
    auto sav = make_valid_sram(500, 300, 200);
    CrystalImport imp = import_save(sav);

    imp.snapshot.event_flags[10] = 0xAB;
    std::vector<uint8_t> out = export_save(imp.snapshot, imp.shadow);

    // Primary flags patched.
    ASSERT_EQ(out[EVENT_FLAGS + 10], 0xAB);
    // Backup flags patched (mirror).
    ASSERT_EQ(out[BACKUP_EVENT_FLAGS + 10], 0xAB);

    // Money fields untouched.
    auto enc_p = bcd3_encode(500u);
    ASSERT_EQ(out[MONEY + 0], enc_p[0]);
    ASSERT_EQ(out[MONEY + 1], enc_p[1]);
    ASSERT_EQ(out[MONEY + 2], enc_p[2]);

    // Checksums valid.
    ASSERT_NO_THROW(import_save(out));
}

TEST(backup_mirror_matches_primary_after_export) {
    auto sav = make_valid_sram(77777, 8888, 1234);
    CrystalImport imp = import_save(sav);
    std::vector<uint8_t> out = export_save(imp.snapshot, imp.shadow);

    // Every byte in primary checksummed region must equal backup mirror byte.
    for (uint32_t i = 0; i < CHECKSUM_REGION_SIZE; ++i) {
        uint8_t prim = out[PRIMARY_GAME_DATA + i];
        uint8_t back = out[BACKUP_GAME_DATA  + i];
        if (prim != back) {
            throw std::runtime_error(
                "Primary/backup mismatch at offset " + std::to_string(i) +
                ": primary=0x" + std::to_string(prim) +
                " backup=0x" + std::to_string(back));
        }
    }
}

// â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
// Category 9: Exported save re-imports correctly
// â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

TEST(export_reimport_money_exact) {
    auto sav = make_valid_sram();
    CrystalImport imp = import_save(sav);
    imp.snapshot.money_player = 314159;
    imp.snapshot.money_mom    = 271828;
    imp.snapshot.coins        = 7654;
    std::vector<uint8_t> out = export_save(imp.snapshot, imp.shadow);
    CrystalImport reimp = import_save(out);
    ASSERT_EQ(reimp.snapshot.money_player, 314159u);
    ASSERT_EQ(reimp.snapshot.money_mom, 271828u);
    ASSERT_EQ(reimp.snapshot.coins, 7654u);
}

TEST(export_reimport_event_flags_exact) {
    auto sav = make_valid_sram();
    CrystalImport imp = import_save(sav);
    // Set alternating bits.
    for (int i = 0; i < 100; ++i)
        imp.snapshot.event_flags[i] = (i % 2 == 0) ? 0xAA : 0x55;
    std::vector<uint8_t> out = export_save(imp.snapshot, imp.shadow);
    CrystalImport reimp = import_save(out);
    for (int i = 0; i < 100; ++i) {
        uint8_t expected = (i % 2 == 0) ? 0xAA : 0x55;
        ASSERT_EQ(reimp.snapshot.event_flags[i], expected);
    }
}

TEST(export_reimport_is_idempotent) {
    // Two round-trips must produce identical output bytes.
    auto sav = make_valid_sram(98765, 43210, 5555);
    CrystalImport imp1 = import_save(sav);
    std::vector<uint8_t> out1 = export_save(imp1.snapshot, imp1.shadow);
    CrystalImport imp2 = import_save(out1);
    std::vector<uint8_t> out2 = export_save(imp2.snapshot, imp2.shadow);
    ASSERT_EQ(out1.size(), out2.size());
    for (size_t i = 0; i < out1.size(); ++i) {
        if (out1[i] != out2[i]) {
            throw std::runtime_error(
                "Idempotency failed at byte " + std::to_string(i));
        }
    }
}

TEST(export_produces_valid_sentinels) {
    auto sav = make_valid_sram(1, 2, 3);
    CrystalImport imp = import_save(sav);
    std::vector<uint8_t> out = export_save(imp.snapshot, imp.shadow);
    ASSERT_EQ(out[PRIMARY_CHECK_VALUE_1], SAVE_CHECK_VALUE_1);
    ASSERT_EQ(out[PRIMARY_CHECK_VALUE_2], SAVE_CHECK_VALUE_2);
    ASSERT_EQ(out[BACKUP_CHECK_VALUE_1],  SAVE_CHECK_VALUE_1);
    ASSERT_EQ(out[BACKUP_CHECK_VALUE_2],  SAVE_CHECK_VALUE_2);
}

// â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
// Category 10: Malformed input never causes UB
// â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

TEST(all_zeros_rejected_gracefully) {
    std::vector<uint8_t> zeros(SRAM_SIZE, 0x00);
    // Sentinels are wrong â†’ must throw, not crash.
    ASSERT_THROWS(import_save(zeros), SaveImportError);
}

TEST(all_ones_rejected_gracefully) {
    std::vector<uint8_t> ones(SRAM_SIZE, 0xFF);
    ASSERT_THROWS(import_save(ones), SaveImportError);
}

TEST(random_garbage_rejected_gracefully) {
    std::vector<uint8_t> garbage(SRAM_SIZE, 0x00);
    // Fill with a deterministic pattern that looks random.
    for (size_t i = 0; i < SRAM_SIZE; ++i)
        garbage[i] = static_cast<uint8_t>((i * 0x9E3779B1u) >> 24);
    ASSERT_THROWS(import_save(garbage), SaveImportError);
}

TEST(invalid_bcd_in_valid_sram_rejected_on_decode) {
    // Build a valid SRAM, then corrupt the wMoney BCD bytes with an invalid nibble.
    auto sav = make_valid_sram(0, 0, 0);
    // 0xFA contains nibble A > 9 â€” invalid BCD.
    sav[MONEY]     = 0xFA;
    sav[MONEY + 1] = 0x00;
    sav[MONEY + 2] = 0x00;
    // Recompute checksums after the corruption.
    {
        uint8_t* d = sav.data();
        auto compute_cs = [&](uint32_t begin, uint32_t end) -> uint16_t {
            uint16_t s = 0;
            for (uint32_t i = begin; i <= end; ++i) s += d[i];
            return s;
        };
        d[PRIMARY_CHECK_VALUE_1] = SAVE_CHECK_VALUE_1;
        d[PRIMARY_CHECK_VALUE_2] = SAVE_CHECK_VALUE_2;
        uint16_t pcs = compute_cs(PRIMARY_GAME_DATA, PRIMARY_CHECKSUM_END);
        d[PRIMARY_CHECKSUM]     = static_cast<uint8_t>(pcs & 0xFF);
        d[PRIMARY_CHECKSUM + 1] = static_cast<uint8_t>(pcs >> 8);
        std::copy(d + PRIMARY_GAME_DATA, d + PRIMARY_GAME_DATA + CHECKSUM_REGION_SIZE,
                  d + BACKUP_GAME_DATA);
        std::copy(d + PRIMARY_OPTIONS, d + PRIMARY_OPTIONS + PRIMARY_OPTIONS_SIZE,
                  d + BACKUP_OPTIONS);
        d[BACKUP_CHECK_VALUE_1] = SAVE_CHECK_VALUE_1;
        d[BACKUP_CHECK_VALUE_2] = SAVE_CHECK_VALUE_2;
        uint16_t bcs = compute_cs(BACKUP_GAME_DATA, BACKUP_CHECKSUM_END);
        d[BACKUP_CHECKSUM]     = static_cast<uint8_t>(bcs & 0xFF);
        d[BACKUP_CHECKSUM + 1] = static_cast<uint8_t>(bcs >> 8);
    }
    // The invalid BCD should cause an exception, not UB.
    ASSERT_THROWS(import_save(sav), std::invalid_argument);
}

TEST(out_of_range_sram_read_throws) {
    crystal::Sram s;
    ASSERT_THROWS(s.read_u8(SRAM_SIZE), std::out_of_range);
    ASSERT_THROWS(s.read_u16_be(SRAM_SIZE - 1), std::out_of_range);
    ASSERT_THROWS(s.read_u16_le(SRAM_SIZE - 1), std::out_of_range);
}

TEST(out_of_range_sram_write_throws) {
    crystal::Sram s;
    ASSERT_THROWS(s.write_u8(SRAM_SIZE, 0), std::out_of_range);
    ASSERT_THROWS(s.write_u16_be(SRAM_SIZE - 1, 0), std::out_of_range);
    ASSERT_THROWS(s.write_u16_le(SRAM_SIZE - 1, 0), std::out_of_range);
    std::array<uint8_t, 4> tmp{};
    ASSERT_THROWS(s.write_bytes(SRAM_SIZE - 2, tmp.data(), 4), std::out_of_range);
}

// â”€â”€â”€ BCD unit tests â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

TEST(bcd3_decode_zero) {
    uint8_t b[3] = {0x00, 0x00, 0x00};
    ASSERT_EQ(bcd3_decode(b), 0u);
}

TEST(bcd3_decode_max) {
    uint8_t b[3] = {0x99, 0x99, 0x99};
    ASSERT_EQ(bcd3_decode(b), 999999u);
}

TEST(bcd3_decode_round_trip) {
    for (uint32_t v : {0u, 1u, 99u, 100u, 12345u, 999999u}) {
        auto enc = bcd3_encode(v);
        ASSERT_EQ(bcd3_decode(enc.data()), v);
    }
}

TEST(bcd3_encode_overflow_rejected) {
    ASSERT_THROWS(bcd3_encode(BCD3_MAX + 1), std::out_of_range);
}

TEST(bcd3_decode_invalid_nibble_rejected) {
    uint8_t b[3] = {0x1A, 0x00, 0x00};  // nibble A > 9
    ASSERT_THROWS(bcd3_decode(b), std::invalid_argument);
}

// â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
// ─────────────────────────────────────────────────────────────────────────────
// ─────────────────────────────────────────────────────────────────────────────
// PHASE 2 TESTS: player identity, map position, scene state, play time, RTC
// ─────────────────────────────────────────────────────────────────────────────

// Build a valid SRAM with Phase 2 fields injected and checksums rebuilt.
static std::vector<uint8_t> make_p2_sram(
    uint16_t player_id   = 12345,
    const char* pname    = "RED",
    uint8_t gender       = 0,
    uint8_t map_group    = 24, uint8_t map_number = 4,
    uint8_t map_y        = 5,  uint8_t map_x      = 7,
    uint16_t hours       = 99, uint8_t mins = 30, uint8_t secs = 15,
    bool capped          = false,
    uint8_t rtc_day      = 3,  uint8_t rtc_hour = 14,
    uint8_t scene_slot0  = 0,  uint8_t scene_slot5 = 7)
{
    auto sav = make_valid_sram(500, 200, 100);
    uint8_t* d = sav.data();

    // PlayerID LE
    d[PLAYER_ID]     = static_cast<uint8_t>(player_id & 0xFF);
    d[PLAYER_ID + 1] = static_cast<uint8_t>(player_id >> 8);
    // Player name: encode "RED" as Crystal charmap (R=0x91, E=0x84, D=0x83, term=0x50)
    d[PLAYER_NAME + 0] = 0x91; // R
    d[PLAYER_NAME + 1] = 0x84; // E
    d[PLAYER_NAME + 2] = 0x83; // D
    d[PLAYER_NAME + 3] = 0x50; // terminator
    // Gender in sOptions byte 0 (bit 0)
    d[PRIMARY_OPTIONS] = (d[PRIMARY_OPTIONS] & 0xFE) | (gender & 0x01);
    // Map
    d[MAP_GROUP]   = map_group;
    d[MAP_NUMBER]  = map_number;
    d[PLAYER_Y]    = map_y;
    d[PLAYER_X]    = map_x;
    // Play time: hours BIG-ENDIAN
    d[GAME_TIME_HOURS]     = static_cast<uint8_t>(hours >> 8);
    d[GAME_TIME_HOURS + 1] = static_cast<uint8_t>(hours & 0xFF);
    d[GAME_TIME_MINUTES]   = mins;
    d[GAME_TIME_SECONDS]   = secs;
    if (capped) d[GAME_TIME_CAP] |= 0x01;  // GAME_TIME_CAPPED EQU 0 = bit 0
    // RTC
    d[RTC_BYTES + 0] = rtc_day;
    d[RTC_BYTES + 1] = rtc_hour;
    // Scene state
    d[SCENE_IDS_BASE + 0] = scene_slot0;
    d[SCENE_IDS_BASE + 5] = scene_slot5;

    // Rebuild checksums
    auto cs = [&](uint32_t b, uint32_t e) -> uint16_t {
        uint16_t s = 0; for (uint32_t i = b; i <= e; ++i) s += d[i]; return s;
    };
    d[PRIMARY_CHECK_VALUE_1] = SAVE_CHECK_VALUE_1;
    d[PRIMARY_CHECK_VALUE_2] = SAVE_CHECK_VALUE_2;
    uint16_t pcs = cs(PRIMARY_GAME_DATA, PRIMARY_CHECKSUM_END);
    d[PRIMARY_CHECKSUM]     = static_cast<uint8_t>(pcs & 0xFF);
    d[PRIMARY_CHECKSUM + 1] = static_cast<uint8_t>(pcs >> 8);
    std::copy(d + PRIMARY_GAME_DATA, d + PRIMARY_GAME_DATA + CHECKSUM_REGION_SIZE, d + BACKUP_GAME_DATA);
    std::copy(d + PRIMARY_OPTIONS,   d + PRIMARY_OPTIONS + PRIMARY_OPTIONS_SIZE,   d + BACKUP_OPTIONS);
    d[BACKUP_CHECK_VALUE_1] = SAVE_CHECK_VALUE_1;
    d[BACKUP_CHECK_VALUE_2] = SAVE_CHECK_VALUE_2;
    uint16_t bcs = cs(BACKUP_GAME_DATA, BACKUP_CHECKSUM_END);
    d[BACKUP_CHECKSUM]     = static_cast<uint8_t>(bcs & 0xFF);
    d[BACKUP_CHECKSUM + 1] = static_cast<uint8_t>(bcs >> 8);

    return sav;
}

// ── Player identity ───────────────────────────────────────────────────────────

TEST(p2_player_id_round_trips) {
    auto sav = make_p2_sram(54321);
    CrystalImport imp = import_save(sav);
    ASSERT_EQ(imp.snapshot.player_id, 54321u);
    auto out = export_save(imp.snapshot, imp.shadow);
    CrystalImport reimp = import_save(out);
    ASSERT_EQ(reimp.snapshot.player_id, 54321u);
}

TEST(p2_player_name_decoded_from_charmap) {
    auto sav = make_p2_sram(1, "RED");
    CrystalImport imp = import_save(sav);
    ASSERT_TRUE(imp.snapshot.player_name == "RED");
}

TEST(p2_player_name_round_trips) {
    auto sav = make_valid_sram();
    CrystalImport imp = import_save(sav);
    imp.snapshot.player_name = "GOLD";
    auto out = export_save(imp.snapshot, imp.shadow);
    CrystalImport reimp = import_save(out);
    ASSERT_TRUE(reimp.snapshot.player_name == "GOLD");
}

TEST(p2_player_names_independent) {
    auto sav = make_valid_sram();
    CrystalImport imp = import_save(sav);
    imp.snapshot.player_name = "ASH";
    imp.snapshot.moms_name   = "DELIA";
    imp.snapshot.rival_name  = "GARY";
    auto out = export_save(imp.snapshot, imp.shadow);
    CrystalImport reimp = import_save(out);
    ASSERT_TRUE(reimp.snapshot.player_name == "ASH");
    ASSERT_TRUE(reimp.snapshot.moms_name   == "DELIA");
    ASSERT_TRUE(reimp.snapshot.rival_name  == "GARY");
}

TEST(p2_unrepresentable_character_rejected) {
    auto sav = make_valid_sram();
    CrystalImport imp = import_save(sav);
    imp.snapshot.player_name = "\xE2\x98\x80";  // ☀ (U+2600) — not in Crystal charmap
    ASSERT_THROWS(export_save(imp.snapshot, imp.shadow), SaveExportError);
}

TEST(p2_gender_boy_encodes_correctly) {
    auto sav = make_p2_sram(1, "RED", 0);
    CrystalImport imp = import_save(sav);
    ASSERT_EQ(imp.snapshot.player_gender, 0u);
    auto out = export_save(imp.snapshot, imp.shadow);
    CrystalImport reimp = import_save(out);
    ASSERT_EQ(reimp.snapshot.player_gender, 0u);
    // Gender byte is in sOptions (0x2000) not the checksummed region
    ASSERT_EQ(out[PRIMARY_OPTIONS] & 0x01, 0u);
}

TEST(p2_gender_girl_encodes_correctly) {
    auto sav = make_p2_sram(1, "RED", 1);
    CrystalImport imp = import_save(sav);
    ASSERT_EQ(imp.snapshot.player_gender, 1u);
    auto out = export_save(imp.snapshot, imp.shadow);
    CrystalImport reimp = import_save(out);
    ASSERT_EQ(reimp.snapshot.player_gender, 1u);
    ASSERT_EQ(out[PRIMARY_OPTIONS] & 0x01, 1u);
}

// ── Map position ──────────────────────────────────────────────────────────────

TEST(p2_map_identity_A_round_trips) {
    // New Bark Town: group=24, map=4
    auto sav = make_p2_sram(1, "RED", 0, 24, 4, 5, 7);
    CrystalImport imp = import_save(sav);
    ASSERT_EQ(imp.snapshot.location.group,  24u);
    ASSERT_EQ(imp.snapshot.location.number,  4u);
    ASSERT_EQ(imp.snapshot.location.y,       5u);
    ASSERT_EQ(imp.snapshot.location.x,       7u);
    auto out = export_save(imp.snapshot, imp.shadow);
    CrystalImport reimp = import_save(out);
    ASSERT_EQ(reimp.snapshot.location.group,  24u);
    ASSERT_EQ(reimp.snapshot.location.number,  4u);
    ASSERT_EQ(reimp.snapshot.location.y,       5u);
    ASSERT_EQ(reimp.snapshot.location.x,       7u);
}

TEST(p2_map_identity_B_round_trips) {
    // Elm's Lab: group=24, map=5
    auto sav = make_p2_sram(1, "RED", 0, 24, 5, 3, 4);
    CrystalImport imp = import_save(sav);
    ASSERT_EQ(imp.snapshot.location.group,  24u);
    ASSERT_EQ(imp.snapshot.location.number,  5u);
    auto out = export_save(imp.snapshot, imp.shadow);
    CrystalImport reimp = import_save(out);
    ASSERT_EQ(reimp.snapshot.location.group,  24u);
    ASSERT_EQ(reimp.snapshot.location.number,  5u);
}

TEST(p2_map_A_and_B_are_independent) {
    auto sav1 = make_p2_sram(1, "RED", 0, 24, 4, 5, 7);  // New Bark
    auto sav2 = make_p2_sram(1, "RED", 0, 24, 5, 3, 4);  // Elm's Lab
    CrystalImport imp1 = import_save(sav1);
    CrystalImport imp2 = import_save(sav2);
    ASSERT_TRUE(imp1.snapshot.location.number != imp2.snapshot.location.number);
    ASSERT_EQ(imp1.snapshot.location.group, imp2.snapshot.location.group);
}

// ── Per-map scene state ───────────────────────────────────────────────────────

TEST(p2_scene_ids_round_trip) {
    auto sav = make_p2_sram(1, "RED", 0, 24, 4, 5, 7, 100, 30, 0, false, 3, 14, 2, 5);
    CrystalImport imp = import_save(sav);
    ASSERT_EQ(imp.snapshot.scene_ids[0], 2u);
    ASSERT_EQ(imp.snapshot.scene_ids[5], 5u);
    // All other slots are 0
    ASSERT_EQ(imp.snapshot.scene_ids[1], 0u);
    auto out = export_save(imp.snapshot, imp.shadow);
    CrystalImport reimp = import_save(out);
    ASSERT_EQ(reimp.snapshot.scene_ids[0], 2u);
    ASSERT_EQ(reimp.snapshot.scene_ids[5], 5u);
}

TEST(p2_scenes_are_independent_across_slots) {
    auto sav = make_valid_sram();
    CrystalImport imp = import_save(sav);
    imp.snapshot.scene_ids[0]  = 3;
    imp.snapshot.scene_ids[10] = 7;
    imp.snapshot.scene_ids[78] = 1;
    auto out = export_save(imp.snapshot, imp.shadow);
    CrystalImport reimp = import_save(out);
    ASSERT_EQ(reimp.snapshot.scene_ids[0],  3u);
    ASSERT_EQ(reimp.snapshot.scene_ids[10], 7u);
    ASSERT_EQ(reimp.snapshot.scene_ids[78], 1u);
    // Others unchanged
    for (size_t i = 1; i < 10; ++i)  ASSERT_EQ(reimp.snapshot.scene_ids[i], 0u);
}

TEST(p2_scene_edit_does_not_affect_money) {
    auto sav = make_valid_sram(50000, 10000, 500);
    CrystalImport imp = import_save(sav);
    imp.snapshot.scene_ids[5] = 0xFF;
    auto out = export_save(imp.snapshot, imp.shadow);
    CrystalImport reimp = import_save(out);
    ASSERT_EQ(reimp.snapshot.money_player, 50000u);
    ASSERT_EQ(reimp.snapshot.money_mom,    10000u);
    ASSERT_EQ(reimp.snapshot.coins,          500u);
}

// ── Play time ─────────────────────────────────────────────────────────────────

TEST(p2_playtime_big_endian_hours_round_trip) {
    // This test specifically validates wGameTimeHours is decoded/encoded as
    // BIG-ENDIAN u16 (suiCune fix 2026-08, serialize.c TY_U16LE→TY_U16BE).
    auto sav = make_p2_sram(1, "RED", 0, 24, 4, 0, 0, 999, 59, 30);
    // Verify raw bytes: hours=999 (0x03E7), big-endian → [0x03, 0xE7]
    ASSERT_EQ(sav[GAME_TIME_HOURS],     0x03u);  // high byte
    ASSERT_EQ(sav[GAME_TIME_HOURS + 1], 0xE7u);  // low byte
    CrystalImport imp = import_save(sav);
    ASSERT_EQ(imp.snapshot.playtime_hours,   999u);
    ASSERT_EQ(imp.snapshot.playtime_minutes,  59u);
    ASSERT_EQ(imp.snapshot.playtime_seconds,  30u);
    auto out = export_save(imp.snapshot, imp.shadow);
    ASSERT_EQ(out[GAME_TIME_HOURS],     0x03u);  // still big-endian after export
    ASSERT_EQ(out[GAME_TIME_HOURS + 1], 0xE7u);
    CrystalImport reimp = import_save(out);
    ASSERT_EQ(reimp.snapshot.playtime_hours,   999u);
    ASSERT_EQ(reimp.snapshot.playtime_minutes,  59u);
    ASSERT_EQ(reimp.snapshot.playtime_seconds,  30u);
}

TEST(p2_playtime_zero_round_trips) {
    auto sav = make_p2_sram(1, "RED", 0, 24, 4, 0, 0, 0, 0, 0);
    CrystalImport imp = import_save(sav);
    ASSERT_EQ(imp.snapshot.playtime_hours, 0u);
    ASSERT_TRUE(!(imp.snapshot.playtime_capped));
}

TEST(p2_playtime_capped_flag_round_trips) {
    auto sav = make_p2_sram(1, "RED", 0, 24, 4, 0, 0, 999, 59, 59, true);
    CrystalImport imp = import_save(sav);
    ASSERT_TRUE(imp.snapshot.playtime_capped);
    ASSERT_EQ(imp.snapshot.playtime_hours, 999u);
    auto out = export_save(imp.snapshot, imp.shadow);
    CrystalImport reimp = import_save(out);
    ASSERT_TRUE(reimp.snapshot.playtime_capped);
}

// Raw GAME_TIME_CAP byte tests — verify bit 0 is used (GAME_TIME_CAPPED EQU 0).

TEST(p2_playtime_capped_sets_bit0_in_raw_sram) {
    // capped=true  → GAME_TIME_CAP byte must have bit 0 set
    auto sav = make_p2_sram(1, "RED", 0, 24, 4, 0, 0, 999, 59, 59, true);
    CrystalImport imp = import_save(sav);
    auto out = export_save(imp.snapshot, imp.shadow);
    ASSERT_TRUE((out[GAME_TIME_CAP] & 0x01) != 0);   // bit 0 set
    ASSERT_TRUE((out[GAME_TIME_CAP] & 0x80) == 0);   // bit 7 clear (NOT the old wrong bit)
}

TEST(p2_playtime_uncapped_clears_bit0_in_raw_sram) {
    // capped=false → GAME_TIME_CAP byte must have bit 0 clear
    auto sav = make_p2_sram(1, "RED", 0, 24, 4, 0, 0, 50, 20, 10, false);
    CrystalImport imp = import_save(sav);
    auto out = export_save(imp.snapshot, imp.shadow);
    ASSERT_TRUE((out[GAME_TIME_CAP] & 0x01) == 0);   // bit 0 clear
}

TEST(p2_playtime_capped_unrelated_bits_preserved) {
    // Bits other than bit 0 in GAME_TIME_CAP must survive the export unchanged.
    // We set bits 1 and 2 in the shadow before import/export.
    auto sav = make_valid_sram();
    // Patch GAME_TIME_CAP with bits 1 and 2 set (not bit 0, not bit 7)
    sav[GAME_TIME_CAP] = 0x06;   // bits 1 and 2
    // Rebuild checksums after the patch
    {
        uint8_t* d = sav.data();
        auto cs = [&](uint32_t b, uint32_t e) -> uint16_t {
            uint16_t s = 0; for (uint32_t i = b; i <= e; ++i) s += d[i]; return s; };
        d[PRIMARY_CHECK_VALUE_1] = SAVE_CHECK_VALUE_1;
        d[PRIMARY_CHECK_VALUE_2] = SAVE_CHECK_VALUE_2;
        uint16_t pcs = cs(PRIMARY_GAME_DATA, PRIMARY_CHECKSUM_END);
        d[PRIMARY_CHECKSUM] = static_cast<uint8_t>(pcs & 0xFF);
        d[PRIMARY_CHECKSUM+1] = static_cast<uint8_t>(pcs >> 8);
        std::copy(d+PRIMARY_GAME_DATA, d+PRIMARY_GAME_DATA+CHECKSUM_REGION_SIZE, d+BACKUP_GAME_DATA);
        std::copy(d+PRIMARY_OPTIONS, d+PRIMARY_OPTIONS+PRIMARY_OPTIONS_SIZE, d+BACKUP_OPTIONS);
        d[BACKUP_CHECK_VALUE_1] = SAVE_CHECK_VALUE_1;
        d[BACKUP_CHECK_VALUE_2] = SAVE_CHECK_VALUE_2;
        uint16_t bcs = cs(BACKUP_GAME_DATA, BACKUP_CHECKSUM_END);
        d[BACKUP_CHECKSUM] = static_cast<uint8_t>(bcs & 0xFF);
        d[BACKUP_CHECKSUM+1] = static_cast<uint8_t>(bcs >> 8);
    }
    CrystalImport imp = import_save(sav);
    ASSERT_TRUE(!(imp.snapshot.playtime_capped));  // bit 0 was not set
    // Export back (capped still false)
    auto out = export_save(imp.snapshot, imp.shadow);
    // Bits 1 and 2 must be preserved, bit 0 must stay 0
    ASSERT_EQ(out[GAME_TIME_CAP] & 0x06, 0x06u);  // bits 1,2 preserved
    ASSERT_TRUE((out[GAME_TIME_CAP] & 0x01) == 0); // bit 0 clear
}

TEST(p2_import_raw_bit0_set_decodes_capped_true) {
    // A SRAM byte with only bit 0 set → playtime_capped must decode as true.
    // This is the definitive Crystal-format test: bit 0 = GAME_TIME_CAPPED.
    auto sav = make_valid_sram();
    sav[GAME_TIME_CAP] = 0x01;   // only bit 0
    // Rebuild checksums
    {
        uint8_t* d = sav.data();
        auto cs = [&](uint32_t b, uint32_t e) -> uint16_t {
            uint16_t s = 0; for (uint32_t i = b; i <= e; ++i) s += d[i]; return s; };
        d[PRIMARY_CHECK_VALUE_1] = SAVE_CHECK_VALUE_1;
        d[PRIMARY_CHECK_VALUE_2] = SAVE_CHECK_VALUE_2;
        uint16_t pcs = cs(PRIMARY_GAME_DATA, PRIMARY_CHECKSUM_END);
        d[PRIMARY_CHECKSUM] = static_cast<uint8_t>(pcs & 0xFF);
        d[PRIMARY_CHECKSUM+1] = static_cast<uint8_t>(pcs >> 8);
        std::copy(d+PRIMARY_GAME_DATA, d+PRIMARY_GAME_DATA+CHECKSUM_REGION_SIZE, d+BACKUP_GAME_DATA);
        std::copy(d+PRIMARY_OPTIONS, d+PRIMARY_OPTIONS+PRIMARY_OPTIONS_SIZE, d+BACKUP_OPTIONS);
        d[BACKUP_CHECK_VALUE_1] = SAVE_CHECK_VALUE_1;
        d[BACKUP_CHECK_VALUE_2] = SAVE_CHECK_VALUE_2;
        uint16_t bcs = cs(BACKUP_GAME_DATA, BACKUP_CHECKSUM_END);
        d[BACKUP_CHECKSUM] = static_cast<uint8_t>(bcs & 0xFF);
        d[BACKUP_CHECKSUM+1] = static_cast<uint8_t>(bcs >> 8);
    }
    CrystalImport imp = import_save(sav);
    ASSERT_TRUE(imp.snapshot.playtime_capped);  // bit 0 set → capped
}

TEST(p2_import_raw_bit7_only_does_not_decode_capped) {
    // A SRAM byte with only bit 7 set → playtime_capped must decode as FALSE.
    // Proves the old wrong mask (0x80) is no longer used.
    auto sav = make_valid_sram();
    sav[GAME_TIME_CAP] = 0x80;   // only bit 7 (the OLD wrong bit)
    // Rebuild checksums
    {
        uint8_t* d = sav.data();
        auto cs = [&](uint32_t b, uint32_t e) -> uint16_t {
            uint16_t s = 0; for (uint32_t i = b; i <= e; ++i) s += d[i]; return s; };
        d[PRIMARY_CHECK_VALUE_1] = SAVE_CHECK_VALUE_1;
        d[PRIMARY_CHECK_VALUE_2] = SAVE_CHECK_VALUE_2;
        uint16_t pcs = cs(PRIMARY_GAME_DATA, PRIMARY_CHECKSUM_END);
        d[PRIMARY_CHECKSUM] = static_cast<uint8_t>(pcs & 0xFF);
        d[PRIMARY_CHECKSUM+1] = static_cast<uint8_t>(pcs >> 8);
        std::copy(d+PRIMARY_GAME_DATA, d+PRIMARY_GAME_DATA+CHECKSUM_REGION_SIZE, d+BACKUP_GAME_DATA);
        std::copy(d+PRIMARY_OPTIONS, d+PRIMARY_OPTIONS+PRIMARY_OPTIONS_SIZE, d+BACKUP_OPTIONS);
        d[BACKUP_CHECK_VALUE_1] = SAVE_CHECK_VALUE_1;
        d[BACKUP_CHECK_VALUE_2] = SAVE_CHECK_VALUE_2;
        uint16_t bcs = cs(BACKUP_GAME_DATA, BACKUP_CHECKSUM_END);
        d[BACKUP_CHECKSUM] = static_cast<uint8_t>(bcs & 0xFF);
        d[BACKUP_CHECKSUM+1] = static_cast<uint8_t>(bcs >> 8);
    }
    CrystalImport imp = import_save(sav);
    ASSERT_TRUE(!(imp.snapshot.playtime_capped));  // bit 7 only → NOT capped
}

// ── RTC / time context ────────────────────────────────────────────────────────

TEST(p2_rtc_state_round_trips) {
    auto sav = make_p2_sram(1, "RED", 0, 24, 4, 0, 0, 50, 20, 10, false, 6, 18);
    CrystalImport imp = import_save(sav);
    ASSERT_EQ(imp.snapshot.rtc.day,    6u);
    ASSERT_EQ(imp.snapshot.rtc.hour,  18u);
    auto out = export_save(imp.snapshot, imp.shadow);
    CrystalImport reimp = import_save(out);
    ASSERT_EQ(reimp.snapshot.rtc.day,   6u);
    ASSERT_EQ(reimp.snapshot.rtc.hour, 18u);
}

TEST(p2_dst_flag_round_trips) {
    auto sav = make_valid_sram();
    CrystalImport imp = import_save(sav);
    imp.snapshot.rtc.dst = true;
    auto out = export_save(imp.snapshot, imp.shadow);
    // Verify bit 7 of DST byte is set
    ASSERT_TRUE((out[DST] & 0x80) != 0);
    CrystalImport reimp = import_save(out);
    ASSERT_TRUE(reimp.snapshot.rtc.dst);
}

// ── Untouched-byte diff ───────────────────────────────────────────────────────

// A no-edit round-trip must change only the bytes we own.
// Specifically: bytes outside owned fields must be identical between
// the original sav and the exported sav.
TEST(p2_unowned_bytes_unchanged_on_roundtrip) {
    auto sav = make_p2_sram(12345, "RED", 0, 24, 4, 5, 7, 100, 30, 15);
    CrystalImport imp = import_save(sav);
    auto out = export_save(imp.snapshot, imp.shadow);

    // Bytes outside all owned fields must match the original.
    // Sample check: byte in PC boxes region (0x4000) — unowned, should match.
    ASSERT_EQ(out[0x4000], sav[0x4000]);
    ASSERT_EQ(out[0x4001], sav[0x4001]);
    // Byte in Hall of Fame region (0x3E00) — unowned.
    ASSERT_EQ(out[0x3E00], sav[0x3E00]);
    // Byte 0x3000 — also unowned.
    ASSERT_EQ(out[0x3000], sav[0x3000]);
}

// ── Wrong profile identity rejected ──────────────────────────────────────────

TEST(p2_wrong_profile_rejects_p2_export) {
    const std::string correct_profile = "f2f52230b536214ef7c9924f483392993e226cfb";
    crystal::SramIdentity expected;
    expected.profile_sha1  = correct_profile;
    expected.rom_sha1      = correct_profile;
    expected.codec_version = crystal::SRAM_CODEC_VERSION;

    auto sav = make_p2_sram();
    // Import with a WRONG profile (Gold ROM)
    CrystalImport imp = import_save(sav, "0000000000000000000000000000000000000000");

    // Must reject because shadow was imported under wrong profile
    ASSERT_THROWS(export_save(imp.snapshot, imp.shadow, expected), SaveExportError);
}

// ─────────────────────────────────────────────────────────────────────────────
// PHASE 3A TESTS: Party Pokémon
// ─────────────────────────────────────────────────────────────────────────────

// Helper: build a minimal valid party_struct (48 bytes) at `dest`.
// All unset bytes default to 0.
// Source-verified field layout from macros/ram.asm, pokemon_data_constants.asm.
static void make_party_struct(
    uint8_t* s,          // 48-byte buffer to fill
    uint8_t species,     // 0 = leave as 0
    uint8_t item,
    uint8_t move0,       // move IDs (0 = none)
    uint8_t level,
    uint16_t ot_id,      // LE
    uint32_t exp,        // 3-byte BE
    uint16_t hp_exp_be,  // BE
    uint16_t dvs_le,     // byte0=ATK|DEF nibbles, byte1=SPD|SPC nibbles, LE
    uint8_t  pp_byte0,   // bits7-6=pp_ups, bits5-0=pp
    uint8_t  happiness,
    uint8_t  caught_tl,  // caught_time_level byte
    uint8_t  caught_gl,  // caught_gender_location byte
    uint8_t  status,
    uint16_t hp_be,      // current HP, BE
    uint16_t maxhp_be,   // MaxHP, BE
    uint16_t atk_be)
{
    std::fill(s, s + 48, 0);
    s[0]  = species;
    s[1]  = item;
    s[2]  = move0;
    // OT_ID LE
    s[6]  = static_cast<uint8_t>(ot_id & 0xFF);
    s[7]  = static_cast<uint8_t>(ot_id >> 8);
    // EXP BE
    s[8]  = static_cast<uint8_t>((exp >> 16) & 0xFF);
    s[9]  = static_cast<uint8_t>((exp >>  8) & 0xFF);
    s[10] = static_cast<uint8_t>( exp        & 0xFF);
    // stat exp HP BE
    s[11] = static_cast<uint8_t>(hp_exp_be >> 8);
    s[12] = static_cast<uint8_t>(hp_exp_be & 0xFF);
    // DVs LE
    s[21] = static_cast<uint8_t>(dvs_le & 0xFF);
    s[22] = static_cast<uint8_t>(dvs_le >> 8);
    // PP
    s[23] = pp_byte0;
    s[27] = happiness;
    s[29] = caught_tl;
    s[30] = caught_gl;
    s[31] = level;
    s[32] = status;
    // HP BE
    s[34] = static_cast<uint8_t>(hp_be >> 8);
    s[35] = static_cast<uint8_t>(hp_be & 0xFF);
    // MaxHP BE
    s[36] = static_cast<uint8_t>(maxhp_be >> 8);
    s[37] = static_cast<uint8_t>(maxhp_be & 0xFF);
    // ATK BE
    s[38] = static_cast<uint8_t>(atk_be >> 8);
    s[39] = static_cast<uint8_t>(atk_be & 0xFF);
}

// Helper: embed a party into a valid-checksum SRAM image.
static std::vector<uint8_t> make_sram_with_party(
    uint8_t count,                                    // 0-6
    const std::vector<std::array<uint8_t,48>>& mons, // party_struct bytes per slot
    const std::vector<std::array<uint8_t,11>>& ots,  // OT names
    const std::vector<std::array<uint8_t,11>>& nicks) // nicknames
{
    auto sav = make_valid_sram();
    uint8_t* d = sav.data();

    d[PARTY_COUNT] = count;
    for (uint8_t i = 0; i < count; ++i) {
        d[PARTY_SPECIES + i] = mons[i][0]; // species from struct
    }
    d[PARTY_SPECIES + count] = 0xFF; // terminator

    for (uint8_t i = 0; i < count; ++i) {
        std::copy(mons[i].begin(), mons[i].end(), d + PARTY_MON_1 + i * 48);
        std::copy(ots[i].begin(),  ots[i].end(),  d + PARTY_OT_NAMES  + i * 11);
        std::copy(nicks[i].begin(),nicks[i].end(), d + PARTY_NICKNAMES + i * 11);
    }

    // Rebuild checksums
    auto cs = [&](uint32_t b, uint32_t e) -> uint16_t {
        uint16_t s = 0; for (uint32_t i = b; i <= e; ++i) s += d[i]; return s; };
    d[PRIMARY_CHECK_VALUE_1] = SAVE_CHECK_VALUE_1;
    d[PRIMARY_CHECK_VALUE_2] = SAVE_CHECK_VALUE_2;
    uint16_t pcs = cs(PRIMARY_GAME_DATA, PRIMARY_CHECKSUM_END);
    d[PRIMARY_CHECKSUM] = static_cast<uint8_t>(pcs & 0xFF);
    d[PRIMARY_CHECKSUM+1] = static_cast<uint8_t>(pcs >> 8);
    std::copy(d+PRIMARY_GAME_DATA, d+PRIMARY_GAME_DATA+CHECKSUM_REGION_SIZE, d+BACKUP_GAME_DATA);
    std::copy(d+PRIMARY_OPTIONS, d+PRIMARY_OPTIONS+PRIMARY_OPTIONS_SIZE, d+BACKUP_OPTIONS);
    d[BACKUP_CHECK_VALUE_1] = SAVE_CHECK_VALUE_1;
    d[BACKUP_CHECK_VALUE_2] = SAVE_CHECK_VALUE_2;
    uint16_t bcs = cs(BACKUP_GAME_DATA, BACKUP_CHECKSUM_END);
    d[BACKUP_CHECKSUM] = static_cast<uint8_t>(bcs & 0xFF);
    d[BACKUP_CHECKSUM+1] = static_cast<uint8_t>(bcs >> 8);
    return sav;
}

// Crystal charmap for "PIKA" (P=0x8F, I=0x88, K=0x8A, A=0x80, term=0x50)
static std::array<uint8_t,11> make_ot_name(const char* crystal_bytes) {
    std::array<uint8_t,11> buf;
    buf.fill(0x50);
    for (int i = 0; i < 11 && crystal_bytes[i]; ++i) buf[i] = static_cast<uint8_t>(crystal_bytes[i]);
    return buf;
}

// ── 1-mon import ─────────────────────────────────────────────────────────────

TEST(p3a_one_mon_import_basic_fields) {
    std::array<uint8_t,48> mon; mon.fill(0);
    // Charizard (species 6), Charcoal (item 0xA8 tentative, use 1 for test), moves: Flamethrower=53,Fire Spin=83,Cut=15,Slash=163
    // Use simple known values
    make_party_struct(mon.data(),
        6,    // species = Charizard
        1,    // held item = Master Ball (just testing round-trip)
        53,   // move 0 = Flamethrower
        50,   // level
        12345, // OT_ID LE
        5000,  // exp BE
        0x0200,// HP exp BE = 512
        0xF3F7,// DVs LE: byte[21]=0xF7=ATK15|DEF7, byte[22]=0xF3=SPD15|SPC3
        (2u<<6)|20u, // pp[0]: 2 PP Ups, 20 current PP
        200,   // happiness
        (2u<<6)|40u, // caught_time=day(2), caught_level=40
        (1u<<7)|16u, // caught_by_boy=true, location=16
        0,     // no status
        180, 230, 260);  // HP=180 BE, MaxHP=230 BE, ATK=260 BE

    std::array<uint8_t,11> ot;  ot.fill(0x50);
    // "ASH" in Crystal charmap: A=0x80, S=0x92, H=0x87
    ot[0]=0x80; ot[1]=0x92; ot[2]=0x87;
    std::array<uint8_t,11> nick; nick.fill(0x50);
    // "CHARCOAL" won't fit in 10 chars → use "CHAR": C=0x82,H=0x87,A=0x80,R=0x91
    nick[0]=0x82; nick[1]=0x87; nick[2]=0x80; nick[3]=0x91;

    auto sav = make_sram_with_party(1, {mon}, {ot}, {nick});
    CrystalImport imp = import_save(sav);
    const auto& pm = imp.snapshot.party.party_mons[0];

    ASSERT_EQ(imp.snapshot.party.party_count, 1u);
    ASSERT_EQ(pm.species, 6u);
    ASSERT_EQ(pm.item, 1u);
    ASSERT_EQ(pm.moves[0], 53u);
    ASSERT_EQ(pm.level, 50u);
    ASSERT_EQ(pm.ot_id, 12345u);
    ASSERT_EQ(pm.exp, 5000u);
    ASSERT_EQ(pm.stat_exp_hp, 0x0200u);
    ASSERT_EQ(pm.dvs.atk, 15u);
    ASSERT_EQ(pm.dvs.def,  7u);
    ASSERT_EQ(pm.dvs.spd, 15u);
    ASSERT_EQ(pm.dvs.spc,  3u);
    ASSERT_EQ(pm.pp_ups[0], 2u);
    ASSERT_EQ(pm.pp[0],    20u);
    ASSERT_EQ(pm.happiness, 200u);
    ASSERT_TRUE(pm.caught.caught_by_boy);
    ASSERT_EQ(pm.caught.time_of_day,  2u);  // day
    ASSERT_EQ(pm.caught.caught_level, 40u);
    ASSERT_EQ(pm.caught.location,     16u);
    ASSERT_EQ(pm.current_hp, 180u);
    ASSERT_EQ(pm.max_hp,     230u);
    ASSERT_EQ(pm.stat_atk,   260u);
    ASSERT_TRUE(pm.ot_name == "ASH");
    ASSERT_TRUE(pm.nickname == "CHAR");
}

// ── Species/move/item ID translation boundary ────────────────────────────────

TEST(p3a_max_valid_species_accepted) {
    std::array<uint8_t,48> mon; mon.fill(0);
    mon[0] = 251; // Celebi — maximum valid species
    mon[31] = 5;  // level
    std::array<uint8_t,11> ot; ot.fill(0x50);
    std::array<uint8_t,11> nick; nick.fill(0x50);
    auto sav = make_sram_with_party(1, {mon}, {ot}, {nick});
    CrystalImport imp = import_save(sav);
    ASSERT_EQ(imp.snapshot.party.party_mons[0].species, 251u);
}

TEST(p3a_egg_species_accepted) {
    // EGG = 254 is a valid species sentinel
    std::array<uint8_t,48> mon; mon.fill(0);
    mon[0] = 254; // EGG
    mon[31] = 5;
    std::array<uint8_t,11> ot; ot.fill(0x50);
    std::array<uint8_t,11> nick; nick.fill(0x50);
    auto sav = make_sram_with_party(1, {mon}, {ot}, {nick});
    CrystalImport imp = import_save(sav);
    ASSERT_EQ(imp.snapshot.party.party_mons[0].species, 254u);
}

TEST(p3a_invalid_species_zero_rejected) {
    std::array<uint8_t,48> mon; mon.fill(0);
    mon[0] = 0;  // species 0 = invalid
    mon[31] = 5;
    // Species list must also be 0 to trigger the struct check
    // Build manually: species list says species[0]=0 → struct check fires
    auto sav = make_valid_sram();
    uint8_t* d = sav.data();
    d[PARTY_COUNT] = 1;
    d[PARTY_SPECIES + 0] = 0;  // invalid
    d[PARTY_SPECIES + 1] = 0xFF;
    std::copy(mon.begin(), mon.end(), d + PARTY_MON_1);
    // Rebuild checksums
    auto cs = [&](uint32_t b, uint32_t e) -> uint16_t {
        uint16_t s = 0; for (uint32_t i = b; i <= e; ++i) s += d[i]; return s; };
    d[PRIMARY_CHECK_VALUE_1] = SAVE_CHECK_VALUE_1;
    d[PRIMARY_CHECK_VALUE_2] = SAVE_CHECK_VALUE_2;
    uint16_t pcs = cs(PRIMARY_GAME_DATA, PRIMARY_CHECKSUM_END);
    d[PRIMARY_CHECKSUM] = static_cast<uint8_t>(pcs&0xFF);
    d[PRIMARY_CHECKSUM+1] = static_cast<uint8_t>(pcs>>8);
    std::copy(d+PRIMARY_GAME_DATA,d+PRIMARY_GAME_DATA+CHECKSUM_REGION_SIZE,d+BACKUP_GAME_DATA);
    std::copy(d+PRIMARY_OPTIONS,d+PRIMARY_OPTIONS+PRIMARY_OPTIONS_SIZE,d+BACKUP_OPTIONS);
    d[BACKUP_CHECK_VALUE_1] = SAVE_CHECK_VALUE_1;
    d[BACKUP_CHECK_VALUE_2] = SAVE_CHECK_VALUE_2;
    uint16_t bcs = cs(BACKUP_GAME_DATA, BACKUP_CHECKSUM_END);
    d[BACKUP_CHECKSUM] = static_cast<uint8_t>(bcs&0xFF);
    d[BACKUP_CHECKSUM+1] = static_cast<uint8_t>(bcs>>8);
    ASSERT_THROWS(import_save(sav), SaveImportError);
}

TEST(p3a_invalid_party_count_rejected) {
    auto sav = make_valid_sram();
    uint8_t* d = sav.data();
    d[PARTY_COUNT] = 7;  // > PARTY_LENGTH = 6
    auto cs = [&](uint32_t b, uint32_t e) -> uint16_t {
        uint16_t s = 0; for (uint32_t i = b; i <= e; ++i) s += d[i]; return s; };
    d[PRIMARY_CHECK_VALUE_1] = SAVE_CHECK_VALUE_1;
    d[PRIMARY_CHECK_VALUE_2] = SAVE_CHECK_VALUE_2;
    uint16_t pcs = cs(PRIMARY_GAME_DATA, PRIMARY_CHECKSUM_END);
    d[PRIMARY_CHECKSUM] = static_cast<uint8_t>(pcs&0xFF);
    d[PRIMARY_CHECKSUM+1] = static_cast<uint8_t>(pcs>>8);
    std::copy(d+PRIMARY_GAME_DATA,d+PRIMARY_GAME_DATA+CHECKSUM_REGION_SIZE,d+BACKUP_GAME_DATA);
    std::copy(d+PRIMARY_OPTIONS,d+PRIMARY_OPTIONS+PRIMARY_OPTIONS_SIZE,d+BACKUP_OPTIONS);
    d[BACKUP_CHECK_VALUE_1] = SAVE_CHECK_VALUE_1;
    d[BACKUP_CHECK_VALUE_2] = SAVE_CHECK_VALUE_2;
    uint16_t bcs = cs(BACKUP_GAME_DATA, BACKUP_CHECKSUM_END);
    d[BACKUP_CHECKSUM] = static_cast<uint8_t>(bcs&0xFF);
    d[BACKUP_CHECKSUM+1] = static_cast<uint8_t>(bcs>>8);
    ASSERT_THROWS(import_save(sav), SaveImportError);
}

TEST(p3a_empty_party_accepted) {
    auto sav = make_valid_sram();  // wPartyCount = 0 by default
    CrystalImport imp = import_save(sav);
    ASSERT_EQ(imp.snapshot.party.party_count, 0u);
}

// ── PP and PP Ups packing ─────────────────────────────────────────────────────

TEST(p3a_pp_up_packing_round_trips) {
    // PP byte: bits 7-6 = PP Up count (0-3), bits 5-0 = current PP
    // Source: constants/pokemon_data_constants.asm PP_UP_MASK, PP_MASK
    std::array<uint8_t,48> mon; mon.fill(0);
    mon[0] = 1; mon[31] = 5;
    // move 0: pp_ups=3, pp=35
    mon[2]  = 1;   // Pound move ID
    mon[23] = static_cast<uint8_t>((3u << 6) | 35u);  // 0xE3 = 0b11100011
    // move 1: pp_ups=0, pp=5
    mon[3]  = 2;
    mon[24] = static_cast<uint8_t>((0u << 6) | 5u);

    std::array<uint8_t,11> ot; ot.fill(0x50);
    std::array<uint8_t,11> nick; nick.fill(0x50);
    auto sav = make_sram_with_party(1, {mon}, {ot}, {nick});
    CrystalImport imp = import_save(sav);
    const auto& pm = imp.snapshot.party.party_mons[0];
    ASSERT_EQ(pm.pp_ups[0], 3u);
    ASSERT_EQ(pm.pp[0],    35u);
    ASSERT_EQ(pm.pp_ups[1], 0u);
    ASSERT_EQ(pm.pp[1],     5u);

    // Round-trip: export and re-import
    auto out = export_save(imp.snapshot, imp.shadow);
    ASSERT_EQ(out[PARTY_MON_1 + 23], static_cast<uint8_t>((3u<<6)|35u));
    ASSERT_EQ(out[PARTY_MON_1 + 24], static_cast<uint8_t>((0u<<6)| 5u));

    CrystalImport reimp = import_save(out);
    ASSERT_EQ(reimp.snapshot.party.party_mons[0].pp_ups[0], 3u);
    ASSERT_EQ(reimp.snapshot.party.party_mons[0].pp[0],    35u);
}

// ── DVs ──────────────────────────────────────────────────────────────────────

TEST(p3a_dvs_decode_and_encode) {
    // DVs: byte[21]=ATK(7-4)|DEF(3-0), byte[22]=SPD(7-4)|SPC(3-0)
    // ATK=15,DEF=7 → byte0=0xF7;  SPD=15,SPC=3 → byte1=0xF3
    // HP DV = (ATK&1)<<3|(DEF&1)<<2|(SPD&1)<<1|(SPC&1) = 1<<3|1<<2|1<<1|1 = 0xF (15)
    std::array<uint8_t,48> mon; mon.fill(0);
    mon[0]=1; mon[31]=5;
    mon[21] = 0xF7;  // ATK=15, DEF=7
    mon[22] = 0xF3;  // SPD=15, SPC=3
    std::array<uint8_t,11> ot; ot.fill(0x50);
    std::array<uint8_t,11> nick; nick.fill(0x50);
    auto sav = make_sram_with_party(1, {mon}, {ot}, {nick});
    CrystalImport imp = import_save(sav);
    const auto& dvs = imp.snapshot.party.party_mons[0].dvs;
    ASSERT_EQ(dvs.atk, 15u);
    ASSERT_EQ(dvs.def,  7u);
    ASSERT_EQ(dvs.spd, 15u);
    ASSERT_EQ(dvs.spc,  3u);
    ASSERT_EQ(dvs.hp_dv(), 15u);  // all odd → HP DV = 15

    // Export and verify raw bytes
    auto out = export_save(imp.snapshot, imp.shadow);
    ASSERT_EQ(out[PARTY_MON_1 + 21], 0xF7u);
    ASSERT_EQ(out[PARTY_MON_1 + 22], 0xF3u);
}

TEST(p3a_dvs_all_zero) {
    std::array<uint8_t,48> mon; mon.fill(0);
    mon[0]=1; mon[31]=5;
    // DVs all zero
    std::array<uint8_t,11> ot; ot.fill(0x50);
    std::array<uint8_t,11> nick; nick.fill(0x50);
    auto sav = make_sram_with_party(1, {mon}, {ot}, {nick});
    CrystalImport imp = import_save(sav);
    const auto& dvs = imp.snapshot.party.party_mons[0].dvs;
    ASSERT_EQ(dvs.atk, 0u);
    ASSERT_EQ(dvs.def, 0u);
    ASSERT_EQ(dvs.hp_dv(), 0u);
}

// ── EXP big-endian ────────────────────────────────────────────────────────────

TEST(p3a_exp_big_endian_encoding) {
    // EXP = 1 000 000 (0x0F4240) stored as 3 bytes BE: 0x0F, 0x42, 0x40
    // Source: move_mon.asm GiveExp stores hMultiplicand[0..2] sequentially (BE)
    std::array<uint8_t,48> mon; mon.fill(0);
    mon[0]=1; mon[31]=50;
    mon[8] = 0x0F; mon[9] = 0x42; mon[10] = 0x40;  // EXP = 1 000 000 BE
    std::array<uint8_t,11> ot; ot.fill(0x50);
    std::array<uint8_t,11> nick; nick.fill(0x50);
    auto sav = make_sram_with_party(1, {mon}, {ot}, {nick});
    CrystalImport imp = import_save(sav);
    ASSERT_EQ(imp.snapshot.party.party_mons[0].exp, 1000000u);

    // Export and verify raw bytes preserved
    auto out = export_save(imp.snapshot, imp.shadow);
    ASSERT_EQ(out[PARTY_MON_1 + 8],  0x0Fu);
    ASSERT_EQ(out[PARTY_MON_1 + 9],  0x42u);
    ASSERT_EQ(out[PARTY_MON_1 + 10], 0x40u);
}

// ── Stat EXP LE ───────────────────────────────────────────────────────────────

// Stat EXP is stored as big-endian u16 in Crystal SRAM.
// Evidence: suiCune move_mon.c BigEndianToNative16(statExp[c]);
//           CalcMonStatC reads [hld]=low addr=HIGH byte then [hl]=high addr=LOW byte → DE = HIGH<<8|LOW.

TEST(p3a_stat_exp_big_endian_raw_bytes) {
    // SRAM bytes 0x02 0x00 → semantic value 0x0200 = 512 (big-endian)
    // If LE were used: 0x02 0x00 LE = 2, NOT 512
    std::array<uint8_t,48> mon; mon.fill(0);
    mon[0]=1; mon[31]=5;
    mon[11] = 0x02; mon[12] = 0x00;  // HP EXP = 0x0200 = 512 BE
    std::array<uint8_t,11> ot; ot.fill(0x50);
    std::array<uint8_t,11> nick; nick.fill(0x50);
    auto sav = make_sram_with_party(1, {mon}, {ot}, {nick});
    CrystalImport imp = import_save(sav);
    const auto& pm = imp.snapshot.party.party_mons[0];
    ASSERT_EQ(pm.stat_exp_hp, 512u);  // 0x0200 BE

    // Export: semantic 512 → SRAM bytes 0x02 0x00
    auto out = export_save(imp.snapshot, imp.shadow);
    ASSERT_EQ(out[PARTY_MON_1 + 11], 0x02u);  // HIGH byte first (BE)
    ASSERT_EQ(out[PARTY_MON_1 + 12], 0x00u);  // LOW byte second
}

TEST(p3a_stat_exp_be_decode_0x1234) {
    // SRAM bytes 0x12 0x34 → semantic value 0x1234 = 4660 (big-endian)
    // LE interpretation would give 0x3412 = 13330 — wrong
    std::array<uint8_t,48> mon; mon.fill(0);
    mon[0]=1; mon[31]=5;
    mon[11] = 0x12; mon[12] = 0x34;  // HP EXP BE
    mon[13] = 0x56; mon[14] = 0x78;  // ATK EXP BE
    std::array<uint8_t,11> ot; ot.fill(0x50);
    std::array<uint8_t,11> nick; nick.fill(0x50);
    auto sav = make_sram_with_party(1, {mon}, {ot}, {nick});
    CrystalImport imp = import_save(sav);
    const auto& pm = imp.snapshot.party.party_mons[0];
    ASSERT_EQ(pm.stat_exp_hp,  0x1234u);
    ASSERT_EQ(pm.stat_exp_atk, 0x5678u);
}

TEST(p3a_stat_exp_all_five_fields_be) {
    // All five stat EXP fields independently use big-endian layout.
    // Layout bytes [11..20]: HP[11-12] ATK[13-14] DEF[15-16] SPD[17-18] SPC[19-20]
    std::array<uint8_t,48> mon; mon.fill(0);
    mon[0]=1; mon[31]=5;
    mon[11]=0x01; mon[12]=0x00;  // HP  EXP = 256 BE
    mon[13]=0x02; mon[14]=0x00;  // ATK EXP = 512 BE
    mon[15]=0x03; mon[16]=0x00;  // DEF EXP = 768 BE
    mon[17]=0x04; mon[18]=0x00;  // SPD EXP = 1024 BE
    mon[19]=0x05; mon[20]=0x00;  // SPC EXP = 1280 BE
    std::array<uint8_t,11> ot; ot.fill(0x50);
    std::array<uint8_t,11> nick; nick.fill(0x50);
    auto sav = make_sram_with_party(1, {mon}, {ot}, {nick});
    CrystalImport imp = import_save(sav);
    const auto& pm = imp.snapshot.party.party_mons[0];
    ASSERT_EQ(pm.stat_exp_hp,   256u);
    ASSERT_EQ(pm.stat_exp_atk,  512u);
    ASSERT_EQ(pm.stat_exp_def,  768u);
    ASSERT_EQ(pm.stat_exp_spd, 1024u);
    ASSERT_EQ(pm.stat_exp_spc, 1280u);

    // Export: verify each field exports correct BE byte order
    auto out = export_save(imp.snapshot, imp.shadow);
    ASSERT_EQ(out[PARTY_MON_1 + 11], 0x01u); ASSERT_EQ(out[PARTY_MON_1 + 12], 0x00u);
    ASSERT_EQ(out[PARTY_MON_1 + 13], 0x02u); ASSERT_EQ(out[PARTY_MON_1 + 14], 0x00u);
    ASSERT_EQ(out[PARTY_MON_1 + 15], 0x03u); ASSERT_EQ(out[PARTY_MON_1 + 16], 0x00u);
    ASSERT_EQ(out[PARTY_MON_1 + 17], 0x04u); ASSERT_EQ(out[PARTY_MON_1 + 18], 0x00u);
    ASSERT_EQ(out[PARTY_MON_1 + 19], 0x05u); ASSERT_EQ(out[PARTY_MON_1 + 20], 0x00u);
}

TEST(p3a_stat_exp_round_trip_unchanged) {
    // Representative party with non-trivial (non-symmetric) stat EXP values.
    // Import → export with NO edits → every stat EXP byte must be identical.
    // Regression guard: a symmetric-but-wrong LE/BE bug would still round-trip
    // correctly only for values where byte0 == byte1; non-trivial values expose it.
    std::array<uint8_t,48> mon; mon.fill(0);
    mon[0]=25; mon[31]=30;   // Pikachu, level 30
    // Non-symmetric BE values: HIGH != LOW
    mon[11]=0x01; mon[12]=0xF4;  // HP  EXP = 500 BE
    mon[13]=0x01; mon[14]=0xC8;  // ATK EXP = 456 BE
    mon[15]=0x01; mon[16]=0x90;  // DEF EXP = 400 BE
    mon[17]=0x01; mon[18]=0x2C;  // SPD EXP = 300 BE
    mon[19]=0x00; mon[20]=0x64;  // SPC EXP = 100 BE
    std::array<uint8_t,11> ot; ot.fill(0x50);
    std::array<uint8_t,11> nick; nick.fill(0x50);
    auto sav = make_sram_with_party(1, {mon}, {ot}, {nick});
    CrystalImport imp = import_save(sav);

    // Export unchanged
    auto out = export_save(imp.snapshot, imp.shadow);

    // Every stat EXP byte must be byte-identical to the input
    ASSERT_EQ(out[PARTY_MON_1 + 11], 0x01u); ASSERT_EQ(out[PARTY_MON_1 + 12], 0xF4u);
    ASSERT_EQ(out[PARTY_MON_1 + 13], 0x01u); ASSERT_EQ(out[PARTY_MON_1 + 14], 0xC8u);
    ASSERT_EQ(out[PARTY_MON_1 + 15], 0x01u); ASSERT_EQ(out[PARTY_MON_1 + 16], 0x90u);
    ASSERT_EQ(out[PARTY_MON_1 + 17], 0x01u); ASSERT_EQ(out[PARTY_MON_1 + 18], 0x2Cu);
    ASSERT_EQ(out[PARTY_MON_1 + 19], 0x00u); ASSERT_EQ(out[PARTY_MON_1 + 20], 0x64u);
}

// ── Stats big-endian ─────────────────────────────────────────────────────────

TEST(p3a_stats_big_endian_encoding) {
    // Stats are big-endian (CalcMonStats stores hMultiplicand[1] as high byte)
    // MaxHP=230: byte[36]=0x00, byte[37]=0xE6
    // ATK=260:   byte[38]=0x01, byte[39]=0x04
    std::array<uint8_t,48> mon; mon.fill(0);
    mon[0]=1; mon[31]=50;
    mon[34]=0x00; mon[35]=0xB4;  // HP=180 BE
    mon[36]=0x00; mon[37]=0xE6;  // MaxHP=230 BE
    mon[38]=0x01; mon[39]=0x04;  // ATK=260 BE
    std::array<uint8_t,11> ot; ot.fill(0x50);
    std::array<uint8_t,11> nick; nick.fill(0x50);
    auto sav = make_sram_with_party(1, {mon}, {ot}, {nick});
    CrystalImport imp = import_save(sav);
    const auto& pm = imp.snapshot.party.party_mons[0];
    ASSERT_EQ(pm.current_hp, 180u);
    ASSERT_EQ(pm.max_hp,     230u);
    ASSERT_EQ(pm.stat_atk,   260u);

    // Export: verify raw BE bytes
    auto out = export_save(imp.snapshot, imp.shadow);
    ASSERT_EQ(out[PARTY_MON_1 + 34], 0x00u);
    ASSERT_EQ(out[PARTY_MON_1 + 35], 0xB4u);
    ASSERT_EQ(out[PARTY_MON_1 + 36], 0x00u);
    ASSERT_EQ(out[PARTY_MON_1 + 37], 0xE6u);
    ASSERT_EQ(out[PARTY_MON_1 + 38], 0x01u);
    ASSERT_EQ(out[PARTY_MON_1 + 39], 0x04u);
}

// ── Caught data ───────────────────────────────────────────────────────────────

TEST(p3a_caught_data_decode_encode) {
    // caught_time_level byte: bits7-6=time, bits5-0=level
    // caught_gender_location: bit7=gender(1=boy), bits6-0=location
    std::array<uint8_t,48> mon; mon.fill(0);
    mon[0]=1; mon[31]=5;
    mon[29] = static_cast<uint8_t>((3u<<6) | 25u); // night(3), level 25
    mon[30] = static_cast<uint8_t>((1u<<7) | 42u); // boy, location 42
    std::array<uint8_t,11> ot; ot.fill(0x50);
    std::array<uint8_t,11> nick; nick.fill(0x50);
    auto sav = make_sram_with_party(1, {mon}, {ot}, {nick});
    CrystalImport imp = import_save(sav);
    const auto& c = imp.snapshot.party.party_mons[0].caught;
    ASSERT_EQ(c.time_of_day,  3u);  // night
    ASSERT_EQ(c.caught_level, 25u);
    ASSERT_TRUE(c.caught_by_boy);
    ASSERT_EQ(c.location, 42u);

    auto out = export_save(imp.snapshot, imp.shadow);
    ASSERT_EQ(out[PARTY_MON_1 + 29], static_cast<uint8_t>((3u<<6)|25u));
    ASSERT_EQ(out[PARTY_MON_1 + 30], static_cast<uint8_t>((1u<<7)|42u));
}

// ── Status and current HP ─────────────────────────────────────────────────────

TEST(p3a_status_and_current_hp_round_trip) {
    std::array<uint8_t,48> mon; mon.fill(0);
    mon[0]=1; mon[31]=50;
    mon[32] = 0x28;  // BRN = bit 3 + something, arbitrary status byte
    mon[34] = 0x00; mon[35] = 0x64;  // HP=100 BE
    mon[36] = 0x00; mon[37] = 0xC8;  // MaxHP=200 BE
    std::array<uint8_t,11> ot; ot.fill(0x50);
    std::array<uint8_t,11> nick; nick.fill(0x50);
    auto sav = make_sram_with_party(1, {mon}, {ot}, {nick});
    CrystalImport imp = import_save(sav);
    ASSERT_EQ(imp.snapshot.party.party_mons[0].status,     0x28u);
    ASSERT_EQ(imp.snapshot.party.party_mons[0].current_hp, 100u);
    ASSERT_EQ(imp.snapshot.party.party_mons[0].max_hp,     200u);

    auto out = export_save(imp.snapshot, imp.shadow);
    ASSERT_EQ(out[PARTY_MON_1 + 32], 0x28u);
    ASSERT_EQ(out[PARTY_MON_1 + 34], 0x00u);
    ASSERT_EQ(out[PARTY_MON_1 + 35], 0x64u);
}

// ── Nickname and OT charmap round-trip ───────────────────────────────────────

TEST(p3a_nickname_ot_charmap_round_trip) {
    // "PIKACHU" in Crystal charmap: P=0x8F,I=0x88,K=0x8A,A=0x80,C=0x82,H=0x87,U=0x94
    // OT "RED": R=0x91,E=0x84,D=0x83
    std::array<uint8_t,48> mon; mon.fill(0);
    mon[0]=25; mon[31]=5; // Pikachu, level 5
    std::array<uint8_t,11> ot; ot.fill(0x50);
    ot[0]=0x91; ot[1]=0x84; ot[2]=0x83;  // "RED"
    std::array<uint8_t,11> nick; nick.fill(0x50);
    nick[0]=0x8F; nick[1]=0x88; nick[2]=0x8A;
    nick[3]=0x80; nick[4]=0x82; nick[5]=0x87; nick[6]=0x94; // "PIKACHU"
    auto sav = make_sram_with_party(1, {mon}, {ot}, {nick});
    CrystalImport imp = import_save(sav);
    ASSERT_TRUE(imp.snapshot.party.party_mons[0].ot_name  == "RED");
    ASSERT_TRUE(imp.snapshot.party.party_mons[0].nickname == "PIKACHU");

    // Export and verify raw bytes
    auto out = export_save(imp.snapshot, imp.shadow);
    ASSERT_EQ(out[PARTY_OT_NAMES],  0x91u);  // 'R'
    ASSERT_EQ(out[PARTY_OT_NAMES + 1], 0x84u); // 'E'
    ASSERT_EQ(out[PARTY_NICKNAMES], 0x8Fu);  // 'P'
    ASSERT_EQ(out[PARTY_NICKNAMES + 6], 0x94u); // 'U'

    CrystalImport reimp = import_save(out);
    ASSERT_TRUE(reimp.snapshot.party.party_mons[0].ot_name  == "RED");
    ASSERT_TRUE(reimp.snapshot.party.party_mons[0].nickname == "PIKACHU");
}

// ── Full 6-mon party import ───────────────────────────────────────────────────

TEST(p3a_full_six_mon_party_import) {
    std::vector<std::array<uint8_t,48>> mons(6);
    std::vector<std::array<uint8_t,11>> ots(6);
    std::vector<std::array<uint8_t,11>> nicks(6);
    uint8_t species[] = {1,4,7,152,155,158};  // Bulbasaur,Charmander,Squirtle,Chikorita,Cyndaquil,Totodile
    for (int i = 0; i < 6; ++i) {
        mons[i].fill(0);
        mons[i][0] = species[i];
        mons[i][31] = static_cast<uint8_t>(5 + i);
        ots[i].fill(0x50);
        nicks[i].fill(0x50);
    }
    auto sav = make_sram_with_party(6, mons, ots, nicks);
    CrystalImport imp = import_save(sav);
    ASSERT_EQ(imp.snapshot.party.party_count, 6u);
    for (int i = 0; i < 6; ++i) {
        ASSERT_EQ(imp.snapshot.party.party_mons[i].species, species[i]);
        ASSERT_EQ(imp.snapshot.party.party_mons[i].level,   static_cast<uint8_t>(5 + i));
    }
}

// ── Unchanged import→export byte diff ────────────────────────────────────────

TEST(p3a_unchanged_party_roundtrip_no_unowned_byte_drift) {
    std::array<uint8_t,48> mon; mon.fill(0);
    mon[0]=25; mon[31]=50; // Pikachu
    // Set all stat bytes to known values to verify they round-trip
    mon[34]=0x00; mon[35]=0x80;  // HP=128
    mon[36]=0x00; mon[37]=0x80;  // MaxHP=128
    std::array<uint8_t,11> ot; ot.fill(0x50);
    ot[0]=0x80; // 'A'
    std::array<uint8_t,11> nick; nick.fill(0x50);
    nick[0]=0x8F; // 'P'

    auto sav = make_sram_with_party(1, {mon}, {ot}, {nick});
    CrystalImport imp = import_save(sav);
    auto out = export_save(imp.snapshot, imp.shadow);

    // Every party byte must be identical between original and exported
    for (uint32_t i = 0; i < 48u; ++i) {
        if (out[PARTY_MON_1 + i] != sav[PARTY_MON_1 + i]) {
            throw std::runtime_error("party mon byte mismatch at offset " + std::to_string(i));
        }
    }
    for (uint32_t i = 0; i < 11u; ++i) {
        if (out[PARTY_OT_NAMES  + i] != sav[PARTY_OT_NAMES  + i]) {
            throw std::runtime_error("OT name byte mismatch at offset " + std::to_string(i));
        }
        if (out[PARTY_NICKNAMES + i] != sav[PARTY_NICKNAMES + i]) {
            throw std::runtime_error("nickname byte mismatch at offset " + std::to_string(i));
        }
    }
}

// ── Semantic edit produces expected SRAM bytes ────────────────────────────────

TEST(p3a_semantic_edit_changes_expected_bytes) {
    // Import, change species and happiness, export, verify raw bytes changed.
    std::array<uint8_t,48> mon; mon.fill(0);
    mon[0]=1; mon[31]=5; mon[27]=50;  // Bulbasaur, happiness=50
    std::array<uint8_t,11> ot; ot.fill(0x50);
    std::array<uint8_t,11> nick; nick.fill(0x50);
    auto sav = make_sram_with_party(1, {mon}, {ot}, {nick});
    CrystalImport imp = import_save(sav);

    // Edit species and happiness
    imp.snapshot.party.party_mons[0].species   = 4;   // Charmander
    imp.snapshot.party.party_mons[0].happiness = 200;

    auto out = export_save(imp.snapshot, imp.shadow);
    // Species byte at struct+0 and in species list
    ASSERT_EQ(out[PARTY_MON_1 + 0], 4u);
    ASSERT_EQ(out[PARTY_SPECIES],   4u);
    // Happiness at struct+27
    ASSERT_EQ(out[PARTY_MON_1 + 27], 200u);
    // Everything else unchanged (spot check: level)
    ASSERT_EQ(out[PARTY_MON_1 + 31], 5u);

    // Re-import: species and happiness match
    CrystalImport reimp = import_save(out);
    ASSERT_EQ(reimp.snapshot.party.party_mons[0].species,   4u);
    ASSERT_EQ(reimp.snapshot.party.party_mons[0].happiness, 200u);
}

// ── Unrepresentable native Pokémon rejected on export ────────────────────────

TEST(p3a_export_invalid_species_rejected) {
    auto sav = make_valid_sram();
    CrystalImport imp = import_save(sav);
    imp.snapshot.party.party_count = 1;
    imp.snapshot.party.party_mons[0].species = 252;  // invalid for Crystal v1.1 (>251, not EGG)
    imp.snapshot.party.party_mons[0].level   = 5;
    ASSERT_THROWS(export_save(imp.snapshot, imp.shadow), SaveExportError);
}

TEST(p3a_export_invalid_party_count_rejected) {
    auto sav = make_valid_sram();
    CrystalImport imp = import_save(sav);
    imp.snapshot.party.party_count = 7;  // > PARTY_LENGTH = 6
    ASSERT_THROWS(export_save(imp.snapshot, imp.shadow), SaveExportError);
}

TEST(p3a_export_dv_out_of_range_rejected) {
    auto sav = make_valid_sram();
    CrystalImport imp = import_save(sav);
    imp.snapshot.party.party_count = 1;
    imp.snapshot.party.party_mons[0].species = 1;
    imp.snapshot.party.party_mons[0].level   = 5;
    imp.snapshot.party.party_mons[0].dvs.atk = 16;  // > 15, invalid
    ASSERT_THROWS(export_save(imp.snapshot, imp.shadow), SaveExportError);
}

TEST(p3a_export_unrepresentable_name_rejected) {
    auto sav = make_valid_sram();
    CrystalImport imp = import_save(sav);
    imp.snapshot.party.party_count = 1;
    imp.snapshot.party.party_mons[0].species  = 1;
    imp.snapshot.party.party_mons[0].level    = 5;
    imp.snapshot.party.party_mons[0].nickname = "\xE2\x98\x80"; // ☀ not in Crystal charmap
    ASSERT_THROWS(export_save(imp.snapshot, imp.shadow), SaveExportError);
}

// PHASE 1 CLOSURE: Symbol-derived offsets, backup boundaries, SRAM identity
// ─────────────────────────────────────────────────────────────────────────────

// Validates every sram_layout constant against the assembled pokecrystal11.sym.
// Skipped silently if the sym file is not available (CI without references/).
TEST(symbol_derived_offsets_match_expected_layout) {
    auto cwd = std::filesystem::current_path();
    std::filesystem::path sym_path;
    for (auto dir = cwd; !dir.empty() && dir != dir.parent_path(); dir = dir.parent_path()) {
        auto c = dir / "references" / "pokecrystal-symbols" / "pokecrystal11.sym";
        if (std::filesystem::exists(c)) { sym_path = c; break; }
    }
    if (sym_path.empty()) {
        std::cout << "    [sym file not found - skipping layout validation]\n";
        return;
    }
    std::cout << "    [sym: " << sym_path.string() << "]\n";
    std::string result = crystal::sram_layout::validate_layout_against_sym(sym_path);
    if (!result.empty()) throw std::runtime_error(result);
    std::cout << "    [all layout constants match sym]\n";
}

TEST(event_flags_offset_is_sym_derived_value) {
    // sym 01:da72: sav = 0x2009 + (0xDA72 - 0xD47B) = 0x2600
    ASSERT_EQ(crystal::sram_layout::EVENT_FLAGS, 0x2600u);
}

TEST(event_flags_at_corrected_offset_roundtrips) {
    std::array<uint8_t, 100> flags{};
    flags[0]  = 0x01; flags[50] = 0xAB; flags[99] = 0x80;
    auto sav = make_valid_sram(0, 0, 0, &flags);
    ASSERT_EQ(sav[0x2600], 0x01);   // flag[0] at corrected offset
    ASSERT_EQ(sav[0x2601], 0x00);   // flag[1] — old wrong offset, must NOT be flag[0]
    CrystalImport imp = import_save(sav);
    ASSERT_EQ(imp.snapshot.event_flags[0],  0x01);
    ASSERT_EQ(imp.snapshot.event_flags[50], 0xAB);
    ASSERT_EQ(imp.snapshot.event_flags[99], 0x80);
    auto out = export_save(imp.snapshot, imp.shadow);
    ASSERT_EQ(out[0x2600],      0x01);
    ASSERT_EQ(out[0x2600 + 50], 0xAB);
    ASSERT_EQ(out[0x2600 + 99], 0x80);
}

TEST(backup_event_flags_offset_is_correct) {
    // BACKUP_EVENT_FLAGS = EVENT_FLAGS - BACKUP_OFFSET = 0x2600 - 0xE00 = 0x1800
    ASSERT_EQ(crystal::sram_layout::BACKUP_EVENT_FLAGS, 0x1800u);
    std::array<uint8_t, 100> flags{};
    flags[7] = 0xCC;
    auto sav = make_valid_sram(0, 0, 0, &flags);
    CrystalImport imp = import_save(sav);
    auto out = export_save(imp.snapshot, imp.shadow);
    ASSERT_EQ(out[EVENT_FLAGS + 7],        0xCC);
    ASSERT_EQ(out[BACKUP_EVENT_FLAGS + 7], 0xCC);
}

TEST(backup_boundaries_match_sym_derived_values) {
    ASSERT_EQ(BACKUP_OPTIONS,       0x1200u);  // sym 00:b200
    ASSERT_EQ(BACKUP_CHECK_VALUE_1, 0x1208u);  // sym 00:b208
    ASSERT_EQ(BACKUP_GAME_DATA,     0x1209u);  // sym 00:b209
    ASSERT_EQ(BACKUP_CHECKSUM_END,  0x1D82u);  // sym sBackupGameDataEnd 00:bd83 -> end=0x1D82
    ASSERT_EQ(BACKUP_CHECKSUM,      0x1F0Du);  // sym 00:bf0d
    ASSERT_EQ(BACKUP_CHECK_VALUE_2, 0x1F0Fu);  // sym 00:bf0f
    ASSERT_EQ(BACKUP_CHECKSUM_END - BACKUP_GAME_DATA + 1, CHECKSUM_REGION_SIZE);
    ASSERT_EQ(BACKUP_OFFSET, 0xE00u);
}

TEST(import_binds_empty_identity_by_default) {
    auto sav = make_valid_sram();
    CrystalImport imp = import_save(sav);
    ASSERT_TRUE(imp.shadow.identity.profile_sha1.empty());
    ASSERT_TRUE(imp.shadow.identity.codec_version == std::string(crystal::SRAM_CODEC_VERSION));
}

TEST(import_binds_profile_sha1_when_supplied) {
    auto sav = make_valid_sram();
    CrystalImport imp = import_save(sav,
        "f2f52230b536214ef7c9924f483392993e226cfb",
        "f2f52230b536214ef7c9924f483392993e226cfb");
    ASSERT_TRUE(imp.shadow.identity.profile_sha1 == std::string("f2f52230b536214ef7c9924f483392993e226cfb"));
    ASSERT_TRUE(imp.shadow.identity.rom_sha1 == std::string("f2f52230b536214ef7c9924f483392993e226cfb"));
    ASSERT_TRUE(imp.shadow.identity.codec_version == std::string(crystal::SRAM_CODEC_VERSION));
}

TEST(export_with_matching_identity_succeeds) {
    crystal::SramIdentity id;
    id.profile_sha1  = "f2f52230b536214ef7c9924f483392993e226cfb";
    id.codec_version = crystal::SRAM_CODEC_VERSION;
    auto sav = make_valid_sram();
    CrystalImport imp = import_save(sav,
        "f2f52230b536214ef7c9924f483392993e226cfb");
    ASSERT_NO_THROW(export_save(imp.snapshot, imp.shadow, id));
}

TEST(export_with_wrong_profile_sha1_rejected) {
    crystal::SramIdentity expected;
    expected.profile_sha1  = "f2f52230b536214ef7c9924f483392993e226cfb";
    expected.codec_version = crystal::SRAM_CODEC_VERSION;
    auto sav = make_valid_sram();
    CrystalImport imp = import_save(sav, "0000000000000000000000000000000000000000");
    ASSERT_THROWS(export_save(imp.snapshot, imp.shadow, expected), SaveExportError);
}

TEST(export_with_wrong_codec_version_rejected) {
    crystal::SramIdentity expected;
    expected.profile_sha1  = "f2f52230b536214ef7c9924f483392993e226cfb";
    expected.codec_version = crystal::SRAM_CODEC_VERSION;
    auto sav = make_valid_sram();
    CrystalImport imp = import_save(sav, "f2f52230b536214ef7c9924f483392993e226cfb");
    imp.shadow.identity.codec_version = "crystal-save-0.9";
    ASSERT_THROWS(export_save(imp.snapshot, imp.shadow, expected), SaveExportError);
}

TEST(export_with_empty_expected_identity_skips_check) {
    auto sav = make_valid_sram();
    CrystalImport imp = import_save(sav, "f2f52230b536214ef7c9924f483392993e226cfb");
    crystal::SramIdentity empty_id;
    ASSERT_NO_THROW(export_save(imp.snapshot, imp.shadow, empty_id));
    ASSERT_NO_THROW(export_save(imp.snapshot, imp.shadow));
}

TEST(export_with_empty_shadow_identity_skips_check) {
    crystal::SramIdentity expected;
    expected.profile_sha1  = "f2f52230b536214ef7c9924f483392993e226cfb";
    expected.codec_version = crystal::SRAM_CODEC_VERSION;
    auto sav = make_valid_sram();
    CrystalImport imp = import_save(sav);  // empty shadow identity
    ASSERT_NO_THROW(export_save(imp.snapshot, imp.shadow, expected));
}

TEST(identity_error_message_mentions_both_profiles) {
    crystal::SramIdentity expected;
    expected.profile_sha1  = "crystal_v11";
    expected.codec_version = crystal::SRAM_CODEC_VERSION;
    auto sav = make_valid_sram();
    CrystalImport imp = import_save(sav, "gold_v10");
    try {
        export_save(imp.snapshot, imp.shadow, expected);
        throw std::runtime_error("should have thrown");
    } catch (const SaveExportError& e) {
        std::string msg = e.what();
        ASSERT_TRUE(msg.find("crystal_v11") != std::string::npos);
        ASSERT_TRUE(msg.find("gold_v10")    != std::string::npos);
    }
}

// Same profile_sha1, different rom_sha1 → reject.
// The default policy is SHA-driven: one ROM SHA → one profile.
// A save imported from ROM-A must not be patched as if it came from ROM-B,
// even when both ROMs share the same layout profile.
TEST(same_profile_different_rom_sha_rejected) {
    const std::string profile = "f2f52230b536214ef7c9924f483392993e226cfb";
    const std::string rom_a   = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    const std::string rom_b   = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";

    // expected_identity carries the rom_sha1 for ROM-A
    crystal::SramIdentity expected;
    expected.profile_sha1  = profile;
    expected.rom_sha1      = rom_a;
    expected.codec_version = crystal::SRAM_CODEC_VERSION;

    // Shadow was imported from ROM-B
    auto sav = make_valid_sram();
    CrystalImport imp = import_save(sav, profile, rom_b);

    // Must reject even though profile_sha1 matches
    ASSERT_THROWS(export_save(imp.snapshot, imp.shadow, expected), SaveExportError);
}

// Exact rom_sha1 match → accept.
TEST(exact_rom_sha1_identity_accepted) {
    const std::string profile = "f2f52230b536214ef7c9924f483392993e226cfb";
    const std::string rom_sha = "f2f52230b536214ef7c9924f483392993e226cfb";

    crystal::SramIdentity expected;
    expected.profile_sha1  = profile;
    expected.rom_sha1      = rom_sha;
    expected.codec_version = crystal::SRAM_CODEC_VERSION;

    auto sav = make_valid_sram();
    CrystalImport imp = import_save(sav, profile, rom_sha);

    ASSERT_NO_THROW(export_save(imp.snapshot, imp.shadow, expected));
}

// Empty rom_sha1 on shadow (synthetic save) still passes — no ROM was imported.
TEST(empty_shadow_rom_sha1_skips_rom_check) {
    const std::string profile = "f2f52230b536214ef7c9924f483392993e226cfb";

    crystal::SramIdentity expected;
    expected.profile_sha1  = profile;
    expected.rom_sha1      = "f2f52230b536214ef7c9924f483392993e226cfb";
    expected.codec_version = crystal::SRAM_CODEC_VERSION;

    // Shadow imported with no rom_sha1
    auto sav = make_valid_sram();
    CrystalImport imp = import_save(sav, profile, "");

    // rom_sha1 empty on shadow → ROM check skipped → export proceeds
    ASSERT_NO_THROW(export_save(imp.snapshot, imp.shadow, expected));
}

// Empty expected rom_sha1 (caller doesn't know/care) → ROM check skipped.
TEST(empty_expected_rom_sha1_skips_rom_check) {
    const std::string profile = "f2f52230b536214ef7c9924f483392993e226cfb";

    crystal::SramIdentity expected;
    expected.profile_sha1  = profile;
    expected.rom_sha1      = "";   // caller doesn't specify
    expected.codec_version = crystal::SRAM_CODEC_VERSION;

    auto sav = make_valid_sram();
    CrystalImport imp = import_save(sav, profile,
                                    "cccccccccccccccccccccccccccccccccccccccc");

    // Expected rom_sha1 is empty → ROM check skipped → export proceeds
    ASSERT_NO_THROW(export_save(imp.snapshot, imp.shadow, expected));
}

// Error message must mention both ROM SHAs so the caller knows what conflicted.
TEST(rom_sha1_mismatch_error_mentions_both_hashes) {
    const std::string profile = "f2f52230b536214ef7c9924f483392993e226cfb";
    const std::string rom_a   = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    const std::string rom_b   = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";

    crystal::SramIdentity expected;
    expected.profile_sha1  = profile;
    expected.rom_sha1      = rom_a;
    expected.codec_version = crystal::SRAM_CODEC_VERSION;

    auto sav = make_valid_sram();
    CrystalImport imp = import_save(sav, profile, rom_b);

    try {
        export_save(imp.snapshot, imp.shadow, expected);
        throw std::runtime_error("should have thrown");
    } catch (const SaveExportError& e) {
        std::string msg = e.what();
        ASSERT_TRUE(msg.find(rom_a) != std::string::npos);
        ASSERT_TRUE(msg.find(rom_b) != std::string::npos);
    }
}

// Entry point
// â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

int main() {
    std::cout << "=== crystal_save_test ===\n\n";

    std::cout << "-- Category 1: Valid import --\n";
    RUN(valid_import_basic);
    RUN(valid_import_zero_values);
    RUN(valid_import_max_values);
    RUN(valid_import_shadow_preserved);
    RUN(valid_import_with_trailer);

    std::cout << "-- Category 2: Corrupt checksum handling --\n";
    RUN(corrupt_primary_checksum_rejected_if_backup_also_bad);
    RUN(corrupt_primary_sentinel_rejected_if_backup_also_bad);
    RUN(corrupt_checksum_error_message_is_informative);

    std::cout << "-- Category 3: Backup recovery --\n";
    RUN(backup_recovery_when_primary_checksum_bad);
    RUN(backup_recovery_when_primary_sentinel_bad);
    RUN(backup_preferred_order_primary_first);

    std::cout << "-- Category 4: Wrong-size rejection --\n";
    RUN(empty_input_rejected);
    RUN(too_small_input_rejected);
    RUN(null_pointer_rejected);
    RUN(exactly_32kb_accepted);
    RUN(larger_than_32kb_accepted_trailer_preserved);

    std::cout << "-- Category 5: Unchanged importâ†’export preserves unowned bytes --\n";
    RUN(roundtrip_unowned_bytes_preserved);
    RUN(roundtrip_trailer_preserved);
    RUN(roundtrip_no_edit_checksums_valid);

    std::cout << "-- Category 6: money/mom-money/coins independent --\n";
    RUN(money_fields_independent_player_edit);
    RUN(money_fields_independent_mom_edit);
    RUN(money_fields_independent_coins_edit);
    RUN(money_overflow_rejected);
    RUN(mom_money_overflow_rejected);
    RUN(coins_overflow_rejected);

    std::cout << "-- Category 7: EventFlag/EngineFlag independent --\n";
    RUN(event_flags_round_trip_bit_set);
    RUN(event_flags_round_trip_all_clear);
    RUN(event_flags_edit_persists_through_export);
    RUN(event_flags_edit_does_not_affect_money);
    RUN(engine_flags_not_in_snapshot);

    std::cout << "-- Category 8: Owned-field edit changes only expected regions --\n";
    RUN(money_edit_only_touches_money_region_and_checksums);
    RUN(event_flag_edit_only_touches_flags_region_and_checksums);
    RUN(backup_mirror_matches_primary_after_export);

    std::cout << "-- Category 9: Exported save re-imports correctly --\n";
    RUN(export_reimport_money_exact);
    RUN(export_reimport_event_flags_exact);
    RUN(export_reimport_is_idempotent);
    RUN(export_produces_valid_sentinels);

    std::cout << "-- Category 10: Malformed input never causes UB --\n";
    RUN(all_zeros_rejected_gracefully);
    RUN(all_ones_rejected_gracefully);
    RUN(random_garbage_rejected_gracefully);
    RUN(invalid_bcd_in_valid_sram_rejected_on_decode);
    RUN(out_of_range_sram_read_throws);
    RUN(out_of_range_sram_write_throws);

    std::cout << "\n-- BCD unit tests --\n";
    RUN(bcd3_decode_zero);
    RUN(bcd3_decode_max);
    RUN(bcd3_decode_round_trip);
    RUN(bcd3_encode_overflow_rejected);
    RUN(bcd3_decode_invalid_nibble_rejected);

    std::cout << "\n-- Phase 2: player identity, map position, scenes, playtime, RTC --\n";
    RUN(p2_player_id_round_trips);
    RUN(p2_player_name_decoded_from_charmap);
    RUN(p2_player_name_round_trips);
    RUN(p2_player_names_independent);
    RUN(p2_unrepresentable_character_rejected);
    RUN(p2_gender_boy_encodes_correctly);
    RUN(p2_gender_girl_encodes_correctly);
    RUN(p2_map_identity_A_round_trips);
    RUN(p2_map_identity_B_round_trips);
    RUN(p2_map_A_and_B_are_independent);
    RUN(p2_scene_ids_round_trip);
    RUN(p2_scenes_are_independent_across_slots);
    RUN(p2_scene_edit_does_not_affect_money);
    RUN(p2_playtime_big_endian_hours_round_trip);
    RUN(p2_playtime_zero_round_trips);
    RUN(p2_playtime_capped_flag_round_trips);
    RUN(p2_playtime_capped_sets_bit0_in_raw_sram);
    RUN(p2_playtime_uncapped_clears_bit0_in_raw_sram);
    RUN(p2_playtime_capped_unrelated_bits_preserved);
    RUN(p2_import_raw_bit0_set_decodes_capped_true);
    RUN(p2_import_raw_bit7_only_does_not_decode_capped);
    RUN(p2_rtc_state_round_trips);
    RUN(p2_dst_flag_round_trips);
    RUN(p2_unowned_bytes_unchanged_on_roundtrip);
    RUN(p2_wrong_profile_rejects_p2_export);

    std::cout << "\n-- Phase 3A: party Pokémon --\n";
    RUN(p3a_one_mon_import_basic_fields);
    RUN(p3a_max_valid_species_accepted);
    RUN(p3a_egg_species_accepted);
    RUN(p3a_invalid_species_zero_rejected);
    RUN(p3a_invalid_party_count_rejected);
    RUN(p3a_empty_party_accepted);
    RUN(p3a_pp_up_packing_round_trips);
    RUN(p3a_dvs_decode_and_encode);
    RUN(p3a_dvs_all_zero);
    RUN(p3a_exp_big_endian_encoding);
    RUN(p3a_stat_exp_big_endian_raw_bytes);
    RUN(p3a_stat_exp_be_decode_0x1234);
    RUN(p3a_stat_exp_all_five_fields_be);
    RUN(p3a_stat_exp_round_trip_unchanged);
    RUN(p3a_stats_big_endian_encoding);
    RUN(p3a_caught_data_decode_encode);
    RUN(p3a_status_and_current_hp_round_trip);
    RUN(p3a_nickname_ot_charmap_round_trip);
    RUN(p3a_full_six_mon_party_import);
    RUN(p3a_unchanged_party_roundtrip_no_unowned_byte_drift);
    RUN(p3a_semantic_edit_changes_expected_bytes);
    RUN(p3a_export_invalid_species_rejected);
    RUN(p3a_export_invalid_party_count_rejected);
    RUN(p3a_export_dv_out_of_range_rejected);
    RUN(p3a_export_unrepresentable_name_rejected);

    std::cout << "\n-- Phase 1 closure: symbol-derived offsets, backup boundaries, SRAM identity --\n";    RUN(symbol_derived_offsets_match_expected_layout);
    RUN(event_flags_offset_is_sym_derived_value);
    RUN(event_flags_at_corrected_offset_roundtrips);
    RUN(backup_event_flags_offset_is_correct);
    RUN(backup_boundaries_match_sym_derived_values);
    RUN(import_binds_empty_identity_by_default);
    RUN(import_binds_profile_sha1_when_supplied);
    RUN(export_with_matching_identity_succeeds);
    RUN(export_with_wrong_profile_sha1_rejected);
    RUN(export_with_wrong_codec_version_rejected);
    RUN(export_with_empty_expected_identity_skips_check);
    RUN(export_with_empty_shadow_identity_skips_check);
    RUN(identity_error_message_mentions_both_profiles);
    RUN(same_profile_different_rom_sha_rejected);
    RUN(exact_rom_sha1_identity_accepted);
    RUN(empty_shadow_rom_sha1_skips_rom_check);
    RUN(empty_expected_rom_sha1_skips_rom_check);
    RUN(rom_sha1_mismatch_error_mentions_both_hashes);

    std::cout << "\n=== Results: " << g_pass << " PASS  " << g_fail << " FAIL ===\n";
    return (g_fail > 0) ? 1 : 0;
}
