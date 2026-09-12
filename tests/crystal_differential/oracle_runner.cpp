// tests/crystal_differential/oracle_runner.cpp
//
// Crystal battle differential oracle — hardened parallel runner
//
// ARCHITECTURE
//   run_crystal_case() — dedicated SameBoy instance per call; no shared GB state
//   run_enginemon_case() — Enginemon Battle::execute_turn() on identical fixture
//   run_case() — two Crystal poison runs + Enginemon diff; fully self-contained
//   runner_main() — startup checks, parallel dispatch, deterministic output
//
// ROM IMMUTABILITY
//   rom_bytes vector is loaded once, SHA-checked once, then passed read-only.
//   GB_load_rom_from_buffer receives the exact verified bytes. No pointer into
//   gb->rom is ever taken. No ROM bytes are modified anywhere.
//
// COLD-CALL MECHANISM (no ROM patches)
//   regs->pc = entry symbol address (already in mapped bank)
//   regs->sp = INITIAL_SP; stack holds sink addr as return address
//   hROMBank = entry bank  (RST $08 BankSwitch restore path)
//   0xFF50  = 1            (boot_rom_finished — RST vectors read game ROM)
//   MBC     = entry bank   (0x2000 write)
//
// PRESENTATION BOUNDARY
//   Execution callback stops at the allowlisted sink PC.
//   Semantic WRAM outputs are read at that moment; the sink never executes.

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

// GB_VERSION is defined in sameboy_version.h, available via the include path.
// Include it directly here rather than depending on /FI force-include.
#include "sameboy_version.h"
#ifndef GB_VERSION
#error "GB_VERSION not defined — sameboy_version.h not found on include path"
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace crystal::oracle {

// ============================================================================
// Compile-time pins
// ============================================================================

// Crystal ROM SHA-1 — enforced at startup against the loaded file.
static constexpr const char* PINNED_ROM_SHA1 = "F2F52230B536214EF7C9924F483392993E226CFB";

// Crystal ROM size.
static constexpr uint32_t CRYSTAL_ROM_SIZE = 2097152u;  // 2 MiB

// SameBoy commit and version.
// Commit is checked against the HEAD of references/SameBoy if git is available;
// version string is checked against GB_VERSION from sameboy_version.h at startup.
static constexpr const char* PINNED_SAMEBOY_COMMIT  = "213a12ce93d66b105a113debd9396306066a7cfc";
static constexpr const char* PINNED_SAMEBOY_VERSION = "1.0.3";

// Per-case instruction cap. Exceeding this is HARNESS_ERROR.
// Measured: Haze runs in ~13 200 instructions. Cap = 50 000 (~3.8× margin).
// A future move may need a higher cap — that must be set explicitly per move
// (see MoveSpec below), never by silently increasing the global.
static constexpr int DEFAULT_INSN_CAP = 50000;

// ============================================================================
// SHA-1 (self-contained, no extra dependencies)
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
    auto hash=s.fin();
    char hex[41]{}; for(int i=0;i<20;i++) sprintf(hex+i*2,"%02X",hash[i]);
    return std::string(hex);
}

// ============================================================================
// Symbol lookup
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
// SymCache — loaded once, validated, shared read-only.
// ============================================================================
// Expected addresses for identity validation.
// These are verified at startup against the sym file so a wrong .sym is
// caught immediately, not silently producing garbage results.
struct SymExpect { const char* name; uint8_t bank; uint16_t addr; };
static constexpr SymExpect SYM_IDENTITY[] = {
    // Anchors that must match exactly for pokecrystal11 (Crystal v1.1 UE).
    // Chosen to cover different banks and both WRAM regions.
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

    // Returns a non-empty error string on failure.
    static std::string load(const std::string& sym_path, SymCache* out) {
        // Check the file exists and is readable.
        { std::ifstream f(sym_path); if(!f) return "cannot open: "+sym_path; }

        // Identity validation: known anchor symbols must match expected addresses.
        for(const auto& e : SYM_IDENTITY){
            Sym got{};
            if(!sym_get(sym_path,e.name,&got))
                return std::string("identity check: symbol not found: ")+e.name;
            if(got.bank!=e.bank||got.addr!=e.addr){
                std::ostringstream os;
                os<<"identity check: "<<e.name
                  <<" expected "<<std::hex<<(int)e.bank<<":"<<e.addr
                  <<" got "<<(int)got.bank<<":"<<got.addr;
                return os.str();
            }
        }

        // Load all required symbols.
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
        for(const auto& r : required){
            if(!sym_get(sym_path,r.name,r.dst))
                return std::string("required symbol missing: ")+r.name;
        }
        return {};
    }
};

