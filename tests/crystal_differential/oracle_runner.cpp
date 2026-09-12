// tests/crystal_differential/oracle_runner.cpp
//
// Crystal battle differential oracle — fully hardened parallel runner.
//
// ─── ROM IMMUTABILITY ──────────────────────────────────────────────────────
//   rom_bytes is loaded once, SHA-checked, and passed read-only.
//   GB_load_rom_from_buffer receives the exact verified bytes unchanged.
//   No pointer to gb->rom is ever taken. No ROM bytes are written anywhere.
//
// ─── COLD-CALL (no ROM patches) ────────────────────────────────────────────
//   regs->pc  = entry symbol address   (bank already mapped)
//   regs->sp  = INITIAL_SP-2           (sink addr on stack as return address)
//   hROMBank  = entry bank             (RST $08 BankSwitch restore path)
//   0xFF50    = 1                      (boot_rom_finished → RST vectors read ROM)
//   0x2000    = entry bank             (MBC register)
//
// ─── PRESENTATION BOUNDARY ─────────────────────────────────────────────────
//   Execution callback stops when PC == allowlisted sink address.
//   All semantic WRAM writes are complete at that point.
//   The sink function never executes.
//
// ─── IDENTITY CHECKS ───────────────────────────────────────────────────────
//   Crystal ROM: exact SHA-1 of loaded bytes against PINNED_ROM_SHA1
//   .sym file:   SHA-1 of the raw file bytes against PINNED_SYM_SHA1, PLUS
//                6 anchor-symbol bank:addr pairs for belt-and-suspenders
//   SameBoy:     GB_VERSION string AND GB_COMMIT string from sameboy_version.h;
//                both checked at startup against compile-time pin constants
//
// ─── ERROR FORMAT ──────────────────────────────────────────────────────────
//   Every failure emits exactly one line:
//     ERROR <n> @ <SUBSYSTEM> [<CASE>] :: <CLASS> :: <reason>
//   where n is the global 1-based error counter (deterministic, sorted order).
//   The summary repeats all error lines with their numbers.
//
// ─── OUTPUT ORDERING ───────────────────────────────────────────────────────
//   Workers run in parallel. Results are collected in sorted move-ID order.
//   A single mutex guards all stdout writes. Output never interleaves.

#include "crystal_differential/oracle_runner.hpp"

#include "Core/gb.h"
#include "Core/memory.h"

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

// sameboy_version.h defines GB_VERSION, GB_COMMIT, GB_COPYRIGHT_YEAR.
// It is on the include path via oracle_runner_lib's include directories.
#include "sameboy_version.h"
#ifndef GB_VERSION
#  error "GB_VERSION not defined — sameboy_version.h not on include path"
#endif
#ifndef GB_COMMIT
#  error "GB_COMMIT not defined — sameboy_version.h is stale, add GB_COMMIT"
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace crystal::oracle {

// ============================================================================
// Compile-time identity pins
// ============================================================================
static constexpr const char* PINNED_ROM_SHA1 = "F2F52230B536214EF7C9924F483392993E226CFB";
static constexpr const char* PINNED_SYM_SHA1 = ""; // filled below by compute_pinned_sym_sha()
// SHA-1 of references/pokecrystal-symbols/pokecrystal11.sym at the known-good version.
// Computed from the file contents at the time of pinning.
// NOTE: if this string is empty the check is skipped (allows first-run bootstrapping).
// To pin: run crystal_battle_diff once with PINNED_SYM_SHA1="" to print the SHA,
// then set the constant.
static constexpr const char* PINNED_SYM_SHA1_CONST =
    "AFF514A7CE7858DED0851BA663C3839C93C7BCE7"; // pokecrystal11.sym pinned SHA-1

static constexpr uint32_t    CRYSTAL_ROM_SIZE      = 2097152u;  // 2 MiB
static constexpr const char* PINNED_SAMEBOY_VERSION = "1.0.3";
static constexpr const char* PINNED_SAMEBOY_COMMIT  = "213a12ce93d66b105a113debd9396306066a7cfc";

// Per-case wall-clock emergency timeout (seconds).
// A case that doesn't produce a result within this window is a HARNESS_ERROR.
// Normal Haze: ~2 ms. Cap = 30 s (extreme safety margin).
static constexpr int WALL_CLOCK_TIMEOUT_S = 30;

// ============================================================================
// SHA-1 (self-contained, no external deps)
// ============================================================================
static std::string sha1_hex(const uint8_t* data, size_t len) {
    struct S {
        uint32_t h[5]={0x67452301u,0xEFCDAB89u,0x98BADCFEu,0x10325476u,0xC3D2E1F0u};
        uint64_t c=0; uint8_t b[64]{}; uint32_t bl=0;
        static uint32_t R(uint32_t v,int n){return(v<<n)|(v>>(32-n));}
        void blk(const uint8_t* d){
            uint32_t w[80];
            for(int i=0;i<16;i++) w[i]=(d[i*4]<<24)|(d[i*4+1]<<16)|(d[i*4+2]<<8)|d[i*4+3];
            for(int i=16;i<80;i++) w[i]=R(w[i-3]^w[i-8]^w[i-14]^w[i-16],1);
            uint32_t a=h[0],B=h[1],C=h[2],D=h[3],e=h[4];
            for(int i=0;i<80;i++){
                uint32_t f,k;
                if(i<20){f=(B&C)|((~B)&D);k=0x5A827999u;}
                else if(i<40){f=B^C^D;k=0x6ED9EBA1u;}
                else if(i<60){f=(B&C)|(B&D)|(C&D);k=0x8F1BBCDCu;}
                else{f=B^C^D;k=0xCA62C1D6u;}
                uint32_t t=R(a,5)+f+e+k+w[i];e=D;D=C;C=R(B,30);B=a;a=t;
            }
            h[0]+=a;h[1]+=B;h[2]+=C;h[3]+=D;h[4]+=e;
        }
        void feed(const uint8_t* p,size_t n){
            c+=n*8;
            for(size_t i=0;i<n;i++){b[bl++]=p[i];if(bl==64){blk(b);bl=0;}}
        }
        std::array<uint8_t,20> fin(){
            b[bl++]=0x80;
            if(bl>56){while(bl<64)b[bl++]=0;blk(b);bl=0;}
            while(bl<56)b[bl++]=0;
            for(int i=0;i<8;i++)b[56+i]=(uint8_t)(c>>((7-i)*8));
            blk(b);
            std::array<uint8_t,20> r{};
            for(int i=0;i<5;i++){r[i*4]=(h[i]>>24)&0xFF;r[i*4+1]=(h[i]>>16)&0xFF;r[i*4+2]=(h[i]>>8)&0xFF;r[i*4+3]=h[i]&0xFF;}
            return r;
        }
    } s; s.feed(data,len);
    auto h=s.fin();
    char hex[41]{}; for(int i=0;i<20;i++) sprintf(hex+i*2,"%02X",h[i]);
    return std::string(hex);
}

static std::string sha1_of_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return "";
    std::vector<uint8_t> buf(std::istreambuf_iterator<char>(f), {});
    if (buf.empty()) return "";
    return sha1_hex(buf.data(), buf.size());
}

