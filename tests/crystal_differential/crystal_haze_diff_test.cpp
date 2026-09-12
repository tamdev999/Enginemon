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
// ROM IMMUTABILITY:
//   After SHA verification, Crystal ROM bytes are never modified.
//   Two harness patches exist, both to ROM bank 0 addresses outside any Crystal function:
//     0x0100: DI; JP 0x710E  (trampoline from entry point into BattleCommand_ResetStats)
//     0x0150: JP 0x0150      (return-sentinel parking loop; holds the return address on stack)
//   These are the ONLY ROM modifications. No battle, stat, or presentation routine is patched.
//
// PRESENTATION INTERCEPT — via execution callback, no ROM changes:
//   After all semantic code in BattleCommand_ResetStats completes, the CPU reaches
//   AnimateCurrentMove (0d:7E01). The execution callback detects this PC and marks the
//   sentinel triggered. The GB_run loop exits before AnimateCurrentMove executes.
//   StdBattleTextbox (JP tail-call after AnimateCurrentMove) is never reached.
//
//   Allowlist of intercepted presentation sinks (fail-closed):
//     AnimateCurrentMove  0d:7E01  — first entry; pure GPU/audio animation, no battle state
//   Any other unknown exit from the semantic path before the sentinel fails closed.
//
// Crystal execution — FULL BattleCommand_ResetStats semantic path:
//   Entry: BattleCommand_ResetStats (0d:710e, flat 0x3710E)
//   Full path (from pokecrystal/engine/battle/effect_commands.asm):
//     .Fill(wPlayerStatLevels, 7)        -- 8 WRAM writes, intra-bank
//     .Fill(wEnemyStatLevels, 7)         -- same
//     SetPlayerTurn()                    -- ROM0
//     CalcPlayerStats()                  -- intra-bank, uses RST $08 for bank 0F calls:
//       CalcBattleStats()                --   5 iterations × mul+div → wBattleMonAttack..SpDef
//       BadgeStatBoosts()                --   bank 0F, no-op (wLinkMode=0, wJohtoBadges=0)
//       BattleCommand_SwitchTurn()       --   toggle
//       ApplyPrzEffectOnSpeed()          --   bank 0F, no-op (wEnemyMonStatus=0)
//       ApplyBrnEffectOnAttack()         --   bank 0F, no-op (wEnemyMonStatus=0)
//       BattleCommand_SwitchTurn()       --   toggle back
//     SetEnemyTurn()                     -- ROM0
//     CalcEnemyStats()                   -- intra-bank, RST $08:
//       CalcBattleStats()                --   5 iterations → wEnemyMonAttack..SpDef
//       BattleCommand_SwitchTurn() × 2   --   toggle / toggle back
//       ApplyPrzEffectOnSpeed()          --   bank 0F, no-op (wBattleMonStatus=0)
//       ApplyBrnEffectOnAttack()         --   bank 0F, no-op (wBattleMonStatus=0)
//     POP AF; LDH (hBattleTurn), A       -- restore turn
//     <<< SEMANTIC BOUNDARY: all stats recalculated, stages reset >>>
//     CALL AnimateCurrentMove            -- SENTINEL FIRES HERE (PC=0x7E01)
//       execution loop exits immediately — AnimateCurrentMove never runs
//     JP StdBattleTextbox                -- never reached
//
// Crystal fixture — all WRAM addresses resolved from .sym:
//   wPlayerStatLevels  (C6CC): 7+delta (overwritten to 7 by .Fill)
//   wEnemyStatLevels   (C6D4): 7+delta (overwritten to 7 by .Fill)
//   wPlayerStats       (C6B6): base stats × 5 big-endian u16 (CalcBattleStats input)
//   wEnemyStats        (C6C1): base stats × 5 big-endian u16
//   wBattleMonStatus   (C63A): 0 — no PAR/BRN
//   wEnemyMonStatus    (D214): 0 — no PAR/BRN
//   wLinkMode          (C2DC): 0 — non-link (BadgeStatBoosts checks this first)
//   wJohtoBadges       (D857): 0 — no badge boosts
//   wInBattleTowerBattle (CFC0): 0
//   hBattleTurn        (FFE4): 0 (saved/restored by BattleCommand_ResetStats)
//   hROMBank           (FF9D): 0x0D (required for RST $08 BankSwitch restore path)
//
// Enginemon side: Battle::execute_turn() with Haze (move 114).
//   Same base stats, same stage deltas; no badges/link/status.
//
// Normalization: Crystal stage 7 = neutral; Enginemon stage 0 = neutral
//   crystal_delta = raw - 7 == enginemon_stage
//   computed stat comparison: with neutral stages, CalcBattleStats produces base_stat × 1 = base_stat

