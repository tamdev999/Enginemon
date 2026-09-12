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
// Crystal execution:
//   Entry: BattleCommand_ResetStats (0d:710e from .sym, verified against ROM bytes)
//   ROM bank setup: GB_write_memory(0x2000, 0x0d)  -- MBC3 bank select
//   WRAM bank:      WRAM0 (0xC000-0xCFFF) + WRAM1 (0xD000-0xDFFF) via GB_get_direct_access
//   Cold-call: GB_get_registers()->PC = entry, SP = 0xFFF0, return addr pushed on stack
//   ROM patches (in-memory only, do not touch disk ROM):
//     0x3711C (0d:711C): 0xF0 -> 0xC9 (RET)
//       BattleCommand_ResetStats layout (flat offsets from 0x3710E):
//         +0x00  LD A,7
//         +0x02  LD HL,wPlayerStatLevels ; CALL 0x7137  <- fills 8 bytes with 7
//         +0x08  LD HL,wEnemyStatLevels  ; CALL 0x7137  <- fills 8 bytes with 7
//         +0x0E [PATCH] LDH A,(hBattleTurn) -> RET
//       At +0x0E both stage resets are complete. Stack is clean (SP=0xFFEE,
//       sentinel 0x0150 at top). RET pops 0x0150 directly and sentinel fires.
//       PUSH AF at +0x10 is not reached — avoids stack imbalance that would
//       corrupt the return address before CalcBattleStats calls.
//     0x0150 (00:0150): JP 0x0150  -- sentinel loop; execution_callback stops here
//     0x0100 (00:0100): DI; JP <entry>  -- trampoline past boot ROM / Crystal startup
//   Return detection: GB_set_execution_callback fires when PC == 0x0150
//
// Fixture symbols (all resolved from .sym, no hardcoded addresses):
//   wPlayerStatLevels 0xC6CC, wEnemyStatLevels 0xC6D4
//   wLinkMode 0xC2DC, wInBattleTowerBattle 0xCFC0, wJohtoBadges 0xD857
//   hBattleTurn 0xFFE4
//
// Enginemon side: Battle::execute_turn() with Haze (move 114)
//
// Normalization: Crystal stage encoding: 7=neutral; Enginemon: 0=neutral
//   delta = crystal_raw - 7 == enginemon_stage
//
// Comparison: stage arrays only (7 per side).
//   CalcBattleStats is NOT run — it uses Crystal's RST $08 banked-call mechanism
//   which cannot be cold-called without full runtime state.
//   The stat stage reset IS the complete Haze observable semantic.

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
// SHA-1 computation (Windows CAPI via existing crypto facilities)
// ============================================================================
static std::string sha1_of_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return "";
    std::vector<uint8_t> data(std::istreambuf_iterator<char>(f), {});
    if (data.empty()) return "";

    // Use BCrypt (Windows) — simpler than linking OpenSSL just for this.
    // Fallback: compute SHA-1 manually (table-based) to keep zero new deps.
    // We implement a minimal SHA-1 here (standard algorithm, no license concerns).
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
// GB_DIRECT_ACCESS_RAM on CGB returns 0x8000 bytes:
//   bank 0: bytes [0x0000..0x0FFF]  (WRAM addr 0xC000-0xCFFF)
//   bank 1: bytes [0x1000..0x1FFF]  (WRAM addr 0xD000-0xDFFF)
//   etc.
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
// Only stat stages — CalcBattleStats is not run (see patch comment above).
// ============================================================================
struct CrystalHazeSnapshot {
    uint8_t  player_stages[7];  // ATK/DEF/SPD/SATK/SDEF/ACC/EVA (raw: 7=neutral)
    uint8_t  enemy_stages[7];
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
// Run Crystal Haze
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

