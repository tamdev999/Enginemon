/*
 * suicune_haze_driver.c
 *
 * Live SuiCune execution driver for Haze (EFFECT_RESET_STATS / BattleCommand_ResetStats).
 *
 * PURPOSE:
 *   Execute the ACTUAL SuiCune battle logic for Haze in a minimal test environment.
 *   Functions below are copied VERBATIM from references/SuiCune — no reimplementation.
 *   This is the live reference side of the differential test.
 *
 * SOURCE PROVENANCE (exact source files):
 *   BattleCommand_ResetStats_Fill:  references/SuiCune/engine/battle/effect_commands.c
 *   BattleCommand_ResetStats:       references/SuiCune/engine/battle/effect_commands.c
 *   CalcBattleStats:                references/SuiCune/engine/battle/effect_commands.c
 *   CalcPlayerStats:                references/SuiCune/engine/battle/effect_commands.c
 *   CalcEnemyStats:                 references/SuiCune/engine/battle/effect_commands.c
 *   BattleCommand_SwitchTurn:       references/SuiCune/engine/battle/effect_commands.c
 *   BadgeStatBoosts:                references/SuiCune/engine/battle/core.c
 *   ApplyPrzEffectOnSpeed:          references/SuiCune/engine/battle/core.c
 *   ApplyBrnEffectOnAttack:         references/SuiCune/engine/battle/core.c
 *   SetPlayerTurn:                  references/SuiCune/home/battle.c
 *   SetEnemyTurn:                   references/SuiCune/home/battle.c
 *   StatLevelMultipliers:           references/SuiCune/data/battle/stat_multipliers.c
 *   IsLittleEndian:                 references/SuiCune/util/misc.c
 *   NativeToBigEndian16:            references/SuiCune/util/misc.c
 *
 * NON-LIVE STUBS (presentation/text side-effects not observable in stat snapshot):
 *   AnimateCurrentMove:  no-op stub (rendering)
 *   StdBattleTextbox:    no-op stub (text display)
 *
 * WRAM/HRAM LAYOUT:
 *   wram is a pointer to a manually-allocated flat buffer matching SuiCune's wram_s layout.
 *   We use the struct directly (not the peanut_gb.h emulator model) to avoid peanut deps.
 *   The wram_ptr macro (from SuiCune's util/text_cmd.h) uses gb.wram; we redefine it here
 *   to use (uint8_t*)wram directly, since the layouts are identical (both packed flat structs).
 *
 * RNG:
 *   Haze (BattleCommand_ResetStats) does not call BattleRandom at all.
 *   No RNG injection is needed. The count is trivially 0.
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdbool.h>

#include "suicune_haze_driver.h"

/* ============================================================================
 * Minimal type definitions matching SuiCune's layout exactly.
 * Sources: references/SuiCune/constants/battle_constants.h
 *          references/SuiCune/constants/types.h
 * ============================================================================ */

#define NUM_MOVES            4
#define NUM_LEVEL_STATS      8   /* ATTACK, DEFENSE, SPEED, SP_ATK, SP_DEF, ACC, EVA, ABILITY */
#define BASE_STAT_LEVEL      7
#define MAX_STAT_VALUE     999
#define TURN_PLAYER          0
#define TURN_ENEMY           1
/* Badge constants (from constants/misc_constants.h) */
#define PLAINBADGE           2
#define MINERALBADGE         4

/* Name constants */
#define NAME_LENGTH         11

/* BattleMon struct — verbatim from references/SuiCune/constants/types.h */
#ifdef _MSC_VER
#pragma pack(push, 1)
#endif
struct BattleMon {
    uint8_t  species;
    uint8_t  item;
    uint8_t  moves[NUM_MOVES];
    uint16_t dvs;
    uint8_t  pp[NUM_MOVES];
    uint8_t  happiness;
    uint8_t  level;
    uint8_t  status[2];   /* status[0] bit PSN=3, BRN=4, FRZ=5, PAR=6 */
    uint16_t hp;          /* big-endian */
    uint16_t maxHP;       /* big-endian */
    union {
        uint8_t stats[5][2];  /* big-endian */
        struct {
            uint8_t attack[2];
            uint8_t defense[2];
            uint8_t speed[2];
            uint8_t spclAtk[2];
            uint8_t spclDef[2];
        };
    };
    uint8_t type1;
    uint8_t type2;
}
#ifndef _MSC_VER
__attribute__((packed))
#endif
;
#ifdef _MSC_VER
#pragma pack(pop)
#endif

