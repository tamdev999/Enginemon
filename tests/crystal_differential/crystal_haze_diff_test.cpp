// tests/crystal_differential/crystal_haze_diff_test.cpp
//
// LIVE CRYSTAL DIFFERENTIAL TEST -- Haze (move 114 / BattleCommand_ResetStats)
//
// Crystal side:
//   ROM:  references/Pokemon - Crystal Version (UE) (V1.1) [C][!].gbc
//         SHA-1: F2F52230B536214EF7C9924F483392993E226CFB  (enforced at startup)
//   .sym: references/pokecrystal-symbols/pokecrystal11.sym
//   Emulator: SameBoy Core (unmodified, commit 213a12ce93d66b105a113debd9396306066a7cfc)
//
// Crystal execution — FULL BattleCommand_ResetStats path:
//   Entry: BattleCommand_ResetStats (0d:710e, flat 0x3710E)
//   ROM bank setup: GB_write_memory(0x2000, 0x0d)  -- MBC3 bank select
//   Cold-call: regs->pc = trampoline; sentinel addr pushed as return address
//
//   Full semantic path executed (from pokecrystal/engine/battle/effect_commands.asm):
//     BattleCommand_ResetStats:
//       .Fill(wPlayerStatLevels, 7)   -- 8 WRAM writes, no calls outside bank 0D
//       .Fill(wEnemyStatLevels, 7)    -- same
//       SetPlayerTurn()               -- ROM0, XOR A; LDH (hBattleTurn),A; RET
//       CalcPlayerStats()             -- intra-bank 0D, uses RST $08 for bank 0F calls
//         CalcBattleStats()           -- 5 iterations × mul+div → wBattleMonAttack..SpDef
//         BadgeStatBoosts()           -- bank 0F, no-op (wLinkMode=0, wJohtoBadges=0)
//         BattleCommand_SwitchTurn()  -- toggles hBattleTurn
//         ApplyPrzEffectOnSpeed()     -- bank 0F, no-op (wEnemyMonStatus=0)
//         ApplyBrnEffectOnAttack()    -- bank 0F, no-op (wEnemyMonStatus=0)
//         BattleCommand_SwitchTurn()  -- toggles back
//       SetEnemyTurn()                -- ROM0, LD A,1; LDH (hBattleTurn),A; RET
//       CalcEnemyStats()             -- intra-bank 0D, RST $08 for bank 0F
//         CalcBattleStats()           -- 5 iterations → wEnemyMonAttack..SpDef
//         BattleCommand_SwitchTurn()  -- toggles hBattleTurn
//         ApplyPrzEffectOnSpeed()     -- bank 0F, no-op (wBattleMonStatus=0)
//         ApplyBrnEffectOnAttack()    -- bank 0F, no-op (wBattleMonStatus=0)
//         BattleCommand_SwitchTurn()  -- toggles back
//       POP AF; LDH (hBattleTurn), A  -- restore original hBattleTurn
//       <<< SEMANTIC BOUNDARY: all stats recalculated, stages reset >>>
//       CALL AnimateCurrentMove       -- INTERCEPTED (see Patch 3)
//       JP StdBattleTextbox           -- NOT REACHED
//
//   ROM patches (in-memory only, do not touch disk ROM):
//     0x37E01 (0d:7E01): 0xE5 -> 0xC9 (RET)
//       AnimateCurrentMove: first byte changed from PUSH HL to RET.
//       Semantic state is fully committed before this call:
//         - wPlayerStatLevels / wEnemyStatLevels reset to 7
//         - wBattleMonAttack..SpDef (C640-C649) recalculated
//         - wEnemyMonAttack..SpDef (D21A-D223) recalculated
//         - hBattleTurn restored to its entry value
//       AnimateCurrentMove only touches wBattleAnimParam (save+restore).
//       StdBattleTextbox (not reached) only does text display.
//       NO ROM BYTES INSIDE BattleCommand_ResetStats are modified.
//     0x0150 (00:0150): JP 0x0150  -- sentinel loop
//     0x0100 (00:0100): DI; JP 0x710E  -- trampoline
//
//   RST $08 (BankSwitch at 0x2D63) is used by CallBattleCore (0D:7E73) to call
//   bank 0F functions. It requires hROMBank (FF9D) = current bank. We initialize
//   hROMBank = 0x0D so the restore-bank path works correctly.
//
// Crystal fixture — WRAM initialized from .sym (all addresses resolved):
//   wPlayerStatLevels  (C6CC): 7+delta (8 bytes, overwritten by Fill)
//   wEnemyStatLevels   (C6D4): 7+delta (8 bytes, overwritten by Fill)
//   wPlayerStats       (C6B6): base stats × 5 (big-endian uint16) — CalcBattleStats source
//   wEnemyStats        (C6C1): base stats × 5 (big-endian uint16) — CalcBattleStats source
//   wBattleMonStatus   (C63A): 0x0000 — no PAR/BRN, so Prz/Brn adjustments are no-ops
//   wEnemyMonStatus    (D214): 0x0000
//   wLinkMode          (C2DC): 0 — BadgeStatBoosts reads this first; 0 = not link battle
//   wJohtoBadges       (D857): 0 — no badge boosts applied
//   wInBattleTowerBattle (CFC0): 0
//   hBattleTurn        (FFE4): 0 — player turn (saved/restored by BattleCommand_ResetStats)
//   hROMBank           (FF9D): 0x0D — required for RST $08 bank-switch restore path
//
// Enginemon side: Battle::execute_turn() with Haze (move 114)
//   Same base stats as Crystal fixture; status=0; no badges/link.
//   Stages are set pre-Haze, then Haze resets them to 0 (neutral).
//
// Normalization: Crystal stage encoding: 7=neutral; Enginemon: 0=neutral
//   stage_delta = crystal_raw - 7 == enginemon_stage
//   computed stat comparison: Crystal wBattleMonAttack/wEnemyMonAttack == Enginemon computed
//
// Poison stability: Crystal is run twice with different garbage in unspecified WRAM.
//   Both runs must produce identical normalized snapshots.
//
// No handwritten expected Haze outputs.
// No existing oracle/golden values consulted.
// Zero Pokemon battle logic in this harness.

#include "Core/gb.h"