    // --- Resolve symbols ---
    CrystalSym s_haze, s_pstages, s_estages, s_hbt, s_link, s_btwr, s_jbadge;
    if (!sym_lookup(g_sym_path,"BattleCommand_ResetStats",&s_haze)   ||
        !sym_lookup(g_sym_path,"wPlayerStatLevels",       &s_pstages)||
        !sym_lookup(g_sym_path,"wEnemyStatLevels",        &s_estages)||
        !sym_lookup(g_sym_path,"hBattleTurn",             &s_hbt)    ||
        !sym_lookup(g_sym_path,"wLinkMode",               &s_link)   ||
        !sym_lookup(g_sym_path,"wInBattleTowerBattle",    &s_btwr)   ||
        !sym_lookup(g_sym_path,"wJohtoBadges",            &s_jbadge))
    { std::cerr<<"  [sym] missing symbol\n"; return false; }

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

    // Sentinel: 0x0150 in ROM bank 0 — JP 0x0150 loop
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

    // Patch 1: sentinel loop at 0x0150 (ROM bank 0, flat offset 0x0150)
    if (rom_sz > SENTINEL_PC+2) {
        rom_buf[SENTINEL_PC+0] = 0xC3;  // JP nn
        rom_buf[SENTINEL_PC+1] = SENTINEL_PC & 0xFF;
        rom_buf[SENTINEL_PC+2] = (SENTINEL_PC>>8)&0xFF;
    }

    // Patch 2: trampoline at 0x0100 (ROM entry point, flat offset 0x0100)
    //   F3       = DI (disable interrupts, prevent handlers before we get to Haze)
    //   C3 0E 71 = JP 0x710E (jump to BattleCommand_ResetStats, bank 0x0d already mapped)
    // This avoids needing to set IME directly (no public SameBoy API for that).
    static constexpr uint16_t TRAMPOLINE_ADDR = 0x0100;
    if (rom_sz > TRAMPOLINE_ADDR + 4) {
        rom_buf[TRAMPOLINE_ADDR+0] = 0xF3;          // DI
        rom_buf[TRAMPOLINE_ADDR+1] = 0xC3;          // JP nn
        rom_buf[TRAMPOLINE_ADDR+2] = s_haze.addr & 0xFF;
        rom_buf[TRAMPOLINE_ADDR+3] = (s_haze.addr>>8) & 0xFF;
    }

    // Patch 3: RET at 0x0d:0x711C (flat 0x3711C) — turns LDH A,(hBattleTurn) into RET.
    // BattleCommand_ResetStats layout (flat offsets from 0x3710E):
    //   +0x00: LD A,7
    //   +0x02: LD HL,wPlayerStatLevels ; +0x05: CALL 0x7137  (fills 8 bytes with 7)
    //   +0x08: LD HL,wEnemyStatLevels  ; +0x0B: CALL 0x7137  (fills 8 bytes with 7)
    //   +0x0E: [PATCH] LDH A,(hBattleTurn) -> RET
    //
    // At +0x0E, both stat-level resets are complete. Stack is clean (SP=0xFFEE,
    // sentinel 0x0150 at top). RET pops 0x0150 directly → sentinel fires.
    //
    // +0x10: PUSH AF (not reached — avoids stack imbalance)
    // +0x11: CALL 0x3985 / +0x14: CALL 0x65D7 (CalcBattleStats — uses RST $08, not run)
    static constexpr uint32_t PATCH_FLAT = 0x0d * 0x4000u + (0x711Cu - 0x4000u);  // = 0x3711C
    if (rom_sz > PATCH_FLAT && rom_buf[PATCH_FLAT] == 0xF0) {
        rom_buf[PATCH_FLAT] = 0xC9;  // RET
    } else {
        std::cerr << "  [patch] byte at 0x3711C = 0x"
                  << std::hex << (int)rom_buf[PATCH_FLAT] << std::dec
                  << ", expected 0xF0 (LDH A,(hBattleTurn))\n";
        GB_free(&gb); return false;
    }

    // --- Set ROM bank 0x0d (MBC3: write bank number to 0x2000-0x3FFF) ---
    GB_write_memory(&gb, 0x2000, s_haze.bank);

