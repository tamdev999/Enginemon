// crystal/script/legality_gate.cpp
// Stage 5: Hard per-script legality gate implementation

#include "crystal/script/legality_gate.hpp"
#include <sstream>
#include <iomanip>
#include <iostream>

namespace crystal {

// =============================================================================
// ILLEGAL SCRIPT
// =============================================================================

std::string IllegalScript::summary() const {
    std::ostringstream ss;
    ss << "Script " << script_id << " failed at " << first_failure_stage
       << " (" << legality_failure_kind_name(first_failure_kind) << ")";
    if (!diagnostics.empty()) {
        ss << ": " << diagnostics[0].reason;
    }
    return ss.str();
}

// =============================================================================
// LEGALITY RESULT
// =============================================================================

LegalityResult LegalityResult::make_legal(LegalScript script) {
    LegalityResult r;
    r.is_legal = true;
    r.legal = std::move(script);
    return r;
}

LegalityResult LegalityResult::make_illegal(IllegalScript script) {
    LegalityResult r;
    r.is_legal = false;
    r.illegal = std::move(script);
    return r;
}

const std::string& LegalityResult::script_id() const {
    static const std::string empty;
    if (is_legal && legal) return legal->script_id;
    if (!is_legal && illegal) return illegal->script_id;
    return empty;
}

const std::vector<LegalityDiagnostic>& LegalityResult::diagnostics() const {
    static const std::vector<LegalityDiagnostic> empty;
    if (!is_legal && illegal) return illegal->diagnostics;
    return empty;
}

// =============================================================================
// LEGALITY GATE IMPLEMENTATION
// =============================================================================

LegalityDiagnostic LegalityGate::make_diagnostic(
    LegalityFailureKind kind,
    const std::string& script_id,
    const std::string& stage,
    const std::string& offending,
    const std::string& reason) {
    
    LegalityDiagnostic d;
    d.kind = kind;
    d.script_id = script_id;
    d.failing_stage = stage;
    d.offending_element = offending;
    d.reason = reason;
    return d;
}

LegalityResult LegalityGate::validate(const LegalityInput& input) {
    std::vector<LegalityDiagnostic> all_diagnostics;
    std::string script_id;
    
    // Get script ID from available sources
    if (input.cfg) {
        script_id = input.cfg->script_name;
    } else if (input.lowering) {
        script_id = input.lowering->ir.script_id;
    } else if (input.ir && !input.ir->commands.empty()) {
        std::ostringstream ss;
        ss << "script_0x" << std::hex << input.ir->commands[0].span.rom_address;
        script_id = ss.str();
    } else {
        script_id = "unknown_script";
    }
    
    // Run all stage checks
    auto stage1 = check_stage1(input);
    auto stage2 = check_stage2(input);
    auto stage3 = check_stage3(input);
    auto stage4 = check_stage4(input);
    auto stage5 = check_stage5_ir(input);
    
    // Collect all diagnostics
    all_diagnostics.insert(all_diagnostics.end(), stage1.begin(), stage1.end());
    all_diagnostics.insert(all_diagnostics.end(), stage2.begin(), stage2.end());
    all_diagnostics.insert(all_diagnostics.end(), stage3.begin(), stage3.end());
    all_diagnostics.insert(all_diagnostics.end(), stage4.begin(), stage4.end());
    all_diagnostics.insert(all_diagnostics.end(), stage5.begin(), stage5.end());
    
    // If any failures, return IllegalScript
    if (!all_diagnostics.empty()) {
        IllegalScript illegal;
        illegal.script_id = script_id;
        illegal.diagnostics = std::move(all_diagnostics);
        illegal.first_failure_stage = illegal.diagnostics[0].failing_stage;
        illegal.first_failure_kind = illegal.diagnostics[0].kind;
        return LegalityResult::make_illegal(std::move(illegal));
    }
    
    // All checks passed - return LegalScript
    LegalScript legal;
    legal.script_id = script_id;
    
    if (input.lowering) {
        legal.ir = input.lowering->ir;
        legal.commands_lowered = input.lowering->commands_lowered;
        legal.commands_absorbed = input.lowering->commands_absorbed;
    }
    
    if (input.cfg) {
        legal.blocks = input.cfg->blocks.size();
    }
    
    legal.instructions = legal.ir.total_instructions();
    
    return LegalityResult::make_legal(std::move(legal));
}

std::vector<LegalityDiagnostic> LegalityGate::check_stage1(const LegalityInput& input) {
    std::vector<LegalityDiagnostic> diagnostics;
    
    std::string script_id = input.cfg ? input.cfg->script_name : "unknown";
    std::string provenance;
    if (input.ir && !input.ir->commands.empty()) {
        std::ostringstream ss;
        ss << "0x" << std::hex << input.ir->commands[0].span.rom_address;
        provenance = ss.str();
    }
    
    // Check decode completeness
    if (!input.decode_complete) {
        auto d = make_diagnostic(
            LegalityFailureKind::DecodeIncomplete,
            script_id, "Stage1", provenance,
            "Script decoding did not complete successfully");
        diagnostics.push_back(std::move(d));
    }
    
    // Check round-trip integrity
    if (input.round_trip_failures > 0) {
        auto d = make_diagnostic(
            LegalityFailureKind::RoundTripFailure,
            script_id, "Stage1", 
            std::to_string(input.round_trip_failures) + " commands",
            "Commands do not round-trip to original bytes");
        diagnostics.push_back(std::move(d));
    }
    
    // Check for unknown opcodes
    if (input.unknown_opcodes > 0) {
        auto d = make_diagnostic(
            LegalityFailureKind::UnknownOpcode,
            script_id, "Stage1",
            std::to_string(input.unknown_opcodes) + " commands",
            "Opcodes outside valid 0x00-0xA9 range");
        diagnostics.push_back(std::move(d));
    }
    
    // Check IR for Cmd_Unknown variants
    if (input.ir) {
        for (const auto& cmd : input.ir->commands) {
            if (std::holds_alternative<Cmd_Unknown>(cmd.data)) {
                auto d = make_diagnostic(
                    LegalityFailureKind::UnknownOpcode,
                    script_id, "Stage1",
                    "opcode 0x" + ([&]() {
                        std::ostringstream ss;
                        ss << std::hex << static_cast<int>(cmd.opcode());
                        return ss.str();
                    })(),
                    "Unknown opcode in decoded IR");
                d.opcode = cmd.opcode();
                d.address = cmd.span.rom_address;
                diagnostics.push_back(std::move(d));
                break;  // One diagnostic is enough
            }
        }
    }
    
    return diagnostics;
}

std::vector<LegalityDiagnostic> LegalityGate::check_stage2(const LegalityInput& input) {
    std::vector<LegalityDiagnostic> diagnostics;
    
    if (!input.cfg) {
        auto d = make_diagnostic(
            LegalityFailureKind::CFGUnclosed,
            "unknown", "Stage2", "no CFG",
            "CFG was not constructed");
        diagnostics.push_back(std::move(d));
        return diagnostics;
    }
    
    const auto& cfg = *input.cfg;
    std::string script_id = cfg.script_name;
    
    // Check for unclosed CFG (computed/native/unresolved exits)
    if (!cfg.is_closed()) {
        std::ostringstream reason;
        reason << "CFG has non-static exits: ";
        if (cfg.validation.computed_exits > 0) {
            reason << cfg.validation.computed_exits << " computed, ";
        }
        if (cfg.validation.native_call_exits > 0) {
            reason << cfg.validation.native_call_exits << " native call, ";
        }
        if (cfg.validation.unresolved_exits > 0) {
            reason << cfg.validation.unresolved_exits << " unresolved";
        }
        
        auto d = make_diagnostic(
            LegalityFailureKind::CFGUnclosed,
            script_id, "Stage2", "CFG",
            reason.str());
        diagnostics.push_back(std::move(d));
    }
    
    // Check for invalid target edges
    if (cfg.validation.invalid_targets > 0) {
        auto d = make_diagnostic(
            LegalityFailureKind::CFGInvalidTarget,
            script_id, "Stage2",
            std::to_string(cfg.validation.invalid_targets) + " edges",
            "Static edges land on non-command-boundary addresses");
        diagnostics.push_back(std::move(d));
    }
    
    // Check for orphan commands
    if (cfg.validation.orphan_commands > 0) {
        auto d = make_diagnostic(
            LegalityFailureKind::CFGOrphanCommands,
            script_id, "Stage2",
            std::to_string(cfg.validation.orphan_commands) + " commands",
            "Commands not covered by any basic block");
        diagnostics.push_back(std::move(d));
    }
    
    // Check for overlapping blocks
    if (cfg.validation.overlapping_commands > 0) {
        auto d = make_diagnostic(
            LegalityFailureKind::CFGOverlappingBlocks,
            script_id, "Stage2",
            std::to_string(cfg.validation.overlapping_commands) + " commands",
            "Commands belong to multiple basic blocks");
        diagnostics.push_back(std::move(d));
    }
    
    return diagnostics;
}

std::vector<LegalityDiagnostic> LegalityGate::check_stage3(const LegalityInput& input) {
    std::vector<LegalityDiagnostic> diagnostics;
    
    if (!input.ir) return diagnostics;
    
    std::string script_id = input.cfg ? input.cfg->script_name : "unknown";
    
    // Check each command for unresolved native/RAM references
    for (const auto& cmd : input.ir->commands) {
        std::visit([&](const auto& c) {
            using T = std::decay_t<decltype(c)>;
            
            // Native call checks
            if constexpr (std::is_same_v<T, Cmd_Callasm>) {
                if (input.native_registry) {
                    const auto* entry = input.native_registry->get(c.flat_address);
                    if (!entry || !entry->is_classified()) {
                        auto d = make_diagnostic(
                            LegalityFailureKind::UnresolvedNativeTarget,
                            script_id, "Stage3",
                            "0x" + ([&]() {
                                std::ostringstream ss;
                                ss << std::hex << c.flat_address;
                                return ss.str();
                            })(),
                            "Native call to unclassified routine");
                        d.address = c.flat_address;
                        diagnostics.push_back(std::move(d));
                    }
                }
            }
            else if constexpr (std::is_same_v<T, Cmd_Memcallasm>) {
                // memcallasm reads pointer from RAM - both RAM and native must be classified
                if (input.ram_registry) {
                    const auto* entry = input.ram_registry->get(c.ram_address);
                    if (!entry || !entry->is_classified()) {
                        auto d = make_diagnostic(
                            LegalityFailureKind::UnresolvedRAMAddress,
                            script_id, "Stage3",
                            "0x" + ([&]() {
                                std::ostringstream ss;
                                ss << std::hex << c.ram_address;
                                return ss.str();
                            })(),
                            "memcallasm reads from unclassified RAM address");
                        d.address = c.ram_address;
                        diagnostics.push_back(std::move(d));
                    }
                }
            }
            
            // RAM access checks (readmem, writemem, loadmem already lowered in Stage 4)
            // These are checked at Stage 4 lowering time - if they fail to lower,
            // it means the RAM address wasn't in the lowering rules
            
        }, cmd.data);
    }
    
    return diagnostics;
}

std::vector<LegalityDiagnostic> LegalityGate::check_stage4(const LegalityInput& input) {
    std::vector<LegalityDiagnostic> diagnostics;
    
    if (!input.lowering) {
        auto d = make_diagnostic(
            LegalityFailureKind::UnloweredCommand,
            "unknown", "Stage4", "no lowering result",
            "Semantic lowering was not performed");
        diagnostics.push_back(std::move(d));
        return diagnostics;
    }
    
    const auto& result = *input.lowering;
    std::string script_id = result.ir.script_id;
    
    // Check for unlowered commands
    if (result.commands_unlowered > 0) {
        std::ostringstream offending;
        offending << result.commands_unlowered << " commands (";
        size_t shown = 0;
        for (const auto& [opcode, count] : result.unlowered_by_opcode) {
            if (shown++ > 0) offending << ", ";
            offending << "0x" << std::hex << static_cast<int>(opcode) 
                      << std::dec << "×" << count;
            if (shown >= 3) {
                offending << ", ...";
                break;
            }
        }
        offending << ")";
        
        auto d = make_diagnostic(
            LegalityFailureKind::UnloweredCommand,
            script_id, "Stage4", offending.str(),
            "Commands have no lowering rule");
        diagnostics.push_back(std::move(d));
    }
    
    // Check accounting invariant: consumed = lowered + unlowered + absorbed
    size_t accounting_sum = result.commands_lowered + 
                            result.commands_unlowered + 
                            result.commands_absorbed;
    if (accounting_sum != result.commands_consumed) {
        auto d = make_diagnostic(
            LegalityFailureKind::AccountingMismatch,
            script_id, "Stage4",
            std::to_string(accounting_sum) + " != " + std::to_string(result.commands_consumed),
            "Source command accounting invariant violated");
        diagnostics.push_back(std::move(d));
    }
    
    // Check for compiler diagnostic residue
    if (!result.unlowered.empty()) {
        auto d = make_diagnostic(
            LegalityFailureKind::CompilerDiagnosticResidue,
            script_id, "Stage4",
            std::to_string(result.unlowered.size()) + " UnloweredDiagnostic entries",
            "Compiler diagnostics remain in lowering result");
        diagnostics.push_back(std::move(d));
    }
    
    return diagnostics;
}

std::vector<LegalityDiagnostic> LegalityGate::check_stage5_ir(const LegalityInput& input) {
    std::vector<LegalityDiagnostic> diagnostics;
    
    if (!input.lowering) return diagnostics;
    
    const auto& ir = input.lowering->ir;
    std::string script_id = ir.script_id;
    
    // Stage 5 validates the SemanticScriptIR itself:
    // - No invalid semantic IDs
    // - No raw Crystal concepts (ROM addresses, opcodes, etc.)
    // - All references are resolvable
    
    for (const auto& block : ir.blocks) {
        for (const auto& inst : block.instructions) {
            // Visit each semantic operation and validate
            std::visit([&](const auto& op) {
                using T = std::decay_t<decltype(op)>;
                
                // Check for invalid semantic references
                // Most IDs are uint16_t and can't be "invalid" in the type system,
                // but we can check for known invalid sentinels
                
                if constexpr (std::is_same_v<T, enginemon::Sem_SetFlag> ||
                              std::is_same_v<T, enginemon::Sem_ClearFlag> ||
                              std::is_same_v<T, enginemon::Sem_CheckFlag>) {
                    // EventFlags: 0-2047 valid, EngineFlags: 0-189 valid
                    // See pokecrystal/constants/event_flags.asm, engine_flags.asm
                    bool invalid = false;
                    if (op.flag.ns == enginemon::FlagNamespace::Event && op.flag.value >= 2048) {
                        invalid = true;
                    } else if (op.flag.ns == enginemon::FlagNamespace::Engine && op.flag.value >= 190) {
                        invalid = true;
                    }
                    if (invalid) {
                        auto d = make_diagnostic(
                            LegalityFailureKind::InvalidSemanticReference,
                            script_id, "Stage5",
                            op.flag.to_string(),
                            "Invalid flag ID - out of valid range for namespace");
                        diagnostics.push_back(std::move(d));
                    }
                }
                
                if constexpr (std::is_same_v<T, enginemon::Sem_Warp> ||
                              std::is_same_v<T, enginemon::Sem_WarpFacing> ||
                              std::is_same_v<T, enginemon::Sem_SetMapScene> ||
                              std::is_same_v<T, enginemon::Sem_CheckMapScene>) {
                    // MapId 0xFFFF is typically invalid/none
                    if (op.map == enginemon::MAP_NONE) {
                        auto d = make_diagnostic(
                            LegalityFailureKind::InvalidSemanticReference,
                            script_id, "Stage5",
                            "MapId 0xFFFF",
                            "Invalid map ID in semantic operation");
                        diagnostics.push_back(std::move(d));
                    }
                }
                
                if constexpr (std::is_same_v<T, enginemon::Sem_GiveItem> ||
                              std::is_same_v<T, enginemon::Sem_TakeItem> ||
                              std::is_same_v<T, enginemon::Sem_CheckItem> ||
                              std::is_same_v<T, enginemon::Sem_GiveItemVerbose>) {
                    // ItemId 0 is ITEM_NONE
                    if (op.item == enginemon::ITEM_NONE) {
                        auto d = make_diagnostic(
                            LegalityFailureKind::InvalidSemanticReference,
                            script_id, "Stage5",
                            "ItemId 0",
                            "Invalid item ID (ITEM_NONE) in semantic operation");
                        diagnostics.push_back(std::move(d));
                    }
                }
                
                // Check movement commands for valid data
                if constexpr (std::is_same_v<T, enginemon::Sem_ApplyMovement>) {
                    if (op.commands.empty()) {
                        auto d = make_diagnostic(
                            LegalityFailureKind::InvalidSemanticReference,
                            script_id, "Stage5",
                            "empty movement sequence",
                            "Movement operation has no commands");
                        diagnostics.push_back(std::move(d));
                    }
                }
                
                // Check state variable IDs
                if constexpr (std::is_same_v<T, enginemon::Sem_ReadStateVar> ||
                              std::is_same_v<T, enginemon::Sem_WriteStateVar> ||
                              std::is_same_v<T, enginemon::Sem_SetStateVar>) {
                    if (op.state_var == enginemon::STATEVAR_NONE) {
                        auto d = make_diagnostic(
                            LegalityFailureKind::InvalidSemanticReference,
                            script_id, "Stage5",
                            "StateVarId 0xFFFF",
                            "Invalid state variable ID in semantic operation");
                        diagnostics.push_back(std::move(d));
                    }
                }
                
                // =================================================================
                // Empty text sequence check
                // =================================================================
                // Sem_ShowText, Sem_ShowTextAndEnd, and Sem_FacePlayerAndShowText
                // must NEVER have an empty sequence in a legal script.
                //
                // An empty sequence means the text pointer could not be resolved
                // (null TextRegistry, TEXT_NONE from extract(), or extraction failure).
                // This is explicit failure — a script that shows invisible text is NOT
                // a valid script. The lowering rules pass a potentially empty sequence
                // through; this gate is the hard rejection point.
                //
                // "Lowering returning success after substituting defaults/empty values"
                // is a gate violation. Empty sequences must be caught here.
                if constexpr (std::is_same_v<T, enginemon::Sem_ShowText>) {
                    if (op.sequence.empty()) {
                        auto d = make_diagnostic(
                            LegalityFailureKind::InvalidSemanticReference,
                            script_id, "Stage5",
                            "Sem_ShowText{empty sequence}",
                            "ShowText has empty text sequence — TextRegistry missing or extraction failed");
                        diagnostics.push_back(std::move(d));
                    }
                }
                if constexpr (std::is_same_v<T, enginemon::Sem_ShowTextAndEnd>) {
                    if (op.sequence.empty()) {
                        auto d = make_diagnostic(
                            LegalityFailureKind::InvalidSemanticReference,
                            script_id, "Stage5",
                            "Sem_ShowTextAndEnd{empty sequence}",
                            "ShowTextAndEnd has empty text sequence — TextRegistry missing or extraction failed");
                        diagnostics.push_back(std::move(d));
                    }
                }
                if constexpr (std::is_same_v<T, enginemon::Sem_FacePlayerAndShowText>) {
                    if (op.sequence.empty()) {
                        auto d = make_diagnostic(
                            LegalityFailureKind::InvalidSemanticReference,
                            script_id, "Stage5",
                            "Sem_FacePlayerAndShowText{empty sequence}",
                            "FacePlayerAndShowText has empty text sequence — TextRegistry missing or extraction failed");
                        diagnostics.push_back(std::move(d));
                    }
                }
                
                // =================================================================
                // Sem_PrepareTextArg: buffer_slot must be in valid range 0-2
                // =================================================================
                // Source: GetStringBuffer NUM_STRING_BUFFERS = 3
                // strbuf=0 → wStringBuffer3, strbuf=1 → wStringBuffer4, strbuf=2 → wStringBuffer5
                // No vanilla script command produces strbuf > 2 via GetStringBuffer.
                if constexpr (std::is_same_v<T, enginemon::Sem_PrepareTextArg>) {
                    if (op.buffer_slot > 2) {
                        auto d = make_diagnostic(
                            LegalityFailureKind::InvalidSemanticReference,
                            script_id, "Stage5",
                            "Sem_PrepareTextArg{buffer_slot=" + std::to_string(op.buffer_slot) + "}",
                            "buffer_slot exceeds valid range 0-2 (GetStringBuffer wStringBuffer3/4/5 only)");
                        diagnostics.push_back(std::move(d));
                    }
                }

                // =================================================================
                // Sem_GameSpecificEvent: behavior_name must be non-empty
                // and present in the compiled behavior registry
                // =================================================================
                // Stage 4 lowers Crystal Specials to named GameSpecificEvent ops
                // using BEHAVIOR_TABLE. Stage 5 validates that the name survived
                // correctly and is in the compiled game data registry.
                if constexpr (std::is_same_v<T, enginemon::Sem_GameSpecificEvent>) {
                    if (op.behavior_name.empty()) {
                        auto d = make_diagnostic(
                            LegalityFailureKind::InvalidSemanticReference,
                            script_id, "Stage5",
                            "Sem_GameSpecificEvent{empty behavior_name}",
                            "Sem_GameSpecificEvent must have a non-empty behavior_name");
                        diagnostics.push_back(std::move(d));
                    } else if (input.game_data &&
                               !input.game_data->behavior_names.contains(op.behavior_name)) {
                        auto d = make_diagnostic(
                            LegalityFailureKind::InvalidSemanticReference,
                            script_id, "Stage5",
                            "Sem_GameSpecificEvent{behavior_name=" + op.behavior_name + "}",
                            "behavior_name not in compiled behavior registry (BEHAVIOR_TABLE): " + op.behavior_name);
                        diagnostics.push_back(std::move(d));
                    }
                }

                // =================================================================
                // Sem_Special: raw Crystal Special ID — never legal in package
                // =================================================================
                // Sem_Special must never survive legality. It carries a raw
                // Crystal Special table index, which is Crystal implementation
                // identity — not a stable semantic ID.
                //
                // The legalizer fallback that previously emitted Sem_Special has
                // been removed. This check is the hard backstop: if Sem_Special
                // is manually constructed or survives any path, reject here.
                if constexpr (std::is_same_v<T, enginemon::Sem_Special>) {
                    auto d = make_diagnostic(
                        LegalityFailureKind::InvalidSemanticReference,
                        script_id, "Stage5",
                        "Sem_Special{id=" + std::to_string(op.special_id) + "}",
                        "Sem_Special carries raw Crystal Special ID — not a valid packageable semantic. "
                        "Add a named lowering rule for this Special.");
                    diagnostics.push_back(std::move(d));
                }

                // NOTE: We don't have raw Crystal concepts to check for since
                // Sem_Unlowered is no longer in the SemanticOp variant.
                // The only way raw Crystal concepts could leak is if a lowering
                // rule incorrectly preserved them - but that's caught at Stage 4
                // by the unlowered command check.
                
            }, inst.op);
        }
    }
    
    return diagnostics;
}

// =============================================================================
// CORPUS LEGALITY STATISTICS
// =============================================================================

void CorpusLegalityStats::accumulate(const LegalityResult& result) {
    total_scripts++;
    
    if (result.is_legal) {
        legal_scripts++;
        if (result.legal) {
            eligible_scripts.push_back(result.legal->script_id);
        }
    } else {
        illegal_scripts++;
        
        if (result.illegal) {
            const auto& illegal = *result.illegal;
            
            // Track by kind
            for (const auto& diag : illegal.diagnostics) {
                failures_by_kind[diag.kind]++;
                failures_by_stage[diag.failing_stage]++;
            }
            
            // Track first failure
            if (!illegal.diagnostics.empty()) {
                failures_by_kind[illegal.first_failure_kind]++;
            }
            
            // Sample failures
            if (sample_failures.size() < MAX_SAMPLES && !illegal.diagnostics.empty()) {
                sample_failures.push_back(illegal.diagnostics[0]);
            }
        }
    }
}

double CorpusLegalityStats::legal_percentage() const {
    if (total_scripts == 0) return 100.0;
    return 100.0 * legal_scripts / total_scripts;
}

void CorpusLegalityStats::print_summary() const {
    std::cout << "\n=== Stage 5: Legality Gate Results ===\n";
    std::cout << "Total scripts:               " << total_scripts << "\n";
    std::cout << "Legal scripts:               " << legal_scripts 
              << " (" << std::fixed << std::setprecision(1) << legal_percentage() << "%)\n";
    std::cout << "Illegal scripts:             " << illegal_scripts << "\n";
    
    if (!failures_by_stage.empty()) {
        std::cout << "\nFailures by stage:\n";
        for (const auto& [stage, count] : failures_by_stage) {
            std::cout << "  " << stage << ": " << count << "\n";
        }
    }
    
    if (!failures_by_kind.empty()) {
        std::cout << "\nFailures by kind:\n";
        for (const auto& [kind, count] : failures_by_kind) {
            std::cout << "  " << legality_failure_kind_name(kind) << ": " << count << "\n";
        }
    }
    
    if (!sample_failures.empty()) {
        std::cout << "\nSample failures:\n";
        for (const auto& diag : sample_failures) {
            std::cout << "  " << diag.script_id << " at " << diag.failing_stage
                      << ": " << diag.reason << "\n";
        }
    }
    
    std::cout << "\nScripts eligible for new pipeline: " << eligible_scripts.size() << "\n";
}

// =============================================================================
// SCRIPT COUNT RECONCILIATION
// =============================================================================

bool ScriptCountReconciliation::is_reconciled() const {
    // The corpus and compiler should have the same unique addresses
    // after accounting for:
    // - Deduplication (same ROM address, multiple events)
    // - Standard scripts (called but not compiled as map events)
    return corpus_unique_addresses == compiler_unique_addresses;
}

void ScriptCountReconciliation::print_report() const {
    std::cout << "\n=== Script Count Reconciliation ===\n";
    std::cout << "Corpus (Stage 1-5) unique addresses: " << corpus_unique_addresses << "\n";
    std::cout << "FullGameCompiler unique addresses:   " << compiler_unique_addresses << "\n";
    std::cout << "FullGameCompiler deduplicated:       " << compiler_deduplicated << "\n";
    std::cout << "Total map events with scripts:       " << compiler_total_events << "\n";
    
    std::cout << "\nBreakdown:\n";
    std::cout << "  Standard scripts (callstd/jumpstd): " << standard_scripts << "\n";
    std::cout << "  Generated wrappers:                 " << generated_wrappers << "\n";
    std::cout << "  Text-only scripts (jumptext):       " << text_only_scripts << "\n";
    std::cout << "  Duplicate addresses:                " << duplicate_addresses << "\n";
    
    std::cout << "\n" << explanation << "\n";
    
    if (is_reconciled()) {
        std::cout << "\n*** RECONCILED: Script counts match ***\n";
    } else {
        std::cout << "\n*** NOT RECONCILED: Difference = " 
                  << (static_cast<int64_t>(compiler_unique_addresses) - 
                      static_cast<int64_t>(corpus_unique_addresses)) << " ***\n";
    }
}

} // namespace crystal
