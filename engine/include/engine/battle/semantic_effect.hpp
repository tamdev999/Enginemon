#pragma once
// engine/include/engine/battle/semantic_effect.hpp
//
// SemanticEffectDescription — typed compiler-side description of one move's
// battle semantics, produced by the Crystal frontend from a decoded effect
// script (DecodedEffectScript) and consumed by execute_move() at runtime.
//
// This header has no includes beyond <cstdint> to avoid circular dependencies
// with engine/core/types.hpp (MoveData includes SemanticEffectDescription).

#include <cstdint>

namespace enginemon {

// ============================================================================
// Secondary effect type — what happens to the target on an effectchance roll
// ============================================================================
enum class SecondaryEffectType : uint8_t {
    None        = 0,
    Burn        = 1,
    Freeze      = 2,
    Paralysis   = 3,
    Poison      = 4,
    Flinch      = 5,
    Confusion   = 6,
    AttackDown  = 7,
    DefenseDown = 8,
    SpeedDown   = 9,
    SpAtkDown   = 10,
    SpDefDown   = 11,
    AccuracyDown = 12,
    EvasionDown = 13,
    AttackUp    = 14,
    DefenseUp   = 15,
    AllStatsUp  = 16,
    Defrost     = 17,  // FlameWheel/SacredFire defrost-on-hit before burn check
    Sleep       = 18,  // Tri Attack sleep component
    TriAttack   = 19,  // TriAttack: 1/3 chance burn/freeze/paralysis each
};

// ============================================================================
// Primary status application target
// ============================================================================
enum class PrimaryStatusType : uint8_t {
    None     = 0,
    Sleep    = 1,
    Poison   = 2,
    Toxic    = 3,    // Bad Poison (Toxic)
    Paralysis = 4,
    Confusion = 5,   // Primary confusion (Confuse Ray)
};

// ============================================================================
// Stat change target
// ============================================================================
enum class StatChangeTarget : uint8_t {
    None           = 0,
    AttackUp1      = 1,
    AttackUp2      = 2,
    DefenseUp1     = 3,
    DefenseUp2     = 4,
    SpeedUp1       = 5,
    SpeedUp2       = 6,
    SpAtkUp1       = 7,
    SpAtkUp2       = 8,
    SpDefUp1       = 9,
    SpDefUp2       = 10,
    AccuracyUp1    = 11,
    AccuracyUp2    = 12,
    EvasionUp1     = 13,
    EvasionUp2     = 14,
    AllUp1         = 15,  // AllStatsUp hit (AncientPower etc.)
    AttackDown1    = 16,
    AttackDown2    = 17,
    DefenseDown1   = 18,
    DefenseDown2   = 19,
    SpeedDown1     = 20,
    SpeedDown2     = 21,
    SpAtkDown1     = 22,
    SpAtkDown2     = 23,
    SpDefDown1     = 24,
    SpDefDown2     = 25,
    AccuracyDown1  = 26,
    AccuracyDown2  = 27,
    EvasionDown1   = 28,
    EvasionDown2   = 29,
    Reset          = 30,  // Haze — reset all stages
    CopyOpponent   = 31,  // Psych Up
    MaxAttack      = 32,  // Belly Drum (set attack to +6, cost half HP)
};

// ============================================================================
// Constant-damage source — how ConstantDamage value is computed
// ============================================================================
enum class ConstantDamageSource : uint8_t {
    None         = 0,
    MoveFixed    = 1,  // move_power byte (Dragon Rage=40, etc.)
    UserLevel    = 2,  // floor(user level) (Level Damage / Seismic Toss)
    HalfTargetHP = 3,  // floor(target_hp / 2) max(1) (Super Fang)
    Psywave      = 4,  // random 1..(floor(user_level * 1.5) - 1)
    ReversalFlail = 5, // HP-bar table (Reversal/Flail)
};

// ============================================================================
// Set-power source — power is computed at runtime from a table or stat
// ============================================================================
enum class SetPowerSource : uint8_t {
    None            = 0,
    MagnitudeTable  = 1,  // BattleRules magnitude table (RNG → threshold → power)
    PresentTable    = 2,  // BattleRules present table (RNG → threshold → power or heal)
    HappinessReturn = 3,  // floor(happiness × 10 / 25), max 102
    HappinessFrustration = 4,  // floor((255 - happiness) × 10 / 25), max 102
    HiddenPower     = 5,  // DV-derived power (25–70) — requires DV access
};

// ============================================================================
// Conditional double multiplier condition
// ============================================================================
enum class ConditionalDoubleCondition : uint8_t {
    None        = 0,
    TargetFlying = 1,       // target has Flying volatile (Gust, Twister)
    TargetUnderground = 2,  // target has Underground volatile (Earthquake)
    TargetMinimized = 3,    // target has Minimized volatile (Stomp)
};

// ============================================================================
// Heal source
// ============================================================================
enum class HealSource : uint8_t {
    None            = 0,
    HalfMaxHP       = 1,  // floor(max_hp / 2) — Recover, Softboiled, Milk Drink
    WeatherHealing  = 2,  // 1/2 (sun), 1/4 (neutral), 1/8 (other) — Morning Sun, Synthesis, Moonlight
};

// ============================================================================
// Screen type
// ============================================================================
enum class ScreenType : uint8_t {
    None        = 0,
    Reflect     = 1,
    LightScreen = 2,
};

// ============================================================================
// Weather type (engine-semantic, not Crystal encoding)
// ============================================================================
enum class WeatherSetType : uint8_t {
    None      = 0,
    Rain      = 1,
    Sun       = 2,
    Sandstorm = 3,
};

// ============================================================================
// SemanticEffectDescription
//
// A complete typed description of a move's battle semantics.
// Produced once per move at compile time by the Crystal frontend.
// Stored in the EMON package as part of MoveData.
// Consumed by execute_move() at runtime.
//
// The struct is intentionally flat (no vtable, no inheritance) so it serialises
// cleanly and is trivially copyable for package storage.
// ============================================================================
struct SemanticEffectDescription {