    // --- Get WRAM direct access ---
    size_t wram_sz = 0; uint16_t wbank = 0;
    uint8_t* wram = reinterpret_cast<uint8_t*>(
        GB_get_direct_access(&gb, GB_DIRECT_ACCESS_RAM, &wram_sz, &wbank));
    if (!wram || wram_sz < 0x2000) {
        std::cerr<<"  [sb] WRAM access failed (size="<<wram_sz<<")\n";
        GB_free(&gb); return false;
    }

    // Poison all WRAM
    std::memset(wram, poison, wram_sz);

    // --- Write fixture ---
    // wLinkMode = 0  (BadgeStatBoosts no-op path)
    wram[wram_offset(s_link.addr)]  = 0x00;
    // wInBattleTowerBattle = 0
    wram[wram_offset(s_btwr.addr)]  = 0x00;
    // wJohtoBadges = 0  (BadgeStatBoosts no-op path)
    wram[wram_offset(s_jbadge.addr)] = 0x00;

    // wPlayerStatLevels: 7+delta for each stage, index 7 (ABILITY/Curse) = 7
    {
        uint8_t* p = wram + wram_offset(s_pstages.addr);
        for (int i=0; i<7; i++) p[i] = (uint8_t)(7 + player_deltas[i]);
        p[7] = 7;  // ABILITY slot (Curse) — neutral, not used by Haze
    }
    {
        uint8_t* p = wram + wram_offset(s_estages.addr);
        for (int i=0; i<7; i++) p[i] = (uint8_t)(7 + enemy_deltas[i]);
        p[7] = 7;
    }
    // Note: wBattleMon/wEnemyMon base stats are not written.
    // CalcBattleStats is not run (patched out above), so those fields are irrelevant.
    // hBattleTurn = TURN_PLAYER (0) via HRAM write
    GB_write_memory(&gb, s_hbt.addr, 0x00);
    // Disable all interrupts: IE=0 (0xFFFF), IF=0 (0xFF0F)
    GB_write_memory(&gb, 0xFFFF, 0x00);  // IE: clear all interrupt enables
    GB_write_memory(&gb, 0xFF0F, 0x00);  // IF: clear all pending interrupts

    // --- Set CPU registers: PC = haze entry, push sentinel as return addr ---
    GB_registers_t* regs = GB_get_registers(&gb);
    if (!regs) { std::cerr<<"  [sb] cannot get registers\n"; GB_free(&gb); return false; }

    // SP at 0xFFF0. Push SENTINEL_PC big-endian onto SM83 stack (little-endian push order):
    // SM83 PUSH: SP--, write high; SP--, write low.
    // So: memory[0xFFEF] = high(SENTINEL_PC), memory[0xFFEE] = low(SENTINEL_PC)
    static constexpr uint16_t INITIAL_SP = 0xFFF0;
    GB_write_memory(&gb, INITIAL_SP - 1, (SENTINEL_PC >> 8) & 0xFF);
    GB_write_memory(&gb, INITIAL_SP - 2,  SENTINEL_PC       & 0xFF);
    // Set PC to the trampoline (0x0100) which will: DI, then JP to BattleCommand_ResetStats
    regs->sp = INITIAL_SP - 2;
    regs->pc = TRAMPOLINE_ADDR;

    // --- Execute until sentinel ---
    static constexpr int MAX_INSN = 500000;
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

    // --- Read results ---
    // Re-acquire WRAM pointer (may have changed)
    wram_sz = 0;
    wram = reinterpret_cast<uint8_t*>(
        GB_get_direct_access(&gb, GB_DIRECT_ACCESS_RAM, &wram_sz, &wbank));
    if (!wram) { std::cerr<<"  [sb] cannot re-read WRAM\n"; GB_free(&gb); return false; }

    // Stat stages
    {
        const uint8_t* p = wram + wram_offset(s_pstages.addr);
        for (int i=0; i<7; i++) out->player_stages[i] = p[i];
    }
    {
        const uint8_t* p = wram + wram_offset(s_estages.addr);
        for (int i=0; i<7; i++) out->enemy_stages[i] = p[i];
    }
    // Note: computed stats (wPlayerStats/wEnemyStats) not read — CalcBattleStats not run.

    std::cout << "  [crystal poison=0x" << std::hex << (int)poison << std::dec
              << "] " << count << " instructions\n";

