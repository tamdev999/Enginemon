// tests/crystal_differential/oracle_runner.cpp
//
// Crystal battle differential oracle — parallel runner implementation.
//
// ARCHITECTURE:
//   Each case runs independently:
//     run_crystal_case()  — initializes SameBoy, writes fixture WRAM, cold-calls
//                           the Crystal routine, reads semantic outputs
//     run_enginemon_case() — runs Enginemon Battle::execute_turn() on identical fixture
//     compare()           — normalizes representations, diffs
//
// ROM IMMUTABILITY:
//   rom_bytes (std::vector<uint8_t>) is loaded once, SHA-checked once, then
//   passed read-only to every worker. Each worker calls GB_load_rom_from_buffer
//   with the same bytes — SameBoy makes its own internal copy. Neither the
//   shared vector nor any SameBoy internal ROM buffer is ever modified.
//
// NO TRAMPOLINE / NO SENTINEL LOOP PATCH:
//   Cold-call entry is established by writing registers directly:
//     regs->pc = BattleCommand_ResetStats addr (in already-mapped bank 0x0D)
//     regs->sp = INITIAL_SP (safe HRAM area)
//   The stack holds the AnimateCurrentMove addr as return address — if execution
//   somehow RETs before the callback fires, it lands at AnimateCurrentMove and
//   the callback stops it there anyway.
//   There is no ROM parking loop. There is no 0x0100 trampoline patch.
//
// PRESENTATION BOUNDARY:
//   The execution callback fires when PC == AnimateCurrentMove (0d:7E01).
//   At that point all semantic WRAM writes in BattleCommand_ResetStats are complete:
//     wPlayerStatLevels / wEnemyStatLevels — reset to 7 by .Fill
//     wBattleMonAttack..SpDef (C640-C649)  — CalcBattleStats output
//     wEnemyMonAttack..SpDef (D21A-D223)   — CalcBattleStats output
//   AnimateCurrentMove itself never executes.

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

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace crystal::oracle {

// ============================================================================
// Pins
// ============================================================================
static constexpr const char* SAMEBOY_COMMIT   = "213a12ce93d66b105a113debd9396306066a7cfc";
static constexpr const char* CRYSTAL_ROM_SHA1 = "F2F52230B536214EF7C9924F483392993E226CFB";
static constexpr uint32_t    CRYSTAL_ROM_SIZE = 2097152u;  // 2 MiB

// ============================================================================
// SHA-1 (self-contained, no extra deps)
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
                if(i<20){f=(B&C)|((~B)&D);k=0x5A827999;}
                else if(i<40){f=B^C^D;k=0x6ED9EBA1;}
                else if(i<60){f=(B&C)|(B&D)|(C&D);k=0x8F1BBCDC;}
                else{f=B^C^D;k=0xCA62C1D6;}
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
            std::array<uint8_t,20>r{};
            for(int i=0;i<5;i++){r[i*4]=(h[i]>>24)&0xFF;r[i*4+1]=(h[i]>>16)&0xFF;r[i*4+2]=(h[i]>>8)&0xFF;r[i*4+3]=h[i]&0xFF;}
            return r;
        }
    } s;
    s.feed(data,len);
    auto hash=s.fin();
    char hex[41]{}; for(int i=0;i<20;i++) sprintf(hex+i*2,"%02X",hash[i]);
    return std::string(hex);
}

// ============================================================================
// Symbol lookup (reads .sym file)
// ============================================================================
struct Sym { uint8_t bank; uint16_t addr; };

static bool sym_get(const std::string& sym_path, const std::string& name, Sym* out) {
    std::ifstream f(sym_path); if(!f) return false;
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
// Sym cache — loaded once, shared read-only across all workers.
// ============================================================================
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

    static std::optional<SymCache> load(const std::string& sym_path) {
        SymCache c;
        if (!sym_get(sym_path,"BattleCommand_ResetStats",&c.BattleCommand_ResetStats) ||
            !sym_get(sym_path,"wPlayerStatLevels",       &c.wPlayerStatLevels)        ||
            !sym_get(sym_path,"wEnemyStatLevels",        &c.wEnemyStatLevels)         ||
            !sym_get(sym_path,"wPlayerStats",            &c.wPlayerStats)             ||
            !sym_get(sym_path,"wEnemyStats",             &c.wEnemyStats)              ||
            !sym_get(sym_path,"wBattleMonAttack",        &c.wBattleMonAttack)         ||
            !sym_get(sym_path,"wEnemyMonAttack",         &c.wEnemyMonAttack)          ||
            !sym_get(sym_path,"wBattleMonStatus",        &c.wBattleMonStatus)         ||
            !sym_get(sym_path,"wEnemyMonStatus",         &c.wEnemyMonStatus)          ||
            !sym_get(sym_path,"hBattleTurn",             &c.hBattleTurn)              ||
            !sym_get(sym_path,"hROMBank",                &c.hROMBank)                 ||
            !sym_get(sym_path,"wLinkMode",               &c.wLinkMode)                ||
            !sym_get(sym_path,"wInBattleTowerBattle",    &c.wInBattleTowerBattle)     ||
            !sym_get(sym_path,"wJohtoBadges",            &c.wJohtoBadges)             ||
            !sym_get(sym_path,"AnimateCurrentMove",      &c.AnimateCurrentMove))
        { return std::nullopt; }
        return c;
    }
};