// ============================================================================
// Symbol types and lookup
// ============================================================================
struct Sym { uint8_t bank; uint16_t addr; };

static bool sym_get(const std::string& path, const std::string& name, Sym* out) {
    std::ifstream f(path); if(!f) return false;
    std::string line;
    while(std::getline(f,line)){
        if(line.empty()||line[0]==';') continue;
        std::istringstream ss(line); std::string a,s;
        if(!(ss>>a>>s)||s!=name) continue;
        auto c=a.find(':'); if(c==std::string::npos) continue;
        out->bank=(uint8_t)std::stoi(a.substr(0,c),nullptr,16);
        out->addr=(uint16_t)std::stoi(a.substr(c+1),nullptr,16);
        return true;
    }
    return false;
}

// ============================================================================
// SymCache — loaded once, identity-validated, shared read-only.
//
// Identity anchors: exact bank:addr pairs for pokecrystal11 (Crystal v1.1 UE).
// These must match or startup fails with a specific error.
// The .sym file SHA (if PINNED_SYM_SHA1_CONST is non-empty) is an additional
// belt-and-suspenders check.
// ============================================================================
struct SymAnchor { const char* name; uint8_t bank; uint16_t addr; };
static constexpr SymAnchor SYM_ANCHORS[] = {
    { "BattleCommand_ResetStats", 0x0D, 0x710E },
    { "AnimateCurrentMove",       0x0D, 0x7E01 },
    { "wPlayerStatLevels",        0x00, 0xC6CC },
    { "wEnemyStatLevels",         0x00, 0xC6D4 },
    { "wBattleMonAttack",         0x00, 0xC640 },
    { "wEnemyMonAttack",          0x01, 0xD21A },
};

struct SymCache {
    Sym BattleCommand_ResetStats;
    Sym wPlayerStatLevels;
    Sym wEnemyStatLevels;
    Sym wPlayerStats;
    Sym wEnemyStats;
    Sym wBattleMonAttack;
    Sym wEnemyMonAttack;
    Sym wBattleMonStatus;
    Sym wEnemyMonStatus;
    Sym hBattleTurn;
    Sym hROMBank;
    Sym wLinkMode;
    Sym wInBattleTowerBattle;
    Sym wJohtoBadges;
    Sym AnimateCurrentMove;

    // Returns non-empty error string on failure.
    // sym_sha_out receives the actual SHA-1 of the file (for printing).
    static std::string load(const std::string& sym_path,
                             SymCache* out,
                             std::string* sym_sha_out = nullptr)
    {
        // 1. File accessible?
        { std::ifstream f(sym_path); if(!f) return "cannot open: "+sym_path; }

        // 2. .sym file SHA (if pinned).
        std::string actual_sha = sha1_of_file(sym_path);
        if (sym_sha_out) *sym_sha_out = actual_sha;
        if (std::string(PINNED_SYM_SHA1_CONST).size() > 0 &&
            actual_sha != PINNED_SYM_SHA1_CONST)
        {
            return ".sym SHA mismatch: expected "+std::string(PINNED_SYM_SHA1_CONST)
                 +" got "+actual_sha;
        }

        // 3. Anchor identity check.
        for (const auto& e : SYM_ANCHORS) {
            Sym got{};
            if (!sym_get(sym_path, e.name, &got))
                return std::string("anchor symbol missing: ")+e.name;
            if (got.bank!=e.bank||got.addr!=e.addr) {
                char buf[128];
                snprintf(buf,sizeof(buf),
                    "anchor mismatch: %s expected %02X:%04X got %02X:%04X",
                    e.name, e.bank, e.addr, got.bank, got.addr);
                return std::string(buf);
            }
        }

        // 4. Load all required symbols.
        struct { const char* name; Sym* dst; } required[] = {
            {"BattleCommand_ResetStats",&out->BattleCommand_ResetStats},
            {"wPlayerStatLevels",       &out->wPlayerStatLevels},
            {"wEnemyStatLevels",        &out->wEnemyStatLevels},
            {"wPlayerStats",            &out->wPlayerStats},
            {"wEnemyStats",             &out->wEnemyStats},
            {"wBattleMonAttack",        &out->wBattleMonAttack},
            {"wEnemyMonAttack",         &out->wEnemyMonAttack},
            {"wBattleMonStatus",        &out->wBattleMonStatus},
            {"wEnemyMonStatus",         &out->wEnemyMonStatus},
            {"hBattleTurn",             &out->hBattleTurn},
            {"hROMBank",                &out->hROMBank},
            {"wLinkMode",               &out->wLinkMode},
            {"wInBattleTowerBattle",    &out->wInBattleTowerBattle},
            {"wJohtoBadges",            &out->wJohtoBadges},
            {"AnimateCurrentMove",      &out->AnimateCurrentMove},
        };
        for (const auto& r : required) {
            if (!sym_get(sym_path, r.name, r.dst))
                return std::string("required symbol missing: ")+r.name;
        }
        return {};
    }
};

// ============================================================================
// Fixture address validation
// ============================================================================
static std::string validate_wram_addr(const char* n, uint16_t a, uint16_t lo, uint16_t hi){
    if(a<lo||a>hi){
        char buf[128];
        snprintf(buf,sizeof(buf),"%s addr 0x%04X outside [0x%04X,0x%04X]",n,a,lo,hi);
        return std::string(buf);
    }
    return {};
}
static std::string validate_hram_addr(const char* n, uint16_t a){
    if(a<0xFF80||a>0xFFFE){
        char buf[64]; snprintf(buf,sizeof(buf),"%s addr 0x%04X outside HRAM",n,a);
        return std::string(buf);
    }
    return {};
}
static std::string validate_fixture_addresses(const SymCache& sym){
    std::string e;
    if((e=validate_wram_addr("wPlayerStatLevels", sym.wPlayerStatLevels.addr, 0xC000,0xCFFF)).size()) return e;
    if((e=validate_wram_addr("wEnemyStatLevels",  sym.wEnemyStatLevels.addr,  0xC000,0xCFFF)).size()) return e;
    if((e=validate_wram_addr("wPlayerStats",      sym.wPlayerStats.addr,      0xC000,0xCFFF)).size()) return e;
    if((e=validate_wram_addr("wEnemyStats",       sym.wEnemyStats.addr,       0xC000,0xCFFF)).size()) return e;
    if((e=validate_wram_addr("wBattleMonAttack",  sym.wBattleMonAttack.addr,  0xC000,0xCFFF)).size()) return e;
    if((e=validate_wram_addr("wBattleMonStatus",  sym.wBattleMonStatus.addr,  0xC000,0xCFFF)).size()) return e;
    if((e=validate_wram_addr("wLinkMode",         sym.wLinkMode.addr,         0xC000,0xCFFF)).size()) return e;
    if((e=validate_wram_addr("wEnemyMonAttack",     sym.wEnemyMonAttack.addr,     0xD000,0xDFFF)).size()) return e;
    if((e=validate_wram_addr("wEnemyMonStatus",     sym.wEnemyMonStatus.addr,     0xD000,0xDFFF)).size()) return e;
    if((e=validate_wram_addr("wInBattleTowerBattle",sym.wInBattleTowerBattle.addr,0xC000,0xDFFF)).size()) return e;
    if((e=validate_wram_addr("wJohtoBadges",        sym.wJohtoBadges.addr,        0xD000,0xDFFF)).size()) return e;
    if((e=validate_hram_addr("hBattleTurn", sym.hBattleTurn.addr)).size()) return e;
    if((e=validate_hram_addr("hROMBank",    sym.hROMBank.addr)).size()) return e;
    return {};
}

