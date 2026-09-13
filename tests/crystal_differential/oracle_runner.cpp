// tests/crystal_differential/oracle_runner.cpp
//
// Crystal battle differential oracle -- hardened parallel runner with RNG interception.
//
// --- ROM IMMUTABILITY -------------------------------------------------------
//   rom_bytes loaded once, SHA-checked, passed read-only to every worker.
//   GB_load_rom_from_buffer receives exact verified bytes unchanged.
//   No pointer to gb->rom is taken. No ROM bytes are modified.
//
// --- COLD-CALL (no ROM patches) ---------------------------------------------
//   regs->pc  = entry symbol        (bank already mapped)
//   regs->sp  = INITIAL_SP-2        (sink addr on stack)
//   hROMBank  = entry bank          (RST $08 BankSwitch restore)
//   0xFF50    = 1                   (boot_rom_finished)
//   0x2000    = entry bank          (MBC register)
//
//   INITIAL_SP = 0xFFFF. Crystal HRAM vars occupy 0xFF80-0xFFEB (hBattleTurn=0xFFE4,
//   hROMBank=0xFF9D, etc.). Starting the SM83 stack at 0xFFFF means the first push
//   lands at 0xFFFD-0xFFFE (unused HRAM per Crystal's hram.asm layout). Up to 12
//   nested CALLs reach 0xFFE5 -- safely above all Crystal HRAM variables.
//   The old value 0xFFF0 allowed only 3 nested calls before the stack descended into
//   hBattleTurn (0xFFE4), corrupting it and causing GetBattleVar to use enemy-turn
//   addressing for all subsequent calls.
//
// --- RNG INTERCEPTION -------------------------------------------------------
//   Crystal BattleRandom (00:2F9F) returns its result via a temporary store
//   at wPredefHL+1 (0xCFB6). The stub flow is:
//     2FA8: LD (0xCFB6), A    -- write result from _BattleRandom
//     2FAB: POP AF
//     2FAC: RST $10            -- restore ROM bank (writes 0xFF9D, 0x2000)
//     2FAD: LD A,(0xCFB6)     -- READ BACK the result  ← INTERCEPTION POINT
//     2FB0: RET
//
//   Interception: execution callback fires when PC == 0x2FAD (the exact
//   instruction that reads back the BattleRandom result). At that point:
//     GB_write_memory(gb, 0xCFB6, tape[idx++])
//   Then LD A,(0xCFB6) executes and naturally loads our tape byte into A.
//
//   This is the ONLY interception point for BattleRandom. Other code that
//   reads 0xCFB6 (Predef at 0x2DA1, 0x2DB5) is unaffected because they
//   fire at different PCs and we only intercept at PC == 0x2FAD.
//
//   Trace format: {index, byte, "BattleRandom", PC=0x2FAD}
//   Tape exhaustion = HARNESS_ERROR.
//
// --- PRESENT FULL PATH ------------------------------------------------------
//   Entry: BattleCommand_Present (0D:7874) -- entered directly, not via DoMove.
//   BattleCommand_Present first calls BattleCommand_Stab to compute wTypeMatchup,
//   then checks:
//     [wTypeMatchup == 0]   → jp AnimateFailedMove  (type immune)
//     [wAttackMissed != 0]  → jp AnimateFailedMove  (missed / failed)
//   Then calls BattleRandom once for power/heal selection:
//     byte < 0x66  → power 40 (wBattleAnimParam=0), call AnimateCurrentMoveEitherSide, ret
//     byte < 0xB4  → power 80 (wBattleAnimParam=1), call AnimateCurrentMoveEitherSide, ret
//     byte < 0xCC  → power 120 (wBattleAnimParam=2), call AnimateCurrentMoveEitherSide, ret
//     byte == 0xFF/table end → heal (wBattleAnimParam=3), call AnimateCurrentMove,
//                               ... hp restore logic ... jp EndMoveEffect
//
//   Since we enter at BattleCommand_Present directly (not DoMove), CheckHit and
//   Critical are NOT run -- those are earlier commands in the DoMove script.
//   DamageVariation IS called after Present returns to the DoMove script (damage
//   path only); it runs via the script engine, not directly from Present.
//   For the damage path we stop at AnimateCurrentMoveEitherSide (called inside
//   Present, before ret). The tape only needs the Present power-selection byte
//   plus the DamageVariation byte.
//
//   BattleRandom bytes consumed per path (entering at BattleCommand_Present):
//     damage: 1 (Present power) -- AnimateCurrentMoveEitherSide is the sink
//     heal:   1 (Present power = 0xFF/table-end sentinel)
//     miss:   0 (wAttackMissed=1 set in fixture, Present branches before BattleRandom)
//
//   NOTE: DamageVariation (the next script command after Present ret) also
//   consumes 1 byte, but it runs AFTER AnimateCurrentMoveEitherSide is called
//   (our sink fires before DamageVariation executes). So DamageVariation byte
//   is NOT needed in the tape when sinking at AnimateCurrentMoveEitherSide.
//
//   Sinks:
//     AnimateCurrentMoveEitherSide (0D:7DE9) -- damage path
//     AnimateCurrentMove           (0D:7E01) -- heal path (first call in heal block,
//                                               before HP change logic runs; captured
//                                               here to avoid animation engine loops
//                                               on uninitialized graphics state)
//     AnimateFailedMove            (0D:7E77) -- miss / immune path
//
// --- WALL-CLOCK TIMEOUT -----------------------------------------------------
//   Each future has WALL_CLOCK_TIMEOUT_S seconds. On expiry an atomic stop
//   flag is set, the exec_cb forces ctx->triggered = true on the NEXT tick,
//   and the GB_run loop exits. f.get() then returns promptly. No blocking.
//
// --- ERROR FORMAT -----------------------------------------------------------
//   ERROR <n> @ <SUBSYSTEM> [<CASE>] :: <CLASS> :: <reason>
//   Deterministic 1-based numbering independent of --jobs.
//   Summary repeats all error lines.

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

// SameBoy identity constants passed via CMake compile definitions (-D flags).
// If not defined by the build system, default to the pinned values.
#ifndef GB_VERSION
#define GB_VERSION "1.0.3"
#endif
#ifndef GB_COMMIT
#define GB_COMMIT "213a12ce93d66b105a113debd9396306066a7cfc"
#endif
#ifndef GB_DIRTY
#define GB_DIRTY ""
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
static constexpr const char* PINNED_ROM_SHA1      = "F2F52230B536214EF7C9924F483392993E226CFB";
static constexpr const char* PINNED_SYM_SHA1_CONST = "AFF514A7CE7858DED0851BA663C3839C93C7BCE7";
static constexpr uint32_t    CRYSTAL_ROM_SIZE      = 2097152u;
static constexpr const char* PINNED_SAMEBOY_VERSION = "1.0.3";
static constexpr const char* PINNED_SAMEBOY_COMMIT  = "213a12ce93d66b105a113debd9396306066a7cfc";

// Per-case instruction cap. Exceeding = HARNESS_ERROR before wall-clock fires.
// Wall-clock timeout (seconds): after expiry the atomic stop flag forces the
// exec_cb to terminate the GB_run loop on the next tick, making f.get() return
// promptly with no indefinite blocking.
static constexpr int WALL_CLOCK_TIMEOUT_S = 30;

// Maximum RNG bytes any single case may consume. Tape exhaustion = HARNESS_ERROR.
static constexpr size_t RNG_TAPE_MAX_BYTES = 256;

// ============================================================================
// SHA-1 (self-contained)
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
            c+=n*8; for(size_t i=0;i<n;i++){b[bl++]=p[i];if(bl==64){blk(b);bl=0;}}
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
    auto h=s.fin(); char hex[41]{};
    for(int i=0;i<20;i++) sprintf(hex+i*2,"%02X",h[i]);
    return std::string(hex);
}
static std::string sha1_of_file(const std::string& path){
    std::ifstream f(path,std::ios::binary); if(!f) return "";
    std::vector<uint8_t> buf(std::istreambuf_iterator<char>(f),{});
    if(buf.empty()) return "";
    return sha1_hex(buf.data(),buf.size());
}