// ============================================================================
// WRAM layout helper
// ============================================================================
static size_t wram_off(uint16_t addr) {
    if (addr>=0xD000) return size_t{0x1000}+(addr-0xD000);
    if (addr>=0xC000) return addr-0xC000;
    throw std::logic_error("not WRAM");
}

// ============================================================================
// SameBoy no-op callbacks (per-instance — non-capturing lambdas cast to C ptr)
// ============================================================================
static void sb_log_cb(GB_gameboy_t*, const char*, GB_log_attributes_t) {}
static uint32_t sb_rgb_cb(GB_gameboy_t*, uint8_t r, uint8_t g, uint8_t b) {
    return 0xFF000000u|((uint32_t)r<<16)|((uint32_t)g<<8)|b;
}

// ============================================================================
// Execution callback context (per SameBoy instance)
// ============================================================================
struct ExecCtx {
    uint16_t    sink_pc;     // AnimateCurrentMove addr — stop here
    bool        triggered;
    const char* sink_name;   // symbol name, for diagnostics
    int         insn_count;  // incremented each callback
};

static void exec_cb(GB_gameboy_t* gb, uint16_t pc, uint8_t) {
    auto* ctx = static_cast<ExecCtx*>(GB_get_user_data(gb));
    if (!ctx || ctx->triggered) return;
    ++ctx->insn_count;
    if (pc == ctx->sink_pc) {
        ctx->triggered = true;
    }
}

// ============================================================================
// Crystal snapshot — semantic outputs captured at the presentation boundary
// ============================================================================
struct CrystalSnapshot {
    uint8_t  player_stages[7];  // wPlayerStatLevels[0..6]  (raw: 7=neutral)
    uint8_t  enemy_stages[7];   // wEnemyStatLevels[0..6]
    uint16_t player_stats[5];   // wBattleMonAttack..SpDef  (native endian)
    uint16_t enemy_stats[5];    // wEnemyMonAttack..SpDef
    int      insn_count;        // instructions executed
    const char* boundary;       // sink symbol name that fired

    bool operator==(const CrystalSnapshot& o) const {
        for(int i=0;i<7;i++) if(player_stages[i]!=o.player_stages[i]||enemy_stages[i]!=o.enemy_stages[i]) return false;
        for(int i=0;i<5;i++) if(player_stats[i]!=o.player_stats[i]||enemy_stats[i]!=o.enemy_stats[i]) return false;
        return true;
    }
};

// ============================================================================
// Enginemon snapshot
// ============================================================================
struct EngineSnapshot {
    int8_t   player_stages[7];  // delta from neutral (0=neutral)
    int8_t   enemy_stages[7];
    uint16_t player_stats[5];   // computed stats post-Haze
    uint16_t enemy_stats[5];
};

// ============================================================================
// Base stats fixture (written to Crystal WRAM and Enginemon BattlePokemon)
// With neutral stages, CalcBattleStats: output = base_stat × 1/1 = base_stat
// These values are deliberately not round numbers to distinguish from coincidence.
// ============================================================================
static constexpr struct { uint16_t atk,def,spd,satk,sdef; } PLAYER_BASE = {110,60,130,95,70};
static constexpr struct { uint16_t atk,def,spd,satk,sdef; } ENEMY_BASE  = {75,110,30,100,80};

// Pre-Haze stage deltas: deliberately non-zero to prove Haze resets them.
static constexpr int8_t PLAYER_DELTA[7] = {+2,-3,+1,-1,+3,+4,-2};
static constexpr int8_t ENEMY_DELTA[7]  = {-4,+6,-2,+3,-1,-3,+5};

