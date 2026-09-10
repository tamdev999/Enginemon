// engine/battle/battle_program.cpp
//
// Architecture B: execute_program() and engine hooks.
//
// Implements Battle::execute_program() — the runtime executor for
// SemanticEffectProgram — and all 9 engine event hooks.
//
// Called from battle.cpp execute_move() when MoveData::has_program == true.
// Also called from execute_turn() at the appropriate execution phases.
//
// No Crystal opcodes, effect IDs, or ROM addresses appear in this file.

#include "engine/battle/battle.hpp"
#include "engine/battle/calculator.hpp"
#include "engine/party/party.hpp"
#include "engine/party/pokemon.hpp"
#include <algorithm>
#include <cassert>
#include <cstdint>

namespace enginemon {

// ============================================================================
// Internal helpers (mirroring battle.cpp helpers used by A path)
// ============================================================================

// Forward-declare the free functions used in the A path that we also need here.
// They are defined in calculator.cpp.
int32_t apply_stat_stage(int32_t base_stat, int8_t stage);
int32_t apply_stat_stage(int32_t base_stat, int8_t stage, const BattleRules& rules);

// ============================================================================
// Hidden Power: DV-based type and power computation.
// Source: suiCune engine/battle/hidden_power.c HiddenPowerDamage.
// Source: pokecrystal engine/battle/hidden_power.asm HiddenPowerDamage.
//
// TYPE:
//   type_raw = (def_dv & 3) | ((atk_dv & 3) << 2)  [4-bit, 0-15]
//   type = type_raw + 1       (skip Normal=0)
//   if type >= BIRD(6): type += 1      (skip Bird)
//   if type >= UNUSED_TYPES_START(10): type += 10   (skip unused 10-19)
//   Result sequence: 1,2,3,4,5,7,8,9,20,21,22,23,24,25,26,27
//
// POWER:
//   p_bits = (atk&8) | ((def&8)>>1) | ((spd&8)>>2) | ((spc&8)>>3)  [bit 3 of each DV]
//   power = ((p_bits * 5 + (spc_dv & 3)) >> 1) + 31   [range 31-70]
//
// Returns {type_id, power}.
// ============================================================================
static std::pair<uint8_t, uint8_t> calc_hidden_power(uint8_t dv_atk, uint8_t dv_def,
                                                       uint8_t dv_spd, uint8_t dv_spc) {
    // Type: low 2 bits of Attack and Defense only.
    uint8_t hp_type = static_cast<uint8_t>((dv_def & 3u) | ((dv_atk & 3u) << 2u));
    hp_type += 1u;               // skip Normal (0)
    if (hp_type >= 6u)  hp_type += 1u;  // skip Bird (6)
    if (hp_type >= 10u) hp_type += 10u; // skip unused (10-19)

    // Power: bit 3 of each DV.
    const uint8_t p_bits = static_cast<uint8_t>(
          (dv_atk & 8u)
        | ((dv_def & 8u) >> 1u)
        | ((dv_spd & 8u) >> 2u)
        | ((dv_spc & 8u) >> 3u));
    const uint8_t spc_low2 = static_cast<uint8_t>(dv_spc & 3u);
    const uint8_t power = static_cast<uint8_t>(
        ((static_cast<uint32_t>(p_bits) * 5u + spc_low2) >> 1u) + 31u);

    return {hp_type, power};
}

// ============================================================================
// Battle::execute_program
// ============================================================================
MoveExecutionResult Battle::execute_program(BattlePokemon& user, BattlePokemon& target,
                                            const MoveData& md, size_t move_slot,
                                            bool user_is_player) {
    const SemanticEffectProgram& prog = md.effect_program;

    // NOTE: damage_received_this_turn is intentionally NOT reset here.
    // execute_turn() resets it at the start of each turn; StoreDamage (Counter/MirrorCoat)
    // reads the value accumulated by hook_on_damage_received when the opponent hit us earlier
    // this same turn. Resetting it here would destroy the Counter mechanic.

    // -- Recharge gate (Hyper Beam) — same as A path ---------------------------
    if (user.recharge_turns > 0) {
        --user.recharge_turns;
        message(md.name + " — must recharge!");
        return MoveExecutionResult::UnsupportedSemantic;
    }

    // ── PP deduction (same as A path) ────────────────────────────────────────
    if (move_slot < 4 && user.moves[move_slot].pp > 0)
        user.moves[move_slot].pp--;

    // ── Rampage gate (continuation turns) ────────────────────────────────────
    // Crystal: BattleCommand_CheckRampage runs at the START of moves on turns 2+.
    // It decrements the counter and, when 0, clears SUBSTATUS_RAMPAGE + applies confusion.
    // On ALL continuation turns (counter > 0 or == 0 after decrement): damage fires.
    // Source: suiCune CheckRampage / .continue_rampage always jumps to rampage_command.
    //
    // Turn 1: Rampage NOT yet set → gate skipped → program ops set volatile + counter + damage.
    // Turns 2+: Rampage IS set → gate fires: decrement, possibly clear+confuse, then damage.
    if (user.has_volatile(VolatileStatus::Rampage)) {
        if (user.turn_counter > 0) {
            --user.turn_counter;
            if (user.turn_counter == 0) {
                // Last attack turn: clear Rampage, apply confusion (unless Safeguard).
                user.clear_volatile(VolatileStatus::Rampage);
                const bool safeguarded = user_is_player
                    ? (field_.safeguard_player > 0)
                    : (field_.safeguard_opponent > 0);
                if (!safeguarded && !user.has_volatile(VolatileStatus::Confusion)) {
                    user.set_volatile(VolatileStatus::Confusion);
                    // P1-Confusion: BattleRandom & 1 + 2 = 2 or 3 turns.
                    // Source: BattleCommand_CheckRampage asm: and %00000001; inc a; inc a.
                    user.confusion_turns = static_cast<uint8_t>((rng_.next_byte() & 0x01u) + 2u);
                    message((user_is_player ? std::string("Player") : std::string("Opponent"))
                            + " became confused from the rampage!");
                }
            }
        }
        // Regardless of counter value: deal damage this continuation turn.
        // Skip to the Damage op only (don't re-run SetVolatile + InitCounter).
        // Inline standard damage calculation here, reusing execute_program's Damage path.
        // We do this by falling through to the ops loop, but the Rampage program's ops
        // include a SetVolatile + InitCounter before Damage. To avoid resetting the counter,
        // we skip those ops and execute only the Damage.
        // IMPLEMENTATION: set a flag so the ops loop skips non-Damage ops.
        // Simplest: execute the damage via a direct inline standard damage.
        // Type effectiveness.
        const uint16_t ramp_type_raw = get_combined_effectiveness(
            md.type, target.type1, target.type2, registries_.type_chart);
        const uint16_t ramp_type_eff = (ramp_type_raw == 0
                                         && target.has_volatile(VolatileStatus::Identified)
                                         && (static_cast<uint8_t>(md.type) == 0u
                                             || static_cast<uint8_t>(md.type) == 1u))
                                        ? uint16_t{100} : ramp_type_raw;
        if (ramp_type_eff == 0) {
            message("It doesn't affect the opposing Pokémon…");
            return MoveExecutionResult::Immune;
        }
        // Accuracy: rampage never misses on continuation (no checkhit in continuation).
        // Crit + damage.
        uint8_t ramp_crit_stage = 0;
        if (rules_) ramp_crit_stage = build_crit_stage(user, md, *rules_);
        const bool ramp_crit = rules_
            ? roll_critical(ramp_crit_stage, rng_.next_byte(), *rules_)
            : roll_critical(ramp_crit_stage, rng_.next_byte());
        const int8_t eff_atk_r = (ramp_crit && user.stages.attack < 0) ? 0 : user.stages.attack;
        const int8_t eff_def_r = (ramp_crit && target.stages.defense > 0) ? 0 : target.stages.defense;
        const bool physical = (md.category == MoveCategory::Physical);
        auto rss = [this](int32_t b, int8_t s) {
            return rules_ ? apply_stat_stage(b,s,*rules_) : apply_stat_stage(b,s);
        };
        int32_t ramp_atk = physical ? rss(user.base_stats.attack, eff_atk_r)
                                     : rss(user.base_stats.special_attack, user.stages.special_attack);
        int32_t ramp_def = physical ? rss(target.base_stats.defense, eff_def_r)
                                     : rss(target.base_stats.special_defense, target.stages.special_defense);
        if (physical && user_is_player  && field_.reflect_opponent > 0) ramp_def *= 2;
        if (physical && !user_is_player && field_.reflect_player   > 0) ramp_def *= 2;
        if (!physical && user_is_player  && field_.light_screen_opponent > 0) ramp_def *= 2;
        if (!physical && !user_is_player && field_.light_screen_player   > 0) ramp_def *= 2;
        if (ramp_atk < 1) ramp_atk = 1;
        if (ramp_def < 1) ramp_def = 1;
        DamageParams rdp{};
        rdp.attacker_level = user.level; rdp.attack_stat = ramp_atk;
        rdp.defense_stat = ramp_def; rdp.move_power = md.power;
        rdp.type_effectiveness = 100; rdp.stab = (md.type == user.type1 || md.type == user.type2);
        rdp.critical = ramp_crit; rdp.burned = (physical && user.status == Status::Burn);
        rdp.weather = field_.weather; rdp.move_type = md.type;
        int32_t ramp_dmg = rules_ ? enginemon::calculate_damage(rdp, *rules_)
                                  : enginemon::calculate_damage(rdp);
        if (ramp_dmg == 0) return MoveExecutionResult::Immune;
        if (rules_ && field_.weather != Weather::None)
            ramp_dmg = apply_weather_modifier(ramp_dmg,
                static_cast<uint8_t>(field_.weather),
                static_cast<uint8_t>(md.type), md.effect_id, *rules_);
        if (rdp.stab) { ramp_dmg += ramp_dmg/2; ramp_dmg = std::clamp(ramp_dmg, 2, 999); }
        if (ramp_type_eff != 100) {
            ramp_dmg = ramp_dmg * static_cast<int32_t>(ramp_type_eff) / 100;
            ramp_dmg = std::clamp(ramp_dmg, 1, 999);
        }
        // Variation.
        const uint8_t vt2 = rules_ ? rules_->get_damage_var_lower_bound() : uint8_t{0xD9};
        const int32_t vd2 = rules_ ? static_cast<int32_t>(rules_->get_damage_var_divisor()) : 255;
        uint8_t rvar; do { uint8_t rr=rng_.next_byte(); rvar=(rr>>1)|(rr<<7); } while(rvar<vt2);
        ramp_dmg = ramp_dmg * static_cast<int32_t>(rvar) / (vd2>0?vd2:255);
        if (ramp_dmg < 2) ramp_dmg = 2;
        if (ramp_crit) message("A critical hit!");
        if (ramp_type_eff > 100) message("It's super effective!");
        else if (ramp_type_eff < 100) message("It's not very effective…");
        // Apply damage.
        const int16_t old_ramp_hp = target.stats.hp;
        target.stats.hp = static_cast<int16_t>(std::max(0,
            static_cast<int32_t>(target.stats.hp) - ramp_dmg));
        if (target.has_volatile(VolatileStatus::Endure) && target.stats.hp <= 0)
            target.stats.hp = 1;
        hp_change(user_is_player ? 1u : 0u, old_ramp_hp, target.stats.hp);
        outcome_.damage_dealt += static_cast<uint16_t>(ramp_dmg);
        hook_on_damage_received(target, ramp_dmg, static_cast<uint8_t>(md.category));
        if (target.stats.hp <= 0 && target.has_volatile(VolatileStatus::DestinyBond))
            hook_destiny_bond_check(target, user, !user_is_player);
        return MoveExecutionResult::Success;
    }


    // When counter reaches 0: release stored_damage × 2.
    // Source: suiCune bide.c BattleCommand_StoreEnergy / BattleCommand_UnleashEnergy.
    // Turn 1: Bide volatile NOT yet set → gate skipped → program ops set volatile+counter.
    // Turns 2+: Bide IS set → gate fires.
    if (user.has_volatile(VolatileStatus::Bide)) {
        if (user.turn_counter > 0) {
            --user.turn_counter;
            message(md.name + " — storing energy!");
            return MoveExecutionResult::Success;
        }
        // turn_counter reached 0: release.
        // Crystal UnleashEnergy → checkhit → applydamage path.
        // Must interact with Protect, Substitute, Endure, and damage history
        // exactly as normal damage does.
        // Source: suiCune bide.c BattleCommand_UnleashEnergy calls checkhit.
        const int32_t bide_dmg = std::min(65535,
            static_cast<int32_t>(user.bide_stored) * 2);
        user.clear_volatile(VolatileStatus::Bide);
        user.bide_stored  = 0;
        user.turn_counter = 0;
        if (bide_dmg == 0) {
            message(md.name + " — no energy stored!");
            return MoveExecutionResult::Miss;
        }
        message(md.name + " — unleashing stored energy!");
        // Protect: checkhit blocks Bide release when target is protected.
        if (target.has_volatile(VolatileStatus::Protect)) {
            message((user_is_player ? std::string("Opponent") : std::string("Player"))
                    + " protected itself!");
            return MoveExecutionResult::Miss;
        }
        // Substitute: Bide release damage is routed into Substitute, not real HP.
        if (target.has_volatile(VolatileStatus::Substitute) && target.substitute_hp > 0) {
            if (bide_dmg >= static_cast<int32_t>(target.substitute_hp)) {
                target.substitute_hp = 0;
                target.clear_volatile(VolatileStatus::Substitute);
                message((user_is_player ? std::string("Opponent") : std::string("Player"))
                        + "'s substitute broke!");
            } else {
                target.substitute_hp = static_cast<uint16_t>(
                    target.substitute_hp - static_cast<uint16_t>(bide_dmg));
            }
            outcome_.damage_dealt += static_cast<uint16_t>(bide_dmg);
            // No hook_on_damage_received: Crystal skips accumulation for Substitute hits.
            return MoveExecutionResult::Success;
        }
        // Apply to real HP.
        const int16_t old_hp = target.stats.hp;
        target.stats.hp = static_cast<int16_t>(
            std::max(0, static_cast<int32_t>(target.stats.hp) - bide_dmg));
        // Endure: Bide release floors target HP at 1 when Endure is active.
        if (target.has_volatile(VolatileStatus::Endure) && target.stats.hp <= 0) {
            target.stats.hp = 1;
            message((user_is_player ? std::string("Opponent") : std::string("Player"))
                    + " endured the hit!");
        }
        hp_change(user_is_player ? 1u : 0u, old_hp, target.stats.hp);
        outcome_.damage_dealt += static_cast<uint16_t>(bide_dmg);
        // Damage history: hook must fire exactly as for normal damage.
        hook_on_damage_received(target, bide_dmg, static_cast<uint8_t>(md.category));
        if (target.stats.hp <= 0 && target.has_volatile(VolatileStatus::DestinyBond)) {
            hook_destiny_bond_check(target, user, !user_is_player);
        }
        return MoveExecutionResult::Success;
    }

    // ── Execute ordered ops ───────────────────────────────────────────────────
    bool phase1_done = false;   // for two-phase charge moves
    bool accuracy_checked = false;
    bool move_hit = true;
    // ScalePower multiplier communicated to the next Damage op.
    // 0 = no scaling pending; non-zero = multiply eff_power by this.
    uint8_t pending_scale_mult = 0;
    // King's Rock: rolled exactly once per execute_program call, AFTER all ops complete.
    // Placed outside the ops loop to guarantee one roll even for TripleKick (3 Damage ops).
    // Uses move_hit which is set by the first accuracy check and persists across all ops.


    for (const BOp& op : prog.ops) {
        switch (op.kind) {

        // ────────────────────────────────────────────────────────────────────
        case BOpKind::Damage: {
            const auto src = static_cast<BDamageSource>(op.param8a);

            if (src == BDamageSource::StoredEnergy) {
                // Bide release: stored_damage × 2, bypasses defense/type.
                // Crystal UnleashEnergy → checkhit → normal applydamage routing.
                // Must interact with Protect, Substitute, Endure, and damage history.
                const int32_t bide_dmg = std::min(65535,
                    static_cast<int32_t>(user.bide_stored) * 2);
                user.clear_volatile(VolatileStatus::Bide);
                user.bide_stored   = 0;
                user.turn_counter  = 0;
                if (bide_dmg == 0) {
                    message(md.name + " — no energy stored!");
                    return MoveExecutionResult::Miss;
                }
                message(md.name + " — unleashing stored energy!");
                // Protect: checkhit blocks release when target is protected.
                if (target.has_volatile(VolatileStatus::Protect)) {
                    message((user_is_player ? std::string("Opponent") : std::string("Player"))
                            + " protected itself!");
                    return MoveExecutionResult::Miss;
                }
                // Substitute: route damage into Substitute, not real HP.
                if (target.has_volatile(VolatileStatus::Substitute) && target.substitute_hp > 0) {
                    if (bide_dmg >= static_cast<int32_t>(target.substitute_hp)) {
                        target.substitute_hp = 0;
                        target.clear_volatile(VolatileStatus::Substitute);
                        message((user_is_player ? std::string("Opponent") : std::string("Player"))
                                + "'s substitute broke!");
                    } else {
                        target.substitute_hp = static_cast<uint16_t>(
                            target.substitute_hp - static_cast<uint16_t>(bide_dmg));
                    }
                    outcome_.damage_dealt += static_cast<uint16_t>(bide_dmg);
                    // No hook_on_damage_received: Crystal skips accumulation for Substitute hits.
                    break;
                }
                // Apply to real HP.
                const int16_t old_hp = target.stats.hp;
                target.stats.hp = static_cast<int16_t>(
                    std::max(0, static_cast<int32_t>(target.stats.hp) - bide_dmg));
                // Endure: floor target HP at 1.
                if (target.has_volatile(VolatileStatus::Endure) && target.stats.hp <= 0) {
                    target.stats.hp = 1;
                    message((user_is_player ? std::string("Opponent") : std::string("Player"))
                            + " endured the hit!");
                }
                hp_change(user_is_player ? 1u : 0u, old_hp, target.stats.hp);
                outcome_.damage_dealt += static_cast<uint16_t>(bide_dmg);
                // Damage history: must fire exactly as for normal damage.
                hook_on_damage_received(target, bide_dmg, static_cast<uint8_t>(md.category));
                // Destiny Bond check on Bide release
                if (target.stats.hp <= 0 && target.has_volatile(VolatileStatus::DestinyBond)) {
                    hook_destiny_bond_check(target, user, !user_is_player);
                }
                break;
            }

            // ── Fire-turn charge: clear volatiles if Charging already set ────
            // The SetVolatile(Charging) case handles the charge-turn defer (returns Success).
            // If we reach Damage, it means either:
            //   - No charge volatile (non-charge move), OR
            //   - Charging IS set (fire turn): clear it and proceed to damage.
            if (user.has_volatile(VolatileStatus::Charging)) {
                // Fire turn: clear charge + invulnerability volatiles.
                user.clear_volatile(VolatileStatus::Charging);
                user.clear_volatile(VolatileStatus::Flying);
                user.clear_volatile(VolatileStatus::Underground);
            }

            // ── Accuracy check ────────────────────────────────────────────────
            // For ordinary MultiHit and TripleKick: accuracy is checked per-hit.
            // param8d==1 on the preceding ScalePower op signals "per-kick accuracy recheck".
            // We use accuracy_checked flag: cleared before each TripleKick Damage op via
            // the ScalePower step (ScalePower sets accuracy_checked = false when param8d==1).
            // For standard cases: accuracy_checked fires once per execute_program call.
            if (!accuracy_checked) {
                accuracy_checked = true;
                // -- Lock-On / Mind Reader accuracy bypass (B-path) ---------------
                // Source: suiCune lock_on.c — always-hit next move when LockOn active.
                // LockOn is set on the target. Consume it here on the first accuracy check.
                if (target.has_volatile(VolatileStatus::LockOn)) {
                    target.clear_volatile(VolatileStatus::LockOn);
                    // Accuracy roll skipped — guaranteed hit.
                } else if (md.accuracy != 0) {
                    uint8_t base_accuracy = md.accuracy;
                    // Thunder accuracy: Rain=always hit, Sun=50%.
                    if (md.effect_desc.has_thunder_accuracy) {
                        if (field_.weather == Weather::Rain) {
                            base_accuracy = 0xFF;  // always hit
                        } else if (field_.weather == Weather::Sun) {
                            base_accuracy = 50;
                        }
                    }
                    // P1-21: Always apply accuracy/evasion stage modifiers before the
                    // always-hit (0xFF) check. Crystal's BattleCommand_CheckHit runs
                    // .StatModifiers before "cp -1; jr z .Hit", so sufficiently bad
                    // stages can cause 0xFF-accuracy moves to miss.
                    // If base_accuracy is 0xFF and stages are both 0, eff_acc will
                    // be clamped to 255 = 0xFF → guaranteed hit (no RNG consumed).
                    // If stages reduce eff_acc below 255, a miss becomes possible.
                    uint32_t eff_acc;
                    if (base_accuracy == 0xFF) {
                        // Apply stages to 255; if still 255 → always-hit, skip roll.
                        if (rules_) {
                            const auto e1 = rules_->get_acc_mult(user.stages.accuracy);
                            uint32_t acc = static_cast<uint32_t>(base_accuracy) * e1.numerator / e1.denominator;
                            const auto e2 = rules_->get_acc_mult(static_cast<int8_t>(-target.stages.evasion));
                            acc = acc * e2.numerator / e2.denominator;
                            eff_acc = std::min(acc, uint32_t{255});
                        } else {
                            eff_acc = 255u;
                        }
                        if (eff_acc >= 255u) {
                            // Still guaranteed-hit: no RNG consumed.
                            // (move_hit stays true)
                        } else {
                            // Stages reduced effective accuracy — roll needed.
                            move_hit = (rng_.next_byte() < eff_acc);
                            if (!move_hit) {
                                if (user.hit_loop_remaining > 0) {
                                    // multi-hit: loop continues with 0 damage
                                } else {
                                    message(md.name + " missed!");
                                    hook_chain_reset(user);
                                    return MoveExecutionResult::Miss;
                                }
                            }
                        }
                    } else {
                        uint8_t effective_accuracy = base_accuracy;
                        if (effective_accuracy != 0xFF) {
                            move_hit = rules_
                                ? roll_accuracy(effective_accuracy, user.stages.accuracy,
                                                target.stages.evasion, rng_.next_byte(), *rules_)
                                : roll_accuracy(effective_accuracy, user.stages.accuracy,
                                                target.stages.evasion, rng_.next_byte());
                            if (!move_hit) {
                                if (user.hit_loop_remaining > 0) {
                                    // MultiHit/TripleKick: miss = 0 damage, loop continues.
                                    // (fall through with move_hit=false; damage=0 handled below)
                                } else {
                                    message(md.name + " missed!");
                                    hook_chain_reset(user);
                                    return MoveExecutionResult::Miss;
                                }
                            }
                        }
                    }
                }
            }

            if (!move_hit && user.hit_loop_remaining == 0) break;

            // Type effectiveness.
            const uint16_t type_eff_raw = get_combined_effectiveness(
                md.type, target.type1, target.type2, registries_.type_chart);
            // ── Foresight / Identified: Normal and Fighting hit Identified Ghost ──
            // Source: suiCune foresight.c — same bypass as A-path.
            const uint16_t type_eff = (type_eff_raw == 0
                                        && target.has_volatile(VolatileStatus::Identified)
                                        && (static_cast<uint8_t>(md.type) == 0u
                                            || static_cast<uint8_t>(md.type) == 1u))
                                       ? uint16_t{100}
                                       : type_eff_raw;
            if (type_eff == 0 && user.hit_loop_remaining == 0) {
                message("It doesn't affect the opposing Pokémon…");
                return MoveExecutionResult::Immune;
            }

            // ── Hit-loop ──────────────────────────────────────────────────────
            const uint8_t hit_count = (user.hit_loop_remaining > 0)
                ? user.hit_loop_remaining : uint8_t{1};
            uint8_t hits_landed = 0;
            // B-path effectchance: Crystal fires effectchance ONCE, before critical, on pass 1.
            // endloop rewinds to critical (not checkhit/effectchance), so pass 2+ never re-roll.
            // Source: PoisonMultiHit script order: checkhit → effectchance → critical → ... → endloop
            // loop_back_to_critical scans backward for the critical byte; effectchance is before it.
            // Roll once here, before the hit loop, and use this single result for the post-loop secondary.
            bool last_secondary_fired = false;
            if (md.effect_desc.has_effectchance_phase) {
                last_secondary_fired = (rng_.next_byte() < static_cast<uint32_t>(md.effect_chance));
            }

            for (uint8_t hit_i = 0; hit_i < hit_count; ++hit_i) {
                if (target.is_fainted()) break;

                // Crystal: checkhit is ABOVE critical in the script; endloop rewinds to
                // critical, NOT to checkhit. Therefore accuracy is rolled exactly once
                // (by the outer accuracy_checked block above) and never re-rolled per
                // hit iteration. The outer check already handled the miss case.
                // This_hit_lands is always true here; all hits proceed if the move hit.

                int32_t damage = 0;

                if (src == BDamageSource::BeatUpMember) {
                    bool found_member = false;
                    const bool attacker_is_player = user_is_player;
                    const size_t party_size = attacker_is_player
                        ? player_party_.size()
                        : opponent_party_.size();
                    while (user.beat_up_index < party_size) {
                        const size_t beat_idx = user.beat_up_index;
                        ++user.beat_up_index;
                        uint8_t member_level = 50;
                        SpeciesId member_species = SPECIES_NONE;
                        bool member_ok = false;
                        if (attacker_is_player) {
                            const Pokemon* pmon = player_party_.get(beat_idx);
                            if (pmon && pmon->current_hp > 0 && pmon->status == Status::None) {
                                member_level   = pmon->level;
                                member_species = pmon->species;
                                member_ok      = true;
                            }
                        } else {
                            if (beat_idx < opponent_party_.size()) {
                                const BattlePokemon& obp = opponent_party_[beat_idx];
                                if (!obp.is_fainted() && obp.status == Status::None) {
                                    member_level   = obp.level;
                                    member_species = obp.species;
                                    member_ok      = true;
                                }
                            }
                        }
                        if (!member_ok) continue;
                        const SpeciesData* spd_data = registries_.species.get(member_species);
                        const uint8_t b_atk = spd_data ? spd_data->base_stats.attack : 50;
                        const SpeciesData* osd = registries_.species.get(target.species);
                        const uint8_t b_def = osd ? osd->base_stats.defense : 50;
                        const int32_t atk_f = static_cast<int32_t>(b_atk) / 10 + 5;
                        const int32_t lvl_f = static_cast<int32_t>(member_level) / 2 + 5;
                        const int32_t def_f = std::max(1, static_cast<int32_t>(b_def) / 10 + 5);
                        damage = std::max(1, (atk_f * lvl_f) / def_f);
                        found_member = true;
                        break;
                    }
                    if (!found_member) {
                        user.beat_up_index = 0;
                        break;
                    }
                } else {
                    // Standard damage path.
                    const bool physical = (md.category == MoveCategory::Physical);
                    const bool stab     = (md.type == user.type1 || md.type == user.type2);

                    uint8_t crit_stage = 0;
                    if (rules_) crit_stage = build_crit_stage(user, md, *rules_);
                    const bool is_crit = rules_
                        ? roll_critical(crit_stage, rng_.next_byte(), *rules_)
                        : roll_critical(crit_stage, rng_.next_byte());

                    const int8_t eff_atk_s  = (is_crit && user.stages.attack < 0) ? 0 : user.stages.attack;
                    const int8_t eff_def_s  = (is_crit && target.stages.defense > 0) ? 0 : target.stages.defense;
                    const int8_t eff_satk_s = (is_crit && user.stages.special_attack < 0) ? 0 : user.stages.special_attack;
                    const int8_t eff_sdef_s = (is_crit && target.stages.special_defense > 0) ? 0 : target.stages.special_defense;
                    auto ss = [this](int32_t b, int8_t s) {
                        return rules_ ? apply_stat_stage(b, s, *rules_) : apply_stat_stage(b, s);
                    };

                    int32_t atk_stat, def_stat;
                    if (physical) {
                        atk_stat = ss(user.base_stats.attack, eff_atk_s);
                        def_stat = ss(target.base_stats.defense, eff_def_s);
                        if (user_is_player && field_.reflect_opponent > 0) def_stat *= 2;
                        if (!user_is_player && field_.reflect_player > 0)  def_stat *= 2;
                    } else {
                        atk_stat = ss(user.base_stats.special_attack, eff_satk_s);
                        def_stat = ss(target.base_stats.special_defense, eff_sdef_s);
                        if (user_is_player && field_.light_screen_opponent > 0) def_stat *= 2;
                        if (!user_is_player && field_.light_screen_player > 0)   def_stat *= 2;
                    }
                    const bool burned = physical && (user.status == Status::Burn);

                    // Metal Powder (SpeciesDefenseBoost): x1.5 defense for Ditto.
                    // Source: DittoMetalPowder -- Crystal applies the x1.5 modifier on both
                    // physical and special defensive stat paths.
                    if (target.held_item != ITEM_NONE) {
                        const ItemData* mp_item = registries_.items.get(target.held_item);
                        if (mp_item && mp_item->held_effect_type == HeldItemEffectType::SpeciesDefenseBoost
                                && mp_item->species_restriction != SPECIES_NONE
                                && target.species == mp_item->species_restriction) {
                            def_stat = def_stat + def_stat / 2;
                            if (def_stat < 1) def_stat = 1;
                        }
                    }


                    uint8_t eff_power = md.power;
                    if (pending_scale_mult > 0) {
                        eff_power = static_cast<uint8_t>(
                            std::min(255, static_cast<int32_t>(md.power) * pending_scale_mult));
                        pending_scale_mult = 0;
                    } else if (user.fury_cutter_count > 0) {
                        const uint8_t shifts = std::min(uint8_t{5}, user.fury_cutter_count) - 1u;
                        eff_power = static_cast<uint8_t>(
                            std::min(255, static_cast<int32_t>(md.power) << shifts));
                    } else if (user.rollout_count > 0) {
                        const uint8_t shifts = std::min(uint8_t{5}, user.rollout_count) - 1u;
                        int32_t rp = static_cast<int32_t>(md.power) << shifts;
                        if (user.has_volatile(VolatileStatus::Curled)) rp *= 2;
                        eff_power = static_cast<uint8_t>(std::min(255, rp));
                    }

                    // ── Rage damage scaling ────────────────────────────────────
                    // Crystal RageDamage: outgoing damage = base * (1 + rage_accumulator).
                    // FIXED: no Attack stage change; only outgoing damage is multiplied.
                    if (user.has_volatile(VolatileStatus::Rage) && user.rage_accumulator > 0) {
                        eff_power = static_cast<uint8_t>(
                            std::min(255, static_cast<int32_t>(eff_power) * (1 + user.rage_accumulator)));
                    }

                    // ── Pursuit 2× when target is switching ───────────────────
                    if (md.effect_desc.is_pursuit && target.is_switching) {
                        eff_power = static_cast<uint8_t>(std::min(255, static_cast<int32_t>(eff_power) * 2));
                    }

                    if (md.effect_desc.user_faints && rules_) {
                        const uint8_t ds = rules_->get_selfdestruct_def_shift();
                        if (ds > 0) def_stat = std::max(1, def_stat >> ds);
                    }

                    DamageParams dp{};
                    dp.attacker_level     = user.level;
                    dp.attack_stat        = atk_stat;
                    dp.defense_stat       = def_stat;
                    dp.move_power         = eff_power;
                    dp.type_effectiveness = 100;
                    dp.stab               = false;
                    dp.critical           = is_crit;
                    dp.burned             = burned;
                    dp.weather            = field_.weather;
                    dp.move_type          = md.type;

                    damage = rules_ ? enginemon::calculate_damage(dp, *rules_)
                                    : enginemon::calculate_damage(dp);

                    if (rules_ && field_.weather != Weather::None)
                        damage = apply_weather_modifier(damage,
                            static_cast<uint8_t>(field_.weather),
                            static_cast<uint8_t>(md.type),
                            md.effect_id, *rules_);

                    // SolarBeam rain penalty: halves_in_rain is set by the Crystal
                    // frontend for EFFECT_SOLARBEAM. The WeatherMoveModifiers table
                    // lookup via effect_id is dead (raw Crystal ID vs semantic ID);
                    // this explicit check replaces it with no raw-ID dispatch.
                    // Source: suiCune DoWeatherModifiers WeatherMoveModifiers entry
                    //   {WEATHER_RAIN, EFFECT_SOLARBEAM, multiplier=5 (×0.5)}.
                    if (md.effect_desc.halves_in_rain && field_.weather == Weather::Rain) {
                        damage = std::max(1, damage / 2);
                    }

                    if (stab) {
                        damage += damage / 2;
                        damage = std::clamp(damage, 2, 999);
                    }
                    if (type_eff != 100) {
                        damage = damage * static_cast<int32_t>(type_eff) / 100;
                        damage = std::clamp(damage, 1, 999);
                    }

                    const uint8_t vt = rules_ ? rules_->get_damage_var_lower_bound() : uint8_t{0xD9};
                    const int32_t vd = rules_ ? static_cast<int32_t>(rules_->get_damage_var_divisor()) : 255;
                    uint8_t variation;
                    do { uint8_t r = rng_.next_byte(); variation = (r >> 1) | (r << 7); }
                    while (variation < vt);
                    damage = damage * static_cast<int32_t>(variation) / (vd > 0 ? vd : 255);
                    if (damage < 2) damage = 2;

                    if (is_crit) message("A critical hit!");
                    if (type_eff > 100) message("It's super effective!");
                    else if (type_eff < 100) message("It's not very effective…");
                }

                damage = std::max(1, damage);

                // -- P0-1: Protect interception (B-path) -------------------------
                // Crystal: BattleCommand_CheckHit checks SUBSTATUS_PROTECT on target.
                // If Protect is active, the move misses entirely for this hit.
                if (target.has_volatile(VolatileStatus::Protect)) {
                    message(md.name + " -- blocked by Protect!");
                    move_hit = false;
                    if (user.hit_loop_remaining > 0) { continue; }
                    break;
                }

                // ── Substitute blocking ────────────────────────────────────────
                // Crystal: if target has Substitute, damage goes to substitute_hp instead of HP.
                // When substitute_hp depletes to ≤ 0: clear Substitute volatile, no further damage.
                if (target.has_volatile(VolatileStatus::Substitute) && target.substitute_hp > 0) {
                    if (damage >= static_cast<int32_t>(target.substitute_hp)) {
                        // Substitute breaks.
                        target.substitute_hp = 0;
                        target.clear_volatile(VolatileStatus::Substitute);
                        message((user_is_player ? std::string("Opponent") : std::string("Player"))
                                + "'s substitute broke!");
                    } else {
                        target.substitute_hp = static_cast<uint16_t>(
                            target.substitute_hp - static_cast<uint16_t>(damage));
                    }
                    // Damage routed to substitute — no HP change on target mon.
                    outcome_.damage_dealt += static_cast<uint16_t>(damage);
                    ++hits_landed;
                    continue;
                }

                animate(md.animation_id, user_is_player ? 0u : 1u, user_is_player ? 1u : 0u);

                const int16_t old_hp = target.stats.hp;
                target.stats.hp = static_cast<int16_t>(std::max(0,
                    static_cast<int32_t>(target.stats.hp) - damage));
                hp_change(user_is_player ? 1u : 0u, old_hp, target.stats.hp);
                outcome_.damage_dealt += static_cast<uint16_t>(damage);

                // -- P0-2: Endure 1-HP floor (B-path) ----------------------------
                if (target.has_volatile(VolatileStatus::Endure) && target.stats.hp <= 0) {
                    target.stats.hp = 1;
                    message(md.name + " — " + (user_is_player ? "Opponent" : "Player") + " endured the hit!");
                }

                hook_on_damage_received(target, damage, static_cast<uint8_t>(md.category));

                // Destiny Bond: if target just fainted from this direct-damage hit.
                if (target.stats.hp <= 0 && target.has_volatile(VolatileStatus::DestinyBond)) {
                    hook_destiny_bond_check(target, user, !user_is_player);
                }

                if (md.effect_desc.has_recoil && rules_) {
                    const int32_t recoil = std::max(1, damage >> rules_->get_recoil_shift());
                    const int16_t old_user_hp = user.stats.hp;
                    user.stats.hp = static_cast<int16_t>(std::max(0,
                        static_cast<int32_t>(user.stats.hp) - recoil));
                    hp_change(user_is_player ? 0u : 1u, old_user_hp, user.stats.hp);
                }

                if (md.effect_desc.user_faints) {
                    user.stats.hp = 0;
                    const int16_t old_u = user.stats.hp;
                    hp_change(user_is_player ? 0u : 1u, old_u, 0);
                }

                ++hits_landed;
            } // end hit_loop

            if (user.hit_loop_remaining > 0) {
                message(md.name + " hit " + std::to_string(hits_landed) + " time(s)!");
                user.hit_loop_remaining = 0;
            }

            // -- P0-5: B-path secondary effects ----------------------------------
            // Crystal (PoisonMultiHit/Twineedle): effectchance fires ONCE (pass 1 only),
            // before critical. endloop rewinds to critical, bypassing effectchance on pass 2+.
            // The single pre-loop roll (last_secondary_fired) controls whether poisontarget applies.
            if (md.effect_desc.secondary_effect != SecondaryEffectType::None
                    && !target.is_fainted()
                    && md.effect_desc.has_effectchance_phase
                    && last_secondary_fired) {
                apply_secondary_effect(user, target, md.effect_desc.secondary_effect, user_is_player);
            }

        }  // end case BOpKind::Damage

        // ────────────────────────────────────────────────────────────────────
        case BOpKind::ScalePower: {
            const auto chain = static_cast<BScaleChainKind>(op.param8a);
            if (chain == BScaleChainKind::Rollout) {
                ++user.rollout_count;
                user.set_volatile(VolatileStatus::Rollout);
            } else if (chain == BScaleChainKind::FuryCutter) {
                ++user.fury_cutter_count;
            } else if (chain == BScaleChainKind::TripleKick) {
                pending_scale_mult = op.param8b;
            }
            break;
        }

        // ────────────────────────────────────────────────────────────────────
        case BOpKind::SetVolatile: {
            const int sv_result = execute_program_set_volatile(op, user, target, md, user_is_player);
            if (sv_result == 1) return MoveExecutionResult::Success;
            if (sv_result == 2) return MoveExecutionResult::Miss;
            break;
        }


        // ────────────────────────────────────────────────────────────────────
        case BOpKind::InitCounter: {
            const auto kind = static_cast<BCounterKind>(op.param8a);
            const uint8_t rmin = op.param8b;
            const uint8_t rmax = op.param8c;

            uint8_t count;
            if (rmin == rmax) {
                count = rmin;
            } else {
                // Crystal MultiHit distribution: P(2)=37.5%, P(3)=37.5%, P(4)=12.5%, P(5)=12.5%
                // Encoded as (2,5); execute using Crystal's two-roll algorithm.
                if (kind == BCounterKind::HitLoop && rmin == 2 && rmax == 5) {
                    const uint8_t r1 = rng_.next_byte() & 3u;  // 0,1,2,3
                    if (r1 < 2u) {
                        count = r1 + 1u;   // 0→1+1=2 or 1→1+1... wait
                        // Crystal: if <2 use it; then inc a gives count+1
                        // r1=0 → count+1=1+1=2 hits? Let me re-derive.
                        // .not_triple_kick: BattleRandom & 3; cp 2; jr c .got_number_hits → if <2 take it
                        // .got_number_hits: inc a → count = r1+1
                        // So r1=0 → 1, r1=1 → 2 (both are loop counter, total=counter+1 iterations)
                        // r1=0: counter=1 → loop 2 times total. r1=1: counter=2 → 3 times.
                        // r1>=2: second roll; same result.
                        count = r1 + 1u;  // 1 or 2
                    } else {
                        const uint8_t r2 = rng_.next_byte() & 3u;
                        count = r2 + 1u;  // 1,2,3,4
                    }
                    // count is the loop_counter; total hits = count+1
                    // But InitCounter stores hit_loop_remaining, and Damage fires count times.
                    // To get the correct distribution: store count+1 directly.
                    count = count + 1u;  // 2..5 total hits
                } else {
                    count = rmin + (rng_.next_byte() % (rmax - rmin + 1u));
                }
            }

            if (op.param8d == 2u) {
                // Disable marker: set target.disable_turns and target.disabled_move.
                if (target.last_move_used == MOVE_NONE) {
                    message(md.name + " — target has no last move!");
                    return MoveExecutionResult::Miss;
                }
                if (target.disable_turns > 0) {
                    message(md.name + " — target's move is already disabled!");
                    return MoveExecutionResult::Miss;
                }
                // count already randomised as 1-7.
                target.disable_turns  = count;
                target.disabled_move  = target.last_move_used;
                message(md.name + " — " + (user_is_player ? "Opponent" : "Player")
                        + "'s move was disabled!");
                break;
            }

            if (op.param8d == 3u) {
                // Encore marker: set target.encore_turns and target.encored_move.
                if (target.last_move_used == MOVE_NONE) {
                    message(md.name + " — failed!");
                    return MoveExecutionResult::Miss;
                }
                if (target.encore_turns > 0) {
                    message(md.name + " — failed!");
                    return MoveExecutionResult::Miss;
                }
                // Verify target has PP for the last-used move.
                bool has_pp = false;
                for (size_t s = 0; s < 4; ++s) {
                    if (target.moves[s].move == target.last_move_used && target.moves[s].pp > 0) {
                        has_pp = true; break;
                    }
                }
                if (!has_pp) { message(md.name + " — failed!"); return MoveExecutionResult::Miss; }
                target.encore_turns  = count;
                target.encored_move  = target.last_move_used;
                message(md.name + " — got an encore!");
                break;
            }

            switch (kind) {
                case BCounterKind::Bide:
                    user.turn_counter = count;
                    user.bide_stored  = 0;
                    break;
                case BCounterKind::Rampage:
                    user.turn_counter = count;
                    break;
                case BCounterKind::Trapping:
                    target.trap_turns    = count;
                    target.trapping_move = move_slot < 4 ? user.moves[move_slot].move : MOVE_NONE;
                    message(md.name + " — the target was trapped!");
                    break;
                case BCounterKind::HitLoop:
                    // BeatUp: count set at runtime from eligible party members.
                    if (count == 1u) {
                        // Count the eligible party members now.
                        const bool attacker_is_player2 = user_is_player;
                        uint8_t eligible = 0;
                        if (attacker_is_player2) {
                            for (size_t i = 0; i < player_party_.size(); ++i) {
                                const Pokemon* pm = player_party_.get(i);
                                if (pm && pm->current_hp > 0 && pm->status == Status::None) ++eligible;
                            }
                        } else {
                            for (const auto& bp : opponent_party_) {
                                if (!bp.is_fainted() && bp.status == Status::None) ++eligible;
                            }
                        }
                        count = std::max(uint8_t{1}, eligible);
                    }
                    user.hit_loop_remaining = count;
                    user.beat_up_index      = 0;
                    break;
            }
            break;
        }

        // ────────────────────────────────────────────────────────────────────
        case BOpKind::ScheduleDelayed: {
            // Future Sight: compute damage now, schedule it for N turns later.
            // Damage is computed at cast time using current stats.
            const uint8_t turns = op.param8a;

            // Fail if Future Sight already pending.
            FieldState::FutureSightState& fs_state = user_is_player
                ? field_.player_future_sight : field_.opponent_future_sight;
            if (fs_state.turns > 0) {
                message(md.name + " failed!");
                return MoveExecutionResult::Miss;
            }

            // Compute damage now (standard formula, no crits, type=Normal per Crystal).
            const bool physical = (md.category == MoveCategory::Physical);
            auto ss = [this](int32_t b, int8_t s) {
                return rules_ ? apply_stat_stage(b, s, *rules_) : apply_stat_stage(b, s);
            };
            int32_t atk_stat, def_stat;
            if (physical) {
                atk_stat = ss(user.base_stats.attack, user.stages.attack);
                def_stat = ss(target.base_stats.defense, target.stages.defense);
            } else {
                atk_stat = ss(user.base_stats.special_attack, user.stages.special_attack);
                def_stat = ss(target.base_stats.special_defense, target.stages.special_defense);
            }
            DamageParams dp{};
            dp.attacker_level = user.level; dp.attack_stat = atk_stat;
            dp.defense_stat   = def_stat;   dp.move_power  = md.power;
            dp.type_effectiveness = 100;    dp.stab = false;
            dp.critical = false;            dp.burned = false;
            dp.weather  = Weather::None;    dp.move_type = md.type;
            int32_t future_dmg = rules_ ? enginemon::calculate_damage(dp, *rules_)
                                        : enginemon::calculate_damage(dp);
            future_dmg = std::max(1, future_dmg);

            fs_state.turns  = turns;
            fs_state.damage = static_cast<uint16_t>(std::min(65535, future_dmg));
            message(md.name + " — " + (user_is_player ? "Opponent" : "Player") + " foresaw an attack!");
            break;
        }

        // ────────────────────────────────────────────────────────────────────
        case BOpKind::StoreDamage: {
            // Counter / Mirror Coat: check that the COUNTER USER received damage
            // this turn with the matching category.
            // user   = the Counter/MirrorCoat user (they were hit, now retaliating)
            // target = the opponent (who dealt the damage)
            const uint8_t filter = op.param8a;  // 0=Physical, 1=Special
            if (user.damage_received_this_turn == 0) {
                message(md.name + " failed!");
                return MoveExecutionResult::Miss;
            }
            // Check category filter.
            if (filter != 2u && user.damage_category_received != filter) {
                message(md.name + " failed!");
                return MoveExecutionResult::Miss;
            }
            break;
        }

        // ────────────────────────────────────────────────────────────────────
        case BOpKind::UseStoredDamage: {
            const uint8_t mult = op.param8a;
            // Read from the COUNTER USER's recorded incoming damage, not target's.
            const int32_t stored = static_cast<int32_t>(user.damage_received_this_turn);
            if (stored == 0) {
                message(md.name + " failed!");
                return MoveExecutionResult::Miss;
            }
            int32_t retort = std::min(65535, stored * static_cast<int32_t>(mult));
            retort = std::max(1, retort);
            const int16_t old_hp = target.stats.hp;
            target.stats.hp = static_cast<int16_t>(std::max(0,
                static_cast<int32_t>(target.stats.hp) - retort));
            hp_change(user_is_player ? 1u : 0u, old_hp, target.stats.hp);
            outcome_.damage_dealt += static_cast<uint16_t>(retort);
            break;
        }

        // ────────────────────────────────────────────────────────────────────
        case BOpKind::InvokeMove: {
            const auto src = static_cast<BInvokeMoveSource>(op.param8a);

            // HealBell (param8d=7): cure status of all party members on user's side.
            if (op.param8d == 7u) {
                const bool is_player = user_is_player;
                if (is_player) {
                    for (size_t i = 0; i < player_party_.size(); ++i) {
                        Pokemon* pm = player_party_.get(i);
                        if (pm) {
                            pm->status = Status::None;
                            pm->status_data = 0;
                        }
                    }
                } else {
                    for (auto& bp : opponent_party_) {
                        bp.status = Status::None;
                        bp.status_turns = 0;
                    }
                }
                // Also clear active battler status.
                user.status = Status::None;
                user.status_turns = 0;
                message(md.name + " — cured all party members!");
                break;
            }

            MoveId invoked = MOVE_NONE;
            if (src == BInvokeMoveSource::LastOpponentMove) {
                // MirrorMove: use opponent's last used move.
                invoked = target.last_move_used;
                if (invoked == MOVE_NONE) {
                    message(md.name + " failed!");
                    return MoveExecutionResult::Miss;
                }
                // MirrorMove only blocks self-recursion — not all is_copy_move moves.
                // Crystal CheckUserMove: fails if user already has the move.
                bool user_has_it = false;
                for (size_t s = 0; s < 4; ++s) {
                    if (user.moves[s].move == invoked) { user_has_it = true; break; }
                }
                if (user_has_it) {
                    message(md.name + " failed!");
                    return MoveExecutionResult::Miss;
                }
                // Only block recursion: invoked == MirrorMove's own move id.
                if (invoked == md.id) {
                    message(md.name + " failed!");
                    return MoveExecutionResult::Miss;
                }
            } else if (op.param8d == 1u) {
                // SleepTalk: user must be asleep.
                if (user.status != Status::Sleep) {
                    message(md.name + " — user isn't asleep!");
                    return MoveExecutionResult::Miss;
                }
                // Pick a random non-disabled move the user knows.
                std::vector<size_t> usable;
                for (size_t s = 0; s < 4; ++s) {
                    if (user.moves[s].move == MOVE_NONE) continue;
                    if (user.moves[s].move == md.id) continue;  // can't Sleep Talk → Sleep Talk
                    if (user.moves[s].move == user.disabled_move) continue;
                    usable.push_back(s);
                }
                if (usable.empty()) {
                    message(md.name + " — failed!");
                    return MoveExecutionResult::Miss;
                }
                const size_t slot = usable[rng_.next_byte() % usable.size()];
                invoked = user.moves[slot].move;
            } else {
                // Metronome: random valid move not in MetronomeExcepts.
                const std::vector<MoveId>* excepts = rules_ ? &rules_->metronome_excepts : nullptr;
                constexpr uint32_t MAX_TRIES = 1000;
                for (uint32_t tries = 0; tries < MAX_TRIES; ++tries) {
                    const uint8_t rnd = rng_.next_byte();
                    MoveId candidate = static_cast<MoveId>(1 + (rnd % 251));
                    if (candidate == MOVE_NONE) continue;
                    bool excluded = false;
                    if (excepts) {
                        for (MoveId ex : *excepts) if (ex == candidate) { excluded = true; break; }
                    }
                    if (excluded) continue;
                    bool already_known = false;
                    for (size_t s = 0; s < 4; ++s)
                        if (user.moves[s].move == candidate) { already_known = true; break; }
                    if (already_known) continue;
                    if (!registries_.moves.get(candidate)) continue;
                    invoked = candidate;
                    break;
                }
                if (invoked == MOVE_NONE) {
                    message(md.name + " failed!");
                    return MoveExecutionResult::Miss;
                }
            }

            return execute_move(user, target, invoked, 0xFF, user_is_player);
        }

        // ────────────────────────────────────────────────────────────────────
        case BOpKind::CopyMoveToSlot: {
            const auto mode = static_cast<BCopyMoveMode>(op.param8a);
            const uint8_t slot_kind = op.param8b;  // 0=FindMimic, 1=FindSketch

            // Find the source move from opponent's last used move.
            MoveId source_move = target.last_move_used;
            if (source_move == MOVE_NONE) {
                message(md.name + " failed!");
                return MoveExecutionResult::Miss;
            }
            const MoveData* src_md = registries_.moves.get(source_move);
            if (!src_md) {
                message(md.name + " failed!");
                return MoveExecutionResult::Miss;
            }

            // Find the slot to replace (FindMimic=0 or FindSketch=1 in user's moves).
            const MoveId search_for = (slot_kind == 0) ? md.id : md.id;  // self (Mimic or Sketch)
            uint8_t found_slot = 0xFF;
            for (uint8_t s = 0; s < 4; ++s) {
                if (user.moves[s].move == search_for) { found_slot = s; break; }
            }
            if (found_slot == 0xFF) {
                message(md.name + " failed!");
                return MoveExecutionResult::Miss;
            }

            if (mode == BCopyMoveMode::BattleDuration) {
                // Mimic: store original for restoration on switch-out.
                user.mimic_slot          = found_slot;
                user.mimic_original_move = user.moves[found_slot].move;
                user.mimic_original_pp   = user.moves[found_slot].pp;
                user.moves[found_slot].move   = source_move;
                user.moves[found_slot].max_pp = 5;
                user.moves[found_slot].pp     = 5;
                message(md.name + " — learned " + src_md->name + "!");
            } else {
                // Sketch: permanent — write to party Pokémon on the correct side.
                user.moves[found_slot].move   = source_move;
                user.moves[found_slot].max_pp = src_md->pp;
                user.moves[found_slot].pp     = src_md->pp;
                if (user_is_player) {
                    Pokemon* pmon = player_party_.get(user.party_index);
                    if (pmon && found_slot < 4) {
                        pmon->moves[found_slot].id = source_move;
                        pmon->moves[found_slot].pp = src_md->pp;
                    }
                } else {
                    // Opponent sketching — write back to opponent_party_.
                    if (opponent_active_index_ < opponent_party_.size()) {
                        auto& op_bp = opponent_party_[opponent_active_index_];
                        if (found_slot < 4) {
                            op_bp.moves[found_slot].move   = source_move;
                            op_bp.moves[found_slot].max_pp = src_md->pp;
                            op_bp.moves[found_slot].pp     = src_md->pp;
                        }
                    }
                }
                message(md.name + " — " + (user_is_player ? "Player" : "Opponent") + " sketched " + src_md->name + "!");
            }
            break;
        }

        // ────────────────────────────────────────────────────────────────────
        case BOpKind::TransformInto: {
            // Transform: copy opponent's species, types, stats, stat stages, moves.
            if (target.has_volatile(VolatileStatus::Transformed)) {
                message(md.name + " failed!");
                return MoveExecutionResult::Miss;
            }
            // Backup user DVs.
            user.backup_dvs = static_cast<uint16_t>(
                ((user.dv_atk & 0x0F) << 12) | ((user.dv_def & 0x0F) << 8) |
                ((user.dv_spd & 0x0F) << 4)  |  (user.dv_spc & 0x0F));
            // Copy target attributes.
            user.species = target.species;
            user.type1   = target.type1;
            user.type2   = target.type2;
            user.stages  = target.stages;
            user.base_stats = target.base_stats;
            user.stats.attack          = target.stats.attack;
            user.stats.defense         = target.stats.defense;
            user.stats.speed           = target.stats.speed;
            user.stats.special_attack  = target.stats.special_attack;
            user.stats.special_defense = target.stats.special_defense;
            // Copy moves with PP=5 each.
            for (size_t s = 0; s < 4; ++s) {
                user.moves[s].move   = target.moves[s].move;
                user.moves[s].max_pp = 5;
                user.moves[s].pp     = 5;
            }
            // Copy DVs from target.
            user.dv_atk = target.dv_atk;
            user.dv_def = target.dv_def;
            user.dv_spd = target.dv_spd;
            user.dv_spc = target.dv_spc;
            user.set_volatile(VolatileStatus::Transformed);
            message(md.name + " — " + (user_is_player ? "Player" : "Opponent") + " transformed!");
            break;
        }

        // ────────────────────────────────────────────────────────────────────
        case BOpKind::ForceSwitch: {
            if (op.param8d == 6u) {
                // BatonPass: switch out user passing select volatile state + stat stages.
                // Source: pokecrystal baton_pass.asm ResetBatonPassStatus.
                //
                // ResetBatonPassStatus explicitly CLEARS (not passed):
                //   - Nightmare (if target is not asleep)
                //   - Infatuation (both sides)
                //   - Transformed
                //   - Encored
                //   - Trap count (wPlayerWrapCount / wEnemyWrapCount)
                //
                // Also excluded (turn-start cleared or naturally unreachable with Baton Pass):
                //   - Flinch, Protect, Endure (turn-transient)
                //   - DestinyBond (cleared at turn start)
                //   - Recharge (can't Baton Pass while recharging)
                //   - Identified/Foresight (cleared on switch)
                //   - Charging, Flying, Underground (CheckPlayerLockedIn gates on SUBSTATUS_CHARGED;
                //       two-turn move forces continuation — Baton Pass unreachable while charging)
                //   - Bide, Rampage (locked multi-turn move state; same forced-continuation gate)
                //
                // PASSED (surviving substatus bits + wPlayerMinimized, a separate RAM
                //   byte never touched by ResetBatonPassStatus):
                //   Confusion, Substitute, FocusEnergy, Seeded, Cursed, Mist, LockOn,
                //   CantRun, Perish, Rage, Rollout, Curled, Minimized.
                //
                // Opponent path: EnemySwitch_SetMode does NOT call NewEnemyMonStatus or
                //   ResetEnemyStatLevels — substatus bytes and stat levels carry over
                //   identically to player path.
                //
                // Counters transferred atomically with their volatile bits:
                //   confusion_turns, substitute_hp, perish_count, rollout_count,
                //   fury_cutter_count, rage_accumulator.
                const uint32_t pass_mask =
                      static_cast<uint32_t>(VolatileStatus::Confusion)
                    | static_cast<uint32_t>(VolatileStatus::Substitute)
                    | static_cast<uint32_t>(VolatileStatus::FocusEnergy)
                    | static_cast<uint32_t>(VolatileStatus::Seeded)
                    | static_cast<uint32_t>(VolatileStatus::Cursed)
                    | static_cast<uint32_t>(VolatileStatus::Mist)
                    | static_cast<uint32_t>(VolatileStatus::LockOn)
                    | static_cast<uint32_t>(VolatileStatus::CantRun)
                    | static_cast<uint32_t>(VolatileStatus::Perish)
                    | static_cast<uint32_t>(VolatileStatus::Rage)
                    | static_cast<uint32_t>(VolatileStatus::Rollout)
                    | static_cast<uint32_t>(VolatileStatus::Curled)
                    | static_cast<uint32_t>(VolatileStatus::Minimized);
                if (user_is_player) {
                    const auto switches = available_switches_player();
                    if (switches.empty()) {
                        message(md.name + " — no Pokemon to switch to!");
                        return MoveExecutionResult::Miss;
                    }
                    const size_t pick = switches[0];
                    const auto     saved_stages   = user.stages;
                    const uint32_t passed_vs      = user.volatile_status & pass_mask;
                    const uint8_t  saved_conf     = user.confusion_turns;
                    const uint16_t saved_sub_hp   = user.substitute_hp;
                    const uint8_t  saved_perish   = user.perish_count;
                    const uint8_t  saved_rollout  = user.rollout_count;
                    const uint8_t  saved_fury     = user.fury_cutter_count;
                    const uint8_t  saved_rage     = user.rage_accumulator;
                    force_switch_player(pick);
                    // Stat stages: Crystal player Baton Pass preserves stages.
                    player_pokemon_.stages            = saved_stages;
                    player_pokemon_.volatile_status  |= passed_vs;
                    player_pokemon_.confusion_turns   = saved_conf;
                    player_pokemon_.substitute_hp     = saved_sub_hp;
                    player_pokemon_.perish_count      = saved_perish;
                    player_pokemon_.rollout_count     = saved_rollout;
                    player_pokemon_.fury_cutter_count = saved_fury;
                    player_pokemon_.rage_accumulator  = saved_rage;
                    message(md.name + " — passed the baton!");
                } else {
                    // Opponent Baton Pass: pick first available opponent party member.
                    // Crystal: EnemySwitch_SetMode does NOT call NewEnemyMonStatus or
                    //   ResetEnemyStatLevels — stat stages and substatus carry over,
                    //   same as player path. Stages ARE preserved.
                    std::vector<size_t> opp_sw;
                    for (size_t i = 0; i < opponent_party_.size(); ++i)
                        if (i != opponent_active_index_ && !opponent_party_[i].is_fainted())
                            opp_sw.push_back(i);
                    if (opp_sw.empty()) {
                        message(md.name + " — failed!");
                        return MoveExecutionResult::Miss;
                    }
                    const size_t opick = opp_sw[0];
                    const auto     saved_stages   = user.stages;
                    const uint32_t passed_vs      = user.volatile_status & pass_mask;
                    const uint8_t  saved_conf     = user.confusion_turns;
                    const uint16_t saved_sub_hp   = user.substitute_hp;
                    const uint8_t  saved_perish   = user.perish_count;
                    const uint8_t  saved_rollout  = user.rollout_count;
                    const uint8_t  saved_fury     = user.fury_cutter_count;
                    const uint8_t  saved_rage     = user.rage_accumulator;
                    force_switch_opponent(opick);
                    // Stat stages preserved (Crystal enemy path does NOT reset them).
                    opponent_pokemon_.stages          = saved_stages;
                    opponent_pokemon_.volatile_status |= passed_vs;
                    opponent_pokemon_.confusion_turns  = saved_conf;
                    opponent_pokemon_.substitute_hp    = saved_sub_hp;
                    opponent_pokemon_.perish_count     = saved_perish;
                    opponent_pokemon_.rollout_count    = saved_rollout;
                    opponent_pokemon_.fury_cutter_count = saved_fury;
                    opponent_pokemon_.rage_accumulator  = saved_rage;
                    message(md.name + " — opponent passed the baton!");
                }
                return MoveExecutionResult::Success;
            }

            // Standard ForceSwitch: force opponent to a random live party member.
            if (type_ == BattleType::Wild) {
                message(md.name + " — the wild Pokémon fled!");
                result_ = BattleResult::Draw;
                return MoveExecutionResult::Success;
            }
            std::vector<size_t> available;
            for (size_t i = 0; i < opponent_party_.size(); ++i) {
                if (i != opponent_active_index_ && !opponent_party_[i].is_fainted())
                    available.push_back(i);
            }
            if (available.empty()) {
                message(md.name + " failed!");
                return MoveExecutionResult::Miss;
            }
            const size_t pick = available[rng_.next_byte() % available.size()];
            force_switch_opponent(pick);
            message(md.name + " — the opponent was forced out!");
            break;
        }

        // ────────────────────────────────────────────────────────────────────
        case BOpKind::TransferItem: {
            // Thief: transfer target's held item to user if user has none.
            if (user.held_item != ITEM_NONE) break;
            if (target.held_item == ITEM_NONE) break;
            user.held_item   = target.held_item;
            target.held_item = ITEM_NONE;
            // Write back to the relevant party record so the change persists after battle.
            if (user_is_player) {
                // Player stole from opponent — clear opponent party slot.
                if (opponent_active_index_ < opponent_party_.size())
                    opponent_party_[opponent_active_index_].held_item = ITEM_NONE;
            } else {
                // Opponent stole from player — clear player party slot.
                Pokemon* pmon = player_party_.get(user.party_index);
                if (pmon) pmon->held_item = ITEM_NONE;
            }
            message(md.name + " — " + (user_is_player ? "Player" : "Opponent") + " stole the item!");
            break;
        }

        } // switch op.kind
    } // for ops

    // -- King's Rock (PostHitFlinch): B-path, exactly once per move ---------------
    // Source: Crystal BattleCommand_HeldFlinch (kingsrock command, 0x4D).
    // Placed after ALL ops to guarantee exactly one roll even for TripleKick (3 Damage ops).
    // Fires only when move_hit=true (acc check passed) and target not behind Substitute.
    if (move_hit) {
        apply_kings_rock(md.effect_desc, user, target);
    }

    user.last_move_used = md.id;
    return MoveExecutionResult::Success;
}

int Battle::execute_program_set_volatile(const BOp& op, BattlePokemon& user,
                                          BattlePokemon& target, const MoveData& md,
                                          bool user_is_player) {
    const uint32_t bits  = op.param32;
    const auto tgt       = static_cast<BVolatileTarget>(op.param8a);
    const bool set_value = (op.param8b != 0);
    BattlePokemon& affected = (tgt == BVolatileTarget::User) ? user : target;

            if (op.param8d == 4u) {
                // Curse (Ghost path): check user's type at runtime.
                // Non-Ghost: Attack+1, Defense+1, Speed-1 (no HP cost, no curse volatile).
                // Ghost: lose 50% HP, set Cursed volatile on target.
                const bool user_is_ghost = (user.type1 == 8 || user.type2 == 8);  // type 8 = GHOST
                if (user_is_ghost) {
                    // 50% HP cost
                    const int32_t cost = std::max(1, static_cast<int32_t>(user.stats.max_hp) / 2);
                    if (user.stats.hp <= cost) {
                        message(md.name + " — not enough HP!");
                        return 2;
                    }
                    const int16_t old_user_hp = user.stats.hp;
                    user.stats.hp = static_cast<int16_t>(user.stats.hp - cost);
                    hp_change(user_is_player ? 0u : 1u, old_user_hp, user.stats.hp);
                    // Set Cursed volatile on target
                    target.set_volatile(VolatileStatus::Cursed);
                    message((user_is_player ? std::string("Opponent") : std::string("Player"))
                            + " was cursed!");
                } else {
                    // Non-Ghost: stat stages
                    apply_one_stage_change(user,   0, +1);  // Attack +1
                    apply_one_stage_change(user,   1, +1);  // Defense +1
                    apply_one_stage_change(user,   2, -1);  // Speed -1
                    message(md.name + " — stat changes applied!");
                }
                return 0;
            }

            if (op.param8d == 5u || op.param8d == 8u) {
                // Protect (param8d=5) or Endure (param8d=8): probability check.
                // Source: suiCune move_effects/protect.c ProtectChance()
                //
                // Crystal exact behavior:
                //   1. Fail if opponent went first this turn.
                //   2. Fail if user has an active Substitute.
                //   3. threshold = 0xFF >> consecutive_count
                //   4. If threshold == 0: fail, reset counter.
                //   5. Loop: roll = BattleRandom(); if roll == 0 → resample
                //   6. Success iff (roll - 1) < threshold  (so P = threshold/255)
                //   7. On failure: reset consecutive counter to 0.
                //   8. On success: increment consecutive counter.
                //
                // Consequences:
                //   count=0: 255/255 = 100% (zero always resampled)
                //   count=1: 127/255 ≈ 49.8%
                //   count=2: 63/255 ≈ 24.7%
                //   ...
                //   count=8: threshold=0 → always fail, reset
                const bool user_went_first = (user_is_player == player_goes_first_);
                if (!user_went_first) {
                    message(md.name + " — failed!");
                    user.protect_consecutive = 0;
                    return 2;
                }
                // Crystal: fail if user has Substitute (BattleCommand_CheckHit_DrainSub gate).
                if (user.has_volatile(VolatileStatus::Substitute)) {
                    message(md.name + " — failed!");
                    user.protect_consecutive = 0;
                    return 2;
                }
                const uint8_t threshold = (user.protect_consecutive < 8)
                    ? static_cast<uint8_t>(0xFF >> user.protect_consecutive) : 0u;
                if (threshold == 0u) {
                    message(md.name + " — failed!");
                    user.protect_consecutive = 0;
                    return 2;
                }
                // Crystal: resample until non-zero, then check (roll-1) < threshold.
                // This gives P(success) = threshold/255 (denominator 255 not 256).
                uint8_t roll;
                do { roll = rng_.next_byte(); } while (roll == 0u);
                if (static_cast<uint8_t>(roll - 1u) >= threshold) {
                    message(md.name + " — failed!");
                    user.protect_consecutive = 0;
                    return 2;
                }
                ++user.protect_consecutive;
                const VolatileStatus vs_protect = (op.param8d == 5u)
                    ? VolatileStatus::Protect : VolatileStatus::Endure;
                user.set_volatile(vs_protect);
                message(md.name + " — " + (user_is_player ? "Player" : "Opponent") + " protected itself!");
                return 0;
            }

            if (bits) {  // SetVolatile with a real flag
                const auto vs = static_cast<VolatileStatus>(bits);
                // Save pre-set state: needed for Charging defer-only-on-first-set logic.
                const bool was_charging_before =
                    (vs == VolatileStatus::Charging) && affected.has_volatile(VolatileStatus::Charging);
                if (set_value) {
                    affected.set_volatile(vs);

                    // ── Curled + param8c=1: also apply DefenseUp1 (DefenseCurl). ─
                    if (vs == VolatileStatus::Curled && op.param8c == 1u) {
                        apply_one_stage_change(user, 1, +1);  // Defense +1
                        message(md.name + " — " + (user_is_player ? "Player" : "Opponent")
                                + "'s Defense rose!");
                    }
                    // ── Charging + param8c=1: SkullBash Defense +1. ────────────
                    // P1-22: Gate on !was_charging_before so the boost fires only on
                    // the charge turn (turn 1), not again on the fire turn (turn 2).
                    // Crystal: BattleCommand_SkullBash calls DefenseUp1 only when
                    // wVolatileFlags does NOT already have the Charging bit set.
                    if (vs == VolatileStatus::Charging && op.param8c == 1u && !was_charging_before) {
                        apply_one_stage_change(affected, 1, +1);
                    }
                    // ── Charging + param8d=1: SolarBeam skip-sun-check. ────────
                    if (vs == VolatileStatus::Charging && op.param8d == 1u) {
                        if (field_.weather == Weather::Sun) {
                            // Sun: skip charge, fire damage immediately.
                            affected.clear_volatile(VolatileStatus::Charging);
                        }
                        // Non-Sun: leave Charging set; defer to fire on next turn.
                        // was_charging_before is false on turn 1 (we just set it).
                        // The defer is applied after this block.
                    }
                    // ── Seeded (LeechSeed): fail if target is Grass type. ──────
                    if (vs == VolatileStatus::Seeded) {
                        // Crystal type_constants.h: GRASS = 22 (not 12 which is ROCK).
                        // Source: suiCune/constants/type_constants.h enum order:
                        //   NORMAL=0...STEEL=9, CURSE_TYPE=19, FIRE=20, WATER=21, GRASS=22.
                        const bool target_is_grass = (target.type1 == 22 || target.type2 == 22); // 22=GRASS
                        if (target_is_grass) {
                            target.clear_volatile(VolatileStatus::Seeded);
                            message(md.name + " — it doesn't affect Grass types!");
                            return 2;
                        }
                        if (target.has_volatile(VolatileStatus::Seeded)) {
                            // Silently prevent re-seeding (Seeded was set before clear check)
                        }
                        message(md.name + " — a seed was planted!");
                    }
                    // ── Nightmare: fail if target not asleep. ─────────────────
                    if (vs == VolatileStatus::Nightmare) {
                        if (target.status != Status::Sleep) {
                            target.clear_volatile(VolatileStatus::Nightmare);
                            message(md.name + " — failed! Target isn't asleep.");
                            return 2;
                        }
                        message(md.name + " — a nightmare began!");
                    }
                    // ── DestinyBond message ───────────────────────────────────
                    if (vs == VolatileStatus::DestinyBond) {
                        message(md.name + " — if the user faints, the foe will too!");
                    }
                    // ── LockOn message ────────────────────────────────────────
                    if (vs == VolatileStatus::LockOn) {
                        if (target.has_volatile(VolatileStatus::Substitute)) {
                            target.clear_volatile(VolatileStatus::LockOn);
                            message(md.name + " — failed!");
                            return 2;
                        }
                        message(md.name + " — took aim!");
                    }
                    // ── Perish (PerishSong): set count=4 on both. ─────────────
                    if (vs == VolatileStatus::Perish) {
                        // When applied to User: set user.perish_count=4
                        if (tgt == BVolatileTarget::User) {
                            user.perish_count = 4;
                        } else {
                            target.perish_count = 4;
                        }
                    }
                    // ── CantRun (MeanLook): fail if target is hidden or already trapped. ─
                    if (vs == VolatileStatus::CantRun) {
                        if (target.has_volatile(VolatileStatus::Flying)
                         || target.has_volatile(VolatileStatus::Underground)) {
                            target.clear_volatile(VolatileStatus::CantRun);
                            message(md.name + " — failed!");
                            return 2;
                        }
                        message(md.name + " — can't escape now!");
                    }
                    // ── Identified (Foresight) ────────────────────────────────
                    if (vs == VolatileStatus::Identified) {
                        if (target.has_volatile(VolatileStatus::Flying)
                         || target.has_volatile(VolatileStatus::Underground)) {
                            target.clear_volatile(VolatileStatus::Identified);
                            message(md.name + " — failed!");
                            return 2;
                        }
                        message(md.name + " — target was identified!");
                    }
                    // ── Infatuation (Attract): gender check. ──────────────────
                    if (vs == VolatileStatus::Infatuation) {
                        // Check opposite genders using Crystal formula.
                        // gender_dv = (dv_atk << 4) | dv_spd. Female if gender_dv <= gender_ratio.
                        const uint8_t user_gd = static_cast<uint8_t>((user.dv_atk << 4) | user.dv_spd);
                        const uint8_t tgt_gd  = static_cast<uint8_t>((target.dv_atk << 4) | target.dv_spd);
                        const bool user_female  = (user.gender_ratio  != 255 && user_gd  <= user.gender_ratio);
                        const bool user_male    = (user.gender_ratio  != 255 && user_gd  > user.gender_ratio);
                        const bool tgt_female   = (target.gender_ratio != 255 && tgt_gd  <= target.gender_ratio);
                        const bool tgt_male     = (target.gender_ratio != 255 && tgt_gd  > target.gender_ratio);
                        const bool user_genderless = (user.gender_ratio  == 255);
                        const bool tgt_genderless  = (target.gender_ratio == 255);
                        // Attract fails if genderless or same gender.
                        const bool opposite = (!user_genderless && !tgt_genderless)
                                           && ((user_female && tgt_male) || (user_male && tgt_female));
                        if (!opposite) {
                            target.clear_volatile(VolatileStatus::Infatuation);
                            message(md.name + " — it doesn't work on the target!");
                            return 2;
                        }
                        // Already in love?
                        if (target.has_volatile(VolatileStatus::Infatuation)) {
                            // was set above and already had it — no need to double-set
                        }
                        message(md.name + " — the target fell in love!");
                    }
                } else {
                    affected.clear_volatile(vs);
                }

                // ── Charging DeferTurn: on charge turn, set Charging and defer ──────
                // Only defer if Charging was NOT already set before this op ran.
                // On the fire turn, Charging IS already set (from charge turn); the Damage op
                // clears it and proceeds. The SetVolatile(Charging) op must not re-defer.
                if (bits == static_cast<uint32_t>(VolatileStatus::Charging) && set_value
                        && !was_charging_before) {
                    // DeferTurn for non-Solar (param8d==0): always defer on charge turn.
                    if (op.param8d == 0u) {
                        // Standard charge: Fly/Dig/RazorWind/SkyAttack/SkullBash
                        message(md.name + " — " + (user_is_player ? "Player" : "Opponent") + " is charging!");
                        return 1;
                    } else if (op.param8d == 1u) {
                        // SolarBeam: already cleared Charging in Sun above.
                        // If still set (non-Sun): defer.
                        if (affected.has_volatile(VolatileStatus::Charging)) {
                            message(md.name + " — absorbing sunlight!");
                            return 1;
                        }
                        // Sun path: Charging already cleared; fall through to Damage.
                    }
                }
            }

            // ── Substitute HP cost (param8c=1) ────────────────────────────────
            if (bits == static_cast<uint32_t>(VolatileStatus::Substitute) && set_value && op.param8c == 1u) {
                const int32_t cost = std::max(1, static_cast<int32_t>(user.stats.max_hp) / 4);
                if (user.stats.hp <= cost) {
                    user.clear_volatile(VolatileStatus::Substitute);
                    message(md.name + " — too weak to create a substitute!");
                    return 2;
                }
                user.substitute_hp = static_cast<uint16_t>(cost);
                const int16_t old_hp = user.stats.hp;
                user.stats.hp = static_cast<int16_t>(user.stats.hp - cost);
                hp_change(user_is_player ? 0u : 1u, old_hp, user.stats.hp);
                message(md.name + " — " + (user_is_player ? "Player" : "Opponent") + " created a substitute!");
            }

            // ── FocusEnergy message ───────────────────────────────────────────
            if (bits == static_cast<uint32_t>(VolatileStatus::FocusEnergy) && set_value) {
                message(md.name + " — " + (user_is_player ? "Player" : "Opponent") + " is getting pumped!");
            }

    return 0;  // continue executing ops
}

// ============================================================================
// Engine Event Hooks
// ============================================================================

// HOOK 1: OnDamageReceived — fires inside apply_damage() after HP subtraction.
// Records damage and category for Counter / Mirror Coat and Bide accumulation.
void Battle::hook_on_damage_received(BattlePokemon& defender, int32_t damage,
                                      uint8_t move_category) {
    defender.damage_received_this_turn = static_cast<uint16_t>(
        std::min(65535, static_cast<int32_t>(defender.damage_received_this_turn) + damage));
    defender.damage_category_received = move_category;

    // Bide: accumulate incoming damage.
    if (defender.has_volatile(VolatileStatus::Bide)) {
        defender.bide_stored = static_cast<uint16_t>(
            std::min(65535, static_cast<int32_t>(defender.bide_stored) + damage));
    }

    // Rage: increment rage_accumulator (outgoing damage multiplier, NOT Attack stage).
    // Crystal Rage: outgoing damage = base × (1 + rage_accumulator). No Attack stage change.
    // FIXED: removed incorrect "apply_one_stage_change(defender, 0, +1)".
    if (defender.has_volatile(VolatileStatus::Rage)) {
        if (defender.rage_accumulator < 255) ++defender.rage_accumulator;
    }
}

// HOOK 3: FutureSightTick — fires at start of end-of-turn, before weather.
void Battle::hook_future_sight_tick() {
    for (int side = 0; side < 2; ++side) {
        FieldState::FutureSightState& fs = (side == 0)
            ? field_.player_future_sight : field_.opponent_future_sight;
        if (fs.turns == 0) continue;
        --fs.turns;
        if (fs.turns == 0) {
            // Fire the stored damage against the opponent of the original caster.
            BattlePokemon& victim = (side == 0) ? opponent_pokemon_ : player_pokemon_;
            if (!victim.is_fainted()) {
                const int16_t old_hp = victim.stats.hp;
                victim.stats.hp = static_cast<int16_t>(
                    std::max(0, static_cast<int32_t>(victim.stats.hp) - fs.damage));
                hp_change((side == 0) ? 1u : 0u, old_hp, victim.stats.hp);
                message("The Future Sight attack struck!");
                outcome_.damage_dealt += fs.damage;
            }
            fs.damage = 0;
        }
    }
}

// HOOK 4: TrapDamageTick — fires after weather in end-of-turn.
void Battle::hook_trap_damage_tick() {
    auto tick = [&](BattlePokemon& victim, bool is_player) {
        if (!victim.has_volatile(VolatileStatus::Trapped)) return;
        if (victim.trap_turns == 0) {
            victim.clear_volatile(VolatileStatus::Trapped);
            victim.trapping_move = MOVE_NONE;
            message((is_player ? std::string("Player") : std::string("Opponent"))
                    + " was released from the trapping move!");
            return;
        }
        --victim.trap_turns;
        // Residual: GetSixteenthMaxHP (max_hp / 16).
        const int32_t denom = rules_ ? static_cast<int32_t>(rules_->get_toxic_denom()) : 16;
        const int32_t trap_dmg = std::max(1, static_cast<int32_t>(victim.stats.max_hp) / denom);
        const int16_t old_hp = victim.stats.hp;
        victim.stats.hp = static_cast<int16_t>(std::max(0,
            static_cast<int32_t>(victim.stats.hp) - trap_dmg));
        hp_change(is_player ? 0u : 1u, old_hp, victim.stats.hp);
        if (victim.trap_turns == 0) {
            victim.clear_volatile(VolatileStatus::Trapped);
            victim.trapping_move = MOVE_NONE;
        }
    };
    tick(player_pokemon_,   true);
    tick(opponent_pokemon_,  false);
}

// HOOK 5 (in execute_turn): LockCheck — handled inline in determine_turn_order / action dispatch.
// The lock is checked in execute_turn's AI section; we expose a helper.
// HOOK 6: ChainReset — fires on miss or ActionSwitch.
void Battle::hook_chain_reset(BattlePokemon& combatant) {
    if (combatant.rollout_count > 0) {
        combatant.rollout_count = 0;
        combatant.clear_volatile(VolatileStatus::Rollout);
    }
    if (combatant.fury_cutter_count > 0) {
        combatant.fury_cutter_count = 0;
    }
}

// HOOK 7: PursuitCheck — fires before a switch resolves.
MoveExecutionResult Battle::hook_pursuit_check(BattlePokemon& pursuer, BattlePokemon& switcher,
                                                bool pursuer_is_player) {
    // Find Pursuit in pursuer's moves.
    for (size_t s = 0; s < 4; ++s) {
        if (pursuer.moves[s].move == MOVE_NONE) continue;
        const MoveData* pmd = registries_.moves.get(pursuer.moves[s].move);
        if (!pmd) continue;
        if (!pmd->has_program || !pmd->effect_desc.is_pursuit) continue;
        // Temporarily double the move power for this execution.
        // We do this by temporarily setting is_switching on the switcher
        // (the program executor uses this to detect the double-power case).
        switcher.is_switching = true;
        const MoveExecutionResult res = execute_move(pursuer, switcher,
            pursuer.moves[s].move, s, pursuer_is_player);
        switcher.is_switching = false;
        return res;
    }
    return MoveExecutionResult::NoTarget;  // No Pursuit move found
}

// HOOK 8: RampageEndCheck — fires at end-of-turn for each combatant.
// Crystal model: counter set once on turn 1 to rand&1+1 = 1 or 2.
// Each continuation turn: counter decremented. When counter reaches 0:
//   clear RAMPAGE, apply confusion (unless Safeguard), damage fired that same turn.
//   RAMPAGE cleared; player is no longer locked next turn.
// This hook only handles post-turn cleanup; the damage-phase is done in execute_program.
void Battle::hook_rampage_end_check(BattlePokemon& combatant, bool is_player) {
    if (!combatant.has_volatile(VolatileStatus::Rampage)) return;
    if (combatant.turn_counter > 0) {
        --combatant.turn_counter;
        if (combatant.turn_counter == 0) {
            // Last continuation turn: clear RAMPAGE.
            combatant.clear_volatile(VolatileStatus::Rampage);
            combatant.turn_counter = 0;
            // Apply confusion unless Safeguard protects.
            const bool safeguarded = is_player
                ? (field_.safeguard_player > 0)
                : (field_.safeguard_opponent > 0);
            if (!safeguarded && !combatant.has_volatile(VolatileStatus::Confusion)) {
                combatant.set_volatile(VolatileStatus::Confusion);
                // P1-Confusion: duration 2–5 turns. Source: BattleCommand_FinishConfusingTarget.
                // Rampage uses the same confusion init as a regular confusion move.
                combatant.confusion_turns = (rng_.next_byte() & 0x03u) + 2u;
                message((is_player ? std::string("Player") : std::string("Opponent"))
                        + " became confused from the rampage!");
            }
        }
    }
}

// HOOK 9: BideGate — returns true if Bide is active and suppresses normal move.
// Called from execute_action before dispatching Fight action.
bool Battle::hook_bide_gate(BattlePokemon& combatant, bool is_player) {
    if (!combatant.has_volatile(VolatileStatus::Bide)) return false;
    if (combatant.turn_counter > 0) {
        --combatant.turn_counter;
        message((is_player ? std::string("Player") : std::string("Opponent"))
                + " is storing energy!");
        return true;
    }
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// New EndOfTurn hooks
// ─────────────────────────────────────────────────────────────────────────────

void Battle::hook_end_of_turn_leech_seed() {
    auto drain = [&](BattlePokemon& victim, BattlePokemon& healer, bool victim_is_player) {
        if (!victim.has_volatile(VolatileStatus::Seeded)) return;
        if (victim.is_fainted()) return;
        // Skip drain if victim is Flying or Underground.
        if (victim.has_volatile(VolatileStatus::Flying)
         || victim.has_volatile(VolatileStatus::Underground)) return;
        // Drain: 1/8 max_hp.
        const int32_t drain_hp = std::max(1, static_cast<int32_t>(victim.stats.max_hp) / 8);
        const int16_t old_victim = victim.stats.hp;
        victim.stats.hp = static_cast<int16_t>(
            std::max(0, static_cast<int32_t>(victim.stats.hp) - drain_hp));
        hp_change(victim_is_player ? 0u : 1u, old_victim, victim.stats.hp);
        message((victim_is_player ? std::string("Player") : std::string("Opponent"))
                + "'s HP was sapped by Leech Seed!");
        // Heal healer (capped at max_hp).
        if (!healer.is_fainted()) {
            const int16_t old_healer = healer.stats.hp;
            healer.stats.hp = static_cast<int16_t>(std::min(
                static_cast<int32_t>(healer.stats.max_hp),
                static_cast<int32_t>(healer.stats.hp) + drain_hp));
            if (healer.stats.hp != old_healer)
                hp_change(victim_is_player ? 1u : 0u, old_healer, healer.stats.hp);
        }
    };
    drain(player_pokemon_,   opponent_pokemon_, true);
    drain(opponent_pokemon_, player_pokemon_,  false);
}

void Battle::hook_end_of_turn_nightmare() {
    auto tick = [&](BattlePokemon& bp, bool is_player) {
        if (!bp.has_volatile(VolatileStatus::Nightmare)) return;
        if (bp.status != Status::Sleep) {
            // Nightmare clears when target wakes.
            bp.clear_volatile(VolatileStatus::Nightmare);
            return;
        }
        const int32_t dmg = std::max(1, static_cast<int32_t>(bp.stats.max_hp) / 4);
        const int16_t old = bp.stats.hp;
        bp.stats.hp = static_cast<int16_t>(std::max(0, static_cast<int32_t>(bp.stats.hp) - dmg));
        hp_change(is_player ? 0u : 1u, old, bp.stats.hp);
        message((is_player ? std::string("Player") : std::string("Opponent"))
                + " is having a nightmare!");
    };
    tick(player_pokemon_,   true);
    tick(opponent_pokemon_, false);
}

void Battle::hook_end_of_turn_curse() {
    auto tick = [&](BattlePokemon& bp, bool is_player) {
        if (!bp.has_volatile(VolatileStatus::Cursed)) return;
        const int32_t dmg = std::max(1, static_cast<int32_t>(bp.stats.max_hp) / 4);
        const int16_t old = bp.stats.hp;
        bp.stats.hp = static_cast<int16_t>(std::max(0, static_cast<int32_t>(bp.stats.hp) - dmg));
        hp_change(is_player ? 0u : 1u, old, bp.stats.hp);
        message((is_player ? std::string("Player") : std::string("Opponent"))
                + " is afflicted by the curse!");
    };
    tick(player_pokemon_,   true);
    tick(opponent_pokemon_, false);
}

void Battle::hook_end_of_turn_perish_song() {
    auto tick = [&](BattlePokemon& bp, bool is_player) {
        if (!bp.has_volatile(VolatileStatus::Perish)) return;
        if (bp.is_fainted()) return;
        message((is_player ? std::string("Player") : std::string("Opponent"))
                + "'s perish count fell to " + std::to_string(bp.perish_count) + "!");
        if (bp.perish_count == 0) {
            // Faint.
            const int16_t old = bp.stats.hp;
            bp.stats.hp = 0;
            hp_change(is_player ? 0u : 1u, old, 0);
            bp.clear_volatile(VolatileStatus::Perish);
        } else {
            --bp.perish_count;
        }
    };
    tick(player_pokemon_,   true);
    tick(opponent_pokemon_, false);
}

// Clear Protect/Endure/DestinyBond at the start of the user's turn, before the
// effect script runs. Crystal: EndUserDestinyBond + EndOpponentProtectEndureDestinyBond.
void Battle::hook_pre_move_clear_protect(BattlePokemon& current_actor) {
    // Clear Protect and Endure only for the CURRENT ACTOR at the start of their action.
    // Crystal: EndOpponentProtect clears the OPPONENT's Protect/Endure at the start of
    // their action (i.e., the actor that just started their turn).
    // This preserves the FIRST actor's Protect/Endure until the second actor moves against them.
    // Both Protect and Endure last exactly one turn (cleared at the start of the owning
    // pokemon's NEXT action, which is when this hook fires for that pokemon).
    current_actor.clear_volatile(VolatileStatus::Protect);
    current_actor.clear_volatile(VolatileStatus::Endure);
    // NOTE: DestinyBond cleared separately via hook_pre_move_clear_destiny_bond.
}
void Battle::hook_pre_move_clear_destiny_bond(BattlePokemon& user) {
    // Clear DestinyBond on the current actor before they act.
    // Crystal: EndUserDestinyBond fires at the start of the user's turn.
    // This preserves DestinyBond on the opponent side until the opponent acts.
    user.clear_volatile(VolatileStatus::DestinyBond);
}
bool Battle::hook_pre_move_check(BattlePokemon& user, BattlePokemon& target,
                                   size_t move_slot, bool user_is_player) {
    // Source: BattleCommand_CheckTurn / CheckEnemyTurn in effect_commands.asm.
    // All status conditions are checked in this exact order (Crystal order preserved).

    // ── Flinch ────────────────────────────────────────────────────────────────
    // P1-Flinch: Flinch was already cleared at execute_turn() start for both sides.
    // Any flinch that survived to here means the user was flinched after the turn-start
    // clear (i.e. this is the second actor who hasn't moved yet). Consume and block.
    // Source: effect_commands.asm lines ~213-221 / ~439-448.
    if (user.has_volatile(VolatileStatus::Flinch)) {
        user.clear_volatile(VolatileStatus::Flinch);  // res SUBSTATUS_FLINCHED
        message((user_is_player ? std::string("Player") : std::string("Opponent"))
                + " flinched and couldn't move!");
        return true;
    }

    // ── Sleep ─────────────────────────────────────────────────────────────────
    // P1-Sleep: Decrement counter at start of acting turn. If counter reaches 0 = woke up.
    // P1-Sleep: Crystal wake-up behavior (effect_commands.asm .woke_up path):
    //   dec a; ld [wBattleMonStatus], a; and SLP_MASK; jr z, .woke_up
    //   .woke_up: StdBattleTextbox, CantMove (clears volatiles), UpdateParty, clear Nightmare
    //             jr .not_asleep  ← FALLS THROUGH, does NOT call EndTurn
    // The mon DOES act on the wake turn — it continues through freeze/flinch/confusion/
    // paralysis checks and then executes its move.
    // Source: effect_commands.asm lines 148-195 (player) / 378-424 (enemy).
    if (user.status == Status::Sleep) {
        if (user.status_turns > 0) {
            --user.status_turns;
        }
        if (user.status_turns == 0) {
            // Woke up — clear Sleep and Nightmare, then FALL THROUGH to remaining checks.
            // Crystal: jr .not_asleep after CantMove housekeeping; EndTurn is NOT called.
            // The mon proceeds through freeze/flinch/confusion/paralysis and may act.
            user.status = Status::None;
            message((user_is_player ? std::string("Player") : std::string("Opponent"))
                    + " woke up!");
            user.clear_volatile(VolatileStatus::Nightmare);
            // Do NOT return true here — fall through to remaining pre-move checks.
        } else {
            // Still asleep. Snore and Sleep Talk bypass sleep.
            const MoveId mid = (move_slot < 4) ? user.moves[move_slot].move : MOVE_NONE;
            const MoveData* md_ptr = registries_.moves.get(mid);
            if (md_ptr && (md_ptr->effect_desc.requires_user_asleep || md_ptr->effect_desc.is_sleep_talk)) {
                // Snore / Sleep Talk: bypass sleep, proceed to execute_move.
                return false;
            }
            message((user_is_player ? std::string("Player") : std::string("Opponent"))
                    + " is fast asleep!");
            return true;  // CantMove
        }
    }

    // ── Freeze ────────────────────────────────────────────────────────────────
    // P1-Freeze: Frozen mon cannot act UNLESS it uses Flame Wheel or Sacred Fire
    // (they self-thaw by being allowed to proceed to execute_move).
    // Source: effect_commands.asm lines ~199-221 / ~426-448.
    if (user.status == Status::Freeze) {
        const MoveId mid = (move_slot < 4) ? user.moves[move_slot].move : MOVE_NONE;
        const MoveData* md_ptr = registries_.moves.get(mid);
        if (md_ptr && md_ptr->effect_desc.secondary_effect == SecondaryEffectType::Defrost) {
            // Flame Wheel / Sacred Fire — let through; the Defrost secondary will clear freeze.
            // Clear freeze here exactly as Crystal does (the check is bypassed entirely).
            user.status = Status::None;
            user.freeze_guard = false;
            return false;
        }
        message((user_is_player ? std::string("Player") : std::string("Opponent"))
                + " is frozen solid!");
        return true;  // CantMove
    }

    // ── Confusion ─────────────────────────────────────────────────────────────
    // P1-Confusion: Decrement counter. If counter reaches 0 = confusion ends (mon acts).
    // Otherwise: 50% self-hit (BattleRandom < 129). Self-hit uses power=40, own atk/def/level.
    // Source: effect_commands.asm lines ~242-279 (player) / ~494-556 (enemy).
    if (user.has_volatile(VolatileStatus::Confusion)) {
        if (user.confusion_turns > 0) --user.confusion_turns;
        if (user.confusion_turns == 0) {
            user.clear_volatile(VolatileStatus::Confusion);
            message((user_is_player ? std::string("Player") : std::string("Opponent"))
                    + " snapped out of confusion!");
            // Falls through to .not_confused — mon acts normally.
        } else {
            message((user_is_player ? std::string("Player") : std::string("Opponent"))
                    + " is confused!");
            // 50% self-hit: BattleRandom < 129 (cp 50 percent + 1 = 129).
            if (rng_.next_byte() < 129u) {
                // Self-hit: power=40, user's own attack vs own defense, user's level.
                // Source: HitSelfInConfusion + BattleCommand_DamageCalc.
                // Type is typeless (no STAB, no type chart); Reflect doubles defense.
                // Formula: ((2*level/5 + 2) * power * atk / def) / 50 + 2, then variation.
                const int32_t level   = static_cast<int32_t>(user.level);
                int32_t atk  = static_cast<int32_t>(user.stats.attack);
                int32_t def  = static_cast<int32_t>(user.stats.defense);
                // Reflect on the user's own side doubles defense for self-hit.
                const bool reflect_up = user_is_player
                    ? (field_.reflect_player > 0)
                    : (field_.reflect_opponent > 0);
                if (reflect_up) def *= 2;
                if (def < 1) def = 1;
                if (atk < 1) atk = 1;
                int32_t dmg = (2 * level / 5 + 2) * 40 * atk / def / 50 + 2;
                // Damage variation: rotation byte, must be >= 0xD9.
                const uint8_t vt = rules_ ? rules_->get_damage_var_lower_bound() : uint8_t{0xD9};
                const int32_t vd = rules_ ? static_cast<int32_t>(rules_->get_damage_var_divisor()) : 255;
                uint8_t var;
                do { uint8_t r = rng_.next_byte(); var = (r >> 1) | (r << 7); } while (var < vt);
                dmg = dmg * static_cast<int32_t>(var) / (vd > 0 ? vd : 255);
                if (dmg < 1) dmg = 1;
                message((user_is_player ? std::string("Player") : std::string("Opponent"))
                        + " hurt itself in its confusion!");
                const int16_t old_hp = user.stats.hp;
                user.stats.hp = static_cast<int16_t>(
                    std::max(0, static_cast<int32_t>(user.stats.hp) - dmg));
                hp_change(user_is_player ? 0u : 1u, old_hp, user.stats.hp);
                return true;  // CantMove after self-hit
            }
            // Didn't self-hit — falls through to .not_confused (mon acts).
        }
    }

    // ── Paralysis ─────────────────────────────────────────────────────────────
    // P1-Paralysis: 25% chance of full immobilization (BattleRandom < 64 = 25 percent).
    // Source: effect_commands.asm lines ~320-332 (player) / ~569-582 (enemy).
    if (user.status == Status::Paralysis) {
        if (rng_.next_byte() < 64u) {  // cp 25 percent = cp 64 = 0x40
            message((user_is_player ? std::string("Player") : std::string("Opponent"))
                    + " is fully paralyzed!");
            return true;  // CantMove
        }
    }

    // ── Disable: if the move in move_slot is the disabled move, block it ──────
    if (user.disable_turns > 0) {
        if (move_slot < 4 && user.moves[move_slot].move == user.disabled_move) {
            message("The move is disabled!");
            --user.disable_turns;
            if (user.disable_turns == 0) user.disabled_move = MOVE_NONE;
            return true;  // block the move
        }
        --user.disable_turns;
        if (user.disable_turns == 0) user.disabled_move = MOVE_NONE;
    }

    // ── Encore: decrement counter ─────────────────────────────────────────────
    if (user.encore_turns > 0) {
        --user.encore_turns;
        if (user.encore_turns == 0) user.encored_move = MOVE_NONE;
    }

    // ── Attract: 50% chance user skips its move ───────────────────────────────
    if (user.has_volatile(VolatileStatus::Infatuation)) {
        message((user_is_player ? std::string("Player") : std::string("Opponent"))
                + " is immobilized by love!");
        if (rng_.next_byte() < 128u) {
            return true;  // block the move
        }
    }

    (void)target;
    return false;
}

void Battle::hook_destiny_bond_check(BattlePokemon& destiny_bond_user, BattlePokemon& killer,
                                      bool killer_is_player) {
    // Crystal CheckFaint path: if destiny_bond_user has DestinyBond and just fainted
    // from a direct-damage move, faint the killer too.
    if (!destiny_bond_user.has_volatile(VolatileStatus::DestinyBond)) return;
    if (!destiny_bond_user.is_fainted()) return;
    // Faint the killer.
    message((killer_is_player ? std::string("Player") : std::string("Opponent"))
            + " was taken down with it!");
    const int16_t old_killer = killer.stats.hp;
    killer.stats.hp = 0;
    if (old_killer > 0) {
        hp_change(killer_is_player ? 0u : 1u, old_killer, 0);
    }
    destiny_bond_user.clear_volatile(VolatileStatus::DestinyBond);
}

bool Battle::hook_end_of_turn_natural_thaw(BattlePokemon& bp, bool is_player) {
    // P1-Freeze: Natural thaw. Source: HandleDefrost in core.asm.
    // Crystal: runs after every turn. Thaw condition: BattleRandom < 10 percent = 25 (0x19).
    // Same-turn-freeze guard (freeze_guard) prevents thaw on the turn freeze was applied.
    // Guard is consumed (cleared) here regardless of whether a thaw roll occurs.
    if (bp.freeze_guard) {
        bp.freeze_guard = false;
        return false;  // guard consumed; no thaw roll this turn
    }
    if (bp.status != Status::Freeze) return false;

    // Thaw if BattleRandom < 25 (~9.8% per turn).
    if (rng_.next_byte() < 25u) {
        bp.status = Status::None;
        message((is_player ? std::string("Player") : std::string("Opponent"))
                + " thawed out!");
        return true;
    }
    return false;
}

} // namespace enginemon