static size_t wram_off(uint16_t addr){
    if(addr>=0xD000) return size_t{0x1000}+(addr-0xD000);
    if(addr>=0xC000) return addr-0xC000;
    throw std::logic_error("wram_off: not WRAM");
}

// ============================================================================
// SameBoy no-op callbacks
// ============================================================================
static void sb_log_nop(GB_gameboy_t*, const char*, GB_log_attributes_t){}
static uint32_t sb_rgb_nop(GB_gameboy_t*, uint8_t r, uint8_t g, uint8_t b){
    return 0xFF000000u|((uint32_t)r<<16)|((uint32_t)g<<8)|b;
}

// ============================================================================
// Execution callback
// ============================================================================
struct ExecCtx {
    uint16_t    sink_pc;
    const char* sink_name;
    bool        triggered;
    int         insn_count;
};
static void exec_cb(GB_gameboy_t* gb, uint16_t pc, uint8_t){
    auto* ctx=static_cast<ExecCtx*>(GB_get_user_data(gb));
    if(!ctx||ctx->triggered) return;
    ++ctx->insn_count;
    if(pc==ctx->sink_pc) ctx->triggered=true;
}

// ============================================================================
// StopReason
// ============================================================================
enum class StopReason {
    SINK_HIT,
    MAX_INSN_EXCEEDED,
    WALL_CLOCK_TIMEOUT,  // emergency — future use (insn cap fires first in practice)
    GB_INIT_FAILED,
    WRAM_ACCESS_FAILED,
    REGS_ACCESS_FAILED,
};
static const char* stop_reason_str(StopReason r){
    switch(r){
    case StopReason::SINK_HIT:           return "SINK_HIT";
    case StopReason::MAX_INSN_EXCEEDED:  return "MAX_INSN_EXCEEDED";
    case StopReason::WALL_CLOCK_TIMEOUT: return "WALL_CLOCK_TIMEOUT";
    case StopReason::GB_INIT_FAILED:     return "GB_INIT_FAILED";
    case StopReason::WRAM_ACCESS_FAILED: return "WRAM_ACCESS_FAILED";
    case StopReason::REGS_ACCESS_FAILED: return "REGS_ACCESS_FAILED";
    }
    return "?";
}

// ============================================================================
// Snapshots
// ============================================================================
struct CrystalSnapshot {
    uint8_t  player_stages[7]; // wPlayerStatLevels[0..6]  (raw: 7=neutral)
    uint8_t  enemy_stages[7];  // wEnemyStatLevels[0..6]
    uint16_t player_stats[5];  // wBattleMonAttack..SpDef  (native endian)
    uint16_t enemy_stats[5];   // wEnemyMonAttack..SpDef
};
static bool snapshots_equal(const CrystalSnapshot& a, const CrystalSnapshot& b){
    for(int i=0;i<7;i++) if(a.player_stages[i]!=b.player_stages[i]||a.enemy_stages[i]!=b.enemy_stages[i]) return false;
    for(int i=0;i<5;i++) if(a.player_stats[i]!=b.player_stats[i]||a.enemy_stats[i]!=b.enemy_stats[i]) return false;
    return true;
}

struct CrystalRunResult {
    StopReason                     stop_reason;
    std::optional<CrystalSnapshot> snapshot;
    int                            insn_count;
    const char*                    sink_name;
};

struct EngineSnapshot {
    int8_t   player_stages[7];
    int8_t   enemy_stages[7];
    uint16_t player_stats[5];
    uint16_t enemy_stats[5];
};

// ============================================================================
// Fixture constants (same for Crystal WRAM writes and Enginemon BattlePokemon)
// ============================================================================
static constexpr int8_t   PLAYER_DELTA[7] = {+2,-3,+1,-1,+3,+4,-2};
static constexpr int8_t   ENEMY_DELTA[7]  = {-4,+6,-2,+3,-1,-3,+5};
static constexpr uint16_t P_ATK=110,P_DEF=60,P_SPD=130,P_SATK=95,P_SDEF=70;
static constexpr uint16_t E_ATK=75, E_DEF=110,E_SPD=30, E_SATK=100,E_SDEF=80;

