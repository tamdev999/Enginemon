#pragma once
// engine/include/engine/battle/semantic_program.hpp
//
// SemanticEffectProgram — Architecture B representation for stateful/multi-turn
// battle effects that cannot be expressed as flat SemanticEffectDescription fields.
//
// Architecture A (SemanticEffectDescription) handles 127 / 157 stock Crystal effects:
//   single-turn, unordered, no persistent cross-turn state.
//
// Architecture B (SemanticEffectProgram) handles the remaining 30 effects:
//   requires ordering, loops, persistent state, phase interception, or party mutation.
//
// The two architectures are parallel compile-time outputs of the Crystal frontend.
// execute_move() dispatches to execute_program() when MoveData::has_program is true.
//
// No Crystal opcodes, effect IDs, or ROM addresses appear in this file.
// All semantics are expressed as engine-native typed operations.

#include "engine/battle/semantic_effect.hpp"  // StatChangeTarget, SecondaryEffectType, etc.
#include <cstdint>
#include <vector>

namespace enginemon {

// Note: This header does NOT include engine/core/types.hpp to avoid a circular
// include (types.hpp includes this file). BOp uses only primitive types directly.
// Callers that need MoveId / ItemId / SpeciesId by name must include types.hpp separately.

// ============================================================================
// Operation kinds — exactly 12 derived from the 30 B effects
// ============================================================================
enum class BOpKind : uint8_t {
    // 1. Standard damage (or per-loop-iteration damage for multi-hit/BeatUp).
    Damage                  = 1,

    // 2. Scale the current power before Damage (Rollout, FuryCutter, TripleKick escalation).
    ScalePower              = 2,

    // 3. Set or clear a VolatileStatus bit on User or Opponent.
    SetVolatile             = 3,

    // 4. Start a persistent counter on the acting combatant.
    InitCounter             = 4,

    // 5. Schedule a delayed payload to fire after N turns (Future Sight).
    ScheduleDelayed         = 5,

    // 6. Capture this turn's incoming damage into storage for Counter/MirrorCoat.
    //    This op is compiled into the program to record intent; the engine hook
    //    OnDamageReceived actually accumulates the value.
    StoreDamage             = 6,

    // 7. Output stored damage × multiplier as the damage value (Counter, Mirror Coat).
    UseStoredDamage         = 7,

    // 8. Execute another move on behalf of the user (MirrorMove, Metronome).
    InvokeMove              = 8,

    // 9. Replace a move slot in the user's moveset (Mimic, Sketch).
    CopyMoveToSlot          = 9,

    // 10. Copy the opponent's full species/stats/moves into the user (Transform).
    TransformInto           = 10,

    // 11. Force opponent to switch to a random live party member (Whirlwind, Roar).
    ForceSwitch             = 11,

    // 12. Transfer the opponent's held item to the user after damage (Thief).
    TransferItem            = 12,
};

// ============================================================================
// DamageSource — what power/formula Damage() uses
// ============================================================================
enum class BDamageSource : uint8_t {
    Standard       = 0,  // Normal DamageCalc pipeline
    EscalatingChain = 1, // Power from rollout_count / fury_cutter_count (see ScalePower)
    StoredEnergy   = 2,  // Bide release: bide_stored_damage × 2
    BeatUpMember   = 3,  // Per-party-member base_attack + level formula
};

// ============================================================================
// CounterKind — which persistent counter to initialise / decrement
// ============================================================================
enum class BCounterKind : uint8_t {
    Bide      = 0,   // 2–3 turns; accumulates damage taken; SUBSTATUS_BIDE
    Rampage   = 1,   // 2–3 turns; locks move; Confusion on expiry; SUBSTATUS_RAMPAGE
    Trapping  = 2,   // 2–5 turns; end-of-turn residual; SUBSTATUS_TRAPPED
    HitLoop   = 3,   // N hits within execute_move (MultiHit, TripleKick, BeatUp)
};

// ============================================================================
// InvokeMoveSource — where InvokeMove gets the move to use
// ============================================================================
enum class BInvokeMoveSource : uint8_t {
    LastOpponentMove = 0,  // MirrorMove
    RandomFromPool   = 1,  // Metronome (excludes MetronomeExcepts list)
};

// ============================================================================
// CopyMoveMode — persistence of CopyMoveToSlot
// ============================================================================
enum class BCopyMoveMode : uint8_t {
    BattleDuration = 0,  // Mimic: reverts on switch-out or battle end
    Permanent      = 1,  // Sketch: writes party Pokémon moveset immediately
};

// ============================================================================
// ScaleChainKind — which escalating-power chain to use
// ============================================================================
enum class BScaleChainKind : uint8_t {
    Rollout    = 0,  // 2^rollout_count, doubled if SUBSTATUS_CURLED; caps at 5
    FuryCutter = 1,  // 2^fury_cutter_count; caps at 5 then resets
    TripleKick = 2,  // iteration_multiplier (1×, 2×, 3×) stored in op.param
};

// ============================================================================
// SetVolatile target
// ============================================================================
enum class BVolatileTarget : uint8_t {
    User     = 0,
    Opponent = 1,
};

// ============================================================================
// BOp — one semantic operation in a SemanticEffectProgram
// ============================================================================
struct BOp {
    BOpKind kind = BOpKind::Damage;