    // ── Damage pipeline ────────────────────────────────────────────────────
    // True if this move uses the standard DamageCalc + Stab + Variation pipeline.
    // All moves with damagecalc in the script set this.  OHKO and ConstantDamage
    // do not use it (they replace the standard formula entirely).
    bool has_standard_damage = false;

    // ── Recoil ──────────────────────────────────────────────────────────────
    // User takes max(1, damage >> recoil_shift) after dealing damage.
    // shift_count sourced from BattleRules (lifted from BattleCommand_Recoil).
    bool has_recoil = false;

    // ── Drain ───────────────────────────────────────────────────────────────
    // User heals max(1, damage >> drain_shift) after dealing damage.
    // shift_count sourced from BattleRules (lifted from SapHealth).
    bool has_drain = false;

    // ── Dream Eater drain ───────────────────────────────────────────────────
    // Like drain, but requires target to have Status::Sleep.
    // If target is not asleep the move fails.
    bool drain_requires_sleep = false;

    // ── Selfdestruct / Explosion ─────────────────────────────────────────────
    // User faints after dealing damage.  Defense halving applied in damage calc
    // (defense_stat >>= selfdestruct_defense_shift before formula).
    bool user_faints = false;

    // ── OHKO ────────────────────────────────────────────────────────────────
    // One-hit KO attempt.  No standard damage pipeline.
    // Accuracy modified by (user_level - target_level) × ohko_level_multiplier.
    // Auto-fails if target level > user level.
    // Sourced from BattleRules (ohko_level_multiplier lifted from BattleCommand_OHKO).
    bool is_ohko = false;

    // ── Cannot KO (False Swipe) ──────────────────────────────────────────────
    // Target always survives with at least 1 HP.
    bool cannot_ko = false;

    // ── Hyper Beam recharge ──────────────────────────────────────────────────
    // User must recharge next turn (cannot move).
    bool sets_recharge = false;

    // ── Constant damage ──────────────────────────────────────────────────────
    // Bypasses the standard damage formula; damage is computed from a fixed
    // source enumerated by constant_damage_source.
    // Type effectiveness still applies (via resettypematchup in some cases).
    ConstantDamageSource constant_damage_source = ConstantDamageSource::None;