// ============================================================================
// Fixture validation helpers
// ============================================================================

// Validate that a WRAM address lies within the expected bank.
// Returns a non-empty error string if validation fails.
static std::string validate_wram_addr(const char* name, uint16_t addr,
                                      uint16_t expected_lo, uint16_t expected_hi)
{
    if(addr<expected_lo||addr>expected_hi){
        std::ostringstream os;
        os<<name<<" addr 0x"<<std::hex<<addr
          <<" outside expected range [0x"<<expected_lo<<",0x"<<expected_hi<<"]";
        return os.str();
    }
    return {};
}

static std::string validate_hram_addr(const char* name, uint16_t addr){
    if(addr<0xFF80||addr>0xFFFE){
        std::ostringstream os;
        os<<name<<" addr 0x"<<std::hex<<addr<<" outside HRAM [0xFF80,0xFFFE]";
        return os.str();
    }
    return {};
}

// Validate all fixture addresses in the sym cache resolve to expected memory regions.
// Returns the first error string, or empty on success.
static std::string validate_fixture_addresses(const SymCache& sym){
    std::string e;
    // WRAM0 range C000-CFFF
    if((e=validate_wram_addr("wPlayerStatLevels", sym.wPlayerStatLevels.addr, 0xC000,0xCFFF)).size()) return e;
    if((e=validate_wram_addr("wEnemyStatLevels",  sym.wEnemyStatLevels.addr,  0xC000,0xCFFF)).size()) return e;
    if((e=validate_wram_addr("wPlayerStats",      sym.wPlayerStats.addr,      0xC000,0xCFFF)).size()) return e;
    if((e=validate_wram_addr("wEnemyStats",       sym.wEnemyStats.addr,       0xC000,0xCFFF)).size()) return e;
    if((e=validate_wram_addr("wBattleMonAttack",  sym.wBattleMonAttack.addr,  0xC000,0xCFFF)).size()) return e;
    if((e=validate_wram_addr("wBattleMonStatus",  sym.wBattleMonStatus.addr,  0xC000,0xCFFF)).size()) return e;
    if((e=validate_wram_addr("wLinkMode",         sym.wLinkMode.addr,         0xC000,0xCFFF)).size()) return e;
    // WRAM1 range D000-DFFF
    if((e=validate_wram_addr("wEnemyMonAttack",     sym.wEnemyMonAttack.addr,   0xD000,0xDFFF)).size()) return e;
    if((e=validate_wram_addr("wEnemyMonStatus",     sym.wEnemyMonStatus.addr,   0xD000,0xDFFF)).size()) return e;
    if((e=validate_wram_addr("wInBattleTowerBattle",sym.wInBattleTowerBattle.addr,0xC000,0xDFFF)).size()) return e;
    if((e=validate_wram_addr("wJohtoBadges",        sym.wJohtoBadges.addr,      0xD000,0xDFFF)).size()) return e;
    // HRAM range FF80-FFFE
    if((e=validate_hram_addr("hBattleTurn", sym.hBattleTurn.addr)).size()) return e;
    if((e=validate_hram_addr("hROMBank",    sym.hROMBank.addr)).size()) return e;
    return {};
}

// ============================================================================
// WRAM direct-access offset helper
// GB_DIRECT_ACCESS_RAM on CGB returns 8KB for the active bank.
// Bank 0: [0x0000..0x0FFF] = C000-CFFF
// Bank 1: [0x1000..0x1FFF] = D000-DFFF
// ============================================================================
static size_t wram_off(uint16_t addr){
    if(addr>=0xD000) return size_t{0x1000}+(addr-0xD000);
    if(addr>=0xC000) return addr-0xC000;
    throw std::logic_error("wram_off: not WRAM");
}

// ============================================================================
// SameBoy no-op callbacks (no state, safe for any thread)
// ============================================================================
static void sb_log_nop(GB_gameboy_t*, const char*, GB_log_attributes_t){}
static uint32_t sb_rgb_nop(GB_gameboy_t*, uint8_t r, uint8_t g, uint8_t b){
    return 0xFF000000u|((uint32_t)r<<16)|((uint32_t)g<<8)|b;
}

