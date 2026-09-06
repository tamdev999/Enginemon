#pragma once
// frontends/crystal/include/crystal/battle/effect_semanticizer.hpp
//
// EffectSemanticizer — translates a DecodedEffectScript (raw 1-byte command
// stream) into a SemanticEffectDescription (typed engine-facing struct).
//
// INPUT:  DecodedEffectScript — decoded by EffectScriptDecoder
// OUTPUT: SemanticEffectDescription — consumed by execute_move()
//
// DESIGN RULES
//   - No Crystal opcode numbers appear in the output struct or the engine.
//   - The command opcode ↔ semantic meaning table lives entirely here.
//   - The support gate is computed from the description fields, not from
//     a hand-maintained whitelist of Crystal effect IDs.
//   - to_semantic_effect() is preserved for the AI classification tables
//     extracted in BattleRulesExtractor; it is NOT used for execution dispatch.

#include "crystal/extract/effect_script_decoder.hpp"
#include "engine/battle/semantic_effect.hpp"

namespace crystal {

// ============================================================================
// EffectSemanticizer
// ============================================================================
class EffectSemanticizer {
public:
    // Translate one decoded effect script into a SemanticEffectDescription.
    //
    // capability_flags encodes which runtime capabilities currently exist.
    // Pass CapabilityFlags::current() for production compilation.
    //
    // ai_classification is the SemEffect:: value to store for AI classification;
    // pass the result of to_semantic_effect(crystal_raw_effect_id).
    static enginemon::SemanticEffectDescription semanticize(
        const DecodedEffectScript& script,
        uint8_t ai_classification);

    // Recompute is_supported from the description fields and the current
    // set of implemented capabilities.  Called after semanticize().
    static void apply_support_gate(enginemon::SemanticEffectDescription& desc);

    // Command opcode → semantic role.
    // Returns true if the opcode is in the presentation/bookkeeping category
    // (checkobedience, usedmovetext, moveanim, text commands, etc.) whose
    // absence or presence does not change the move's battle semantics.
    // NOTE: kingsrock, lowersub, raisesub, buildopponentrage are NOT presentation
    // — they are deferred capabilities.
    static bool is_presentation(uint8_t opcode);

private:
    // Internal: process one command byte and update the description.
    static void apply_command(uint8_t opcode, enginemon::SemanticEffectDescription& desc);
};

} // namespace crystal