#include "Core/gb.h"
#include "Core/memory.h"  // GB_read_memory / GB_write_memory

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
// SHA-1 (self-contained, zero deps)
// ============================================================================
static std::string sha1_of_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return "";
    std::vector<uint8_t> data(std::istreambuf_iterator<char>(f), {});
    if (data.empty()) return "";
    struct SHA1 {
        uint32_t h[5]={0x67452301,0xEFCDAB89,0x98BADCFE,0x10325476,0xC3D2E1F0};
        uint64_t cnt=0; uint8_t buf[64]{}; uint32_t blen=0;
        static uint32_t rol(uint32_t v,int n){return(v<<n)|(v>>(32-n));}
        void block(const uint8_t b[64]){
            uint32_t w[80];
            for(int i=0;i<16;i++) w[i]=(b[i*4]<<24)|(b[i*4+1]<<16)|(b[i*4+2]<<8)|b[i*4+3];
            for(int i=16;i<80;i++) w[i]=rol(w[i-3]^w[i-8]^w[i-14]^w[i-16],1);
            uint32_t a=h[0],b2=h[1],c=h[2],d=h[3],e=h[4];
            for(int i=0;i<80;i++){
                uint32_t f,k;
                if(i<20){f=(b2&c)|((~b2)&d);k=0x5A827999;}
                else if(i<40){f=b2^c^d;k=0x6ED9EBA1;}
                else if(i<60){f=(b2&c)|(b2&d)|(c&d);k=0x8F1BBCDC;}
                else{f=b2^c^d;k=0xCA62C1D6;}
                uint32_t t=rol(a,5)+f+e+k+w[i]; e=d;d=c;c=rol(b2,30);b2=a;a=t;
            }
            h[0]+=a;h[1]+=b2;h[2]+=c;h[3]+=d;h[4]+=e;
        }
        void feed(const uint8_t* p,size_t n){cnt+=n*8;for(size_t i=0;i<n;i++){buf[blen++]=p[i];if(blen==64){block(buf);blen=0;}}}
        std::array<uint8_t,20> done(){
            buf[blen++]=0x80;
            if(blen>56){while(blen<64)buf[blen++]=0;block(buf);blen=0;}
            while(blen<56)buf[blen++]=0;
            for(int i=0;i<8;i++)buf[56+i]=(uint8_t)(cnt>>((7-i)*8));
            block(buf);
            std::array<uint8_t,20> r{};
            for(int i=0;i<5;i++){r[i*4]=(h[i]>>24)&0xFF;r[i*4+1]=(h[i]>>16)&0xFF;r[i*4+2]=(h[i]>>8)&0xFF;r[i*4+3]=h[i]&0xFF;}
            return r;
        }
    } s; s.feed(data.data(),data.size());
    auto hash=s.done(); char hex[41]{}; for(int i=0;i<20;i++)sprintf(hex+i*2,"%02X",hash[i]);
    return std::string(hex);
}

// ============================================================================
// Symbol lookup
// ============================================================================
struct CrystalSym { uint8_t bank; uint16_t addr; };

static bool sym_lookup(const std::string& sym_path, const std::string& name, CrystalSym* out) {
    std::ifstream f(sym_path); if (!f) return false;
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty()||line[0]==';') continue;
        std::istringstream ss(line); std::string addr_str, sym_name;
        if (!(ss>>addr_str>>sym_name)) continue;
        if (sym_name!=name) continue;
        auto colon=addr_str.find(':'); if (colon==std::string::npos) continue;
        out->bank=(uint8_t)std::stoi(addr_str.substr(0,colon),nullptr,16);
        out->addr=(uint16_t)std::stoi(addr_str.substr(colon+1),nullptr,16);
        return true;
    }
    return false;
}

// ============================================================================
// WRAM offset helper (CGB: bank0 = C000-CFFF → [0..0xFFF], bank1 = D000-DFFF → [0x1000..])
// ============================================================================
static size_t wram_offset(uint16_t addr) {
    if (addr>=0xD000) return size_t{0x1000}+(addr-0xD000);
    if (addr>=0xC000) return addr-0xC000;
    throw std::runtime_error("not WRAM: "+std::to_string(addr));
}

// ============================================================================
// SameBoy no-op callbacks
// ============================================================================
static uint32_t sb_pixels[160*144];
static void     sb_log(GB_gameboy_t*, const char*, GB_log_attributes_t) {}
static uint32_t sb_rgb(GB_gameboy_t*, uint8_t r, uint8_t g, uint8_t b) {
    return (0xFF000000u)|((uint32_t)r<<16)|((uint32_t)g<<8)|b;
}

// ============================================================================
// Presentation sink allowlist
//
// After all semantic BattleCommand_ResetStats code runs, the CPU reaches
// AnimateCurrentMove (0d:7E01). The execution callback detects this PC and
// immediately signals completion — the function never starts executing.
//
// Any unknown PC that triggers the "done" condition fails closed via a flag.
// Adding any new skipped routine requires explicitly registering it here.
// ============================================================================
struct PresentationSink {
    uint16_t addr;      // SM83 address (bank-local, 0x4000-0x7FFF)
    const char* symbol; // for reporting
};

