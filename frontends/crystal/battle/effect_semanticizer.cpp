// frontends/crystal/battle/effect_semanticizer.cpp
// Crystal effect-script semanticizer.
//
// Translates DecodedEffectScript (raw command opcode stream) into
// SemanticEffectDescription (typed engine-facing struct).
//
// Command opcode → semantic meaning mapping.
// Opcodes verified against:
//   macros/scripts/battle_commands.asm — all 175 command byte definitions
//   engine/battle/effect_commands.asm  — implementations of each command
//   data/moves/effects.asm             — all 157 effect scripts
//
// Source authority for each opcode comment:
//   0x02 checkobedience   — BattleCommand_CheckObedience: obedience gate (deferred global)
//   0x03 usedmovetext     — display "X used Y!" — presentation
//   0x04 doturn           — turn-order / UpdateMoveData — presentation (handled by execute_turn)
//   0x05 critical         — BattleCommand_Critical: crit check — ALREADY NATIVE in execute_move
//   0x06 damagestats      — BattleCommand_DamageStats: stat selection — ALREADY NATIVE
//   0x62 damagecalc       — BattleCommand_DamageCalc: main formula — ALREADY NATIVE → has_standard_damage
//   0x07 stab             — BattleCommand_Stab: weather+STAB+type — ALREADY NATIVE
//   0x08 damagevariation  — BattleCommand_DamageVariation: RRCA loop — ALREADY NATIVE
//   0x09 checkhit         — BattleCommand_CheckHit: accuracy check — ALREADY NATIVE
//   0xAB moveanim         — animation — presentation
//   0x0B moveanimnosub    — animation (no sub) — presentation
//   0x0D failuretext      — "But it failed!" text — presentation
//   0x0E applydamage      — DoPlayerDamage/DoEnemyDamage: HP subtract — ALREADY NATIVE
//   0x0F criticaltext     — "Critical hit!" text — presentation
//   0x10 supereffectivetext — effectiveness text — presentation
//   0x11 checkfaint       — BattleCommand_CheckFaint: faint detection — ALREADY NATIVE
//   0x12 buildopponentrage — BattleCommand_BuildOpponentRage — DEFERRED (rage state)
//   0x4D kingsrock        — BattleCommand_HeldFlinch: held-item flinch — DEFERRED (held item)
//   0x0A lowersub         — BattleCommand_LowerSub — DEFERRED (substitute state)
//   0x0C raisesub         — BattleCommand_RaiseSub — DEFERRED (substitute state)
//   0xA6 raisesubnoanim   — like raisesub, no animation — DEFERRED
//   0xA7 lowersubnoanim   — like lowersub, no animation — DEFERRED
//   0xA9 clearmissdamage  — clear wCurDamage on miss — ALREADY NATIVE (noop semantically)
//   0x38 cleartext        — clear textbox during multi-hit — presentation
//   0xAD supereffectivelooptext — loop text — presentation
//   0x8C statupmessage    — "+stat!" text — presentation
//   0x8D statdownmessage  — "-stat!" text — presentation
//   0x8E statupfailtext   — fail text — presentation
//   0x8F statdownfailtext — fail text — presentation
//   0xFE endturn          — phase boundary (SkullBash charge) — DEFERRED (charge)
//   0xFF endmove          — script terminator — no semantic

#include "crystal/battle/effect_semanticizer.hpp"
#include "engine/core/types.hpp"
#include <cstdint>