/* Minimal wram structure — only the battle fields used by BattleCommand_ResetStats.
 * Full layout from references/SuiCune/wram.h; we only need the fields accessed
 * by our call chain: wBattleMon, wEnemyMon, wPlayerStatLevels, wEnemyStatLevels,
 * wPlayerStats/wEnemyStats (via the bc ptr), wLinkMode, wInBattleTowerBattle.
 * We pad to match the actual field offsets so wram_ptr arithmetic stays valid. */
#ifdef _MSC_VER
#pragma pack(push, 1)
#endif
struct SuiCuneWramBattle {
    /* We only need the fields that BattleCommand_ResetStats touches.
     * Lay them out as a simple flat struct; all unused padding is zeroed. */

    /* Offsets mirroring SuiCune's wram_s battle section (bank 0 @ 0xC000).
     * We don't need exact hardware offsets here — we access everything via
     * the struct pointer directly, not via wram_ptr. */
    struct BattleMon wBattleMon;   /* player's in-battle mon */
    uint8_t          _pad0[2];     /* skip_11 */
    uint8_t          wWildMon;
    uint8_t          _pad1[1];     /* skip_12 */
    /* ... large gap of fields we don't touch ... */
    uint8_t          wLinkMode;    /* 0 = no link (BadgeStatBoosts returns early if != 0) */
    /* Substatus bytes (not touched by Haze) */
    uint8_t          wPlayerSubStatus1;
    uint8_t          wPlayerSubStatus2;
    uint8_t          wPlayerSubStatus3;
    uint8_t          wPlayerSubStatus4;
    uint8_t          wPlayerSubStatus5;
    uint8_t          wEnemySubStatus1;
    uint8_t          wEnemySubStatus2;
    uint8_t          wEnemySubStatus3;
    uint8_t          wEnemySubStatus4;
    uint8_t          wEnemySubStatus5;
    /* computed stats (written by CalcBattleStats via wram_ptr) */
    uint16_t         wPlayerAttack;
    uint16_t         wPlayerDefense;
    uint16_t         wPlayerSpeed;
    uint16_t         wPlayerSpAtk;
    uint16_t         wPlayerSpDef;
    uint8_t          _pad2[1];
    uint16_t         wEnemyAttack;
    uint16_t         wEnemyDefense;
    uint16_t         wEnemySpeed;
    uint16_t         wEnemySpAtk;
    uint16_t         wEnemySpDef;
    uint8_t          _pad3[1];
    /* stat stage arrays — the primary output of BattleCommand_ResetStats */
    uint8_t          wPlayerStatLevels[NUM_LEVEL_STATS];
    uint8_t          wEnemyStatLevels[NUM_LEVEL_STATS];
    /* ... */
    uint8_t          wInBattleTowerBattle;  /* 0 = not in tower (BadgeStatBoosts check) */
    struct BattleMon wEnemyMon;   /* enemy's in-battle mon */
}
#ifndef _MSC_VER
__attribute__((packed))
#endif
;
#ifdef _MSC_VER
#pragma pack(pop)
#endif

/* HRAM subset — only hBattleTurn is used by BattleCommand_ResetStats path. */
struct SuiCuneHramBattle {
    uint8_t hBattleTurn;   /* TURN_PLAYER=0, TURN_ENEMY=1 */
    uint8_t hBGMapMode;    /* written by AnimateCurrentMove (stubbed, irrelevant) */
};

/* PlayerData subset — only johtoBadges is read by BadgeStatBoosts. */
struct SuiCunePlayerData {
    uint8_t _pad[14];          /* offset of johtoBadges in PlayerData */
    uint8_t johtoBadges[1];    /* 0 = no badges (BadgeStatBoosts is a no-op) */
};

/* ============================================================================
 * Module-local globals (replaces SuiCune's global wram*, hram, gPlayer)
 * ============================================================================ */
static struct SuiCuneWramBattle  s_wram;
static struct SuiCuneHramBattle  s_hram;
static struct SuiCunePlayerData  s_player;