#include "engine/battle/battle.hpp"
#include "engine/battle/battle_rules.hpp"
#include "engine/core/types.hpp"
#include "engine/party/party.hpp"
#include "engine/party/pokemon.hpp"
#include "engine/core/registry.hpp"
#include "crystal/extract/battle_rules_extractor.hpp"
#include "crystal/rom/loader.hpp"
#include "crystal/rom/profile.hpp"
#include "crystal/compile/move_semanticizer.hpp"
#include "crystal/battle/crystal_effects.hpp"
#include "crystal/output/native_package.hpp"
#include "engine/package/package_reader.hpp"

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <optional>
#include <cstring>
#include <cstdint>
#include <filesystem>
#include <cassert>
#include <stdexcept>
#include <array>

// ============================================================================
// Pins
// ============================================================================
static constexpr const char* SAMEBOY_COMMIT =
    "213a12ce93d66b105a113debd9396306066a7cfc";

static constexpr const char* CRYSTAL_ROM_SHA1 =
    "F2F52230B536214EF7C9924F483392993E226CFB";

// ============================================================================
// Test framework
// ============================================================================
static int  g_passed = 0;
static int  g_failed = 0;
static bool g_current_failed = false;

#define ASSERT_EQ(a,b) \
    do { if (!((a)==(b))) { \
        std::cerr << "  FAIL: " << #a << "==" << #b \
                  << " got " << (int64_t)(a) << " exp " << (int64_t)(b) \
                  << " (line " << __LINE__ << ")\n"; \
        g_current_failed = true; \
    } } while(0)
#define ASSERT_TRUE(x) \
    do { if (!(x)) { std::cerr<<"  FAIL: "<<#x<<" (line "<<__LINE__<<")\n"; \
                     g_current_failed=true; } } while(0)
#define RUN_TEST(name) \
    do { g_current_failed=false; \
         std::cout<<"  "<<#name<<" ... "; std::cout.flush(); \
         name(); \
         if(!g_current_failed){++g_passed;std::cout<<"PASS\n";} \
         else{++g_failed;std::cout<<"FAIL\n";} } while(0)

// ============================================================================
// ROM globals
// ============================================================================
static const crystal::RomData*           g_rom     = nullptr;
static const crystal::ExtractionProfile* g_profile = nullptr;
static std::string                       g_rom_path;
static std::string                       g_sym_path;

// ============================================================================
// SHA-1 computation (self-contained, zero additional deps)
// ============================================================================
static std::string sha1_of_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return "";
    std::vector<uint8_t> data(std::istreambuf_iterator<char>(f), {});
    if (data.empty()) return "";

    struct SHA1State {
        uint32_t h[5] = {0x67452301,0xEFCDAB89,0x98BADCFE,0x10325476,0xC3D2E1F0};
        uint64_t count = 0;
        uint8_t  buf[64]{};
        uint32_t buf_len = 0;

        static uint32_t rotl(uint32_t v, int n) { return (v<<n)|(v>>(32-n)); }

        void process_block(const uint8_t blk[64]) {
            uint32_t w[80];
            for (int i=0;i<16;i++) w[i]=(blk[i*4]<<24)|(blk[i*4+1]<<16)|(blk[i*4+2]<<8)|blk[i*4+3];
            for (int i=16;i<80;i++) w[i]=rotl(w[i-3]^w[i-8]^w[i-14]^w[i-16],1);
            uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4];
            for (int i=0;i<80;i++) {
                uint32_t f,k;
                if(i<20){f=(b&c)|((~b)&d);k=0x5A827999;}
                else if(i<40){f=b^c^d;k=0x6ED9EBA1;}
                else if(i<60){f=(b&c)|(b&d)|(c&d);k=0x8F1BBCDC;}
                else{f=b^c^d;k=0xCA62C1D6;}
                uint32_t tmp=rotl(a,5)+f+e+k+w[i];
                e=d;d=c;c=rotl(b,30);b=a;a=tmp;
            }
            h[0]+=a;h[1]+=b;h[2]+=c;h[3]+=d;h[4]+=e;
        }
        void update(const uint8_t* data, size_t len) {
            count+=len*8;
            for(size_t i=0;i<len;i++){
                buf[buf_len++]=data[i];
                if(buf_len==64){process_block(buf);buf_len=0;}
            }
        }
        std::array<uint8_t,20> final_hash() {
            buf[buf_len++]=0x80;
            if(buf_len>56){while(buf_len<64)buf[buf_len++]=0;process_block(buf);buf_len=0;}
            while(buf_len<56)buf[buf_len++]=0;
            for(int i=0;i<8;i++) buf[56+i]=(uint8_t)(count>>((7-i)*8));
            process_block(buf);
            std::array<uint8_t,20> r{};
            for(int i=0;i<5;i++){r[i*4]=(h[i]>>24)&0xFF;r[i*4+1]=(h[i]>>16)&0xFF;r[i*4+2]=(h[i]>>8)&0xFF;r[i*4+3]=h[i]&0xFF;}
            return r;
        }
    } s;
    s.update(data.data(), data.size());
    auto hash = s.final_hash();
    char hex[41]{};
    for (int i=0;i<20;i++) sprintf(hex+i*2,"%02X",hash[i]);
    return std::string(hex);
}

// ============================================================================
// Symbol lookup from .sym file
// ============================================================================
struct CrystalSym { uint8_t bank; uint16_t addr; };

static bool sym_lookup(const std::string& sym_path, const std::string& name, CrystalSym* out) {
    std::ifstream f(sym_path);
    if (!f) return false;
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0]==';') continue;
        std::istringstream ss(line);
        std::string addr_str, sym_name;
        if (!(ss>>addr_str>>sym_name)) continue;
        if (sym_name!=name) continue;
        auto colon = addr_str.find(':');
        if (colon==std::string::npos) continue;
        out->bank = (uint8_t)std::stoi(addr_str.substr(0,colon),nullptr,16);
        out->addr = (uint16_t)std::stoi(addr_str.substr(colon+1),nullptr,16);
        return true;
    }
    return false;
}

// ============================================================================
// WRAM offset helper
// GB_DIRECT_ACCESS_RAM on CGB:
//   bank 0: [0x0000..0x0FFF]  (WRAM addr C000-CFFF)
//   bank 1: [0x1000..0x1FFF]  (WRAM addr D000-DFFF)
// ============================================================================
static size_t wram_offset(uint16_t addr) {
    if (addr >= 0xD000) return size_t{0x1000} + (addr - 0xD000);
    if (addr >= 0xC000) return addr - 0xC000;
    throw std::runtime_error("Address not in WRAM: " + std::to_string(addr));
}