// ============================================================================
// run_crystal_case — runs the Crystal routine in a dedicated SameBoy instance.
//
// ROM IMMUTABILITY: rom_bytes are passed unmodified to GB_load_rom_from_buffer.
// No ROM bytes are accessed or patched after that call.
// Cold-call is achieved by direct register/bank state:
//   regs->pc  = BattleCommand_ResetStats (bank 0x0D mapped via MBC write)
//   regs->sp  = INITIAL_SP (safe HRAM area; AnimateCurrentMove addr on stack)
//   hROMBank  = 0x0D (for RST $08 BankSwitch restore path)
//   GB_IO_BANK (0xFF50) = 1 (marks boot ROM finished so RST vectors read game ROM)
// ============================================================================
static std::optional<CrystalSnapshot> run_crystal_case(
    const std::vector<uint8_t>& rom_bytes,
    const SymCache& sym,
    uint8_t poison)
{
    // SameBoy is not thread-safe. Each call owns a dedicated GB_gameboy_t on
    // the stack. No shared SameBoy state.
    GB_gameboy_t gb;
    if (!GB_init(&gb, GB_MODEL_CGB_E)) return std::nullopt;
    // Pixels buffer: small, per-call on stack is fine (160×144×4 = 92160 bytes).
    // Use a static thread_local to avoid repeated stack allocation.
    static thread_local uint32_t tl_pixels[160*144];
    GB_set_log_callback(&gb, sb_log_cb);
    GB_set_rgb_encode_callback(&gb, sb_rgb_cb);
    GB_set_pixels_output(&gb, tl_pixels);
    GB_set_rendering_disabled(&gb, true);
    GB_set_turbo_mode(&gb, true, true);

    // Presentation-sink allowlist. AnimateCurrentMove is the only registered
    // boundary for BattleCommand_ResetStats / Haze. All semantic outputs are
    // committed before execution reaches this address. Future move cases must
    // register their own sinks explicitly.
    ExecCtx ctx;
    ctx.sink_pc    = sym.AnimateCurrentMove.addr;  // 0x7E01
    ctx.sink_name  = "AnimateCurrentMove";
    ctx.triggered  = false;
    ctx.insn_count = 0;
    GB_set_user_data(&gb, &ctx);
    GB_set_execution_callback(&gb, exec_cb);

    // Load ROM — EXACT BYTES, UNMODIFIED.
    // rom_bytes is the SHA-verified vector read from disk. It is never written.
    // SameBoy malloc-copies it internally. We hold no pointer to gb->rom.
    GB_load_rom_from_buffer(&gb, rom_bytes.data(), rom_bytes.size());

    // Mark boot ROM as finished.
    // Required so RST $08 (BankSwitch at 0x0008) and RST $10 (SwitchROM at 0x0010)
    // read from the Crystal game ROM rather than the zero-filled boot ROM buffer.
    // Writing 1 to 0xFF50 (GB_IO_BANK) calls gb->boot_rom_finished = true via the
    // memory write handler.
    GB_write_memory(&gb, 0xFF50, 1);

    // Set ROM bank 0x0D (MBC3 bank register at 0x2000-0x3FFF).
    // BattleCommand_ResetStats and CalcPlayerStats/CalcEnemyStats are all in bank 0x0D.
    GB_write_memory(&gb, 0x2000, sym.BattleCommand_ResetStats.bank);

    // Initialize WRAM: poison all, then write fixture values.
    size_t wram_sz = 0; uint16_t wbank = 0;
    uint8_t* wram = static_cast<uint8_t*>(
        GB_get_direct_access(&gb, GB_DIRECT_ACCESS_RAM, &wram_sz, &wbank));
    if (!wram || wram_sz < 0x2000) { GB_free(&gb); return std::nullopt; }
    std::memset(wram, poison, wram_sz);

    // wLinkMode = 0: non-link battle. BadgeStatBoosts reads this first and falls
    // through to badge checks (0 badges → no boosts applied, no short-circuit).
    wram[wram_off(sym.wLinkMode.addr)] = 0;

    // wInBattleTowerBattle = 0
    wram[wram_off(sym.wInBattleTowerBattle.addr)] = 0;

    // wJohtoBadges = 0: all BIT checks in BadgeStatBoosts fail → no stat boosts.
    wram[wram_off(sym.wJohtoBadges.addr)] = 0;

    // wBattleMonStatus = 0: no PAR/BRN → ApplyPrzEffectOnSpeed and
    // ApplyBrnEffectOnAttack both return early.
    wram[wram_off(sym.wBattleMonStatus.addr)  ] = 0;
    wram[wram_off(sym.wBattleMonStatus.addr)+1] = 0;

    // wEnemyMonStatus = 0
    wram[wram_off(sym.wEnemyMonStatus.addr)  ] = 0;
    wram[wram_off(sym.wEnemyMonStatus.addr)+1] = 0;

    // wPlayerStatLevels: write pre-Haze deltas. BattleCommand_ResetStats .Fill
    // overwrites these to 7 regardless. Writing non-neutral values here proves
    // .Fill actually executed (poison value of 7 would be indistinguishable).
    {
        uint8_t* p = wram + wram_off(sym.wPlayerStatLevels.addr);
        for(int i=0;i<7;i++) p[i]=(uint8_t)(7+PLAYER_DELTA[i]);
        p[7]=7;  // 8th slot (Curse), not touched by Haze
    }
    {
        uint8_t* p = wram + wram_off(sym.wEnemyStatLevels.addr);
        for(int i=0;i<7;i++) p[i]=(uint8_t)(7+ENEMY_DELTA[i]);
        p[7]=7;
    }

    // wPlayerStats: 5 × big-endian uint16 — CalcBattleStats reads these as the
    // base stat values. With neutral stages (7 → 1/1 multiplier), output = input.
    {
        uint8_t* p = wram + wram_off(sym.wPlayerStats.addr);
        auto be16=[](uint8_t* d,uint16_t v){d[0]=(v>>8);d[1]=v&0xFF;};
        be16(p+0,PLAYER_BASE.atk); be16(p+2,PLAYER_BASE.def);
        be16(p+4,PLAYER_BASE.spd); be16(p+6,PLAYER_BASE.satk); be16(p+8,PLAYER_BASE.sdef);
    }
    // wEnemyStats
    {
        uint8_t* p = wram + wram_off(sym.wEnemyStats.addr);
        auto be16=[](uint8_t* d,uint16_t v){d[0]=(v>>8);d[1]=v&0xFF;};
        be16(p+0,ENEMY_BASE.atk); be16(p+2,ENEMY_BASE.def);
        be16(p+4,ENEMY_BASE.spd); be16(p+6,ENEMY_BASE.satk); be16(p+8,ENEMY_BASE.sdef);
    }

    // hBattleTurn = 0 (player turn). BattleCommand_ResetStats saves + restores this.
    GB_write_memory(&gb, sym.hBattleTurn.addr, 0x00);

    // hROMBank: must reflect the currently-mapped bank. BankSwitch (RST $08 handler,
    // 0x2D63) reads hROMBank, saves it, switches to the target bank, calls the callee,
    // then restores hROMBank. Correct value = bank of BattleCommand_ResetStats = 0x0D.
    GB_write_memory(&gb, sym.hROMBank.addr, sym.BattleCommand_ResetStats.bank);

    // Clear IE/IF: belt-and-suspenders. IME starts as 0 from GB_init memset.
    GB_write_memory(&gb, 0xFFFF, 0x00);  // IE
    GB_write_memory(&gb, 0xFF0F, 0x00);  // IF

    // Set CPU entry point directly.
    // PC = BattleCommand_ResetStats (0x710E in bank 0x0D, currently mapped).
    // SP = 0xFFF0 − 2; stack holds AnimateCurrentMove.addr as return address.
    //   If the routine ever RETs before the execution callback fires, it lands
    //   at AnimateCurrentMove and the callback stops it there. No ROM needed.
    GB_registers_t* regs = GB_get_registers(&gb);
    if (!regs) { GB_free(&gb); return std::nullopt; }

    static constexpr uint16_t INITIAL_SP = 0xFFF0;
    const uint16_t ret_addr = sym.AnimateCurrentMove.addr;  // 0x7E01
    GB_write_memory(&gb, INITIAL_SP-1, (ret_addr>>8)&0xFF);
    GB_write_memory(&gb, INITIAL_SP-2,  ret_addr    &0xFF);
    regs->sp = INITIAL_SP-2;
    regs->pc = sym.BattleCommand_ResetStats.addr;  // 0x710E

    // Execute until AnimateCurrentMove boundary (or safety limit).
    static constexpr int MAX_INSN = 200000;
    while (!ctx.triggered && ctx.insn_count < MAX_INSN)
        GB_run(&gb);

    if (!ctx.triggered) {
        GB_free(&gb);
        return std::nullopt;  // harness error: didn't reach boundary
    }

    // Read semantic outputs from WRAM.
    wram_sz = 0;
    wram = static_cast<uint8_t*>(
        GB_get_direct_access(&gb, GB_DIRECT_ACCESS_RAM, &wram_sz, &wbank));
    if (!wram) { GB_free(&gb); return std::nullopt; }

    CrystalSnapshot snap{};
    snap.insn_count = ctx.insn_count;
    snap.boundary   = ctx.sink_name;

    { auto* p=wram+wram_off(sym.wPlayerStatLevels.addr); for(int i=0;i<7;i++) snap.player_stages[i]=p[i]; }
    { auto* p=wram+wram_off(sym.wEnemyStatLevels.addr);  for(int i=0;i<7;i++) snap.enemy_stages[i]=p[i]; }
    { auto* p=wram+wram_off(sym.wBattleMonAttack.addr);  for(int i=0;i<5;i++) snap.player_stats[i]=(uint16_t)((p[i*2]<<8)|p[i*2+1]); }
    { auto* p=wram+wram_off(sym.wEnemyMonAttack.addr);   for(int i=0;i<5;i++) snap.enemy_stats[i]=(uint16_t)((p[i*2]<<8)|p[i*2+1]); }

    GB_free(&gb);
    return snap;
}