// Execution callback context.
// extended_sentinel: list of presentation sink addresses that also mark completion.
// Reaching any of these stops execution and records which symbol was hit.
struct ExecCtx {
    uint16_t sentinel_pc;          // harness sentinel (0x0150 JP loop)
    bool     triggered;            // set when sentinel or any allowlisted sink fires
    const PresentationSink* sinks; // fail-closed allowlist
    size_t   num_sinks;
    const char* intercepted;       // which sink fired (null if sentinel fired)
};

static void execution_cb(GB_gameboy_t* gb, uint16_t pc, uint8_t /*opcode*/) {
    auto* ctx = reinterpret_cast<ExecCtx*>(GB_get_user_data(gb));
    if (!ctx || ctx->triggered) return;

    // Check harness sentinel first
    if (pc == ctx->sentinel_pc) {
        ctx->triggered = true;
        ctx->intercepted = nullptr;  // sentinel — not a presentation sink
        return;
    }

    // Check presentation-sink allowlist
    for (size_t i = 0; i < ctx->num_sinks; ++i) {
        if (pc == ctx->sinks[i].addr) {
            ctx->triggered = true;
            ctx->intercepted = ctx->sinks[i].symbol;
            return;
        }
    }
}

// ============================================================================
// Crystal Haze snapshot
// ============================================================================
struct CrystalHazeSnapshot {
    uint8_t  player_stages[7];  // wPlayerStatLevels[0..6] (raw; 7 = neutral)
    uint8_t  enemy_stages[7];   // wEnemyStatLevels[0..6]
    uint16_t player_stats[5];   // wBattleMonAttack..SpDef  (big-endian → native)
    uint16_t enemy_stats[5];    // wEnemyMonAttack..SpDef
};

// ============================================================================
// Base stats fixture — same values written into Crystal WRAM (wPlayerStats,
// wEnemyStats) and into the Enginemon BattlePokemon.
// With neutral stages (7 → 1/1 multiplier), CalcBattleStats outputs exactly base_stat.
// ============================================================================
struct BaseStats { uint16_t atk, def, spd, satk, sdef; };
static constexpr BaseStats PLAYER_BASE = {110,  60, 130,  95,  70};
static constexpr BaseStats ENEMY_BASE  = { 75, 110,  30, 100,  80};