// ============================================================================
// SameBoy callbacks — no-ops for rendering/audio
// ============================================================================
static uint32_t sb_pixels[160*144];
static void     sb_log(GB_gameboy_t*, const char*, GB_log_attributes_t) {}
static uint32_t sb_rgb(GB_gameboy_t*, uint8_t r, uint8_t g, uint8_t b) {
    return ((uint32_t)0xFF<<24)|((uint32_t)r<<16)|((uint32_t)g<<8)|b;
}

// ============================================================================
// Crystal Haze snapshot
// Captures the semantic outputs of BattleCommand_ResetStats:
//   - stat stage levels (both sides reset to 7=neutral)
//   - computed active battle stats (CalcBattleStats output, stage×base)
// ============================================================================
struct CrystalHazeSnapshot {
    // Stat stage levels — written by .Fill, then consumed by CalcBattleStats
    uint8_t  player_stages[7];  // ATK/DEF/SPD/SATK/SDEF/ACC/EVA (raw: 7=neutral)
    uint8_t  enemy_stages[7];
    // Computed battle stats — written by CalcBattleStats through CalcPlayerStats/CalcEnemyStats
    // wBattleMonAttack..SpDef (C640-C649), wEnemyMonAttack..SpDef (D21A-D223)
    // Both big-endian uint16 in WRAM, stored as native uint16 here.
    uint16_t player_stats[5];   // ATK/DEF/SPD/SATK/SDEF
    uint16_t enemy_stats[5];
};

// Execution sentinel state
struct SentinelCtx {
    uint16_t stop_pc;
    bool     triggered;
};

static void execution_cb(GB_gameboy_t* gb, uint16_t pc, uint8_t) {
    auto* ctx = reinterpret_cast<SentinelCtx*>(GB_get_user_data(gb));
    if (ctx && pc == ctx->stop_pc) ctx->triggered = true;
}

// ============================================================================
// Base-stats fixture for Crystal WRAM and Enginemon BattlePokemon.
// These are the values written into wPlayerStats / wEnemyStats.
// With all stages at neutral (7), CalcBattleStats produces:
//   computed = base * 1/1 = base  (for each stat, clamped to [1,999])
// ============================================================================
struct BaseStats {
    uint16_t atk, def, spd, satk, sdef;
};

// Player mon: Jolteon-ish (arbitrary round numbers, no Crystal-specific meaning)
static constexpr BaseStats PLAYER_BASE = {110, 60, 130, 95, 70};
// Enemy mon: Slowbro-ish
static constexpr BaseStats ENEMY_BASE  = {75, 110, 30, 100, 80};

