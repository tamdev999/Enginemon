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
#include "crystal/battle/crystal_effects.hpp"
#include "engine/battle/semantic_effect.hpp"
#include <iostream>
#include <cstdio>
#include <cstring>

namespace crystal {

// Pack a SemanticEffectDescription into the 43-byte wire layout.
// Must match the layout documented in native_package.hpp::MoveDataEntry::effect_desc_raw.
static void pack_effect_desc(const enginemon::SemanticEffectDescription& d,
                              uint8_t (&raw)[PackageWriter::MoveDataEntry::EFFECT_DESC_BYTES])
{
    std::memset(raw, 0, sizeof(raw));
    auto pb = [&](bool v, size_t i) { raw[i] = v ? 1u : 0u; };
    auto pe = [&](auto v, size_t i) { raw[i] = static_cast<uint8_t>(v); };

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
    // [36..42] already zeroed
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

            pack_effect_desc(desc, e.effect_desc_raw);
            if (!desc.is_supported) ++unsupported_count;
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