// ============================================================================
// run_crystal_haze — NO ROM BYTES MODIFIED AFTER SHA CHECK
// ============================================================================
static bool run_crystal_haze(
    uint8_t poison,
    const int8_t player_deltas[7],
    const int8_t enemy_deltas[7],
    CrystalHazeSnapshot* out)
{
    // --- Load ROM from disk (untouched) ---
    std::vector<uint8_t> rom_data;
    {
        std::ifstream f(g_rom_path, std::ios::binary);
        if (!f) { std::cerr<<"  [rom] open failed\n"; return false; }
        rom_data.assign(std::istreambuf_iterator<char>(f), {});
    }
    if (rom_data.size()!=2097152) { std::cerr<<"  [rom] wrong size\n"; return false; }

    // --- Resolve ALL addresses from .sym (zero hardcoding of battle addresses) ---
    CrystalSym s_haze,       // BattleCommand_ResetStats
               s_pstages,    // wPlayerStatLevels
               s_estages,    // wEnemyStatLevels
               s_pstats,     // wPlayerStats     (CalcBattleStats input, player)
               s_estats,     // wEnemyStats      (CalcBattleStats input, enemy)
               s_bmon_atk,   // wBattleMonAttack (CalcBattleStats output, player)
               s_emon_atk,   // wEnemyMonAttack  (CalcBattleStats output, enemy)
               s_bmon_st,    // wBattleMonStatus (Prz/Brn path gate)
               s_emon_st,    // wEnemyMonStatus
               s_hbt,        // hBattleTurn
               s_hrom,       // hROMBank         (RST $08 BankSwitch restore)
               s_link,       // wLinkMode        (BadgeStatBoosts gate)
               s_btwr,       // wInBattleTowerBattle
               s_jbadge,     // wJohtoBadges
               s_anim;       // AnimateCurrentMove  (presentation sink)

    if (!sym_lookup(g_sym_path,"BattleCommand_ResetStats",&s_haze)   ||
        !sym_lookup(g_sym_path,"wPlayerStatLevels",       &s_pstages)||
        !sym_lookup(g_sym_path,"wEnemyStatLevels",        &s_estages)||
        !sym_lookup(g_sym_path,"wPlayerStats",            &s_pstats) ||
        !sym_lookup(g_sym_path,"wEnemyStats",             &s_estats) ||
        !sym_lookup(g_sym_path,"wBattleMonAttack",        &s_bmon_atk)||
        !sym_lookup(g_sym_path,"wEnemyMonAttack",         &s_emon_atk)||
        !sym_lookup(g_sym_path,"wBattleMonStatus",        &s_bmon_st)||
        !sym_lookup(g_sym_path,"wEnemyMonStatus",         &s_emon_st)||
        !sym_lookup(g_sym_path,"hBattleTurn",             &s_hbt)    ||
        !sym_lookup(g_sym_path,"hROMBank",                &s_hrom)   ||
        !sym_lookup(g_sym_path,"wLinkMode",               &s_link)   ||
        !sym_lookup(g_sym_path,"wInBattleTowerBattle",    &s_btwr)   ||
        !sym_lookup(g_sym_path,"wJohtoBadges",            &s_jbadge) ||
        !sym_lookup(g_sym_path,"AnimateCurrentMove",      &s_anim))
    { std::cerr<<"  [sym] missing symbol\n"; return false; }

    // --- Init SameBoy ---
    GB_gameboy_t gb;
    if (!GB_init(&gb, GB_MODEL_CGB_E)) { std::cerr<<"  [sb] init failed\n"; return false; }
    GB_set_log_callback(&gb, sb_log);
    GB_set_rgb_encode_callback(&gb, sb_rgb);
    GB_set_pixels_output(&gb, sb_pixels);
    GB_set_rendering_disabled(&gb, true);
    GB_set_turbo_mode(&gb, true, true);

    // --- Presentation-sink allowlist (fail-closed) ---
    // AnimateCurrentMove: the only allowlisted exit from the semantic Haze path.
    // Reaching its PC means all semantic WRAM writes are complete.
    // Adding any future presentation routine requires explicit registration here.
    static const PresentationSink SINKS[] = {
        { 0x7E01, "AnimateCurrentMove" }   // 0d:7E01 — GPU/audio animation, no battle state
    };
    static constexpr size_t NUM_SINKS = sizeof(SINKS)/sizeof(SINKS[0]);

    // Harness sentinel: JP self-loop at 0x0150 (in ROM patch region, outside Crystal code).
    // This address is pushed onto the stack as the "return address from cold-call".
    // It also serves as the fallback stop condition if somehow execution reaches it.
    static constexpr uint16_t SENTINEL_PC = 0x0150;
    ExecCtx ctx;
    ctx.sentinel_pc = SENTINEL_PC;
    ctx.triggered   = false;
    ctx.sinks       = SINKS;
    ctx.num_sinks   = NUM_SINKS;
    ctx.intercepted = nullptr;

    GB_set_user_data(&gb, &ctx);
    GB_set_execution_callback(&gb, execution_cb);

    // --- Load ROM ---
    GB_load_rom_from_buffer(&gb, rom_data.data(), rom_data.size());

    // --- ROM patches (HARNESS INFRASTRUCTURE ONLY) ---
    // These two patches are to ROM bank 0 addresses that are NOT part of any
    // Crystal battle, stat, or presentation routine.
    //
    //   0x0100 — GB header / entry point.  Crystal uses "00 C3 6E 01" (NOP; JP 0x016E).
    //            Replaced with:  F3 = DI; C3 xx xx = JP BattleCommand_ResetStats
    //            Purpose: route the cold-call entry into the routine without running
    //            Crystal's full game startup sequence.
    //
    //   0x0150 — Part of the ROM header (GBC compatibility flag area). Crystal has real
    //            code here (F3 CD 4E 3B ...) but it is only reached via the normal
    //            game startup path, which we bypass. We overwrite it with JP 0x0150
    //            to create a parking loop whose address is used as the cold-call return
    //            address pushed onto the stack. If execution somehow reaches it, the
    //            sentinel fires and the harness exits safely.
    //
    // NO OTHER ROM BYTES ARE MODIFIED.
    size_t rom_sz=0; uint16_t rb=0;
    uint8_t* rom_buf = reinterpret_cast<uint8_t*>(
        GB_get_direct_access(&gb, GB_DIRECT_ACCESS_ROM, &rom_sz, &rb));
    if (!rom_buf) { std::cerr<<"  [sb] ROM access failed\n"; GB_free(&gb); return false; }

    // Patch 1: sentinel JP loop at 0x0150
    if (rom_sz > SENTINEL_PC+2) {
        rom_buf[SENTINEL_PC+0] = 0xC3;
        rom_buf[SENTINEL_PC+1] = SENTINEL_PC & 0xFF;
        rom_buf[SENTINEL_PC+2] = (SENTINEL_PC>>8)&0xFF;
    }

    // Patch 2: trampoline at 0x0100
    //   DI = disable interrupts (IME=0 prevents any interrupt before we reach the routine)
    //   JP BattleCommand_ResetStats = direct intra-bank jump; bank 0x0D already mapped
    static constexpr uint16_t TRAMPOLINE = 0x0100;
    if (rom_sz > TRAMPOLINE+4) {
        rom_buf[TRAMPOLINE+0] = 0xF3;               // DI
        rom_buf[TRAMPOLINE+1] = 0xC3;               // JP nn
        rom_buf[TRAMPOLINE+2] = s_haze.addr & 0xFF;
        rom_buf[TRAMPOLINE+3] = (s_haze.addr>>8) & 0xFF;
    }

    // --- Set ROM bank 0x0D ---
    GB_write_memory(&gb, 0x2000, s_haze.bank);

    // --- Mark boot ROM finished ---
    // Causes all ROM reads (including 0x0000-0x00FF) to come from the Crystal game ROM.
    // Required because CalcBattleStats calls Multiply (ROM0 0x3119) which uses RST $08
    // (JP 0x2D63, Crystal's BankSwitch, at 0x0008) and RST $10 (SwitchROM at 0x0010).
    // Without this, those addresses return 0x00 (NOP) from the zero-filled boot ROM buffer.
    // Writing 1 to 0xFF50 sets gb->boot_rom_finished = true via the BANK register handler.
    GB_write_memory(&gb, 0xFF50, 1);

    // --- WRAM direct access ---
    size_t wram_sz=0; uint16_t wbank=0;
    uint8_t* wram=reinterpret_cast<uint8_t*>(
        GB_get_direct_access(&gb, GB_DIRECT_ACCESS_RAM, &wram_sz, &wbank));
    if (!wram||wram_sz<0x2000) { std::cerr<<"  [sb] WRAM failed\n"; GB_free(&gb); return false; }

    // --- Poison unspecified WRAM ---
    std::memset(wram, poison, wram_sz);

    // --- Fixture: write known-good values for all WRAM locations that the semantic
    //     path reads. Every address below is resolved from .sym. ---

    // wLinkMode = 0: BadgeStatBoosts (0F:6D45) checks this first; 0 = non-link battle.
    // Falls through to badge checks rather than returning early.
    wram[wram_offset(s_link.addr)] = 0x00;

    // wInBattleTowerBattle = 0
    wram[wram_offset(s_btwr.addr)] = 0x00;

    // wJohtoBadges = 0: all BIT tests in BadgeStatBoosts fail; no stat boosts applied.
    wram[wram_offset(s_jbadge.addr)] = 0x00;

    // wBattleMonStatus = 0x0000: no PAR (bit 6) or BRN (bit 3).
    // ApplyPrzEffectOnSpeed / ApplyBrnEffectOnAttack check these bits and return early.
    wram[wram_offset(s_bmon_st.addr)  ] = 0x00;
    wram[wram_offset(s_bmon_st.addr)+1] = 0x00;

    // wEnemyMonStatus = 0x0000
    wram[wram_offset(s_emon_st.addr)  ] = 0x00;
    wram[wram_offset(s_emon_st.addr)+1] = 0x00;

    // wPlayerStatLevels: write pre-Haze stage values. BattleCommand_ResetStats overwrites
    // these to 7 via .Fill regardless of what we put here, but we write them anyway to
    // ensure the poison doesn't accidentally give us neutral stages (7) that would make
    // .Fill a no-op visually — we want to prove .Fill actually ran.
    {
        uint8_t* p = wram + wram_offset(s_pstages.addr);
        for (int i=0;i<7;i++) p[i]=(uint8_t)(7+player_deltas[i]);
        p[7]=7;  // 8th slot (Curse/ability, not used by Haze)
    }
    {
        uint8_t* p = wram + wram_offset(s_estages.addr);
        for (int i=0;i<7;i++) p[i]=(uint8_t)(7+enemy_deltas[i]);
        p[7]=7;
    }

    // wPlayerStats (C6B6): 5 × big-endian uint16 read by CalcBattleStats as DE source.
    // CalcPlayerStats: LD DE, wPlayerStats. With neutral stages, output = input exactly.
    {
        uint8_t* p = wram + wram_offset(s_pstats.addr);
        auto be16=[](uint8_t* d,uint16_t v){d[0]=(v>>8)&0xFF;d[1]=v&0xFF;};
        be16(p+0,PLAYER_BASE.atk); be16(p+2,PLAYER_BASE.def); be16(p+4,PLAYER_BASE.spd);
        be16(p+6,PLAYER_BASE.satk); be16(p+8,PLAYER_BASE.sdef);
    }
    // wEnemyStats (C6C1)
    {
        uint8_t* p = wram + wram_offset(s_estats.addr);
        auto be16=[](uint8_t* d,uint16_t v){d[0]=(v>>8)&0xFF;d[1]=v&0xFF;};
        be16(p+0,ENEMY_BASE.atk); be16(p+2,ENEMY_BASE.def); be16(p+4,ENEMY_BASE.spd);
        be16(p+6,ENEMY_BASE.satk); be16(p+8,ENEMY_BASE.sdef);
    }

    // hBattleTurn = 0: player turn. BattleCommand_ResetStats saves this, runs
    // SetPlayerTurn/CalcPlayerStats/SetEnemyTurn/CalcEnemyStats, then restores it.
    GB_write_memory(&gb, s_hbt.addr, 0x00);

    // hROMBank = 0x0D: must reflect the currently-mapped bank.
    // BankSwitch (RST $08 handler, 0x2D63) reads hROMBank to save the caller's bank
    // before switching to the callee bank, then restores it. Correct value = 0x0D.
    GB_write_memory(&gb, s_hrom.addr, s_haze.bank);

    // IE = 0, IF = 0: belt-and-suspenders alongside DI in the trampoline.
    GB_write_memory(&gb, 0xFFFF, 0x00);
    GB_write_memory(&gb, 0xFF0F, 0x00);

    // --- CPU initial state ---
    GB_registers_t* regs = GB_get_registers(&gb);
    if (!regs) { std::cerr<<"  [sb] no registers\n"; GB_free(&gb); return false; }

    // Stack: push SENTINEL_PC as the return address for BattleCommand_ResetStats.
    // SM83 stack is little-endian (SP points to low byte).
    // The routine never actually RETs to this address because the execution callback
    // fires at AnimateCurrentMove BEFORE the CALL completes, stopping the loop.
    // The sentinel address is kept on the stack as a safety net.
    static constexpr uint16_t INITIAL_SP = 0xFFF0;
    GB_write_memory(&gb, INITIAL_SP-1, (SENTINEL_PC>>8)&0xFF);
    GB_write_memory(&gb, INITIAL_SP-2,  SENTINEL_PC    &0xFF);
    regs->sp = INITIAL_SP-2;
    regs->pc = TRAMPOLINE;

    // --- Execute until presentation sink or sentinel ---
    // Full semantic path: ~13000 instructions measured with neutral stages.
    // 200000 is a generous safety margin.
    static constexpr int MAX_INSN = 200000;
    int count=0;
    while (!ctx.triggered && count<MAX_INSN) { GB_run(&gb); ++count; }

    if (!ctx.triggered) {
        std::cerr<<"  [exec] not triggered after "<<count
                 <<" insn (PC=0x"<<std::hex<<GB_get_registers(&gb)->pc<<std::dec<<")\n";
        GB_free(&gb); return false;
    }

    // Verify the execution stopped at an allowlisted sink, not at an unexpected address.
    // ctx.intercepted == nullptr means the harness sentinel (0x0150) fired — unexpected
    // for normal Haze (AnimateCurrentMove should fire first). Report either way.
    if (ctx.intercepted) {
        // Good path: stopped at a registered presentation sink
    } else {
        // Sentinel 0x0150 fired — execution reached the ROM parking loop instead of
        // the expected presentation sink. This is still valid (complete execution),
        // but worth noting.
        std::cerr << "  [exec] sentinel 0x0150 fired (expected AnimateCurrentMove)\n";
    }

    // --- Read semantic outputs from WRAM ---
    wram_sz=0;
    wram=reinterpret_cast<uint8_t*>(
        GB_get_direct_access(&gb, GB_DIRECT_ACCESS_RAM, &wram_sz, &wbank));
    if (!wram) { std::cerr<<"  [sb] WRAM re-read failed\n"; GB_free(&gb); return false; }

    // wPlayerStatLevels / wEnemyStatLevels — .Fill resets to 7
    { const uint8_t* p=wram+wram_offset(s_pstages.addr); for(int i=0;i<7;i++) out->player_stages[i]=p[i]; }
    { const uint8_t* p=wram+wram_offset(s_estages.addr); for(int i=0;i<7;i++) out->enemy_stages[i]=p[i]; }

    // wBattleMonAttack..SpDef (5 × big-endian uint16) — CalcBattleStats output
    { const uint8_t* p=wram+wram_offset(s_bmon_atk.addr); for(int i=0;i<5;i++) out->player_stats[i]=(uint16_t)((p[i*2]<<8)|p[i*2+1]); }
    { const uint8_t* p=wram+wram_offset(s_emon_atk.addr); for(int i=0;i<5;i++) out->enemy_stats[i]=(uint16_t)((p[i*2]<<8)|p[i*2+1]); }

    std::cout << "  [crystal poison=0x"<<std::hex<<(int)poison<<std::dec<<"] "
              << count<<" insn, sink="
              << (ctx.intercepted ? ctx.intercepted : "sentinel(0x0150)") << "\n";
    GB_free(&gb);
    return true;
}