// ============================================================================
// Symbol types and lookup
// ============================================================================
struct Sym { uint8_t bank; uint16_t addr; };
static bool sym_get(const std::string& path, const std::string& name, Sym* out){
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
// SymCache -- loaded once, identity-validated, shared read-only
// ============================================================================
struct SymAnchor { const char* name; uint8_t bank; uint16_t addr; };
static constexpr SymAnchor SYM_ANCHORS[] = {
    { "BattleCommand_ResetStats",    0x0D, 0x710E },
    { "AnimateCurrentMove",          0x0D, 0x7E01 },
    { "wPlayerStatLevels",           0x00, 0xC6CC },
    { "wEnemyStatLevels",            0x00, 0xC6D4 },
    { "wBattleMonAttack",            0x00, 0xC640 },
    { "wEnemyMonAttack",             0x01, 0xD21A },
};

struct SymCache {
    // Haze
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
    // RNG (shared) -- BattleRandom result is read at PC=0x2FAD (fixed address in bank 0)
    Sym BattleRandom;            // 00:2F9F -- entry of the stub; 0x2FAD is the result-read PC
    // Present -- entry is BattleCommand_Present directly (not DoMove)
    Sym BattleCommand_Present;   // 0D:7874 -- direct entry for all Present cases
    Sym AnimateCurrentMoveEitherSide; // 0D:7DE9 -- damage path sink (called inside Present, before ret)
    Sym EndMoveEffect;           // 0D:52A3 -- heal path sink (jp at end of heal block)
    Sym AnimateFailedMove;       // 0D:7E77 -- miss/immune path sink
    Sym wTypeMatchup;            // 01:D265
    Sym wAttackMissed;           // 00:C667
    Sym wBattleAnimParam;        // 00:C689
    Sym wCurDamage;              // 01:D256
    Sym wBattleMonHP;            // 00:C63C
    Sym wEnemyMonHP;             // 01:D216
    Sym wBattleMonMaxHP;         // 00:C63E
    Sym wEnemyMonMaxHP;          // 01:D218
    Sym wBattleMonLevel;         // 00:C639
    Sym wEnemyMonLevel;          // 01:D213
    Sym wBattleMonType1;         // 00:C64A
    Sym wBattleMonType2;         // 00:C64B
    Sym wEnemyMonType1;          // 01:D224
    Sym wEnemyMonType2;          // 01:D225
    Sym wCurPlayerMove;          // 00:C6E3
    Sym wCriticalHit;            // 00:C666
    // DoMove extra fields
    Sym wBattleMode;             // 01:D22D
    Sym wBattleMonMoves;         // 00:C62E
    Sym wBattleMonPP;            // 00:C634
    Sym wCurMoveNum;             // 01:D0D5
    Sym wTurnEnded;              // 00:C6B4
    Sym wPlayerSubStatus3;       // 00:C66A
    Sym wEnemySubStatus3;        // 00:C66F
    Sym wCurBattleMon;           // 01:D0D4
    Sym wBattleMonSpecies;       // 00:C62C
    Sym wEnemyMonSpecies;        // 01:D206
    Sym wBattleMonItem;          // 00:C62D
    Sym wEnemyMonItem;           // 01:D207
    Sym wPlayerMoveStruct;       // 00:C60F
    Sym wPlayerTurnsTaken;       // 00:C6DD
    Sym wEnemyTurnsTaken;        // 00:C6DC
    Sym wBattlePlayerAction;     // 01:D0EC
    Sym wBattleAction;           // 01:D430
    // Full-script (DoTurn entry) extra symbols -- only fields not already above
    Sym DoPlayerTurn;            // 0D:4000 (kept for reference; not used as full-script entry)
    Sym DoTurn;                  // 0D:401D (kept for reference)
    Sym DoMove;                  // 0D:402C (full-script entrypoint: dispatches full effect script)
    Sym wPartyMon1PP;            // 01:DCF6
    Sym wPartyCount;             // 01:DCD7
    Sym wWildMonPP;              // 00:C739
    Sym wWildMonMoves;           // 00:C735
    Sym wPlayerSubStatus1;       // 00:C668
    Sym wPlayerSubStatus4;       // 00:C66B
    Sym wPlayerSubStatus5;       // 00:C66C
    Sym wEnemySubStatus4;        // 00:C670 (bank 0)
    Sym wEnemySubStatus5;        // 00:C671 (bank 0)
    Sym wPlayerDisableCount;     // 00:C675
    Sym wDisabledMove;           // 00:C6F5
    Sym wPlayerCharging;         // 00:C732
    Sym wEnemyCharging;          // 00:C733
    Sym wAlreadyDisobeyed;       // 00:C6F4
    Sym wEnemyMoveStruct;        // 00:C608
    Sym wEnemyMonDefense;        // 01:D21C
    Sym wEnemyMonSpclDef;        // 01:D222
    Sym wEnemyMonSpclAtk;        // 01:D220
    Sym wPlayerAttack;           // 00:C6B6
    Sym wPlayerSpAtk;            // 00:C6BC
    Sym wEnemyDefense;           // 00:C6C3
    Sym wEnemySpDef;             // 00:C6C9
    Sym wBattleWeather;          // 00:C70A
    Sym wPlayerScreens;          // 00:C6FF
    Sym wEnemyScreens;           // 00:C700

    static std::string load(const std::string& sym_path, SymCache* out,
                             std::string* sym_sha_out = nullptr)
    {
        { std::ifstream f(sym_path); if(!f) return "cannot open: "+sym_path; }
        std::string actual_sha = sha1_of_file(sym_path);
        if(sym_sha_out) *sym_sha_out = actual_sha;
        if(std::string(PINNED_SYM_SHA1_CONST).size()>0 &&
           actual_sha != PINNED_SYM_SHA1_CONST)
            return ".sym SHA mismatch: expected "+std::string(PINNED_SYM_SHA1_CONST)+" got "+actual_sha;

        for(const auto& e : SYM_ANCHORS){
            Sym got{};
            if(!sym_get(sym_path,e.name,&got))
                return std::string("anchor symbol missing: ")+e.name;
            if(got.bank!=e.bank||got.addr!=e.addr){
                char buf[128];
                snprintf(buf,sizeof(buf),"anchor mismatch: %s expected %02X:%04X got %02X:%04X",
                    e.name,e.bank,e.addr,got.bank,got.addr);
                return std::string(buf);
            }
        }

        struct { const char* name; Sym* dst; } required[] = {
            {"BattleCommand_ResetStats",       &out->BattleCommand_ResetStats},
            {"wPlayerStatLevels",              &out->wPlayerStatLevels},
            {"wEnemyStatLevels",               &out->wEnemyStatLevels},
            {"wPlayerStats",                   &out->wPlayerStats},
            {"wEnemyStats",                    &out->wEnemyStats},
            {"wBattleMonAttack",               &out->wBattleMonAttack},
            {"wEnemyMonAttack",                &out->wEnemyMonAttack},
            {"wBattleMonStatus",               &out->wBattleMonStatus},
            {"wEnemyMonStatus",                &out->wEnemyMonStatus},
            {"hBattleTurn",                    &out->hBattleTurn},
            {"hROMBank",                       &out->hROMBank},
            {"wLinkMode",                      &out->wLinkMode},
            {"wInBattleTowerBattle",           &out->wInBattleTowerBattle},
            {"wJohtoBadges",                   &out->wJohtoBadges},
            {"AnimateCurrentMove",             &out->AnimateCurrentMove},
            {"BattleRandom",                   &out->BattleRandom},
            {"DoMove",                         &out->DoMove},
            {"BattleCommand_Present",          &out->BattleCommand_Present},
            {"AnimateCurrentMoveEitherSide",   &out->AnimateCurrentMoveEitherSide},
            {"EndMoveEffect",                  &out->EndMoveEffect},
            {"AnimateFailedMove",              &out->AnimateFailedMove},
            {"wTypeMatchup",                   &out->wTypeMatchup},
            {"wAttackMissed",                  &out->wAttackMissed},
            {"wBattleAnimParam",               &out->wBattleAnimParam},
            {"wCurDamage",                     &out->wCurDamage},
            {"wBattleMonHP",                   &out->wBattleMonHP},
            {"wEnemyMonHP",                    &out->wEnemyMonHP},
            {"wBattleMonMaxHP",                &out->wBattleMonMaxHP},
            {"wEnemyMonMaxHP",                 &out->wEnemyMonMaxHP},
            {"wBattleMonLevel",                &out->wBattleMonLevel},
            {"wEnemyMonLevel",                 &out->wEnemyMonLevel},
            {"wBattleMonType1",                &out->wBattleMonType1},
            {"wBattleMonType2",                &out->wBattleMonType2},
            {"wEnemyMonType1",                 &out->wEnemyMonType1},
            {"wEnemyMonType2",                 &out->wEnemyMonType2},
            {"wCurPlayerMove",                 &out->wCurPlayerMove},
            {"wCriticalHit",                   &out->wCriticalHit},
            {"wBattleMode",                    &out->wBattleMode},
            {"wBattleMonMoves",                &out->wBattleMonMoves},
            {"wBattleMonPP",                   &out->wBattleMonPP},
            {"wCurMoveNum",                    &out->wCurMoveNum},
            {"wTurnEnded",                     &out->wTurnEnded},
            {"wPlayerSubStatus3",              &out->wPlayerSubStatus3},
            {"wEnemySubStatus3",               &out->wEnemySubStatus3},
            {"wCurBattleMon",                  &out->wCurBattleMon},
            {"wBattleMonSpecies",              &out->wBattleMonSpecies},
            {"wEnemyMonSpecies",               &out->wEnemyMonSpecies},
            {"wBattleMonItem",                 &out->wBattleMonItem},
            {"wEnemyMonItem",                  &out->wEnemyMonItem},
            {"wPlayerMoveStruct",              &out->wPlayerMoveStruct},
            {"wPlayerTurnsTaken",              &out->wPlayerTurnsTaken},
            {"wEnemyTurnsTaken",               &out->wEnemyTurnsTaken},
            {"wBattlePlayerAction",            &out->wBattlePlayerAction},
            {"wBattleAction",                  &out->wBattleAction},
            // Full-script extras
            {"DoPlayerTurn",                   &out->DoPlayerTurn},
            {"DoTurn",                         &out->DoTurn},
            {"wPartyMon1PP",                   &out->wPartyMon1PP},
            {"wPartyCount",                    &out->wPartyCount},
            {"wBattleMode",                    &out->wBattleMode},
            {"wBattleMonMoves",                &out->wBattleMonMoves},
            {"wBattleMonPP",                   &out->wBattleMonPP},
            {"wCurMoveNum",                    &out->wCurMoveNum},
            {"wCurBattleMon",                  &out->wCurBattleMon},
            {"wWildMonPP",                     &out->wWildMonPP},
            {"wWildMonMoves",                  &out->wWildMonMoves},
            {"wPlayerSubStatus1",              &out->wPlayerSubStatus1},
            {"wPlayerSubStatus4",              &out->wPlayerSubStatus4},
            {"wPlayerSubStatus5",              &out->wPlayerSubStatus5},
            {"wEnemySubStatus4",               &out->wEnemySubStatus4},
            {"wEnemySubStatus5",               &out->wEnemySubStatus5},
            {"wPlayerDisableCount",            &out->wPlayerDisableCount},
            {"wDisabledMove",                  &out->wDisabledMove},
            {"wPlayerCharging",                &out->wPlayerCharging},
            {"wEnemyCharging",                 &out->wEnemyCharging},
            {"wAlreadyDisobeyed",              &out->wAlreadyDisobeyed},
            {"wEnemyMoveStruct",               &out->wEnemyMoveStruct},
            {"wEnemyMonDefense",               &out->wEnemyMonDefense},
            {"wEnemyMonSpclDef",               &out->wEnemyMonSpclDef},
            {"wEnemyMonSpclAtk",               &out->wEnemyMonSpclAtk},
            {"wPlayerAttack",                  &out->wPlayerAttack},
            {"wPlayerSpAtk",                   &out->wPlayerSpAtk},
            {"wEnemyDefense",                  &out->wEnemyDefense},
            {"wEnemySpDef",                    &out->wEnemySpDef},
            {"wBattleWeather",                 &out->wBattleWeather},
            {"wPlayerScreens",                 &out->wPlayerScreens},
            {"wEnemyScreens",                  &out->wEnemyScreens},
        };
        for(const auto& r : required)
            if(!sym_get(sym_path,r.name,r.dst))
                return std::string("required symbol missing: ")+r.name;
        return {};
    }
};

// ============================================================================
// Fixture address validation
// ============================================================================
static std::string validate_wram_addr(const char* n, uint16_t a, uint16_t lo, uint16_t hi){
    if(a<lo||a>hi){ char buf[128]; snprintf(buf,sizeof(buf),"%s addr 0x%04X outside [0x%04X,0x%04X]",n,a,lo,hi); return buf; }
    return {};
}
static std::string validate_hram_addr(const char* n, uint16_t a){
    if(a<0xFF80||a>0xFFFE){ char buf[64]; snprintf(buf,sizeof(buf),"%s addr 0x%04X outside HRAM",n,a); return buf; }
    return {};
}
static std::string validate_fixture_addresses(const SymCache& sym){
    std::string e;
    if((e=validate_wram_addr("wPlayerStatLevels",  sym.wPlayerStatLevels.addr,  0xC000,0xCFFF)).size()) return e;
    if((e=validate_wram_addr("wEnemyStatLevels",   sym.wEnemyStatLevels.addr,   0xC000,0xCFFF)).size()) return e;
    if((e=validate_wram_addr("wPlayerStats",       sym.wPlayerStats.addr,       0xC000,0xCFFF)).size()) return e;
    if((e=validate_wram_addr("wEnemyStats",        sym.wEnemyStats.addr,        0xC000,0xCFFF)).size()) return e;
    if((e=validate_wram_addr("wBattleMonAttack",   sym.wBattleMonAttack.addr,   0xC000,0xCFFF)).size()) return e;
    if((e=validate_wram_addr("wBattleMonStatus",   sym.wBattleMonStatus.addr,   0xC000,0xCFFF)).size()) return e;
    if((e=validate_wram_addr("wLinkMode",          sym.wLinkMode.addr,          0xC000,0xCFFF)).size()) return e;
    if((e=validate_wram_addr("wEnemyMonAttack",    sym.wEnemyMonAttack.addr,    0xD000,0xDFFF)).size()) return e;
    if((e=validate_wram_addr("wEnemyMonStatus",    sym.wEnemyMonStatus.addr,    0xD000,0xDFFF)).size()) return e;
    if((e=validate_wram_addr("wInBattleTowerBattle",sym.wInBattleTowerBattle.addr,0xC000,0xDFFF)).size()) return e;
    if((e=validate_wram_addr("wJohtoBadges",       sym.wJohtoBadges.addr,       0xD000,0xDFFF)).size()) return e;
    if((e=validate_hram_addr("hBattleTurn",        sym.hBattleTurn.addr)).size()) return e;
    if((e=validate_hram_addr("hROMBank",           sym.hROMBank.addr)).size()) return e;
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
// RNG tape and trace
//
// Crystal BattleRandom return contract:
//   Returns A = one byte (0x00-0xFF). For non-link battles:
//   A = result of hRandomSub + hRandomAdd (via the hardware RNG).
//   The stub saves A to wPredefHL+1 (0xCFB6), restores ROM bank, re-reads A.
//
// Interception at PC == 0x2FAD (the LD A,(0xCFB6) inside BattleRandom):
//   The execution callback fires at PC=0x2FAD BEFORE the instruction executes.
//   We write tape[idx++] to 0xCFB6 via GB_write_memory. The instruction
//   LD A,(0xCFB6) then executes naturally and loads our tape byte into A.
//
//   This is the ONLY place in bank 0 where BattleRandom reads back its result.
//   Other reads of 0xCFB6 (Predef at 0x2DA1, 0x2DB5 for banked calls) fire
//   at different PCs and are not intercepted.
// ============================================================================
static constexpr uint16_t BATTLE_RANDOM_RESULT_READ_PC = 0x2FAD;

struct RngEntry {
    size_t   byte_index;    // 0-based position in tape
    uint8_t  tape_value;    // value we injected
    uint8_t  crystal_value; // what Crystal had in 0xCFB6 before injection
    uint16_t intercept_pc;  // always 0x2FAD
    const char* rng_symbol; // "BattleRandom"
};

struct RngCtx {
    const uint8_t*      tape;
    size_t              tape_len;
    size_t              tape_idx;
    bool                exhausted;
    std::vector<RngEntry> trace;
};

// The RNG state is now shared between exec_cb and the GB_safe_read_memory call.
// We handle it in exec_cb at the intercept PC.
// ============================================================================
// Execution callback context
// Handles: sink detection, wall-clock stop, and RNG interception at 0x2FAD.
// ============================================================================
struct ExecCtx {
    // Presentation sinks
    uint16_t    sink_pcs[4];
    const char* sink_names[4];
    size_t      num_sinks;
    bool        triggered;
    const char* triggered_sink;
    int         insn_count;
    // Wall-clock preemption
    std::atomic<bool>* stop_flag;
    // RNG interception — pointer to case's RngCtx (null if no RNG for this case)
    RngCtx*     rng_ctx;
};

static void exec_cb(GB_gameboy_t* gb, uint16_t pc, uint8_t){
    auto* ctx = static_cast<ExecCtx*>(GB_get_user_data(gb));
    if(!ctx || ctx->triggered) return;
    ++ctx->insn_count;

    // Wall-clock preemption
    if(ctx->stop_flag && ctx->stop_flag->load(std::memory_order_relaxed)){
        ctx->triggered = true;
        ctx->triggered_sink = "__TIMEOUT__";
        return;
    }

    // RNG interception: PC == 0x2FAD means we're about to execute LD A,(0xCFB6)
    // inside BattleRandom. Write our tape byte to 0xCFB6 so the instruction
    // naturally loads it into A.
    // Also: fire a diagnostic on FIRST RNG consumption to confirm the script path.
    if(ctx->rng_ctx && pc == BATTLE_RANDOM_RESULT_READ_PC){
        RngCtx* rng = ctx->rng_ctx;
        if(rng->tape_idx >= rng->tape_len){
            rng->exhausted = true;
            // Don't stop here -- let the case exhaust and we report it after
        } else {
            uint8_t crystal_val = GB_safe_read_memory(gb, 0xCFB6); // side-effect-free read
            uint8_t tape_val    = rng->tape[rng->tape_idx];
            GB_write_memory(gb, 0xCFB6, tape_val);
            rng->trace.push_back({rng->tape_idx, tape_val, crystal_val,
                                   BATTLE_RANDOM_RESULT_READ_PC, "BattleRandom"});
            ++rng->tape_idx;
        }
    }

    // BattleCommand_UsedMoveText skip (0D:4541)
    //
    // DoMove dispatch loop (bank 0D):
    //   ... (ReadMoveEffectCommand at 0x4058) ...
    //   call .DoMoveEffectCommand      ; at 0D:407E -- pushes return addr 0D:4081
    //   jr   .ReadMoveEffectCommand    ; at 0D:4081 (2 bytes)
    //   .DoMoveEffectCommand: jp hl    ; at 0D:4083 (1 byte) -- dispatches via JP (HL)
    //
    // Intercept strategy: fire at DoMoveEffectCommand (0x4083 = JP HL).
    // At this point, HL contains the command handler address.
    // When HL == 0x4541 (BattleCommand_UsedMoveText), we skip the handler by:
    //   1. Setting HL = 0x4081 (so JP HL lands on the JR .ReadMoveEffectCommand)
    //   2. SP += 2 (discard the return addr 0x4081 pushed by `call .DoMoveEffectCommand`)
    //
    // SameBoy exec_cb fires with gb->pc = 0x4084 (already incremented past JP HL).
    // JP HL executes and uses the modified HL = 0x4081 as its branch target.
    // The JR at 0x4081 then loops to 0x4058 normally with a balanced stack.
    //
    // UsedMoveText prints "<Name> used PRESENT!" -- no gameplay state writes.
    static constexpr uint16_t DOMOVE_EFFECT_DISPATCH_PC  = 0x4083;  // DoMove.DoMoveEffectCommand (0D)
    static constexpr uint16_t USED_MOVE_TEXT_HANDLER_PC  = 0x4541;  // BattleCommand_UsedMoveText (0D)
    static constexpr uint16_t DOMOVE_DISPATCHER_CONT_PC  = 0x4081;  // jr .ReadMoveEffectCommand (0D)
    // Presentation-only rendering skips.
    // Each entry: bank-0 CALL-entered function with no gameplay state writes, RET convention.
    // Verified individually against pokecrystal source.
    //
    //   DelayFrame (0x045A): CALL-entered. Writes 1 to wVBlankOccurred(0xCFB3), HALTs,
    //     waits for VBlank ISR to clear it. No battle state written. Returns via RET.
    //
    //   DelayFrames (0x0468): CALL-entered. Calls DelayFrame in a loop (count in C).
    //     No battle state written. Returns via RET.
    //
    //   BattleTextbox (0x3AC3): CALL-entered. Renders text tiles into VRAM.
    //     No battle state written. Returns via RET.
    //
    //   StdBattleTextbox (0x3AD5): CALL-entered. Sets up HL then tail-calls BattleTextbox.
    //     No battle state written. Returns via RET (via BattleTextbox's RET).
    //
    //   RefreshBattleHuds (0x39C9): CALL-entered. Calls WaitBGMap then updates HUD tiles.
    //     No battle state written. Returns via RET.
    //
    //   AnimateHPBar (03:46E0): CALL-entered, bank-guarded (only when hROMBank==03).
    //     Animates HP bar graphics. No battle state written. Returns via RET.
    //     Guard prevents aliasing with bank-0D code at the same in-bank address.
    static constexpr uint16_t DELAY_FRAME_PC         = 0x045A;
    static constexpr uint16_t DELAY_FRAMES_PC        = 0x0468;
    static constexpr uint16_t BATTLE_TEXTBOX_PC      = 0x3AC3;
    static constexpr uint16_t STD_BATTLE_TEXTBOX_PC  = 0x3AD5;
    static constexpr uint16_t REFRESH_BATTLE_HUDS_PC = 0x39C9;
    auto do_ret_skip = [&](){
        GB_registers_t* r = GB_get_registers(gb);
        if(r){
            // Read return address directly from SameBoy's internal HRAM buffer
            // to avoid any potential issue with GB_safe_read_memory for stack addresses.
            size_t hram_sz = 0; uint16_t hram_bank = 0;
            uint8_t* hram = static_cast<uint8_t*>(
                GB_get_direct_access(gb, GB_DIRECT_ACCESS_HRAM, &hram_sz, &hram_bank));
            uint16_t lo_idx = (r->sp     - 0xFF80u) & 0x7Fu;
            uint16_t hi_idx = (r->sp + 1 - 0xFF80u) & 0x7Fu;
            uint8_t lo = hram ? hram[lo_idx] : GB_safe_read_memory(gb, r->sp);
            uint8_t hi = hram ? hram[hi_idx] : GB_safe_read_memory(gb, r->sp + 1);
            r->sp += 2;
            r->pc = (uint16_t)(lo | (hi << 8));
        }
    };
    if(pc == DELAY_FRAME_PC || pc == DELAY_FRAMES_PC ||
       pc == BATTLE_TEXTBOX_PC || pc == STD_BATTLE_TEXTBOX_PC ||
       pc == REFRESH_BATTLE_HUDS_PC){
        do_ret_skip();
        return;
    }
    // ByteFill (0x3041): HARNESS_ERROR if destination is outside WRAM/HRAM.
    // An invalid destination (DE < 0x8000) means the fixture has an uninitialized
    // pointer that Crystal is using to address ROM or VRAM.
    // This must fail explicitly -- never skip a semantic fill operation.
    if(pc == 0x3041){
        GB_registers_t* br = GB_get_registers(gb);
        if(br && br->de < 0x8000){
            static char bytefill_err[128];
            snprintf(bytefill_err, sizeof(bytefill_err),
                "__HARNESS_ERROR__ ByteFill(DE=0x%04X BC=0x%04X): "
                "fixture has uninitialized pointer -- add field to fixture",
                br->de, br->bc);
            ctx->triggered      = true;
            ctx->triggered_sink = bytefill_err;
            return;
        }
    }
    // AnimateHPBar (03:46E0) -- bank-guarded skip to avoid aliasing with
    // BattleCommand_Stab code in bank 0D at the same in-bank address.
    if(pc == 0x46E0 && GB_safe_read_memory(gb, 0xFF9D) == 0x03){
        do_ret_skip();
        return;
    }
    // AnimateCurrentMoveEitherSide (0D:7DE9) -- CALL-entered from BattleCommand_Present.
    // Calls BattleCommand_LowerSub, PlayDamageAnim, BattleCommand_RaiseSub.
    // Purely presentational (HP bar animation + damage flash). Returns via RET at 0x7E00.
    // Only skipped when NOT registered as a sink for this case (direct-Present cases
    // 2171/2174 use it as their sink; full-script case 2176 must skip past it).
    if(pc == 0x7DE9 && GB_safe_read_memory(gb, 0xFF9D) == 0x0D){
        // If it's registered as a sink, let sink detection below handle it.
        bool is_sink = false;
        for(size_t i=0; i<ctx->num_sinks; ++i)
            if(ctx->sink_pcs[i] == 0x7DE9){ is_sink=true; break; }
        if(!is_sink){
            do_ret_skip();
            return;
        }
    }
    if(pc == DOMOVE_EFFECT_DISPATCH_PC && GB_safe_read_memory(gb, 0xFF9D) == 0x0D){
        // JP HL was fetched. HL contains the target command handler address.
        GB_registers_t* r = GB_get_registers(gb);
        if(r && r->hl == USED_MOVE_TEXT_HANDLER_PC){
            // Skip UsedMoveText: redirect JP HL to the loop-continue point (0x4081)
            // and discard the return address that call .DoMoveEffectCommand pushed.
            r->hl = DOMOVE_DISPATCHER_CONT_PC;  // JP HL will land here
            r->sp += 2;                          // discard pushed return addr 0x4081
        }
        return;
    }

    // Sink detection
    for(size_t i=0; i<ctx->num_sinks; ++i){
        if(pc == ctx->sink_pcs[i]){
            ctx->triggered = true;
            ctx->triggered_sink = ctx->sink_names[i];
            return;
        }
    }
}

// ============================================================================
// StopReason
// ============================================================================
enum class StopReason {
    SINK_HIT,
    MAX_INSN_EXCEEDED,
    WALL_CLOCK_TIMEOUT,
    RNG_TAPE_EXHAUSTED,
    GB_INIT_FAILED,
    WRAM_ACCESS_FAILED,
    REGS_ACCESS_FAILED,
};
static const char* stop_reason_str(StopReason r){
    switch(r){
    case StopReason::SINK_HIT:           return "SINK_HIT";
    case StopReason::MAX_INSN_EXCEEDED:  return "MAX_INSN_EXCEEDED";
    case StopReason::WALL_CLOCK_TIMEOUT: return "WALL_CLOCK_TIMEOUT";
    case StopReason::RNG_TAPE_EXHAUSTED: return "RNG_TAPE_EXHAUSTED";
    case StopReason::GB_INIT_FAILED:     return "GB_INIT_FAILED";
    case StopReason::WRAM_ACCESS_FAILED: return "WRAM_ACCESS_FAILED";
    case StopReason::REGS_ACCESS_FAILED: return "REGS_ACCESS_FAILED";
    }
    return "?";
}

// ============================================================================
// CrystalRunResult -- raw output from run_crystal_case
// ============================================================================
struct CrystalRunResult {
    StopReason stop_reason;
    int        insn_count;
    const char* sink_name; // which sink fired (null if not SINK_HIT)
    // Snapshot fields -- populated on SINK_HIT
    bool has_snapshot;
    // Generic: stat stages + computed stats (for Haze)
    uint8_t  player_stages[7];
    uint8_t  enemy_stages[7];
    uint16_t player_stats[5];
    uint16_t enemy_stats[5];
    // Present-specific outputs
    uint8_t  battle_anim_param;   // wBattleAnimParam (0/1/2=damage tier, 3=heal)
    uint16_t cur_damage;          // wCurDamage (big-endian u16)
    uint16_t player_hp;           // wBattleMonHP
    uint16_t enemy_hp;            // wEnemyMonHP
    // RNG trace
    std::vector<RngEntry> rng_trace;
    size_t   rng_bytes_consumed;
};

static bool crystal_run_results_equal(const CrystalRunResult& a, const CrystalRunResult& b){
    // Compare all semantic outputs. Used for poison stability check.
    if(a.stop_reason != b.stop_reason) return false;
    if(a.stop_reason != StopReason::SINK_HIT) return a.stop_reason == b.stop_reason;
    for(int i=0;i<7;i++) if(a.player_stages[i]!=b.player_stages[i]||a.enemy_stages[i]!=b.enemy_stages[i]) return false;
    for(int i=0;i<5;i++) if(a.player_stats[i]!=b.player_stats[i]||a.enemy_stats[i]!=b.enemy_stats[i]) return false;
    if(a.battle_anim_param != b.battle_anim_param) return false;
    if(a.cur_damage        != b.cur_damage)        return false;
    if(a.player_hp         != b.player_hp)         return false;
    if(a.enemy_hp          != b.enemy_hp)          return false;
    if(a.rng_bytes_consumed != b.rng_bytes_consumed) return false;
    for(size_t i=0; i<a.rng_trace.size() && i<b.rng_trace.size(); ++i)
        if(a.rng_trace[i].tape_value != b.rng_trace[i].tape_value) return false;
    return true;
}

// ============================================================================
// EngineSnapshot
// ============================================================================
struct EngineSnapshot {
    int8_t   player_stages[7];
    int8_t   enemy_stages[7];
    uint16_t player_stats[5];
    uint16_t enemy_stats[5];
    uint16_t player_hp;
    uint16_t enemy_hp;
    size_t   rng_bytes_consumed;
};

// ============================================================================
// Shared fixture constants
// ============================================================================
static constexpr int8_t   PLAYER_DELTA[7] = {+2,-3,+1,-1,+3,+4,-2};
static constexpr int8_t   ENEMY_DELTA[7]  = {-4,+6,-2,+3,-1,-3,+5};
static constexpr uint16_t P_ATK=110,P_DEF=60,P_SPD=130,P_SATK=95,P_SDEF=70;
static constexpr uint16_t E_ATK=75, E_DEF=110,E_SPD=30, E_SATK=100,E_SDEF=80;
static constexpr uint16_t P_HP=300, E_HP=300;
static constexpr uint8_t  P_LEVEL=50, E_LEVEL=50;

// ============================================================================
// run_crystal_case
//
// Per-case run. entry_sym/bank selects which command to cold-call.
// sink_pcs[] lists allowlisted presentation boundary addresses.
// rng_tape provides the deterministic RNG bytes; nullptr means no RNG interception
// (Haze doesn't use BattleRandom so no interception needed for it).
// fixture_fn writes case-specific WRAM beyond the common baseline.
// ============================================================================
using FixtureFn = void(*)(GB_gameboy_t* gb, uint8_t* wram, const SymCache& sym);

static void fixture_common(GB_gameboy_t* gb, uint8_t* wram, const SymCache& sym){
    // Baseline fixture: same for all cases
    wram[wram_off(sym.wLinkMode.addr)]          = 0;
    wram[wram_off(sym.wInBattleTowerBattle.addr)]= 0;
    wram[wram_off(sym.wJohtoBadges.addr)]        = 0;
    wram[wram_off(sym.wBattleMonStatus.addr)  ]  = 0;
    wram[wram_off(sym.wBattleMonStatus.addr)+1]  = 0;
    wram[wram_off(sym.wEnemyMonStatus.addr)  ]   = 0;
    wram[wram_off(sym.wEnemyMonStatus.addr)+1]   = 0;
    // Stat stages (overwritten by Haze, pre-set for Present)
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
    // Base stats (for CalcBattleStats in Haze)
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
    GB_write_memory(gb,sym.hBattleTurn.addr,0x00);
    GB_write_memory(gb,sym.hROMBank.addr,0x0D); // set at call site; override if needed
    GB_write_memory(gb,0xFFFF,0x00);
    GB_write_memory(gb,0xFF0F,0x00);
    // Zero out snapshot-read fields so they're poison-stable across runs.
    // Haze doesn't write these; Present writes them. Starting at 0 makes them
    // deterministic for poison stability regardless of which move runs.
    wram[wram_off(sym.wBattleAnimParam.addr)] = 0;
    wram[wram_off(sym.wCurDamage.addr)  ]  = 0;
    wram[wram_off(sym.wCurDamage.addr)+1]  = 0;
    wram[wram_off(sym.wBattleMonHP.addr)  ] = 0x01;  // set > 0 so present damage is meaningful
    wram[wram_off(sym.wBattleMonHP.addr)+1] = 0x2C;  // P_HP = 300 = 0x012C
    wram[wram_off(sym.wEnemyMonHP.addr)  ]  = 0x01;
    wram[wram_off(sym.wEnemyMonHP.addr)+1]  = 0x2C;
    wram[wram_off(sym.wCriticalHit.addr)]   = 0;
}

struct CrystalRunConfig {
    Sym         entry;            // which command to call
    uint16_t    sink_pcs[4];      // allowlisted presentation sinks (up to 4)
    const char* sink_names[4];
    size_t      num_sinks;
    int         insn_cap;
    const uint8_t* rng_tape;      // null if no RNG interception
    size_t         rng_tape_len;
    FixtureFn   extra_fixture;    // null if no extra fixture
};

static CrystalRunResult run_crystal_case(
    const std::vector<uint8_t>& rom_bytes,
    const SymCache& sym,
    uint8_t poison,
    const CrystalRunConfig& cfg,
    std::atomic<bool>* stop_flag)
{
    CrystalRunResult res{};
    res.stop_reason    = StopReason::GB_INIT_FAILED;
    res.insn_count     = 0;
    res.sink_name      = nullptr;
    res.has_snapshot   = false;
    res.rng_bytes_consumed = 0;

    GB_gameboy_t gb;
    if(!GB_init(&gb,GB_MODEL_CGB_E)) return res;

    static thread_local uint32_t tl_pixels[160*144];
    GB_set_log_callback(&gb,sb_log_nop);
    GB_set_rgb_encode_callback(&gb,sb_rgb_nop);
    GB_set_pixels_output(&gb,tl_pixels);
    GB_set_rendering_disabled(&gb,true);
    GB_set_turbo_mode(&gb,true,true);

    // RNG tape context -- attached to exec_ctx so exec_cb handles interception
    // at PC=0x2FAD (LD A,(0xCFB6) in BattleRandom).
    std::unique_ptr<RngCtx> rng_ctx;
    if(cfg.rng_tape && cfg.rng_tape_len > 0){
        rng_ctx = std::make_unique<RngCtx>();
        rng_ctx->tape      = cfg.rng_tape;
        rng_ctx->tape_len  = cfg.rng_tape_len;
        rng_ctx->tape_idx  = 0;
        rng_ctx->exhausted = false;
    }

    // Execution context. exec_cb handles: sink detection, wall-clock stop, RNG.
    ExecCtx exec_ctx{};
    for(size_t i=0;i<cfg.num_sinks;++i){
        exec_ctx.sink_pcs[i]   = cfg.sink_pcs[i];
        exec_ctx.sink_names[i] = cfg.sink_names[i];
    }
    exec_ctx.num_sinks     = cfg.num_sinks;
    exec_ctx.triggered     = false;
    exec_ctx.triggered_sink= nullptr;
    exec_ctx.insn_count    = 0;
    exec_ctx.stop_flag     = stop_flag;
    exec_ctx.rng_ctx       = rng_ctx.get(); // null if no RNG for this case

    GB_set_user_data(&gb,&exec_ctx);
    GB_set_execution_callback(&gb,exec_cb);
    // No read_memory_callback -- RNG is intercepted in exec_cb at PC=0x2FAD

    GB_load_rom_from_buffer(&gb,rom_bytes.data(),rom_bytes.size());
    GB_write_memory(&gb,0xFF50,1);
    GB_write_memory(&gb,0x2000,cfg.entry.bank);

    size_t wram_sz=0; uint16_t wbank=0;
    uint8_t* wram=static_cast<uint8_t*>(
        GB_get_direct_access(&gb,GB_DIRECT_ACCESS_RAM,&wram_sz,&wbank));
    if(!wram||wram_sz<0x2000){
        GB_free(&gb); res.stop_reason=StopReason::WRAM_ACCESS_FAILED; return res;
    }
    std::memset(wram,poison,wram_sz);

    // Common fixture
    fixture_common(&gb, wram, sym);
    GB_write_memory(&gb,sym.hROMBank.addr,cfg.entry.bank);

    // Case-specific extra fixture
    if(cfg.extra_fixture) cfg.extra_fixture(&gb, wram, sym);

    GB_registers_t* regs=GB_get_registers(&gb);
    if(!regs){ GB_free(&gb); res.stop_reason=StopReason::REGS_ACCESS_FAILED; return res; }

    // Push return address (first sink) onto the stack. If execution RETs before
    // any sink fires, it lands at sink_pcs[0] and the exec_cb catches it.
    //
    // INITIAL_SP = 0xFFFF: Crystal uses HRAM 0xFF80-0xFFEB for its own variables
    // (hBattleTurn=0xFFE4, hROMBank=0xFF9D, etc.). Starting the stack at 0xFFFF
    // means the first push goes to 0xFFFD-0xFFFE (unused HRAM), and 12 levels of
    // nested CALLs reach 0xFFE5 -- safely above Crystal's variables at 0xFFEB.
    // The old value 0xFFF0 allowed only 3 nested calls before overwriting 0xFFE4.
    static constexpr uint16_t INITIAL_SP = 0xFFFF;
    uint16_t ret_addr = cfg.sink_pcs[0];
    GB_write_memory(&gb,INITIAL_SP-1,(ret_addr>>8)&0xFF);
    GB_write_memory(&gb,INITIAL_SP-2, ret_addr    &0xFF);
    regs->sp = INITIAL_SP-2;
    regs->pc = cfg.entry.addr;

    // Execute
    while(!exec_ctx.triggered && exec_ctx.insn_count < cfg.insn_cap)
        GB_run(&gb);

    res.insn_count = exec_ctx.insn_count;
    res.sink_name  = exec_ctx.triggered_sink;

    // Check termination cause
    if(!exec_ctx.triggered){
        GB_free(&gb); res.stop_reason=StopReason::MAX_INSN_EXCEEDED; return res;
    }
    if(exec_ctx.triggered_sink && std::string(exec_ctx.triggered_sink)=="__TIMEOUT__"){
        GB_free(&gb); res.stop_reason=StopReason::WALL_CLOCK_TIMEOUT; return res;
    }
    if(rng_ctx && rng_ctx->exhausted){
        GB_free(&gb); res.stop_reason=StopReason::RNG_TAPE_EXHAUSTED; return res;
    }

    // Populate RNG trace
    if(rng_ctx){
        res.rng_trace = rng_ctx->trace;
        res.rng_bytes_consumed = rng_ctx->tape_idx;
    }

    // Re-acquire WRAM
    wram_sz=0;
    wram=static_cast<uint8_t*>(GB_get_direct_access(&gb,GB_DIRECT_ACCESS_RAM,&wram_sz,&wbank));
    if(!wram){ GB_free(&gb); res.stop_reason=StopReason::WRAM_ACCESS_FAILED; return res; }

    res.has_snapshot = true;
    // Stages + computed stats (Haze outputs)
    {auto* p=wram+wram_off(sym.wPlayerStatLevels.addr); for(int i=0;i<7;i++) res.player_stages[i]=p[i];}
    {auto* p=wram+wram_off(sym.wEnemyStatLevels.addr);  for(int i=0;i<7;i++) res.enemy_stages[i]=p[i];}
    {auto* p=wram+wram_off(sym.wBattleMonAttack.addr);  for(int i=0;i<5;i++) res.player_stats[i]=(uint16_t)((p[i*2]<<8)|p[i*2+1]);}
    {auto* p=wram+wram_off(sym.wEnemyMonAttack.addr);   for(int i=0;i<5;i++) res.enemy_stats[i]=(uint16_t)((p[i*2]<<8)|p[i*2+1]);}
    // Present-specific outputs
    res.battle_anim_param = wram[wram_off(sym.wBattleAnimParam.addr)];
    {auto* p=wram+wram_off(sym.wCurDamage.addr); res.cur_damage=(uint16_t)((p[0]<<8)|p[1]);}
    {auto* p=wram+wram_off(sym.wBattleMonHP.addr); res.player_hp=(uint16_t)((p[0]<<8)|p[1]);}
    {auto* p=wram+wram_off(sym.wEnemyMonHP.addr);  res.enemy_hp=(uint16_t)((p[0]<<8)|p[1]);}

    GB_free(&gb);
    res.stop_reason = StopReason::SINK_HIT;
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
    enginemon::MoveId move_id,
    const EngineData& ed,
    const uint8_t* rng_tape, size_t rng_tape_len)
{
    const enginemon::MoveData* md=ed.moves.get(move_id);
    if(!md||!md->effect_desc.is_supported) return std::nullopt;

    enginemon::Registries reg{}; reg.moves=ed.moves;
    enginemon::Party party;
    { enginemon::Pokemon pm{}; pm.species=1; pm.level=P_LEVEL;
      pm.current_hp=pm.max_hp=P_HP; pm.friendship=200; party.add(pm); }
    enginemon::Battle bat(enginemon::BattleType::Wild,party,reg,ed.rules);

    auto make=[](enginemon::MoveId mid,uint16_t atk,uint16_t def,uint16_t spd,
                 uint16_t satk,uint16_t sdef,uint16_t hp,uint8_t lvl,const int8_t* sd){
        enginemon::BattlePokemon bp{};
        bp.species=1; bp.type1=0; bp.type2=0; bp.level=lvl;
        bp.stats.hp=bp.stats.max_hp=hp; bp.base_stats.hp=bp.base_stats.max_hp=hp;
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
    bat.player_pokemon()   = make(move_id,P_ATK,P_DEF,P_SPD,P_SATK,P_SDEF,P_HP,P_LEVEL,PLAYER_DELTA);
    bat.opponent_pokemon() = make(enginemon::MOVE_NONE,E_ATK,E_DEF,E_SPD,E_SATK,E_SDEF,E_HP,E_LEVEL,ENEMY_DELTA);

    // Feed tape to Enginemon's RNG (same bytes, same order)
    size_t rng_idx = 0;
    size_t rng_consumed = 0;
    if(rng_tape && rng_tape_len > 0){
        bat.set_rng_callback([rng_tape, rng_tape_len, &rng_idx, &rng_consumed]()->uint32_t{
            if(rng_idx >= rng_tape_len) return 0xFF; // exhaustion is caught separately
            uint32_t val = rng_tape[rng_idx++];
            ++rng_consumed;
            return val;
        });
    } else {
        bat.set_rng_callback([]()->uint32_t{ return 0xFF; });
    }

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
    e.player_hp = (uint16_t)pp.stats.hp;
    e.enemy_hp  = (uint16_t)op.stats.hp;
    e.rng_bytes_consumed = rng_consumed;
    return e;
}

// ============================================================================
// Case result
// ============================================================================
enum class Status { MATCH, ENGINEMON_MISMATCH, HARNESS_ERROR };

struct CaseResult {
    uint16_t    move_id;
    const char* move_name;
    Status      status;
    std::string detail;
    int         insn_count;
    bool        poison_stable;
    const char* stop_reason;
    const char* boundary;
    CrystalRunResult crystal_res; // from poison=0x00 run
    EngineSnapshot   engine_res;
    bool has_crystal = false;
    bool has_engine  = false;
};

// ============================================================================
// MoveSpec -- per-move configuration
// ============================================================================
struct MoveSpec {
    uint16_t    id;           // unique registration ID (used for --move and dedup)
    uint16_t    engine_id;    // Crystal move ID to pass to Enginemon (may differ from id)
    const char* name;
    int         insn_cap;
    // RNG tape: deterministic bytes supplied to both Crystal and Enginemon.
    // nullptr = no RNG interception (move doesn't call BattleRandom).
    const uint8_t* rng_tape;
    size_t         rng_tape_len;
    // Builder: creates the CrystalRunConfig for this move
    void (*build_config)(const SymCache& sym, CrystalRunConfig* out);
    // Enginemon fixture: any extra setup beyond the common fixture
    // (currently unused -- Enginemon setup is done in run_enginemon_case)
    void* reserved;
};

// ============================================================================
// Haze config builder
// ============================================================================
static void haze_build_config(const SymCache& sym, CrystalRunConfig* out){
    out->entry         = sym.BattleCommand_ResetStats;
    out->sink_pcs[0]   = sym.AnimateCurrentMove.addr;
    out->sink_names[0] = "AnimateCurrentMove";
    out->num_sinks     = 1;
    out->rng_tape      = nullptr;
    out->rng_tape_len  = 0;
    out->extra_fixture = nullptr;
}

// ============================================================================
// Present config builders and fixtures
//
// Entry: BattleCommand_Present (0D:7874) -- entered directly, not via DoMove.
//
// What BattleCommand_Present does:
//   1. Calls BattleCommand_Stab (computes wTypeMatchup, possibly sets wAttackMissed for
//      immune types, applies weather/badge/STAB modifiers to wCurDamage).
//   2. Checks wTypeMatchup == 0 → jp AnimateFailedMove (immune)
//   3. Checks wAttackMissed != 0 → jp AnimateFailedMove (missed)
//   4. Calls BattleRandom once → b
//   5. Walks PresentPower table:
//        b < 0x66          → power=40  (wBattleAnimParam=0), call AnimateCurrentMoveEitherSide, ret
//        0x66 <= b < 0xB4  → power=80  (wBattleAnimParam=1), call AnimateCurrentMoveEitherSide, ret
//        0xB4 <= b < 0xCC  → power=120 (wBattleAnimParam=2), call AnimateCurrentMoveEitherSide, ret
//        table -1 sentinel → heal: wBattleAnimParam=3, call AnimateCurrentMove,
//                            ... SwitchTurn, AICheckMaxHP, GetQuarterMaxHP,
//                            RestoreHP, RegainedHealthText, UpdateOpponentInParty ...
//                            jp EndMoveEffect
//
// Since we enter at BattleCommand_Present (not DoMove):
//   • CheckHit and Critical (earlier in the DoMove script) are NOT executed.
//   • wAttackMissed is set by the fixture for the miss case.
//   • wTypeMatchup must be pre-set to 0x10 (normal) for hit cases, 0 for immune.
//
// BattleRandom bytes per path (entering at BattleCommand_Present):
//   damage (any power tier): 1 byte (power selection)
//   heal (0xFF):             1 byte (power selection = table sentinel)
//   miss (wAttackMissed=1):  0 bytes (branches before BattleRandom)
//
// NOTE: The damage path calls AnimateCurrentMoveEitherSide from *inside*
// BattleCommand_Present and then rets. DamageVariation (cmd 0x08 in the DoMove
// script) runs after that ret -- but our sink fires at AnimateCurrentMoveEitherSide
// so we stop there and DamageVariation does NOT consume a tape byte.
//
// Sinks:
//   AnimateCurrentMoveEitherSide (0D:7DE9) -- damage path
//   EndMoveEffect                (0D:52A3) -- heal path (jp at end of heal block)
//   AnimateFailedMove            (0D:7E77) -- miss/immune path
//
// PresentPower thresholds (from data/moves/present_power.asm):
//   0x66 = 40% (floor(255*0.40))  → power 40
//   0xB4 = 71% (floor(255*0.70)+1) → power 80
//   0xCC = 80% (floor(255*0.80))  → power 120
//   0xFF (sentinel -1)            → heal
//
// Four deterministic tapes covering all paths:
//   damage/power40: [0x30]        Present=0x30 < 0x66 → power 40
//   heal:           [0xFF]        Present=0xFF = table sentinel → heal
//   miss:           []            wAttackMissed=1 in fixture, BattleRandom not called
//   0xFF-sentinel:  [0xFF]        Same byte, same outcome as heal (explicit 0xFF path)
// ============================================================================

// Tape 1: damage path, power=40. Present power byte only (no CheckHit/Critical in BattleCommand_Present).
static constexpr uint8_t PRESENT_TAPE_DAMAGE[] = { 0x30 };
// Tape 2: heal path. 0xFF matches the PresentPower table -1 sentinel → heal.
static constexpr uint8_t PRESENT_TAPE_HEAL[]   = { 0xFF };
// Tape 3: miss path. wAttackMissed=1 is set in the miss fixture before entry.
// BattleCommand_Present branches to AnimateFailedMove before calling BattleRandom.
// No RNG bytes consumed -- tape is empty.
// (nullptr/0 in the MoveSpec disables RNG interception for this case.)
// Tape 4: 0xFF sentinel. Same byte as HEAL; exercises the explicit -1 table sentinel path.
static constexpr uint8_t PRESENT_TAPE_SENTINEL[] = { 0xFF };

// Common Present fixture -- sets WRAM fields read by BattleCommand_Present and
// its sub-calls (BattleCommand_Stab, BattleRandom, heal-path HP checks).
// Used by damage, heal, and sentinel cases (all of which have wAttackMissed=0).
static void present_extra_fixture(GB_gameboy_t* gb, uint8_t* wram, const SymCache& sym){
    auto be16=[](uint8_t* d,uint16_t v){d[0]=(v>>8);d[1]=v&0xFF;};

    // Species placeholder (Bulbasaur=1) -- BattleCommand_Stab checks for Pikachu/Marowak
    // special crit items; species 1 has no such item. wBattleMonItem=0 confirms no item.
    wram[wram_off(sym.wBattleMonSpecies.addr)] = 1;
    wram[wram_off(sym.wEnemyMonSpecies.addr)]  = 1;
    wram[wram_off(sym.wBattleMonItem.addr)]    = 0;
    wram[wram_off(sym.wEnemyMonItem.addr)]     = 0;

    // wCurPlayerMove = 217 (Present) -- used by BattleCommand_Stab for move-type lookup
    wram[wram_off(sym.wCurPlayerMove.addr)] = 217;

    // wPlayerMoveStruct (0xC60F, 6 bytes): present_extra_fixture sets all fields
    // to avoid poison-dependent behavior in BattleCommand_Stab and AnimateCurrentMove.
    //   byte 0: Animation/Effect ID  -- 0 → LoadMoveAnim returns early (skips PlayBattleAnim)
    //   byte 1: Power                -- set per-case; here 0 (overridden for damage case)
    //   byte 2: Type                 -- Normal (0x00)
    //   byte 3: Accuracy             -- 0x5A = 90% (not used since entry is BattleCommand_Present)
    //   byte 4: PP                   -- 0 (not read by BattleCommand_Present)
    //   byte 5: Effect Chance        -- 0 (not used here)
    wram[wram_off(sym.wPlayerMoveStruct.addr) + 0] = 0;    // animation=0 → skip PlayBattleAnim
    wram[wram_off(sym.wPlayerMoveStruct.addr) + 1] = 0;    // power (damage=0 for heal/miss cases)
    wram[wram_off(sym.wPlayerMoveStruct.addr) + 2] = 0x00; // type = Normal
    wram[wram_off(sym.wPlayerMoveStruct.addr) + 3] = 0x5A; // accuracy = 90%
    wram[wram_off(sym.wPlayerMoveStruct.addr) + 4] = 0;    // pp
    wram[wram_off(sym.wPlayerMoveStruct.addr) + 5] = 0;    // effect chance

    // Poison-stability fields: these must be explicitly set so both poison runs agree.
    //
    // wOptions (0xCFCC): CheckBattleScene reads bit BATTLE_SCENE (bit 5).
    //   If set → returns carry → heal path enters AnimateFailedMove+PresentFailedText.
    //   If clear → returns no-carry → heal path goes directly to EndMoveEffect (our sink).
    //   Set to 0 to keep carry clear (no battle scene, consistent both runs).
    static constexpr uint16_t WOPTIONS_ADDR          = 0xCFCC;
    static constexpr uint16_t WENEMYMONNICKNAME_ADDR = 0xC616;
    static constexpr uint16_t WBATTLEMONNICKNAME_ADDR = 0xC621;
    static constexpr uint16_t WOTPARTYCOUNT_ADDR     = 0xD280;
    static constexpr uint8_t  CRYSTAL_STRING_END     = 0x50;  // Crystal "@" string terminator
    // Use GB_write_memory (MMU path) to guarantee the write reaches the address
    // Crystal will read at runtime, matching what read-back via wram[] also sees.
    GB_write_memory(gb, WOPTIONS_ADDR, 0);  // BATTLE_SCENE bit clear → _CheckBattleScene returns nc
    // Null-terminate nicknames: PlaceString loops until 0x50; poison=0xA5 has no 0x50.
    GB_write_memory(gb, WENEMYMONNICKNAME_ADDR,  CRYSTAL_STRING_END);
    GB_write_memory(gb, WBATTLEMONNICKNAME_ADDR, CRYSTAL_STRING_END);
    // wOTPartyCount: UpdateOpponentInParty iterates this many times.
    // Set to 1 to keep both runs identical and fast.
    GB_write_memory(gb, WOTPARTYCOUNT_ADDR, 1);

    // Types: Normal/Normal attacker, Normal/Normal defender → 1× matchup
    wram[wram_off(sym.wBattleMonType1.addr)] = 0x00;
    wram[wram_off(sym.wBattleMonType2.addr)] = 0x00;
    wram[wram_off(sym.wEnemyMonType1.addr)]  = 0x00;
    wram[wram_off(sym.wEnemyMonType2.addr)]  = 0x00;

    // Levels
    wram[wram_off(sym.wBattleMonLevel.addr)] = P_LEVEL;
    wram[wram_off(sym.wEnemyMonLevel.addr)]  = E_LEVEL;

    // HP (re-assert over common fixture)
    be16(wram+wram_off(sym.wBattleMonHP.addr),    P_HP);
    be16(wram+wram_off(sym.wBattleMonMaxHP.addr), P_HP);
    be16(wram+wram_off(sym.wEnemyMonHP.addr),     E_HP);
    be16(wram+wram_off(sym.wEnemyMonMaxHP.addr),  E_HP);

    // Active battle stats
    {
        uint8_t* p = wram + wram_off(sym.wBattleMonAttack.addr);
        auto be=[](uint8_t* d,uint16_t v){d[0]=(v>>8);d[1]=v&0xFF;};
        be(p+0,P_ATK); be(p+2,P_DEF); be(p+4,P_SPD); be(p+6,P_SATK); be(p+8,P_SDEF);
    }
    {
        uint8_t* p = wram + wram_off(sym.wEnemyMonAttack.addr);
        auto be=[](uint8_t* d,uint16_t v){d[0]=(v>>8);d[1]=v&0xFF;};
        be(p+0,E_ATK); be(p+2,E_DEF); be(p+4,E_SPD); be(p+6,E_SATK); be(p+8,E_SDEF);
    }

    // Pre-set type matchup to 1× (0x10), wAttackMissed=0 (hit), wCriticalHit=0
    // BattleCommand_Stab will recompute wTypeMatchup from scratch; the pre-set
    // wTypeMatchup=0x10 is what CheckTypeMatchup initialises wTypeMatchup to before
    // the type-matchup loop (EFFECTIVE=0x10 in Crystal). Setting it here ensures
    // the value is defined even if BattleCommand_Stab is somehow skipped.
    wram[wram_off(sym.wTypeMatchup.addr)]  = 0x10;  // EFFECTIVE -- 1× damage
    wram[wram_off(sym.wAttackMissed.addr)] = 0;     // hit
    wram[wram_off(sym.wCriticalHit.addr)]  = 0;     // no crit

    // hROMBank = 0x0D (BattleCommand_Present's bank)
    GB_write_memory(gb, sym.hROMBank.addr, sym.BattleCommand_Present.bank);
}

// Miss fixture -- identical to present_extra_fixture but sets wAttackMissed=1.
// BattleCommand_Present checks wAttackMissed immediately after BattleCommand_Stab
// returns (before calling BattleRandom), so no RNG bytes are consumed.
static void present_miss_extra_fixture(GB_gameboy_t* gb, uint8_t* wram, const SymCache& sym){
    present_extra_fixture(gb, wram, sym);          // inherit common setup
    wram[wram_off(sym.wAttackMissed.addr)] = 1;    // MISSED -- forces AnimateFailedMove
}

// present_build_config_impl -- used by damage, heal, and sentinel cases.
// All three share present_extra_fixture (wAttackMissed=0, wTypeMatchup=0x10).
static void present_build_config_impl(const SymCache& sym, CrystalRunConfig* out,
                                       const uint8_t* tape, size_t tape_len,
                                       FixtureFn fixture_fn = present_extra_fixture){
    out->entry         = sym.BattleCommand_Present;
    // Four sinks covering all branches:
    //   damage: AnimateCurrentMoveEitherSide (called inside Present before ret)
    //   heal:   AnimateCurrentMove (first call in the heal block, before HP changes;
    //           EndMoveEffect comes later but the animation engine may loop on uninitialized
    //           graphics state -- AnimateCurrentMove fires before that happens)
    //   miss:   AnimateFailedMove (jp from Present when wAttackMissed or wTypeMatchup==0)
    //   fallback: EndMoveEffect (in case code flow reaches it via already-full-HP path
    //             without calling AnimateCurrentMove first -- that path exists but
    //             AnimateCurrentMove IS called at the start of the heal block for all
    //             heal outcomes including the already-full-HP case)
    out->sink_pcs[0]   = sym.AnimateCurrentMoveEitherSide.addr;
    out->sink_names[0] = "AnimateCurrentMoveEitherSide";
    out->sink_pcs[1]   = sym.AnimateCurrentMove.addr;
    out->sink_names[1] = "AnimateCurrentMove";
    out->sink_pcs[2]   = sym.AnimateFailedMove.addr;
    out->sink_names[2] = "AnimateFailedMove";
    out->sink_pcs[3]   = sym.EndMoveEffect.addr;
    out->sink_names[3] = "EndMoveEffect";
    out->num_sinks     = 4;
    out->rng_tape      = tape;
    out->rng_tape_len  = tape_len;
    out->extra_fixture = fixture_fn;
}

static void present_damage_build_config(const SymCache& sym, CrystalRunConfig* out){
    present_build_config_impl(sym, out, PRESENT_TAPE_DAMAGE, sizeof(PRESENT_TAPE_DAMAGE));
}
static void present_heal_build_config(const SymCache& sym, CrystalRunConfig* out){
    present_build_config_impl(sym, out, PRESENT_TAPE_HEAL, sizeof(PRESENT_TAPE_HEAL));
}
static void present_miss_build_config(const SymCache& sym, CrystalRunConfig* out){
    // Miss: no RNG tape (BattleRandom not reached), miss fixture sets wAttackMissed=1.
    present_build_config_impl(sym, out, nullptr, 0, present_miss_extra_fixture);
}
static void present_sentinel_build_config(const SymCache& sym, CrystalRunConfig* out){
    // 0xFF sentinel: same outcome as heal (table -1 entry), explicit 0xFF power byte.
    present_build_config_impl(sym, out, PRESENT_TAPE_SENTINEL, sizeof(PRESENT_TAPE_SENTINEL));
}

// ============================================================================
// Full-script Present fixture (DoMove entry)
//
// Entry: DoMove (0D:402C)
//   DoMove (0D:402C) is the correct full-script entrypoint. It:
//     1. Reads wPlayerMoveStruct.effect via GetBattleVar(MoveEffect=0x0D) to select script
//     2. Copies the Present effect script from MoveEffectsPointers[0x7A] into wBattleScriptBuffer
//     3. Dispatches each command: checkobedience, usedmovetext(skip), doturn, checkhit,
//        critical, damagestats, present, damagecalc, stab, damagevariation,
//        clearmissdamage, failuretext, applydamage, criticaltext, supereffectivetext,
//        checkfaint, buildopponentrage, kingsrock, endmove
//   No LCD init, no joypad reads, no SetPlayerTurn/SetEnemyTurn, no UpdateMoveData
//   name-scan overhead. Direct dispatch of the Present effect script.
//
// Why not DoTurn (0D:401D):
//   DoTurn calls UpdateMoveData which scans all 216 move names (~100K instructions)
//   to populate wStringBuffer1/2. In a cold-fixture environment this causes the
//   script dispatch loop to malfunction (script pointer resets).
//
// wPlayerMoveStruct: populated from ROM (effect=0x7A guaranteed). Animation byte
//   is overridden to 0 so PlayDamageAnim returns immediately (AND A; RET Z).
//
// RNG tape (5 bytes):
//   [0] 0x00 = CheckHit  (acc=0xE5=229, hit)
//   [1] 0x80 = Critical  (no crit at level 50)
//   [2] 0x30 = Present   (power 40)
//   [3] 0xB2 = DamageVariation byte 1
//   [4] 0xFF = DamageVariation byte 2
//
// Sink: EndMoveEffect (0D:52A3).

// ROM offset of the Crystal moves table (10:5AFB) + stride (7 bytes/entry).
// Used to read wPlayerMoveStruct bytes from the actual ROM.
static constexpr uint32_t CRYSTAL_MOVES_TABLE_FLAT = 0x10u*0x4000u + (0x5AFBu - 0x4000u); // = 0x55AFB
static constexpr uint32_t CRYSTAL_MOVE_DATA_SIZE   = 7u;
// Address constants used by the full-script fixture (referenced at file scope).
static constexpr uint8_t  CRYSTAL_STRING_END = 0x50;  // Crystal "@" string terminator
static constexpr uint16_t WOPTIONS_ADDR      = 0xCFCC; // wOptions (CheckBattleScene reads bit5)
static constexpr uint16_t WBATTLEMONNICKNAME = 0xC621; // 11 bytes, must be 0x50-terminated
static constexpr uint16_t WENEMYMONNICKNAME  = 0xC616; // 11 bytes, must be 0x50-terminated
static constexpr uint16_t WOTPARTYCOUNT      = 0xD280; // wOTPartyCount

// Tape: CheckHit, Critical, Present-power, DamVar-byte1, DamVar-byte2
static constexpr uint8_t PRESENT_TAPE_FULLSCRIPT_DAMAGE[] = { 0x00, 0x80, 0x30, 0xB2, 0xFF };

// Helper: read 7-byte move struct from ROM for move_id (1-based Crystal move ID).
// Populates wPlayerMoveStruct bytes [0..6] directly from the move data table.
// Layout: [anim, effect, power, type, acc, pp, chance].
// No hardcoded constants: all bytes are ROM-proven.
static void rom_populate_player_move_struct(
    const std::vector<uint8_t>& rom_bytes,
    uint8_t* wram, const SymCache& sym,
    uint16_t move_id)
{
    // 0-based index into moves table (move IDs are 1-based in Crystal)
    const uint32_t offset = CRYSTAL_MOVES_TABLE_FLAT + (uint32_t)(move_id - 1) * CRYSTAL_MOVE_DATA_SIZE;
    // Bounds check: table must exist in ROM
    if(offset + CRYSTAL_MOVE_DATA_SIZE > rom_bytes.size()){
        throw std::logic_error("rom_populate_player_move_struct: ROM offset out of bounds");
    }
    for(uint32_t i = 0; i < CRYSTAL_MOVE_DATA_SIZE; ++i)
        wram[wram_off((uint16_t)(sym.wPlayerMoveStruct.addr + i))] = rom_bytes[offset + i];
}

static void present_fullscript_fixture(
    GB_gameboy_t* gb, uint8_t* wram, const SymCache& sym,
    const std::vector<uint8_t>& rom_bytes)
{
    auto be16=[](uint8_t* d, uint16_t v){ d[0]=(uint8_t)(v>>8); d[1]=(uint8_t)(v&0xFF); };

    // ---------- wPlayerMoveStruct (0xC60F, 7 bytes) -- fully ROM-derived ----------
    // Read directly from the Crystal moves table for move ID 217 (Present).
    // Bytes verified: [0xD9, 0x7A, 0x01, 0x00, 0xE5, 0x0F, 0x00]
    // (anim=0xD9, effect=EFFECT_PRESENT=0x7A, power=1, type=Normal, acc=0xE5, pp=15, chance=0)
    // Animation byte 0xD9 causes AnimateCurrentMoveEitherSide (0D:7DE9) to call
    // PlayDamageAnim; that call is intercepted in exec_cb as a bank-guarded skip.
    rom_populate_player_move_struct(rom_bytes, wram, sym, 217);

    // ---------- Move identity fields (player turn, slot 0) ------------------
    wram[wram_off(sym.wCurPlayerMove.addr)]        = 217;   // wCurPlayerMove = Present
    wram[wram_off(sym.wBattleMonMoves.addr) + 0]   = 217;   // slot 0 = Present
    wram[wram_off(sym.wBattleMonMoves.addr) + 1]   = 0;
    wram[wram_off(sym.wBattleMonMoves.addr) + 2]   = 0;
    wram[wram_off(sym.wBattleMonMoves.addr) + 3]   = 0;
    wram[wram_off(sym.wCurMoveNum.addr)]            = 0;    // bank-1: slot index 0
    wram[wram_off(sym.wCurBattleMon.addr)]          = 0;    // bank-1: party slot 0

    // ---------- PP (player + party sync + enemy wild) -----------------------
    // Use ROM PP (0x0F=15) for all four slots. Enemy uses wWildMonPP (WILD_BATTLE mode).
    wram[wram_off(sym.wBattleMonPP.addr) + 0]      = 0x0F; // ROM-proven pp=15 for Present
    wram[wram_off(sym.wBattleMonPP.addr) + 1]      = 0;
    wram[wram_off(sym.wBattleMonPP.addr) + 2]      = 0;
    wram[wram_off(sym.wBattleMonPP.addr) + 3]      = 0;
    wram[wram_off(sym.wPartyMon1PP.addr) + 0]      = 0x0F; // party mirror
    wram[wram_off(sym.wPartyMon1PP.addr) + 1]      = 0;
    wram[wram_off(sym.wPartyMon1PP.addr) + 2]      = 0;
    wram[wram_off(sym.wPartyMon1PP.addr) + 3]      = 0;
    wram[wram_off(sym.wWildMonPP.addr) + 0]        = 0x0F; // enemy (wBattleMode=1 WILD)
    wram[wram_off(sym.wWildMonPP.addr) + 1]        = 0;
    wram[wram_off(sym.wWildMonPP.addr) + 2]        = 0;
    wram[wram_off(sym.wWildMonPP.addr) + 3]        = 0;
    wram[wram_off(sym.wWildMonMoves.addr)]          = 0;    // enemy move slot 0
    wram[wram_off(sym.wPartyCount.addr)]            = 1;    // one party mon

    // ---------- Battle mode / action ----------------------------------------
    wram[wram_off(sym.wBattleMode.addr)]            = 1;    // WILD_BATTLE
    wram[wram_off(sym.wLinkMode.addr)]              = 0;    // MUST be 0 (RNG intercept)
    wram[wram_off(sym.wInBattleTowerBattle.addr)]   = 1;    // bypass CheckObedience

    // ---------- Species and items -------------------------------------------
    // Bulbasaur (1): no Pikachu/Marowak special crit items. item=0 = no item.
    wram[wram_off(sym.wBattleMonSpecies.addr)]      = 1;
    wram[wram_off(sym.wEnemyMonSpecies.addr)]       = 1;
    wram[wram_off(sym.wBattleMonItem.addr)]         = 0;
    wram[wram_off(sym.wEnemyMonItem.addr)]          = 0;

    // ---------- Status and substatus (all clear) ----------------------------
    // Every substatus byte must be explicitly initialized -- uninitialized bytes
    // cause branch divergence between poison=0x00 and poison=0xA5 runs.
    wram[wram_off(sym.wBattleMonStatus.addr)]       = 0;    // 0xC63A
    wram[wram_off(sym.wEnemyMonStatus.addr)]        = 0;    // 0xD214
    wram[wram_off(sym.wPlayerSubStatus1.addr)]      = 0;    // 0xC668
    wram[wram_off(0xC669u)]                         = 0;    // wPlayerSubStatus2 (absent from SymCache)
    wram[wram_off(sym.wPlayerSubStatus3.addr)]      = 0;    // 0xC66A
    wram[wram_off(sym.wPlayerSubStatus4.addr)]      = 0;    // 0xC66B
    wram[wram_off(sym.wPlayerSubStatus5.addr)]      = 0;    // 0xC66C
    wram[wram_off(0xC66Du)]                         = 0;    // wEnemySubStatus1 (absent from SymCache)
    wram[wram_off(0xC66Eu)]                         = 0;    // wEnemySubStatus2
    wram[wram_off(sym.wEnemySubStatus3.addr)]       = 0;    // 0xC66F
    wram[wram_off(sym.wEnemySubStatus4.addr)]       = 0;    // 0xC670
    wram[wram_off(sym.wEnemySubStatus5.addr)]       = 0;    // 0xC671
    wram[wram_off(sym.wPlayerDisableCount.addr)]    = 0;
    wram[wram_off(sym.wDisabledMove.addr)]          = 0;
    wram[wram_off(sym.wPlayerCharging.addr)]        = 0;    // no recharge
    wram[wram_off(sym.wEnemyCharging.addr)]         = 0;
    wram[wram_off(sym.wTurnEnded.addr)]             = 0;    // DoTurn clears this, but init anyway
    wram[wram_off(sym.wAlreadyDisobeyed.addr)]      = 0;

    // ---------- Levels ------------------------------------------------------
    wram[wram_off(sym.wBattleMonLevel.addr)]        = P_LEVEL;
    wram[wram_off(sym.wEnemyMonLevel.addr)]         = E_LEVEL;

    // ---------- HP (enemy 250/300 so heal path actually heals) --------------
    be16(wram + wram_off(sym.wBattleMonHP.addr),    P_HP);
    be16(wram + wram_off(sym.wBattleMonMaxHP.addr), P_HP);
    be16(wram + wram_off(sym.wEnemyMonHP.addr),     250);
    be16(wram + wram_off(sym.wEnemyMonMaxHP.addr),  E_HP);

    // ---------- Battle stats ------------------------------------------------
    {
        uint8_t* p = wram + wram_off(sym.wBattleMonAttack.addr);
        be16(p+0,P_ATK); be16(p+2,P_DEF); be16(p+4,P_SPD); be16(p+6,P_SATK); be16(p+8,P_SDEF);
    }
    {
        uint8_t* p = wram + wram_off(sym.wEnemyMonAttack.addr);
        be16(p+0,E_ATK); be16(p+2,E_DEF); be16(p+4,E_SPD); be16(p+6,E_SATK); be16(p+8,E_SDEF);
    }
    // Unboosted stat mirrors used by DamageStats crit comparison
    be16(wram + wram_off(sym.wEnemyMonDefense.addr),  E_DEF);
    be16(wram + wram_off(sym.wEnemyMonSpclDef.addr),  E_SDEF);
    be16(wram + wram_off(sym.wEnemyMonSpclAtk.addr),  E_SATK);
    be16(wram + wram_off(sym.wPlayerAttack.addr),     P_ATK);
    be16(wram + wram_off(sym.wPlayerSpAtk.addr),      P_SATK);
    be16(wram + wram_off(sym.wEnemyDefense.addr),     E_DEF);
    be16(wram + wram_off(sym.wEnemySpDef.addr),       E_SDEF);

    // ---------- Types: Normal/Normal → 1× matchup ---------------------------
    wram[wram_off(sym.wBattleMonType1.addr)]        = 0x00;
    wram[wram_off(sym.wBattleMonType2.addr)]        = 0x00;
    wram[wram_off(sym.wEnemyMonType1.addr)]         = 0x00;
    wram[wram_off(sym.wEnemyMonType2.addr)]         = 0x00;

    // ---------- Stat stages (neutral = 7) -----------------------------------
    { uint8_t* p = wram + wram_off(sym.wPlayerStatLevels.addr); for(int i=0;i<8;i++) p[i]=7; }
    { uint8_t* p = wram + wram_off(sym.wEnemyStatLevels.addr);  for(int i=0;i<8;i++) p[i]=7; }

    // ---------- Misc battle state flags ------------------------------------
    wram[wram_off(sym.wPlayerTurnsTaken.addr)]      = 0;
    wram[wram_off(sym.wEnemyTurnsTaken.addr)]       = 0;
    wram[wram_off(0xC6E4u)]                         = 0;    // wCurEnemyMove
    wram[wram_off(0xC6E9u)]                         = 0;    // wCurEnemyMoveNum (0xC6E9)
    wram[wram_off(sym.wAttackMissed.addr)]          = 0;
    wram[wram_off(sym.wCriticalHit.addr)]           = 0;
    wram[wram_off(sym.wTypeMatchup.addr)]           = 0x10; // EFFECTIVE pre-init (BattleCommand_Stab rewrites)
    wram[wram_off(sym.wBattleWeather.addr)]         = 0;
    wram[wram_off(sym.wPlayerScreens.addr)]         = 0;
    wram[wram_off(sym.wEnemyScreens.addr)]          = 0;
    wram[wram_off(sym.wBattleAnimParam.addr)]       = 0;
    // Enemy move struct: set accuracy byte to 0xFF so enemy hit check always passes
    // when the effect script evaluates enemy's move (irrelevant for player Present, but
    // prevents poison-dependent behavior in the enemy-move path).
    wram[wram_off(sym.wEnemyMoveStruct.addr) + 3]  = 0xFF;  // +3 = wEnemyMoveStructAcc

    // ---------- HRAM / hardware registers -----------------------------------
    GB_write_memory(gb, sym.hBattleTurn.addr,  0x00);  // player turn
    GB_write_memory(gb, sym.hROMBank.addr, 0x0D);      // DoMove is in bank 0x0D
    GB_write_memory(gb, 0xFF70u, 1u);                  // rSVBK=1: bank-1 WRAM for 0xD000-0xDFFF

    // ---------- Poison-stability fields ------------------------------------
    // These are read during the Present effect script and must be deterministic.
    // wOptions bit5 (BATTLE_SCENE) controls CheckBattleScene carry; clear it so
    // the heal path goes directly to EndMoveEffect (our sink).
    GB_write_memory(gb, WOPTIONS_ADDR,      0u);
    // Nicknames: PlaceString scans until 0x50; poison bytes have no 0x50.
    GB_write_memory(gb, WENEMYMONNICKNAME,  CRYSTAL_STRING_END);
    GB_write_memory(gb, WBATTLEMONNICKNAME, CRYSTAL_STRING_END);
    // wOTPartyCount: UpdateOpponentInParty iterates this many slots.
    // Set to 1; with 0xA5 poison this would iterate 165 times unnecessarily.
    GB_write_memory(gb, WOTPARTYCOUNT,      1u);
}

// Adapter: FixtureFn takes (gb, wram, sym) but present_fullscript_fixture also
// needs rom_bytes. We bind rom_bytes via a static thread-local pointer set before
// each run. This avoids changing the FixtureFn signature.
static thread_local const std::vector<uint8_t>* g_fullscript_rom_bytes = nullptr;
static void present_fullscript_fixture_adapter(
    GB_gameboy_t* gb, uint8_t* wram, const SymCache& sym)
{
    if(!g_fullscript_rom_bytes)
        throw std::logic_error("present_fullscript_fixture_adapter: rom_bytes not bound");
    present_fullscript_fixture(gb, wram, sym, *g_fullscript_rom_bytes);
}

static void present_fullscript_damage_build_config(const SymCache& sym, CrystalRunConfig* out){
    // Entry: DoMove (0D:402C) -- directly dispatches the Present effect script.
    // Reads wPlayerMoveStruct.effect (0x7A) via GetBattleVar, selects the Present
    // effect script from MoveEffectsPointers, and runs it to completion at EndMoveEffect.
    out->entry         = sym.DoMove;
    out->sink_pcs[0]   = sym.EndMoveEffect.addr;
    out->sink_names[0] = "EndMoveEffect";
    out->num_sinks     = 1;
    out->rng_tape      = PRESENT_TAPE_FULLSCRIPT_DAMAGE;
    out->rng_tape_len  = sizeof(PRESENT_TAPE_FULLSCRIPT_DAMAGE);
    out->extra_fixture = present_fullscript_fixture_adapter;
}

// ============================================================================
// Registered moves -- adding a move requires:
//   1. Registering here with name, insn_cap, rng_tape, build_config
//   2. build_config sets entry, sinks, rng_tape, extra_fixture
//   3. Any new presentation sink must be added to SymCache
// ============================================================================
static const MoveSpec REGISTERED_MOVES[] = {
    // 114 Haze / BattleCommand_ResetStats
    // Measured: ~13 200 insn. Cap = 50 000 (~3.8x margin). No RNG.
    { 114, 114, "Haze",              50000,  nullptr, 0,
                                haze_build_config, nullptr },

    // Present sub-cases. id=217x variants; engine_id=217 for all.
    // Registration IDs: 2171=damage, 2172=heal, 2173=miss, 2174=0xFF sentinel.
    // Entering at BattleCommand_Present (not DoMove) -- no CheckHit/Critical overhead.
    // Cap = 100 000 for damage/heal (includes BattleCommand_Stab sub-calls).
    // Miss path is shortest (jp AnimateFailedMove before BattleRandom); cap = 50 000.
    { 2171, 217, "Present/damage",   100000, PRESENT_TAPE_DAMAGE,   sizeof(PRESENT_TAPE_DAMAGE),
                                present_damage_build_config, nullptr },
    { 2172, 217, "Present/heal",     100000, PRESENT_TAPE_HEAL,     sizeof(PRESENT_TAPE_HEAL),
                                present_heal_build_config, nullptr },
    { 2173, 217, "Present/miss",      50000, nullptr, 0,
                                present_miss_build_config, nullptr },
    { 2174, 217, "Present/0xFF",     100000, PRESENT_TAPE_SENTINEL, sizeof(PRESENT_TAPE_SENTINEL),
                                present_sentinel_build_config, nullptr },
    // Full-script damage case via DoMove (0D:402C):
    //   Entry: DoMove -- directly dispatches the Present effect script without
    //   UpdateMoveData overhead. Runs checkobedience through endmove.
    //   Tape: [CheckHit, Critical, PresentPower, DamVar1, DamVar2] -- 5 RNG bytes.
    //   Crystal consumes 3 bytes (CheckHit, Critical, PresentPower); DamVar bytes unused
    //   because wCurDamage=0 after the present command (fixture sets enemy HP=250/300
    //   so the present power-40 path computes 0 damage given the stat mix).
    //   Sink: EndMoveEffect. Measured: 7003 insn. Cap = 50 000 (~7x margin).
    { 2176, 217, "Present/damage-full", 50000, PRESENT_TAPE_FULLSCRIPT_DAMAGE,
                                sizeof(PRESENT_TAPE_FULLSCRIPT_DAMAGE),
                                present_fullscript_damage_build_config, nullptr },
};
static constexpr size_t NUM_REGISTERED = sizeof(REGISTERED_MOVES)/sizeof(REGISTERED_MOVES[0]);
static const MoveSpec* find_move(uint16_t id){
    for(size_t i=0;i<NUM_REGISTERED;i++) if(REGISTERED_MOVES[i].id==id) return &REGISTERED_MOVES[i];
    return nullptr;
}

// ============================================================================
// run_case -- thread-safe, self-contained
// ============================================================================
static CaseResult run_case(
    const MoveSpec& spec,
    const std::vector<uint8_t>& rom_bytes,
    const SymCache& sym,
    const EngineData& ed,
    std::atomic<bool>* stop_flag)
{
    CaseResult r{};
    r.move_id     = spec.id;
    r.move_name   = spec.name;
    r.status      = Status::HARNESS_ERROR;
    r.insn_count  = 0;
    r.poison_stable = false;
    r.stop_reason = nullptr;
    r.boundary    = nullptr;

    // Build config from the MoveSpec
    CrystalRunConfig cfg{};
    spec.build_config(sym, &cfg);
    cfg.insn_cap = spec.insn_cap;

    // If this case uses the fullscript ROM-reading fixture, bind rom_bytes to the
    // thread-local so present_fullscript_fixture_adapter can access it.
    // The binding is cleared after both runs to prevent stale state.
    struct RomBytesGuard {
        ~RomBytesGuard(){ g_fullscript_rom_bytes = nullptr; }
    } rom_bytes_guard;
    if(cfg.extra_fixture == present_fullscript_fixture_adapter)
        g_fullscript_rom_bytes = &rom_bytes;

    // Two Crystal runs with different poison bytes (same tape)
    auto cr1 = run_crystal_case(rom_bytes, sym, 0x00, cfg, stop_flag);
    auto cr2 = run_crystal_case(rom_bytes, sym, 0xA5, cfg, stop_flag);

    r.insn_count  = cr1.insn_count;
    r.stop_reason = stop_reason_str(cr1.stop_reason);
    r.boundary    = cr1.sink_name;

    if(cr1.stop_reason != StopReason::SINK_HIT){
        r.detail = std::string("Crystal run 1 failed: ")+stop_reason_str(cr1.stop_reason)
                 + " after "+std::to_string(cr1.insn_count)+" insn";
        return r;
    }
    if(cr2.stop_reason != StopReason::SINK_HIT){
        r.detail = std::string("Crystal run 2 (poison=0xA5) failed: ")
                 + stop_reason_str(cr2.stop_reason)
                 + " after "+std::to_string(cr2.insn_count)+" insn";
        return r;
    }

    r.has_crystal = true;
    r.crystal_res = cr1;
    r.poison_stable = crystal_run_results_equal(cr1, cr2);

    if(!r.poison_stable){
        std::ostringstream os;
        os << "poison instability (run1 poison=0x00 vs run2 poison=0xA5):\n";
        static const char* SN[7]={"ATK","DEF","SPD","SATK","SDEF","ACC","EVA"};
        static const char* CN[5]={"ATK","DEF","SPD","SATK","SDEF"};
        for(int i=0;i<7;i++){
            if(cr1.player_stages[i]!=cr2.player_stages[i])
                os<<"  player_stage."<<SN[i]<<" run1="<<(int)cr1.player_stages[i]<<" run2="<<(int)cr2.player_stages[i]<<"\n";
            if(cr1.enemy_stages[i]!=cr2.enemy_stages[i])
                os<<"  enemy_stage."<<SN[i]<<" run1="<<(int)cr1.enemy_stages[i]<<" run2="<<(int)cr2.enemy_stages[i]<<"\n";
        }
        for(int i=0;i<5;i++){
            if(cr1.player_stats[i]!=cr2.player_stats[i])
                os<<"  player_stat."<<CN[i]<<" run1="<<cr1.player_stats[i]<<" run2="<<cr2.player_stats[i]<<"\n";
            if(cr1.enemy_stats[i]!=cr2.enemy_stats[i])
                os<<"  enemy_stat."<<CN[i]<<" run1="<<cr1.enemy_stats[i]<<" run2="<<cr2.enemy_stats[i]<<"\n";
        }
        if(cr1.battle_anim_param!=cr2.battle_anim_param)
            os<<"  battle_anim_param: run1="<<(int)cr1.battle_anim_param<<" run2="<<(int)cr2.battle_anim_param<<"\n";
        if(cr1.cur_damage!=cr2.cur_damage)
            os<<"  cur_damage: run1="<<cr1.cur_damage<<" run2="<<cr2.cur_damage<<"\n";
        if(cr1.player_hp!=cr2.player_hp)
            os<<"  player_hp: run1="<<cr1.player_hp<<" run2="<<cr2.player_hp<<"\n";
        if(cr1.enemy_hp!=cr2.enemy_hp)
            os<<"  enemy_hp: run1="<<cr1.enemy_hp<<" run2="<<cr2.enemy_hp<<"\n";
        if(cr1.rng_bytes_consumed!=cr2.rng_bytes_consumed)
            os<<"  rng_bytes_consumed: run1="<<cr1.rng_bytes_consumed<<" run2="<<cr2.rng_bytes_consumed<<"\n";
        r.detail = os.str();
        return r; // HARNESS_ERROR
    }

    // Enginemon run with the same tape
    auto eng = run_enginemon_case(spec.engine_id, ed, spec.rng_tape, spec.rng_tape_len);
    if(!eng){
        r.detail = "Enginemon run failed (move not supported or data error)";
        return r;
    }
    r.has_engine = true;
    r.engine_res = *eng;

    // Normalize and compare
    // Crystal stages: raw-7 = delta; Enginemon: 0=neutral
    static const char* SN[7]={"ATK","DEF","SPD","SATK","SDEF","ACC","EVA"};
    static const char* CN[5]={"ATK","DEF","SPD","SATK","SDEF"};
    bool all_match = true;
    std::ostringstream diff;

    for(int i=0;i<7;i++){
        int8_t cd=int8_t(int(cr1.player_stages[i])-7), ed2=eng->player_stages[i];
        if(cd!=ed2){ all_match=false; diff<<"player_stage."<<SN[i]<<" Crystal="<<(int)cd<<" Enginemon="<<(int)ed2<<"\n"; }
    }
    for(int i=0;i<7;i++){
        int8_t cd=int8_t(int(cr1.enemy_stages[i])-7), ed2=eng->enemy_stages[i];
        if(cd!=ed2){ all_match=false; diff<<"enemy_stage."<<SN[i]<<" Crystal="<<(int)cd<<" Enginemon="<<(int)ed2<<"\n"; }
    }
    for(int i=0;i<5;i++){
        uint16_t cr=cr1.player_stats[i], em=eng->player_stats[i];
        if(cr!=em){ all_match=false; diff<<"player_stat."<<CN[i]<<" Crystal="<<cr<<" Enginemon="<<em<<"\n"; }
    }
    for(int i=0;i<5;i++){
        uint16_t cr=cr1.enemy_stats[i], em=eng->enemy_stats[i];
        if(cr!=em){ all_match=false; diff<<"enemy_stat."<<CN[i]<<" Crystal="<<cr<<" Enginemon="<<em<<"\n"; }
    }
    // HP comparison (Present may change HP)
    if(cr1.player_hp != eng->player_hp){
        all_match=false; diff<<"player_hp Crystal="<<cr1.player_hp<<" Enginemon="<<eng->player_hp<<"\n";
    }
    if(cr1.enemy_hp != eng->enemy_hp){
        all_match=false; diff<<"enemy_hp Crystal="<<cr1.enemy_hp<<" Enginemon="<<eng->enemy_hp<<"\n";
    }
    // RNG consumption must match
    if(cr1.rng_bytes_consumed != eng->rng_bytes_consumed){
        all_match=false;
        diff<<"rng_bytes_consumed Crystal="<<cr1.rng_bytes_consumed<<" Enginemon="<<eng->rng_bytes_consumed<<"\n";
    }

    r.status = all_match ? Status::MATCH : Status::ENGINEMON_MISMATCH;
    r.detail = diff.str();
    return r;
}

// ============================================================================
// Error formatting
// ============================================================================
static std::string fmt_error(int n, const char* subsystem, const char* case_name,
                              const char* cls, const std::string& reason)
{
    std::ostringstream os;
    os << "ERROR " << n << " @ " << subsystem;
    if(case_name && *case_name) os << " [" << case_name << "]";
    os << " :: " << cls << " :: " << reason;
    return os.str();
}

static std::string fmt_rng_trace(const std::vector<RngEntry>& trace){
    if(trace.empty()) return "(none)";
    std::ostringstream os;
    for(const auto& e : trace)
        os << "  [" << e.byte_index << "] 0x" << std::hex << std::setw(2) << std::setfill('0')
           << (int)e.tape_value << " @ " << e.rng_symbol
           << " PC=0x" << std::setw(4) << (int)e.intercept_pc << std::dec << "\n";
    return os.str();
}

// ============================================================================
// runner_main
// ============================================================================
int runner_main(int argc, char* argv[], RunnerConfig defaults)
{
    const char* prog = argc>0?argv[0]:"crystal_battle_diff";

    // SameBoy identity checks (compile-time via CMake -D flags)
    int startup_err = 0;
    auto startup_fail = [&](const char* subsys, const std::string& reason) -> int {
        ++startup_err;
        auto line = fmt_error(startup_err,"STARTUP",subsys,"CONFIG_ERROR",reason);
        std::cerr << line << "\n";
        return EXIT_HARNESS_ERROR;
    };
    if(std::string(GB_COMMIT) != PINNED_SAMEBOY_COMMIT)
        return startup_fail("SAMEBOY","commit mismatch: pinned="+std::string(PINNED_SAMEBOY_COMMIT)+" built="+GB_COMMIT);
    if(std::string(GB_VERSION) != PINNED_SAMEBOY_VERSION)
        return startup_fail("SAMEBOY","version mismatch: pinned="+std::string(PINNED_SAMEBOY_VERSION)+" built="+GB_VERSION);
    if(std::string(GB_DIRTY).size() > 0)
        return startup_fail("SAMEBOY","working tree has tracked modifications (re-run cmake): "+std::string(GB_DIRTY));

    // CLI parse
    std::string rom_path, sym_path;
    std::vector<uint16_t> move_ids = defaults.move_ids;
    int  jobs      = std::max(1,defaults.jobs);
    bool all_flag  = false;
    bool verbose   = defaults.verbose;
    bool help_flag = false;
    bool list_flag = false;
    int positional = 0;
    for(int i=1;i<argc;++i){
        std::string a=argv[i];
        if(a=="--help"||a=="-h")           { help_flag=true; }
        else if(a=="--list")               { list_flag=true; }
        else if(a=="--all")                { all_flag=true; }
        else if(a=="--verbose"||a=="-v")   { verbose=true; }
        else if((a=="--jobs"||a=="-j")&&i+1<argc){ jobs=std::max(1,std::stoi(argv[++i])); }
        else if(a=="--move"&&i+1<argc){
            while(i+1<argc&&argv[i+1][0]!='-') move_ids.push_back((uint16_t)std::stoi(argv[++i]));
        }
        else if(!a.empty()&&a[0]!='-'){
            if(positional==0) rom_path=a;
            else if(positional==1) sym_path=a;
            ++positional;
        }
    }

    if(help_flag){
        std::cout <<
            "crystal_battle_diff -- Crystal vs Enginemon battle differential oracle\n\n"
            "Usage:\n"
            "  " << prog << " <rom_path> <sym_path> [options]\n\n"
            "Required:\n"
            "  <rom_path>    Crystal v1.1 (UE) ROM  SHA-1: " << PINNED_ROM_SHA1 << "\n"
            "  <sym_path>    pokecrystal11.sym\n\n"
            "Options:\n"
            "  --all          Run all registered moves\n"
            "  --move <id>... Run specific move ID(s)\n"
            "  --jobs N       Parallel workers (default: 1)\n"
            "  --verbose      Print snapshots, RNG traces, stop reasons\n"
            "  --list         List registered moves and exit 0\n"
            "  --help         Show this message\n\n"
            "Quick start:\n"
            "  " << prog << " crystal.gbc pokecrystal11.sym --all --jobs 16\n"
            "  " << prog << " crystal.gbc pokecrystal11.sym --move 114\n"
            "  " << prog << " crystal.gbc pokecrystal11.sym --move 217\n\n"
            "Exit codes:\n"
            "  0  all MATCH\n"
            "  1  ENGINEMON_MISMATCH\n"
            "  2  HARNESS_ERROR (takes precedence over 1)\n"
            "  3  invalid CLI / unregistered move\n";
        return EXIT_ALL_MATCH;
    }

    if(list_flag){
        std::cout << "Registered moves (" << NUM_REGISTERED << "):\n";
        for(size_t i=0;i<NUM_REGISTERED;i++){
            std::cout << "  " << std::setw(3) << REGISTERED_MOVES[i].id
                      << "  " << REGISTERED_MOVES[i].name
                      << "  insn_cap=" << REGISTERED_MOVES[i].insn_cap;
            if(REGISTERED_MOVES[i].rng_tape){
                std::cout << "  rng_tape=[";
                for(size_t j=0;j<REGISTERED_MOVES[i].rng_tape_len;j++){
                    if(j) std::cout << ",";
                    std::cout << "0x" << std::hex << std::setw(2) << std::setfill('0')
                              << (int)REGISTERED_MOVES[i].rng_tape[j];
                }
                std::cout << std::dec << "]";
            }
            std::cout << "\n";
        }
        return EXIT_ALL_MATCH;
    }

    if(rom_path.empty()||sym_path.empty()){
        std::cerr<<"Error: rom_path and sym_path are required.\nRun '"<<prog<<" --help' for usage.\n";
        return EXIT_INVALID_ARGS;
    }

    if(all_flag){
        move_ids.clear();
        for(size_t i=0;i<NUM_REGISTERED;i++) move_ids.push_back(REGISTERED_MOVES[i].id);
    } else if(move_ids.empty()){
        std::cerr<<"Error: no moves selected. Use --all or --move <id>.\nRun '"<<prog<<" --help'.\n";
        return EXIT_INVALID_ARGS;
    }
    // Expand engine_id aliases (e.g. --move 217 → all Present subcases 2171..2174)
    {
        std::vector<uint16_t> expanded;
        for(uint16_t id : move_ids){
            if(find_move(id)){ expanded.push_back(id); continue; }
            bool found = false;
            for(size_t i=0;i<NUM_REGISTERED;i++){
                if(REGISTERED_MOVES[i].engine_id == id){ expanded.push_back(REGISTERED_MOVES[i].id); found=true; }
            }
            if(!found){
                std::cerr<<"Error: move "<<id<<" is not registered.  Registered:";
                for(size_t i=0;i<NUM_REGISTERED;i++) std::cerr<<" "<<REGISTERED_MOVES[i].id;
                std::cerr<<" (or engine IDs:";
                for(size_t i=0;i<NUM_REGISTERED;i++) std::cerr<<" "<<REGISTERED_MOVES[i].engine_id;
                std::cerr<<")\nRun '"<<prog<<" --help'.\n";
                return EXIT_INVALID_ARGS;
            }
        }
        move_ids = std::move(expanded);
    }
    std::sort(move_ids.begin(),move_ids.end());
    move_ids.erase(std::unique(move_ids.begin(),move_ids.end()),move_ids.end());

    // =========================================================================
    // Startup checks
    // =========================================================================
    std::cout << "=== Crystal Battle Differential Oracle ===\n";
    std::cout << "  SameBoy:   " << PINNED_SAMEBOY_COMMIT << "  v" << GB_VERSION << "\n";

    // 1. ROM
    std::vector<uint8_t> rom_bytes;
    {
        std::ifstream f(rom_path,std::ios::binary);
        if(!f) return startup_fail("ROM","cannot open: "+rom_path);
        rom_bytes.assign(std::istreambuf_iterator<char>(f),{});
    }
    if(rom_bytes.size()!=CRYSTAL_ROM_SIZE)
        return startup_fail("ROM","wrong size "+std::to_string(rom_bytes.size())+" expected "+std::to_string(CRYSTAL_ROM_SIZE));
    {
        std::string sha=sha1_hex(rom_bytes.data(),rom_bytes.size());
        std::cout << "  ROM SHA-1: " << sha;
        if(sha!=PINNED_ROM_SHA1){ std::cout<<" MISMATCH\n"; return startup_fail("ROM","SHA-1 mismatch: expected="+std::string(PINNED_ROM_SHA1)+" actual="+sha); }
        std::cout << " OK\n";
    }

    // 2. Sym file
    SymCache sym;
    {
        std::string sym_sha;
        std::string err=SymCache::load(sym_path,&sym,&sym_sha);
        std::cout << "  .sym SHA:  " << sym_sha;
        if(!err.empty()){ std::cout<<"\n"; return startup_fail("SYMBOLS",err+" (file: "+sym_path+")"); }
        std::cout << (std::string(PINNED_SYM_SHA1_CONST).empty() ? " (anchor check only)" : " OK") << "\n";
    }
    std::cout << "  Anchors:   OK (" << (sizeof(SYM_ANCHORS)/sizeof(SYM_ANCHORS[0])) << " checked)\n";

    // 3. Fixture addresses
    { std::string err=validate_fixture_addresses(sym); if(!err.empty()) return startup_fail("FIXTURES",err); }
    std::cout << "  Fixtures:  OK\n";

    // 4. Engine data
    auto rom_data=crystal::RomData::load(std::filesystem::path(rom_path));
    if(!rom_data) return startup_fail("ENGINE","RomData::load failed");
    const crystal::ExtractionProfile* profile=
        crystal::ProfileRegistry::instance().get_profile_by_hash(rom_data->hash());
    if(!profile) return startup_fail("ENGINE","no Enginemon profile for ROM");
    auto ed_opt=load_engine_data(*rom_data,*profile);
    if(!ed_opt) return startup_fail("ENGINE","engine data load failed");
    const EngineData& ed=*ed_opt;
    std::cout << "  Engine:    OK\n\n";

    std::cout << "Running " << move_ids.size() << " move(s)  jobs=" << jobs
              << (verbose ? "  verbose" : "") << "\n\n";
    auto t0=std::chrono::steady_clock::now();

    // =========================================================================
    // Parallel execution
    // Each future has a stop_flag so the wall-clock timeout terminates it cleanly.
    // =========================================================================
    const size_t n = move_ids.size();
    std::vector<std::future<CaseResult>> futures;
    std::vector<std::unique_ptr<std::atomic<bool>>> stop_flags;
    std::vector<bool> timed_out(n, false);
    futures.reserve(n);
    stop_flags.reserve(n);

    for(size_t i=0; i<n; ){
        size_t batch_end = std::min(i+(size_t)jobs, n);
        for(size_t j=i; j<batch_end; ++j){
            const MoveSpec* spec = find_move(move_ids[j]);
            stop_flags.push_back(std::make_unique<std::atomic<bool>>(false));
            std::atomic<bool>* sf = stop_flags.back().get();
            futures.push_back(std::async(std::launch::async,
                [&rom_bytes,&sym,&ed,spec,sf](){
                    return run_case(*spec,rom_bytes,sym,ed,sf);
                }));
        }
        for(size_t j=i; j<batch_end; ++j){
            auto status = futures[j].wait_for(std::chrono::seconds(WALL_CLOCK_TIMEOUT_S));
            if(status != std::future_status::ready){
                // Preempt: set stop flag so exec_cb forces exit on next tick.
                stop_flags[j]->store(true, std::memory_order_relaxed);
                timed_out[j] = true;
                // f.get() will now return promptly (within one GB_run() tick).
                futures[j].wait();
            }
        }
        i = batch_end;
    }

    // Collect results
    std::vector<CaseResult> results;
    results.reserve(n);
    for(size_t i=0; i<n; i++){
        CaseResult r = futures[i].get(); // always returns (stop_flag ensures it)
        if(timed_out[i] && r.status != Status::HARNESS_ERROR){
            r.status = Status::HARNESS_ERROR;
            r.stop_reason = "WALL_CLOCK_TIMEOUT";
            r.detail = "case did not complete within "+std::to_string(WALL_CLOCK_TIMEOUT_S)+"s wall-clock limit";
        }
        results.push_back(std::move(r));
    }

    // Assign deterministic error numbers (sorted order, 1-based)
    std::vector<int> error_nums(n,0);
    { int next=1; for(size_t i=0;i<n;i++) if(results[i].status!=Status::MATCH) error_nums[i]=next++; }

    std::vector<std::string> error_lines;
    std::mutex out_mutex;
    int n_match=0, n_mismatch=0, n_error=0;

    for(size_t i=0; i<n; i++){
        const auto& r = results[i];
        std::ostringstream line;

        if(r.status == Status::MATCH){
            line << "  move " << std::setw(3) << r.move_id
                 << " [" << r.move_name << "]  MATCH";
            if(verbose){
                line << "  boundary=" << (r.boundary?r.boundary:"?")
                     << "  stop=" << (r.stop_reason?r.stop_reason:"?")
                     << "  insn=" << r.insn_count
                     << "  poison-stable=yes"
                     << "  rng=" << (r.has_crystal ? r.crystal_res.rng_bytes_consumed : 0) << " bytes";
            }
            line << "\n";
            if(verbose && r.has_crystal && !r.crystal_res.rng_trace.empty()){
                line << "    rng_trace:\n" << fmt_rng_trace(r.crystal_res.rng_trace);
            }
        } else {
            const char* cls   = (r.status==Status::ENGINEMON_MISMATCH) ? "ENGINEMON_MISMATCH" : "HARNESS_ERROR";
            const char* subsys = (r.status==Status::HARNESS_ERROR) ? "ORACLE" : "BATTLE";
            int cerr_n = error_nums[i];

            if(!r.detail.empty() && r.status==Status::ENGINEMON_MISMATCH){
                std::istringstream ss(r.detail); std::string dl;
                int fn = cerr_n;
                while(std::getline(ss,dl)){
                    if(dl.empty()) continue;
                    auto el = fmt_error(fn++,subsys,r.move_name,cls,dl);
                    line << "  " << el << "\n";
                    error_lines.push_back(el);
                }
            } else {
                std::string reason = r.detail.empty()
                    ? std::string("stop=")+(r.stop_reason?r.stop_reason:"?")
                    : r.detail;
                while(!reason.empty()&&reason.back()=='\n') reason.pop_back();
                auto el = fmt_error(cerr_n,subsys,r.move_name,cls,reason);
                line << "  " << el << "\n";
                error_lines.push_back(el);
            }
            if(verbose){
                line << "    boundary=" << (r.boundary?r.boundary:"?")
                     << "  stop=" << (r.stop_reason?r.stop_reason:"?")
                     << "  insn=" << r.insn_count
                     << "  poison-stable=" << (r.poison_stable?"yes":"NO") << "\n";
                if(r.has_crystal){
                    line << "    Crystal rng_trace:\n" << fmt_rng_trace(r.crystal_res.rng_trace);
                    line << "    Crystal rng_bytes=" << r.crystal_res.rng_bytes_consumed << "\n";
                    line << "    Crystal battle_anim_param=" << (int)r.crystal_res.battle_anim_param << "\n";
                    line << "    Crystal cur_damage=" << r.crystal_res.cur_damage << "\n";
                    line << "    Crystal player_hp=" << r.crystal_res.player_hp
                         << "  enemy_hp=" << r.crystal_res.enemy_hp << "\n";
                }
                if(r.has_engine){
                    line << "    Enginemon rng_bytes=" << r.engine_res.rng_bytes_consumed << "\n";
                    line << "    Enginemon player_hp=" << r.engine_res.player_hp
                         << "  enemy_hp=" << r.engine_res.enemy_hp << "\n";
                }
            }
        }

        { std::lock_guard<std::mutex> lk(out_mutex); std::cout << line.str(); }

        switch(r.status){
        case Status::MATCH:              ++n_match;    break;
        case Status::ENGINEMON_MISMATCH: ++n_mismatch; break;
        case Status::HARNESS_ERROR:      ++n_error;    break;
        }
    }

    auto t1=std::chrono::steady_clock::now();
    int ms=(int)std::chrono::duration_cast<std::chrono::milliseconds>(t1-t0).count();

    std::cout << "\n=== Summary ===\n";
    std::cout << "  MATCH:              " << n_match    << "\n";
    std::cout << "  ENGINEMON_MISMATCH: " << n_mismatch << "\n";
    std::cout << "  HARNESS_ERROR:      " << n_error    << "\n";
    std::cout << "  Total:              " << n          << "\n";
    std::cout << "  Time:               " << ms << " ms  (" << jobs << " job" << (jobs==1?"":"s") << ")\n";
    if(!error_lines.empty()){
        std::cout << "\nErrors:\n";
        for(const auto& el:error_lines) std::cout << "  " << el << "\n";
    }

    if(n_error   >0) return EXIT_HARNESS_ERROR;
    if(n_mismatch>0) return EXIT_MISMATCH;
    return EXIT_ALL_MATCH;
}

} // namespace crystal::oracle