    // Shared parameter word — interpretation depends on kind:
    //   Damage:          BDamageSource (cast to uint8_t) in param8a
    //   ScalePower:      BScaleChainKind in param8a; iteration_multiplier in param8b
    //   SetVolatile:     VolatileStatus bit pattern in param32 (low 16 bits);
    //                    BVolatileTarget in param8a; bool value in param8b (1=set, 0=clear)
    //   InitCounter:     BCounterKind in param8a; rand_min in param8b; rand_max in param8c
    //                    (if rand_min==rand_max, count is fixed)
    //   ScheduleDelayed: turns in param8a (always 3 for FutureSight)
    //   StoreDamage:     MoveCategory filter in param8a (Physical=0, Special=1, Both=2)
    //   UseStoredDamage: multiplier in param8a (always 2)
    //   InvokeMove:      BInvokeMoveSource in param8a
    //   CopyMoveToSlot:  BCopyMoveMode in param8a
    //                    0=FindMimic slot, 1=FindSketch slot in param8b
    //   TransformInto:   (no parameters needed)
    //   ForceSwitch:     (no parameters needed)
    //   TransferItem:    (no parameters needed)

    uint8_t  param8a  = 0;
    uint8_t  param8b  = 0;
    uint8_t  param8c  = 0;
    uint8_t  param8d  = 0;
    uint32_t param32  = 0;

    // ── Static constructors for each op kind ─────────────────────────────────

    static BOp Damage(BDamageSource src = BDamageSource::Standard) {
        BOp op; op.kind = BOpKind::Damage; op.param8a = static_cast<uint8_t>(src); return op;
    }

    static BOp ScalePower(BScaleChainKind chain, uint8_t iteration_multiplier = 0) {
        BOp op; op.kind = BOpKind::ScalePower;
        op.param8a = static_cast<uint8_t>(chain);
        op.param8b = iteration_multiplier;
        return op;
    }

    static BOp SetVolatileBit(uint32_t vs_bits, BVolatileTarget target, bool value) {
        BOp op; op.kind = BOpKind::SetVolatile;
        op.param32 = vs_bits;
        op.param8a = static_cast<uint8_t>(target);
        op.param8b = value ? 1u : 0u;
        return op;
    }

    static BOp InitCounter(BCounterKind ck, uint8_t rand_min, uint8_t rand_max) {
        BOp op; op.kind = BOpKind::InitCounter;
        op.param8a = static_cast<uint8_t>(ck);
        op.param8b = rand_min;
        op.param8c = rand_max;
        return op;
    }

    static BOp ScheduleDelayed(uint8_t turns) {
        BOp op; op.kind = BOpKind::ScheduleDelayed; op.param8a = turns; return op;
    }

    // StoreDamage: filter = 0 Physical, 1 Special
    static BOp StoreDamage(uint8_t category_filter) {
        BOp op; op.kind = BOpKind::StoreDamage; op.param8a = category_filter; return op;
    }

    static BOp UseStoredDamage(uint8_t multiplier = 2) {
        BOp op; op.kind = BOpKind::UseStoredDamage; op.param8a = multiplier; return op;
    }

    static BOp InvokeMove(BInvokeMoveSource src) {
        BOp op; op.kind = BOpKind::InvokeMove; op.param8a = static_cast<uint8_t>(src); return op;
    }

    // slot_kind: 0=FindMimic, 1=FindSketch
    static BOp CopyMoveToSlot(BCopyMoveMode mode, uint8_t slot_kind = 0) {
        BOp op; op.kind = BOpKind::CopyMoveToSlot;
        op.param8a = static_cast<uint8_t>(mode);
        op.param8b = slot_kind;
        return op;
    }

    static BOp TransformInto() {
        BOp op; op.kind = BOpKind::TransformInto; return op;
    }

    static BOp ForceSwitch() {
        BOp op; op.kind = BOpKind::ForceSwitch; return op;
    }

    static BOp TransferItem() {
        BOp op; op.kind = BOpKind::TransferItem; return op;
    }
};

// ============================================================================
// SemanticEffectProgram — the Architecture B representation of one move's effect.
//
// Produced at compile time by the Crystal frontend (EffectProgramCompiler).
// Stored alongside SemanticEffectDescription in MoveData.
// Consumed by execute_program() at runtime.
//
// A move uses Architecture B when MoveData::has_program == true.
// Architecture A (SemanticEffectDescription::is_supported) is still populated
// for the AI classification fields and the architecture-A path metadata.
// ============================================================================
struct SemanticEffectProgram {
    // Ordered list of semantic operations to execute.
    std::vector<BOp> ops;

    // AI classification (same field as SemanticEffectDescription::ai_classification).
    uint8_t ai_classification = 0;

    // Whether this program was successfully compiled (analogous to is_supported in A).
    bool is_compiled = false;

    bool empty() const { return ops.empty(); }
};

// ============================================================================
// MetronomeExcepts — ROM-materialized list of moves Metronome cannot select.
// Source: pokecrystal/data/moves/metronome_exception_moves.asm
// Stored in BattleRules after extraction.
// ============================================================================
// (The list is added to BattleRules in battle_rules.hpp.)

} // namespace enginemon