// ============================================================================
// Run Crystal Haze — executes the full BattleCommand_ResetStats semantic path
// ============================================================================
static bool run_crystal_haze(
    uint8_t poison,
    const int8_t player_deltas[7],   // stage deltas from neutral (0=no change)
    const int8_t enemy_deltas[7],
    CrystalHazeSnapshot* out)
{
    // --- Load ROM ---
    std::vector<uint8_t> rom_data;
    {
        std::ifstream f(g_rom_path, std::ios::binary);
        if (!f) { std::cerr<<"  [rom] cannot open "<<g_rom_path<<"\n"; return false; }
        rom_data.assign(std::istreambuf_iterator<char>(f), {});
    }
    if (rom_data.size() != 2097152) { std::cerr<<"  [rom] wrong size\n"; return false; }

    // --- Resolve symbols from .sym (no hardcoded addresses except what .sym gives us) ---
    CrystalSym s_haze, s_pstages, s_estages,
               s_pstats, s_estats,        // wPlayerStats, wEnemyStats  (CalcBattleStats input)
               s_bmon_atk, s_emon_atk,    // wBattleMonAttack, wEnemyMonAttack (CalcBattleStats output)
               s_bmon_status, s_emon_status,  // wBattleMonStatus, wEnemyMonStatus (Prz/Brn path)
               s_hbt,                     // hBattleTurn
               s_hrom_bank,               // hROMBank  (required for RST $08 BankSwitch restore)
               s_link, s_btwr, s_jbadge;  // wLinkMode, wInBattleTowerBattle, wJohtoBadges
    CrystalSym s_anim;                    // AnimateCurrentMove (presentation intercept)

    if (!sym_lookup(g_sym_path,"BattleCommand_ResetStats",&s_haze)   ||
        !sym_lookup(g_sym_path,"wPlayerStatLevels",       &s_pstages)||
        !sym_lookup(g_sym_path,"wEnemyStatLevels",        &s_estages)||
        !sym_lookup(g_sym_path,"wPlayerStats",            &s_pstats) ||
        !sym_lookup(g_sym_path,"wEnemyStats",             &s_estats) ||
        !sym_lookup(g_sym_path,"wBattleMonAttack",        &s_bmon_atk)||
        !sym_lookup(g_sym_path,"wEnemyMonAttack",         &s_emon_atk)||
        !sym_lookup(g_sym_path,"wBattleMonStatus",        &s_bmon_status)||
        !sym_lookup(g_sym_path,"wEnemyMonStatus",         &s_emon_status)||
        !sym_lookup(g_sym_path,"hBattleTurn",             &s_hbt)    ||
        !sym_lookup(g_sym_path,"hROMBank",                &s_hrom_bank)||
        !sym_lookup(g_sym_path,"wLinkMode",               &s_link)   ||
        !sym_lookup(g_sym_path,"wInBattleTowerBattle",    &s_btwr)   ||
        !sym_lookup(g_sym_path,"wJohtoBadges",            &s_jbadge) ||
        !sym_lookup(g_sym_path,"AnimateCurrentMove",      &s_anim))
    {
        std::cerr<<"  [sym] missing symbol\n"; return false;
    }

    // --- Init SameBoy ---
    GB_gameboy_t gb;
    if (!GB_init(&gb, GB_MODEL_CGB_E)) {
        std::cerr<<"  [sb] GB_init failed\n"; return false;
    }
    GB_set_log_callback(&gb, sb_log);
    GB_set_rgb_encode_callback(&gb, sb_rgb);
    GB_set_pixels_output(&gb, sb_pixels);
    GB_set_rendering_disabled(&gb, true);
    GB_set_turbo_mode(&gb, true, true);

    // Sentinel: JP loop at ROM bank 0 address 0x0150
    static constexpr uint16_t SENTINEL_PC = 0x0150;
    SentinelCtx sentinel{SENTINEL_PC, false};
    GB_set_user_data(&gb, &sentinel);
    GB_set_execution_callback(&gb, execution_cb);

    // Load ROM
    GB_load_rom_from_buffer(&gb, rom_data.data(), rom_data.size());

    // --- In-memory ROM patches ---
    size_t rom_sz = 0; uint16_t rb = 0;
    uint8_t* rom_buf = reinterpret_cast<uint8_t*>(
        GB_get_direct_access(&gb, GB_DIRECT_ACCESS_ROM, &rom_sz, &rb));
    if (!rom_buf) { std::cerr<<"  [sb] cannot access ROM\n"; GB_free(&gb); return false; }

    // Patch 1: sentinel loop at 0x0150 (ROM bank 0)
    if (rom_sz > SENTINEL_PC+2) {
        rom_buf[SENTINEL_PC+0] = 0xC3;  // JP nn
        rom_buf[SENTINEL_PC+1] = SENTINEL_PC & 0xFF;
        rom_buf[SENTINEL_PC+2] = (SENTINEL_PC>>8)&0xFF;
    }

    // Patch 2: trampoline at 0x0100 (entry point in ROM bank 0)
    //   F3        = DI  (clear IME to prevent any interrupt before reaching the routine)
    //   C3 0E 71  = JP 0x710E  (direct jump — bank 0x0D already mapped via MBC write below)
    // NOTE: JP does NOT push a return address. BattleCommand_ResetStats will return via
    //       the sentinel address we placed on the stack (written via GB_write_memory below).
    static constexpr uint16_t TRAMPOLINE_ADDR = 0x0100;
    if (rom_sz > TRAMPOLINE_ADDR + 4) {
        rom_buf[TRAMPOLINE_ADDR+0] = 0xF3;
        rom_buf[TRAMPOLINE_ADDR+1] = 0xC3;          // JP nn
        rom_buf[TRAMPOLINE_ADDR+2] = s_haze.addr & 0xFF;
        rom_buf[TRAMPOLINE_ADDR+3] = (s_haze.addr>>8) & 0xFF;
    }

    // Patch 3: AnimateCurrentMove (0d:7E01, flat 0x37E01) — presentation intercept.
    //
    // This is the ONLY patch inside the battle code region. It targets the first
    // instruction of AnimateCurrentMove, NOT anything inside BattleCommand_ResetStats.
    //
    // Why here: immediately before this CALL, BattleCommand_ResetStats has:
    //   - reset both stat stage arrays to 7 via .Fill
    //   - run SetPlayerTurn / CalcPlayerStats / SetEnemyTurn / CalcEnemyStats
    //   - restored hBattleTurn via POP AF
    // All semantic WRAM writes are complete. AnimateCurrentMove only saves and restores
    // wBattleAnimParam (C689) and calls the GPU/audio animation engine. StdBattleTextbox
    // (the next instruction after AnimateCurrentMove) is text-display only.
    //
    // With 0xE5 (PUSH HL) → 0xC9 (RET): AnimateCurrentMove returns immediately.
    // BattleCommand_ResetStats then falls to JP StdBattleTextbox which is also RET-able
    // because at that point the call stack has our sentinel return address.
    // Wait — BattleCommand_ResetStats ends with:
    //   call AnimateCurrentMove   ; intercepted, returns cleanly
    //   ld hl, EliminatedStatsText
    //   jp StdBattleTextbox       ; this JP is a tail-call, not a CALL — it replaces the
    //                             ; return address on the stack with the JP target's address.
    // PROBLEM: JP StdBattleTextbox is a plain JP (not CALL), so it does NOT push.
    // After AnimateCurrentMove returns, BattleCommand_ResetStats will execute
    // LD HL, 0x5476 and JP 0x3AD5 (StdBattleTextbox). We must also intercept
    // StdBattleTextbox OR patch the JP instruction.
    //
    // Solution: Also patch StdBattleTextbox (00:3AD5) first byte 0xF0->0xC9.
    // ROM bank 0 is always accessible. StdBattleTextbox only does text display.
    // With 0xF0 (LDH A,(n)) → 0xC9 (RET): it returns immediately from the JP target.
    // Then BattleCommand_ResetStats's CALL stack frame pops the sentinel → PC=0x0150.
    //
    // ROM bytes modified:
    //   0x37E01 (0d:7E01): AnimateCurrentMove  0xE5 → 0xC9
    //   0x03AD5 (00:3AD5): StdBattleTextbox    0xF0 → 0xC9
    // Neither address is inside BattleCommand_ResetStats (0x3710E-0x37136).
    {
        uint32_t flat_anim = (uint32_t)s_anim.bank * 0x4000u + (s_anim.addr - 0x4000u);
        if (rom_sz > flat_anim && rom_buf[flat_anim] == 0xE5) {
            rom_buf[flat_anim] = 0xC9;  // RET — AnimateCurrentMove returns immediately
        } else {
            std::cerr << "  [patch3a] AnimateCurrentMove byte = 0x"
                      << std::hex << (int)rom_buf[flat_anim] << std::dec
                      << ", expected 0xE5\n";
            GB_free(&gb); return false;
        }
    }
    {
        // StdBattleTextbox at 00:3AD5, flat = 0x3AD5 (ROM bank 0, always accessible)
        CrystalSym s_textbox;
        if (!sym_lookup(g_sym_path,"StdBattleTextbox",&s_textbox)) {
            std::cerr<<"  [sym] missing StdBattleTextbox\n"; GB_free(&gb); return false;
        }
        // Bank 0 flat = addr directly
        uint32_t flat_tb = s_textbox.addr;
        if (rom_sz > flat_tb && rom_buf[flat_tb] == 0xF0) {
            rom_buf[flat_tb] = 0xC9;  // RET
        } else {
            std::cerr << "  [patch3b] StdBattleTextbox byte = 0x"
                      << std::hex << (int)rom_buf[flat_tb] << std::dec
                      << ", expected 0xF0\n";
            GB_free(&gb); return false;
        }
    }

    // --- Set ROM bank 0x0D (MBC3: write to 0x2000-0x3FFF region) ---
    GB_write_memory(&gb, 0x2000, s_haze.bank);

    // Mark boot ROM as finished so all ROM reads (including 0x0000-0x00FF RST vectors)
    // come from the Crystal game ROM rather than the zero-filled boot ROM buffer.
    // This is required because CalcBattleStats calls Multiply (ROM0 0x3119) which uses
    // RST $08 (BankSwitch at 0x0008) and RST $10 (SwitchROM at 0x0010). Without this,
    // reads from 0x00-0xFF would return 0x00 (NOP) causing an infinite NOP spin.
    // Writing 1 to 0xFF50 (GB_IO_BANK) sets gb->boot_rom_finished = true.
    GB_write_memory(&gb, 0xFF50, 1);

    // --- Get WRAM direct access ---
    size_t wram_sz = 0; uint16_t wbank = 0;
    uint8_t* wram = reinterpret_cast<uint8_t*>(
        GB_get_direct_access(&gb, GB_DIRECT_ACCESS_RAM, &wram_sz, &wbank));
    if (!wram || wram_sz < 0x2000) {
        std::cerr<<"  [sb] WRAM access failed (size="<<wram_sz<<")\n";
        GB_free(&gb); return false;
    }

    // Poison all WRAM with the test pattern
    std::memset(wram, poison, wram_sz);

    // --- Write fixture into WRAM ---
    // All addresses resolved from .sym.

    // wLinkMode = 0: BadgeStatBoosts reads this first; 0 = non-link → falls through to
    // check wJohtoBadges. This keeps the real BadgeStatBoosts code path active.
    wram[wram_offset(s_link.addr)] = 0x00;

    // wInBattleTowerBattle = 0
    wram[wram_offset(s_btwr.addr)] = 0x00;

    // wJohtoBadges = 0: no badge boosts applied (all BIT checks in BadgeStatBoosts fail)
    wram[wram_offset(s_jbadge.addr)] = 0x00;

    // wBattleMonStatus = 0x0000 (2 bytes): no PAR/BRN → ApplyPrzEffectOnSpeed and
    // ApplyBrnEffectOnAttack check bit 2 (PAR) and bit 3 (BRN) respectively; both are 0.
    wram[wram_offset(s_bmon_status.addr)+0] = 0x00;
    wram[wram_offset(s_bmon_status.addr)+1] = 0x00;

    // wEnemyMonStatus = 0x0000
    wram[wram_offset(s_emon_status.addr)+0] = 0x00;
    wram[wram_offset(s_emon_status.addr)+1] = 0x00;

    // wPlayerStatLevels: pre-Haze stage deltas (will be overwritten to 7 by .Fill)
    // We write them anyway to verify that the pre-Haze stages don't affect the output
    // (they shouldn't — Haze always resets to neutral regardless of input).
    {
        uint8_t* p = wram + wram_offset(s_pstages.addr);
        for (int i=0; i<7; i++) p[i] = (uint8_t)(7 + player_deltas[i]);
        p[7] = 7;  // 8th slot (Curse/ability) — not used by Haze
    }
    {
        uint8_t* p = wram + wram_offset(s_estages.addr);
        for (int i=0; i<7; i++) p[i] = (uint8_t)(7 + enemy_deltas[i]);
        p[7] = 7;
    }

    // wPlayerStats (C6B6): base stats for CalcBattleStats (DE register source).
    // 5 × big-endian uint16: ATK, DEF, SPD, SATK, SDEF.
    // With neutral stages (7 → 1/1 multiplier), output = base.
    {
        uint8_t* p = wram + wram_offset(s_pstats.addr);
        auto be16 = [](uint8_t* d, uint16_t v){ d[0]=(v>>8)&0xFF; d[1]=v&0xFF; };
        be16(p+0, PLAYER_BASE.atk);
        be16(p+2, PLAYER_BASE.def);
        be16(p+4, PLAYER_BASE.spd);
        be16(p+6, PLAYER_BASE.satk);
        be16(p+8, PLAYER_BASE.sdef);
    }

    // wEnemyStats (C6C1): base stats for CalcBattleStats (enemy side).
    {
        uint8_t* p = wram + wram_offset(s_estats.addr);
        auto be16 = [](uint8_t* d, uint16_t v){ d[0]=(v>>8)&0xFF; d[1]=v&0xFF; };
        be16(p+0, ENEMY_BASE.atk);
        be16(p+2, ENEMY_BASE.def);
        be16(p+4, ENEMY_BASE.spd);
        be16(p+6, ENEMY_BASE.satk);
        be16(p+8, ENEMY_BASE.sdef);
    }

    // hBattleTurn = 0 (player turn) — saved by BattleCommand_ResetStats before the
    // SetPlayerTurn/SetEnemyTurn calls, then restored at the end.
    GB_write_memory(&gb, s_hbt.addr, 0x00);

    // hROMBank (FF9D): must contain the currently-mapped ROM bank.
    // BankSwitch (RST $08 handler at 0x2D63) reads hROMBank to save the current bank
    // before switching to the target bank, then restores it afterward.
    // We set it to 0x0D (the bank we're executing in) so the restore path is correct.
    GB_write_memory(&gb, s_hrom_bank.addr, s_haze.bank);

    // Disable all interrupts: IE=0 (0xFFFF), IF=0 (0xFF0F)
    // DI in the trampoline sets IME=0. Clearing IE/IF is belt-and-suspenders.
    GB_write_memory(&gb, 0xFFFF, 0x00);
    GB_write_memory(&gb, 0xFF0F, 0x00);

    // --- Set CPU state: PC = trampoline, SP = 0xFFEE with sentinel return address ---
    GB_registers_t* regs = GB_get_registers(&gb);
    if (!regs) { std::cerr<<"  [sb] cannot get registers\n"; GB_free(&gb); return false; }

    // Push sentinel address (0x0150) onto the SM83 stack.
    // SM83 CALL convention: SP-=2, [SP]=low, [SP+1]=high.
    // BattleCommand_ResetStats ends with CALL AnimateCurrentMove (intercepted) then
    // JP StdBattleTextbox (intercepted). After StdBattleTextbox's patched RET, the
    // call returns to BattleCommand_ResetStats's frame, which pops sentinel → PC=0x0150.
    //
    // Wait — the actual CALL structure is:
    //   CALL AnimateCurrentMove  ; pushes return addr (0x37131 in bank 0D) → SP-=2
    //   ; AnimateCurrentMove patched to RET → pops 0x37131 → resumes here
    //   LD HL, EliminatedStatsText ; loads HL with text pointer
    //   JP StdBattleTextbox        ; plain JP (no push) → goes to 0x3AD5
    //   ; StdBattleTextbox patched to RET → pops TOP OF STACK → our sentinel!
    //
    // Stack at the JP StdBattleTextbox point:
    //   SP = initial SP - 2 (the two bytes we pushed for BattleCommand_ResetStats RET)
    // So the sentinel is correctly at [SP] when StdBattleTextbox's RET executes.
    static constexpr uint16_t INITIAL_SP = 0xFFF0;
    GB_write_memory(&gb, INITIAL_SP - 1, (SENTINEL_PC >> 8) & 0xFF);  // [0xFFEF] = 0x01
    GB_write_memory(&gb, INITIAL_SP - 2,  SENTINEL_PC       & 0xFF);  // [0xFFEE] = 0x50
    regs->sp = INITIAL_SP - 2;  // SP = 0xFFEE
    regs->pc = TRAMPOLINE_ADDR; // PC = 0x0100

    // --- Execute until sentinel (or timeout) ---
    // Full path: ~256 NOPs in boot ROM range + trampoline + BattleCommand_ResetStats.
    // CalcBattleStats: 5 stats × Multiply + Divide = ~100 instructions each → ~1000 total.
    // BankSwitch overhead: ~20 instructions per banked call × ~5 calls → ~100.
    // Total estimate: < 5000 instructions. 200000 is a generous safety margin.
    static constexpr int MAX_INSN = 200000;
    int count = 0;
    while (!sentinel.triggered && count < MAX_INSN) {
        GB_run(&gb);
        ++count;
    }

    if (!sentinel.triggered) {
        std::cerr << "  [exec] sentinel not triggered after " << count << " instructions"
                  << " (PC=0x" << std::hex << GB_get_registers(&gb)->pc << std::dec << ")\n";
        GB_free(&gb); return false;
    }

    // --- Read semantic outputs ---
    wram_sz = 0;
    wram = reinterpret_cast<uint8_t*>(
        GB_get_direct_access(&gb, GB_DIRECT_ACCESS_RAM, &wram_sz, &wbank));
    if (!wram) { std::cerr<<"  [sb] cannot re-read WRAM\n"; GB_free(&gb); return false; }

    // Stat stage levels — BattleCommand_ResetStats .Fill resets these to 7
    {
        const uint8_t* p = wram + wram_offset(s_pstages.addr);
        for (int i=0; i<7; i++) out->player_stages[i] = p[i];
    }
    {
        const uint8_t* p = wram + wram_offset(s_estages.addr);
        for (int i=0; i<7; i++) out->enemy_stages[i] = p[i];
    }

    // Computed battle stats — CalcBattleStats output (big-endian in WRAM)
    // wBattleMonAttack (s_bmon_atk.addr): ATK, DEF, SPD, SATK, SDEF × 2 bytes each
    {
        const uint8_t* p = wram + wram_offset(s_bmon_atk.addr);
        for (int i=0; i<5; i++) out->player_stats[i] = (uint16_t)((p[i*2]<<8)|p[i*2+1]);
    }
    // wEnemyMonAttack (s_emon_atk.addr)
    {
        const uint8_t* p = wram + wram_offset(s_emon_atk.addr);
        for (int i=0; i<5; i++) out->enemy_stats[i] = (uint16_t)((p[i*2]<<8)|p[i*2+1]);
    }

    std::cout << "  [crystal poison=0x" << std::hex << (int)poison << std::dec
              << "] " << count << " instructions\n";

    GB_free(&gb);
    return true;
}

