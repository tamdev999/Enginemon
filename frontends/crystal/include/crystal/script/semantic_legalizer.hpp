#pragma once
// crystal/script/semantic_legalizer.hpp
// Stage 4: Block-local semantic legalization from Crystal CFG to SemanticScriptIR
//
// This module converts Crystal-specific CrystalCommand sequences into
// generic Enginemon SemanticOp sequences. It operates block-locally,
// never matching across basic-block boundaries.
//
// INVARIANTS:
// - Never match across block boundaries, entry points, branch targets, or merge points
// - Every source command is either consumed by a lowering rule or explicitly marked Unlowered
// - No raw Crystal addresses, opcodes, or GB RAM addresses leak into SemanticScriptIR
// - Unlowered commands cannot silently disappear

#include "crystal/script/crystal_command.hpp"
#include "crystal/script/crystal_cfg.hpp"
#include "crystal/script/native_registry.hpp"
#include "crystal/script/elevator_registry.hpp"
#include "engine/scripting/semantic_ir.hpp"
#include <cstdint>
#include <string>
#include <vector>
#include <optional>
#include <functional>

namespace crystal {

// Forward declarations
class RomData;
class TypedScriptDecoder;
class PokeMailRegistry;
class TextRegistry;

// =============================================================================
// LOWERING CONTEXT
// =============================================================================

// Context passed to lowering rules
struct LoweringContext {
    // Source script data
    const CrystalScriptIR* source_ir = nullptr;
    const CrystalCFG* cfg = nullptr;
    const BasicBlock* current_block = nullptr;
    
    // Registries for native/RAM classification
    const NativeCallRegistry* native_registry = nullptr;
    const RamAddressRegistry* ram_registry = nullptr;
    
    // Elevator registry for resolving floor-list ROM pointers
    ElevatorRegistry* elevator_registry = nullptr;
    
    // PokeMail registry for resolving mail ROM pointers to semantic IDs
    PokeMailRegistry* pokemail_registry = nullptr;
    
    // Text registry for resolving text ROM pointers to semantic IDs
    TextRegistry* text_registry = nullptr;
    
    // Current position in block
    size_t cursor = 0;              // Command index within block
    
    // Block-local label mapping (source ROM address -> semantic label ID)
    std::unordered_map<uint32_t, enginemon::SemanticLabelId> label_map;
    
    // Helper to get command at cursor + offset (bounds-checked)
    const CrystalCommand* peek(size_t offset = 0) const;
    
    // Helper to get remaining commands in block
    size_t remaining() const;
    
    // Helper to check if address is a block boundary (leader)
    bool is_block_boundary(uint32_t addr) const;
};

// =============================================================================
// LOWERING RULE RESULT
// =============================================================================

struct RuleResult {
    bool matched = false;           // Did the rule match?
    size_t consumed = 0;            // Number of source commands consumed
    std::vector<enginemon::SemanticInstruction> instructions;  // Emitted semantic ops
    std::vector<uint8_t> absorbed_opcodes;  // Opcodes consumed without producing instructions
    std::string diagnostics;        // Optional diagnostic message
};

// =============================================================================
// LOWERING RULES
// =============================================================================

// A lowering rule is a function that:
// - Examines commands starting at cursor
// - Returns matched=false if it doesn't apply
// - Returns matched=true with consumed>0 and semantic instructions if it applies
using LoweringRule = std::function<RuleResult(LoweringContext& ctx)>;

// Rule registry for extensibility
class LoweringRules {
public:
    // Register a lowering rule
    void add_rule(const std::string& name, LoweringRule rule);
    
    // Try all rules in order, return first match
    RuleResult try_rules(LoweringContext& ctx) const;
    
    // Get rule count
    size_t count() const { return rules_.size(); }
    
    // Initialize with default Crystal lowering rules
    static LoweringRules create_default();
    
private:
    std::vector<std::pair<std::string, LoweringRule>> rules_;
};

// =============================================================================
// SEMANTIC LEGALIZER
// =============================================================================

class SemanticLegalizer {
public:
    // Set registries for native/RAM classification
    void set_native_registry(const NativeCallRegistry* reg) { native_registry_ = reg; }
    void set_ram_registry(const RamAddressRegistry* reg) { ram_registry_ = reg; }
    
    // Set elevator registry for floor-list resolution
    void set_elevator_registry(ElevatorRegistry* reg) { elevator_registry_ = reg; }
    
    // Set PokeMail registry for mail resolution
    void set_pokemail_registry(PokeMailRegistry* reg) { pokemail_registry_ = reg; }
    
    // Set Text registry for trainer win/loss text resolution
    void set_text_registry(TextRegistry* reg) { text_registry_ = reg; }
    
    // Set custom rules (optional, uses defaults if not set)
    void set_rules(LoweringRules rules) { rules_ = std::move(rules); }
    
    // Lower a single script CFG to SemanticScriptIR
    enginemon::LoweringResult lower(const CrystalScriptIR& ir, const CrystalCFG& cfg);
    
private:
    const NativeCallRegistry* native_registry_ = nullptr;
    const RamAddressRegistry* ram_registry_ = nullptr;
    ElevatorRegistry* elevator_registry_ = nullptr;
    PokeMailRegistry* pokemail_registry_ = nullptr;
    TextRegistry* text_registry_ = nullptr;
    LoweringRules rules_;
    
    // Initialize rules if not set
    void ensure_rules();
    
