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
    // CAPABILITY FLAGS
    // These record whether the required runtime machinery currently exists.
    // execute_move() checks the corresponding has_* flag before applying an effect.
    // The Crystal frontend sets them based on the current engine capability set.
    // ────────────────────────────────────────────────────────────────────────

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
    // AI CLASSIFICATION FIELDS
    // These carry the same information that was previously stored as a single
    // SemEffect::X enum value.  The AI reads them directly; no enum dispatch.
    // ────────────────────────────────────────────────────────────────────────

    uint8_t ai_classification = 0;  // SemEffect:: value for AI lists (SemEffect::Unknown = 0)

    // ────────────────────────────────────────────────────────────────────────
    // SUPPORT GATE
    // ────────────────────────────────────────────────────────────────────────

    // True when every required semantic in this description has a complete
    // native implementation in execute_move().  Set by the Crystal frontend
    // at package build time.  execute_move() returns UnsupportedSemantic
    // immediately if this is false.
    //
    // Specifically: a script is unsupported if it contains any deferred semantic
    // (is_multi_hit, is_charge, is_future_sight, is_rampage, is_escalating_power,
    //  is_trapping, is_counter, is_mirror_coat, is_bide, is_pursuit, is_copy_move)
    // OR if its set_power_source / constant_damage_source requires data not yet
    // available (e.g. HiddenPower requires DV access).
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