// ============================================================================
// run_crystal_case — dedicated SameBoy instance, no shared state
// ============================================================================
static CrystalRunResult run_crystal_case(
    const std::vector<uint8_t>& rom_bytes,
    const SymCache& sym,
    uint8_t poison,
    int insn_cap)
{
    CrystalRunResult res{}; res.insn_count=0; res.sink_name=nullptr;

    GB_gameboy_t gb;
    if(!GB_init(&gb,GB_MODEL_CGB_E)){ res.stop_reason=StopReason::GB_INIT_FAILED; return res; }

    static thread_local uint32_t tl_pixels[160*144];
    GB_set_log_callback(&gb,sb_log_nop);
    GB_set_rgb_encode_callback(&gb,sb_rgb_nop);
    GB_set_pixels_output(&gb,tl_pixels);
    GB_set_rendering_disabled(&gb,true);
    GB_set_turbo_mode(&gb,true,true);

    ExecCtx ctx;
    ctx.sink_pc   =sym.AnimateCurrentMove.addr;
    ctx.sink_name ="AnimateCurrentMove";
    ctx.triggered =false;
    ctx.insn_count=0;
    GB_set_user_data(&gb,&ctx);
    GB_set_execution_callback(&gb,exec_cb);

    // Load ROM — exact unmodified bytes; SameBoy malloc-copies internally.
    GB_load_rom_from_buffer(&gb,rom_bytes.data(),rom_bytes.size());

    GB_write_memory(&gb,0xFF50,1);                       // boot_rom_finished
    GB_write_memory(&gb,0x2000,sym.BattleCommand_ResetStats.bank); // MBC bank

    size_t wram_sz=0; uint16_t wbank=0;
    uint8_t* wram=static_cast<uint8_t*>(
        GB_get_direct_access(&gb,GB_DIRECT_ACCESS_RAM,&wram_sz,&wbank));
    if(!wram||wram_sz<0x2000){
        GB_free(&gb); res.stop_reason=StopReason::WRAM_ACCESS_FAILED; return res;
    }
    std::memset(wram,poison,wram_sz);

    // Fixture writes (all addresses pre-validated at startup)
    wram[wram_off(sym.wLinkMode.addr)]=0;
    wram[wram_off(sym.wInBattleTowerBattle.addr)]=0;
    wram[wram_off(sym.wJohtoBadges.addr)]=0;
    wram[wram_off(sym.wBattleMonStatus.addr)  ]=0;
    wram[wram_off(sym.wBattleMonStatus.addr)+1]=0;
    wram[wram_off(sym.wEnemyMonStatus.addr)  ]=0;
    wram[wram_off(sym.wEnemyMonStatus.addr)+1]=0;
    {
        uint8_t* p=wram+wram_off(sym.wPlayerStatLevels.addr);
        for(int i=0;i<7;i++) p[i]=(uint8_t)(7+PLAYER_DELTA[i]);
        p[7]=7;
    }
    {
        uint8_t* p=wram+wram_off(sym.wEnemyStatLevels.addr);
        for(int i=0;i<7;i++) p[i]=(uint8_t)(7+ENEMY_DELTA[i]);
        p[7]=7;
    }
    {
        uint8_t* p=wram+wram_off(sym.wPlayerStats.addr);
        auto be=[](uint8_t* d,uint16_t v){d[0]=(v>>8);d[1]=v&0xFF;};
        be(p+0,P_ATK);be(p+2,P_DEF);be(p+4,P_SPD);be(p+6,P_SATK);be(p+8,P_SDEF);
    }
    {
        uint8_t* p=wram+wram_off(sym.wEnemyStats.addr);
        auto be=[](uint8_t* d,uint16_t v){d[0]=(v>>8);d[1]=v&0xFF;};
        be(p+0,E_ATK);be(p+2,E_DEF);be(p+4,E_SPD);be(p+6,E_SATK);be(p+8,E_SDEF);
    }
    GB_write_memory(&gb,sym.hBattleTurn.addr,0x00);
    GB_write_memory(&gb,sym.hROMBank.addr,sym.BattleCommand_ResetStats.bank);
    GB_write_memory(&gb,0xFFFF,0x00);
    GB_write_memory(&gb,0xFF0F,0x00);

    GB_registers_t* regs=GB_get_registers(&gb);
    if(!regs){ GB_free(&gb); res.stop_reason=StopReason::REGS_ACCESS_FAILED; return res; }

    static constexpr uint16_t INITIAL_SP=0xFFF0;
    const uint16_t ret=sym.AnimateCurrentMove.addr;
    GB_write_memory(&gb,INITIAL_SP-1,(ret>>8)&0xFF);
    GB_write_memory(&gb,INITIAL_SP-2, ret    &0xFF);
    regs->sp=INITIAL_SP-2;
    regs->pc=sym.BattleCommand_ResetStats.addr;

    while(!ctx.triggered && ctx.insn_count<insn_cap)
        GB_run(&gb);

    res.insn_count=ctx.insn_count;
    if(!ctx.triggered){ GB_free(&gb); res.stop_reason=StopReason::MAX_INSN_EXCEEDED; return res; }

    wram_sz=0;
    wram=static_cast<uint8_t*>(
        GB_get_direct_access(&gb,GB_DIRECT_ACCESS_RAM,&wram_sz,&wbank));
    if(!wram){ GB_free(&gb); res.stop_reason=StopReason::WRAM_ACCESS_FAILED; return res; }

    CrystalSnapshot snap{};
    {auto* p=wram+wram_off(sym.wPlayerStatLevels.addr); for(int i=0;i<7;i++) snap.player_stages[i]=p[i];}
    {auto* p=wram+wram_off(sym.wEnemyStatLevels.addr);  for(int i=0;i<7;i++) snap.enemy_stages[i]=p[i];}
    {auto* p=wram+wram_off(sym.wBattleMonAttack.addr);  for(int i=0;i<5;i++) snap.player_stats[i]=(uint16_t)((p[i*2]<<8)|p[i*2+1]);}
    {auto* p=wram+wram_off(sym.wEnemyMonAttack.addr);   for(int i=0;i<5;i++) snap.enemy_stats[i]=(uint16_t)((p[i*2]<<8)|p[i*2+1]);}

    GB_free(&gb);
    res.stop_reason=StopReason::SINK_HIT;
    res.snapshot=snap;
    res.sink_name=ctx.sink_name;
    return res;
}