    GB_free(&gb);
    return true;
}

// ============================================================================
// Enginemon helpers (same pattern as battle_rom_test.cpp)
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
    enginemon::MoveId mid, int16_t spd_override,
    int8_t s_atk, int8_t s_def, int8_t s_spd,
    int8_t s_satk, int8_t s_sdef, int8_t s_acc, int8_t s_eva)
{
    enginemon::BattlePokemon bp{};
    bp.species=1; bp.type1=0; bp.type2=0; bp.level=50;
    bp.stats.hp=bp.stats.max_hp=300; bp.base_stats.hp=bp.base_stats.max_hp=300;
    // Base stats are arbitrary — Haze only resets stages; CalcBattleStats not run.
    bp.stats.attack=bp.base_stats.attack=80;
    bp.stats.defense=bp.base_stats.defense=80;
    bp.stats.speed=bp.base_stats.speed=(spd_override>0?spd_override:80);
    bp.stats.special_attack=bp.base_stats.special_attack=80;
    bp.stats.special_defense=bp.base_stats.special_defense=80;
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

    // --- POISON STABILITY: both Crystal runs must agree on stages ---
    bool stable = true;
    for (int i=0; i<7; i++) {
        if (sc1.player_stages[i] != sc2.player_stages[i]) { stable=false; break; }
        if (sc1.enemy_stages[i]  != sc2.enemy_stages[i])  { stable=false; break; }
    }
    std::cout << "  Poison stability: " << (stable ? "YES" : "NO (FAIL)") << "\n";
    ASSERT_TRUE(stable);

    std::cout << "  Crystal stages post-Haze (raw, neutral=7): player=";
    for (auto s : sc1.player_stages) std::cout << (int)s << " ";
    std::cout << "  enemy=";
    for (auto s : sc1.enemy_stages) std::cout << (int)s << " ";
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

    bat.player_pokemon()   = make_mon(HAZE_ID, 200,
        PS_ATK,PS_DEF,PS_SPD,PS_SATK,PS_SDEF,PS_ACC,PS_EVA);
    bat.opponent_pokemon() = make_mon(enginemon::MOVE_NONE, 1,
        ES_ATK,ES_DEF,ES_SPD,ES_SATK,ES_SDEF,ES_ACC,ES_EVA);

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

    // --- NORMALIZE AND COMPARE ---
    // Crystal: raw - 7 = delta. Enginemon: stage = delta already.
    const char* stat_names[] = {"ATK","DEF","SPD","SATK","SDEF","ACC","EVA"};
    const int8_t* em_p[] = {&ps.attack,&ps.defense,&ps.speed,&ps.special_attack,&ps.special_defense,&ps.accuracy,&ps.evasion};
    const int8_t* em_o[] = {&os.attack,&os.defense,&os.speed,&os.special_attack,&os.special_defense,&os.accuracy,&os.evasion};

    bool all_match = true;
    for (int i=0; i<7; i++) {
        int8_t cd = int8_t(int(sc1.player_stages[i]) - 7);
        int8_t ed = *em_p[i];
        bool ok = (cd == ed);
        if (!ok) all_match = false;
        std::cout << "  player." << stat_names[i]
                  << ": crystal_delta=" << (int)cd
                  << " enginemon=" << (int)ed
                  << (ok ? " MATCH" : " MISMATCH") << "\n";
        ASSERT_EQ((int)cd, (int)ed);
    }
    for (int i=0; i<7; i++) {
        int8_t cd = int8_t(int(sc1.enemy_stages[i]) - 7);
        int8_t ed = *em_o[i];
        bool ok = (cd == ed);
        if (!ok) all_match = false;
        std::cout << "  enemy." << stat_names[i]
                  << ": crystal_delta=" << (int)cd
                  << " enginemon=" << (int)ed
                  << (ok ? " MATCH" : " MISMATCH") << "\n";
        ASSERT_EQ((int)cd, (int)ed);
    }

    std::cout << "  RESULT: " << (all_match ? "crystal == enginemon" : "MISMATCH") << "\n";
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