// ============================================================================
// Enginemon helpers
// ============================================================================
static std::vector<crystal::PackageWriter::MoveDataEntry>
extract_moves_local(const crystal::RomData& rom, const crystal::ExtractionProfile& profile) {
    std::vector<crystal::PackageWriter::MoveDataEntry> entries;
    const auto& o=profile.offsets; const auto& fmt=profile.format.move;
    const auto& c=profile.counts;
    if (!o.moves) return entries;
    entries.reserve(c.num_moves);
    for (uint16_t i=1; i<=c.num_moves; ++i) {
        uint32_t addr=o.moves+(uint32_t)(i-1)*fmt.move_data_size;
        auto rec=rom.read_bytes(addr,fmt.move_data_size);
        crystal::PackageWriter::MoveDataEntry e;
        e.id=i; e.type_id=rec[fmt.type_offset]; e.power=rec[fmt.power_offset];
        e.accuracy=rec[fmt.accuracy_offset]; e.pp=rec[fmt.pp_offset];
        e.effect_chance=rec[fmt.effect_chance_offset];
        uint8_t raw=rec[fmt.effect_offset];
        e.effect_id=crystal::to_semantic_effect(raw); e.raw_crystal_effect=raw;
        if(fmt.category_offset!=0xFF&&fmt.category_offset<fmt.move_data_size){
            uint8_t cv=rec[fmt.category_offset];
            e.category=(cv<=2u)?cv:(uint8_t)enginemon::MoveCategory::Physical;
        } else { e.category=(uint8_t)crystal::crystal_move_category_from_type(e.type_id,e.power); }
        entries.push_back(e);
    }
    return entries;
}

