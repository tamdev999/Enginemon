#pragma once
// engine/scripting/script_context.hpp
// Script execution context — typed semantic state that persists across script
// operations and block boundaries.
//
// INTENTIONALLY LEAN: this header includes ONLY engine/core/types.hpp and
// <optional>.  It does NOT include semantic_ir.hpp, which defines the
// 151-alternative SemanticOp variant.
//
// Purpose of the split:
//   lua_runtime.hpp stores ScriptExecutionContext by value, so it previously
//   had to include the full semantic_ir.hpp.  That pulled the expensive
//   SemanticOp variant into every TU that included lua_runtime.hpp (e.g.
//   emitter_test.cpp, api_bindings.hpp users).  By extracting just the context
//   types here, lua_runtime.hpp can include script_context.hpp and the variant
//   machinery stays isolated to TUs that actually manipulate SemanticOp values.
//
// Consumers:
//   lua_runtime.hpp    — includes this instead of semantic_ir.hpp
//   semantic_ir.hpp    — includes this so Sem_* types can reference the enums/structs
//   headless_runtime.hpp (transitively via lua_runtime)
//   game_loop.hpp      (transitively via lua_runtime)

#include "engine/core/types.hpp"
#include <optional>
#include <cstdint>

namespace enginemon {

// =============================================================================
// Party slot identifier
// =============================================================================

using PartySlotId = uint8_t;
constexpr PartySlotId PARTY_SLOT_NONE = 0xFF;

// =============================================================================
// Field move types and capability results
// =============================================================================

// Field move types for capability checks
enum class FieldMoveType : uint8_t {
    Strength,   // Pushing boulders
    RockSmash,  // Breaking rocks (may trigger encounter)
    // Note: Cut, Surf, Fly, Flash, Waterfall, Whirlpool not in corpus
};

// Strength capability check result
enum class StrengthCapabilityResult : uint8_t {
    Available,      // Party has move AND badge, not yet active
    Unavailable,    // No move OR no badge
    AlreadyActive,  // Strength already activated this session
};

// Rock Smash capability check result
enum class RockSmashCapabilityResult : uint8_t {
    Available,      // Party has Rock Smash
    Unavailable,    // No Rock Smash in party
};

// =============================================================================
// SelectedFieldActor — context established by field-move capability checks
// =============================================================================
// ESTABLISHED by: Sem_CheckStrengthCapability, Sem_CheckRockSmashCapability
// CONSUMED by:    Sem_PrepareFieldMoveNickname, Sem_ActivateStrength,
//                 Sem_PlayFieldActorCry
// CLEARED by:     ScriptExecutionContext::clear()
struct SelectedFieldActor {
    PartySlotId slot;       // Which party member has the move (0–5)
    FieldMoveType move;     // Which field move was checked
    SpeciesId species;      // Species for cry/nickname lookup

    SelectedFieldActor(PartySlotId s, FieldMoveType m, SpeciesId sp)
        : slot(s), move(m), species(sp) {}
};

// =============================================================================
// PendingFieldEncounter — context established by field-triggered encounters
// =============================================================================
// ESTABLISHED by: Sem_TryRockSmashEncounter
// CONSUMED by:    Sem_LoadPendingEncounter, Sem_ReadEncounterSpecies
// CLEARED by:     ScriptExecutionContext::clear(), Sem_LoadPendingEncounter
struct PendingFieldEncounter {
    SpeciesId species;  // Species to battle
    uint8_t level;      // Level of wild Pokémon

    PendingFieldEncounter(SpeciesId sp, uint8_t lv)
        : species(sp), level(lv) {}
};

// =============================================================================
// ScriptExecutionContext — full typed context for script execution
// =============================================================================
// Replaces Crystal's scattered GB RAM state with explicit typed fields.
// Owned by LuaRuntime; persists within a script invocation.
//
// Rules:
//  - No arbitrary key/value scratch storage
//  - No generic temporary integer slots
//  - No raw RAM-like state
//  - Every field has explicit semantic meaning
//  - Absence → std::nullopt, not sentinel values
struct ScriptExecutionContext {
    std::optional<SelectedFieldActor>    selected_field_actor;
    std::optional<PendingFieldEncounter> pending_field_encounter;

    // Primary script result variable (wScriptVar equivalent)
    int16_t script_var = 0;

    // Strength active for current map session (survives script termination)
    bool strength_active = false;

    // Clear transient context (called on script termination).
    // strength_active is session-level — NOT cleared here.
    void clear() {
        selected_field_actor    = std::nullopt;
        pending_field_encounter = std::nullopt;
        script_var              = 0;
    }

    bool has_field_context() const {
        return selected_field_actor.has_value()
            || pending_field_encounter.has_value();
    }
};

} // namespace enginemon