    // ── Set power ───────────────────────────────────────────────────────────
    // Power is computed at runtime from a source, injected before damagecalc.
    // set_power_source != None implies has_standard_damage is also true.
    SetPowerSource set_power_source = SetPowerSource::None;

    // ── Conditional double damage ──────────────────────────────────────────
    // Damage is doubled if the target has a specific volatile status.
    ConditionalDoubleCondition conditional_double = ConditionalDoubleCondition::None;

    // ── Secondary effect ────────────────────────────────────────────────────
    // Applied to the target after damage on an effectchance roll.
    // effect_chance is read from MoveData::effect_chance (ROM byte 6).
    SecondaryEffectType secondary_effect = SecondaryEffectType::None;

    // ── Primary status / volatile ───────────────────────────────────────────
    // Applied unconditionally (not on a chance roll) when the move hits.
    // For status-only moves this is the entire effect.
    // Checks Safeguard before applying.
    PrimaryStatusType primary_status = PrimaryStatusType::None;

    // ── Stat change ─────────────────────────────────────────────────────────
    // Applied to the target (stat down) or user (stat up).
    // For status-only moves this is the entire effect.
    StatChangeTarget stat_change = StatChangeTarget::None;

    // ── Healing ──────────────────────────────────────────────────────────────
    // User heals itself (status move — Recover, Morning Sun, etc.).
    // Not the same as drain (drain is from dealing damage).
    HealSource heal_source = HealSource::None;

    // ── Screen setup ─────────────────────────────────────────────────────────
    // Sets Reflect or Light Screen on the user's side.
    ScreenType set_screen = ScreenType::None;

    // ── Weather setup ───────────────────────────────────────────────────────
    // Sets weather condition.
    WeatherSetType set_weather = WeatherSetType::None;

    // ── Spikes ───────────────────────────────────────────────────────────────
    // Sets entry hazard on opponent's side.
    bool sets_spikes = false;

    // ────────────────────────────────────────────────────────────────────────
    // DEFERRED MECHANICS
    // These are semantically classified but require additional state machinery
    // before they can be executed.  The support gate checks has_* flags below.
    // ────────────────────────────────────────────────────────────────────────

    // Multi-hit: startloop/endloop wraps the damage pipeline.
    bool is_multi_hit = false;

    // Charge/two-phase: checkcharge/charge + endturn phase boundary.
    bool is_charge = false;

    // Future Sight delayed damage.
    bool is_future_sight = false;

    // Rampage: random 2–3 turns, confusion after.
    bool is_rampage = false;

    // Rollout/Fury Cutter: escalating power chain.
    bool is_escalating_power = false;

    // Trapping: binds target for 2–5 turns.
    bool is_trapping = false;

    // Counter/Mirror Coat: returns stored last-damage × 2.
    bool is_counter = false;
    bool is_mirror_coat = false;

    // Bide: stores energy, then unleashes stored damage × 2.
    bool is_bide = false;

    // Pursuit: doubled power when target is switching.
    bool is_pursuit = false;

    // Transform / Mimic / Metronome / Sketch.
    bool is_copy_move = false;

    // Rapid Spin: clears hazards after damage.
    bool clears_hazards = false;

    // Spore / Sleep Powder / Lovely Kiss (primary sleep).
    // Same as primary_status == PrimaryStatusType::Sleep; provided as named flag too.
    bool is_sleep_move = false;

    // ────────────────────────────────────────────────────────────────────────
    // COMPILER INVARIANT: unrecognized opcode → compile failure
    // Set by apply_command() default: case when an opcode is not classified.
    // If this is true, the compiler must reject the script with an error.
    // ────────────────────────────────────────────────────────────────────────
    bool    unrecognized_opcode = false;
    uint8_t first_unrecognized  = 0;    // first opcode byte that was not recognized

    // King's Rock flinch — requires held-item dispatch in execute_move.
    // Currently absent from execute_move; marked here as a named gap.
    // When kingsrock is present in the script it should be modeled here,
    // not silently dropped as "presentation".
    bool needs_kingsrock     = false;  // set when script contained kingsrock

    // Substitute interaction — requires substitute_hp usage in execute_move.
    bool needs_substitute    = false;  // set when script contained lowersub/raisesub

    // Rage hooks — requires Rage volatile state tracking.
    bool needs_rage          = false;  // set when script contained buildopponentrage