// ============================================================================
// Enginemon helpers — loaded once from ROM profile, shared read-only.
// ============================================================================
static std::vector<crystal::PackageWriter::MoveDataEntry>
build_move_entries(const crystal::RomData& rom, const crystal::ExtractionProfile& prof) {
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
    auto entries = build_move_entries(rom, profile);
    if (!crystal::semanticize_move_entries(rom, profile, entries)) return std::nullopt;

    crystal::PackageWriter w;
    w.set_source_rom(std::string(40,'x'), "oracle");
    w.add_move_data(entries);

    auto pkg = std::filesystem::temp_directory_path() / "oracle_moves.emon";
    if (!w.write(pkg)) return std::nullopt;
    auto rdr = enginemon::PackageReader::open(pkg);
    if (!rdr) { std::filesystem::remove(pkg); return std::nullopt; }
    auto reg = rdr->load_move_registry();
    std::filesystem::remove(pkg);
    if (!reg) return std::nullopt;

    auto res = crystal::extract_battle_rules(rom, profile);
    EngineData d;
    d.moves = *reg;
    d.rules = res.success ? res.rules : enginemon::BattleRules{};
    return d;
}

// ============================================================================
// run_enginemon_case — runs Enginemon Battle::execute_turn() for a move.
// ============================================================================
static std::optional<EngineSnapshot> run_enginemon_case(
    enginemon::MoveId move_id,
    const EngineData& ed)
{
    const enginemon::MoveData* md = ed.moves.get(move_id);
    if (!md || !md->effect_desc.is_supported) return std::nullopt;

    enginemon::Registries reg{}; reg.moves = ed.moves;

    enginemon::Party party;
    { enginemon::Pokemon pm{}; pm.species=1; pm.level=50;
      pm.current_hp=pm.max_hp=300; pm.friendship=200; party.add(pm); }

    enginemon::Battle bat(enginemon::BattleType::Wild, party, reg, ed.rules);

    // Player: uses move_id, pre-Haze stage deltas, PLAYER_BASE stats.
    {
        enginemon::BattlePokemon bp{};
        bp.species=1; bp.type1=0; bp.type2=0; bp.level=50;
        bp.stats.hp=bp.stats.max_hp=300; bp.base_stats.hp=bp.base_stats.max_hp=300;
        bp.stats.attack=bp.base_stats.attack=PLAYER_BASE.atk;
        bp.stats.defense=bp.base_stats.defense=PLAYER_BASE.def;
        bp.stats.speed=bp.base_stats.speed=PLAYER_BASE.spd;
        bp.stats.special_attack=bp.base_stats.special_attack=PLAYER_BASE.satk;
        bp.stats.special_defense=bp.base_stats.special_defense=PLAYER_BASE.sdef;
        bp.happiness=200; bp.dv_atk=bp.dv_def=bp.dv_spd=bp.dv_spc=15;
        bp.moves[0].move=move_id; bp.moves[0].pp=bp.moves[0].max_pp=10;
        bp.stages.attack=PLAYER_DELTA[0]; bp.stages.defense=PLAYER_DELTA[1];
        bp.stages.speed=PLAYER_DELTA[2];  bp.stages.special_attack=PLAYER_DELTA[3];
        bp.stages.special_defense=PLAYER_DELTA[4]; bp.stages.accuracy=PLAYER_DELTA[5];
        bp.stages.evasion=PLAYER_DELTA[6];
        bat.player_pokemon() = bp;
    }
    // Opponent: no move, ENEMY_BASE stats, pre-Haze enemy stage deltas.
    {
        enginemon::BattlePokemon bp{};
        bp.species=1; bp.type1=0; bp.type2=0; bp.level=50;
        bp.stats.hp=bp.stats.max_hp=300; bp.base_stats.hp=bp.base_stats.max_hp=300;
        bp.stats.attack=bp.base_stats.attack=ENEMY_BASE.atk;
        bp.stats.defense=bp.base_stats.defense=ENEMY_BASE.def;
        bp.stats.speed=bp.base_stats.speed=ENEMY_BASE.spd;
        bp.stats.special_attack=bp.base_stats.special_attack=ENEMY_BASE.satk;
        bp.stats.special_defense=bp.base_stats.special_defense=ENEMY_BASE.sdef;
        bp.happiness=200; bp.dv_atk=bp.dv_def=bp.dv_spd=bp.dv_spc=15;
        bp.moves[0].move=enginemon::MOVE_NONE; bp.moves[0].pp=bp.moves[0].max_pp=0;
        bp.stages.attack=ENEMY_DELTA[0]; bp.stages.defense=ENEMY_DELTA[1];
        bp.stages.speed=ENEMY_DELTA[2];  bp.stages.special_attack=ENEMY_DELTA[3];
        bp.stages.special_defense=ENEMY_DELTA[4]; bp.stages.accuracy=ENEMY_DELTA[5];
        bp.stages.evasion=ENEMY_DELTA[6];
        bat.opponent_pokemon() = bp;
    }

    bat.set_rng_callback([]()->uint32_t{ return 0xFF; });
    bat.set_player_action(enginemon::ActionFight{0,0});
    bat.set_opponent_action(enginemon::ActionFight{0,0});
    bat.execute_turn();

    const auto& pp = bat.player_pokemon();
    const auto& op = bat.opponent_pokemon();
    const auto& ps = pp.stages;
    const auto& os = op.stages;

    EngineSnapshot e{};
    e.player_stages[0]=ps.attack; e.player_stages[1]=ps.defense; e.player_stages[2]=ps.speed;
    e.player_stages[3]=ps.special_attack; e.player_stages[4]=ps.special_defense;
    e.player_stages[5]=ps.accuracy; e.player_stages[6]=ps.evasion;
    e.enemy_stages[0]=os.attack; e.enemy_stages[1]=os.defense; e.enemy_stages[2]=os.speed;
    e.enemy_stages[3]=os.special_attack; e.enemy_stages[4]=os.special_defense;
    e.enemy_stages[5]=os.accuracy; e.enemy_stages[6]=os.evasion;
    e.player_stats[0]=(uint16_t)pp.stats.attack; e.player_stats[1]=(uint16_t)pp.stats.defense;
    e.player_stats[2]=(uint16_t)pp.stats.speed;  e.player_stats[3]=(uint16_t)pp.stats.special_attack;
    e.player_stats[4]=(uint16_t)pp.stats.special_defense;
    e.enemy_stats[0]=(uint16_t)op.stats.attack;  e.enemy_stats[1]=(uint16_t)op.stats.defense;
    e.enemy_stats[2]=(uint16_t)op.stats.speed;   e.enemy_stats[3]=(uint16_t)op.stats.special_attack;
    e.enemy_stats[4]=(uint16_t)op.stats.special_defense;
    return e;
}