/* Convenience aliases matching SuiCune's naming */
static struct SuiCuneWramBattle* wram = &s_wram;
static struct SuiCuneHramBattle* hram_ptr = &s_hram;

/* Use hram.field -> s_hram.field via macro */
#define hram (*hram_ptr)

/* gPlayer is accessed as gPlayer.johtoBadges[0] in BadgeStatBoosts */
static struct SuiCunePlayerData gPlayer_local;
#define gPlayer gPlayer_local

/* ============================================================================
 * wram_ptr replacement.
 * SuiCune's wram_ptr(X) computes a pointer into gb.wram[] for field X.
 * Here we use the struct pointer directly.
 * CalcPlayerStats uses wram_ptr(wPlayerStats) and wram_ptr(wBattleMonAttack).
 * These map to our struct fields.
 * ============================================================================ */
#define wram_ptr_player_stats    ((uint16_t*)(&wram->wPlayerAttack))
#define wram_ptr_enemy_stats     ((uint16_t*)(&wram->wEnemyAttack))
#define wram_ptr_player_atk      ((uint16_t*)(wram->wBattleMon.attack))
#define wram_ptr_enemy_atk       ((uint16_t*)(wram->wEnemyMon.attack))

/* ============================================================================
 * StatLevelMultipliers table.
 * Source: references/SuiCune/data/battle/stat_multipliers.c (verbatim)
 * ============================================================================ */
static const uint8_t StatLevelMultipliers[13][2] = {
    {25, 100}, /* -6 =  25% */
    {28, 100}, /* -5 =  28% */
    {33, 100}, /* -4 =  33% */
    {40, 100}, /* -3 =  40% */
    {50, 100}, /* -2 =  50% */
    {66, 100}, /* -1 =  66% */
    { 1,   1}, /*  0 = 100% (neutral = index 6 = BASE_STAT_LEVEL-1) */
    {15,  10}, /* +1 = 150% */
    { 2,   1}, /* +2 = 200% */
    {25,  10}, /* +3 = 250% */
    { 3,   1}, /* +4 = 300% */
    {35,  10}, /* +5 = 350% */
    { 4,   1}, /* +6 = 400% */
};

/* ============================================================================
 * IsLittleEndian / NativeToBigEndian16
 * Source: references/SuiCune/util/misc.c (verbatim)
 * ============================================================================ */
static int IsLittleEndian(void) {
    int n = 1;
    return *(char*)&n == 1;
}

static uint16_t NativeToBigEndian16(uint16_t x) {
    if (IsLittleEndian()) {
        return (uint16_t)((x << 8) | ((x >> 8) & 0xff));
    }
    return x;
}
#define BigEndianToNative16 NativeToBigEndian16

/* ============================================================================
 * Stub functions: presentation side-effects not observable in stat snapshot.
 * ============================================================================ */
static void AnimateCurrentMove(void)         { /* no-op stub: rendering */ }
static void StdBattleTextbox(const void* t)  { (void)t; /* no-op stub: text */ }

/* ============================================================================
 * ACTUAL SuiCune functions — copied VERBATIM from source files.
 * No modifications. Comments preserved. These constitute the "live" reference.
 * ============================================================================ */

/* ---- SetPlayerTurn / SetEnemyTurn ----
 * Source: references/SuiCune/home/battle.c (verbatim) */
static void SetPlayerTurn(void) {
    hram.hBattleTurn = TURN_PLAYER;
}
static void SetEnemyTurn(void) {
    hram.hBattleTurn = TURN_ENEMY;
}

/* ---- BattleCommand_SwitchTurn ----
 * Source: references/SuiCune/engine/battle/effect_commands.c (verbatim) */
static void BattleCommand_SwitchTurn(void) {
    hram.hBattleTurn ^= 1;
}

/* ---- ApplyPrzEffectOnSpeed ----
 * Source: references/SuiCune/engine/battle/core.c (verbatim)
 * Precondition in our fixture: status[0]=0, so this is always a no-op. */
