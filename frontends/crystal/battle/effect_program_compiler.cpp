// frontends/crystal/battle/effect_program_compiler.cpp
//
// EffectProgramCompiler — Architecture B effect compiler.
// Converts DecodedEffectScript + SemanticEffectDescription flags into a
// SemanticEffectProgram (ordered list of engine-semantic BOp values).
//
// All 30 B effects are lowered here. The resulting program contains no Crystal
// opcodes, effect IDs, or ROM addresses.

#include "crystal/battle/effect_program_compiler.hpp"
#include "crystal/battle/crystal_effects.hpp"  // EffectId:: raw constants
#include <stdexcept>

namespace crystal {

using namespace enginemon;

// ============================================================================
// needs_program — structural A/B selection rule
// ============================================================================
bool EffectProgramCompiler::needs_program(const SemanticEffectDescription& desc) {
    // Architecture B: any deferred-mechanic flag is set.
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
     // New B flags (added in v4):
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
        return true;
    }
    return false;
}

// ============================================================================
// compile
// ============================================================================
EffectProgramResult EffectProgramCompiler::compile(
    const SemanticEffectDescription& desc,
    const DecodedEffectScript& script,
    uint8_t raw_effect,
    uint8_t ai_class)
{
    EffectProgramResult result;
    SemanticEffectProgram& prog = result.program;
    prog.ai_classification = ai_class;
    auto& ops = prog.ops;

    // Helper lambdas for common volatile bits.
    auto setVS = [&](VolatileStatus vs, BVolatileTarget tgt, bool val) {
        ops.push_back(BOp::SetVolatileBit(static_cast<uint32_t>(vs), tgt, val));
    };
    auto userVS   = [&](VolatileStatus vs, bool v){ setVS(vs, BVolatileTarget::User,     v); };
    auto oppVS    = [&](VolatileStatus vs, bool v){ setVS(vs, BVolatileTarget::Opponent,  v); };

    // ── Bide ─────────────────────────────────────────────────────────────────
    // Crystal: UnleashEnergy sets counter = rand&1 + 2 = 2 or 3 turns.
    // InitCounter(Bide, 2, 3) matches.
    if (desc.is_bide) {
        ops.push_back(BOp::InitCounter(BCounterKind::Bide, 2, 3));
        userVS(VolatileStatus::Bide, true);
        ops.push_back(BOp::Damage(BDamageSource::StoredEnergy));
        result.success = true;
        prog.is_compiled = true;
        return result;
    }

    // ── Rampage (Thrash, Petal Dance, Outrage) ───────────────────────────────
    // Crystal: BattleCommand_Rampage sets counter = rand&1 + 1 = 1 or 2.
    // FIXED: was (2,3) which is wrong. Crystal counter is rand&1+1 = 1 or 2.
    // Total attack turns: 2 (counter=1) or 3 (counter=2).
    // The engine RampageEndCheck hook decrements the counter and applies confusion.
    // BattleCommand_Rampage does NOT re-execute on continuation (SkipToBattleCommand
    // skips PAST the rampage byte; only the damage phase runs on continuation turns).
    if (desc.is_rampage) {
        userVS(VolatileStatus::Rampage, true);
        ops.push_back(BOp::InitCounter(BCounterKind::Rampage, 1, 2));
        ops.push_back(BOp::Damage(BDamageSource::Standard));
        result.success = true;
        prog.is_compiled = true;
        return result;
    }

    // ── Trapping (Wrap, Bind, Clamp, Fire Spin, Whirlpool) ───────────────────
    // Crystal: TrapTarget counter = (rand & 3) + 3 = 3..6.
    // FIXED: was (2,5) which is wrong. Crystal code: "and %11; inc a; inc a; inc a"
    // = (rand & 3) + 3. Residual damage ticks = counter - 1 (no damage on release tick).
    if (desc.is_trapping) {
        ops.push_back(BOp::Damage(BDamageSource::Standard));
        ops.push_back(BOp::InitCounter(BCounterKind::Trapping, 3, 6));
        oppVS(VolatileStatus::Trapped, true);
        result.success = true;
        prog.is_compiled = true;
        return result;
    }

    // ── Multi-Hit (MultiHit, PoisonMultiHit, TripleKick, BeatUp) ─────────────
    if (desc.is_multi_hit) {
        // BeatUp — reads party-member stats per iteration.
        if (raw_effect == crystal::EffectId::BEAT_UP) {
            // BeatUp runtime hit count determined at execute_program time from party.
            // InitCounter(HitLoop,1,1) is a placeholder; execute_program overrides it.
            ops.push_back(BOp::InitCounter(BCounterKind::HitLoop, 1, 1));
            ops.push_back(BOp::Damage(BDamageSource::BeatUpMember));
            result.success = true;
            prog.is_compiled = true;
            return result;
        }

        // TripleKick — 3 hits with independent accuracy per kick, power ×1/×2/×3.
        // FIXED: do NOT use InitCounter(HitLoop,3,3). The 3 ScalePower+Damage pairs
        // are explicit; accuracy is rechecked per Damage op.
        if (raw_effect == crystal::EffectId::TRIPLE_KICK) {
            // Three explicitly-ordered kicks; no InitCounter.
            // execute_program will roll accuracy separately for each Damage op
            // when hit_loop_remaining == 0 and accuracy_checked_per_hit == true.
            // Pass param8d=1 on ScalePower to signal "per-kick accuracy recheck".
            {
                BOp sc1 = BOp::ScalePower(BScaleChainKind::TripleKick, 1);
                sc1.param8d = 1u;  // 1 = recheck accuracy for this Damage op
                ops.push_back(sc1);
            }
            ops.push_back(BOp::Damage(BDamageSource::Standard));
            {
                BOp sc2 = BOp::ScalePower(BScaleChainKind::TripleKick, 2);
                sc2.param8d = 1u;
                ops.push_back(sc2);
            }
            ops.push_back(BOp::Damage(BDamageSource::Standard));
            {
                BOp sc3 = BOp::ScalePower(BScaleChainKind::TripleKick, 3);
                sc3.param8d = 1u;
                ops.push_back(sc3);
            }
            ops.push_back(BOp::Damage(BDamageSource::Standard));
            result.success = true;
            prog.is_compiled = true;
            return result;
        }

        // Standard multi-hit (2–5 random hits, Crystal distribution).
        // Crystal EndLoop: P(2 hits)=37.5%, P(3 hits)=37.5%, P(4 hits)=12.5%, P(5 hits)=12.5%.
        // encode as InitCounter(HitLoop, 2, 5) — execute_program samples the Crystal distribution.
        ops.push_back(BOp::InitCounter(BCounterKind::HitLoop, 2, 5));
        ops.push_back(BOp::Damage(BDamageSource::Standard));
        result.success = true;
        prog.is_compiled = true;
        return result;
    }

    // ── Charge / Two-Phase ───────────────────────────────────────────────────
    if (desc.is_charge) {
        // Fly/Dig: also set Flying/Underground invulnerability volatile.
        if (raw_effect == crystal::EffectId::FLY || raw_effect == crystal::EffectId::DIG) {
            userVS(VolatileStatus::Charging, true);
            if (raw_effect == crystal::EffectId::FLY) {
                userVS(VolatileStatus::Flying, true);
            } else {
                userVS(VolatileStatus::Underground, true);
            }
        } else if (raw_effect == crystal::EffectId::SKULL_BASH) {
            // Skull Bash: charge turn also applies Defense +1.
            // Encoded via param8c=1 on the SetVolatile(Charging) op.
            {
                BOp skull_charge;
                skull_charge.kind    = BOpKind::SetVolatile;
                skull_charge.param32 = static_cast<uint32_t>(VolatileStatus::Charging);
                skull_charge.param8a = static_cast<uint8_t>(BVolatileTarget::User);
                skull_charge.param8b = 1u;  // set
                skull_charge.param8c = 1u;  // 1 = also apply DefenseUp1 this turn
                ops.push_back(skull_charge);
            }
        } else if (raw_effect == crystal::EffectId::SOLARBEAM) {
            // SolarBeam: skip charge in Sun — encoded as param8d=1 on the Charging op.
            {
                BOp solar_charge;
                solar_charge.kind    = BOpKind::SetVolatile;
                solar_charge.param32 = static_cast<uint32_t>(VolatileStatus::Charging);
                solar_charge.param8a = static_cast<uint8_t>(BVolatileTarget::User);
                solar_charge.param8b = 1u;  // set
                solar_charge.param8d = 1u;  // 1 = skipsuncharge (clear if weather==Sun)
                ops.push_back(solar_charge);
            }
        } else {
            // Standard charge: RazorWind, SkyAttack.
            userVS(VolatileStatus::Charging, true);
        }
        ops.push_back(BOp::Damage(BDamageSource::Standard));
        result.success = true;
        prog.is_compiled = true;
        return result;
    }

    // ── Future Sight ─────────────────────────────────────────────────────────
    if (desc.is_future_sight) {
        ops.push_back(BOp::ScheduleDelayed(3));
        result.success = true;
        prog.is_compiled = true;
        return result;
    }

    // ── Escalating Power (Rollout, FuryCutter, DefenseCurl) ──────────────────
    if (desc.is_escalating_power) {
        if (raw_effect == crystal::EffectId::DEFENSE_CURL) {
            // DefenseCurl: Defense +1 (emitted as StatChange BOp) + set CURLED volatile.
            // FIXED: previously the B program relied on desc.stat_change being applied
            // by the A path — but B path skips A path entirely. The Defense+1 must be
            // an explicit op in the program.
            // Emit: SetVolatile(Curled), then the stat change via SetVolatile param.
            // Use SetVolatile with bits=0 and param8c=2 to signal "apply DefenseUp1".
            {
                BOp curl_defense;
                curl_defense.kind    = BOpKind::SetVolatile;
                curl_defense.param32 = static_cast<uint32_t>(VolatileStatus::Curled);
                curl_defense.param8a = static_cast<uint8_t>(BVolatileTarget::User);
                curl_defense.param8b = 1u;  // set
                curl_defense.param8c = 1u;  // 1 = also apply DefenseUp1
                ops.push_back(curl_defense);
            }
            result.success = true;
            prog.is_compiled = true;
            return result;
        }
        if (raw_effect == crystal::EffectId::FURY_CUTTER) {
            ops.push_back(BOp::ScalePower(BScaleChainKind::FuryCutter));
            ops.push_back(BOp::Damage(BDamageSource::Standard));
            result.success = true;
            prog.is_compiled = true;
            return result;
        }
        // Rollout (default).
        ops.push_back(BOp::ScalePower(BScaleChainKind::Rollout));
        ops.push_back(BOp::Damage(BDamageSource::Standard));
        result.success = true;
        prog.is_compiled = true;
        return result;
    }

    // ── Counter / Mirror Coat ─────────────────────────────────────────────────
    if (desc.is_counter || desc.is_mirror_coat) {
        const uint8_t filter = desc.is_mirror_coat ? 1u : 0u;
        ops.push_back(BOp::StoreDamage(filter));
        ops.push_back(BOp::UseStoredDamage(2));
        result.success = true;
        prog.is_compiled = true;
        return result;
    }

    // ── Pursuit ───────────────────────────────────────────────────────────────
    if (desc.is_pursuit) {
        ops.push_back(BOp::Damage(BDamageSource::Standard));
        result.success = true;
        prog.is_compiled = true;
        return result;
    }

    // ── Rage ──────────────────────────────────────────────────────────────────
    // Crystal Rage: SetVolatile(RAGE), then damage scaled by rage_accumulator.
    // The hook_on_damage_received increments rage_accumulator when hit while RAGE.
    // FIXED: previously incremented Attack stage (+1); Crystal does NOT do that.
    // The damage multiplier is applied in the Damage op via rage_accumulator.
    if (desc.is_rage) {
        userVS(VolatileStatus::Rage, true);
        ops.push_back(BOp::Damage(BDamageSource::Standard));
        result.success = true;
        prog.is_compiled = true;
        return result;
    }

    // ── Copy Mechanics ────────────────────────────────────────────────────────
    if (desc.is_copy_move) {
        if (raw_effect == crystal::EffectId::FORCE_SWITCH) {
            ops.push_back(BOp::ForceSwitch());
            result.success = true;
            prog.is_compiled = true;
            return result;
        }
        if (raw_effect == crystal::EffectId::SUBSTITUTE) {
            userVS(VolatileStatus::Substitute, true);
            if (!ops.empty()) ops.back().param8c = 1u;
            result.success = true;
            prog.is_compiled = true;
            return result;
        }
        if (raw_effect == crystal::EffectId::THIEF) {
            ops.push_back(BOp::Damage(BDamageSource::Standard));
            ops.push_back(BOp::TransferItem());
            result.success = true;
            prog.is_compiled = true;
            return result;
        }
        if (raw_effect == crystal::EffectId::TRANSFORM) {
            ops.push_back(BOp::TransformInto());
            result.success = true;
            prog.is_compiled = true;
            return result;
        }
        if (raw_effect == crystal::EffectId::MIMIC) {
            ops.push_back(BOp::CopyMoveToSlot(BCopyMoveMode::BattleDuration, 0));
            result.success = true;
            prog.is_compiled = true;
            return result;
        }
        if (raw_effect == crystal::EffectId::MIRROR_MOVE) {
            ops.push_back(BOp::InvokeMove(BInvokeMoveSource::LastOpponentMove));
            result.success = true;
            prog.is_compiled = true;
            return result;
        }
        if (raw_effect == crystal::EffectId::METRONOME) {
            ops.push_back(BOp::InvokeMove(BInvokeMoveSource::RandomFromPool));
            result.success = true;
            prog.is_compiled = true;
            return result;
        }
        if (raw_effect == crystal::EffectId::SKETCH) {
            ops.push_back(BOp::CopyMoveToSlot(BCopyMoveMode::Permanent, 1));
            result.success = true;
            prog.is_compiled = true;
            return result;
        }
        result.error = "EffectProgramCompiler: is_copy_move set but raw_effect "
                       + std::to_string(raw_effect) + " not recognized as a copy mechanic";
        return result;
    }

    // ── Leech Seed ────────────────────────────────────────────────────────────
    // Crystal: set SUBSTATUS_LEECH_SEED on target if not Grass type.
    // Per-turn: drain 1/8 max_hp from target, restore to user side. EndOfTurn hook.
    if (desc.is_leech_seed) {
        oppVS(VolatileStatus::Seeded, true);
        result.success = true;
        prog.is_compiled = true;
        return result;
    }

    // ── Disable ───────────────────────────────────────────────────────────────
    // Crystal: disable target's last-used move for (rand & 7) + 1 turns (retry if 0 → 1-7).
    // BCounterKind::Disable on opponent. PreMove hook checks and decrements.
    // Encoded as SetVolatile(CantRun) not needed — Disable uses disable_turns directly.
    // Use InitCounter with BCounterKind::Disable. New counter kind needed.
    // For now encode via SetVolatile param8c=1 to signal DisableEffect.
    // param8a=5 (Disable counter kind encoded in param8a) param8b=1, param8c=7.
    if (desc.is_disable) {
        // Disable target's last move for 1-7 turns.
        // Encoded as InitCounter(Trapping,1,7) on opponent with param8d=2 (Disable marker).
        // execute_program checks param8d==2 and sets target.disable_turns / disabled_move.
        {
            BOp disable_op = BOp::InitCounter(BCounterKind::Trapping, 1, 7);
            disable_op.param8d = 2u;  // 2 = Disable (not Trapping)
            ops.push_back(disable_op);
        }
        result.success = true;
        prog.is_compiled = true;
        return result;
    }

    // ── Encore ────────────────────────────────────────────────────────────────
    // Crystal: force target to repeat last move for (rand & 3) + 3 turns (3-6).
    if (desc.is_encore) {
        {
            BOp encore_op = BOp::InitCounter(BCounterKind::Trapping, 3, 6);
            encore_op.param8d = 3u;  // 3 = Encore (not Trapping)
            ops.push_back(encore_op);
        }
        result.success = true;
        prog.is_compiled = true;
        return result;
    }

    // ── Lock-On ───────────────────────────────────────────────────────────────
    // Crystal: set SUBSTATUS_LOCK_ON on opponent. Next user move always hits.
    // Consumed (cleared) at start of next user accuracy check.
    if (desc.is_lock_on) {
        oppVS(VolatileStatus::LockOn, true);
        result.success = true;
        prog.is_compiled = true;
        return result;
    }

    // ── Sleep Talk ────────────────────────────────────────────────────────────
    // Crystal: if user is asleep, pick a random non-disabled move and invoke it.
    if (desc.is_sleep_talk) {
        ops.push_back(BOp::InvokeMove(BInvokeMoveSource::RandomFromPool));
        // param8d=1 on the InvokeMove op signals "SleepTalk mode":
        // only select from user's own moves, require user to be asleep.
        if (!ops.empty()) ops.back().param8d = 1u;
        result.success = true;
        prog.is_compiled = true;
        return result;
    }

    // ── Destiny Bond ──────────────────────────────────────────────────────────
    // Crystal: set SUBSTATUS_DESTINY_BOND on user.
    // Cleared at start of user's next turn (EndUserDestinyBond).
    // Fires if user faints from opponent's direct-damage move (BattleCommand_CheckFaint path).
    if (desc.is_destiny_bond) {
        userVS(VolatileStatus::DestinyBond, true);
        result.success = true;
        prog.is_compiled = true;
        return result;
    }

    // ── Nightmare ─────────────────────────────────────────────────────────────
    // Crystal: set SUBSTATUS_NIGHTMARE on opponent if asleep.
    // EndOfTurn hook drains 1/4 max_hp while asleep.
    if (desc.is_nightmare) {
        oppVS(VolatileStatus::Nightmare, true);
        result.success = true;
        prog.is_compiled = true;
        return result;
    }

    // ── Curse ─────────────────────────────────────────────────────────────────
    // Crystal: Ghost = lose 50% HP + set SUBSTATUS_CURSE on target. Non-Ghost = Atk+1/Def+1/Spe-1.
    // Both paths are handled in execute_program; the raw_effect is CURSE for both.
    // Use param8d=1 on SetVolatile to signal "check Ghost type at runtime".
    if (desc.is_curse) {
        {
            BOp curse_op;
            curse_op.kind    = BOpKind::SetVolatile;
            curse_op.param32 = static_cast<uint32_t>(VolatileStatus::Cursed);
            curse_op.param8a = static_cast<uint8_t>(BVolatileTarget::Opponent);
            curse_op.param8b = 1u;  // set
            curse_op.param8d = 4u;  // 4 = Curse (runtime checks user type for Ghost)
            ops.push_back(curse_op);
        }
        result.success = true;
        prog.is_compiled = true;
        return result;
    }

    // ── Protect ───────────────────────────────────────────────────────────────
    // Crystal: ProtectChance() = success if user went first AND halving probability.
    // Sets SUBSTATUS_PROTECT on user. Pre-damage hook blocks all damage.
    // protect_consecutive counter halves success probability each use.
    if (desc.is_protect) {
        {
            BOp protect_op;
            protect_op.kind    = BOpKind::SetVolatile;
            protect_op.param32 = static_cast<uint32_t>(VolatileStatus::Protect);
            protect_op.param8a = static_cast<uint8_t>(BVolatileTarget::User);
            protect_op.param8b = 1u;  // set
            protect_op.param8d = 5u;  // 5 = Protect (runtime does probability check)
            ops.push_back(protect_op);
        }
        result.success = true;
        prog.is_compiled = true;
        return result;
    }

    // ── Perish Song ───────────────────────────────────────────────────────────
    // Crystal: set SUBSTATUS_PERISH on both combatants, count=4.
    // EndOfTurn hook decrements both; faint when count reaches 0.
    if (desc.is_perish_song) {
        userVS(VolatileStatus::Perish, true);
        oppVS(VolatileStatus::Perish, true);
        result.success = true;
        prog.is_compiled = true;
        return result;
    }

    // ── Attract ───────────────────────────────────────────────────────────────
    // Crystal: set SUBSTATUS_IN_LOVE on opponent if opposite gender.
    // PreMove hook: 50% chance target skips its move.
    if (desc.is_attract) {
        oppVS(VolatileStatus::Infatuation, true);
        result.success = true;
        prog.is_compiled = true;
        return result;
    }

    // ── Baton Pass ────────────────────────────────────────────────────────────
    // Crystal: switch out while passing stat stages + select volatiles.
    // Implemented via ForceSwitch with param8d=6 (BatonPass marker).
    if (desc.is_baton_pass) {
        {
            BOp bp_op = BOp::ForceSwitch();
            bp_op.param8d = 6u;  // 6 = BatonPass (transfer stats/volatiles to new mon)
            ops.push_back(bp_op);
        }
        result.success = true;
        prog.is_compiled = true;
        return result;
    }

    // ── Heal Bell ─────────────────────────────────────────────────────────────
    // Crystal: cure status of all party members on user's side.
    // Implemented via InvokeMove with param8d=7 (HealBell marker).
    if (desc.is_heal_bell) {
        {
            BOp hb_op = BOp::InvokeMove(BInvokeMoveSource::LastOpponentMove);
            hb_op.param8d = 7u;  // 7 = HealBell (iterate party, clear status)
            ops.push_back(hb_op);
        }
        result.success = true;
        prog.is_compiled = true;
        return result;
    }

    // ── Endure ────────────────────────────────────────────────────────────────
    // Crystal: survive any hit with 1 HP this turn. Pre-damage hook.
    // Same consecutive-use probability halving as Protect.
    if (desc.is_endure) {
        {
            BOp endure_op;
            endure_op.kind    = BOpKind::SetVolatile;
            endure_op.param32 = static_cast<uint32_t>(VolatileStatus::Endure);
            endure_op.param8a = static_cast<uint8_t>(BVolatileTarget::User);
            endure_op.param8b = 1u;  // set
            endure_op.param8d = 8u;  // 8 = Endure (runtime does probability check)
            ops.push_back(endure_op);
        }
        result.success = true;
        prog.is_compiled = true;
        return result;
    }

    // ── Fell through — should not reach here if needs_program() returned true ─
    result.error = "EffectProgramCompiler: no B program matched for raw_effect "
                   + std::to_string(raw_effect)
                   + " (is_multi_hit=" + std::to_string(desc.is_multi_hit)
                   + " is_charge=" + std::to_string(desc.is_charge) + ")";
    return result;
}

} // namespace crystal