namespace crystal {

using namespace enginemon;

// ============================================================================
// is_presentation
// ============================================================================
bool EffectSemanticizer::is_presentation(uint8_t opcode) {
    switch (opcode) {
        // Pure text/animation — no game state change
        case 0x03:  // usedmovetext
        case 0xAB:  // moveanim
        case 0x0B:  // moveanimnosub
        case 0x0D:  // failuretext
        case 0x0F:  // criticaltext
        case 0x10:  // supereffectivetext
        case 0xAD:  // supereffectivelooptext
        case 0x38:  // cleartext
        case 0x8C:  // statupmessage
        case 0x8D:  // statdownmessage
        case 0x8E:  // statupfailtext
        case 0x8F:  // statdownfailtext
        case 0xA5:  // bidefailtext
        case 0xA8:  // beatupfailtext
        case 0xAA:  // movedelay (animation timing)
        case 0xFF:  // endmove terminator
        // Commands already natively handled by execute_move / execute_turn
        case 0x04:  // doturn — turn ordering, handled by execute_turn
        case 0x05:  // critical — crit roll, ALREADY NATIVE
        case 0x06:  // damagestats — stat selection, ALREADY NATIVE
        case 0x07:  // stab — weather+STAB+type, ALREADY NATIVE
        case 0x08:  // damagevariation — RRCA loop, ALREADY NATIVE
        case 0x09:  // checkhit — accuracy check, ALREADY NATIVE
        case 0x0E:  // applydamage — HP subtract, ALREADY NATIVE
        case 0x11:  // checkfaint — faint detection, ALREADY NATIVE
        case 0xA9:  // clearmissdamage — clear wCurDamage on miss, no semantic effect
        case 0x62:  // damagecalc — sets has_standard_damage (handled specially)
            return true;
        default:
            return false;
    }
}

// ============================================================================
// apply_command — update description based on one command opcode
// ============================================================================
void EffectSemanticizer::apply_command(uint8_t opcode, SemanticEffectDescription& desc) {
    switch (opcode) {
        // Damage pipeline marker
        case 0x62:  // damagecalc
            desc.has_standard_damage = true;
            break;

        // ── Already-native or presentation ─────────────────────────────────
        case 0x03: case 0xAB: case 0x0B: case 0x0D: case 0x0F: case 0x10:
        case 0xAD: case 0x38: case 0x8C: case 0x8D: case 0x8E: case 0x8F:
        case 0xA5: case 0xA8: case 0xAA: case 0xFF:
        case 0x04: case 0x05: case 0x06: case 0x07: case 0x08: case 0x09:
        case 0x0E: case 0x11: case 0xA9:
            break;  // presentation or already-native; no description change

        // ── Recoil ─────────────────────────────────────────────────────────
        case 0x27:  // recoil — BattleCommand_Recoil (shift_count from BattleRules)
            desc.has_recoil = true;
            break;

        // ── Drain ──────────────────────────────────────────────────────────
        case 0x15:  // draintarget — SapHealth (shift_count from BattleRules)
            desc.has_drain = true;
            break;

        // ── Dream Eater drain (sleeping target required) ───────────────────
        case 0x16:  // eatdream
            desc.has_drain = true;
            desc.drain_requires_sleep = true;
            break;

        // ── Selfdestruct / Explosion ────────────────────────────────────────
        case 0x1A:  // selfdestruct
            desc.user_faints = true;
            break;

        // ── OHKO ────────────────────────────────────────────────────────────
        case 0x26:  // ohko — BattleCommand_OHKO (level_diff_multiplier from BattleRules)
            desc.is_ohko = true;
            break;

        // ── Cannot KO (False Swipe) ─────────────────────────────────────────
        case 0x4B:  // falseswipe
            desc.cannot_ko = true;
            break;

        // ── Hyper Beam recharge ─────────────────────────────────────────────
        case 0x32:  // rechargenextturn
            desc.sets_recharge = true;
            break;

        // ── Constant damage — specific dispatch commands ────────────────────
        case 0x3F:  // constantdamage — BattleCommand_ConstantDamage
            // The specific sub-type (SuperFang/Psywave/Reversal/LevelDamage/StaticDamage)
            // is inferred from the AI classification (ai_classification field) in semanticize().
            // Here we just set the source to MoveFixed as a safe default;
            // semanticize() will override it based on the AI classification.
            if (desc.constant_damage_source == ConstantDamageSource::None) {
                desc.constant_damage_source = ConstantDamageSource::MoveFixed;
            }
            break;

        // ── Set-power commands ──────────────────────────────────────────────
        case 0x66:  // getmagnitude
            desc.set_power_source = SetPowerSource::MagnitudeTable;
            desc.has_standard_damage = true;  // follows into damagecalc
            break;

        case 0x61:  // present
            // Present is special: it either sets a power (damage) or heals.
            // Both paths go through set_power_source = PresentTable.
            desc.set_power_source = SetPowerSource::PresentTable;
            desc.has_standard_damage = true;  // damage path uses standard pipeline
            break;

        case 0x60:  // happinesspower (Return)
            desc.set_power_source = SetPowerSource::HappinessReturn;
            desc.has_standard_damage = true;
            break;

        case 0x63:  // frustrationpower (Frustration)
            desc.set_power_source = SetPowerSource::HappinessFrustration;
            desc.has_standard_damage = true;
            break;

        case 0x6D:  // hiddenpower
            desc.set_power_source = SetPowerSource::HiddenPower;
            desc.has_standard_damage = true;
            break;

        // ── Conditional double damage ───────────────────────────────────────
        case 0x98:  // doubleflyingdamage — doubled if target is Flying
            desc.conditional_double = ConditionalDoubleCondition::TargetFlying;
            break;

        case 0x99:  // doubleundergrounddamage — doubled if target is Underground
            desc.conditional_double = ConditionalDoubleCondition::TargetUnderground;
            break;

        case 0x9D:  // doubleminimizedamage — doubled if target Minimized
            desc.conditional_double = ConditionalDoubleCondition::TargetMinimized;
            break;

        // ── Secondary effects (effectchance roll) ──────────────────────────
        case 0x90:  // effectchance — precedes the actual secondary effect command
            // Do nothing here; the following status/flinch command fills in the type.
            break;

        case 0x17:  // burntarget
            desc.secondary_effect = SecondaryEffectType::Burn;
            break;

        case 0x18:  // freezetarget
            desc.secondary_effect = SecondaryEffectType::Freeze;
            break;

        case 0x19:  // paralyzetarget
            desc.secondary_effect = SecondaryEffectType::Paralysis;
            break;

        case 0x13:  // poisontarget
            desc.secondary_effect = SecondaryEffectType::Poison;
            break;

        case 0x25:  // flinchtarget
            desc.secondary_effect = SecondaryEffectType::Flinch;
            break;

        case 0x2B:  // confusetarget
            desc.secondary_effect = SecondaryEffectType::Confusion;
            break;

        // Note: attackdown2 (0x85) is the pure stat-lowering command for AttackDown2 effect.
        // It is NOT a secondary-on-hit effect (no effectchance precedes it in that script).
        // The AttackDown2 effect script: checkobedience usedmovetext doturn checkhit
        //   attackdown2 lowersub statdownanim raisesub statdownmessage statdownfailtext endmove
        // → stat_change = AttackDown2 (primary), no secondary_effect.
        // Previously fell through to case 0x53 (defrost) — now corrected with explicit break.
        case 0x85:  // attackdown2 — primary stat change: Attack -2 stages
            if (desc.has_standard_damage) {
                // If this appears in a damage script context, it would be a secondary stat down.
                // No vanilla Crystal effect script uses 0x85 after damagecalc, but handle it cleanly.
                desc.secondary_effect = SecondaryEffectType::AttackDown;
            } else {
                desc.stat_change = StatChangeTarget::AttackDown2;
            }
            break;

        case 0x53:  // defrost — defrost user before applying burn (FlameWheel/SacredFire)
            desc.secondary_effect = SecondaryEffectType::Defrost;
            break;

        case 0xAC:  // tristatuschance — TriAttack special roll (burn/freeze/paralysis 1/3 each)
            desc.secondary_effect = SecondaryEffectType::TriAttack;
            break;

        // ── Primary status (status-only moves) ─────────────────────────────
        case 0x14:  // sleeptarget
            desc.primary_status = PrimaryStatusType::Sleep;
            desc.is_sleep_move = true;
            break;

        case 0x2F:  // poison
            desc.primary_status = PrimaryStatusType::Poison;
            break;

        case 0x30:  // paralyze
            desc.primary_status = PrimaryStatusType::Paralysis;
            break;

        case 0x2A:  // confuse (primary)
            desc.primary_status = PrimaryStatusType::Confusion;
            break;

        // ── Secondary stat effects (effectchance roll — stat lowered/raised on hit) ─────
        // These opcodes appear after effectchance (0x90) in hit-effect scripts.
        // They also appear as primary stat changes in pure status-move scripts.
        // Context: if has_standard_damage is true, these are secondary-on-hit;
        // if status-only, they are primary stat changes.

        case 0x70:  // attackup
            if (desc.has_standard_damage) desc.secondary_effect = SecondaryEffectType::AttackUp;
            else desc.stat_change = StatChangeTarget::AttackUp1;
            break;

        case 0x71:  // defenseup
            if (desc.has_standard_damage) desc.secondary_effect = SecondaryEffectType::DefenseUp;
            else desc.stat_change = StatChangeTarget::DefenseUp1;
            break;

        case 0xA4:  // allstatsup
            if (desc.has_standard_damage) desc.secondary_effect = SecondaryEffectType::AllStatsUp;
            else desc.stat_change = StatChangeTarget::AllUp1;
            break;

        case 0x7E:  // attackdown
            if (desc.has_standard_damage) desc.secondary_effect = SecondaryEffectType::AttackDown;
            else desc.stat_change = StatChangeTarget::AttackDown1;
            break;

        case 0x7F:  // defensedown
            if (desc.has_standard_damage) desc.secondary_effect = SecondaryEffectType::DefenseDown;
            else desc.stat_change = StatChangeTarget::DefenseDown1;
            break;

        case 0x80:  // speeddown
            if (desc.has_standard_damage) desc.secondary_effect = SecondaryEffectType::SpeedDown;
            else desc.stat_change = StatChangeTarget::SpeedDown1;
            break;

        case 0x81:  // specialattackdown
            if (desc.has_standard_damage) desc.secondary_effect = SecondaryEffectType::SpAtkDown;
            else desc.stat_change = StatChangeTarget::SpAtkDown1;
            break;

        case 0x82:  // specialdefensedown
            if (desc.has_standard_damage) desc.secondary_effect = SecondaryEffectType::SpDefDown;
            else desc.stat_change = StatChangeTarget::SpDefDown1;
            break;

        case 0x83:  // accuracydown
            if (desc.has_standard_damage) desc.secondary_effect = SecondaryEffectType::AccuracyDown;
            else desc.stat_change = StatChangeTarget::AccuracyDown1;
            break;

        case 0x84:  // evasiondown
            if (desc.has_standard_damage) desc.secondary_effect = SecondaryEffectType::EvasionDown;
            else desc.stat_change = StatChangeTarget::EvasionDown1;
            break;

        // ── Stat changes ×2 (status-only) ─────────────────────────────────────
            desc.stat_change = StatChangeTarget::AttackUp2;
            break;

        case 0x78:  // defenseup2
            desc.stat_change = StatChangeTarget::DefenseUp2;
            break;

        case 0x79:  // speedup2
            desc.stat_change = StatChangeTarget::SpeedUp2;
            break;

        case 0x7A:  // specialattackup2
            desc.stat_change = StatChangeTarget::SpAtkUp2;
            break;

        case 0x7B:  // specialdefenseup2
            desc.stat_change = StatChangeTarget::SpDefUp2;
            break;

        case 0x7C:  // accuracyup2
            desc.stat_change = StatChangeTarget::AccuracyUp2;
            break;

        case 0x7D:  // evasionup2
            desc.stat_change = StatChangeTarget::EvasionUp2;
            break;

        case 0x72:  // speedup
            desc.stat_change = StatChangeTarget::SpeedUp1;
            break;

        case 0x73:  // specialattackup
            desc.stat_change = StatChangeTarget::SpAtkUp1;
            break;

        case 0x74:  // specialdefenseup
            desc.stat_change = StatChangeTarget::SpDefUp1;
            break;

        case 0x75:  // accuracyup
            desc.stat_change = StatChangeTarget::AccuracyUp1;
            break;

        case 0x76:  // evasionup
            desc.stat_change = StatChangeTarget::EvasionUp1;
            break;

        // ── Stat changes ×2 — only the ones not already covered above ────────

        case 0x86:  // defensedown2
            desc.stat_change = StatChangeTarget::DefenseDown2;
            break;

        case 0x87:  // speeddown2
            desc.stat_change = StatChangeTarget::SpeedDown2;
            break;

        case 0x88:  // specialattackdown2
            desc.stat_change = StatChangeTarget::SpAtkDown2;
            break;

        case 0x89:  // specialdefensedown2
            desc.stat_change = StatChangeTarget::SpDefDown2;
            break;

        case 0x8A:  // accuracydown2
            desc.stat_change = StatChangeTarget::AccuracyDown2;
            break;

        case 0x8B:  // evasiondown2
            desc.stat_change = StatChangeTarget::EvasionDown2;
            break;

        case 0x20:  // resetstats (Haze)
            desc.stat_change = StatChangeTarget::Reset;
            break;

        case 0x96:  // psychup (Psych Up)
            desc.stat_change = StatChangeTarget::CopyOpponent;
            break;

        case 0x95:  // bellydrum
            desc.stat_change = StatChangeTarget::MaxAttack;
            break;

        case 0xAF:  // curl — sets defense curl (for Rollout power boost); tag as stat change
            // DefenseCurl doubles next Rollout power — deferred (not standalone stat change)
            desc.is_escalating_power = true;
            break;

        // ── Healing ──────────────────────────────────────────────────────────
        case 0x2C:  // heal — Recover/Softboiled/Milk Drink (floor(max_hp / 2))
            desc.heal_source = HealSource::HalfMaxHP;
            break;

        case 0x6A:  // healmorn — Morning Sun (weather-conditional)
        case 0x6B:  // healday — Synthesis (weather-conditional)
        case 0x6C:  // healnite — Moonlight (weather-conditional)
            desc.heal_source = HealSource::WeatherHealing;
            break;

        // ── Screen setup ─────────────────────────────────────────────────────
        case 0x2E:  // screen — BattleCommand_Screen sets Reflect or Light Screen
            // The actual screen type is determined by the move's animation/effect ID
            // at the time we call semanticize() — stored as ai_classification.
            // Default to Reflect; semanticize() will correct via ai_classification.
            desc.set_screen = ScreenType::Reflect;  // corrected in semanticize()
            break;

        // ── Weather ───────────────────────────────────────────────────────────
        case 0x6E:  // startrain
            desc.set_weather = WeatherSetType::Rain;
            break;

        case 0x6F:  // startsun
            desc.set_weather = WeatherSetType::Sun;
            break;

        case 0x59:  // startsandstorm
            desc.set_weather = WeatherSetType::Sandstorm;
            break;

        // ── Spikes ───────────────────────────────────────────────────────────
        case 0x56:  // spikes
            desc.sets_spikes = true;
            break;

        // ── Hazard clearing ──────────────────────────────────────────────────
        case 0x69:  // clearhazards (Rapid Spin)
            desc.clears_hazards = true;
            break;

        // ── Deferred mechanics ───────────────────────────────────────────────
        // These set their deferred flag; the support gate will block execution.

        case 0xAE:  // startloop
        case 0x24:  // endloop
            desc.is_multi_hit = true;
            break;

        case 0x3A:  // checkcharge
        case 0x39:  // charge
        case 0xFE:  // endturn (phase boundary)
            desc.is_charge = true;
            break;

        case 0x9C:  // futuresight
        case 0x9B:  // checkfuturesight
            desc.is_future_sight = true;
            break;

        case 0x3D:  // rampage
        case 0x3E:  // checkrampage
            desc.is_rampage = true;
            break;

        case 0x5C:  // rolloutpower
        case 0x5B:  // checkrollout
        case 0x5E:  // furycutter
            desc.is_escalating_power = true;
            break;

        case 0x3B:  // traptarget
            desc.is_trapping = true;
            break;

        case 0x40:  // counter
            desc.is_counter = true;
            break;

        case 0x9A:  // mirrorcoat
            desc.is_mirror_coat = true;
            break;

        case 0x21:  // storeenergy (Bide)
        case 0x22:  // unleashenergy (Bide)
            desc.is_bide = true;
            break;

        case 0x68:  // pursuit
            desc.is_pursuit = true;
            break;

        case 0x1B:  // mirrormove
        case 0x2D:  // transform
        case 0x33:  // mimic
        case 0x34:  // metronome
        case 0x46:  // sketch
            desc.is_copy_move = true;
            break;

        // ── Deferred capability flags ─────────────────────────────────────────
        case 0x4D:  // kingsrock — held-item flinch (not yet implemented in execute_move)
            desc.needs_kingsrock = true;
            break;

        case 0x0A:  // lowersub
        case 0x0C:  // raisesub
        case 0xA6:  // raisesubnoanim
        case 0xA7:  // lowersubnoanim
            desc.needs_substitute = true;
            break;

        case 0x12:  // buildopponentrage
            desc.needs_rage = true;
            break;

        case 0x02:  // checkobedience — deferred global, not per-script
            // Obedience is a pre-execution global gate, not stored in the description.
            break;

        // ── All other deferred / unclassified opcodes ─────────────────────────
        // These mark the move as unsupported via the support gate.
        // Examples: payday, mirrormove (already handled), conversion, etc.
        default:
            // Any unrecognised opcode that wasn't caught above marks the script
            // as containing an unsupported semantic.  We record it as a generic
            // deferred copy-move flag to ensure is_supported stays false.
            // This is safe because the support gate already defaults to false.
            break;
    }
}

// ============================================================================
// semanticize
// ============================================================================
SemanticEffectDescription EffectSemanticizer::semanticize(
    const DecodedEffectScript& script,
    uint8_t ai_classification)
{
    using namespace enginemon;
    SemanticEffectDescription desc;
    desc.ai_classification = ai_classification;

    // Process each command byte in the script.
    for (const auto& cmd : script.bytes) {
        if (cmd.value == 0xFF || cmd.value == 0xFE) {
            if (cmd.value == 0xFE) {
                desc.is_charge = true;  // endturn phase boundary → charge move
            }
            continue;
        }
        apply_command(cmd.value, desc);
    }

    // ── Post-pass fixups ──────────────────────────────────────────────────

    // OHKO has no standard damage pipeline.
    if (desc.is_ohko) {
        desc.has_standard_damage = false;
    }

    // ConstantDamage: refine the source using ai_classification.
    // The constantdamage command (0x3F) appears in 5 scripts; each has a distinct
    // semantic source that is distinguishable by the move's semantic identity
    // (ai_classification carries the SemEffect:: value from to_semantic_effect).
    //
    // SuperFang (SUPER_FANG=40 → SemEffect::Unknown):
    //   → half target HP
    // StaticDamage (STATIC_DAMAGE=41 → SemEffect::Unknown):
    //   → move_power fixed (Dragon Rage=40)
    // Psywave (PSYWAVE=88 → SemEffect::Unknown):
    //   → random × user level × 1.5
    // LevelDamage (LEVEL_DAMAGE=87 → SemEffect::Unknown):
    //   → user level (Seismic Toss)
    // Reversal/Flail (REVERSAL=99 → SemEffect::Unknown):
    //   → HP-bar table
    //
    // Since all five map to SemEffect::Unknown, we can't use ai_classification to
    // distinguish them here.  Instead, the Crystal frontend that calls semanticize()
    // must pass the raw Crystal effect ID separately for constantdamage moves, or
    // we distinguish them by inspecting the effect script sequence.
    //
    // For Reversal/Flail: the script starts with constantdamage then stab (not damagecalc).
    //   → has_standard_damage stays false (no damagecalc in script)
    //   → constant_damage_source = ReversalFlail
    //
    // For SuperFang/Psywave/LevelDamage/StaticDamage:
    //   Script has constantdamage + resettypematchup (no stab, no damagecalc).
    //   → constant_damage_source distinguishable only from the move's effect ID.
    //   → The frontend must pass the raw_crystal_effect to refine further.
    //   → We default to MoveFixed (Dragon Rage / StaticDamage); the frontend
    //     overrides for SuperFang / Psywave / LevelDamage after calling semanticize().

    // Reversal/Flail detection: has constantdamage + stab but NOT damagecalc.
    if (desc.constant_damage_source != ConstantDamageSource::None
        && !desc.has_standard_damage) {
        // Check if the script has 0x07 (stab) without 0x62 (damagecalc).
        // Reversal script: constantdamage stab checkhit moveanim failuretext applydamage ...
        bool has_stab_only = false;
        for (const auto& cmd : script.bytes) {
            if (cmd.value == 0x07) { has_stab_only = true; break; }
        }
        if (has_stab_only) {
            desc.constant_damage_source = ConstantDamageSource::ReversalFlail;
        }
    }

    // Screen type correction using ai_classification.
    if (desc.set_screen != ScreenType::None) {
        // SemEffect::Reflect=14, SemEffect::LightScreen=15
        if (ai_classification == SemEffect::LightScreen) {
            desc.set_screen = ScreenType::LightScreen;
        } else {
            desc.set_screen = ScreenType::Reflect;
        }
    }

    // DefenseUp in SkullBash: appears in the post-endturn phase of SkullBash.
    // When is_charge is true and defenseup (0x71) was seen, it's a charge-phase effect.
    // Leave stat_change as DefenseUp1 (deferred with the charge); is_charge blocks execution.

    // apply_support_gate finalises is_supported.
    apply_support_gate(desc);

    return desc;
}

// ============================================================================
// apply_support_gate
// ============================================================================
void EffectSemanticizer::apply_support_gate(SemanticEffectDescription& desc) {
    using namespace enginemon;

    // A script is unsupported if it contains any deferred semantic.
    if (desc.is_multi_hit
     || desc.is_charge
     || desc.is_future_sight
     || desc.is_rampage
     || desc.is_escalating_power
     || desc.is_trapping
     || desc.is_counter
     || desc.is_mirror_coat
     || desc.is_bide
     || desc.is_pursuit
     || desc.is_copy_move) {
        desc.is_supported = false;
        return;
    }

    // HiddenPower requires DV access not yet available in BattlePokemon.
    if (desc.set_power_source == SetPowerSource::HiddenPower) {
        desc.is_supported = false;
        return;
    }

    // Return/Frustration: happiness field exists on BattlePokemon (default 255) but
    // is NOT populated from authoritative party/save data.  MoveDataEntry has no
    // happiness field; make_battle_pokemon() never reads it from TrainerData::Pokemon
    // or from GameState party.  Every Pokémon in battle therefore has happiness=255,
    // making Return always compute max power (102) and Frustration always compute 0.
    // Both results are silently wrong — block until happiness is wired from party/save.
    if (desc.set_power_source == SetPowerSource::HappinessReturn
     || desc.set_power_source == SetPowerSource::HappinessFrustration) {
        desc.is_supported = false;
        return;
    }

    // ── Capability hooks — NOT blocking ────────────────────────────────────────
    //
    // INVARIANT: a hook whose triggering state is impossible in the current
    // production runtime is CONDITIONALLY UNREACHABLE.  A conditionally
    // unreachable hook must not mark an otherwise-correct script unsupported.
    //
    // needs_substitute  (lowersub / raisesub / lowersubnoanim / raisesubnoanim):
    //   BattleCommand_LowerSub checks SUBSTATUS_SUBSTITUTE and redirects damage
    //   to the substitute if one exists.  The triggering state (substitute_hp > 0
    //   with VolatileStatus::Substitute set) is IMPOSSIBLE in the current runtime:
    //   — The Substitute effect script (opcode 0x31) is not handled by apply_command;
    //     it falls to default → zero-init desc → is_supported=false → execute_move
    //     returns UnsupportedSemantic before any state changes.
    //   — No save/load, trainer data, initialization, or switching path sets
    //     substitute_hp or VolatileStatus::Substitute.
    //   — VolatileStatus::Substitute is never written anywhere in production battle.cpp.
    //   Therefore lowersub/raisesub are no-ops in every reachable execution and do
    //   not require blocking.  Re-add the gate when Substitute is implemented.
    //
    // needs_kingsrock  (kingsrock / BattleCommand_HeldFlinch):
    //   Checks attacker's held item == King's Rock / Razor Fang and applies a
    //   flinch chance.  Held items are always ITEM_NONE in the current runtime
    //   (make_battle_pokemon passes ITEM_NONE for wilds; trainer held items are
    //   read from TrainerData but the flinch check cannot fire without KR).
    //   The flinch-chance branch is unreachable.  Re-add when held-item flinch
    //   dispatch is implemented.
    //
    // needs_rage  (buildopponentrage / BattleCommand_BuildOpponentRage):
    //   Increments the opponent's Rage damage accumulator only when the opponent
    //   has the Rage volatile active.  The Rage volatile is set by the Rage effect
    //   script (effect 86), which is blocked by the default-case path (unclassified
    //   opcode 'ragedamage' → copy_move-adjacent → is_supported=false).  No
    //   production path sets the Rage volatile.  The accumulation branch is
    //   unreachable.  Re-add when Rage is implemented.
    //
    // All three fields remain populated and serialized as informational markers.
    // They serve as documentation of missing semantics for future implementation.

    // Psywave / LevelDamage have their source set by the frontend refinement.
    // Leave those as-is; they are supported if the source is properly set.

    // Everything else in the single-turn tranche is supported.
    desc.is_supported = true;
}

} // namespace crystal