    // Obedience — deferred global pre-execution gate (not per-script).
    // The semanticizer always sets this false; the runtime adds the check globally.
    // bool needs_obedience — not stored here; always global.

    // ────────────────────────────────────────────────────────────────────────
    // NEW ARCHITECTURE A SEMANTIC FIELDS (wire bytes [36..55])
    // These are new gameplay-semantic fields for effects that were previously
    // reaching default: in the semanticizer. Each has a direct implementation
    // in execute_move(). No new B infrastructure required for these.
    // ────────────────────────────────────────────────────────────────────────

    // Pay Day — scatter coins; accumulate level×2 in per-battle pending_coins counter.
    bool has_payday = false;

    // Focus Energy — set VolatileStatus::FocusEnergy (doubles crit stage in build_crit_stage).
    bool sets_focus_energy = false;

    // Mist — set VolatileStatus::Mist on user; blocks opponent-inflicted stat drops.
    bool sets_mist = false;

    // Safeguard — set field_.safeguard counter for 5 turns; blocks primary status.
    bool sets_safeguard = false;

    // Conversion — change user type to type of one of user's moves (random, non-CURSE).
    bool changes_user_type = false;

    // Conversion2 — change user type to a type that resists opponent's last move.
    bool changes_user_type_resist = false;

    // Pain Split — equalize HP between user and target: each = floor((u+t)/2).
    bool equalizes_hp = false;

    // Snore — deal standard damage only if user is asleep; fails otherwise.
    bool requires_user_asleep = false;

    // Mean Look / ArenaTrap — set VolatileStatus::CantRun on target.
    bool traps_opponent = false;

    // Foresight — set VolatileStatus::Identified on target; removes immunities.
    bool identifies_opponent = false;

    // Spite — reduce PP of target's last-used move by 2-5.
    bool reduces_pp = false;

    // Thunder accuracy — Rain=100%, Sun=50%, else base accuracy.
    bool has_thunder_accuracy = false;

    // Teleport — end wild battle; fails in trainer battles.
    bool ends_wild_battle = false;

    // Swagger disambiguation — AttackUp2 targets the OPPONENT (not user) when set.
    // Set by switchturn(0x93) appearing before attackup2(0x77) in Swagger's script.
    bool swagger_stat_change = false;

    // Jump Kick / Hi Jump Kick — crash damage on accuracy miss.
    // Source: pokecrystal GetFailureResultText EFFECT_JUMP_KICK path.
    // On miss: user takes max(1, computed_hit_damage >> 3). No crash if type immune.
    // Set via raw_effect == EffectId::JUMP_KICK in semanticizer post-fixup.
    bool crash_on_miss = false;

    // SolarBeam / two-turn charge moves that are halved in Rain.
    // Source: suiCune DoWeatherModifiers WeatherMoveModifiers table — EFFECT_SOLARBEAM
    // entry {weather=Rain, effect=151, multiplier=5 (×0.5)}.
    // Set in the semanticizer for EFFECT_SOLARBEAM (raw crystal effect 151).
    // Runtime checks this flag and applies ×0.5 when field_.weather == Rain.
    // This replaces the dead raw-effect-ID lookup in apply_weather_modifier().
    bool halves_in_rain = false;

    // Minimize — sets VolatileStatus::Minimized on the user.
    // Source: suiCune MinimizeDropSub — only fired for move MINIMIZE (0x6b = 107),
    // not for Double Team, even though both share EFFECT_EVASION_UP.
    // Runtime sets Minimized volatile when apply_stat_change sees EvasionUp1+sets_minimize.
    // Stomp (ConditionalDoubleCondition::TargetMinimized) checks VolatileStatus::Minimized.
    bool sets_minimize = false;

    // Splash — explicit no-op (Splash does nothing; is_supported=true, immediate Success).
    bool is_splash = false;

    // ────────────────────────────────────────────────────────────────────────
    // NEW ARCHITECTURE B FLAGS (wire bytes [56..71])
    // These effects require cross-turn state, party access, move interception,
    // or EndOfTurn/PreMove/PreDamage hooks. They compile to SemanticEffectProgram.
    // ────────────────────────────────────────────────────────────────────────