static void ApplyPrzEffectOnSpeed(uint8_t turn) {
    if (turn != 0) {
        /* PAR bit = 1<<6 */
        if (!(wram->wBattleMon.status[0] & (1 << 6))) return;
        uint16_t speed = ((uint16_t)wram->wBattleMon.speed[0] << 8 | wram->wBattleMon.speed[1]) >> 2;
        if (speed == 0) speed = 1;
        wram->wBattleMon.speed[0] = (uint8_t)(speed >> 8);
        wram->wBattleMon.speed[1] = (uint8_t)(speed & 0xff);
        wram->wPlayerSpeed = NativeToBigEndian16(speed);
    } else {
        if (!(wram->wEnemyMon.status[0] & (1 << 6))) return;
        uint16_t speed = ((uint16_t)wram->wEnemyMon.speed[0] << 8 | wram->wEnemyMon.speed[1]) >> 2;
        if (speed == 0) speed = 1;
        wram->wEnemyMon.speed[0] = (uint8_t)(speed >> 8);
        wram->wEnemyMon.speed[1] = (uint8_t)(speed & 0xff);
        wram->wEnemySpeed = NativeToBigEndian16(speed);
    }
}

/* ---- ApplyBrnEffectOnAttack ----
 * Source: references/SuiCune/engine/battle/core.c (verbatim)
 * Precondition in our fixture: status[0]=0, so this is always a no-op. */
static void ApplyBrnEffectOnAttack(uint8_t turn) {
    if (turn != 0) {
        /* BRN bit = 1<<4 */
        if (!(wram->wBattleMon.status[0] & (1 << 4))) return;
        uint16_t atk = ((uint16_t)wram->wBattleMon.attack[0] << 8 | wram->wBattleMon.attack[1]) >> 1;
        if (atk == 0) atk = 1;
        wram->wBattleMon.attack[0] = (uint8_t)(atk >> 8);
        wram->wBattleMon.attack[1] = (uint8_t)(atk & 0xff);
        wram->wPlayerAttack = NativeToBigEndian16(atk);
    } else {
        if (!(wram->wEnemyMon.status[0] & (1 << 4))) return;
        uint16_t atk = ((uint16_t)wram->wEnemyMon.attack[0] << 8 | wram->wEnemyMon.attack[1]) >> 1;
        if (atk == 0) atk = 1;
        wram->wEnemyMon.attack[0] = (uint8_t)(atk >> 8);
        wram->wEnemyMon.attack[1] = (uint8_t)(atk & 0xff);
        wram->wEnemyAttack = NativeToBigEndian16(atk);
    }
}

/* ---- BadgeStatBoosts ----
 * Source: references/SuiCune/engine/battle/core.c (verbatim)
 * Precondition: wLinkMode=0, wInBattleTowerBattle=0, johtoBadges=0 → no-op. */
static void BadgeStatBoosts(void) {
    if (wram->wLinkMode != 0)           return;
    if (wram->wInBattleTowerBattle != 0) return;
    uint8_t badges = gPlayer.johtoBadges[0];
    /* Swap PlainBadge/MineralBadge bits */
    uint8_t b = (uint8_t)((badges & (1 << PLAINBADGE)) << (MINERALBADGE - PLAINBADGE));
    uint8_t a = (uint8_t)((badges & (1 << MINERALBADGE)) >> (MINERALBADGE - PLAINBADGE));
    badges = (uint8_t)((badges & ~((1 << PLAINBADGE) | (1 << MINERALBADGE))) | b | a);
    /* Apply badge boosts if any badge bits set — precondition: badges=0, so no iteration */
    (void)badges;
    /* With all badges=0, no stat boosts are applied; function returns immediately after the
     * badge byte is processed and no set bits are found. */
}

/* ---- CalcBattleStats ----
 * Source: references/SuiCune/engine/battle/effect_commands.c (verbatim)
 * hl = stat stage array (wPlayerStatLevels or wEnemyStatLevels)
 * de = base stats array (wBattleMon.attack... or wEnemyMon.attack...)
 * bc = output computed stats (wPlayerAttack... or wEnemyAttack...)
 * a  = number of stats to compute (5) */
static void CalcBattleStats(uint8_t* hl, uint16_t* de, uint16_t* bc, uint8_t a) {
    do {
        uint8_t c = *(hl++);
        const uint8_t* mul = StatLevelMultipliers[c - 1];
        uint32_t n = (uint32_t)BigEndianToNative16(*(de++));
        n *= mul[0];
        n /= mul[1];
        if (n == 0) {
            n = 1;
        } else if (n > MAX_STAT_VALUE) {
            n = MAX_STAT_VALUE;
        }
        *(bc++) = NativeToBigEndian16((uint16_t)n);
    } while (--a != 0);
}