static std::optional<enginemon::Registry<enginemon::MoveId,enginemon::MoveData>> load_moves() {
    auto entries=extract_moves_local(*g_rom,*g_profile);
    if (!crystal::semanticize_move_entries(*g_rom,*g_profile,entries)) return std::nullopt;
    crystal::PackageWriter w; w.set_source_rom(std::string(40,'x'),"haze_diff");
    w.add_move_data(entries);
    auto pkg=std::filesystem::temp_directory_path()/"haze_diff.emon";
    if (!w.write(pkg)) return std::nullopt;
    auto rdr=enginemon::PackageReader::open(pkg);
    if (!rdr) { std::filesystem::remove(pkg); return std::nullopt; }
    auto reg=rdr->load_move_registry(); std::filesystem::remove(pkg); return reg;
}

static enginemon::BattleRules load_rules() {
    auto res=crystal::extract_battle_rules(*g_rom,*g_profile);
    return res.success ? res.rules : enginemon::BattleRules{};
}

static enginemon::BattlePokemon make_mon(
    enginemon::MoveId mid,
    const BaseStats& base,
    int8_t s_atk, int8_t s_def, int8_t s_spd,
    int8_t s_satk, int8_t s_sdef, int8_t s_acc, int8_t s_eva)
{
    enginemon::BattlePokemon bp{};
    bp.species=1; bp.type1=0; bp.type2=0; bp.level=50;
    bp.stats.hp=bp.stats.max_hp=300; bp.base_stats.hp=bp.base_stats.max_hp=300;
    bp.stats.attack=bp.base_stats.attack=base.atk;
    bp.stats.defense=bp.base_stats.defense=base.def;
    bp.stats.speed=bp.base_stats.speed=base.spd;
    bp.stats.special_attack=bp.base_stats.special_attack=base.satk;
    bp.stats.special_defense=bp.base_stats.special_defense=base.sdef;
    bp.happiness=200; bp.dv_atk=bp.dv_def=bp.dv_spd=bp.dv_spc=15;
    bp.moves[0].move=mid; bp.moves[0].pp=bp.moves[0].max_pp=10;
    bp.stages.attack=s_atk; bp.stages.defense=s_def; bp.stages.speed=s_spd;
    bp.stages.special_attack=s_satk; bp.stages.special_defense=s_sdef;
    bp.stages.accuracy=s_acc; bp.stages.evasion=s_eva;
    return bp;
}

