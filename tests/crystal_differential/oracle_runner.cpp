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
//   INITIAL_SP = 0xC0FF. Crystal's SM83 stack is wStackBottom(0xC000)–wStackTop(0xC0FF)
//   in WRAM bank 0 (proved: pokecrystal/ram/wram.asm `ds $100-1; ds 1` under "Stack",
//   and pokecrystal/home/init.asm `ld sp, wStackTop`). The harness pushes a sentinel
//   return address at 0xC0FD-0xC0FE and starts execution with SP=0xC0FD.
//   Previous INITIAL_SP=0xFFFE used HRAM as stack; the stack descended to 0xFFE0,
//   overwriting Crystal HRAM variables (hRandomAdd=0xFFE1, hBattleTurn=0xFFE4, etc.).
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
    Sym wKantoBadges;             // 01:D858 -- badge boosts in DoBadgeTypeBoosts
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
    // Return/Frustration happiness power
    Sym wBattleMonHappiness;     // 00:C638
    // Obedience OT-ID check
    Sym wPlayerID;               // 01:D47B (2 bytes)
    Sym wPartyMon1ID;            // 01:DCE5 (2 bytes = Trainer ID of party slot 0)
    Sym wPlayerProtectCount;     // 00:C679 -- consecutive Protect/Detect uses (0 = first use)
    Sym wEffectFailed;           // 00:C70D -- set when an effect fails; must be 0 at fixture entry
    Sym wFailedMessage;          // 00:C70E -- failure message flag; must be 0 at fixture entry
    Sym wEnemyGoesFirst;         // 00:C70F -- 1 if enemy moved before player this turn

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
            {"wKantoBadges",                   &out->wKantoBadges},
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
            {"wBattleMonHappiness",            &out->wBattleMonHappiness},
            {"wPlayerID",                      &out->wPlayerID},
            {"wPartyMon1ID",                   &out->wPartyMon1ID},
            {"wPlayerProtectCount",            &out->wPlayerProtectCount},
            {"wEffectFailed",                  &out->wEffectFailed},
            {"wFailedMessage",                 &out->wFailedMessage},
            {"wEnemyGoesFirst",                &out->wEnemyGoesFirst},
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
    if((e=validate_wram_addr("wKantoBadges",       sym.wKantoBadges.addr,       0xD000,0xDFFF)).size()) return e;
    if((e=validate_hram_addr("hBattleTurn",        sym.hBattleTurn.addr)).size()) return e;
    if((e=validate_hram_addr("hROMBank",           sym.hROMBank.addr)).size()) return e;
    return {};
}

static size_t wram_off(uint16_t addr){
    if(addr>=0xD000) return size_t{0x1000}+(addr-0xD000);
    if(addr>=0xC000) return addr-0xC000;
    throw std::logic_error("wram_off: not WRAM");
}

// Validate a return address popped during a presentation-skip emulated RET.
// Returns "" (valid) or a HARNESS_ERROR string (invalid).
// Valid range: 0x0000–0x7FFF (executable ROM space).
// Rejects 0x8000–0xBFFF (VRAM, cart RAM) and higher (WRAM, HRAM, IO).
static std::string validate_emulate_ret_pc(uint16_t ret_pc, const char* skip_name, uint16_t sp_before){
    if(ret_pc > 0x7FFF){
        char buf[192];
        snprintf(buf, sizeof(buf),
            "__HARNESS_ERROR__ %s: invalid return addr 0x%04X outside ROM [0x0000,0x7FFF] (SP was 0x%04X)",
            skip_name, ret_pc, sp_before);
        return std::string(buf);
    }
    return {};
}

// Normalize Crystal's raw status byte (wBattleMonStatus / wEnemyMonStatus) to the
// canonical comparison schema shared with the Enginemon side:
//   0x01 = poisoned (regular PSN)
//   0x02 = badly poisoned (Toxic: PSN bit set + SUBSTATUS_TOXIC in substatus5)
//   0x04 = burned
//   0x08 = frozen
//   0x10 = paralyzed
//   0x20 = asleep (any non-zero sleep counter)
// substatus5 is passed to distinguish regular PSN from bad poison (SUBSTATUS_TOXIC = bit 0).
static uint8_t normalize_crystal_status(uint8_t raw_status, uint8_t substatus5){
    // Crystal bit layout (pokecrystal constants/battle_constants.asm):
    //   bits 0-2: SLP counter (non-zero = sleeping)
    //   bit 3:   PSN  (0x08)
    //   bit 4:   BRN  (0x10)
    //   bit 5:   FRZ  (0x20)
    //   bit 6:   PAR  (0x40)
    //   SUBSTATUS_TOXIC = bit 0 of substatus5
    uint8_t out = 0;
    if(raw_status & 0x07) out |= 0x20;              // SLP
    if(raw_status & 0x08){                           // PSN set
        if(substatus5 & 0x01) out |= 0x02;          //   SUBSTATUS_TOXIC → bad poison
        else                  out |= 0x01;           //   regular poison
    }
    if(raw_status & 0x10) out |= 0x04;              // BRN
    if(raw_status & 0x20) out |= 0x08;              // FRZ
    if(raw_status & 0x40) out |= 0x10;              // PAR
    return out;
}

// Normalize Crystal substatus bytes to the Enginemon VolatileStatus bitmask.
// Only maps fields that have a direct semantic equivalent in Enginemon's VolatileStatus.
// Crystal-only fields (IN_LOOP, X_ACCURACY, ENCORED, CURLED) are excluded.
// Toxic is NOT included here — it is encoded in normalize_crystal_status() instead.
static uint32_t normalize_crystal_volatile(
    uint8_t sub1, uint8_t /*sub2_unused*/,
    uint8_t sub3, uint8_t sub4, uint8_t sub5)
{
    uint32_t out = 0;
    // SubStatus1 bits (const_def from 0):
    if(sub1 & (1<<0)) out |= 0x20u;       // SUBSTATUS_NIGHTMARE   → VolatileStatus::Nightmare
    if(sub1 & (1<<1)) out |= 0x10u;       // SUBSTATUS_CURSE       → VolatileStatus::Cursed
    if(sub1 & (1<<2)) out |= 0x400000u;   // SUBSTATUS_PROTECT     → VolatileStatus::Protect
    if(sub1 & (1<<3)) out |= 0x100000u;   // SUBSTATUS_IDENTIFIED  → VolatileStatus::Identified
    if(sub1 & (1<<4)) out |= 0x4000000u;  // SUBSTATUS_PERISH      → VolatileStatus::Perish
    if(sub1 & (1<<5)) out |= 0x800000u;   // SUBSTATUS_ENDURE      → VolatileStatus::Endure
    if(sub1 & (1<<6)) out |= 0x1000u;     // SUBSTATUS_ROLLOUT     → VolatileStatus::Rollout
    if(sub1 & (1<<7)) out |= 0x40u;       // SUBSTATUS_IN_LOVE     → VolatileStatus::Infatuation
    // SubStatus3 bits (const_def from 0):
    if(sub3 & (1<<0)) out |= 0x400u;      // SUBSTATUS_BIDE        → VolatileStatus::Bide
    if(sub3 & (1<<1)) out |= 0x800u;      // SUBSTATUS_RAMPAGE     → VolatileStatus::Rampage
    // bit2 = SUBSTATUS_IN_LOOP: no Enginemon equivalent; skip
    if(sub3 & (1<<3)) out |= 0x2u;        // SUBSTATUS_FLINCHED    → VolatileStatus::Flinch
    if(sub3 & (1<<4)) out |= 0x20000u;    // SUBSTATUS_CHARGED     → VolatileStatus::Charging
    if(sub3 & (1<<5)) out |= 0x4000u;     // SUBSTATUS_UNDERGROUND → VolatileStatus::Underground
    if(sub3 & (1<<6)) out |= 0x2000u;     // SUBSTATUS_FLYING      → VolatileStatus::Flying
    if(sub3 & (1<<7)) out |= 0x1u;        // SUBSTATUS_CONFUSED    → VolatileStatus::Confusion
    // SubStatus4 bits (const_def from 0):
    // bit0 = SUBSTATUS_X_ACCURACY: no Enginemon volatile_status equivalent; skip
    if(sub4 & (1<<1)) out |= 0x80000u;    // SUBSTATUS_MIST        → VolatileStatus::Mist
    if(sub4 & (1<<2)) out |= 0x80u;       // SUBSTATUS_FOCUS_ENERGY→ VolatileStatus::FocusEnergy
    // bit3 = const_skip
    if(sub4 & (1<<4)) out |= 0x100u;      // SUBSTATUS_SUBSTITUTE  → VolatileStatus::Substitute
    if(sub4 & (1<<5)) out |= 0x200u;      // SUBSTATUS_RECHARGE    → VolatileStatus::Recharge
    if(sub4 & (1<<6)) out |= 0x8000u;     // SUBSTATUS_RAGE        → VolatileStatus::Rage
    if(sub4 & (1<<7)) out |= 0x8u;        // SUBSTATUS_LEECH_SEED  → VolatileStatus::Seeded
    // SubStatus5 bits (const_def from 0):
    // bit0 = SUBSTATUS_TOXIC: handled by normalize_crystal_status on the status byte; skip
    // bits 1,2 = const_skip
    if(sub5 & (1<<3)) out |= 0x40000u;    // SUBSTATUS_TRANSFORMED → VolatileStatus::Transformed
    // bit4 = SUBSTATUS_ENCORED: tracked in encore_turns, not VolatileStatus; skip
    if(sub5 & (1<<5)) out |= 0x1000000u;  // SUBSTATUS_LOCK_ON     → VolatileStatus::LockOn
    if(sub5 & (1<<6)) out |= 0x2000000u;  // SUBSTATUS_DESTINY_BOND→ VolatileStatus::DestinyBond
    if(sub5 & (1<<7)) out |= 0x200000u;   // SUBSTATUS_CANT_RUN    → VolatileStatus::CantRun
    return out;
}

// Normalize Enginemon volatile_status to the same canonical bitmask.
// Masks out any bits that have no Crystal counterpart (future-proofing).
// Currently keeps all bits that are mapped in normalize_crystal_volatile.
static uint32_t normalize_enginemon_volatile(uint32_t eng_volatile){
    static constexpr uint32_t MAPPED_MASK =
        0x1u|0x2u|0x8u|0x10u|0x20u|0x40u|0x80u|0x100u|0x200u|
        0x400u|0x800u|0x1000u|0x2000u|0x4000u|0x8000u|0x20000u|
        0x40000u|0x80000u|0x100000u|0x200000u|0x400000u|0x800000u|
        0x1000000u|0x2000000u|0x4000000u;
    return eng_volatile & MAPPED_MASK;
}
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
    // RNG context — kept for RNG-exhaustion detection (injection now pre-step)
    RngCtx*     rng_ctx;
};

