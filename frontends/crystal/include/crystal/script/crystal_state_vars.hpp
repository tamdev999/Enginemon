#pragma once
// crystal/script/crystal_state_vars.hpp
// Crystal-specific semantic state variable IDs.
//
// These are Crystal game-specific gameplay states that require their own
// StateVarId values distinct from the generic engine WellKnownStateVar.
// They must NOT be referenced by generic engine code.
//
// IDs 1–3 are used by the generic WellKnownStateVar enum.
// IDs 4+ are Crystal-exclusive assignments.

#include "engine/core/types.hpp"

namespace crystal {

// Crystal-specific well-known state variable IDs.
// Numeric values agree with the compiled package's StateVar domain.
// They are consumed by SemanticLegalizer (readmem/callasm rules that
// lower to Sem_ReadStateVar) and by RamAddressRegistry.
enum class CrystalStateVar : uint16_t {
    // 1–3 are reserved by WellKnownStateVar (game-agnostic)

    // Battle Tower streak counter.
    // Source: wNrOfBeatenBattleTowerTrainers (00:cf64)
    // Set by battle results; read by BattleTower deferred script to check
    // whether all 7 trainers in the current run have been beaten.
    BattleTowerBeatenTrainers = 4,

    // Battle Tower level-group selection (1–10 maps to L10/L20/.../L100).
    // Source: wBTChoiceOfLvlGroup in SRAM (read by callasm 0x9f5cb).
    BattleTowerLevelGroup = 5,
};

// Convenience cast: CrystalStateVar → StateVarId (the underlying uint16_t)
inline constexpr enginemon::StateVarId crystal_state_var_id(CrystalStateVar v) noexcept {
    return static_cast<enginemon::StateVarId>(static_cast<uint16_t>(v));
}

} // namespace crystal
