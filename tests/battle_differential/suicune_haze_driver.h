/*
 * suicune_haze_driver.h
 *
 * Public C API for the SuiCune live Haze driver.
 * Included by both the C driver and the C++ test.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* Number of stat stages per side tracked by the driver.
 * Matches SuiCune's NUM_LEVEL_STATS (8: ATK, DEF, SPD, SATK, SDEF, ACC, EVA, ABILITY).
 * We expose 7 meaningful ones (indices 0–6); index 7 (ABILITY/Curse slot) is also
 * set to BASE_STAT_LEVEL by Haze but not semantically meaningful here. */
#define SUICUNE_NUM_STAT_STAGES  8

/*
 * SuicuneHazeInput: pre-Haze battle state.
 *
 * stat stages: BASE_STAT_LEVEL (7) = neutral, range [1..13].
 * base_attack/defense/speed/spatk/spdef: raw base stats (native endian).
 */
typedef struct {
    /* Stat stages before Haze (SuiCune encoding: 7=neutral) */
    uint8_t  player_stages[SUICUNE_NUM_STAT_STAGES];
    uint8_t  enemy_stages[SUICUNE_NUM_STAT_STAGES];
    /* Base stats for CalcBattleStats (neutral stage → computed = base) */
    uint16_t player_base_attack;
    uint16_t player_base_defense;
    uint16_t player_base_speed;
    uint16_t player_base_spatk;
    uint16_t player_base_spdef;
    uint16_t enemy_base_attack;
    uint16_t enemy_base_defense;
    uint16_t enemy_base_speed;
    uint16_t enemy_base_spatk;
    uint16_t enemy_base_spdef;
} SuicuneHazeInput;

/*
 * SuicuneHazeOutput: post-Haze observable state.
 *
 * stat stages: after Haze all should be BASE_STAT_LEVEL (7).
 * computed_*: calculated stat values after stage reset.
 * rng_calls: number of BattleRandom calls (always 0 for Haze).
 */
typedef struct {
    uint8_t  player_stages[SUICUNE_NUM_STAT_STAGES];
    uint8_t  enemy_stages[SUICUNE_NUM_STAT_STAGES];
    uint16_t player_computed_attack;
    uint16_t player_computed_defense;
    uint16_t player_computed_speed;
    uint16_t player_computed_spatk;
    uint16_t player_computed_spdef;
    uint16_t enemy_computed_attack;
    uint16_t enemy_computed_defense;
    uint16_t enemy_computed_speed;
    uint16_t enemy_computed_spatk;
    uint16_t enemy_computed_spdef;
    uint32_t rng_calls;
} SuicuneHazeOutput;

/*
 * suicune_haze_run:
 *   Execute BattleCommand_ResetStats (Haze) live using SuiCune source code.
 *   Writes post-Haze observable state to *out.
 */
void suicune_haze_run(const SuicuneHazeInput* in, SuicuneHazeOutput* out);

#ifdef __cplusplus
} /* extern "C" */
#endif