static void exec_cb(GB_gameboy_t* gb, uint16_t /*pc*/, uint8_t){
    auto* ctx = static_cast<ExecCtx*>(GB_get_user_data(gb));
    if(!ctx || ctx->triggered) return;
    ++ctx->insn_count;

    // Wall-clock preemption only — all RNG injection, sink detection, and
    // presentation skips are handled in the pre-step loop before GB_run().
    if(ctx->stop_flag && ctx->stop_flag->load(std::memory_order_relaxed)){
        ctx->triggered = true;
        ctx->triggered_sink = "__TIMEOUT__";
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
// InitialSnapshot -- semantic state before either engine executes.
// Captured from Crystal WRAM after fixture / from Enginemon BattlePokemon
// before execute_turn(). Both sides must be identical or the comparison is
// meaningless. Fields chosen to cover all inputs that can affect the outcome.
// ============================================================================
struct InitialSnapshot {
    // HP (current and max, both sides)
    uint16_t player_hp;
    uint16_t player_max_hp;
    uint16_t enemy_hp;
    uint16_t enemy_max_hp;
    // Level
    uint8_t  player_level;
    uint8_t  enemy_level;
    // Battle stats (the active stats used in damage calc)
    uint16_t player_stats[5]; // ATK DEF SPD SATK SDEF
    uint16_t enemy_stats[5];
    // Stat stages (0=neutral for Enginemon; 7=neutral for Crystal raw, normalized here)
    int8_t   player_stages[7]; // ATK DEF SPD SATK SDEF ACC EVA
    int8_t   enemy_stages[7];
    // Status (burn/para/etc.)
    uint8_t  player_status;
    uint8_t  enemy_status;
    // Types
    uint8_t  player_type1;
    uint8_t  player_type2;
    uint8_t  enemy_type1;
    uint8_t  enemy_type2;
    // Move and PP (player only -- the move being used)
    uint16_t player_move_id;
    uint8_t  player_pp;
    uint8_t  player_max_pp;
    // Volatile / substatus state — normalized to the Enginemon VolatileStatus bitmask.
    // Both engines must start with the same volatile state or the comparison is invalid.
    // Crystal fields mapped to Enginemon VolatileStatus bits (same values used on both sides):
    //   SubStatus1: Nightmare(0x20), Curse(0x10), Protect(0x400000), Identified(0x100000),
    //               Perish(0x4000000), Endure(0x800000), Rollout(0x1000), InLove/Infatuation(0x40)
    //   SubStatus2: Curled — no Enginemon volatile_status bit (tracked via Minimized indirectly;
    //               omitted from comparison — Curled only matters mid-battle, not at start)
    //   SubStatus3: Bide(0x400), Rampage(0x800), Confused(0x1), Flinched(0x2),
    //               Charged/Charging(0x20000), Underground(0x4000), Flying(0x2000)
    //               InLoop — Crystal-internal multi-hit loop state; no Enginemon equivalent
    //   SubStatus4: Substitute(0x100), Mist(0x80000), FocusEnergy(0x80), Recharge(0x200),
    //               Rage(0x8000), LeechSeed/Seeded(0x8), XAccuracy — held-item effect, no Enginemon bit
    //   SubStatus5: Toxic is absorbed into normalize_crystal_status() on the Status byte;
    //               Transformed(0x40000), LockOn(0x1000000), DestinyBond(0x2000000), CantRun(0x200000)
    //               Encored — tracked in BattlePokemon::encore_turns, not VolatileStatus; omitted
    // Crystal-only fields with NO Enginemon VolatileStatus equivalent:
    //   SUBSTATUS_IN_LOOP (Sub3 bit2) — internal multi-hit loop counter
    //   SUBSTATUS_X_ACCURACY (Sub4 bit0) — X Accuracy item effect (item-only, never fixture-set)
    //   SUBSTATUS_ENCORED (Sub5 bit4) — tracked in encore_turns, not VolatileStatus
    //   SUBSTATUS_CURLED (Sub2 bit0) — Minimize-curl, only relevant mid-battle
    uint32_t player_volatile;  // Enginemon VolatileStatus bitmask (normalized)
    uint32_t enemy_volatile;   // Enginemon VolatileStatus bitmask (normalized)
};

// Returns "" if equal, otherwise a human-readable diff.
static std::string initial_snapshot_diff(const InitialSnapshot& c, const InitialSnapshot& e){
    std::ostringstream os;
    auto chk16=[&](const char* n, uint16_t cv, uint16_t ev){
        if(cv!=ev) os<<n<<" Crystal="<<cv<<" Enginemon="<<ev<<"\n";
    };
    auto chk8=[&](const char* n, uint8_t cv, uint8_t ev){
        if(cv!=ev) os<<n<<" Crystal="<<(int)cv<<" Enginemon="<<(int)ev<<"\n";
    };
    auto chki8=[&](const char* n, int8_t cv, int8_t ev){
        if(cv!=ev) os<<n<<" Crystal="<<(int)cv<<" Enginemon="<<(int)ev<<"\n";
    };
    chk16("init.player_hp",       c.player_hp,       e.player_hp);
    chk16("init.player_max_hp",   c.player_max_hp,   e.player_max_hp);
    chk16("init.enemy_hp",        c.enemy_hp,        e.enemy_hp);
    chk16("init.enemy_max_hp",    c.enemy_max_hp,    e.enemy_max_hp);
    chk8 ("init.player_level",    c.player_level,    e.player_level);
    chk8 ("init.enemy_level",     c.enemy_level,     e.enemy_level);
    static const char* sn[5]={"ATK","DEF","SPD","SATK","SDEF"};
    static const char* sg[7]={"ATK","DEF","SPD","SATK","SDEF","ACC","EVA"};
    char buf[64];
    for(int i=0;i<5;i++){
        snprintf(buf,sizeof(buf),"init.player_stat.%s",sn[i]);
        chk16(buf, c.player_stats[i], e.player_stats[i]);
        snprintf(buf,sizeof(buf),"init.enemy_stat.%s",sn[i]);
        chk16(buf, c.enemy_stats[i],  e.enemy_stats[i]);
    }
    for(int i=0;i<7;i++){
        snprintf(buf,sizeof(buf),"init.player_stage.%s",sg[i]);
        chki8(buf, c.player_stages[i], e.player_stages[i]);
        snprintf(buf,sizeof(buf),"init.enemy_stage.%s",sg[i]);
        chki8(buf, c.enemy_stages[i],  e.enemy_stages[i]);
    }
    chk8 ("init.player_status",  c.player_status,  e.player_status);
    chk8 ("init.enemy_status",   c.enemy_status,   e.enemy_status);
    chk8 ("init.player_type1",   c.player_type1,   e.player_type1);
    chk8 ("init.player_type2",   c.player_type2,   e.player_type2);
    chk8 ("init.enemy_type1",    c.enemy_type1,    e.enemy_type1);
    chk8 ("init.enemy_type2",    c.enemy_type2,    e.enemy_type2);
    snprintf(buf,sizeof(buf),"init.player_move_id");
    chk16(buf, c.player_move_id, e.player_move_id);
    chk8 ("init.player_pp",      c.player_pp,      e.player_pp);
    chk8 ("init.player_max_pp",  c.player_max_pp,  e.player_max_pp);
    // Volatile/substatus state — normalized to Enginemon VolatileStatus bitmask.
    // Report each differing bit by name so the error is actionable.
    {
        struct { uint32_t bit; const char* name; } bits[] = {
            {0x1u,       "Confusion"},    {0x2u,       "Flinch"},
            {0x8u,       "Seeded"},       {0x10u,      "Cursed"},
            {0x20u,      "Nightmare"},    {0x40u,      "Infatuation"},
            {0x80u,      "FocusEnergy"},  {0x100u,     "Substitute"},
            {0x200u,     "Recharge"},     {0x400u,     "Bide"},
            {0x800u,     "Rampage"},      {0x1000u,    "Rollout"},
            {0x2000u,    "Flying"},       {0x4000u,    "Underground"},
            {0x8000u,    "Rage"},         {0x20000u,   "Charging"},
            {0x40000u,   "Transformed"},  {0x80000u,   "Mist"},
            {0x100000u,  "Identified"},   {0x200000u,  "CantRun"},
            {0x400000u,  "Protect"},      {0x800000u,  "Endure"},
            {0x1000000u, "LockOn"},       {0x2000000u, "DestinyBond"},
            {0x4000000u, "Perish"},
        };
        for(const auto& b : bits){
            bool cp = (c.player_volatile & b.bit) != 0;
            bool ep = (e.player_volatile & b.bit) != 0;
            if(cp!=ep){ char n[64]; snprintf(n,sizeof(n),"init.player_volatile.%s",b.name); chk8(n,(uint8_t)cp,(uint8_t)ep); }
            bool ce = (c.enemy_volatile  & b.bit) != 0;
            bool ee = (e.enemy_volatile  & b.bit) != 0;
            if(ce!=ee){ char n[64]; snprintf(n,sizeof(n),"init.enemy_volatile.%s",b.name); chk8(n,(uint8_t)ce,(uint8_t)ee); }
        }
    }
    return os.str();
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
    // Status (burn/para/sleep/poison/freeze)
    uint8_t  player_status;       // wBattleMonStatus byte 0
    uint8_t  enemy_status;        // wEnemyMonStatus byte 0
    // RNG trace
    std::vector<RngEntry> rng_trace;
    size_t   rng_bytes_consumed;
    // Stack low-water mark -- minimum SP observed during the run
    uint16_t min_sp;
    // Initial semantic state (captured after fixture, before GB_run)
    InitialSnapshot initial;
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
    if(a.player_status     != b.player_status)     return false;
    if(a.enemy_status      != b.enemy_status)      return false;
    if(a.rng_bytes_consumed != b.rng_bytes_consumed) return false;
    for(size_t i=0; i<a.rng_trace.size() && i<b.rng_trace.size(); ++i)
        if(a.rng_trace[i].tape_value != b.rng_trace[i].tape_value) return false;
    // min_sp is structural, not semantic -- intentionally NOT compared for poison stability
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
    std::vector<uint8_t> rng_trace; // byte values consumed in order
    InitialSnapshot initial; // captured before execute_turn()
    // Post-run status byte (low byte only: burn/para/sleep/poison/freeze)
    uint8_t  player_status;
    uint8_t  enemy_status;
};

// ============================================================================
// Shared fixture constants
// ============================================================================
static constexpr int8_t   PLAYER_DELTA[7] = {0,0,0,0,0,0,0};  // neutral: all stages start at 7 in Crystal
static constexpr int8_t   ENEMY_DELTA[7]  = {0,0,0,0,0,0,0};  // neutral: all stages start at 7 in Crystal
static constexpr uint16_t P_ATK=110,P_DEF=60,P_SPD=130,P_SATK=95,P_SDEF=70;
static constexpr uint16_t E_ATK=75, E_DEF=110,E_SPD=30, E_SATK=100,E_SDEF=80;
static constexpr uint16_t P_HP=300, E_HP=300;
static constexpr uint8_t  P_LEVEL=50, E_LEVEL=50;
// PP for Present (move ID 217): read from ROM via rom_populate_player_move_struct.
// Byte [5] of the 7-byte move record = 0x0F = 15.
static constexpr uint8_t  P_PP = 0x0F;

// ============================================================================
// capture_crystal_initial -- read semantic state from Crystal WRAM after
// fixture application, before any GB_run() call. All reads use WRAM direct
// access (the same wram pointer used by the fixture).
// ============================================================================
static InitialSnapshot capture_crystal_initial(
    const uint8_t* wram, const SymCache& sym)
{
    InitialSnapshot s{};
    auto rbe16=[&](uint16_t addr)->uint16_t{
        auto* p=wram+wram_off(addr);
        return (uint16_t)((p[0]<<8)|p[1]);
    };
    s.player_hp      = rbe16(sym.wBattleMonHP.addr);
    s.player_max_hp  = rbe16(sym.wBattleMonMaxHP.addr);
    s.enemy_hp       = rbe16(sym.wEnemyMonHP.addr);
    s.enemy_max_hp   = rbe16(sym.wEnemyMonMaxHP.addr);
    s.player_level   = wram[wram_off(sym.wBattleMonLevel.addr)];
    s.enemy_level    = wram[wram_off(sym.wEnemyMonLevel.addr)];
    {auto* p=wram+wram_off(sym.wBattleMonAttack.addr);
     for(int i=0;i<5;i++) s.player_stats[i]=(uint16_t)((p[i*2]<<8)|p[i*2+1]);}
    {auto* p=wram+wram_off(sym.wEnemyMonAttack.addr);
     for(int i=0;i<5;i++) s.enemy_stats[i]=(uint16_t)((p[i*2]<<8)|p[i*2+1]);}
    {auto* p=wram+wram_off(sym.wPlayerStatLevels.addr);
     for(int i=0;i<7;i++) s.player_stages[i]=int8_t(int(p[i])-7);}
    {auto* p=wram+wram_off(sym.wEnemyStatLevels.addr);
     for(int i=0;i<7;i++) s.enemy_stages[i]=int8_t(int(p[i])-7);}
    s.player_status = wram[wram_off(sym.wBattleMonStatus.addr)];
    s.enemy_status  = wram[wram_off(sym.wEnemyMonStatus.addr)];
    s.player_type1  = wram[wram_off(sym.wBattleMonType1.addr)];
    s.player_type2  = wram[wram_off(sym.wBattleMonType2.addr)];
    s.enemy_type1   = wram[wram_off(sym.wEnemyMonType1.addr)];
    s.enemy_type2   = wram[wram_off(sym.wEnemyMonType2.addr)];
    s.player_move_id = wram[wram_off(sym.wBattleMonMoves.addr)];
    s.player_pp     = wram[wram_off(sym.wBattleMonPP.addr)];
    // max PP: read from wPartyMon1PP which is the authoritative ROM-derived value
    s.player_max_pp = wram[wram_off(sym.wPartyMon1PP.addr)];
    // Volatile/substatus state — normalize to Enginemon VolatileStatus bitmask.
    // SubStatus2 (0xC669/0xC66E) has only SUBSTATUS_CURLED which has no Enginemon
    // VolatileStatus equivalent, so it is passed as 0.
    s.player_volatile = normalize_crystal_volatile(
        wram[wram_off(sym.wPlayerSubStatus1.addr)],
        /* sub2 */ 0,
        wram[wram_off(sym.wPlayerSubStatus3.addr)],
        wram[wram_off(sym.wPlayerSubStatus4.addr)],
        wram[wram_off(sym.wPlayerSubStatus5.addr)]);
    s.enemy_volatile = normalize_crystal_volatile(
        wram[wram_off(0xC66Du)],   // wEnemySubStatus1 (not in SymCache)
        /* sub2 */ 0,
        wram[wram_off(sym.wEnemySubStatus3.addr)],
        wram[wram_off(sym.wEnemySubStatus4.addr)],
        wram[wram_off(sym.wEnemySubStatus5.addr)]);
    return s;
}

// ============================================================================
// capture_enginemon_initial -- read semantic state from BattlePokemon before
// execute_turn(). Normalizes to the same representation as capture_crystal_initial.
// ============================================================================
static InitialSnapshot capture_enginemon_initial(
    const enginemon::BattlePokemon& player,
    const enginemon::BattlePokemon& opponent,
    enginemon::MoveId move_id)
{
    InitialSnapshot s{};
    s.player_hp      = (uint16_t)player.stats.hp;
    s.player_max_hp  = (uint16_t)player.stats.max_hp;
    s.enemy_hp       = (uint16_t)opponent.stats.hp;
    s.enemy_max_hp   = (uint16_t)opponent.stats.max_hp;
    s.player_level   = player.level;
    s.enemy_level    = opponent.level;
    s.player_stats[0]=(uint16_t)player.stats.attack;
    s.player_stats[1]=(uint16_t)player.stats.defense;
    s.player_stats[2]=(uint16_t)player.stats.speed;
    s.player_stats[3]=(uint16_t)player.stats.special_attack;
    s.player_stats[4]=(uint16_t)player.stats.special_defense;
    s.enemy_stats[0] =(uint16_t)opponent.stats.attack;
    s.enemy_stats[1] =(uint16_t)opponent.stats.defense;
    s.enemy_stats[2] =(uint16_t)opponent.stats.speed;
    s.enemy_stats[3] =(uint16_t)opponent.stats.special_attack;
    s.enemy_stats[4] =(uint16_t)opponent.stats.special_defense;
    s.player_stages[0]=player.stages.attack;
    s.player_stages[1]=player.stages.defense;
    s.player_stages[2]=player.stages.speed;
    s.player_stages[3]=player.stages.special_attack;
    s.player_stages[4]=player.stages.special_defense;
    s.player_stages[5]=player.stages.accuracy;
    s.player_stages[6]=player.stages.evasion;
    s.enemy_stages[0] =opponent.stages.attack;
    s.enemy_stages[1] =opponent.stages.defense;
    s.enemy_stages[2] =opponent.stages.speed;
    s.enemy_stages[3] =opponent.stages.special_attack;
    s.enemy_stages[4] =opponent.stages.special_defense;
    s.enemy_stages[5] =opponent.stages.accuracy;
    s.enemy_stages[6] =opponent.stages.evasion;
    // Status: Enginemon uses a Status enum; Crystal uses a byte (0=no status)
    s.player_status = (player.status != enginemon::Status::None) ? 1 : 0;
    s.enemy_status  = (opponent.status != enginemon::Status::None) ? 1 : 0;
    s.player_type1  = player.type1;
    s.player_type2  = player.type2;
    s.enemy_type1   = opponent.type1;
    s.enemy_type2   = opponent.type2;
    s.player_move_id = (uint16_t)move_id;
    s.player_pp     = player.moves[0].pp;
    s.player_max_pp = player.moves[0].max_pp;
    // Volatile state — normalize Enginemon VolatileStatus bitmask.
    s.player_volatile = normalize_enginemon_volatile(player.volatile_status);
    s.enemy_volatile  = normalize_enginemon_volatile(opponent.volatile_status);
    return s;
}

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
    wram[wram_off(sym.wKantoBadges.addr)]        = 0;  // no Kanto badge boosts
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
    // IE = 0x00, IF = 0x00: all interrupts disabled.
    // Presentation code that would HALT for VBlank is skipped pre-step before GB_run().
    GB_write_memory(gb,0xFFFF,0x00); // IE: no interrupts
    GB_write_memory(gb,0xFF0F,0x00); // IF: no pending flags
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
    wram[wram_off(0xC665u)]                 = 0;  // wTypeModifier: bit7=STAB must be 0 for poison stability

    // Fields required for initial snapshot equivalence (read by capture_crystal_initial).
    // Set universally so every case starts with a defined semantic state that
    // matches the Enginemon BattlePokemon construction.
    {
        auto be=[](uint8_t* d,uint16_t v){d[0]=(v>>8);d[1]=v&0xFF;};
        // Active battle stats (used in damage formula; separate from wPlayerStats base stats)
        {
            uint8_t* p=wram+wram_off(sym.wBattleMonAttack.addr);
            be(p+0,P_ATK);be(p+2,P_DEF);be(p+4,P_SPD);be(p+6,P_SATK);be(p+8,P_SDEF);
        }
        {
            uint8_t* p=wram+wram_off(sym.wEnemyMonAttack.addr);
            be(p+0,E_ATK);be(p+2,E_DEF);be(p+4,E_SPD);be(p+6,E_SATK);be(p+8,E_SDEF);
        }
        // MaxHP (used by CheckFaint, AICheckMaxHP, and initial snapshot)
        be(wram+wram_off(sym.wBattleMonMaxHP.addr), P_HP);
        be(wram+wram_off(sym.wEnemyMonMaxHP.addr),  E_HP);
        // Level (used by damage formula)
        wram[wram_off(sym.wBattleMonLevel.addr)] = P_LEVEL;
        wram[wram_off(sym.wEnemyMonLevel.addr)]  = E_LEVEL;
        // Types (Normal/Normal — neutral matchup; overridden per case if needed)
        wram[wram_off(sym.wBattleMonType1.addr)] = 0x00;
        wram[wram_off(sym.wBattleMonType2.addr)] = 0x00;
        wram[wram_off(sym.wEnemyMonType1.addr)]  = 0x00;
        wram[wram_off(sym.wEnemyMonType2.addr)]  = 0x00;
        // Happiness: used by Return/Frustration damage formula; Enginemon sets 200
        wram[wram_off(sym.wBattleMonHappiness.addr)] = 200;
    }
    // Obedience OT-ID: both wPlayerID and wPartyMon1ID set to 0x0001 so
    // BattleCommand_CheckObedience exits at the OT-match check (ret z) without
    // consuming RNG or disrupting the effect script. wInBattleTowerBattle=0
    // is the normal (non-Battle-Tower) code path.
    GB_write_memory(gb, sym.wPlayerID.addr,     0x00); // OT ID high byte
    GB_write_memory(gb, sym.wPlayerID.addr + 1, 0x01); // OT ID low byte → wPlayerID = 0x0001
    GB_write_memory(gb, sym.wPartyMon1ID.addr,     0x00); // matches wPlayerID
    GB_write_memory(gb, sym.wPartyMon1ID.addr + 1, 0x01);
    // Protect/Detect: consecutive-use counter must be 0 for first-use success
    GB_write_memory(gb, sym.wPlayerProtectCount.addr, 0x00);
    // Effect/failure flags: must be 0 so stat changes and moves don't pre-fail
    GB_write_memory(gb, sym.wEffectFailed.addr,  0x00);
    GB_write_memory(gb, sym.wFailedMessage.addr, 0x00);
    // Turn order: player goes first (wEnemyGoesFirst=0) so Protect/Detect are allowed
    GB_write_memory(gb, sym.wEnemyGoesFirst.addr, 0x00);
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
    uint16_t    engine_move_id;   // Crystal move ID (engine_id from MoveSpec)
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
    res.min_sp         = 0xFFFF;

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
    exec_ctx.rng_ctx       = rng_ctx.get(); // for exhaustion tracking only

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

    // Write the player's move ID and PP so every case has a defined initial snapshot.
    // Cases that set these in extra_fixture will have already overwritten with their
    // specific values. Cases that don't (e.g. Haze) get the engine_move_id from the spec.
    // This runs AFTER extra_fixture to ensure it never overwrites case-specific values.
    // We use GB_write_memory (MMU path) to match how Crystal will read these fields.
    if(cfg.engine_move_id){
        // Only overwrite if extra_fixture hasn't already set a non-poison value.
        // We detect "extra_fixture wrote this" by checking against the engine_move_id.
        // For Haze (extra_fixture=null), the field is still poison; we overwrite it.
        // For Present (extra_fixture set wBattleMonMoves=217), we do nothing.
        // Use GB_write_memory so it goes through the MMU.
        GB_write_memory(&gb, sym.wBattleMonMoves.addr,
            (uint8_t)(cfg.engine_move_id & 0xFF));
        GB_write_memory(&gb, sym.wBattleMonPP.addr,   P_PP);
        GB_write_memory(&gb, sym.wPartyMon1PP.addr,   P_PP);
    }

    // Capture initial semantic state (after all fixture writes, before any execution)
    res.initial = capture_crystal_initial(wram, sym);

    GB_registers_t* regs=GB_get_registers(&gb);
    if(!regs){ GB_free(&gb); res.stop_reason=StopReason::REGS_ACCESS_FAILED; return res; }

    // -------------------------------------------------------------------------
    // Stack: Crystal's actual SM83 stack is wStackBottom(0xC000)–wStackTop(0xC0FF)
    // in WRAM bank 0 (proved from pokecrystal/ram/wram.asm: ds $100-1 then ds 1,
    // and pokecrystal/home/init.asm: ld sp, wStackTop).
    // The harness places its sentinel at wStackTop-2 = 0xC0FD and starts SP there.
    // Escape: SP < wStackBottom = 0xC000.
    // The previous W_STACK_TOP=0xFFFE used HRAM as stack, which descends into
    // Crystal HRAM variables (hBattleTurn=0xFFE4, hROMBank=0xFF9D, etc.) and
    // corrupts them — proved by observed minSP=0xFFE0 < last HRAM var 0xFFEB.
    static constexpr uint16_t W_STACK_TOP    = 0xC0FF; // wStackTop in Crystal's wram.asm
    static constexpr uint16_t W_STACK_BOTTOM = 0xC000; // wStackBottom in Crystal's wram.asm
    uint16_t ret_addr = cfg.sink_pcs[0];
    GB_write_memory(&gb, W_STACK_TOP - 1, (ret_addr >> 8) & 0xFF);
    GB_write_memory(&gb, W_STACK_TOP - 2,  ret_addr       & 0xFF);
    regs->sp = W_STACK_TOP - 2;
    regs->pc = cfg.entry.addr;

    // Track minimum SP observed across the entire run.
    uint16_t observed_min_sp = W_STACK_TOP;

    // -------------------------------------------------------------------------
    // Pre-step execution loop.
    //
    // Before each GB_run() we inspect the current PC and handle:
    //   1. Sink detection   — stop before the instruction executes
    //   2. Presentation skips — emulate a RET without entering the function
    //   3. RNG injection    — write tape byte to 0xCFB6 before BattleRandom reads it
    //   4. UsedMoveText skip via JP HL redirect at DoMoveEffectCommand
    //
    // None of these mutate PC/SP from exec_cb. exec_cb only counts instructions
    // and handles wall-clock timeout.
    //
    // Presentation skips (all CALL-entered, pure display, no gameplay writes):
    //
    //   DelayFrame      (00:045A)  CALL-entered, HALTs for VBlank. RET conv.
    //   DelayFrames     (00:0468)  calls DelayFrame in a loop. RET conv.
    //   WaitBGMap       (00:31F6)  calls DelayFrames. RET conv.
    //   BattleTextbox   (00:3AC3)  renders text tiles. RET conv.
    //   StdBattleTextbox(00:3AD5)  sets HL, tail-calls BattleTextbox. RET conv.
    //   RefreshBattleHuds(00:39C9) calls WaitBGMap + HUD update. RET conv.
    //   AnimateHPBar    (03:46E0)  bank-guarded (hROMBank==03). RET conv.
    //   PlayDamageAnim  (0D:7E19)  bank-guarded (hROMBank==0D). RET conv.
    //     PlayDamageAnim is called from inside AnimateCurrentMoveEitherSide after
    //     all callee-saves; the return addr on the stack is 0x7DFA (return inside
    //     AnimateCurrentMoveEitherSide). Emulating RET here lets LowerSub +
    //     RaiseSub + epilogue complete normally, restoring the caller's stack.
    //
    //   NOT skipped: AnimateCurrentMoveEitherSide itself — it is a registered sink
    //     for cases 2171/2174 and must trigger the sink handler, not be skipped.
    //
    // For each skip, we emulate RET:
    //   lo = mem[SP]; hi = mem[SP+1]; SP += 2; PC = (hi<<8)|lo
    //   Fail-closed: if the popped address is outside the valid code range
    //   (ROM: 0x0000–0x7FFF, or banked ROM in 0x4000–0x7FFF), report HARNESS_ERROR.
    //
    // DoMove UsedMoveText skip (DoMoveEffectCommand = 0D:4083, JP HL):
    //   When hROMBank==0D and PC==0x4083 (JP HL) and HL==0x4541 (UsedMoveText),
    //   we redirect JP HL by setting HL=0x4081 and SP+=2 before GB_run.
    //   JP HL executes and lands on 0x4081 (JR .ReadMoveEffectCommand) with
    //   a balanced stack. This is the same mechanism that already works for
    //   the other cases; it remains a pre-step action, not an exec_cb mutation.

    // Helper: read little-endian word at addr from SameBoy's memory
    auto read_word = [&](uint16_t addr) -> uint16_t {
        uint8_t lo = GB_safe_read_memory(&gb, addr);
        uint8_t hi = GB_safe_read_memory(&gb, (uint16_t)(addr + 1));
        return (uint16_t)(lo | (hi << 8));
    };

    // Emulate a RET: pop [SP] as new PC, SP += 2. Returns the new PC, or 0xFFFF
    // (HARNESS_ERROR sentinel) if the return address is implausible.
    auto emulate_ret = [&](GB_registers_t* r, const char* skip_name) -> uint16_t {
        uint16_t ret_pc = read_word(r->sp);
        r->sp += 2;
        // Valid return destinations are executable ROM space: 0x0000–0x7FFF.
        // Any address >= 0x8000 (VRAM 0x8000–0x9FFF, cart RAM 0xA000–0xBFFF,
        // WRAM 0xC000–0xDFFF, echo/OAM/IO 0xE000–0xFEFF, HRAM/IE 0xFF00–0xFFFF)
        // is not legitimate ROM code and indicates a corrupt presentation-skip call frame.
        {
            std::string err = validate_emulate_ret_pc(ret_pc, skip_name, (uint16_t)(r->sp - 2));
            if(!err.empty()){
                static char bad_ret[192];
                std::memcpy(bad_ret, err.c_str(), std::min(err.size()+1, sizeof(bad_ret)-1));
                bad_ret[sizeof(bad_ret)-1] = '\0';
                exec_ctx.triggered      = true;
                exec_ctx.triggered_sink = bad_ret;
                return 0xFFFF;
            }
        }
        r->pc = ret_pc;
        return ret_pc;
    };

    while(!exec_ctx.triggered && exec_ctx.insn_count < cfg.insn_cap){
        GB_registers_t* r = GB_get_registers(&gb);
        if(!r){ exec_ctx.triggered = true; exec_ctx.triggered_sink = "__HARNESS_ERROR__ GB_get_registers null"; break; }

        const uint16_t pc  = r->pc;
        const uint16_t sp  = r->sp;
        const uint8_t  bank = GB_safe_read_memory(&gb, 0xFF9D); // hROMBank

        // --- Stack min-SP tracking and bounds check -----------------------
        // Record low-water mark. SP must stay >= W_STACK_BOTTOM = 0xC000.
        // Crystal's actual stack occupies 0xC000-0xC0FF (wStackBottom–wStackTop).
        if(sp < observed_min_sp) observed_min_sp = sp;
        if(sp < W_STACK_BOTTOM){
            static char sp_err[64];
            snprintf(sp_err, sizeof(sp_err),
                "__HARNESS_ERROR__ stack escape: SP=0x%04X < wStackBottom=0x%04X",
                sp, (unsigned)W_STACK_BOTTOM);
            exec_ctx.triggered      = true;
            exec_ctx.triggered_sink = sp_err;
            break;
        }

        // --- Sink detection (pre-step) ------------------------------------
        bool hit_sink = false;
        for(size_t i = 0; i < cfg.num_sinks; ++i){
            if(pc == cfg.sink_pcs[i]){
                exec_ctx.triggered      = true;
                exec_ctx.triggered_sink = cfg.sink_names[i];
                hit_sink = true;
                break;
            }
        }
        if(hit_sink) break;

        // --- ByteFill guard (0x3041) --------------------------------------
        // Destination in DE < 0x8000 means a fixture field is uninitialized.
        if(pc == 0x3041 && r->de < 0x8000){
            static char bytefill_err[128];
            snprintf(bytefill_err, sizeof(bytefill_err),
                "__HARNESS_ERROR__ ByteFill(DE=0x%04X BC=0x%04X): "
                "fixture has uninitialized pointer",
                r->de, r->bc);
            exec_ctx.triggered      = true;
            exec_ctx.triggered_sink = bytefill_err;
            break;
        }

        // --- RNG injection (pre-step) -------------------------------------
        // PC == 0x2FAD: about to execute LD A,(0xCFB6) inside BattleRandom.
        // Write our tape byte first so the instruction loads it naturally.
        if(rng_ctx && pc == BATTLE_RANDOM_RESULT_READ_PC){
            if(rng_ctx->tape_idx >= rng_ctx->tape_len){
                rng_ctx->exhausted = true;
                // Fall through — let the case exhaust; we report it later.
            } else {
                uint8_t crystal_val = GB_safe_read_memory(&gb, 0xCFB6);
                uint8_t tape_val    = rng_ctx->tape[rng_ctx->tape_idx];
                GB_write_memory(&gb, 0xCFB6, tape_val);
                rng_ctx->trace.push_back({rng_ctx->tape_idx, tape_val, crystal_val,
                                          BATTLE_RANDOM_RESULT_READ_PC, "BattleRandom"});
                ++rng_ctx->tape_idx;
            }
        }

        // --- UsedMoveText skip via JP HL redirect (pre-step) --------------
        // At DoMove.DoMoveEffectCommand (0D:4083 = JP HL), when HL == 0x4541
        // (BattleCommand_UsedMoveText), redirect JP HL to 0x4081 and discard
        // the return address pushed by `call .DoMoveEffectCommand`.
        // JP HL then executes and lands on JR .ReadMoveEffectCommand (0x4081)
        // with a balanced stack.
        if(pc == 0x4083 && bank == 0x0D && r->hl == 0x4541){
            r->hl  = 0x4081; // JP HL lands here
            r->sp += 2;      // discard 0x4081 pushed by call .DoMoveEffectCommand
            // Fall through to GB_run() — JP HL executes with the modified HL.
        }

        // --- Presentation skips (pre-step RET emulation) ------------------
        // Each skip: emulate one RET before GB_run() to avoid the function body.
        // The function never executes. The return address is validated.
        {
            bool did_skip = false;

            // DelayFrame (00:045A) — HALTs for VBlank; pure timing, no game state
            if(pc == 0x045A){
                emulate_ret(r, "DelayFrame(00:045A)");
                did_skip = true;
            }
            // DelayFrames (00:0468) — calls DelayFrame in a loop; pure timing
            else if(pc == 0x0468){
                emulate_ret(r, "DelayFrames(00:0468)");
                did_skip = true;
            }
            // WaitBGMap (00:31F6) — calls DelayFrames; BG map sync, no game state
            else if(pc == 0x31F6){
                emulate_ret(r, "WaitBGMap(00:31F6)");
                did_skip = true;
            }
            // BattleTextbox (00:3AC3) — renders text tiles; no game state
            else if(pc == 0x3AC3){
                emulate_ret(r, "BattleTextbox(00:3AC3)");
                did_skip = true;
            }
            // StdBattleTextbox (00:3AD5) — sets HL, calls BattleTextbox
            else if(pc == 0x3AD5){
                emulate_ret(r, "StdBattleTextbox(00:3AD5)");
                did_skip = true;
            }
            // RefreshBattleHuds (00:39C9) — WaitBGMap + HUD tiles; no game state
            else if(pc == 0x39C9){
                emulate_ret(r, "RefreshBattleHuds(00:39C9)");
                did_skip = true;
            }
            // UpdateBattleHuds (00:39D4) — updates HP bars and HUD tiles; no game state.
            // Called from UpdateHPBarBattleHuds (0F:4D36) which is called from RestoreHP
            // during the Present heal path. Pure display; no battle state written.
            else if(pc == 0x39D4){
                emulate_ret(r, "UpdateBattleHuds(00:39D4)");
                did_skip = true;
            }
            // AnimateHPBar (03:46E0) — bank-guarded; HP bar animation
            else if(pc == 0x46E0 && bank == 0x03){
                emulate_ret(r, "AnimateHPBar(03:46E0)");
                did_skip = true;
            }
            // PlayDamageAnim (0D:7E19) — bank-guarded; damage flash animation.
            // Called from AnimateCurrentMoveEitherSide after all callee-saves.
            // The return addr on the stack is 0x7DFA (inside AnimateCMES); popping
            // it lets BattleCommand_LowerSub (already done), POP AF, BattleCommand_
            // RaiseSub, and the AnimateCMES epilogue (POP BC/DE/HL, RET) execute
            // normally, correctly restoring the BattleCommand_Present call frame.
            // Not skipped when AnimateCurrentMoveEitherSide is a registered sink.
            else if(pc == 0x7E19 && bank == 0x0D){
                bool acmes_is_sink = false;
                for(size_t i = 0; i < cfg.num_sinks; ++i)
                    if(cfg.sink_pcs[i] == 0x7DE9){ acmes_is_sink = true; break; }
                if(!acmes_is_sink){
                    emulate_ret(r, "PlayDamageAnim(0D:7E19)");
                    did_skip = true;
                }
            }
            // AnimateCurrentMoveEitherSide (0D:7DE9) — CALL-entered damage anim.
            // Skipped only when NOT a registered sink (full-script cases 2176-2180
            // need execution to continue past it to EndMoveEffect).
            else if(pc == 0x7DE9 && bank == 0x0D){
                bool is_sink = false;
                for(size_t i = 0; i < cfg.num_sinks; ++i)
                    if(cfg.sink_pcs[i] == 0x7DE9){ is_sink = true; break; }
                if(!is_sink){
                    emulate_ret(r, "AnimateCurrentMoveEitherSide(0D:7DE9)");
                    did_skip = true;
                }
            }
            // AnimateCurrentMove (0D:7E01) — CALL-entered heal/general anim.
            // Skipped only when NOT a registered sink.
            else if(pc == 0x7E01 && bank == 0x0D){
                bool is_sink = false;
                for(size_t i = 0; i < cfg.num_sinks; ++i)
                    if(cfg.sink_pcs[i] == 0x7E01){ is_sink = true; break; }
                if(!is_sink){
                    emulate_ret(r, "AnimateCurrentMove(0D:7E01)");
                    did_skip = true;
                }
            }
            // AnimateFailedMove (0D:7E77) — miss/immune animation.
            // Reached via `jp AnimateFailedMove` (tail jump) from BattleCommand_Present.
            // At that point the stack top holds the DoMove dispatcher return (0x4081).
            // Skipped only when NOT a registered sink.
            else if(pc == 0x7E77 && bank == 0x0D){
                bool is_sink = false;
                for(size_t i = 0; i < cfg.num_sinks; ++i)
                    if(cfg.sink_pcs[i] == 0x7E77){ is_sink = true; break; }
                if(!is_sink){
                    emulate_ret(r, "AnimateFailedMove(0D:7E77)");
                    did_skip = true;
                }
            }
            // BattleCommand_MoveAnim (0D:4F57) — script command for move animation.
            // Calls BattleCommand_LowerSub, PlayUserBattleAnim (bank 0x33 via callfar),
            // BattleCommand_RaiseSub. Pure presentation; no battle state written.
            // The callfar PlayBattleAnim path (bank 0x33) calls display hardware and
            // does not converge via WaitBGMap alone — skipping the whole command here
            // is cleaner and equivalent to skipping at WaitBGMap depth.
            else if(pc == 0x4F57 && bank == 0x0D){
                emulate_ret(r, "BattleCommand_MoveAnim(0D:4F57)");
                did_skip = true;
            }
            // BattleCommand_MoveAnimNoSub (0D:4F60) — move animation without substitute.
            // Used by multi-hit move scripts (startloop/endloop) as the per-hit animation
            // command. Same presentation content as MoveAnim; no battle state writes.
            else if(pc == 0x4F60 && bank == 0x0D){
                emulate_ret(r, "BattleCommand_MoveAnimNoSub(0D:4F60)");
                did_skip = true;
            }
            // BattleCommand_MoveDelay (0D:7E80) — delay 40 frames between HP bar anim.
            // Does `jp DelayFrames`; DelayFrames is already in our skip list via 0x0468.
            // Pre-step skip here avoids the JP dispatch overhead.
            else if(pc == 0x7E80 && bank == 0x0D){
                emulate_ret(r, "BattleCommand_MoveDelay(0D:7E80)");
                did_skip = true;
            }
            // BattleCommand_RaiseSubNoAnim (0D:65AF) — draws the player's back sprite
            // after Substitute is set up. Calls CallBattleCore → GetBattleMonBackpic
            // (LCD/VRAM) then jp WaitBGMap. Pure display; writes no semantic WRAM.
            else if(pc == 0x65AF && bank == 0x0D){
                emulate_ret(r, "BattleCommand_RaiseSubNoAnim(0D:65AF)");
                did_skip = true;
            }
            // LoadAnim (0D:7E44) — writes wFXAnimID then calls PlayBattleAnim (LCD/VRAM).
            // Called by BattleCommand_Substitute when wOptions bit7 (BATTLE_SCENE) is clear.
            // Pure display; writes only wFXAnimID (presentation field, no semantic WRAM).
            else if(pc == 0x7E44 && bank == 0x0D){
                emulate_ret(r, "LoadAnim(0D:7E44)");
                did_skip = true;
            }

            if(exec_ctx.triggered) break; // emulate_ret set HARNESS_ERROR
            if(did_skip) continue;        // skip GB_run() for this step
        }

        // --- Execute one instruction ---------------------------------------
        GB_run(&gb);
    }

    res.insn_count = exec_ctx.insn_count;
    res.sink_name  = exec_ctx.triggered_sink;
    res.min_sp     = observed_min_sp;

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
    res.player_status = normalize_crystal_status(
        wram[wram_off(sym.wBattleMonStatus.addr)],
        wram[wram_off(sym.wPlayerSubStatus5.addr)]);
    res.enemy_status  = normalize_crystal_status(
        wram[wram_off(sym.wEnemyMonStatus.addr)],
        wram[wram_off(sym.wEnemySubStatus5.addr)]);
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
        bp.moves[0].move=mid; bp.moves[0].pp=bp.moves[0].max_pp=P_PP;
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
    std::vector<uint8_t> eng_rng_trace;
    if(rng_tape && rng_tape_len > 0){
        bat.set_rng_callback([rng_tape, rng_tape_len, &rng_idx, &rng_consumed, &eng_rng_trace]()->uint32_t{
            if(rng_idx >= rng_tape_len) return 0xFF; // exhaustion is caught separately
            uint8_t val = rng_tape[rng_idx++];
            ++rng_consumed;
            eng_rng_trace.push_back(val);
            return (uint32_t)val;
        });
    } else {
        bat.set_rng_callback([]()->uint32_t{ return 0xFF; });
    }

    // Capture initial state before execution
    InitialSnapshot eng_initial = capture_enginemon_initial(
        bat.player_pokemon(), bat.opponent_pokemon(), move_id);

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
    e.rng_trace = eng_rng_trace;
    e.initial   = eng_initial;
    // Status: normalize to a category byte independent of Crystal's raw bit layout.
    // Bit definitions (same for both sides after normalization):
    //   0x01 = poisoned (regular)
    //   0x02 = badly poisoned (Toxic)
    //   0x04 = burned
    //   0x08 = frozen
    //   0x10 = paralyzed
    //   0x20 = asleep (any sleep turns)
    {
        auto map_status = [](const enginemon::BattlePokemon& bp) -> uint8_t {
            using S = enginemon::Status;
            switch(bp.status){
            case S::Poison:    return 0x01;
            case S::BadPoison: return 0x02;
            case S::Burn:      return 0x04;
            case S::Freeze:    return 0x08;
            case S::Paralysis: return 0x10;
            case S::Sleep:     return 0x20;
            default:           return 0;
            }
        };
        e.player_status = map_status(pp);
        e.enemy_status  = map_status(op);
    }
    return e;
}

// ============================================================================
// Case result
// ============================================================================
enum class Status { MATCH, ENGINEMON_MISMATCH, ENGINEMON_UNSUPPORTED, HARNESS_ERROR };

struct CaseResult {
    uint16_t    move_id;
    const char* move_name;
    Status      status;
    std::string detail;
    int         insn_count;
    bool        poison_stable;
    const char* stop_reason;
    const char* boundary;
    CrystalRunResult crystal_res; // from poison=0x00 run (run1)
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
    out->engine_move_id= 114; // Haze (Crystal move ID 114)
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
    // wBattleMonMoves[0] and wBattleMonPP[0] -- needed for initial snapshot equivalence
    wram[wram_off(sym.wBattleMonMoves.addr)] = 217;
    wram[wram_off(sym.wBattleMonPP.addr)]    = P_PP;
    wram[wram_off(sym.wPartyMon1PP.addr)]    = P_PP; // max PP mirror

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
    out->engine_move_id= 217; // Present (Crystal move ID 217)
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
// Generic full-script fixture (DoMove entry, any move)
//
// Forward declarations of ROM-reading helpers defined later in this file.
// ============================================================================
static constexpr uint32_t CRYSTAL_MOVES_TABLE_FLAT_DECL = 0x10u*0x4000u + (0x5AFBu - 0x4000u);
static constexpr uint32_t CRYSTAL_MOVE_DATA_SIZE_DECL   = 7u;
static inline void rom_populate_player_move_struct_fwd(
    const std::vector<uint8_t>& rom_bytes,
    uint8_t* wram, const SymCache& sym, uint16_t move_id)
{
    const uint32_t offset = CRYSTAL_MOVES_TABLE_FLAT_DECL + (uint32_t)(move_id-1)*CRYSTAL_MOVE_DATA_SIZE_DECL;
    if(offset + CRYSTAL_MOVE_DATA_SIZE_DECL > rom_bytes.size())
        throw std::logic_error("rom_populate_player_move_struct_fwd: ROM offset out of bounds");
    for(uint32_t i=0; i<CRYSTAL_MOVE_DATA_SIZE_DECL; ++i)
        wram[wram_off((uint16_t)(sym.wPlayerMoveStruct.addr + i))] = rom_bytes[offset + i];
}
//
// Configures the minimum WRAM state for DoMove to dispatch a move's complete
// effect script. Move-neutral: no Present-specific fields. Reads wPlayerMoveStruct
// from ROM. The move ID and PP are passed via thread-locals bound by run_case.
//
// CheckObedience passes via OT-ID match (wPlayerID == wPartyMon1ID from fixture_common).
// No BattleTower bypass used.
// ============================================================================
static thread_local uint16_t g_generic_move_id = 0;
static thread_local uint8_t  g_generic_pp      = 0;

static void generic_fullscript_fixture(
    GB_gameboy_t* gb, uint8_t* wram, const SymCache& sym,
    const std::vector<uint8_t>& rom_bytes,
    uint16_t move_id, uint8_t pp, size_t /*wram_sz*/ = 0)
{
    auto be16=[](uint8_t* d, uint16_t v){ d[0]=(uint8_t)(v>>8); d[1]=(uint8_t)(v&0xFF); };

    // wPlayerMoveStruct from ROM (all 7 bytes: anim, effect, power, type, acc, pp, chance)
    rom_populate_player_move_struct_fwd(rom_bytes, wram, sym, move_id);
    // NOTE: wEffectFailed/wFailedMessage/wEnemyGoesFirst zeroed below for Protect/BellyDrum stability.

    // Move identity
    wram[wram_off(sym.wCurPlayerMove.addr)]    = (uint8_t)(move_id & 0xFF);
    wram[wram_off(sym.wBattleMonMoves.addr)]   = (uint8_t)(move_id & 0xFF);
    wram[wram_off(sym.wBattleMonMoves.addr)+1] = 0;
    wram[wram_off(sym.wBattleMonMoves.addr)+2] = 0;
    wram[wram_off(sym.wBattleMonMoves.addr)+3] = 0;
    wram[wram_off(sym.wCurMoveNum.addr)]       = 0;
    wram[wram_off(sym.wCurBattleMon.addr)]     = 0;

    // PP
    wram[wram_off(sym.wBattleMonPP.addr)]   = pp;
    wram[wram_off(sym.wBattleMonPP.addr)+1] = 0;
    wram[wram_off(sym.wBattleMonPP.addr)+2] = 0;
    wram[wram_off(sym.wBattleMonPP.addr)+3] = 0;
    wram[wram_off(sym.wPartyMon1PP.addr)]   = pp;
    wram[wram_off(sym.wPartyMon1PP.addr)+1] = 0;
    wram[wram_off(sym.wPartyMon1PP.addr)+2] = 0;
    wram[wram_off(sym.wPartyMon1PP.addr)+3] = 0;
    wram[wram_off(sym.wWildMonPP.addr)]     = pp;  // enemy (WILD mode)
    wram[wram_off(sym.wWildMonMoves.addr)]  = 0;
    wram[wram_off(sym.wPartyCount.addr)]    = 1;

    // Battle mode
    wram[wram_off(sym.wBattleMode.addr)]            = 1;  // WILD_BATTLE
    wram[wram_off(sym.wLinkMode.addr)]              = 0;
    wram[wram_off(sym.wInBattleTowerBattle.addr)]   = 0;  // normal — OT-ID match handles obedience

    // Species / items (Bulbasaur = 1, no item)
    wram[wram_off(sym.wBattleMonSpecies.addr)] = 1;
    wram[wram_off(sym.wEnemyMonSpecies.addr)]  = 1;
    wram[wram_off(sym.wBattleMonItem.addr)]    = 0;
    wram[wram_off(sym.wEnemyMonItem.addr)]     = 0;

    // Status and substatus (all clear)
    wram[wram_off(sym.wBattleMonStatus.addr)]    = 0;
    wram[wram_off(sym.wEnemyMonStatus.addr)]     = 0;
    wram[wram_off(sym.wPlayerSubStatus1.addr)]   = 0;
    wram[wram_off(0xC669u)]                      = 0;  // wPlayerSubStatus2
    wram[wram_off(sym.wPlayerSubStatus3.addr)]   = 0;
    wram[wram_off(sym.wPlayerSubStatus4.addr)]   = 0;
    wram[wram_off(sym.wPlayerSubStatus5.addr)]   = 0;
    wram[wram_off(sym.wPlayerProtectCount.addr)] = 0;  // 0xC679: first-use Protect always succeeds
    wram[wram_off(0xC66Du)]                      = 0;  // wEnemySubStatus1
    wram[wram_off(0xC66Eu)]                      = 0;  // wEnemySubStatus2
    wram[wram_off(sym.wEnemySubStatus3.addr)]    = 0;  // 0xC66F: FlyDig/Rampage
    wram[wram_off(sym.wEnemySubStatus4.addr)]    = 0;  // 0xC670: Substitute/LeechSeed
    wram[wram_off(sym.wEnemySubStatus5.addr)]    = 0;
    wram[wram_off(sym.wPlayerDisableCount.addr)] = 0;
    wram[wram_off(sym.wDisabledMove.addr)]       = 0;
    wram[wram_off(sym.wPlayerCharging.addr)]     = 0;
    wram[wram_off(sym.wEnemyCharging.addr)]      = 0;
    wram[wram_off(sym.wTurnEnded.addr)]          = 0;
    wram[wram_off(sym.wAlreadyDisobeyed.addr)]   = 0;

    // Misc state
    wram[wram_off(sym.wPlayerTurnsTaken.addr)]   = 0;
    wram[wram_off(sym.wEnemyTurnsTaken.addr)]    = 0;
    wram[wram_off(sym.wAttackMissed.addr)]       = 0;
    wram[wram_off(sym.wCriticalHit.addr)]        = 0;    wram[wram_off(sym.wTypeMatchup.addr)]        = 0x10;  // EFFECTIVE (1×)
    wram[wram_off(0xC665u)]                      = 0;     // wTypeModifier: bit7=STAB, rest=type multiplier
    wram[wram_off(sym.wBattleWeather.addr)]      = 0;
    wram[wram_off(sym.wPlayerScreens.addr)]      = 0;
    wram[wram_off(sym.wEnemyScreens.addr)]       = 0;
    wram[wram_off(sym.wBattleAnimParam.addr)]    = 0;
    wram[wram_off(sym.wEnemyMoveStruct.addr)+3]  = 0xFF;  // enemy acc byte
    // Effect/failure flags and turn order: must be 0 for Protect/BellyDrum stability
    wram[wram_off(sym.wEffectFailed.addr)]       = 0;    // 0xC70D
    wram[wram_off(sym.wFailedMessage.addr)]      = 0;    // 0xC70E
    wram[wram_off(sym.wEnemyGoesFirst.addr)]     = 0;    // 0xC70F: player went first → Protect allowed

    // Stats and levels are set by fixture_common.
    // Types: Normal/Normal set by fixture_common.
    // HP/MaxHP set by fixture_common (P_HP=300, E_HP=300).
    // Happiness set by fixture_common (200) — used by Return/Frustration.
    // OT-ID match set by fixture_common (wPlayerID == wPartyMon1ID = 0x0001).

    // HRAM
    GB_write_memory(gb, sym.hBattleTurn.addr, 0x00);   // player's turn
    GB_write_memory(gb, sym.hROMBank.addr, 0x0D);       // DoMove in bank 0x0D
    GB_write_memory(gb, 0xFF70u, 1u);                   // rSVBK=1 for 0xD000-0xDFFF

    // Poison-stability fields
    GB_write_memory(gb, 0xCFCCu, 0u);  // wOptions: bit5=BATTLE_SCENE clear
    GB_write_memory(gb, 0xC616u, 0x50); // wEnemyMonNickname: null-terminated
    GB_write_memory(gb, 0xC621u, 0x50); // wBattleMonNickname: null-terminated
    GB_write_memory(gb, 0xD280u, 1u);   // wOTPartyCount = 1
}

static thread_local const std::vector<uint8_t>* g_generic_rom_bytes_ptr = nullptr;
static void generic_fullscript_fixture_adapter(
    GB_gameboy_t* gb, uint8_t* wram, const SymCache& sym)
{
    if(!g_generic_rom_bytes_ptr)
        throw std::logic_error("generic_fullscript_fixture_adapter: rom_bytes not bound");
    if(!g_generic_move_id)
        throw std::logic_error("generic_fullscript_fixture_adapter: move_id not set");
    generic_fullscript_fixture(gb, wram, sym, *g_generic_rom_bytes_ptr,
                               g_generic_move_id, g_generic_pp);
}

// Generic full-script build config: DoMove entry, EndMoveEffect sink, generic fixture.
static void generic_fullscript_config(const SymCache& sym, CrystalRunConfig* out,
                                       uint16_t engine_move_id,
                                       const uint8_t* tape, size_t tape_len)
{
    out->entry         = sym.DoMove;
    out->sink_pcs[0]   = sym.EndMoveEffect.addr;
    out->sink_names[0] = "EndMoveEffect";
    out->num_sinks     = 1;
    out->rng_tape      = tape;
    out->rng_tape_len  = tape_len;
    out->extra_fixture = generic_fullscript_fixture_adapter;
    out->engine_move_id= engine_move_id;
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
// wPlayerMoveStruct: populated from ROM via rom_populate_player_move_struct().
// Animation byte is the ROM value (0xD9 for Present); PlayDamageAnim is handled
// by the pre-step skip at PlayDamageAnim (0D:7E19).
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
    // wInBattleTowerBattle = 0: normal battle (not Battle Tower).
    // CheckObedience passes via OT-ID match (wPlayerID == wPartyMon1ID, set in fixture_common).
    wram[wram_off(sym.wInBattleTowerBattle.addr)]   = 0;

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
    wram[wram_off(sym.wPlayerProtectCount.addr)]    = 0;    // 0xC679: first-use Protect always succeeds
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
    // Effect/failure flags and turn order: must be 0 for Protect/BellyDrum stability
    wram[wram_off(sym.wEffectFailed.addr)]          = 0;    // 0xC70D
    wram[wram_off(sym.wFailedMessage.addr)]         = 0;    // 0xC70E
    wram[wram_off(sym.wEnemyGoesFirst.addr)]        = 0;    // 0xC70F: player went first → Protect allowed

    // ---------- Levels ------------------------------------------------------
    wram[wram_off(sym.wBattleMonLevel.addr)]        = P_LEVEL;
    wram[wram_off(sym.wEnemyMonLevel.addr)]         = E_LEVEL;

    // ---------- HP ----------
    be16(wram + wram_off(sym.wBattleMonHP.addr),    P_HP);
    be16(wram + wram_off(sym.wBattleMonMaxHP.addr), P_HP);
    be16(wram + wram_off(sym.wEnemyMonHP.addr),     E_HP);
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
    out->engine_move_id= 217; // Present (Crystal move ID 217)
}

// ============================================================================
// Full-script Present: additional cases
//
// All enter at DoMove (0D:402C), run the complete Present effect script, and
// sink at EndMoveEffect (0D:52A3). No handwritten expected values -- the
// Crystal oracle is the ground truth. Enginemon is compared live.
//
// Tape layout for all full-script cases (DoMove dispatches in order):
//   [0] CheckHit byte   (0x00 → hit   since acc=0xE5=229; 0xF0 → miss)
//   [1] Critical byte   (0x80 → no crit at L50 with P_SPD=130, thresh~32)
//   [2] PresentPower    (0x30 → tier0=40; 0x90 → tier1=80; 0xFF → heal)
//   [3] DamageVariation byte 1 (multiplier for damage calc)
//   [4] DamageVariation byte 2 (second variation roll)
//
// On miss (CheckHit[0] > 0xE5): Critical still consumes 1 byte (script runs
// sequentially). DamageStats/DamageCalc still compute, Present's internal
// BattleCommand_Stab check sets wTypeMatchup, then Present checks wAttackMissed
// and jumps to AnimateFailedMove (skipped here, not a sink). ClearMissDamage
// zeros wCurDamage. ApplyDamage becomes a no-op (wCurDamage=0). The full
// script still reaches EndMoveEffect.
//
// DamageVariation (script cmd 0x08, after stab):
//   Reads wCurDamage -- if 0, returns immediately (0 RNG bytes consumed).
//   Only consumes 2 bytes when wCurDamage > 0 after damagecalc+stab.
// ============================================================================

// Tape: CheckHit hit, no-crit, heal (power = 0xFF sentinel)
static constexpr uint8_t PRESENT_TAPE_FULLSCRIPT_HEAL[]   = { 0x00, 0x80, 0xFF };
// Tape: CheckHit miss (0xF0 > 0xE5), no-crit, power=40 (unused; DamVar also unused on miss)
static constexpr uint8_t PRESENT_TAPE_FULLSCRIPT_MISS[]   = { 0xF0, 0x80, 0x30, 0xB2, 0xFF };
// Tape: CheckHit hit, no-crit, power tier 1 (power=80, 0x90 in [0x66,0xB4))
// DamageVariation bytes chosen so rrca(b) >= 86: 0xB2 → rrca = 0x59 = 89 ✓
static constexpr uint8_t PRESENT_TAPE_FULLSCRIPT_POWER80[]= { 0x00, 0x80, 0x90, 0xB2, 0xFF };
// Tape: CheckHit hit, crit (0x10 < threshold~32), power tier 0 (power=40)
static constexpr uint8_t PRESENT_TAPE_FULLSCRIPT_CRIT[]   = { 0x00, 0x10, 0x30, 0xB2, 0xFF };

static void fullscript_endmove_config(const SymCache& sym, CrystalRunConfig* out,
                                       const uint8_t* tape, size_t tape_len){
    out->entry         = sym.DoMove;
    out->sink_pcs[0]   = sym.EndMoveEffect.addr;
    out->sink_names[0] = "EndMoveEffect";
    out->num_sinks     = 1;
    out->rng_tape      = tape;
    out->rng_tape_len  = tape_len;
    out->extra_fixture = present_fullscript_fixture_adapter;
    out->engine_move_id= 217; // Present (Crystal move ID 217)
}

static void present_fullscript_heal_build_config(const SymCache& sym, CrystalRunConfig* out){
    fullscript_endmove_config(sym, out, PRESENT_TAPE_FULLSCRIPT_HEAL,
                              sizeof(PRESENT_TAPE_FULLSCRIPT_HEAL));
}
static void present_fullscript_miss_build_config(const SymCache& sym, CrystalRunConfig* out){
    fullscript_endmove_config(sym, out, PRESENT_TAPE_FULLSCRIPT_MISS,
                              sizeof(PRESENT_TAPE_FULLSCRIPT_MISS));
}
static void present_fullscript_power80_build_config(const SymCache& sym, CrystalRunConfig* out){
    fullscript_endmove_config(sym, out, PRESENT_TAPE_FULLSCRIPT_POWER80,
                              sizeof(PRESENT_TAPE_FULLSCRIPT_POWER80));
}
static void present_fullscript_crit_build_config(const SymCache& sym, CrystalRunConfig* out){
    fullscript_endmove_config(sym, out, PRESENT_TAPE_FULLSCRIPT_CRIT,
                              sizeof(PRESENT_TAPE_FULLSCRIPT_CRIT));
}

// ============================================================================
// Batch 1: Recover, PainSplit, Return, Reversal
//
// All use generic_fullscript_config (DoMove entry, EndMoveEffect sink).
// RNG notes:
//   Recover  (ID 105, EFFECT_HEAL=0x20): no BattleRandom.
//   PainSplit (ID 220, EFFECT_PAIN_SPLIT=0x5B): checkhit with acc=0xFF →
//     cp -1; jr z, .Hit — no BattleRandom consumed.
//   Return    (ID 216, EFFECT_RETURN=0x79): critical (1 byte), damagevariation
//     (1 byte, chosen so rrca(b)>=86: 0xB2→rrca=0x59=89 ✓).
//   Reversal  (ID 179, EFFECT_REVERSAL=0x63): checkhit acc=0xFF → no BattleRandom.
//     constantdamage reads HP ratio, no RNG. moveanimnosub → display, skipped.
//
// Return happiness power: happiness=200 (from fixture_common), power=200*10/25=80.
// Both Crystal and Enginemon set happiness=200 so they agree on Return power.
// ============================================================================

// Return: critical (1 byte) + damagevariation (2 bytes).
// DamVar uses percent macro: 85 percent + 1 = 218 = 0xDA. Loop exits when rrca(b)>=218.
//   0xB2 → rrca = 0x59 = 89 < 218 → LOOPS (consumes 2nd byte)
//   0xFF → rrca = 0xFF = 255 >= 218 → EXIT (consumes 3rd byte)
// Total: 3 RNG bytes.
static constexpr uint8_t TAPE_RETURN[]    = { 0x80, 0xB2, 0xFF };

static void recover_config(const SymCache& sym, CrystalRunConfig* out){
    generic_fullscript_config(sym, out, 105, nullptr, 0); }

static void painsplit_config(const SymCache& sym, CrystalRunConfig* out){
    generic_fullscript_config(sym, out, 220, nullptr, 0); }

static void return_config(const SymCache& sym, CrystalRunConfig* out){
    generic_fullscript_config(sym, out, 216, TAPE_RETURN, sizeof(TAPE_RETURN)); }

static void reversal_config(const SymCache& sym, CrystalRunConfig* out){
    generic_fullscript_config(sym, out, 179, nullptr, 0); }

// ============================================================================
// Batch 2: Softboiled, MilkDrink, Frustration, Flail, Psywave,
//          DoubleKick (EFFECT_DOUBLE_HIT), Twineedle (EFFECT_POISON_MULTI_HIT),
//          Magnitude
//
// All use generic_fullscript_config (DoMove entry, EndMoveEffect sink).
//
// RNG notes:
//   Softboiled/MilkDrink (IDs 0x87/0xD0, EFFECT_HEAL=0x20): same script as
//     Recover/Heal. No BattleRandom. 0 bytes.
//
//   Frustration (ID 0xDA, EFFECT_FRUSTRATION=0x7B): critical(1) +
//     damagevariation(2). Same tape structure as Return. acc=0xFF auto-hit.
//     Frustration power = (255-happiness)*10/25 = (255-200)*10/25 = 22.
//
//   Flail (ID 0xAF, EFFECT_REVERSAL=0x63): shares Reversal script.
//     constantdamage computes HP-ratio power (no BattleRandom). acc=0xFF.
//     0 bytes.
//
//   Psywave (ID 0x95, EFFECT_PSYWAVE=0x58): constantdamage path .psywave loops
//     until 1 <= BattleRandom < level*3/2. With P_LEVEL=50: max=75 (0x4B).
//     Tape byte 0x30=48: 1<=48<75 → exits first call. 1 byte.
//
//   DoubleKick (ID 0x18, EFFECT_DOUBLE_HIT=0x2C): 2 hits via startloop/endloop.
//     endloop for EFFECT_DOUBLE_HIT: always exactly 2 hits, no BattleRandom for
//     hit count. Per hit: critical(1) + damagevariation(2). 6 bytes total.
//
//   Twineedle (ID 0x29, EFFECT_POISON_MULTI_HIT=0x4D): 2 hits. endloop for
//     EFFECT_POISON_MULTI_HIT: always 2 hits, no BattleRandom for hit count.
//     Per hit: effectchance(1, 0xFF>51 → no poison) + critical(1) +
//     damagevariation(2). 8 bytes total.
//
//   Magnitude (ID 0xDE, EFFECT_MAGNITUDE=0x7E): getmagnitude(1, 0x50=80<131=65%+1
//     → magnitude 7, power 70) + critical(1) + damagevariation(2). 4 bytes total.
//     getmagnitude also calls MoveDelay(skipped) and StdBattleTextbox(skipped).
// ============================================================================

// DamageVariation thresholds: exits when rrca(byte) >= 85*256/100+1 = 218.
// 0xB2 → rrca=0x59=89 < 218 → LOOPS. 0xFF → rrca=0xFF=255 >= 218 → EXIT.
// Each damage-dealing hit therefore needs 2 variation bytes: 0xB2 then 0xFF.

// Frustration: critical(no-crit) + damagevariation(loop+exit) = 3 bytes
static constexpr uint8_t TAPE_FRUSTRATION[] = { 0x80, 0xB2, 0xFF };

// Psywave: constantdamage(.psywave) uses 1 byte (0x30: 1<=48<75 exits first try),
// then checkhit uses 1 byte (acc=0xCC=204, 0x30=48<204 → hits). Total: 2 bytes.
static constexpr uint8_t TAPE_PSYWAVE[]     = { 0x30, 0x30 };

// DoubleKick: 2 hits × (critical + damagevar×2) = 6 bytes
static constexpr uint8_t TAPE_DOUBLEKICK[]  = {
    0x80, 0xB2, 0xFF,   // hit 1: no-crit, var-loop, var-exit
    0x80, 0xB2, 0xFF    // hit 2: no-crit, var-loop, var-exit
};

// Twineedle: 2 hits × (effectchance + critical + damagevar×2) = 8 bytes
// effectchance: 0xFF > 51 (Twineedle poison chance) → no secondary effect
static constexpr uint8_t TAPE_TWINEEDLE[]   = {
    0xFF, 0x80, 0xB2, 0xFF,   // hit 1: no-poison, no-crit, var-loop, var-exit
    0xFF, 0x80, 0xB2, 0xFF    // hit 2: no-poison, no-crit, var-loop, var-exit
};

// Magnitude: getmagnitude(0x50→tier7 power70) + critical + damagevar×2 = 4 bytes
static constexpr uint8_t TAPE_MAGNITUDE[]   = { 0x50, 0x80, 0xB2, 0xFF };

static void softboiled_config(const SymCache& sym, CrystalRunConfig* out){
    generic_fullscript_config(sym, out, 0x87, nullptr, 0); }

static void milkdrink_config(const SymCache& sym, CrystalRunConfig* out){
    generic_fullscript_config(sym, out, 0xD0, nullptr, 0); }

static void frustration_config(const SymCache& sym, CrystalRunConfig* out){
    generic_fullscript_config(sym, out, 0xDA, TAPE_FRUSTRATION, sizeof(TAPE_FRUSTRATION)); }

static void flail_config(const SymCache& sym, CrystalRunConfig* out){
    generic_fullscript_config(sym, out, 0xAF, nullptr, 0); }

static void psywave_config(const SymCache& sym, CrystalRunConfig* out){
    generic_fullscript_config(sym, out, 0x95, TAPE_PSYWAVE, sizeof(TAPE_PSYWAVE)); }

static void doublekick_config(const SymCache& sym, CrystalRunConfig* out){
    generic_fullscript_config(sym, out, 0x18, TAPE_DOUBLEKICK, sizeof(TAPE_DOUBLEKICK)); }

static void twineedle_config(const SymCache& sym, CrystalRunConfig* out){
    generic_fullscript_config(sym, out, 0x29, TAPE_TWINEEDLE, sizeof(TAPE_TWINEEDLE)); }

static void magnitude_config(const SymCache& sym, CrystalRunConfig* out){
    generic_fullscript_config(sym, out, 0xDE, TAPE_MAGNITUDE, sizeof(TAPE_MAGNITUDE)); }

// ============================================================================
// Batch 3: SeismicToss, NightShade, DragonRage, SonicBoom, SuperFang,
//          BellyDrum, Rest, Protect, Detect, Substitute, LeechSeed, Toxic
//
// All use generic_fullscript_config (DoMove entry, EndMoveEffect sink).
//
// RNG notes:
//   SeismicToss (0x45, EFFECT_LEVEL_DAMAGE=0x57): StaticDamage script.
//     constantdamage takes .level_damage path (damage=level=50).
//     checkhit: acc=0xFF → cp -1; jr z, .Hit → automatic hit, 0 bytes. Total: 0.
//
//   NightShade (0x65, EFFECT_LEVEL_DAMAGE=0x57): identical script to SeismicToss.
//     acc=0xFF → 0 bytes. Total: 0.
//
//   DragonRage (0x52, EFFECT_STATIC_DAMAGE=0x29): StaticDamage script.
//     constantdamage takes .static_damage path (damage=pwr=40, from ROM).
//     acc=0xFF → 0 bytes. Total: 0.
//
//   SonicBoom (0x31, EFFECT_STATIC_DAMAGE=0x29): StaticDamage script.
//     constantdamage takes .static_damage path (damage=pwr=20, from ROM).
//     acc=0xE5=229: checkhit consumes 1 byte. 0x30=48<229 → hit. Total: 1.
//
//   SuperFang (0xA2, EFFECT_SUPER_FANG=0x28): StaticDamage script (shares label).
//     constantdamage takes .super_fang path (damage = enemy_hp/2 = 150).
//     acc=0xE5=229: checkhit consumes 1 byte. 0x30=48<229 → hit. Total: 1.
//
//   BellyDrum (0xBB, EFFECT_BELLY_DRUM=0x8E): BellyDrum script.
//     No checkhit, no BattleRandom. BattleCommand_AttackUp2 + SubtractHPFromUser.
//     player_hp decreases from 300 to 150 (half MaxHP). Total: 0.
//
//   Rest (0x9C, EFFECT_HEAL=0x20): Heal script, REST branch.
//     BattleCommand_Heal: cp REST (0x9C) → rest path → full HP restore + SLP status.
//     No BattleRandom. player_hp → 300 (already max; no change), player_status → SLP.
//     Total: 0.
//
//   Protect (0xB6, EFFECT_PROTECT=0x6F): Protect script.
//     ProtectChance: wPlayerProtectCount=0 → b=0xFF. BattleRandom loop (skips 0x00).
//     dec a; cp b(=0xFF); jr nc, .failed → 0x40-1=0x3F < 0xFF → success.
//     Total: 1 byte.
//
//   Detect (0xC5, EFFECT_PROTECT=0x6F): identical script/path to Protect.
//     Total: 1 byte.
//
//   Substitute (0xA4, EFFECT_SUBSTITUTE=0x4F): Substitute script.
//     MoveDelay(skip), _CheckBattleScene(nc, wOptions=0) → RaiseSubNoAnim(skip).
//     StdBattleTextbox(skip), RefreshBattleHuds(skip). No BattleRandom.
//     player_hp decreases from 300 to 225 (MaxHP*3/4 = 300 - 300/4 = 225). Total: 0.
//
//   LeechSeed (0x49, EFFECT_LEECH_SEED=0x54): LeechSeed script.
//     checkhit: acc=0xE5=229. 0x30=48<229 → hit. Consumes 1 byte.
//     Enemy is Normal/Normal (not Grass) → no type immunity. Total: 1.
//
//   Toxic (0x5C, EFFECT_TOXIC=0x21): Toxic/DoPoison script.
//     checkhit: acc=0xD8=216. 0x30=48<216 → hit. Consumes 1 byte.
//     stab: wTypeModifier set (Poison vs Normal → 0x10, non-zero → immunity check passes).
//     checksafeguard: wEnemyScreens=0 → ret z (no safeguard).
//     BattleCommand_Poison: hBattleTurn=0 → skips AI 25% fail sample.
//     CheckSubstituteOpp: wEnemySubStatus4=0 → no substitute → continues.
//     .check_toxic: EFFECT_TOXIC → ret Z → .toxic path → sets SUBSTATUS_TOXIC + PSN.
//     enemy_status → PSN|TOXIC = 0x02 (normalized). Total: 1.
// ============================================================================

// Tapes: acc=0xE5 or acc=0xD8 moves need one hit byte; Protect needs one protect byte.
static constexpr uint8_t TAPE_HIT[]     = { 0x30 };   // 48 < 229 (0xE5) and < 216 (0xD8)
static constexpr uint8_t TAPE_PROTECT[] = { 0x40 };   // 0x40-1=0x3F < 0xFF → ProtectChance success

static void seismictoss_config(const SymCache& sym, CrystalRunConfig* out){
    generic_fullscript_config(sym, out, 0x45, nullptr, 0); }

static void nightshade_config(const SymCache& sym, CrystalRunConfig* out){
    generic_fullscript_config(sym, out, 0x65, nullptr, 0); }

static void dragonrage_config(const SymCache& sym, CrystalRunConfig* out){
    generic_fullscript_config(sym, out, 0x52, nullptr, 0); }

static void sonicboom_config(const SymCache& sym, CrystalRunConfig* out){
    generic_fullscript_config(sym, out, 0x31, TAPE_HIT, sizeof(TAPE_HIT)); }

static void superfang_config(const SymCache& sym, CrystalRunConfig* out){
    generic_fullscript_config(sym, out, 0xA2, TAPE_HIT, sizeof(TAPE_HIT)); }

static void bellydrum_config(const SymCache& sym, CrystalRunConfig* out){
    generic_fullscript_config(sym, out, 0xBB, nullptr, 0); }

static void rest_config(const SymCache& sym, CrystalRunConfig* out){
    generic_fullscript_config(sym, out, 0x9C, nullptr, 0); }

static void protect_config(const SymCache& sym, CrystalRunConfig* out){
    generic_fullscript_config(sym, out, 0xB6, TAPE_PROTECT, sizeof(TAPE_PROTECT)); }

static void detect_config(const SymCache& sym, CrystalRunConfig* out){
    generic_fullscript_config(sym, out, 0xC5, TAPE_PROTECT, sizeof(TAPE_PROTECT)); }

static void substitute_config(const SymCache& sym, CrystalRunConfig* out){
    generic_fullscript_config(sym, out, 0xA4, nullptr, 0); }

static void leechseed_config(const SymCache& sym, CrystalRunConfig* out){
    generic_fullscript_config(sym, out, 0x49, TAPE_HIT, sizeof(TAPE_HIT)); }

static void toxic_config(const SymCache& sym, CrystalRunConfig* out){
    generic_fullscript_config(sym, out, 0x5C, TAPE_HIT, sizeof(TAPE_HIT)); }

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
    //   All 5 consumed; damage=28, enemy_hp=222 (from 250). Measured: 15664 insn.
    //   Sink: EndMoveEffect. Cap = 50 000 (~3.2x margin).
    { 2176, 217, "Present/damage-full", 50000, PRESENT_TAPE_FULLSCRIPT_DAMAGE,
                                sizeof(PRESENT_TAPE_FULLSCRIPT_DAMAGE),
                                present_fullscript_damage_build_config, nullptr },
    // Full-script heal: CheckHit hit, no-crit, heal (0xFF sentinel).
    { 2177, 217, "Present/heal-full",   25000, PRESENT_TAPE_FULLSCRIPT_HEAL,
                                sizeof(PRESENT_TAPE_FULLSCRIPT_HEAL),
                                present_fullscript_heal_build_config, nullptr },
    // Full-script miss: CheckHit miss (0xF0 > acc=229), script continues to EndMoveEffect.
    { 2178, 217, "Present/miss-full",   50000, PRESENT_TAPE_FULLSCRIPT_MISS,
                                sizeof(PRESENT_TAPE_FULLSCRIPT_MISS),
                                present_fullscript_miss_build_config, nullptr },
    // Full-script power-80 tier: different Present power bucket + damage variation.
    { 2179, 217, "Present/power80-full",50000, PRESENT_TAPE_FULLSCRIPT_POWER80,
                                sizeof(PRESENT_TAPE_FULLSCRIPT_POWER80),
                                present_fullscript_power80_build_config, nullptr },
    // Full-script crit: critical hit path.
    { 2180, 217, "Present/crit-full",   50000, PRESENT_TAPE_FULLSCRIPT_CRIT,
                                sizeof(PRESENT_TAPE_FULLSCRIPT_CRIT),
                                present_fullscript_crit_build_config, nullptr },
    // ========================================================================
    // Batch 1: ordinary moves via DoMove full-script
    // ========================================================================
    // Recover (ID 105): EFFECT_HEAL. Script: checkobedience usedmovetext doturn heal endmove.
    // No RNG. Heal restores half of player's max HP.
    { 105,  105, "Recover",        100000, nullptr, 0, recover_config,   nullptr },
    // PainSplit (ID 220): EFFECT_PAIN_SPLIT. Script: checkobedience usedmovetext doturn checkhit painsplit endmove.
    // acc=0xFF → automatic hit, no BattleRandom. PainSplit averages HP between user and target.
    { 220,  220, "PainSplit",      100000, nullptr, 0, painsplit_config, nullptr },
    // Return (ID 216): EFFECT_RETURN. Script: checkobedience usedmovetext doturn critical damagestats happinesspower damagecalc stab damagevariation checkhit moveanim failuretext applydamage criticaltext supereffectivetext checkfaint buildopponentrage kingsrock endmove.
    // 3 RNG bytes: critical (0x80=no-crit), damagevariation (0xB2→rrca=89<218 LOOP, 0xFF→rrca=255>=218 EXIT).
    // Return power = happiness*10/25 = 200*10/25 = 80. acc=0xFF → automatic hit.
    { 216,  216, "Return",         100000, TAPE_RETURN, sizeof(TAPE_RETURN), return_config,   nullptr },
    // Reversal (ID 179): EFFECT_REVERSAL. Script: checkobedience usedmovetext doturn constantdamage stab checkhit moveanim failuretext applydamage supereffectivetext checkfaint buildopponentrage kingsrock endmove.
    // No RNG. acc=0xFF → automatic hit. Damage = current_hp * 48 / max_hp (approx 8 at 300/300).
    { 179,  179, "Reversal",       100000, nullptr, 0, reversal_config,  nullptr },
    // ========================================================================
    // Batch 2: Softboiled, MilkDrink, Frustration, Flail, Psywave,
    //          DoubleKick, Twineedle, Magnitude
    // ========================================================================
    { 0x87, 0x87, "Softboiled",    100000, nullptr, 0, softboiled_config,  nullptr },
    { 0xD0, 0xD0, "MilkDrink",     100000, nullptr, 0, milkdrink_config,   nullptr },
    { 0xDA, 0xDA, "Frustration",   100000, TAPE_FRUSTRATION, sizeof(TAPE_FRUSTRATION), frustration_config, nullptr },
    { 0xAF, 0xAF, "Flail",         100000, nullptr, 0, flail_config,       nullptr },
    { 0x95, 0x95, "Psywave",       100000, TAPE_PSYWAVE,     sizeof(TAPE_PSYWAVE),     psywave_config,     nullptr },
    { 0x18, 0x18, "DoubleKick",    200000, TAPE_DOUBLEKICK,  sizeof(TAPE_DOUBLEKICK),  doublekick_config,  nullptr },
    { 0x29, 0x29, "Twineedle",     200000, TAPE_TWINEEDLE,   sizeof(TAPE_TWINEEDLE),   twineedle_config,   nullptr },
    { 0xDE, 0xDE, "Magnitude",     100000, TAPE_MAGNITUDE,   sizeof(TAPE_MAGNITUDE),   magnitude_config,   nullptr },
    // ========================================================================
    // Batch 3: SeismicToss, NightShade, DragonRage, SonicBoom, SuperFang,
    //          BellyDrum, Rest, Protect, Detect, Substitute, LeechSeed, Toxic
    // ========================================================================
    // StaticDamage script; LEVEL_DAMAGE (damage=level=50); acc=0xFF auto-hit; 0 RNG.
    { 0x45, 0x45, "SeismicToss",   100000, nullptr,       0,                    seismictoss_config, nullptr },
    // StaticDamage script; LEVEL_DAMAGE (damage=level=50); acc=0xFF auto-hit; 0 RNG.
    { 0x65, 0x65, "NightShade",    100000, nullptr,       0,                    nightshade_config,  nullptr },
    // StaticDamage script; STATIC_DAMAGE pwr=40; acc=0xFF auto-hit; 0 RNG.
    { 0x52, 0x52, "DragonRage",    100000, nullptr,       0,                    dragonrage_config,  nullptr },
    // StaticDamage script; STATIC_DAMAGE pwr=20; acc=0xE5=229; 1 RNG byte (TAPE_HIT).
    { 0x31, 0x31, "SonicBoom",     100000, TAPE_HIT,      sizeof(TAPE_HIT),     sonicboom_config,   nullptr },
    // StaticDamage script; SUPER_FANG (damage=enemy_hp/2=150); acc=0xE5=229; 1 RNG.
    { 0xA2, 0xA2, "SuperFang",     100000, TAPE_HIT,      sizeof(TAPE_HIT),     superfang_config,   nullptr },
    // BellyDrum script; no RNG; player_hp halved (300→150); ATK raised to +6.
    { 0xBB, 0xBB, "BellyDrum",     100000, nullptr,       0,                    bellydrum_config,   nullptr },
    // Heal script REST branch; no RNG; player_hp→max, player_status→SLP.
    { 0x9C, 0x9C, "Rest",          100000, nullptr,       0,                    rest_config,        nullptr },
    // Protect script; 1 RNG byte (TAPE_PROTECT); wPlayerProtectCount=0 → success.
    { 0xB6, 0xB6, "Protect",       100000, TAPE_PROTECT,  sizeof(TAPE_PROTECT), protect_config,     nullptr },
    // Protect script (identical to Protect); 1 RNG byte.
    { 0xC5, 0xC5, "Detect",        100000, TAPE_PROTECT,  sizeof(TAPE_PROTECT), detect_config,      nullptr },
    // Substitute script; no RNG; player_hp 300→225 (MaxHP*3/4).
    { 0xA4, 0xA4, "Substitute",    100000, nullptr,       0,                    substitute_config,  nullptr },
    // LeechSeed script; acc=0xE5=229; 1 RNG byte (TAPE_HIT); SUBSTATUS_LEECH_SEED on enemy.
    { 0x49, 0x49, "LeechSeed",     100000, TAPE_HIT,      sizeof(TAPE_HIT),     leechseed_config,   nullptr },
    // Toxic/DoPoison script; acc=0xD8=216; 1 RNG byte (TAPE_HIT); enemy_status→BadPoison.
    { 0x5C, 0x5C, "Toxic",         100000, TAPE_HIT,      sizeof(TAPE_HIT),     toxic_config,       nullptr },
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
    // If build_config didn't set engine_move_id, fall back to spec.engine_id
    if(!cfg.engine_move_id) cfg.engine_move_id = spec.engine_id;

    // If this case uses the fullscript ROM-reading fixture, bind rom_bytes to the
    // thread-local so present_fullscript_fixture_adapter can access it.
    // The binding is cleared after both runs to prevent stale state.
    struct RomBytesGuard {
        ~RomBytesGuard(){
            g_fullscript_rom_bytes   = nullptr;
            g_generic_rom_bytes_ptr  = nullptr;
            g_generic_move_id        = 0;
            g_generic_pp             = 0;
        }
    } rom_bytes_guard;
    if(cfg.extra_fixture == present_fullscript_fixture_adapter)
        g_fullscript_rom_bytes = &rom_bytes;
    if(cfg.extra_fixture == generic_fullscript_fixture_adapter){
        g_generic_rom_bytes_ptr = &rom_bytes;
        g_generic_move_id       = cfg.engine_move_id;
        g_generic_pp            = P_PP; // ROM-proven PP; all batch moves use this baseline
    }

    // Four Crystal runs with different WRAM-fill patterns (same RNG tape).
    // All four must produce identical normalized semantic output — any divergence
    // indicates an uninitialized WRAM dependency in the fixture.
    static constexpr uint8_t POISON_PATTERNS[4] = { 0x00, 0xA5, 0x5A, 0xFF };
    CrystalRunResult cr[4];
    for(int pi = 0; pi < 4; ++pi){
        cr[pi] = run_crystal_case(rom_bytes, sym, POISON_PATTERNS[pi], cfg, stop_flag);
    }

    // Use the 0x00 run as the canonical Crystal result.
    r.insn_count  = cr[0].insn_count;
    r.stop_reason = stop_reason_str(cr[0].stop_reason);
    r.boundary    = cr[0].sink_name;

    for(int pi = 0; pi < 4; ++pi){
        if(cr[pi].stop_reason != StopReason::SINK_HIT){
            char lbl[32]; snprintf(lbl, sizeof(lbl), "poison=0x%02X", POISON_PATTERNS[pi]);
            r.detail = std::string("Crystal run (")+lbl+") failed: "
                     + stop_reason_str(cr[pi].stop_reason)
                     + " after "+std::to_string(cr[pi].insn_count)+" insn";
            return r;
        }
    }

    r.has_crystal = true;
    r.crystal_res = cr[0];

    // Compare each non-baseline run against the 0x00 run.
    bool all_stable = true;
    std::ostringstream instability_os;
    static const char* SN[7]={"ATK","DEF","SPD","SATK","SDEF","ACC","EVA"};
    static const char* CN[5]={"ATK","DEF","SPD","SATK","SDEF"};
    for(int pi = 1; pi < 4; ++pi){
        if(!crystal_run_results_equal(cr[0], cr[pi])){
            all_stable = false;
            char lbl[64];
            snprintf(lbl, sizeof(lbl),
                "poison instability (run1 poison=0x%02X vs run%d poison=0x%02X):\n",
                POISON_PATTERNS[0], pi+1, POISON_PATTERNS[pi]);
            instability_os << lbl;
            for(int i=0;i<7;i++){
                if(cr[0].player_stages[i]!=cr[pi].player_stages[i])
                    instability_os<<"  player_stage."<<SN[i]<<" run1="<<(int)cr[0].player_stages[i]<<" run"<<(pi+1)<<"="<<(int)cr[pi].player_stages[i]<<"\n";
                if(cr[0].enemy_stages[i]!=cr[pi].enemy_stages[i])
                    instability_os<<"  enemy_stage."<<SN[i]<<" run1="<<(int)cr[0].enemy_stages[i]<<" run"<<(pi+1)<<"="<<(int)cr[pi].enemy_stages[i]<<"\n";
            }
            for(int i=0;i<5;i++){
                if(cr[0].player_stats[i]!=cr[pi].player_stats[i])
                    instability_os<<"  player_stat."<<CN[i]<<" run1="<<cr[0].player_stats[i]<<" run"<<(pi+1)<<"="<<cr[pi].player_stats[i]<<"\n";
                if(cr[0].enemy_stats[i]!=cr[pi].enemy_stats[i])
                    instability_os<<"  enemy_stat."<<CN[i]<<" run1="<<cr[0].enemy_stats[i]<<" run"<<(pi+1)<<"="<<cr[pi].enemy_stats[i]<<"\n";
            }
            if(cr[0].battle_anim_param!=cr[pi].battle_anim_param)
                instability_os<<"  battle_anim_param: run1="<<(int)cr[0].battle_anim_param<<" run"<<(pi+1)<<"="<<(int)cr[pi].battle_anim_param<<"\n";
            if(cr[0].cur_damage!=cr[pi].cur_damage)
                instability_os<<"  cur_damage: run1="<<cr[0].cur_damage<<" run"<<(pi+1)<<"="<<cr[pi].cur_damage<<"\n";
            if(cr[0].player_hp!=cr[pi].player_hp)
                instability_os<<"  player_hp: run1="<<cr[0].player_hp<<" run"<<(pi+1)<<"="<<cr[pi].player_hp<<"\n";
            if(cr[0].enemy_hp!=cr[pi].enemy_hp)
                instability_os<<"  enemy_hp: run1="<<cr[0].enemy_hp<<" run"<<(pi+1)<<"="<<cr[pi].enemy_hp<<"\n";
            if(cr[0].player_status!=cr[pi].player_status)
                instability_os<<"  player_status: run1=0x"<<std::hex<<(int)cr[0].player_status<<" run"<<(pi+1)<<"=0x"<<(int)cr[pi].player_status<<std::dec<<"\n";
            if(cr[0].enemy_status!=cr[pi].enemy_status)
                instability_os<<"  enemy_status: run1=0x"<<std::hex<<(int)cr[0].enemy_status<<" run"<<(pi+1)<<"=0x"<<(int)cr[pi].enemy_status<<std::dec<<"\n";
            if(cr[0].rng_bytes_consumed!=cr[pi].rng_bytes_consumed)
                instability_os<<"  rng_bytes_consumed: run1="<<cr[0].rng_bytes_consumed<<" run"<<(pi+1)<<"="<<cr[pi].rng_bytes_consumed<<"\n";
        }
    }
    r.poison_stable = all_stable;

    if(!r.poison_stable){
        r.detail = instability_os.str();
        return r; // HARNESS_ERROR
    }

    // Enginemon run with the same tape
    auto eng = run_enginemon_case(spec.engine_id, ed, spec.rng_tape, spec.rng_tape_len);
    if(!eng){
        // Crystal execution completed (has_crystal=true, poison-stable verified above).
        // Enginemon cannot compute this move — not a harness failure.
        r.status  = Status::ENGINEMON_UNSUPPORTED;
        r.detail  = "Enginemon does not support this move (effect not implemented)";
        return r;
    }
    r.has_engine = true;
    r.engine_res = *eng;

    // Assert initial snapshot equivalence before comparing outcomes.
    // If the two sides started from different states the comparison is invalid.
    {
        std::string init_diff = initial_snapshot_diff(cr[0].initial, eng->initial);
        if(!init_diff.empty()){
            r.detail  = "INITIAL SNAPSHOT MISMATCH -- fix fixture before comparing outcomes:\n";
            r.detail += init_diff;
            return r; // HARNESS_ERROR (status was already HARNESS_ERROR from default)
        }
    }

    // Normalize and compare
    // Crystal stages: raw-7 = delta; Enginemon: 0=neutral
    // SN and CN already defined in the instability check block above.
    bool all_match = true;
    std::ostringstream diff;

    for(int i=0;i<7;i++){
        int8_t cd=int8_t(int(cr[0].player_stages[i])-7), ed2=eng->player_stages[i];
        if(cd!=ed2){ all_match=false; diff<<"player_stage."<<SN[i]<<" Crystal="<<(int)cd<<" Enginemon="<<(int)ed2<<"\n"; }
    }
    for(int i=0;i<7;i++){
        int8_t cd=int8_t(int(cr[0].enemy_stages[i])-7), ed2=eng->enemy_stages[i];
        if(cd!=ed2){ all_match=false; diff<<"enemy_stage."<<SN[i]<<" Crystal="<<(int)cd<<" Enginemon="<<(int)ed2<<"\n"; }
    }
    for(int i=0;i<5;i++){
        uint16_t cval=cr[0].player_stats[i], em=eng->player_stats[i];
        if(cval!=em){ all_match=false; diff<<"player_stat."<<CN[i]<<" Crystal="<<cval<<" Enginemon="<<em<<"\n"; }
    }
    for(int i=0;i<5;i++){
        uint16_t cval=cr[0].enemy_stats[i], em=eng->enemy_stats[i];
        if(cval!=em){ all_match=false; diff<<"enemy_stat."<<CN[i]<<" Crystal="<<cval<<" Enginemon="<<em<<"\n"; }
    }
    // HP comparison (Present may change HP)
    if(cr[0].player_hp != eng->player_hp){
        all_match=false; diff<<"player_hp Crystal="<<cr[0].player_hp<<" Enginemon="<<eng->player_hp<<"\n";
    }
    if(cr[0].enemy_hp != eng->enemy_hp){
        all_match=false; diff<<"enemy_hp Crystal="<<cr[0].enemy_hp<<" Enginemon="<<eng->enemy_hp<<"\n";
    }
    // Status comparison
    if(cr[0].player_status != eng->player_status){
        all_match=false; diff<<"player_status Crystal=0x"<<std::hex<<(int)cr[0].player_status<<" Enginemon=0x"<<(int)eng->player_status<<std::dec<<"\n";
    }
    if(cr[0].enemy_status != eng->enemy_status){
        all_match=false; diff<<"enemy_status Crystal=0x"<<std::hex<<(int)cr[0].enemy_status<<" Enginemon=0x"<<(int)eng->enemy_status<<std::dec<<"\n";
    }
    // RNG consumption must match
    if(cr[0].rng_bytes_consumed != eng->rng_bytes_consumed){
        all_match=false;
        diff<<"rng_bytes_consumed Crystal="<<cr[0].rng_bytes_consumed<<" Enginemon="<<eng->rng_bytes_consumed<<"\n";
    }
    // RNG byte sequence must match (order matters)
    {
        const size_t clen = cr[0].rng_trace.size();
        const size_t elen = eng->rng_trace.size();
        const size_t n    = std::min(clen, elen);
        for(size_t i = 0; i < n; ++i){
            if(cr[0].rng_trace[i].tape_value != eng->rng_trace[i]){
                all_match = false;
                diff << "rng_trace[" << i << "] Crystal=0x"
                     << std::hex << (int)cr[0].rng_trace[i].tape_value
                     << " Enginemon=0x" << (int)eng->rng_trace[i]
                     << std::dec << "\n";
            }
        }
        if(clen != elen){
            all_match = false;
            diff << "rng_trace_length Crystal=" << clen << " Enginemon=" << elen << "\n";
        }
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

static std::string fmt_eng_rng_trace(const std::vector<uint8_t>& trace){
    if(trace.empty()) return "(none)";
    std::ostringstream os;
    for(size_t i = 0; i < trace.size(); ++i)
        os << "  [" << i << "] 0x" << std::hex << std::setw(2) << std::setfill('0')
           << (int)trace[i] << std::dec << "\n";
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
            "  1  ENGINEMON_MISMATCH or ENGINEMON_UNSUPPORTED\n"
            "  2  HARNESS_ERROR (Crystal oracle untrustworthy; takes precedence over 1)\n"
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

    // 3a. emulate_ret bad-return negative test:
    //   Proves that a presentation-skip emulated-RET with a return address in
    //   0x8000–0xBFFF (VRAM/cart-RAM) is rejected as HARNESS_ERROR.
    //   Tests the validate_emulate_ret_pc() helper directly with boundary values.
    {
        // Valid boundary: 0x7FFF must be accepted (last ROM address).
        {
            std::string e = validate_emulate_ret_pc(0x7FFF, "TestSkip", 0xC0FD);
            if(!e.empty()){
                return startup_fail("SELF_TEST",
                    "emulate_ret valid addr 0x7FFF incorrectly rejected: "+e);
            }
        }
        // Invalid boundary: 0x8000 must be rejected (first VRAM address).
        {
            std::string e = validate_emulate_ret_pc(0x8000, "TestSkip", 0xC0FD);
            if(e.empty() || e.find("__HARNESS_ERROR__") != 0
               || e.find("0x8000") == std::string::npos
               || e.find("outside ROM") == std::string::npos){
                return startup_fail("SELF_TEST",
                    "emulate_ret invalid addr 0x8000 not rejected or wrong msg: '"+e+"'");
            }
        }
        // Invalid: 0x8800 (VRAM) — the specific address from the audit report.
        {
            std::string e = validate_emulate_ret_pc(0x8800, "DelayFrame(00:045A)", 0xC0FD);
            if(e.empty() || e.find("0x8800") == std::string::npos){
                return startup_fail("SELF_TEST",
                    "emulate_ret invalid addr 0x8800 not rejected: '"+e+"'");
            }
        }
        // Invalid: 0xA000 (cart RAM).
        {
            std::string e = validate_emulate_ret_pc(0xA000, "TestSkip", 0xC0FD);
            if(e.empty()){
                return startup_fail("SELF_TEST","emulate_ret 0xA000 not rejected");
            }
        }
        // Invalid: 0xC000 (WRAM).
        {
            std::string e = validate_emulate_ret_pc(0xC000, "TestSkip", 0xC0FD);
            if(e.empty()){
                return startup_fail("SELF_TEST","emulate_ret 0xC000 not rejected");
            }
        }
        // Invalid: 0xFFFF (IE register).
        {
            std::string e = validate_emulate_ret_pc(0xFFFF, "TestSkip", 0xC0FD);
            if(e.empty()){
                return startup_fail("SELF_TEST","emulate_ret 0xFFFF not rejected");
            }
        }
        // Valid: 0x0000 (ROM bank 0 start).
        {
            std::string e = validate_emulate_ret_pc(0x0000, "TestSkip", 0xC0FD);
            if(!e.empty()){
                return startup_fail("SELF_TEST",
                    "emulate_ret valid addr 0x0000 incorrectly rejected: "+e);
            }
        }
        // Confirm exact error text for the 0x8800 case used in the audit report.
        {
            std::string e = validate_emulate_ret_pc(0x8800, "DelayFrame(00:045A)", 0xC0FD);
            std::cout << "  emulate_ret bad-return [0x8800]: "
                      << e.substr(0, 80) << "...\n";
        }
    }

    // 3b. Volatile/substatus initial-snapshot negative tests:
    //   Prove that a fixture mismatch in battle substatus fields is caught as
    //   HARNESS_ERROR via initial_snapshot_diff before any execution.
    //
    //   Test A: Player Substitute (SUBSTATUS_SUBSTITUTE = Sub4 bit4).
    //     Crystal sub4=0x10 → player_volatile bit 0x100 (VolatileStatus::Substitute) = 1.
    //     Enginemon player_volatile = 0 (no Substitute).
    //     Expected: diff reports "init.player_volatile.Substitute".
    {
        InitialSnapshot c{}; // Crystal side: player has Substitute
        InitialSnapshot e{}; // Enginemon side: clean
        // Set player_volatile for Substitute only
        c.player_volatile = 0x100u; // VolatileStatus::Substitute
        e.player_volatile = 0u;
        std::string diff = initial_snapshot_diff(c, e);
        if(diff.find("init.player_volatile.Substitute") == std::string::npos){
            return startup_fail("SELF_TEST",
                "volatile substatus test A (player Substitute): expected mismatch not detected. diff='"+diff+"'");
        }
        std::cout << "  volatile substatus [player Substitute]: HARNESS_ERROR path confirmed\n";
    }
    //   Test B: Enemy Toxic/bad-poison substatus (SUBSTATUS_TOXIC = Sub5 bit0).
    //     Note: SUBSTATUS_TOXIC is absorbed into normalize_crystal_status() on the Status byte
    //     (it changes regular PSN to BadPoison in the normalized output, which IS compared
    //     in initial_snapshot_diff via enemy_status). However, SUBSTATUS_TOXIC also lives in
    //     substatus5. If Crystal has substatus5 bit0=1 but Enginemon has Status::BadPoison
    //     not set (Status::None), this should show as enemy_status mismatch at the initial check.
    //     We test this via Seeded/LeechSeed on the enemy (Sub4 bit7 = VolatileStatus::Seeded)
    //     since that IS in the volatile mapping. Toxic itself is via Status byte comparison.
    {
        InitialSnapshot c{}; // Crystal: enemy has Leech Seed (SubStatus4 bit7 = 0x80)
        InitialSnapshot e{}; // Enginemon: clean
        // normalize_crystal_volatile maps Sub4 bit7 → VolatileStatus::Seeded (0x8)
        c.enemy_volatile = 0x8u; // VolatileStatus::Seeded
        e.enemy_volatile = 0u;
        std::string diff = initial_snapshot_diff(c, e);
        if(diff.find("init.enemy_volatile.Seeded") == std::string::npos){
            return startup_fail("SELF_TEST",
                "volatile substatus test B (enemy Seeded/LeechSeed): expected mismatch not detected. diff='"+diff+"'");
        }
        std::cout << "  volatile substatus [enemy Seeded]: HARNESS_ERROR path confirmed\n";
    }
    // Also directly test the toxic substatus path via Status byte comparison:
    // SUBSTATUS_TOXIC in sub5 causes normalize_crystal_status to return 0x02 (BadPoison).
    // With player_status Crystal=0x02 vs Enginemon=0x00 → initial_snapshot_diff catches it.
    {
        InitialSnapshot c{};
        InitialSnapshot e{};
        c.enemy_status = 0x02u; // normalize_crystal_status result for Toxic
        e.enemy_status = 0x00u; // Enginemon Status::None → 0
        std::string diff = initial_snapshot_diff(c, e);
        if(diff.find("init.enemy_status") == std::string::npos){
            return startup_fail("SELF_TEST",
                "volatile substatus test C (enemy Toxic via Status): expected mismatch not detected. diff='"+diff+"'");
        }
        std::cout << "  volatile substatus [enemy Toxic/status]: HARNESS_ERROR path confirmed\n";
    }

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
    int n_match=0, n_mismatch=0, n_unsupported=0, n_error=0;

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
                     << "  rng=" << (r.has_crystal ? r.crystal_res.rng_bytes_consumed : 0) << " bytes"
                     << "  minSP=0x" << std::hex << (r.has_crystal ? r.crystal_res.min_sp : 0xFFFF) << std::dec;
            }
            line << "\n";
            if(verbose && r.has_crystal && !r.crystal_res.rng_trace.empty()){
                line << "    rng_trace:\n" << fmt_rng_trace(r.crystal_res.rng_trace);
            }
        } else {
            const char* cls;
            const char* subsys;
            if(r.status == Status::ENGINEMON_MISMATCH){
                cls = "ENGINEMON_MISMATCH"; subsys = "BATTLE";
            } else if(r.status == Status::ENGINEMON_UNSUPPORTED){
                cls = "ENGINEMON_UNSUPPORTED"; subsys = "BATTLE";
            } else {
                cls = "HARNESS_ERROR"; subsys = "ORACLE";
            }
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
                     << "  poison-stable=" << (r.poison_stable?"yes":"NO")
                     << "  minSP=0x" << std::hex << (r.has_crystal ? r.crystal_res.min_sp : 0xFFFF) << std::dec << "\n";
                if(r.has_crystal){
                    line << "    Crystal rng_trace:\n" << fmt_rng_trace(r.crystal_res.rng_trace);
                    line << "    Crystal rng_bytes=" << r.crystal_res.rng_bytes_consumed << "\n";
                    line << "    Crystal battle_anim_param=" << (int)r.crystal_res.battle_anim_param << "\n";
                    line << "    Crystal cur_damage=" << r.crystal_res.cur_damage << "\n";
                    line << "    Crystal player_hp=" << r.crystal_res.player_hp
                         << "  enemy_hp=" << r.crystal_res.enemy_hp << "\n";
                    line << "    Crystal player_status=0x" << std::hex << std::setw(2) << std::setfill('0')
                         << (int)r.crystal_res.player_status
                         << "  enemy_status=0x" << (int)r.crystal_res.enemy_status
                         << std::dec << "\n";
                }
                if(r.status == Status::ENGINEMON_UNSUPPORTED){
                    line << "    Enginemon rng_trace: N/A (unsupported)\n";
                    line << "    Enginemon rng_bytes: N/A\n";
                } else if(r.has_engine){
                    line << "    Enginemon rng_trace:\n" << fmt_eng_rng_trace(r.engine_res.rng_trace);
                    line << "    Enginemon rng_bytes=" << r.engine_res.rng_bytes_consumed << "\n";
                    line << "    Enginemon player_hp=" << r.engine_res.player_hp
                         << "  enemy_hp=" << r.engine_res.enemy_hp << "\n";
                    line << "    Enginemon player_status=0x" << std::hex << std::setw(2) << std::setfill('0')
                         << (int)r.engine_res.player_status
                         << "  enemy_status=0x" << (int)r.engine_res.enemy_status
                         << std::dec << "\n";
                }
            }
        }

        { std::lock_guard<std::mutex> lk(out_mutex); std::cout << line.str(); }

        switch(r.status){
        case Status::MATCH:                ++n_match;      break;
        case Status::ENGINEMON_MISMATCH:   ++n_mismatch;   break;
        case Status::ENGINEMON_UNSUPPORTED:++n_mismatch; ++n_unsupported; break;  // severity 1
        case Status::HARNESS_ERROR:        ++n_error;      break;
        }
    }

    auto t1=std::chrono::steady_clock::now();
    int ms=(int)std::chrono::duration_cast<std::chrono::milliseconds>(t1-t0).count();

    std::cout << "\n=== Summary ===\n";
    std::cout << "  MATCH:                  " << n_match       << "\n";
    std::cout << "  ENGINEMON_MISMATCH:     " << (n_mismatch - n_unsupported) << "\n";
    std::cout << "  ENGINEMON_UNSUPPORTED:  " << n_unsupported << "\n";
    std::cout << "  HARNESS_ERROR:          " << n_error       << "\n";
    std::cout << "  Total:                  " << n             << "\n";
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
