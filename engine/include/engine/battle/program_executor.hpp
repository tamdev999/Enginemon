#pragma once
// engine/include/engine/battle/program_executor.hpp
//
// execute_program() — Architecture B program executor.
//
// Called from Battle::execute_move() when MoveData::has_program == true.
// Dispatches the ordered BOp sequence from a SemanticEffectProgram, using
// the same internal Battle helpers (damage, HP change, volatile status, etc.)
// that Architecture A uses.
//
// No Crystal opcodes, effect IDs, or ROM addresses are present here.
// All semantics come from the typed BOp values compiled from the Crystal frontend.
//
// This header is engine-internal; the Crystal frontend never includes it.
