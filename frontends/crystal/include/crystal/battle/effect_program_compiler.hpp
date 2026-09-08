#pragma once
// frontends/crystal/include/crystal/battle/effect_program_compiler.hpp
//
// EffectProgramCompiler — Crystal frontend compiler for Architecture B effects.
//
// Converts a DecodedEffectScript into a SemanticEffectProgram for the 30
// stateful/multi-turn effects that cannot be expressed as SemanticEffectDescription.
//
// Called from move_semanticizer.cpp after EffectSemanticizer::semanticize()
// has set the deferred-mechanic flags on the SemanticEffectDescription.
//
// A move uses Architecture B when ANY of these flags are set in the description:
//   is_multi_hit, is_charge, is_future_sight, is_rampage, is_escalating_power,
//   is_trapping, is_counter, is_mirror_coat, is_bide, is_pursuit, is_copy_move
//
// Or when set_power_source is HappinessReturn/Frustration/HiddenPower — these
// are actually Architecture A (flat SemanticEffectDescription) now that the data
// plumbing is wired, but they are verified here for completeness.
//
// No Crystal effect IDs or opcode bytes appear in the compiled output.

#include "crystal/extract/effect_script_decoder.hpp"
#include "engine/battle/semantic_effect.hpp"
#include "engine/battle/semantic_program.hpp"
#include <string>

namespace crystal {

// Result of compiling one effect script to Architecture B.
struct EffectProgramResult {
    bool success = false;
    std::string error;
    enginemon::SemanticEffectProgram program;
};

class EffectProgramCompiler {
public:
    // Compile a decoded Crystal effect script into a SemanticEffectProgram.
    //
    // desc:       the SemanticEffectDescription already produced by EffectSemanticizer.
    //             Used to read the deferred-mechanic flags and secondary effects.
    // script:     the decoded byte sequence for this effect.
    // raw_effect: the Crystal raw effect index (used for disambiguation where
    //             multiple effects share the same script body, e.g. SuperFang/Psywave).
    // ai_class:   the SemEffect:: value for AI classification (carried through).
    //
    // Crystal knowledge (raw_effect, opcode bytes) stays here. The resulting
    // SemanticEffectProgram contains only engine-semantic BOp values.
    static EffectProgramResult compile(
        const enginemon::SemanticEffectDescription& desc,
        const DecodedEffectScript& script,
        uint8_t raw_effect,
        uint8_t ai_class);

    // True if the given SemanticEffectDescription indicates an Architecture B effect.
    static bool needs_program(const enginemon::SemanticEffectDescription& desc);
};

} // namespace crystal