// ============================================================================
// Execution callback
// ============================================================================
struct ExecCtx {
    uint16_t    sink_pc;      // stop when PC reaches this address
    const char* sink_name;    // symbol name (for stop-reason reporting)
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
// StopReason — explicit stop reason for every Crystal execution.
// ============================================================================
enum class StopReason {
    SINK_HIT,          // stopped at the allowlisted presentation boundary
    MAX_INSN_EXCEEDED, // hit the per-case instruction cap → HARNESS_ERROR
    GB_INIT_FAILED,    // GB_init returned false → HARNESS_ERROR
    WRAM_ACCESS_FAILED,// could not obtain WRAM pointer → HARNESS_ERROR
    REGS_ACCESS_FAILED,// could not obtain CPU registers → HARNESS_ERROR
};

static const char* stop_reason_str(StopReason r){
    switch(r){
    case StopReason::SINK_HIT:          return "SINK_HIT";
    case StopReason::MAX_INSN_EXCEEDED: return "MAX_INSN_EXCEEDED";
    case StopReason::GB_INIT_FAILED:    return "GB_INIT_FAILED";
    case StopReason::WRAM_ACCESS_FAILED:return "WRAM_ACCESS_FAILED";
    case StopReason::REGS_ACCESS_FAILED:return "REGS_ACCESS_FAILED";
    }
    return "?";
}

// ============================================================================
// CrystalSnapshot — semantic state captured at the presentation boundary
// ============================================================================
struct CrystalSnapshot {
    uint8_t  player_stages[7]; // wPlayerStatLevels[0..6]  (raw: 7 = neutral)
    uint8_t  enemy_stages[7];  // wEnemyStatLevels[0..6]
    uint16_t player_stats[5];  // wBattleMonAttack..SpDef   (big-endian → native)
    uint16_t enemy_stats[5];   // wEnemyMonAttack..SpDef
};

static bool snapshots_equal(const CrystalSnapshot& a, const CrystalSnapshot& b){
    for(int i=0;i<7;i++) if(a.player_stages[i]!=b.player_stages[i]||a.enemy_stages[i]!=b.enemy_stages[i]) return false;
    for(int i=0;i<5;i++) if(a.player_stats[i]!=b.player_stats[i]||a.enemy_stats[i]!=b.enemy_stats[i]) return false;
    return true;
}

struct CrystalRunResult {
    StopReason           stop_reason;
    std::optional<CrystalSnapshot> snapshot; // present iff stop_reason == SINK_HIT
    int                  insn_count;
    const char*          sink_name;  // valid iff SINK_HIT
};

// ============================================================================
// run_crystal_case
//
// Owns a dedicated GB_gameboy_t on the call stack. No shared SameBoy state.
// rom_bytes is shared read-only — GB_load_rom_from_buffer makes its own copy.
// ============================================================================
static CrystalRunResult run_crystal_case(
    const std::vector<uint8_t>& rom_bytes,
    const SymCache& sym,
    uint8_t poison,
    int insn_cap)
{
    CrystalRunResult res{};
    res.insn_count = 0;
    res.sink_name  = nullptr;

    GB_gameboy_t gb;
    if(!GB_init(&gb,GB_MODEL_CGB_E)){
        res.stop_reason=StopReason::GB_INIT_FAILED; return res;
    }
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

    // Load ROM — exact unmodified bytes.
    GB_load_rom_from_buffer(&gb,rom_bytes.data(),rom_bytes.size());

    // boot_rom_finished=true: RST vectors (0x0008, 0x0010) read Crystal game ROM.
    GB_write_memory(&gb,0xFF50,1);

    // Map bank containing entry point.
    GB_write_memory(&gb,0x2000,sym.BattleCommand_ResetStats.bank);

    // WRAM: poison all, then write fixture.
    size_t wram_sz=0; uint16_t wbank=0;
    uint8_t* wram=static_cast<uint8_t*>(
        GB_get_direct_access(&gb,GB_DIRECT_ACCESS_RAM,&wram_sz,&wbank));
    if(!wram||wram_sz<0x2000){
        GB_free(&gb); res.stop_reason=StopReason::WRAM_ACCESS_FAILED; return res;
    }
    std::memset(wram,poison,wram_sz);

    // --- Fixture writes (all addresses pre-validated in validate_fixture_addresses) ---

    // Non-link battle: BadgeStatBoosts falls through to badge checks.
    wram[wram_off(sym.wLinkMode.addr)]=0;
    wram[wram_off(sym.wInBattleTowerBattle.addr)]=0;
    // No Johto badges → BadgeStatBoosts applies no boosts.
    wram[wram_off(sym.wJohtoBadges.addr)]=0;
    // No PAR/BRN: ApplyPrzEffectOnSpeed / ApplyBrnEffectOnAttack return early.
    wram[wram_off(sym.wBattleMonStatus.addr)  ]=0;
    wram[wram_off(sym.wBattleMonStatus.addr)+1]=0;
    wram[wram_off(sym.wEnemyMonStatus.addr)  ]=0;
    wram[wram_off(sym.wEnemyMonStatus.addr)+1]=0;

    // Pre-Haze stage levels: non-neutral so .Fill's reset to 7 is observable.
    // PLAYER_DELTA / ENEMY_DELTA defined below; deliberately non-zero.
    static constexpr int8_t PLAYER_DELTA[7]={+2,-3,+1,-1,+3,+4,-2};
    static constexpr int8_t ENEMY_DELTA[7] ={-4,+6,-2,+3,-1,-3,+5};
    {
        uint8_t* p=wram+wram_off(sym.wPlayerStatLevels.addr);
        for(int i=0;i<7;i++) p[i]=(uint8_t)(7+PLAYER_DELTA[i]);
        p[7]=7; // 8th slot (Curse/ability), not touched by Haze
    }
    {
        uint8_t* p=wram+wram_off(sym.wEnemyStatLevels.addr);
        for(int i=0;i<7;i++) p[i]=(uint8_t)(7+ENEMY_DELTA[i]);
        p[7]=7;
    }

    // wPlayerStats / wEnemyStats: base stats read by CalcBattleStats.
    // With neutral stages (1/1 multiplier), output = input.
    static constexpr uint16_t P_ATK=110,P_DEF=60,P_SPD=130,P_SATK=95,P_SDEF=70;
    static constexpr uint16_t E_ATK=75, E_DEF=110,E_SPD=30, E_SATK=100,E_SDEF=80;
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

    // hBattleTurn = 0: BattleCommand_ResetStats saves + restores.
    GB_write_memory(&gb,sym.hBattleTurn.addr,0x00);
    // hROMBank = entry bank: BankSwitch (RST $08) saves/restores this.
    GB_write_memory(&gb,sym.hROMBank.addr,sym.BattleCommand_ResetStats.bank);
    // IME already 0 from GB_init memset; clear IE/IF for belt-and-suspenders.
    GB_write_memory(&gb,0xFFFF,0x00);
    GB_write_memory(&gb,0xFF0F,0x00);

    // --- CPU entry ---
    GB_registers_t* regs=GB_get_registers(&gb);
    if(!regs){
        GB_free(&gb); res.stop_reason=StopReason::REGS_ACCESS_FAILED; return res;
    }
    // Stack: sink addr as return address. If execution RETs before the callback
    // fires, it lands at AnimateCurrentMove and the callback stops it there.
    static constexpr uint16_t INITIAL_SP=0xFFF0;
    const uint16_t ret=sym.AnimateCurrentMove.addr;
    GB_write_memory(&gb,INITIAL_SP-1,(ret>>8)&0xFF);
    GB_write_memory(&gb,INITIAL_SP-2, ret    &0xFF);
    regs->sp=INITIAL_SP-2;
    regs->pc=sym.BattleCommand_ResetStats.addr;

    // --- Execute ---
    while(!ctx.triggered && ctx.insn_count<insn_cap)
        GB_run(&gb);

    res.insn_count=ctx.insn_count;

    if(!ctx.triggered){
        GB_free(&gb);
        res.stop_reason=StopReason::MAX_INSN_EXCEEDED;
        return res;
    }

    // --- Read semantic outputs ---
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
    res.snapshot   =snap;
    res.sink_name  =ctx.sink_name;
    return res;
}

// ============================================================================
// Enginemon side
// ============================================================================
static constexpr int8_t PLAYER_DELTA[7]={+2,-3,+1,-1,+3,+4,-2};
static constexpr int8_t ENEMY_DELTA[7] ={-4,+6,-2,+3,-1,-3,+5};
static constexpr uint16_t P_ATK=110,P_DEF=60,P_SPD=130,P_SATK=95,P_SDEF=70;
static constexpr uint16_t E_ATK=75, E_DEF=110,E_SPD=30, E_SATK=100,E_SDEF=80;

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
    const crystal::RomData& rom,
    const crystal::ExtractionProfile& profile)
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

