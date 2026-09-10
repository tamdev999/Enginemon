// frontends/crystal/compile/move_semanticizer.cpp
//
// Move semanticization step: Crystal ROM → SemanticEffectDescription per move.
//
// Kept in its own TU to avoid blowing MSVC's C1060 heap limit in
// full_compiler.cpp.  The effect-script pipeline headers are heavy
// (std::variant chains via DecodedEffectScript + CrystalCommand IR);
// isolating them here keeps each TU compilable within MSVC constraints.
//
// The SemanticEffectDescription is packed into MoveDataEntry::effect_desc_raw
// (a fixed 43-byte array) using the same layout as the MVDT wire format, so
// native_package.cpp can emit it directly without re-including semantic_effect.hpp.

#include "crystal/compile/move_semanticizer.hpp"
#include "crystal/extract/effect_script_decoder.hpp"
#include "crystal/battle/effect_semanticizer.hpp"
#include "crystal/battle/effect_program_compiler.hpp"
#include "crystal/battle/crystal_effects.hpp"
#include "engine/battle/semantic_effect.hpp"
#include <iostream>
#include <cstdio>
#include <cstring>

namespace crystal {

// Pack a SemanticEffectDescription into the 64-byte wire layout (MVDT schema v4).
// Must match the layout documented in native_package.hpp::MoveDataEntry::effect_desc_raw.
static void pack_effect_desc(const enginemon::SemanticEffectDescription& d,
                              uint8_t (&raw)[PackageWriter::MoveDataEntry::EFFECT_DESC_BYTES])
{
    std::memset(raw, 0, sizeof(raw));
    auto pb = [&](bool v, size_t i) { raw[i] = v ? 1u : 0u; };
    auto pe = [&](auto v, size_t i) { raw[i] = static_cast<uint8_t>(v); };

    // [0..35] — original fields (layout unchanged from v3)
    pb(d.has_standard_damage,   0);
    pb(d.has_recoil,             1);
    pb(d.has_drain,              2);
    pb(d.drain_requires_sleep,   3);
    pb(d.user_faints,            4);
    pb(d.is_ohko,               5);
    pb(d.cannot_ko,             6);
    pb(d.sets_recharge,         7);
    pe(d.constant_damage_source, 8);
    pe(d.set_power_source,       9);
    pe(d.conditional_double,    10);
    pe(d.secondary_effect,      11);
    pe(d.primary_status,        12);
    pe(d.stat_change,           13);
    pe(d.heal_source,           14);
    pe(d.set_screen,            15);
    pe(d.set_weather,           16);
    pb(d.sets_spikes,           17);
    pb(d.is_multi_hit,          18);
    pb(d.is_charge,             19);
    pb(d.is_future_sight,       20);
    pb(d.is_rampage,            21);
    pb(d.is_escalating_power,   22);
    pb(d.is_trapping,           23);
    pb(d.is_counter,            24);
    pb(d.is_mirror_coat,        25);
    pb(d.is_bide,               26);
    pb(d.is_pursuit,            27);
    pb(d.is_copy_move,          28);
    pb(d.clears_hazards,        29);
    pb(d.is_sleep_move,         30);
    pb(d.needs_kingsrock,       31);
    pb(d.needs_substitute,      32);
    pb(d.needs_rage,            33);
    raw[34] = d.ai_classification;
    pb(d.is_supported,          35);

    // [36..63] — new fields added in MVDT schema v4
    pb(d.has_payday,            36);
    pb(d.sets_focus_energy,     37);
    pb(d.sets_mist,             38);
    pb(d.sets_safeguard,        39);
    pb(d.changes_user_type,     40);
    pb(d.changes_user_type_resist, 41);
    pb(d.equalizes_hp,          42);
    pb(d.requires_user_asleep,  43);
    pb(d.traps_opponent,        44);
    pb(d.identifies_opponent,   45);
    pb(d.reduces_pp,            46);
    pb(d.has_thunder_accuracy,  47);
    pb(d.ends_wild_battle,      48);
    pb(d.swagger_stat_change,   49);
    pb(d.is_splash,             50);
    pb(d.is_leech_seed,         51);
    pb(d.is_disable,            52);
    pb(d.is_encore,             53);
    pb(d.is_lock_on,            54);
    pb(d.is_sleep_talk,         55);
    pb(d.is_destiny_bond,       56);
    pb(d.is_nightmare,          57);
    pb(d.is_curse,              58);
    pb(d.is_protect,            59);
    pb(d.is_perish_song,        60);
    pb(d.is_attract,            61);
    pb(d.is_baton_pass,         62);
    // byte [63] is a bitfield: [0]=is_heal_bell, [1]=is_endure, [2]=is_rage,
    //   [3]=has_effectchance_phase, [4]=crash_on_miss, [5]=halves_in_rain, [6]=sets_minimize
    raw[63] = static_cast<uint8_t>(
        (d.is_heal_bell           ? 0x01u : 0u) |
        (d.is_endure              ? 0x02u : 0u) |
        (d.is_rage                ? 0x04u : 0u) |
        (d.has_effectchance_phase ? 0x08u : 0u) |
        (d.crash_on_miss          ? 0x10u : 0u) |
        (d.halves_in_rain         ? 0x20u : 0u) |
        (d.sets_minimize          ? 0x40u : 0u) |
        (d.is_fake_out            ? 0x80u : 0u));
}

bool semanticize_move_entries(
    const RomData&                            rom,
    const ExtractionProfile&                  profile,
    std::vector<PackageWriter::MoveDataEntry>& entries)
{
    // Resolve effect script table addresses from ROM using the profile.
    auto tables = EffectScriptDecoder::resolve_tables(rom, profile);
    if (!tables.ok()) {
        std::cerr << "FATAL: Stage 9 semanticize — effect script table resolution failed: "
                  << tables.error << "\n";
        return false;
    }

    // Decode all effect scripts.  Fail closed on any bounds or validation error.
    EffectScriptDecodeError decode_err;
    auto corpus = EffectScriptDecoder::decode_all(rom, profile, tables, &decode_err);
    if (!corpus) {
        std::cerr << "FATAL: Stage 9 semanticize — effect script decode failed: "
                  << decode_err.message << "\n";
        return false;
    }

    std::cout << "  EffectScriptCorpus: " << corpus->size() << " scripts decoded\n";

    // Populate effect_desc_raw for each move entry.
    uint32_t unsupported_count = 0;
    for (auto& e : entries) {
        const DecodedEffectScript* script = corpus->get(e.raw_crystal_effect);
        if (script) {
            auto desc = EffectSemanticizer::semanticize(*script, e.effect_id);

            // ── Constant-damage source refinement ─────────────────────────────
            // semanticize() cannot distinguish between the four Crystal effects that
            // all share the same constantdamage-only script shape, since all map to
            // SemEffect::Unknown.  We use the raw Crystal effect byte to refine here
            // — this is the only place Crystal raw IDs cross into the package pipeline,
            // and they stay strictly compiler-side (never reach the runtime).
            //
            // Source authority: suiCune/data/moves/effects_pointers.asm — effect indices.
            // CrystalEffectId constants defined in crystal/battle/crystal_effects.hpp.
            if (desc.constant_damage_source == enginemon::ConstantDamageSource::MoveFixed) {
                switch (e.raw_crystal_effect) {
                    case crystal::EffectId::SUPER_FANG:
                        // floor(target_hp / 2), minimum 1
                        desc.constant_damage_source = enginemon::ConstantDamageSource::HalfTargetHP;
                        break;
                    case crystal::EffectId::LEVEL_DAMAGE:
                        // user.level (Seismic Toss, Night Shade)
                        desc.constant_damage_source = enginemon::ConstantDamageSource::UserLevel;
                        break;
                    case crystal::EffectId::PSYWAVE:
                        // random [1 .. floor(user_level × 1.5) − 1]
                        desc.constant_damage_source = enginemon::ConstantDamageSource::Psywave;
                        break;
                    case crystal::EffectId::STATIC_DAMAGE:
                        // move_power byte (Dragon Rage = 40) — MoveFixed is correct
                        break;
                    default:
                        // Any other effect that somehow produced MoveFixed via 0x3F keeps it.
                        break;
                }
            }

            // ── Jump Kick / Hi Jump Kick crash damage on miss ────────────────
            // Source: pokecrystal GetFailureResultText EFFECT_JUMP_KICK path:
            // when accuracy misses and effect == JUMP_KICK, wCurDamage is preserved
            // and crash = wCurDamage >> 3 (min 1) deals to user.
            // This must be set based on raw_crystal_effect (not script opcode).
            if (e.raw_crystal_effect == crystal::EffectId::JUMP_KICK) {
                desc.crash_on_miss = true;
            }

            // ── SolarBeam rain penalty ────────────────────────────────────────
            // Source: suiCune DoWeatherModifiers WeatherMoveModifiers table:
            //   {weather=WEATHER_RAIN, effect=EFFECT_SOLARBEAM, multiplier=5 (×0.5)}
            // Crystal halves SolarBeam damage in Rain. The raw-effect-ID lookup in
            // apply_weather_modifier() is dead because runtime uses SemEffect IDs.
            // Encode this as a semantic bool so runtime can check it without any
            // Crystal raw effect-ID knowledge.
            if (e.raw_crystal_effect == crystal::EffectId::SOLARBEAM) {
                desc.halves_in_rain = true;
            }

            // ── Minimize — set Minimized volatile ────────────────────────────
            // Source: suiCune MinimizeDropSub — called from BattleCommand_StatUp
            // after evasionup raises the stage. Sets wPlayerMinimized=1 ONLY when
            // BATTLE_VARS_MOVE_ANIM == MINIMIZE. Double Team (same EFFECT_EVASION_UP,
            // different move ID) has a different animation byte so the flag is not set.
            // We encode this via move ID at semanticize time (compiler side only;
            // no raw effect-ID or move-ID reaches the runtime).
            // Minimize = move ID 107 (0x6b) in vanilla Crystal.
            if (e.id == static_cast<uint16_t>(107u)) {
                desc.sets_minimize = true;
            }

            // ── Hard fail: unrecognized opcode in script ───────────────────────
            // Every stock command byte must have an explicit case in apply_command().
            // If unrecognized_opcode is set, the script contains a byte with no
            // implemented semantic — this is a compiler bug, not a runtime fallback.
            if (desc.unrecognized_opcode) {
                std::fprintf(stderr,
                    "FATAL: Stage 9 [move %u, effect 0x%02X]: "
                    "script contains unrecognized command byte 0x%02X — "
                    "no semantic mapping. Add a case in effect_semanticizer.cpp.\n",
                    static_cast<unsigned>(e.id), e.raw_crystal_effect,
                    desc.first_unrecognized);
                return false;
            }

            pack_effect_desc(desc, e.effect_desc_raw);
            if (!desc.is_supported) ++unsupported_count;

            // ── Architecture B: compile SemanticEffectProgram if needed ───────
            // After the A-gate packs the description, check whether this effect
            // needs an Architecture B program.  The compiler is structural: it
            // never uses raw effect IDs to decide — it reads the description flags.
            if (EffectProgramCompiler::needs_program(desc)) {
                auto prog_result = EffectProgramCompiler::compile(
                    desc, *script, e.raw_crystal_effect, e.effect_id);
                if (prog_result.success) {
                    e.has_program    = true;
                    e.effect_program = std::move(prog_result.program);
                    // B program compiled — is_supported stays false (already set by gate).
                } else {
                    // B compile failure is a compiler bug — no fallback.
                    // The semanticizer produced B flags for a script that the program
                    // compiler cannot handle. Fix the compiler, not the classification.
                    std::fprintf(stderr,
                        "FATAL: Stage 9 [move %u, effect 0x%02X]: "
                        "EffectProgramCompiler failed: %s\n",
                        static_cast<unsigned>(e.id), e.raw_crystal_effect,
                        prog_result.error.c_str());
                    return false;
                }
            }
        } else {
            // Effect ID out of range for this ROM (e.g., hack-specific move).
            // Leave effect_desc_raw zeroed (is_supported=false) — fails closed.
            if (e.raw_crystal_effect != 0) {
                // raw_crystal_effect=0 is NORMAL_HIT — silently unsupported is fine.
                std::fprintf(stderr,
                    "Stage 9 semanticize [move %u]: raw_crystal_effect 0x%02X out of corpus range "
                    "(%zu scripts) — SemanticEffectDescription left as unsupported\n",
                    static_cast<unsigned>(e.id), e.raw_crystal_effect, corpus->size());
            }
            ++unsupported_count;
        }
    }

    std::cout << "  SemanticEffectDescription: "
              << (entries.size() - unsupported_count) << "/" << entries.size()
              << " moves supported\n";
    return true;
}

} // namespace crystal