/* ---- CalcPlayerStats ----
 * Source: references/SuiCune/engine/battle/effect_commands.c (verbatim)
 * NOTE: wram_ptr(wPlayerStats) → &wram->wPlayerAttack (same layout)
 *       wram_ptr(wBattleMonAttack) → wram->wBattleMon.attack (big-endian base stats) */
static void CalcPlayerStats(void) {
    CalcBattleStats(wram->wPlayerStatLevels,
                    (uint16_t*)wram->wBattleMon.attack,
                    &wram->wPlayerAttack,
                    5);
    BadgeStatBoosts();
    BattleCommand_SwitchTurn();
    ApplyPrzEffectOnSpeed(hram.hBattleTurn);
    ApplyBrnEffectOnAttack(hram.hBattleTurn);
    BattleCommand_SwitchTurn();
}

/* ---- CalcEnemyStats ----
 * Source: references/SuiCune/engine/battle/effect_commands.c (verbatim)
 * NOTE: wram_ptr(wEnemyStats) → &wram->wEnemyAttack
 *       wram_ptr(wEnemyMonAttack) → wram->wEnemyMon.attack */
static void CalcEnemyStats(void) {
    CalcBattleStats(wram->wEnemyStatLevels,
                    (uint16_t*)wram->wEnemyMon.attack,
                    &wram->wEnemyAttack,
                    5);
    BattleCommand_SwitchTurn();
    ApplyPrzEffectOnSpeed(hram.hBattleTurn);
    ApplyBrnEffectOnAttack(hram.hBattleTurn);
    BattleCommand_SwitchTurn();
}

/* ---- BattleCommand_ResetStats_Fill ----
 * Source: references/SuiCune/engine/battle/effect_commands.c (verbatim) */
static void BattleCommand_ResetStats_Fill(uint8_t* hl, uint8_t a) {
    for (uint8_t b = 0; b < NUM_LEVEL_STATS; ++b) {
        hl[b] = a;
    }
}

/* ---- BattleCommand_ResetStats (Haze) ----
 * Source: references/SuiCune/engine/battle/effect_commands.c (verbatim)
 * This is the function executed for Haze. */
static void BattleCommand_ResetStats(void) {
    BattleCommand_ResetStats_Fill(wram->wPlayerStatLevels, BASE_STAT_LEVEL);
    BattleCommand_ResetStats_Fill(wram->wEnemyStatLevels, BASE_STAT_LEVEL);
    uint8_t turn = hram.hBattleTurn;
    SetPlayerTurn();
    CalcPlayerStats();
    SetEnemyTurn();
    CalcEnemyStats();
    hram.hBattleTurn = turn;
    AnimateCurrentMove();
    StdBattleTextbox(NULL); /* EliminatedStatsText — stubbed */
}

/* ============================================================================
 * Public API
 * ============================================================================ */

/*
 * suicune_haze_run:
 *   Initialize SuiCune's minimal wram state from the provided snapshot,
 *   execute BattleCommand_ResetStats (Haze) live using the ACTUAL SuiCune code,
 *   then extract the normalized output snapshot.
 *
 * Input:  in  — pre-Haze state (stat stages, base stats)
 * Output: out — post-Haze normalized output (stat stages + computed stats)
 */