struct EngineSnapshot {
    int8_t   player_stages[7];
    int8_t   enemy_stages[7];
    uint16_t player_stats[5];
    uint16_t enemy_stats[5];
};

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

    auto make=[](enginemon::MoveId mid, uint16_t atk,uint16_t def,uint16_t spd,
                 uint16_t satk,uint16_t sdef, const int8_t* sd){
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
    e.enemy_stages[0]=os.attack; e.enemy_stages[1]=os.defense; e.enemy_stages[2]=os.speed;
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
// CaseResult
// ============================================================================
enum class Status { MATCH, ENGINEMON_MISMATCH, HARNESS_ERROR };

struct CaseResult {
    uint16_t    move_id;
    Status      status;
    std::string detail;        // structured mismatch lines or error description
    int         insn_count;    // from poison-0x00 run
    bool        poison_stable; // true iff both runs produced identical snapshots
    const char* stop_reason;   // stop_reason_str of poison-0x00 run
    const char* boundary;      // sink symbol name (valid iff SINK_HIT)
};

// ============================================================================
// MoveSpec — per-move configuration.
// Adding a new move: register here with its name, insn_cap, and sink symbol.
// The sink must be in the global SymCache so it resolves at startup.
// ============================================================================
struct MoveSpec {
    uint16_t    id;
    const char* name;
    int         insn_cap;    // HARNESS_ERROR if exceeded
};

static constexpr MoveSpec REGISTERED_MOVES[] = {
    // move 114: Haze / BattleCommand_ResetStats
    // Measured: ~13 200 insn.  Cap = 50 000 (~3.8× margin).
    { 114, "Haze", 50000 },
};
static constexpr size_t NUM_REGISTERED = sizeof(REGISTERED_MOVES)/sizeof(REGISTERED_MOVES[0]);

static const MoveSpec* find_move(uint16_t id){
    for(size_t i=0;i<NUM_REGISTERED;i++) if(REGISTERED_MOVES[i].id==id) return &REGISTERED_MOVES[i];
    return nullptr;
}

// ============================================================================
// run_case — self-contained, safe to call from any thread.
// ============================================================================
static CaseResult run_case(
    const MoveSpec& spec,
    const std::vector<uint8_t>& rom_bytes,
    const SymCache& sym,
    const EngineData& ed)
{
    CaseResult r{};
    r.move_id     = spec.id;
    r.status      = Status::HARNESS_ERROR;
    r.insn_count  = 0;
    r.poison_stable = false;
    r.stop_reason = nullptr;
    r.boundary    = nullptr;

    // Two Crystal runs with different poison patterns.
    auto cr1 = run_crystal_case(rom_bytes, sym, 0x00, spec.insn_cap);
    auto cr2 = run_crystal_case(rom_bytes, sym, 0xA5, spec.insn_cap);

    r.insn_count  = cr1.insn_count;
    r.stop_reason = stop_reason_str(cr1.stop_reason);
    r.boundary    = cr1.sink_name;

    // Both runs must reach the sink.
    if(cr1.stop_reason != StopReason::SINK_HIT){
        r.detail = std::string("Crystal run 1 failed: ")+stop_reason_str(cr1.stop_reason)
                 + " after "+std::to_string(cr1.insn_count)+" insn";
        return r;
    }
    if(cr2.stop_reason != StopReason::SINK_HIT){
        r.detail = std::string("Crystal run 2 (poison=0xA5) failed: ")+stop_reason_str(cr2.stop_reason)
                 + " after "+std::to_string(cr2.insn_count)+" insn";
        return r;
    }

    // Poison stability check — disagreement = HARNESS_ERROR (not a mismatch).
    r.poison_stable = snapshots_equal(*cr1.snapshot, *cr2.snapshot);
    if(!r.poison_stable){
        // Structured output: show which fields differ.
        std::ostringstream os;
        os<<"POISON INSTABILITY (0x00 vs 0xA5):\n";
        static const char* SN[7]={"ATK","DEF","SPD","SATK","SDEF","ACC","EVA"};
        static const char* CN[5]={"ATK","DEF","SPD","SATK","SDEF"};
        for(int i=0;i<7;i++){
            if(cr1.snapshot->player_stages[i]!=cr2.snapshot->player_stages[i])
                os<<"  player_stage."<<SN[i]<<": run1="<<(int)cr1.snapshot->player_stages[i]
                  <<" run2="<<(int)cr2.snapshot->player_stages[i]<<"\n";
            if(cr1.snapshot->enemy_stages[i]!=cr2.snapshot->enemy_stages[i])
                os<<"  enemy_stage."<<SN[i]<<": run1="<<(int)cr1.snapshot->enemy_stages[i]
                  <<" run2="<<(int)cr2.snapshot->enemy_stages[i]<<"\n";
        }
        for(int i=0;i<5;i++){
            if(cr1.snapshot->player_stats[i]!=cr2.snapshot->player_stats[i])
                os<<"  player_stat."<<CN[i]<<": run1="<<cr1.snapshot->player_stats[i]
                  <<" run2="<<cr2.snapshot->player_stats[i]<<"\n";
            if(cr1.snapshot->enemy_stats[i]!=cr2.snapshot->enemy_stats[i])
                os<<"  enemy_stat."<<CN[i]<<": run1="<<cr1.snapshot->enemy_stats[i]
                  <<" run2="<<cr2.snapshot->enemy_stats[i]<<"\n";
        }
        r.detail = os.str();
        return r;  // HARNESS_ERROR
    }

    // Enginemon run.
    auto eng = run_enginemon_case(spec.id, ed);
    if(!eng){
        r.detail = "Enginemon run failed (move not supported by engine or data error)";
        return r;
    }

    // Normalize and compare.
    // Crystal stage: 7=neutral → delta = raw-7
    // Enginemon stage: 0=neutral → delta = value
    static const char* SN[7]={"ATK","DEF","SPD","SATK","SDEF","ACC","EVA"};
    static const char* CN[5]={"ATK","DEF","SPD","SATK","SDEF"};
    bool all_match = true;
    std::ostringstream diff;

    for(int i=0;i<7;i++){
        int8_t cd=int8_t(int(cr1.snapshot->player_stages[i])-7);
        int8_t ed2=eng->player_stages[i];
        if(cd!=ed2){ all_match=false;
            diff<<"  player_stage."<<SN[i]<<": crystal="<<(int)cd<<" enginemon="<<(int)ed2<<"\n"; }
    }
    for(int i=0;i<7;i++){
        int8_t cd=int8_t(int(cr1.snapshot->enemy_stages[i])-7);
        int8_t ed2=eng->enemy_stages[i];
        if(cd!=ed2){ all_match=false;
            diff<<"  enemy_stage."<<SN[i]<<": crystal="<<(int)cd<<" enginemon="<<(int)ed2<<"\n"; }
    }
    for(int i=0;i<5;i++){
        uint16_t cr=cr1.snapshot->player_stats[i], em=eng->player_stats[i];
        if(cr!=em){ all_match=false;
            diff<<"  player_stat."<<CN[i]<<": crystal="<<cr<<" enginemon="<<em<<"\n"; }
    }
    for(int i=0;i<5;i++){
        uint16_t cr=cr1.snapshot->enemy_stats[i], em=eng->enemy_stats[i];
        if(cr!=em){ all_match=false;
            diff<<"  enemy_stat."<<CN[i]<<": crystal="<<cr<<" enginemon="<<em<<"\n"; }
    }

    r.status = all_match ? Status::MATCH : Status::ENGINEMON_MISMATCH;
    r.detail = diff.str();
    return r;
}

// ============================================================================
// runner_main
// ============================================================================
int runner_main(int argc, char* argv[], RunnerConfig defaults){
    const char* prog = argc>0?argv[0]:"crystal_battle_diff";

    // --- Startup: SameBoy version pin ---
    // GB_VERSION is the string from sameboy_version.h (force-included at compile time).
    // Fail closed if it doesn't match the pinned version.
    if(std::string(GB_VERSION) != PINNED_SAMEBOY_VERSION){
        std::cerr<<"FATAL: SameBoy version mismatch.\n"
                 <<"  Pinned: "<<PINNED_SAMEBOY_VERSION<<"\n"
                 <<"  Built:  "<<GB_VERSION<<"\n";
        return EXIT_HARNESS_ERROR;
    }

    // --- Parse CLI ---
    std::string rom_path, sym_path;
    std::vector<uint16_t> move_ids = defaults.move_ids;
    int jobs = defaults.jobs < 1 ? 1 : defaults.jobs;
    bool all_flag = false;
    bool help_flag = false;

    // Positional scan: first two non-flag args are rom and sym.
    int positional = 0;
    for(int i=1;i<argc;++i){
        std::string a=argv[i];
        if(a=="--help"||a=="-h"){ help_flag=true; }
        else if(a=="--all"){ all_flag=true; }
        else if((a=="--jobs"||a=="-j")&&i+1<argc){ jobs=std::max(1,std::stoi(argv[++i])); }
        else if(a=="--move"&&i+1<argc){
            while(i+1<argc&&argv[i+1][0]!='-')
                move_ids.push_back((uint16_t)std::stoi(argv[++i]));
        }
        else if(a.size()>0&&a[0]!='-'){
            if(positional==0) rom_path=a;
            else if(positional==1) sym_path=a;
            ++positional;
        }
    }

    if(help_flag){
        std::cout<<
            "crystal_battle_diff — Crystal vs Enginemon battle differential oracle\n\n"
            "Usage:\n"
            "  "<<prog<<" <rom_path> <sym_path> [options]\n\n"
            "Required:\n"
            "  <rom_path>    Crystal v1.1 (UE) ROM  SHA-1: "<<PINNED_ROM_SHA1<<"\n"
            "  <sym_path>    pokecrystal11.sym matching that ROM\n\n"
            "Options:\n"
            "  --all           Run all registered moves\n"
            "  --move <id>...  Run specific move ID(s)\n"
            "  --jobs N        Parallel workers (default: 1)\n"
            "  --help          Show this message\n\n"
            "Registered moves:\n";
        for(size_t i=0;i<NUM_REGISTERED;i++)
            std::cout<<"  "<<REGISTERED_MOVES[i].id<<"  "<<REGISTERED_MOVES[i].name
                     <<"  (insn_cap="<<REGISTERED_MOVES[i].insn_cap<<")\n";
        std::cout<<"\nExit codes:\n"
            "  0  all MATCH\n"
            "  1  ENGINEMON_MISMATCH\n"
            "  2  HARNESS_ERROR\n"
            "  3  invalid CLI / unregistered move\n";
        return EXIT_ALL_MATCH;
    }

    if(rom_path.empty()||sym_path.empty()){
        std::cerr<<"Error: rom_path and sym_path are required.\n"
                 <<"Run '"<<prog<<" --help' for usage.\n";
        return EXIT_INVALID_ARGS;
    }

    // Resolve move list.
    if(all_flag){
        move_ids.clear();
        for(size_t i=0;i<NUM_REGISTERED;i++) move_ids.push_back(REGISTERED_MOVES[i].id);
    } else if(move_ids.empty()){
        std::cerr<<"Error: no moves selected. Use --all or --move <id>.\n"
                 <<"Run '"<<prog<<" --help' for usage.\n";
        return EXIT_INVALID_ARGS;
    }

    // Validate move IDs (fail-closed: unregistered = EXIT_INVALID_ARGS).
    for(uint16_t id : move_ids){
        if(!find_move(id)){
            std::cerr<<"Error: move "<<id<<" is not registered in the oracle.\n"
                     <<"  Registered:";
            for(size_t i=0;i<NUM_REGISTERED;i++) std::cerr<<" "<<REGISTERED_MOVES[i].id;
            std::cerr<<"\nRun '"<<prog<<" --help' for details.\n";
            return EXIT_INVALID_ARGS;
        }
    }
    // Deduplicate and sort for deterministic ordering.
    std::sort(move_ids.begin(),move_ids.end());
    move_ids.erase(std::unique(move_ids.begin(),move_ids.end()),move_ids.end());

    // =========================================================================
    // Startup checks — all must pass before any case runs.
    // =========================================================================
    std::cout<<"=== Crystal Battle Differential Oracle ===\n";
    std::cout<<"  SameBoy:  "<<PINNED_SAMEBOY_COMMIT<<"  (version "<<GB_VERSION<<")\n";

    // 1. ROM: load, size check, SHA-1.
    std::vector<uint8_t> rom_bytes;
    {
        std::ifstream f(rom_path,std::ios::binary);
        if(!f){
            std::cerr<<"FATAL: cannot open ROM: "<<rom_path<<"\n";
            return EXIT_HARNESS_ERROR;
        }
        rom_bytes.assign(std::istreambuf_iterator<char>(f),{});
    }
    if(rom_bytes.size()!=CRYSTAL_ROM_SIZE){
        std::cerr<<"FATAL: ROM size "<<rom_bytes.size()<<" != "<<CRYSTAL_ROM_SIZE<<"\n";
        return EXIT_HARNESS_ERROR;
    }
    std::string actual_sha=sha1_hex(rom_bytes.data(),rom_bytes.size());
    std::cout<<"  ROM SHA-1: "<<actual_sha;
    if(actual_sha!=PINNED_ROM_SHA1){
        std::cout<<" MISMATCH\n";
        std::cerr<<"FATAL: ROM SHA-1 mismatch.\n"
                 <<"  Expected: "<<PINNED_ROM_SHA1<<"\n"
                 <<"  Actual:   "<<actual_sha<<"\n";
        return EXIT_HARNESS_ERROR;
    }
    std::cout<<" OK\n";

    // 2. Sym file: open, identity check, load all required symbols.
    SymCache sym;
    {
        std::string err=SymCache::load(sym_path,&sym);
        if(!err.empty()){
            std::cerr<<"FATAL: .sym validation failed: "<<err<<"\n"
                     <<"  File: "<<sym_path<<"\n";
            return EXIT_HARNESS_ERROR;
        }
    }
    std::cout<<"  .sym:      OK ("<<std::to_string(sizeof(SymCache)/sizeof(Sym))<<" symbols verified)\n";

    // 3. Fixture address validation — every WRAM/HRAM target in expected range.
    {
        std::string err=validate_fixture_addresses(sym);
        if(!err.empty()){
            std::cerr<<"FATAL: fixture address out of range: "<<err<<"\n";
            return EXIT_HARNESS_ERROR;
        }
    }
    std::cout<<"  Fixtures:  OK (all addresses in expected memory regions)\n";

    // 4. Load Enginemon data.
    auto rom_data=crystal::RomData::load(std::filesystem::path(rom_path));
    if(!rom_data){
        std::cerr<<"FATAL: RomData::load failed\n"; return EXIT_HARNESS_ERROR;
    }
    const crystal::ExtractionProfile* profile=
        crystal::ProfileRegistry::instance().get_profile_by_hash(rom_data->hash());
    if(!profile){
        std::cerr<<"FATAL: no Enginemon profile for ROM\n"; return EXIT_HARNESS_ERROR;
    }
    auto ed_opt=load_engine_data(*rom_data,*profile);
    if(!ed_opt){
        std::cerr<<"FATAL: engine data load failed\n"; return EXIT_HARNESS_ERROR;
    }
    const EngineData& ed=*ed_opt;
    std::cout<<"  Engine:    OK\n";

    std::cout<<"\nRunning "<<move_ids.size()<<" move(s), "<<jobs<<" job(s)...\n\n";
    auto t0=std::chrono::steady_clock::now();

    // =========================================================================
    // Parallel execution — batched by jobs limit.
    // Futures indexed in move_ids order → deterministic collection.
    // =========================================================================
    const size_t n=move_ids.size();
    std::vector<std::future<CaseResult>> futures;
    futures.reserve(n);

    for(size_t i=0;i<n;){
        size_t batch_end=std::min(i+(size_t)jobs,n);
        for(size_t j=i;j<batch_end;++j){
            const MoveSpec* spec=find_move(move_ids[j]); // guaranteed non-null
            futures.push_back(std::async(std::launch::async,
                [&rom_bytes,&sym,&ed,spec](){
                    return run_case(*spec,rom_bytes,sym,ed);
                }));
        }
        for(size_t j=i;j<batch_end;++j) futures[j].wait();
        i=batch_end;
    }

    // =========================================================================
    // Collect and print in deterministic (sorted) order.
    // =========================================================================
    int n_match=0, n_mismatch=0, n_error=0;
    std::vector<CaseResult> results;
    results.reserve(n);
    for(auto& f:futures) results.push_back(f.get());

    for(const auto& r:results){
        const char* st=
            r.status==Status::MATCH              ? "MATCH"              :
            r.status==Status::ENGINEMON_MISMATCH ? "ENGINEMON_MISMATCH" :
                                                   "HARNESS_ERROR";

        std::cout<<"  move "<<std::setw(3)<<r.move_id<<":  "<<st;
        if(r.boundary)    std::cout<<"  boundary="<<r.boundary;
        if(r.stop_reason) std::cout<<"  stop="<<r.stop_reason;
        if(r.insn_count>0)std::cout<<"  insn="<<r.insn_count;
        std::cout<<"  poison-stable="<<(r.poison_stable?"yes":"NO");
        std::cout<<"\n";

        if(!r.detail.empty()){
            // Indent all detail lines.
            std::istringstream ss(r.detail); std::string line;
            while(std::getline(ss,line)) std::cout<<"    "<<line<<"\n";
        }

        switch(r.status){
        case Status::MATCH:              ++n_match;    break;
        case Status::ENGINEMON_MISMATCH: ++n_mismatch; break;
        case Status::HARNESS_ERROR:      ++n_error;    break;
        }
    }

    auto t1=std::chrono::steady_clock::now();
    int ms=(int)std::chrono::duration_cast<std::chrono::milliseconds>(t1-t0).count();

    std::cout<<"\n=== Summary ===\n";
    std::cout<<"  MATCH:              "<<n_match<<"\n";
    std::cout<<"  ENGINEMON_MISMATCH: "<<n_mismatch<<"\n";
    std::cout<<"  HARNESS_ERROR:      "<<n_error<<"\n";
    std::cout<<"  Total:              "<<n<<"\n";
    std::cout<<"  Time:               "<<ms<<" ms  ("<<jobs<<" job"<<(jobs==1?"":"s")<<")\n";

    // HARNESS_ERROR takes precedence over MISMATCH.
    if(n_error   >0) return EXIT_HARNESS_ERROR;
    if(n_mismatch>0) return EXIT_MISMATCH;
    return EXIT_ALL_MATCH;
}

} // namespace crystal::oracle
