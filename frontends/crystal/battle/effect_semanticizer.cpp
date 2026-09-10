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
            // Record that the effectchance opcode was present: the runtime must consume
            // exactly one RNG byte for this phase, even when effect_chance == 0.
            desc.has_effectchance_phase = true;
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
        // NOTE: case 0x77 (attackup2) is handled in the B-effect section below
        // because Swagger uses switchturn(0x93) for targeting disambiguation.

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

        // ── B-effect opcodes: recognized but handled by SemanticEffectProgram ──
        // These opcodes indicate the script compiles to Architecture B.
        // The program compiler reads them; the semanticizer just notes their presence
        // via the appropriate deferred flag so apply_support_gate can route correctly.
        // NONE of these hit the default: case.

        // ── Corrected: 0x1E = payday (NOT forceswitch) ─────────────────────────
        case 0x1E:  // payday — scatter coins; A-path with has_payday flag
            // Pure A: standard damage + coin accumulation at end of battle.
            // No B classification needed.
            desc.has_payday = true;
            break;

        // ── Corrected: 0x23 = forceswitch ──────────────────────────────────────
        case 0x23:  // forceswitch (Whirlwind, Roar) — ForceSwitch B op
            // ForceSwitch is a B mechanic: forces opponent party switch.
            desc.is_copy_move = true;
            break;

        // ── Corrected: 0x28 = mist (NOT focusenergy) ───────────────────────────
        case 0x28:  // mist — sets SUBSTATUS_MIST; protects from stat drops
            // A-path: sets Mist volatile on user.
            desc.sets_mist = true;
            break;

        // ── Corrected: 0x29 = focusenergy ──────────────────────────────────────
        case 0x29:  // focusenergy — sets SUBSTATUS_FOCUS_ENERGY via volatile
            // A-path: sets FocusEnergy volatile for doubled crit rate.
            desc.sets_focus_energy = true;
            break;

        case 0x31:  // substitute — creates Substitute via B SetVolatile + HP cost
            desc.is_copy_move = true;
            break;

        // ── Corrected: 0x4E = triplekick (NOT ragedamage) ─────────────────────
        case 0x4E:  // triplekick — per-iteration power multiply (×1/×2/×3)
            desc.is_multi_hit = true;  // TripleKick is a multi-hit B variant
            break;

        // ── Corrected: 0x4F = kickcounter (NOT thief) ─────────────────────────
        case 0x4F:  // kickcounter — increments TripleKick iteration counter
            desc.is_multi_hit = true;  // Part of TripleKick multi-hit
            break;

        // ── Corrected: 0x50 = thief ────────────────────────────────────────────
        case 0x50:  // thief — post-damage item transfer via B TransferItem op
            desc.is_copy_move = true;
            break;

        case 0x4A:  // spite — reduce opponent's last-used move PP by 2-5
            // A-path: instant PP reduction on opponent's last-used move slot.
            desc.reduces_pp = true;
            break;

        // ── Corrected: 0x4C = healbell (NOT kickcounter) ──────────────────────
        case 0x4C:  // healbell — cure all party status conditions
            // B-path: requires party iteration.
            desc.is_heal_bell = true;
            break;

        case 0x65:  // checksafeguard — A-path gate; semantic already captured by other opcodes
            // checksafeguard appears in DoSleep/DoPoison/DoParalyze/DoConfuse scripts.
            // The semantic (primary_status) is already set by the status opcode (0x14/0x2f/etc.).
            // This is a pure execution gate. No new flag needed.
            break;

        case 0x9E:  // skipsuncharge — SolarBeam skip-charge-in-sun; charge variant
            // is_charge is already set by charge(0x39) and checkcharge(0x3a).
            // This is a presentation/execution command within the B charge program.
            break;  // presentation: charge flag already set by co-opcodes

        // ── Corrected: 0x5A = endure (NOT skipsuncharge) ──────────────────────
        case 0x5A:  // endure — survive any hit with 1 HP this turn
            // B-path: pre-damage hook.
            desc.is_endure = true;
            break;

        // ── Corrected: 0x77 = attackup2 ────────────────────────────────────────
        case 0x77:  // attackup2 — raise Attack +2 (Swords Dance, Swagger opponent)
            // Swagger uses switchturn(0x93) to target opponent; attackup2 raises
            // the TARGET's Attack +2 for Swagger, or USER's Attack +2 for standalone.
            // The swagger_stat_change flag disambiguates (set when 0x93 precedes 0x77).
            desc.stat_change = StatChangeTarget::AttackUp2;
            break;

        case 0x3C:  // effect0x3c — unused opcode; appears in no stock scripts
            // Safe no-op: this byte appears in the semanticizer case table but
            // in zero stock effect scripts. Explicit break prevents default: hit.
            break;

        // ── Corrected: 0x91 = statdownanim (presentation) ─────────────────────
        case 0x91:  // statdownanim — stat-decrease animation; stat change already set
            break;

        // ── Corrected: 0x92 = statupanim (presentation) ───────────────────────
        case 0x92:  // statupanim — stat-increase animation; stat change already set
            break;

        // ── Corrected: 0x93 = switchturn (Swagger opponent-targeting marker) ───
        case 0x93:  // switchturn — switches perspective for stat application (Swagger)
            // Swagger: switchturn + attackup2(0x77) + switchturn = opponent gets Attack+2.
            // Since attackup2(0x77) still sets stat_change=AttackUp2 on user in apply_command,
            // Swagger needs the stat change applied to OPPONENT instead.
            // Mark with swagger_stat_change so execute_move applies stat_change to target.
            desc.swagger_stat_change = true;
            break;

        // ── Corrected: 0x94 = fakeout (unused in stock moves) ─────────────────
        case 0x94:  // fakeout — flinch if user went first this turn; effect 141
            // No stock vanilla Crystal move uses effect 141.
            // Zero damage; unconditional Flinch on success.
            // Source: pokecrystal EFFECT_FAKE_OUT / BattleCommand_FakeOut.
            desc.is_fake_out = true;
            break;

        // ── Corrected: 0x97 = rage ─────────────────────────────────────────────
        case 0x97:  // rage — set SUBSTATUS_RAGE; scale outgoing damage per accumulator
            // B-path: Rage volatile + RageDamage scaling.
            desc.is_rage = true;
            break;

        // ── Corrected: 0xA1 = beatup (B sub-command; is_multi_hit already set) ─
        case 0xA1:  // beatup — per-party-member damage; B flag already set by startloop
            break;  // is_multi_hit set by startloop(0xAE); beatup is the B sub-command

        // ── Corrected: 0xA2 = ragedamage ───────────────────────────────────────
        case 0xA2:  // ragedamage — scale Rage outgoing damage by accumulator
            // Part of Rage B program. is_rage flag set by rage(0x97) in same script.
            desc.is_rage = true;
            break;

        // ── Corrected: 0xA3 = resettypematchup (presentation) ─────────────────
        case 0xA3:  // resettypematchup — reset type multiplier for constant damage display
            // Presentation: constant_damage_source already set by 0x3F. No new state.
            break;

        // ── New A-path gameplay semantics ──────────────────────────────────────

        case 0x1F:  // conversion — change user type to type of one of user's moves
            desc.changes_user_type = true;
            break;

        case 0x35:  // leechseed — set SUBSTATUS_LEECH_SEED; per-turn drain
            // B-path: EndOfTurn hook needed.
            desc.is_leech_seed = true;
            break;

        case 0x36:  // splash — does absolutely nothing
            // Explicit no-op: Splash has no gameplay effect.
            desc.is_splash = true;  // explicit flag so is_supported=true + A gate fires correctly
            break;

        case 0x37:  // disable — disable target's last-used move for 1-7 turns
            // B-path: per-turn counter + PreMove gate.
            desc.is_disable = true;
            break;

        case 0x41:  // encore — force target to repeat last move for 3-6 turns
            // B-path: move-selection override.
            desc.is_encore = true;
            break;

        case 0x42:  // painsplit — equalize HP between user and target
            desc.equalizes_hp = true;
            break;

        case 0x43:  // snore — deal damage only if user is asleep
            desc.requires_user_asleep = true;
            break;

        case 0x44:  // conversion2 — change user type to type that resists opponent's last move
            desc.changes_user_type_resist = true;
            break;

        case 0x45:  // lockon — next user move always hits target
            // B-path: accuracy override hook consumes volatile on next move.
            desc.is_lock_on = true;
            break;

        case 0x47:  // defrostopponent — thaw frozen opponent, raise user Attack +1
            // A-path: instant stat change + secondary effect.
            // Already handled by stat_change=AttackUp1 via 0x71 defenseup in the
            // DefrostOpponent script? No — this script only has 0x47. Set directly.
            desc.stat_change = StatChangeTarget::AttackUp1;
            desc.secondary_effect = SecondaryEffectType::Defrost;
            break;

        case 0x48:  // sleeptalk — if asleep, pick and use a random move
            // B-path: InvokeRandomMove while asleep.
            desc.is_sleep_talk = true;
            break;

        case 0x49:  // destinybond — if user faints from damage this turn, opponent faints too
            // B-path: PostDamage CheckFaint hook.
            desc.is_destiny_bond = true;
            break;

        case 0x51:  // arenatrap (MeanLook) — set SUBSTATUS_CANT_RUN on opponent
            desc.traps_opponent = true;
            break;

        case 0x52:  // nightmare — opponent loses 1/4 max_hp per turn while asleep
            // B-path: EndOfTurn hook.
            desc.is_nightmare = true;
            break;

        case 0x54:  // curse — non-Ghost: Atk+1/Def+1/Spe-1; Ghost: lose 50% HP + curse target
            // B-path: Ghost path needs EndOfTurn drain hook. Non-Ghost has immediate stats.
            desc.is_curse = true;
            break;

        case 0x55:  // protect — block all damage this turn; consecutive uses halve success chance
            // B-path: PreDamage hook.
            desc.is_protect = true;
            break;

        case 0x57:  // foresight — set SUBSTATUS_IDENTIFIED on opponent; removes immunities
            desc.identifies_opponent = true;
            break;

        case 0x58:  // perishsong — set perish countdown on both; faint when count reaches 0
            // B-path: EndOfTurn hook on both sides.
            desc.is_perish_song = true;
            break;

        case 0x5F:  // attract — set SUBSTATUS_IN_LOVE on opposite-gender opponent
            // B-path: gender check + PreMove 50% skip hook.
            desc.is_attract = true;
            break;

        case 0x64:  // safeguard — protect user's side from primary status for 5 turns
            // A-path: sets field_.safeguard counter (same infrastructure as Reflect/LightScreen).
            desc.sets_safeguard = true;
            break;

        case 0x67:  // batonpass — switch user out while passing stat stages and volatiles
            // B-path: switch + selective volatile copy.
            desc.is_baton_pass = true;
            break;

        case 0x9F:  // thunderaccuracy — modify accuracy based on weather (Rain=100%, Sun=50%)
            // A-path: applied before accuracy roll.
            desc.has_thunder_accuracy = true;
            break;

        case 0xA0:  // teleport — end wild battle if not trapped; fail in trainer battles
            // A-path: instant battle-end in wild battles.
            desc.ends_wild_battle = true;
            break;

        // ── All other opcodes ──────────────────────────────────────────────────
        // An opcode that is not in any case above is genuinely unrecognized.
        // The compiler invariant: unrecognized opcode → compile failure.
        default:
            desc.unrecognized_opcode = true;
            desc.first_unrecognized  = opcode;
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

    // Hard fail: unrecognized opcode — cannot produce a valid semantic description.
    // The caller (move_semanticizer.cpp) checks this and propagates to a compile error.
    if (desc.unrecognized_opcode) {
        desc.is_supported = false;
        return;
    }

    // Architecture B effects: if any B-mechanic flag is set, the effect lowers to
    // SemanticEffectProgram.  Leave is_supported=false so the A execution path is
    // never entered for B effects.  The B dispatch in execute_move() checks
    // has_program BEFORE is_supported and routes correctly regardless of this flag,
    // but keeping is_supported=false preserves the correct semantic: these descriptions
    // are not A-executable.
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
     || desc.is_copy_move
     // New B flags (effects requiring turn hooks, party access, or interception):
     || desc.is_leech_seed
     || desc.is_disable
     || desc.is_encore
     || desc.is_lock_on
     || desc.is_sleep_talk
     || desc.is_destiny_bond
     || desc.is_nightmare
     || desc.is_curse
     || desc.is_protect
     || desc.is_perish_song
     || desc.is_attract
     || desc.is_baton_pass
     || desc.is_heal_bell
     || desc.is_endure
     || desc.is_rage) {
        // is_supported intentionally left false — B path handles execution.
        return;
    }

    // Everything that reaches here is an Architecture A supported effect.
    desc.is_supported = true;
}

} // namespace crystal