// ============================================================================
// The differential test
// ============================================================================
static void haze_live_crystal_vs_enginemon() {
    std::cout << "\n=== haze_live_crystal_vs_enginemon ===\n";
    std::cout << "  SameBoy commit: " << SAMEBOY_COMMIT << "\n";
    std::cout << "  Crystal ROM SHA1: " << CRYSTAL_ROM_SHA1 << "\n";
    std::cout << "  Player base: atk=" << PLAYER_BASE.atk << " def=" << PLAYER_BASE.def
              << " spd=" << PLAYER_BASE.spd << " satk=" << PLAYER_BASE.satk
              << " sdef=" << PLAYER_BASE.sdef << "\n";
    std::cout << "  Enemy  base: atk=" << ENEMY_BASE.atk << " def=" << ENEMY_BASE.def
              << " spd=" << ENEMY_BASE.spd << " satk=" << ENEMY_BASE.satk
              << " sdef=" << ENEMY_BASE.sdef << "\n";

    // Pre-Haze stage deltas (from neutral = 0)
    constexpr int8_t PS_ATK=+2, PS_DEF=-3, PS_SPD=+1, PS_SATK=-1, PS_SDEF=+3, PS_ACC=+4, PS_EVA=-2;
    constexpr int8_t ES_ATK=-4, ES_DEF=+6, ES_SPD=-2, ES_SATK=+3, ES_SDEF=-1, ES_ACC=-3, ES_EVA=+5;
    const int8_t p_deltas[7] = {PS_ATK,PS_DEF,PS_SPD,PS_SATK,PS_SDEF,PS_ACC,PS_EVA};
    const int8_t e_deltas[7] = {ES_ATK,ES_DEF,ES_SPD,ES_SATK,ES_SDEF,ES_ACC,ES_EVA};

    // --- CRYSTAL RUN 1: poison=0x00 ---
    CrystalHazeSnapshot sc1{};
    bool ok1 = run_crystal_haze(0x00, p_deltas, e_deltas, &sc1);
    ASSERT_TRUE(ok1);
    if (!ok1) { std::cerr<<"  Crystal run 1 failed\n"; return; }

    // --- CRYSTAL RUN 2: poison=0xA5 ---
    CrystalHazeSnapshot sc2{};
    bool ok2 = run_crystal_haze(0xA5, p_deltas, e_deltas, &sc2);
    ASSERT_TRUE(ok2);
    if (!ok2) { std::cerr<<"  Crystal run 2 failed\n"; return; }

    // --- POISON STABILITY: both Crystal runs must agree on all semantic outputs ---
    bool stable = true;
    for (int i=0; i<7; i++) {
        if (sc1.player_stages[i] != sc2.player_stages[i]) { stable=false; break; }
        if (sc1.enemy_stages[i]  != sc2.enemy_stages[i])  { stable=false; break; }
    }
    for (int i=0; i<5 && stable; i++) {
        if (sc1.player_stats[i] != sc2.player_stats[i]) { stable=false; break; }
        if (sc1.enemy_stats[i]  != sc2.enemy_stats[i])  { stable=false; break; }
    }
    std::cout << "  Poison stability: " << (stable ? "YES" : "NO (FAIL)") << "\n";
    ASSERT_TRUE(stable);

    std::cout << "  Crystal stages post-Haze (raw, neutral=7): player=";
    for (auto s : sc1.player_stages) std::cout << (int)s << " ";
    std::cout << "  enemy=";
    for (auto s : sc1.enemy_stages) std::cout << (int)s << " ";
    std::cout << "\n";
    std::cout << "  Crystal player computed stats: ";
    for (auto s : sc1.player_stats) std::cout << s << " ";
    std::cout << "\n";
    std::cout << "  Crystal enemy computed stats:  ";
    for (auto s : sc1.enemy_stats) std::cout << s << " ";
    std::cout << "\n";

    // --- ENGINEMON RUN ---
    auto rules = load_rules();
    auto moves_opt = load_moves();
    ASSERT_TRUE(moves_opt.has_value());
    if (!moves_opt) return;

    constexpr enginemon::MoveId HAZE_ID = 114;
    const enginemon::MoveData* haze_md = moves_opt->get(HAZE_ID);
    ASSERT_TRUE(haze_md != nullptr && haze_md->effect_desc.is_supported);
    if (!haze_md) return;

    enginemon::Registries reg{}; reg.moves = *moves_opt;
    enginemon::Party party;
    { enginemon::Pokemon pm{}; pm.species=1; pm.level=50;
      pm.current_hp=pm.max_hp=300; pm.friendship=200; party.add(pm); }
    enginemon::Battle bat(enginemon::BattleType::Wild, party, reg, rules);

    // Same base stats as the Crystal fixture; same pre-Haze stage deltas.
    bat.player_pokemon() = make_mon(HAZE_ID,
        PLAYER_BASE,
        PS_ATK, PS_DEF, PS_SPD, PS_SATK, PS_SDEF, PS_ACC, PS_EVA);
    bat.opponent_pokemon() = make_mon(enginemon::MOVE_NONE,
        ENEMY_BASE,
        ES_ATK, ES_DEF, ES_SPD, ES_SATK, ES_SDEF, ES_ACC, ES_EVA);

    bat.set_rng_callback([]()->uint32_t{ return 0xFF; });
    bat.set_player_action(enginemon::ActionFight{0,0});
    bat.set_opponent_action(enginemon::ActionFight{0,0});
    bat.execute_turn();

    const auto& ps = bat.player_pokemon().stages;
    const auto& os = bat.opponent_pokemon().stages;
    std::cout << "  Enginemon stages post-Haze (delta, neutral=0): player="
              << (int)ps.attack<<"/"<<(int)ps.defense<<"/"<<(int)ps.speed<<"/"
              << (int)ps.special_attack<<"/"<<(int)ps.special_defense<<"/"
              << (int)ps.accuracy<<"/"<<(int)ps.evasion
              << "  enemy="
              << (int)os.attack<<"/"<<(int)os.defense<<"/"<<(int)os.speed<<"/"
              << (int)os.special_attack<<"/"<<(int)os.special_defense<<"/"
              << (int)os.accuracy<<"/"<<(int)os.evasion << "\n";

    // After Haze, Enginemon computes active stats from the reset stages.
    // Get the computed stats from the Enginemon side (identical to base_stat × 1.0).
    const auto& pp = bat.player_pokemon();
    const auto& op = bat.opponent_pokemon();

    // --- NORMALIZE AND COMPARE ---
    // Stage comparison: crystal_raw - 7 == enginemon_stage (both should be 0)
    // Stat comparison: Crystal wBattleMonAttack == Enginemon computed stat
    //   (neutral stage → multiplier = 1/1 → computed = base)
    const char* stage_names[] = {"ATK","DEF","SPD","SATK","SDEF","ACC","EVA"};
    const char* stat_names[]  = {"ATK","DEF","SPD","SATK","SDEF"};
    const int8_t* em_ps[] = {&ps.attack,&ps.defense,&ps.speed,&ps.special_attack,&ps.special_defense,&ps.accuracy,&ps.evasion};
    const int8_t* em_os[] = {&os.attack,&os.defense,&os.speed,&os.special_attack,&os.special_defense,&os.accuracy,&os.evasion};
    const int16_t em_pp_stats[] = {
        pp.stats.attack, pp.stats.defense, pp.stats.speed,
        pp.stats.special_attack, pp.stats.special_defense
    };
    const int16_t em_op_stats[] = {
        op.stats.attack, op.stats.defense, op.stats.speed,
        op.stats.special_attack, op.stats.special_defense
    };

    bool all_match = true;

    std::cout << "\n  --- Stage comparison ---\n";
    for (int i=0; i<7; i++) {
        int8_t cd = int8_t(int(sc1.player_stages[i]) - 7);
        int8_t ed = *em_ps[i];
        bool ok = (cd == ed);
        if (!ok) all_match = false;
        std::cout << "  player." << stage_names[i]
                  << ": crystal_delta=" << (int)cd
                  << " enginemon=" << (int)ed
                  << (ok ? " MATCH" : " MISMATCH") << "\n";
        ASSERT_EQ((int)cd, (int)ed);
    }
    for (int i=0; i<7; i++) {
        int8_t cd = int8_t(int(sc1.enemy_stages[i]) - 7);
        int8_t ed = *em_os[i];
        bool ok = (cd == ed);
        if (!ok) all_match = false;
        std::cout << "  enemy." << stage_names[i]
                  << ": crystal_delta=" << (int)cd
                  << " enginemon=" << (int)ed
                  << (ok ? " MATCH" : " MISMATCH") << "\n";
        ASSERT_EQ((int)cd, (int)ed);
    }

    std::cout << "\n  --- Computed stat comparison (neutral stage → computed == base) ---\n";
    for (int i=0; i<5; i++) {
        uint16_t cr = sc1.player_stats[i];
        uint16_t em = (uint16_t)em_pp_stats[i];
        bool ok = (cr == em);
        if (!ok) all_match = false;
        std::cout << "  player." << stat_names[i]
                  << ": crystal=" << cr
                  << " enginemon=" << em
                  << (ok ? " MATCH" : " MISMATCH") << "\n";
        ASSERT_EQ((int)cr, (int)em);
    }
    for (int i=0; i<5; i++) {
        uint16_t cr = sc1.enemy_stats[i];
        uint16_t em = (uint16_t)em_op_stats[i];
        bool ok = (cr == em);
        if (!ok) all_match = false;
        std::cout << "  enemy." << stat_names[i]
                  << ": crystal=" << cr
                  << " enginemon=" << em
                  << (ok ? " MATCH" : " MISMATCH") << "\n";
        ASSERT_EQ((int)cr, (int)em);
    }

    std::cout << "\n  RESULT: " << (all_match ? "crystal == enginemon" : "MISMATCH") << "\n";
}