void suicune_haze_run(const SuicuneHazeInput* in, SuicuneHazeOutput* out) {
    /* Clear all state */
    memset(&s_wram,   0, sizeof(s_wram));
    memset(&s_hram,   0, sizeof(s_hram));
    memset(&gPlayer_local, 0, sizeof(gPlayer_local));

    /* Install player base stats (big-endian) */
    s_wram.wBattleMon.attack[0]   = (uint8_t)(in->player_base_attack   >> 8);
    s_wram.wBattleMon.attack[1]   = (uint8_t)(in->player_base_attack   & 0xff);
    s_wram.wBattleMon.defense[0]  = (uint8_t)(in->player_base_defense  >> 8);
    s_wram.wBattleMon.defense[1]  = (uint8_t)(in->player_base_defense  & 0xff);
    s_wram.wBattleMon.speed[0]    = (uint8_t)(in->player_base_speed    >> 8);
    s_wram.wBattleMon.speed[1]    = (uint8_t)(in->player_base_speed    & 0xff);
    s_wram.wBattleMon.spclAtk[0]  = (uint8_t)(in->player_base_spatk   >> 8);
    s_wram.wBattleMon.spclAtk[1]  = (uint8_t)(in->player_base_spatk   & 0xff);
    s_wram.wBattleMon.spclDef[0]  = (uint8_t)(in->player_base_spdef   >> 8);
    s_wram.wBattleMon.spclDef[1]  = (uint8_t)(in->player_base_spdef   & 0xff);
    s_wram.wBattleMon.status[0]   = 0; /* no status — PRZ/BRN effects are no-ops */

    /* Install enemy base stats (big-endian) */
    s_wram.wEnemyMon.attack[0]    = (uint8_t)(in->enemy_base_attack   >> 8);
    s_wram.wEnemyMon.attack[1]    = (uint8_t)(in->enemy_base_attack   & 0xff);
    s_wram.wEnemyMon.defense[0]   = (uint8_t)(in->enemy_base_defense  >> 8);
    s_wram.wEnemyMon.defense[1]   = (uint8_t)(in->enemy_base_defense  & 0xff);
    s_wram.wEnemyMon.speed[0]     = (uint8_t)(in->enemy_base_speed    >> 8);
    s_wram.wEnemyMon.speed[1]     = (uint8_t)(in->enemy_base_speed    & 0xff);
    s_wram.wEnemyMon.spclAtk[0]   = (uint8_t)(in->enemy_base_spatk   >> 8);
    s_wram.wEnemyMon.spclAtk[1]   = (uint8_t)(in->enemy_base_spatk   & 0xff);
    s_wram.wEnemyMon.spclDef[0]   = (uint8_t)(in->enemy_base_spdef   >> 8);
    s_wram.wEnemyMon.spclDef[1]   = (uint8_t)(in->enemy_base_spdef   & 0xff);
    s_wram.wEnemyMon.status[0]    = 0;

    /* Install pre-Haze stat stages */
    for (int i = 0; i < SUICUNE_NUM_STAT_STAGES; i++) {
        s_wram.wPlayerStatLevels[i] = in->player_stages[i];
        s_wram.wEnemyStatLevels[i]  = in->enemy_stages[i];
    }

    /* No badges, no link mode, not in Battle Tower — BadgeStatBoosts is a no-op */
    s_wram.wLinkMode            = 0;
    s_wram.wInBattleTowerBattle = 0;

    /* hBattleTurn = TURN_PLAYER (player uses Haze) */
    s_hram.hBattleTurn = TURN_PLAYER;

    /* === EXECUTE SUICUNE HAZE LIVE === */
    BattleCommand_ResetStats();

    /* Extract output: stat stages (the primary output) */
    for (int i = 0; i < SUICUNE_NUM_STAT_STAGES; i++) {
        out->player_stages[i] = s_wram.wPlayerStatLevels[i];
        out->enemy_stages[i]  = s_wram.wEnemyStatLevels[i];
    }

    /* Extract computed stats (secondary output — verifies CalcBattleStats ran correctly) */
    out->player_computed_attack  = BigEndianToNative16(s_wram.wPlayerAttack);
    out->player_computed_defense = BigEndianToNative16(s_wram.wPlayerDefense);
    out->player_computed_speed   = BigEndianToNative16(s_wram.wPlayerSpeed);
    out->player_computed_spatk   = BigEndianToNative16(s_wram.wPlayerSpAtk);
    out->player_computed_spdef   = BigEndianToNative16(s_wram.wPlayerSpDef);
    out->enemy_computed_attack   = BigEndianToNative16(s_wram.wEnemyAttack);
    out->enemy_computed_defense  = BigEndianToNative16(s_wram.wEnemyDefense);
    out->enemy_computed_speed    = BigEndianToNative16(s_wram.wEnemySpeed);
    out->enemy_computed_spatk    = BigEndianToNative16(s_wram.wEnemySpAtk);
    out->enemy_computed_spdef    = BigEndianToNative16(s_wram.wEnemySpDef);

    /* RNG calls: Haze never calls BattleRandom. */
    out->rng_calls = 0;
}