    bool is_leech_seed   = false;  // per-turn drain; EndOfTurn hook
    bool is_disable      = false;  // disable target's last move; PreMove hook
    bool is_encore       = false;  // force target to repeat last move; MoveSelection hook
    bool is_lock_on      = false;  // next user move always hits; accuracy hook
    bool is_sleep_talk   = false;  // invoke random move while asleep; InvokeMove
    bool is_destiny_bond = false;  // if user faints from direct damage, opponent faints too
    bool is_nightmare    = false;  // drain 1/4 max_hp/turn while target is asleep; EndOfTurn
    bool is_curse        = false;  // Ghost: drain 1/4/turn + cost HP; non-Ghost: stat stages
    bool is_protect      = false;  // block all damage this turn; PreDamage hook
    bool is_perish_song  = false;  // set 4-turn countdown on both; EndOfTurn → faint
    bool is_attract      = false;  // 50% skip-turn on opposite gender; PreMove hook
    bool is_baton_pass   = false;  // switch out, passing stat stages and select volatiles
    bool is_heal_bell    = false;  // cure status of all party members; party iteration
    bool is_endure       = false;  // survive any hit with 1 HP; PreDamage hook
    bool is_rage         = false;  // scale outgoing damage by rage_accumulator

    // True when the Crystal effect script contains an explicit `effectchance` command (0x90).
    // Distinct from effect_chance > 0: Sky Attack has effect_chance=0 but its script DOES
    // contain effectchance, so Crystal always consumes one BattleRandom byte for it.
    // The runtime must consume one RNG byte for the effectchance phase whenever this is true,
    // regardless of whether effect_chance > 0.
    // Wire byte [63] bit 3 (0x08) — packed alongside is_heal_bell/is_endure/is_rage.
    bool has_effectchance_phase = false;

    // ────────────────────────────────────────────────────────────────────────
    // AI CLASSIFICATION FIELDS
    // These carry the same information that was previously stored as a single
    // SemEffect::X enum value.  The AI reads them directly; no enum dispatch.
    // ────────────────────────────────────────────────────────────────────────

    uint8_t ai_classification = 0;  // SemEffect:: value for AI lists (SemEffect::Unknown = 0)

    // Fake Out: zero-damage flinch that succeeds only when user went first this turn.
    // Fails if user went second, or if target has Substitute, is asleep, or is frozen.
    // Source: pokecrystal EFFECT_FAKE_OUT / BattleCommand_FakeOut (opcode 0x94).
    // No stock vanilla Crystal move uses effect 141.
    // Serialized: wire byte [63] bit 7 (0x80).
    bool is_fake_out = false;

    // True when the move's Crystal source effect is EFFECT_ALWAYS_HIT (raw=17).
    // Crystal gate: CheckHit exits before BrightPowder and before stat accuracy
    // modifiers — no accuracy RNG is consumed regardless of held items.
    // Distinct from ordinary accuracy=0xFF: a 0xFF move can be reduced by
    // BrightPowder and then triggers one accuracy RNG call; is_always_hit cannot.
    // Serialized: wire byte [64].
    bool is_always_hit = false;

    // ────────────────────────────────────────────────────────────────────────
    // SUPPORT GATE
    // ────────────────────────────────────────────────────────────────────────

    // True when every required semantic in this description has a complete
    // native implementation in execute_move().  Set by the Crystal frontend
    // at package build time.  execute_move() returns UnsupportedSemantic
    // immediately if this is false.
    bool is_supported = false;

    // ────────────────────────────────────────────────────────────────────────
    // Convenience queries
    // ────────────────────────────────────────────────────────────────────────

    bool has_damage() const {
        return has_standard_damage
            || is_ohko
            || constant_damage_source != ConstantDamageSource::None;
    }

    bool has_secondary_effect() const {
        return secondary_effect != SecondaryEffectType::None;
    }

    bool is_status_only() const {
        return !has_damage()
            && (primary_status != PrimaryStatusType::None
                || stat_change != StatChangeTarget::None
                || heal_source != HealSource::None
                || set_screen != ScreenType::None
                || set_weather != WeatherSetType::None
                || sets_spikes);
    }
};

} // namespace enginemon