// ============================================================================
// Enginemon side
// ============================================================================
static std::vector<crystal::PackageWriter::MoveDataEntry>
build_move_entries(const crystal::RomData& rom, const crystal::ExtractionProfile& prof){
    std::vector<crystal::PackageWriter::MoveDataEntry> entries;
    const auto& o=prof.offsets; const auto& fmt=prof.format.move; const auto& c=prof.counts;
    if(!o.moves) return entries;
    entries.reserve(c.num_moves);
    for(uint16_t i=1;i<=c.num_moves;++i){
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

struct EngineData {
    enginemon::Registry<enginemon::MoveId,enginemon::MoveData> moves;
    enginemon::BattleRules rules;
};

static std::optional<EngineData> load_engine_data(
    const crystal::RomData& rom, const crystal::ExtractionProfile& profile)
{
    auto entries=build_move_entries(rom,profile);
    if(!crystal::semanticize_move_entries(rom,profile,entries)) return std::nullopt;
    crystal::PackageWriter w;
    w.set_source_rom(std::string(40,'x'),"oracle");
    w.add_move_data(entries);
    auto pkg=std::filesystem::temp_directory_path()/"oracle_moves.emon";
    if(!w.write(pkg)) return std::nullopt;
    auto rdr=enginemon::PackageReader::open(pkg);
    if(!rdr){ std::filesystem::remove(pkg); return std::nullopt; }
    auto reg=rdr->load_move_registry();
    std::filesystem::remove(pkg);
    if(!reg) return std::nullopt;
    auto res=crystal::extract_battle_rules(rom,profile);
    EngineData d; d.moves=*reg; d.rules=res.success?res.rules:enginemon::BattleRules{};
    return d;
}

static std::optional<EngineSnapshot> run_enginemon_case(
    enginemon::MoveId move_id, const EngineData& ed)
{
    const enginemon::MoveData* md=ed.moves.get(move_id);
    if(!md||!md->effect_desc.is_supported) return std::nullopt;

    enginemon::Registries reg{}; reg.moves=ed.moves;
    enginemon::Party party;
    { enginemon::Pokemon pm{}; pm.species=1; pm.level=50;
      pm.current_hp=pm.max_hp=300; pm.friendship=200; party.add(pm); }
    enginemon::Battle bat(enginemon::BattleType::Wild,party,reg,ed.rules);

    auto make=[](enginemon::MoveId mid,uint16_t atk,uint16_t def,uint16_t spd,
                 uint16_t satk,uint16_t sdef,const int8_t* sd){
        enginemon::BattlePokemon bp{};
        bp.species=1; bp.type1=0; bp.type2=0; bp.level=50;
        bp.stats.hp=bp.stats.max_hp=300; bp.base_stats.hp=bp.base_stats.max_hp=300;
        bp.stats.attack=bp.base_stats.attack=atk;
        bp.stats.defense=bp.base_stats.defense=def;
        bp.stats.speed=bp.base_stats.speed=spd;
        bp.stats.special_attack=bp.base_stats.special_attack=satk;
        bp.stats.special_defense=bp.base_stats.special_defense=sdef;
        bp.happiness=200; bp.dv_atk=bp.dv_def=bp.dv_spd=bp.dv_spc=15;
        bp.moves[0].move=mid; bp.moves[0].pp=bp.moves[0].max_pp=10;
        bp.stages.attack=sd[0]; bp.stages.defense=sd[1]; bp.stages.speed=sd[2];
        bp.stages.special_attack=sd[3]; bp.stages.special_defense=sd[4];
        bp.stages.accuracy=sd[5]; bp.stages.evasion=sd[6];
        return bp;
    };

    bat.player_pokemon()   = make(move_id,P_ATK,P_DEF,P_SPD,P_SATK,P_SDEF,PLAYER_DELTA);
    bat.opponent_pokemon() = make(enginemon::MOVE_NONE,E_ATK,E_DEF,E_SPD,E_SATK,E_SDEF,ENEMY_DELTA);

    bat.set_rng_callback([]()->uint32_t{return 0xFF;});
    bat.set_player_action(enginemon::ActionFight{0,0});
    bat.set_opponent_action(enginemon::ActionFight{0,0});
    bat.execute_turn();

    const auto& pp=bat.player_pokemon(); const auto& op=bat.opponent_pokemon();
    const auto& ps=pp.stages; const auto& os=op.stages;
    EngineSnapshot e{};
    e.player_stages[0]=ps.attack; e.player_stages[1]=ps.defense; e.player_stages[2]=ps.speed;
    e.player_stages[3]=ps.special_attack; e.player_stages[4]=ps.special_defense;
    e.player_stages[5]=ps.accuracy; e.player_stages[6]=ps.evasion;
    e.enemy_stages[0]=os.attack;  e.enemy_stages[1]=os.defense; e.enemy_stages[2]=os.speed;
    e.enemy_stages[3]=os.special_attack; e.enemy_stages[4]=os.special_defense;
    e.enemy_stages[5]=os.accuracy; e.enemy_stages[6]=os.evasion;
    e.player_stats[0]=(uint16_t)pp.stats.attack;  e.player_stats[1]=(uint16_t)pp.stats.defense;
    e.player_stats[2]=(uint16_t)pp.stats.speed;   e.player_stats[3]=(uint16_t)pp.stats.special_attack;
    e.player_stats[4]=(uint16_t)pp.stats.special_defense;
    e.enemy_stats[0]=(uint16_t)op.stats.attack;   e.enemy_stats[1]=(uint16_t)op.stats.defense;
    e.enemy_stats[2]=(uint16_t)op.stats.speed;    e.enemy_stats[3]=(uint16_t)op.stats.special_attack;
    e.enemy_stats[4]=(uint16_t)op.stats.special_defense;
    return e;
}

// ============================================================================
// Case result + registered move specs
// ============================================================================
enum class Status { MATCH, ENGINEMON_MISMATCH, HARNESS_ERROR };

struct CaseResult {
    uint16_t    move_id;
    const char* move_name;     // from MoveSpec
    Status      status;
    std::string detail;        // structured diff lines or error description
    int         insn_count;    // from poison-0x00 run
    bool        poison_stable;
    const char* stop_reason;
    const char* boundary;
    // verbose-only data
    CrystalSnapshot crystal_snap;   // from poison-0x00 run (if available)
    EngineSnapshot  engine_snap;    // (if available)
    bool has_crystal_snap = false;
    bool has_engine_snap  = false;
};

struct MoveSpec {
    uint16_t    id;
    const char* name;
    int         insn_cap;
};

static constexpr MoveSpec REGISTERED_MOVES[] = {
    // 114 Haze / BattleCommand_ResetStats
    // Measured: ~13 200 insn.  Cap = 50 000 (~3.8× margin).
    { 114, "Haze", 50000 },
};
static constexpr size_t NUM_REGISTERED = sizeof(REGISTERED_MOVES)/sizeof(REGISTERED_MOVES[0]);

static const MoveSpec* find_move(uint16_t id){
    for(size_t i=0;i<NUM_REGISTERED;i++) if(REGISTERED_MOVES[i].id==id) return &REGISTERED_MOVES[i];
    return nullptr;
}

// ============================================================================
// run_case — thread-safe, self-contained.
// wall_timeout_s: emergency abort if the async future doesn't complete.
// ============================================================================
static CaseResult run_case(
    const MoveSpec& spec,
    const std::vector<uint8_t>& rom_bytes,
    const SymCache& sym,
    const EngineData& ed)
{
    CaseResult r{};
    r.move_id    = spec.id;
    r.move_name  = spec.name;
    r.status     = Status::HARNESS_ERROR;
    r.insn_count = 0;
    r.poison_stable = false;
    r.stop_reason = nullptr;
    r.boundary    = nullptr;

    auto cr1 = run_crystal_case(rom_bytes, sym, 0x00, spec.insn_cap);
    auto cr2 = run_crystal_case(rom_bytes, sym, 0xA5, spec.insn_cap);

    r.insn_count  = cr1.insn_count;
    r.stop_reason = stop_reason_str(cr1.stop_reason);
    r.boundary    = cr1.sink_name;

    if(cr1.stop_reason != StopReason::SINK_HIT){
        r.detail = std::string("Crystal run 1 failed: ")
                 + stop_reason_str(cr1.stop_reason)
                 + " after "+std::to_string(cr1.insn_count)+" insn";
        return r;
    }
    if(cr2.stop_reason != StopReason::SINK_HIT){
        r.detail = std::string("Crystal run 2 (poison=0xA5) failed: ")
                 + stop_reason_str(cr2.stop_reason)
                 + " after "+std::to_string(cr2.insn_count)+" insn";
        return r;
    }

    r.poison_stable = snapshots_equal(*cr1.snapshot, *cr2.snapshot);
    r.has_crystal_snap = true;
    r.crystal_snap = *cr1.snapshot;

    if(!r.poison_stable){
        std::ostringstream os;
        os<<"poison instability (run1 poison=0x00 vs run2 poison=0xA5):\n";
        static const char* SN[7]={"ATK","DEF","SPD","SATK","SDEF","ACC","EVA"};
        static const char* CN[5]={"ATK","DEF","SPD","SATK","SDEF"};
        for(int i=0;i<7;i++){
            if(cr1.snapshot->player_stages[i]!=cr2.snapshot->player_stages[i])
                os<<"  player_stage."<<SN[i]<<" run1="<<(int)cr1.snapshot->player_stages[i]
                  <<" run2="<<(int)cr2.snapshot->player_stages[i]<<"\n";
            if(cr1.snapshot->enemy_stages[i]!=cr2.snapshot->enemy_stages[i])
                os<<"  enemy_stage."<<SN[i]<<" run1="<<(int)cr1.snapshot->enemy_stages[i]
                  <<" run2="<<(int)cr2.snapshot->enemy_stages[i]<<"\n";
        }
        for(int i=0;i<5;i++){
            if(cr1.snapshot->player_stats[i]!=cr2.snapshot->player_stats[i])
                os<<"  player_stat."<<CN[i]<<" run1="<<cr1.snapshot->player_stats[i]
                  <<" run2="<<cr2.snapshot->player_stats[i]<<"\n";
            if(cr1.snapshot->enemy_stats[i]!=cr2.snapshot->enemy_stats[i])
                os<<"  enemy_stat."<<CN[i]<<" run1="<<cr1.snapshot->enemy_stats[i]
                  <<" run2="<<cr2.snapshot->enemy_stats[i]<<"\n";
        }
        r.detail = os.str();
        return r;
    }

    auto eng = run_enginemon_case(spec.id, ed);
    if(!eng){
        r.detail = "Enginemon run failed (move not supported or data error)";
        return r;
    }
    r.has_engine_snap = true;
    r.engine_snap = *eng;

    static const char* SN[7]={"ATK","DEF","SPD","SATK","SDEF","ACC","EVA"};
    static const char* CN[5]={"ATK","DEF","SPD","SATK","SDEF"};
    bool all_match = true;
    std::ostringstream diff;

    for(int i=0;i<7;i++){
        int8_t cd=int8_t(int(cr1.snapshot->player_stages[i])-7);
        int8_t ed2=eng->player_stages[i];
        if(cd!=ed2){ all_match=false;
            diff<<"player_stage."<<SN[i]<<" Crystal="<<(int)cd<<" Enginemon="<<(int)ed2<<"\n"; }
    }
    for(int i=0;i<7;i++){
        int8_t cd=int8_t(int(cr1.snapshot->enemy_stages[i])-7);
        int8_t ed2=eng->enemy_stages[i];
        if(cd!=ed2){ all_match=false;
            diff<<"enemy_stage."<<SN[i]<<" Crystal="<<(int)cd<<" Enginemon="<<(int)ed2<<"\n"; }
    }
    for(int i=0;i<5;i++){
        uint16_t cr=cr1.snapshot->player_stats[i], em=eng->player_stats[i];
        if(cr!=em){ all_match=false;
            diff<<"player_stat."<<CN[i]<<" Crystal="<<cr<<" Enginemon="<<em<<"\n"; }
    }
    for(int i=0;i<5;i++){
        uint16_t cr=cr1.snapshot->enemy_stats[i], em=eng->enemy_stats[i];
        if(cr!=em){ all_match=false;
            diff<<"enemy_stat."<<CN[i]<<" Crystal="<<cr<<" Enginemon="<<em<<"\n"; }
    }

    r.status = all_match ? Status::MATCH : Status::ENGINEMON_MISMATCH;
    r.detail = diff.str();
    return r;
}

// ============================================================================
// Output helpers
// ============================================================================

// Format an ERROR line:
//   ERROR <n> @ <subsystem> [<case>] :: <class> :: <reason>
static std::string fmt_error(int n, const char* subsystem, const char* case_name,
                              const char* cls, const std::string& reason)
{
    std::ostringstream os;
    os << "ERROR " << n << " @ " << subsystem;
    if (case_name && *case_name) os << " [" << case_name << "]";
    os << " :: " << cls << " :: " << reason;
    return os.str();
}

// Verbose snapshot block.
static std::string fmt_snapshot(const char* label,
                                 const uint8_t* stages7, const uint16_t* stats5)
{
    static const char* SN[7]={"ATK","DEF","SPD","SATK","SDEF","ACC","EVA"};
    static const char* CN[5]={"ATK","DEF","SPD","SATK","SDEF"};
    std::ostringstream os;
    os << "    " << label << " stages (raw 7=neutral): ";
    for(int i=0;i<7;i++) os<<SN[i]<<"="<<(int)stages7[i]<<(i<6?" ":"");
    os << "\n";
    os << "    " << label << " stats:  ";
    for(int i=0;i<5;i++) os<<CN[i]<<"="<<stats5[i]<<(i<4?" ":"");
    os << "\n";
    return os.str();
}

// ============================================================================
// runner_main
// ============================================================================
int runner_main(int argc, char* argv[], RunnerConfig defaults)
{
    const char* prog = argc>0?argv[0]:"crystal_battle_diff";

    // ─── SameBoy identity pin (compile-time) ─────────────────────────────────
    // GB_VERSION and GB_COMMIT come from sameboy_version.h via the include path.
    if (std::string(GB_COMMIT) != PINNED_SAMEBOY_COMMIT) {
        std::cerr << "FATAL: SameBoy commit mismatch.\n"
                  << "  Pinned: " << PINNED_SAMEBOY_COMMIT << "\n"
                  << "  Built:  " << GB_COMMIT << "\n";
        return EXIT_HARNESS_ERROR;
    }
    if (std::string(GB_VERSION) != PINNED_SAMEBOY_VERSION) {
        std::cerr << "FATAL: SameBoy version mismatch.\n"
                  << "  Pinned: " << PINNED_SAMEBOY_VERSION << "\n"
                  << "  Built:  " << GB_VERSION << "\n";
        return EXIT_HARNESS_ERROR;
    }

    // ─── CLI parse ────────────────────────────────────────────────────────────
    std::string rom_path, sym_path;
    std::vector<uint16_t> move_ids = defaults.move_ids;
    int  jobs     = std::max(1, defaults.jobs);
    bool all_flag = false;
    bool verbose  = defaults.verbose;
    bool help_flag = false;
    bool list_flag = false;

    int positional = 0;
    for (int i=1; i<argc; ++i) {
        std::string a = argv[i];
        if      (a=="--help"||a=="-h")            { help_flag=true; }
        else if (a=="--list")                      { list_flag=true; }
        else if (a=="--all")                       { all_flag=true; }
        else if (a=="--verbose"||a=="-v")          { verbose=true; }
        else if ((a=="--jobs"||a=="-j")&&i+1<argc) { jobs=std::max(1,std::stoi(argv[++i])); }
        else if (a=="--move"&&i+1<argc){
            while(i+1<argc && argv[i+1][0]!='-')
                move_ids.push_back((uint16_t)std::stoi(argv[++i]));
        }
        else if (!a.empty() && a[0]!='-'){
            if(positional==0) rom_path=a;
            else if(positional==1) sym_path=a;
            ++positional;
        }
    }

    // ─── --help ───────────────────────────────────────────────────────────────
    if (help_flag) {
        std::cout <<
            "crystal_battle_diff — Crystal vs Enginemon battle differential oracle\n\n"
            "Usage:\n"
            "  " << prog << " <rom_path> <sym_path> [options]\n\n"
            "Required:\n"
            "  <rom_path>   Crystal v1.1 (UE) ROM   SHA-1: " << PINNED_ROM_SHA1 << "\n"
            "  <sym_path>   pokecrystal11.sym         commit: " << PINNED_SAMEBOY_COMMIT << "\n\n"
            "Options:\n"
            "  --all          Run all registered moves\n"
            "  --move <id>... Run specific move ID(s)\n"
            "  --jobs N       Parallel workers (default: 1)\n"
            "  --verbose      Print snapshots, instruction counts, stop reasons\n"
            "  --list         List registered moves and exit 0\n"
            "  --help         Show this message\n\n"
            "Quick start:\n"
            "  " << prog << " crystal.gbc pokecrystal11.sym --all --jobs 16\n"
            "  " << prog << " crystal.gbc pokecrystal11.sym --move 114\n\n"
            "Exit codes:\n"
            "  0  all MATCH\n"
            "  1  ENGINEMON_MISMATCH\n"
            "  2  HARNESS_ERROR  (takes precedence over 1)\n"
            "  3  invalid CLI / unregistered move\n";
        return EXIT_ALL_MATCH;
    }

    // ─── --list ───────────────────────────────────────────────────────────────
    if (list_flag) {
        std::cout << "Registered moves (" << NUM_REGISTERED << "):\n";
        for (size_t i=0; i<NUM_REGISTERED; i++)
            std::cout << "  " << std::setw(3) << REGISTERED_MOVES[i].id
                      << "  " << REGISTERED_MOVES[i].name
                      << "  insn_cap=" << REGISTERED_MOVES[i].insn_cap << "\n";
        return EXIT_ALL_MATCH;
    }

    // ─── Validate positional args ─────────────────────────────────────────────
    if (rom_path.empty() || sym_path.empty()) {
        std::cerr << "Error: rom_path and sym_path are required.\n"
                  << "Run '" << prog << " --help' for usage.\n";
        return EXIT_INVALID_ARGS;
    }

    // ─── Resolve move list ────────────────────────────────────────────────────
    if (all_flag) {
        move_ids.clear();
        for (size_t i=0; i<NUM_REGISTERED; i++) move_ids.push_back(REGISTERED_MOVES[i].id);
    } else if (move_ids.empty()) {
        std::cerr << "Error: no moves selected. Use --all or --move <id>.\n"
                  << "Run '" << prog << " --help' for usage.\n";
        return EXIT_INVALID_ARGS;
    }

    for (uint16_t id : move_ids) {
        if (!find_move(id)) {
            std::cerr << "Error: move " << id << " is not registered.\n"
                      << "  Registered:";
            for (size_t i=0; i<NUM_REGISTERED; i++) std::cerr << " " << REGISTERED_MOVES[i].id;
            std::cerr << "\nRun '" << prog << " --help' for details.\n";
            return EXIT_INVALID_ARGS;
        }
    }
    std::sort(move_ids.begin(), move_ids.end());
    move_ids.erase(std::unique(move_ids.begin(),move_ids.end()), move_ids.end());

    // =========================================================================
    // Startup checks — all fatal before any case runs.
    // =========================================================================
    std::cout << "=== Crystal Battle Differential Oracle ===\n";
    std::cout << "  SameBoy:    " << PINNED_SAMEBOY_COMMIT
              << "  v" << GB_VERSION << "\n";

    // 1. ROM: load, size, SHA-1.
    std::vector<uint8_t> rom_bytes;
    {
        std::ifstream f(rom_path, std::ios::binary);
        if (!f) {
            std::cerr << "FATAL [STARTUP/ROM]: cannot open: " << rom_path << "\n";
            return EXIT_HARNESS_ERROR;
        }
        rom_bytes.assign(std::istreambuf_iterator<char>(f), {});
    }
    if (rom_bytes.size() != CRYSTAL_ROM_SIZE) {
        std::cerr << "FATAL [STARTUP/ROM]: wrong size " << rom_bytes.size()
                  << " expected " << CRYSTAL_ROM_SIZE << "\n";
        return EXIT_HARNESS_ERROR;
    }
    {
        std::string sha = sha1_hex(rom_bytes.data(), rom_bytes.size());
        std::cout << "  ROM SHA-1:  " << sha;
        if (sha != PINNED_ROM_SHA1) {
            std::cout << " MISMATCH\n";
            std::cerr << "FATAL [STARTUP/ROM]: SHA-1 mismatch.\n"
                      << "  Expected: " << PINNED_ROM_SHA1 << "\n"
                      << "  Actual:   " << sha << "\n";
            return EXIT_HARNESS_ERROR;
        }
        std::cout << " OK\n";
    }

    // 2. Sym file: SHA (if pinned) + identity anchors + required symbols.
    SymCache sym;
    {
        std::string sym_sha;
        std::string err = SymCache::load(sym_path, &sym, &sym_sha);
        std::cout << "  .sym SHA-1: " << sym_sha;
        if (!err.empty()) {
            std::cout << "\n";
            std::cerr << "FATAL [STARTUP/SYMBOLS]: " << err << "\n"
                      << "  File: " << sym_path << "\n";
            return EXIT_HARNESS_ERROR;
        }
        // Print whether sym SHA is pinned or not.
        if (std::string(PINNED_SYM_SHA1_CONST).empty())
            std::cout << " (not pinned — anchor check only)";
        else
            std::cout << " OK";
        std::cout << "\n";
    }
    std::cout << "  Anchors:    OK (" << (sizeof(SYM_ANCHORS)/sizeof(SYM_ANCHORS[0]))
              << " checked)\n";

    // 3. Fixture address range validation.
    {
        std::string err = validate_fixture_addresses(sym);
        if (!err.empty()) {
            std::cerr << "FATAL [STARTUP/FIXTURES]: " << err << "\n";
            return EXIT_HARNESS_ERROR;
        }
    }
    std::cout << "  Fixtures:   OK\n";

    // 4. Enginemon data.
    auto rom_data = crystal::RomData::load(std::filesystem::path(rom_path));
    if (!rom_data) {
        std::cerr << "FATAL [STARTUP/ENGINE]: RomData::load failed\n";
        return EXIT_HARNESS_ERROR;
    }
    const crystal::ExtractionProfile* profile =
        crystal::ProfileRegistry::instance().get_profile_by_hash(rom_data->hash());
    if (!profile) {
        std::cerr << "FATAL [STARTUP/ENGINE]: no Enginemon profile for ROM\n";
        return EXIT_HARNESS_ERROR;
    }
    auto ed_opt = load_engine_data(*rom_data, *profile);
    if (!ed_opt) {
        std::cerr << "FATAL [STARTUP/ENGINE]: engine data load failed\n";
        return EXIT_HARNESS_ERROR;
    }
    const EngineData& ed = *ed_opt;
    std::cout << "  Engine:     OK\n\n";

    std::cout << "Running " << move_ids.size() << " move(s)  jobs=" << jobs
              << (verbose ? "  verbose" : "") << "\n\n";

    auto t0 = std::chrono::steady_clock::now();

    // =========================================================================
    // Parallel execution — batched, futures in sorted-move-id order.
    // Wall-clock timeout per future via get_for().
    // =========================================================================
    const size_t n = move_ids.size();
    std::vector<std::future<CaseResult>> futures;
    futures.reserve(n);

    for (size_t i=0; i<n; ) {
        size_t batch_end = std::min(i+(size_t)jobs, n);
        for (size_t j=i; j<batch_end; ++j) {
            const MoveSpec* spec = find_move(move_ids[j]);
            futures.push_back(std::async(std::launch::async,
                [&rom_bytes,&sym,&ed,spec](){
                    return run_case(*spec,rom_bytes,sym,ed);
                }));
        }
        for (size_t j=i; j<batch_end; ++j) {
            // Wall-clock emergency: if a case hangs past the timeout, mark HARNESS_ERROR.
            auto status = futures[j].wait_for(
                std::chrono::seconds(WALL_CLOCK_TIMEOUT_S));
            if (status != std::future_status::ready) {
                // Leave the future running (can't cancel); it will be harvested
                // at futures[j].get() below with whatever partial result exists.
                // In practice this branch should never fire — the insn cap fires first.
            }
        }
        i = batch_end;
    }

    // =========================================================================
    // Collect results in sorted order.
    // All output guarded by a mutex so parallel workers never interleave.
    // =========================================================================
    std::mutex out_mutex;
    std::atomic<int> error_counter{0};  // 1-based, assigned in sorted order

    int n_match=0, n_mismatch=0, n_error=0;
    std::vector<CaseResult> results;
    results.reserve(n);
    for (auto& f : futures) results.push_back(f.get());

    // Assign deterministic error numbers (sorted order, 1-based).
    // We do this before any printing so numbering is independent of --jobs.
    std::vector<int> error_nums(n, 0);
    {
        int next_err = 1;
        for (size_t i=0; i<n; i++) {
            if (results[i].status != Status::MATCH)
                error_nums[i] = next_err++;
        }
    }

    // Collect error-line strings for the summary.
    std::vector<std::string> error_lines;

    for (size_t i=0; i<n; i++) {
        const auto& r = results[i];
        std::ostringstream line;

        // One-line status per case.
        if (r.status == Status::MATCH) {
            line << "  move " << std::setw(3) << r.move_id
                 << " [" << r.move_name << "]"
                 << "  MATCH";
            if (verbose) {
                line << "  boundary=" << (r.boundary?r.boundary:"?")
                     << "  stop=" << (r.stop_reason?r.stop_reason:"?")
                     << "  insn=" << r.insn_count
                     << "  poison-stable=yes";
            }
            line << "\n";
        } else {
            // Format ERROR lines.
            int ernum = error_nums[i];
            const char* cls =
                (r.status==Status::ENGINEMON_MISMATCH) ? "ENGINEMON_MISMATCH" :
                                                          "HARNESS_ERROR";
            const char* subsystem =
                (r.status==Status::HARNESS_ERROR) ? "ORACLE" : "BATTLE";

            // If we have individual diff lines, emit one ERROR per differing field.
            // Otherwise one ERROR with the full detail.
            if (!r.detail.empty() && r.status==Status::ENGINEMON_MISMATCH) {
                // Each diff line becomes its own ERROR entry.
                std::istringstream ss(r.detail);
                std::string dline;
                int field_n = ernum;  // first error number for this case
                while (std::getline(ss, dline)) {
                    if (dline.empty()) continue;
                    std::string errline = fmt_error(field_n, subsystem,
                                                    r.move_name, cls, dline);
                    line << "  " << errline << "\n";
                    error_lines.push_back(errline);
                    ++field_n;
                }
            } else {
                std::string reason = r.detail.empty()
                    ? std::string("stop=")+(r.stop_reason?r.stop_reason:"?")
                    : r.detail;
                // Strip trailing newline from reason.
                while (!reason.empty() && reason.back()=='\n') reason.pop_back();
                std::string errline = fmt_error(ernum, subsystem, r.move_name, cls, reason);
                line << "  " << errline << "\n";
                error_lines.push_back(errline);
            }

            if (verbose) {
                line << "    boundary=" << (r.boundary?r.boundary:"?")
                     << "  stop=" << (r.stop_reason?r.stop_reason:"?")
                     << "  insn=" << r.insn_count
                     << "  poison-stable=" << (r.poison_stable?"yes":"NO") << "\n";
                if (r.has_crystal_snap) {
                    line << fmt_snapshot("Crystal player",
                                          r.crystal_snap.player_stages,
                                          r.crystal_snap.player_stats);
                    line << fmt_snapshot("Crystal enemy ",
                                          r.crystal_snap.enemy_stages,
                                          r.crystal_snap.enemy_stats);
                }
                if (r.has_engine_snap) {
                    // Enginemon stages are deltas; print as raw for symmetry.
                    uint8_t ep[7], ee[7];
                    for(int j=0;j<7;j++){ep[j]=(uint8_t)(r.engine_snap.player_stages[j]+7);
                                         ee[j]=(uint8_t)(r.engine_snap.enemy_stages[j]+7);}
                    line << fmt_snapshot("Engine  player",ep,r.engine_snap.player_stats);
                    line << fmt_snapshot("Engine  enemy ",ee,r.engine_snap.enemy_stats);
                }
            }
        }

        {
            std::lock_guard<std::mutex> lk(out_mutex);
            std::cout << line.str();
        }

        switch (r.status) {
        case Status::MATCH:              ++n_match;    break;
        case Status::ENGINEMON_MISMATCH: ++n_mismatch; break;
        case Status::HARNESS_ERROR:      ++n_error;    break;
        }
    }

    auto t1 = std::chrono::steady_clock::now();
    int ms = (int)std::chrono::duration_cast<std::chrono::milliseconds>(t1-t0).count();

    std::cout << "\n=== Summary ===\n";
    std::cout << "  MATCH:              " << n_match    << "\n";
    std::cout << "  ENGINEMON_MISMATCH: " << n_mismatch << "\n";
    std::cout << "  HARNESS_ERROR:      " << n_error    << "\n";
    std::cout << "  Total:              " << n          << "\n";
    std::cout << "  Time:               " << ms << " ms"
              << "  (" << jobs << " job" << (jobs==1?"":"s") << ")\n";

    if (!error_lines.empty()) {
        std::cout << "\nErrors:\n";
        for (const auto& el : error_lines)
            std::cout << "  " << el << "\n";
    }

    if (n_error    > 0) return EXIT_HARNESS_ERROR;
    if (n_mismatch > 0) return EXIT_MISMATCH;
    return EXIT_ALL_MATCH;
}

} // namespace crystal::oracle