// ============================================================================
// CaseResult — per-move outcome
// ============================================================================
enum class Status { MATCH, ENGINEMON_MISMATCH, HARNESS_ERROR };
static const char* status_str(Status s) {
    switch(s){
    case Status::MATCH:              return "MATCH";
    case Status::ENGINEMON_MISMATCH: return "ENGINEMON_MISMATCH";
    case Status::HARNESS_ERROR:      return "HARNESS_ERROR";
    }
    return "?";
}

struct CaseResult {
    uint16_t    move_id;
    Status      status;
    std::string detail;   // diff lines or error message
    int         insn_count;
    bool        poison_stable;
    const char* boundary;
};

// ============================================================================
// run_case — executes one move case (two poison runs + Enginemon diff).
// Self-contained, safe to call from any thread.
// ============================================================================
static CaseResult run_case(
    uint16_t move_id,
    const std::vector<uint8_t>& rom_bytes,
    const SymCache& sym,
    const EngineData& ed)
{
    CaseResult r;
    r.move_id     = move_id;
    r.status      = Status::HARNESS_ERROR;
    r.insn_count  = 0;
    r.poison_stable = false;
    r.boundary    = nullptr;

    // Two Crystal runs with different poison bytes — outputs must agree.
    auto c1 = run_crystal_case(rom_bytes, sym, 0x00);
    auto c2 = run_crystal_case(rom_bytes, sym, 0xA5);

    if (!c1 || !c2) {
        r.detail = "Crystal execution failed to reach presentation boundary";
        return r;
    }

    r.insn_count   = c1->insn_count;
    r.boundary     = c1->boundary;
    r.poison_stable = (*c1 == *c2);

    if (!r.poison_stable) {
        r.detail = "Poison instability: 0x00 vs 0xA5 Crystal runs disagree";
        return r;
    }

    // Enginemon run
    auto eng = run_enginemon_case(move_id, ed);
    if (!eng) {
        r.detail = "Enginemon execution failed (move not supported or harness error)";
        return r;
    }

    // Normalize and compare.
    // Crystal stage encoding: 7=neutral. Enginemon: 0=neutral.
    // crystal_delta = raw - 7 == enginemon_stage
    static const char* SNAME[7] = {"ATK","DEF","SPD","SATK","SDEF","ACC","EVA"};
    static const char* CNAME[5] = {"ATK","DEF","SPD","SATK","SDEF"};
    bool all_match = true;
    std::ostringstream diff;

    for(int i=0;i<7;i++){
        int8_t cd = int8_t(int(c1->player_stages[i])-7);
        int8_t ed2 = eng->player_stages[i];
        if(cd!=ed2){ all_match=false; diff<<"player."<<SNAME[i]<<": crystal="<<(int)cd<<" enginemon="<<(int)ed2<<"\n"; }
    }
    for(int i=0;i<7;i++){
        int8_t cd = int8_t(int(c1->enemy_stages[i])-7);
        int8_t ed2 = eng->enemy_stages[i];
        if(cd!=ed2){ all_match=false; diff<<"enemy."<<SNAME[i]<<": crystal="<<(int)cd<<" enginemon="<<(int)ed2<<"\n"; }
    }
    for(int i=0;i<5;i++){
        uint16_t cr=c1->player_stats[i], em=eng->player_stats[i];
        if(cr!=em){ all_match=false; diff<<"player.stat."<<CNAME[i]<<": crystal="<<cr<<" enginemon="<<em<<"\n"; }
    }
    for(int i=0;i<5;i++){
        uint16_t cr=c1->enemy_stats[i], em=eng->enemy_stats[i];
        if(cr!=em){ all_match=false; diff<<"enemy.stat."<<CNAME[i]<<": crystal="<<cr<<" enginemon="<<em<<"\n"; }
    }

    r.status = all_match ? Status::MATCH : Status::ENGINEMON_MISMATCH;
    r.detail = diff.str();
    return r;
}