// ============================================================================
// main
// ============================================================================
int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::cerr << "Usage: crystal_haze_diff_test <rom_path> <sym_path>\n";
        return 1;
    }
    g_rom_path = argv[1];
    g_sym_path = argv[2];

    // --- Enforce ROM SHA-1 ---
    std::string actual_sha1 = sha1_of_file(g_rom_path);
    if (actual_sha1.empty()) {
        std::cerr << "Cannot read ROM: " << g_rom_path << "\n"; return 1;
    }
    std::cout << "ROM SHA-1: " << actual_sha1 << "\n";
    if (actual_sha1 != std::string(CRYSTAL_ROM_SHA1)) {
        std::cerr << "FATAL: ROM SHA-1 mismatch.\n"
                  << "  Expected: " << CRYSTAL_ROM_SHA1 << "\n"
                  << "  Actual:   " << actual_sha1 << "\n"
                  << "  This oracle requires Crystal v1.1 (UE) [C][!].\n";
        return 1;
    }
    std::cout << "ROM hash enforced: OK\n";

    // --- Load ROM via Enginemon (for rules/move extraction) ---
    auto rom = crystal::RomData::load(std::filesystem::path(g_rom_path));
    if (!rom) { std::cerr << "Failed to load ROM via Enginemon\n"; return 1; }
    const crystal::ExtractionProfile* profile =
        crystal::ProfileRegistry::instance().get_profile_by_hash(rom->hash());
    if (!profile) { std::cerr << "No Enginemon profile for ROM\n"; return 1; }
    g_rom = rom.get(); g_profile = profile;

    std::cout << "\n=== Crystal Live Haze Differential ===\n";
    std::cout << "  SameBoy: " << SAMEBOY_COMMIT << "\n";
    std::cout << "  .sym: " << g_sym_path << "\n\n";

    RUN_TEST(haze_live_crystal_vs_enginemon);

    std::cout << "\n=== Results ===\n";
    std::cout << "Passed: " << g_passed << "\n";
    std::cout << "Failed: " << g_failed << "\n";
    return g_failed > 0 ? 1 : 0;
}
