// tests/crystal/legality_test_helpers.hpp
//
// Umbrella helper header — includes both narrow helper sets.
//
// Prefer the narrow headers when a TU only needs one stage:
//   test_crystal_ir_helpers.hpp  — make_minimal_ir, make_minimal_cfg
//   test_semantic_helpers.hpp    — make_minimal_lowering, make_minimal_input
//
// This umbrella is the right choice when a TU genuinely exercises the full
// Stage 1→5 pipeline (e.g. legality_gate_test.cpp).
//
// Both helpers require both CrystalCommandData (171-alt) and SemanticOp
// (154-alt) to be instantiated.  Do not include from TUs that only need
// one stage — use the narrow headers instead.
#pragma once
#include "crystal/test_crystal_ir_helpers.hpp"
#include "crystal/test_semantic_helpers.hpp"