    // Lower a basic block (tracks source_commands_consumed for invariant)
    enginemon::SemanticBasicBlock lower_block(const BasicBlock& block,
                                               const CrystalScriptIR& ir,
                                               LoweringContext& ctx,
                                               size_t& source_commands_consumed,
                                               enginemon::LoweringResult& result,
                                               size_t block_idx);
    
    // Create unlowered diagnostic for a command (compiler-side, NOT in IR)
    enginemon::UnloweredDiagnostic make_unlowered_diagnostic(const CrystalCommand& cmd,
                                                              const std::string& reason,
                                                              size_t block_index,
                                                              size_t instruction_index);
    
    // Build block-local label map
    void build_label_map(const CrystalCFG& cfg, LoweringContext& ctx);
};

// =============================================================================
// DEFAULT LOWERING RULES
// =============================================================================

namespace lowering_rules {

// --- Control Flow ---
RuleResult rule_end(LoweringContext& ctx);
RuleResult rule_return(LoweringContext& ctx);
RuleResult rule_jump(LoweringContext& ctx);
RuleResult rule_conditional(LoweringContext& ctx);
RuleResult rule_call(LoweringContext& ctx);
RuleResult rule_std_script(LoweringContext& ctx);

// --- Flags/Variables ---
RuleResult rule_set_flag(LoweringContext& ctx);
RuleResult rule_clear_flag(LoweringContext& ctx);
RuleResult rule_check_flag(LoweringContext& ctx);
RuleResult rule_set_var(LoweringContext& ctx);
RuleResult rule_add_var(LoweringContext& ctx);
RuleResult rule_random(LoweringContext& ctx);

// --- UI/Text ---
RuleResult rule_open_text(LoweringContext& ctx);
RuleResult rule_close_text(LoweringContext& ctx);
RuleResult rule_write_text(LoweringContext& ctx);
RuleResult rule_jump_text(LoweringContext& ctx);
RuleResult rule_jump_text_face_player(LoweringContext& ctx);
RuleResult rule_wait_button(LoweringContext& ctx);
RuleResult rule_yes_no(LoweringContext& ctx);

// --- Inventory ---
RuleResult rule_give_item(LoweringContext& ctx);
RuleResult rule_take_item(LoweringContext& ctx);
RuleResult rule_check_item(LoweringContext& ctx);
RuleResult rule_verbose_give_item(LoweringContext& ctx);
RuleResult rule_money_ops(LoweringContext& ctx);
RuleResult rule_coin_ops(LoweringContext& ctx);

// --- Party/Pokemon ---
RuleResult rule_give_pokemon(LoweringContext& ctx);
RuleResult rule_give_egg(LoweringContext& ctx);
RuleResult rule_check_pokemon(LoweringContext& ctx);

// --- Movement/Object ---
RuleResult rule_apply_movement(LoweringContext& ctx);
RuleResult rule_face_player(LoweringContext& ctx);
RuleResult rule_face_object(LoweringContext& ctx);
RuleResult rule_turn_object(LoweringContext& ctx);
RuleResult rule_object_visibility(LoweringContext& ctx);
RuleResult rule_move_object(LoweringContext& ctx);
RuleResult rule_set_last_talked(LoweringContext& ctx);
RuleResult rule_emote(LoweringContext& ctx);

// --- Map/Warp/Scene ---
RuleResult rule_warp(LoweringContext& ctx);
RuleResult rule_scene_ops(LoweringContext& ctx);
RuleResult rule_map_ops(LoweringContext& ctx);

// --- Battle ---
RuleResult rule_load_wild_mon(LoweringContext& ctx);
RuleResult rule_load_trainer(LoweringContext& ctx);
RuleResult rule_start_battle(LoweringContext& ctx);
RuleResult rule_battle_aftermath(LoweringContext& ctx);
RuleResult rule_trainer_script_ops(LoweringContext& ctx);
RuleResult rule_battle_tower_text(LoweringContext& ctx);

// --- Audio ---
RuleResult rule_play_music(LoweringContext& ctx);
RuleResult rule_play_sound(LoweringContext& ctx);
RuleResult rule_play_cry(LoweringContext& ctx);
RuleResult rule_audio_control(LoweringContext& ctx);

// --- Time/Wait ---
RuleResult rule_wait(LoweringContext& ctx);
RuleResult rule_pause(LoweringContext& ctx);
RuleResult rule_check_time(LoweringContext& ctx);

// --- Phone ---
RuleResult rule_phone_ops(LoweringContext& ctx);

// --- Visual Effects ---
RuleResult rule_earthquake(LoweringContext& ctx);

// --- RAM/Native Operations ---
RuleResult rule_ram_operations(LoweringContext& ctx);
RuleResult rule_callasm_field_moves(LoweringContext& ctx);

// --- Misc ---
RuleResult rule_wild_toggle(LoweringContext& ctx);
RuleResult rule_special(LoweringContext& ctx);
RuleResult rule_pokepic(LoweringContext& ctx);
RuleResult rule_commerce(LoweringContext& ctx);
RuleResult rule_game_completion(LoweringContext& ctx);
RuleResult rule_checksave(LoweringContext& ctx);

// --- Compound Patterns ---
// These match multi-command patterns like checkevent + iffalse
RuleResult rule_check_flag_conditional(LoweringContext& ctx);
RuleResult rule_check_scene_conditional(LoweringContext& ctx);

} // namespace lowering_rules

// =============================================================================
// OPCODE NAME HELPER
// =============================================================================

// Get human-readable name for Crystal opcode
const char* crystal_opcode_name(uint8_t opcode);

} // namespace crystal