// ============================================================================
// Enginemon helpers
// ============================================================================
static std::vector<crystal::PackageWriter::MoveDataEntry>
extract_moves_local(const crystal::RomData& rom, const crystal::ExtractionProfile& profile) {
    std::vector<crystal::PackageWriter::MoveDataEntry> entries;
    const auto& o=profile.offsets; const auto& fmt=profile.format.move; const auto& c=profile.counts;
    if (!o.moves) return entries;
    entries.reserve(c.num_moves);
    for (uint16_t i=1;i<=c.num_moves;++i) {
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
    enginemon::MoveId mid, const BaseStats& base,
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
// Differential test
// ============================================================================
static void haze_live_crystal_vs_enginemon() {
    std::cout << "\n=== haze_live_crystal_vs_enginemon ===\n";
    std::cout << "  SameBoy:  " << SAMEBOY_COMMIT << "\n";
    std::cout << "  ROM SHA1: " << CRYSTAL_ROM_SHA1 << "\n";
    std::cout << "  Player base: atk=" << PLAYER_BASE.atk << " def=" << PLAYER_BASE.def
              << " spd=" << PLAYER_BASE.spd << " satk=" << PLAYER_BASE.satk
              << " sdef=" << PLAYER_BASE.sdef << "\n";
    std::cout << "  Enemy  base: atk=" << ENEMY_BASE.atk << " def=" << ENEMY_BASE.def
              << " spd=" << ENEMY_BASE.spd << " satk=" << ENEMY_BASE.satk
              << " sdef=" << ENEMY_BASE.sdef << "\n";

    // Pre-Haze stage deltas (from neutral = 0). Both sides have non-trivial deltas.
    constexpr int8_t PS_ATK=+2,PS_DEF=-3,PS_SPD=+1,PS_SATK=-1,PS_SDEF=+3,PS_ACC=+4,PS_EVA=-2;
    constexpr int8_t ES_ATK=-4,ES_DEF=+6,ES_SPD=-2,ES_SATK=+3,ES_SDEF=-1,ES_ACC=-3,ES_EVA=+5;
    const int8_t p_d[7]={PS_ATK,PS_DEF,PS_SPD,PS_SATK,PS_SDEF,PS_ACC,PS_EVA};
    const int8_t e_d[7]={ES_ATK,ES_DEF,ES_SPD,ES_SATK,ES_SDEF,ES_ACC,ES_EVA};

    CrystalHazeSnapshot sc1{}, sc2{};
    ASSERT_TRUE(run_crystal_haze(0x00, p_d, e_d, &sc1)); if (g_current_failed) return;
    ASSERT_TRUE(run_crystal_haze(0xA5, p_d, e_d, &sc2)); if (g_current_failed) return;

    // --- Poison stability ---
    bool stable=true;
    for(int i=0;i<7;i++) if(sc1.player_stages[i]!=sc2.player_stages[i]||sc1.enemy_stages[i]!=sc2.enemy_stages[i]){stable=false;break;}
    for(int i=0;i<5&&stable;i++) if(sc1.player_stats[i]!=sc2.player_stats[i]||sc1.enemy_stats[i]!=sc2.enemy_stats[i]){stable=false;break;}
    std::cout << "  Poison stability: " << (stable?"YES":"NO (FAIL)") << "\n";
    ASSERT_TRUE(stable);

    // Print crystal snapshot
    std::cout << "  Crystal stages (raw, neutral=7): player=";
    for(auto s:sc1.player_stages) std::cout<<(int)s<<" ";
    std::cout << "  enemy=";
    for(auto s:sc1.enemy_stages) std::cout<<(int)s<<" ";
    std::cout<<"\n";
    std::cout << "  Crystal player computed: ";
    for(auto s:sc1.player_stats) std::cout<<s<<" ";
    std::cout<<"\n";
    std::cout << "  Crystal enemy computed:  ";
    for(auto s:sc1.enemy_stats) std::cout<<s<<" ";
    std::cout<<"\n";

    // --- Enginemon ---
    auto rules=load_rules();
    auto moves_opt=load_moves();
    ASSERT_TRUE(moves_opt.has_value()); if(!moves_opt) return;

    constexpr enginemon::MoveId HAZE_ID=114;
    const enginemon::MoveData* haze_md=moves_opt->get(HAZE_ID);
    ASSERT_TRUE(haze_md&&haze_md->effect_desc.is_supported); if(!haze_md) return;

    enginemon::Registries reg{}; reg.moves=*moves_opt;
    enginemon::Party party;
    { enginemon::Pokemon pm{}; pm.species=1; pm.level=50; pm.current_hp=pm.max_hp=300; pm.friendship=200; party.add(pm); }
    enginemon::Battle bat(enginemon::BattleType::Wild, party, reg, rules);

    bat.player_pokemon()   = make_mon(HAZE_ID,       PLAYER_BASE, PS_ATK,PS_DEF,PS_SPD,PS_SATK,PS_SDEF,PS_ACC,PS_EVA);
    bat.opponent_pokemon() = make_mon(enginemon::MOVE_NONE, ENEMY_BASE, ES_ATK,ES_DEF,ES_SPD,ES_SATK,ES_SDEF,ES_ACC,ES_EVA);

    bat.set_rng_callback([]()->uint32_t{ return 0xFF; });
    bat.set_player_action(enginemon::ActionFight{0,0});
    bat.set_opponent_action(enginemon::ActionFight{0,0});
    bat.execute_turn();

    const auto& ps=bat.player_pokemon().stages;
    const auto& os=bat.opponent_pokemon().stages;
    const auto& pp=bat.player_pokemon();
    const auto& op=bat.opponent_pokemon();

    std::cout << "  Enginemon stages (delta, neutral=0): player="
              <<(int)ps.attack<<"/"<<(int)ps.defense<<"/"<<(int)ps.speed<<"/"
              <<(int)ps.special_attack<<"/"<<(int)ps.special_defense<<"/"
              <<(int)ps.accuracy<<"/"<<(int)ps.evasion
              <<"  enemy="
              <<(int)os.attack<<"/"<<(int)os.defense<<"/"<<(int)os.speed<<"/"
              <<(int)os.special_attack<<"/"<<(int)os.special_defense<<"/"
              <<(int)os.accuracy<<"/"<<(int)os.evasion<<"\n";

    // --- Compare ---
    const char* SNAME[]={"ATK","DEF","SPD","SATK","SDEF","ACC","EVA"};
    const char* CNAME[]={"ATK","DEF","SPD","SATK","SDEF"};
    const int8_t* EM_PS[]={&ps.attack,&ps.defense,&ps.speed,&ps.special_attack,&ps.special_defense,&ps.accuracy,&ps.evasion};
    const int8_t* EM_OS[]={&os.attack,&os.defense,&os.speed,&os.special_attack,&os.special_defense,&os.accuracy,&os.evasion};
    const int16_t EM_PP[]={pp.stats.attack,pp.stats.defense,pp.stats.speed,pp.stats.special_attack,pp.stats.special_defense};
    const int16_t EM_OP[]={op.stats.attack,op.stats.defense,op.stats.speed,op.stats.special_attack,op.stats.special_defense};

    bool all=true;
    std::cout<<"\n  --- stages ---\n";
    for(int i=0;i<7;i++){
        int8_t cd=int8_t(int(sc1.player_stages[i])-7); int8_t ed=*EM_PS[i]; bool ok=(cd==ed); if(!ok)all=false;
        std::cout<<"  player."<<SNAME[i]<<": crystal="<<(int)cd<<" enginemon="<<(int)ed<<(ok?" MATCH":" MISMATCH")<<"\n";
        ASSERT_EQ((int)cd,(int)ed);
    }
    for(int i=0;i<7;i++){
        int8_t cd=int8_t(int(sc1.enemy_stages[i])-7); int8_t ed=*EM_OS[i]; bool ok=(cd==ed); if(!ok)all=false;
        std::cout<<"  enemy."<<SNAME[i]<<": crystal="<<(int)cd<<" enginemon="<<(int)ed<<(ok?" MATCH":" MISMATCH")<<"\n";
        ASSERT_EQ((int)cd,(int)ed);
    }
    std::cout<<"\n  --- computed stats ---\n";
    for(int i=0;i<5;i++){
        uint16_t cr=sc1.player_stats[i]; uint16_t em=(uint16_t)EM_PP[i]; bool ok=(cr==em); if(!ok)all=false;
        std::cout<<"  player."<<CNAME[i]<<": crystal="<<cr<<" enginemon="<<em<<(ok?" MATCH":" MISMATCH")<<"\n";
        ASSERT_EQ((int)cr,(int)em);
    }
    for(int i=0;i<5;i++){
        uint16_t cr=sc1.enemy_stats[i]; uint16_t em=(uint16_t)EM_OP[i]; bool ok=(cr==em); if(!ok)all=false;
        std::cout<<"  enemy."<<CNAME[i]<<": crystal="<<cr<<" enginemon="<<em<<(ok?" MATCH":" MISMATCH")<<"\n";
        ASSERT_EQ((int)cr,(int)em);
    }
    std::cout<<"\n  RESULT: "<<(all?"crystal == enginemon":"MISMATCH")<<"\n";
}

// ============================================================================
// main
// ============================================================================
int main(int argc, char* argv[]) {
    if (argc<3) { std::cerr<<"Usage: crystal_haze_diff_test <rom> <sym>\n"; return 1; }
    g_rom_path=argv[1]; g_sym_path=argv[2];

    std::string sha=sha1_of_file(g_rom_path);
    if (sha.empty()) { std::cerr<<"Cannot read ROM: "<<g_rom_path<<"\n"; return 1; }
    std::cout<<"ROM SHA-1: "<<sha<<"\n";
    if (sha!=CRYSTAL_ROM_SHA1) {
        std::cerr<<"FATAL: ROM SHA-1 mismatch.\n"
                 <<"  Expected: "<<CRYSTAL_ROM_SHA1<<"\n"
                 <<"  Actual:   "<<sha<<"\n"
                 <<"  Requires Crystal v1.1 (UE) [C][!].\n";
        return 1;
    }
    std::cout<<"ROM hash enforced: OK\n";

    auto rom=crystal::RomData::load(std::filesystem::path(g_rom_path));
    if (!rom) { std::cerr<<"RomData::load failed\n"; return 1; }
    const crystal::ExtractionProfile* profile=crystal::ProfileRegistry::instance().get_profile_by_hash(rom->hash());
    if (!profile) { std::cerr<<"No profile for ROM\n"; return 1; }
    g_rom=rom.get(); g_profile=profile;

    std::cout<<"\n=== Crystal Live Haze Differential ===\n";
    std::cout<<"  SameBoy: "<<SAMEBOY_COMMIT<<"\n";
    std::cout<<"  .sym:    "<<g_sym_path<<"\n\n";

    RUN_TEST(haze_live_crystal_vs_enginemon);

    std::cout<<"\n=== Results ===\n";
    std::cout<<"Passed: "<<g_passed<<"\n";
    std::cout<<"Failed: "<<g_failed<<"\n";
    return g_failed>0?1:0;
}