// ============================================================================
// Registered move list — one entry per move that has a Crystal execution path
// and an Enginemon implementation. Currently only Haze (114).
// Adding a new move requires: registering here AND ensuring the move's routine
// exits at an allowlisted presentation sink.
// ============================================================================
static const uint16_t REGISTERED_MOVES[] = { 114 };
static constexpr size_t NUM_REGISTERED = sizeof(REGISTERED_MOVES)/sizeof(REGISTERED_MOVES[0]);

static bool is_registered(uint16_t id) {
    for(size_t i=0;i<NUM_REGISTERED;i++) if(REGISTERED_MOVES[i]==id) return true;
    return false;
}

// ============================================================================
// runner_main — public entry point
// ============================================================================
int runner_main(int argc, char* argv[], RunnerConfig defaults) {
    // --- Parse arguments ---
    std::string rom_path, sym_path;
    std::vector<uint16_t> move_ids = defaults.move_ids;
    int jobs = defaults.jobs;
    bool all_flag = false;

    if (argc < 3) {
        std::cerr << "Usage: <exe> <rom_path> <sym_path> [--jobs N] [--all] [--move <id> ...]\n";
        return 1;
    }
    rom_path = argv[1];
    sym_path = argv[2];
    for (int i=3; i<argc; ++i) {
        std::string a = argv[i];
        if (a=="--jobs" && i+1<argc) { jobs = std::stoi(argv[++i]); }
        else if (a=="--all")         { all_flag = true; }
        else if (a=="--move" && i+1<argc) {
            while (i+1<argc && argv[i+1][0]!='-')
                move_ids.push_back((uint16_t)std::stoi(argv[++i]));
        }
    }
    if (jobs < 1) jobs = 1;

    if (all_flag) {
        move_ids.assign(REGISTERED_MOVES, REGISTERED_MOVES+NUM_REGISTERED);
    } else if (move_ids.empty()) {
        // default: whatever was passed in defaults (already set above)
        if (move_ids.empty()) {
            std::cerr << "No moves selected. Use --all or --move <id>.\n";
            return 1;
        }
    }

    // Validate move IDs against the registered list.
    for (uint16_t id : move_ids) {
        if (!is_registered(id)) {
            std::cerr << "Move " << id << " is not registered in the oracle.\n"
                      << "  Registered moves:";
            for (auto m : REGISTERED_MOVES) std::cerr<<" "<<m;
            std::cerr<<"\n";
            return 1;
        }
    }

    // Deduplicate, sort deterministically.
    std::sort(move_ids.begin(), move_ids.end());
    move_ids.erase(std::unique(move_ids.begin(), move_ids.end()), move_ids.end());

    // --- Load and verify ROM ---
    std::vector<uint8_t> rom_bytes;
    {
        std::ifstream f(rom_path, std::ios::binary);
        if (!f) { std::cerr << "Cannot open ROM: " << rom_path << "\n"; return 1; }
        rom_bytes.assign(std::istreambuf_iterator<char>(f), {});
    }
    if (rom_bytes.size() != CRYSTAL_ROM_SIZE) {
        std::cerr << "ROM size wrong: " << rom_bytes.size() << " (expected " << CRYSTAL_ROM_SIZE << ")\n";
        return 1;
    }
    std::string actual_sha = sha1_hex(rom_bytes.data(), rom_bytes.size());
    std::cout << "ROM SHA-1:   " << actual_sha << "\n";
    if (actual_sha != CRYSTAL_ROM_SHA1) {
        std::cerr << "FATAL: ROM SHA-1 mismatch.\n"
                  << "  Expected: " << CRYSTAL_ROM_SHA1 << "\n"
                  << "  Actual:   " << actual_sha << "\n";
        return 1;
    }
    std::cout << "ROM: OK (verified, passed unchanged to SameBoy)\n";
    std::cout << "SameBoy:     " << SAMEBOY_COMMIT << "\n";

    // --- Load sym cache ---
    auto sym_opt = SymCache::load(sym_path);
    if (!sym_opt) { std::cerr << "Failed to load sym cache from: " << sym_path << "\n"; return 1; }
    const SymCache& sym = *sym_opt;

    // --- Load Enginemon data from ROM ---
    auto rom_data = crystal::RomData::load(std::filesystem::path(rom_path));
    if (!rom_data) { std::cerr << "RomData::load failed\n"; return 1; }
    const crystal::ExtractionProfile* profile =
        crystal::ProfileRegistry::instance().get_profile_by_hash(rom_data->hash());
    if (!profile) { std::cerr << "No Enginemon profile for ROM\n"; return 1; }

    auto ed_opt = load_engine_data(*rom_data, *profile);
    if (!ed_opt) { std::cerr << "Failed to load engine data\n"; return 1; }
    const EngineData& ed = *ed_opt;

    std::cout << "\n=== Crystal Oracle (" << move_ids.size() << " move(s), "
              << jobs << " job(s)) ===\n";
    auto t0 = std::chrono::steady_clock::now();

    // --- Parallel execution ---
    // Futures indexed by position in move_ids for deterministic output ordering.
    const size_t n = move_ids.size();
    std::vector<std::future<CaseResult>> futures;
    futures.reserve(n);

    // Semaphore-style throttle to respect --jobs limit.
    // We use a simple approach: launch min(jobs, n) workers at a time.
    std::atomic<size_t> next_job{0};
    std::mutex launch_mutex;

    // Each future launches immediately but we cap active threads.
    // Use std::async(std::launch::async) with a bounded approach:
    // submit jobs in batches of `jobs`, collect results in order.
    for (size_t i = 0; i < n; ) {
        size_t batch_end = std::min(i + (size_t)jobs, n);
        for (size_t j = i; j < batch_end; ++j) {
            uint16_t mid = move_ids[j];
            // Each future captures by value: rom_bytes ref (shared, read-only),
            // sym ref (shared, read-only), ed ref (shared, read-only).
            futures.push_back(std::async(std::launch::async,
                [&rom_bytes, &sym, &ed, mid]() {
                    return run_case(mid, rom_bytes, sym, ed);
                }));
        }
        // Collect batch results before launching next batch.
        for (size_t j = i; j < batch_end; ++j) {
            futures[j].wait();  // block until available
        }
        i = batch_end;
    }

    // --- Collect results in original (sorted) move order ---
    int match_count = 0, mismatch_count = 0, error_count = 0;
    std::vector<CaseResult> results;
    results.reserve(n);
    for (auto& f : futures) results.push_back(f.get());

    // Print summary in deterministic order.
    std::cout << "\n";
    for (const auto& r : results) {
        std::cout << "  move " << r.move_id << ": " << status_str(r.status);
        if (r.boundary)     std::cout << "  [boundary=" << r.boundary << "]";
        if (r.insn_count>0) std::cout << "  [" << r.insn_count << " insn]";
        std::cout << "  [poison-stable=" << (r.poison_stable?"yes":"NO") << "]";
        std::cout << "\n";
        if (!r.detail.empty()) {
            std::istringstream ss(r.detail);
            std::string line;
            while (std::getline(ss,line)) std::cout << "    " << line << "\n";
        }
        switch(r.status){
        case Status::MATCH:              ++match_count; break;
        case Status::ENGINEMON_MISMATCH: ++mismatch_count; break;
        case Status::HARNESS_ERROR:      ++error_count; break;
        }
    }

    auto t1 = std::chrono::steady_clock::now();
    double ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1-t0).count();

    std::cout << "\n=== Summary ===\n";
    std::cout << "  MATCH:              " << match_count     << "\n";
    std::cout << "  ENGINEMON_MISMATCH: " << mismatch_count  << "\n";
    std::cout << "  HARNESS_ERROR:      " << error_count     << "\n";
    std::cout << "  Total:              " << n               << "\n";
    std::cout << "  Time:               " << (int)ms         << " ms\n";
    std::cout << "  Jobs:               " << jobs            << "\n";

    return (mismatch_count > 0 || error_count > 0) ? 1 : 0;
}

} // namespace crystal::oracle
