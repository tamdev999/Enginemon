// crystal/script/crystal_cfg.cpp
// Stage 2: CFG construction over typed CrystalCommand IR
//
// This module consumes already-decoded CrystalScriptIR and builds a CFG.
// It does NOT perform decoding - that is Stage 1's responsibility.
//
// Key corrections applied:
// 1. callasm/memcallasm → NativeCall (control flow is unproven, no fallthrough assumed)
// 2. jumpstd/callstd → Resolved via StdScripts table
// 3. Explicit Fallthrough edges when block ends because next command is a leader

#include "crystal/script/crystal_cfg.hpp"
#include "crystal/rom/loader.hpp"
#include <algorithm>
#include <queue>

namespace crystal {

// =============================================================================
// STD SCRIPTS TABLE
// =============================================================================

bool StdScriptsTable::load(const RomData& rom, uint32_t table_address, size_t count,
                           uint8_t entry_size) {
    entries_.clear();
    loaded_ = false;
    
    if (table_address == 0 || count == 0) {
        return false;
    }
    
    // Entry format:
    //   entry_size=3 (default): dba = bank(1) + address(2 LE)
    //     Vanilla Crystal: each script may live in any bank.
    //   entry_size=2: dw = address(2 LE) only
    //     Polished Crystal: all scripts in the same bank as the table.
    const uint8_t esz = (entry_size == 2) ? 2 : 3;
    const uint8_t table_bank = rom.flat_to_bank(table_address);

    for (size_t i = 0; i < count; ++i) {
        uint32_t entry_addr = table_address + (i * esz);
        if (entry_addr + esz > rom.size()) {
            return false;
        }
        
        StdScriptEntry entry;
        entry.std_id = static_cast<uint16_t>(i);

        if (esz == 3) {
            uint8_t bank = rom.read_byte(entry_addr);
            uint16_t addr = rom.read_word(entry_addr + 1);
            entry.bank = bank;
            entry.address = addr;
            entry.flat_address = rom.bank_to_flat(bank, addr);
        } else {
            // 2-byte dw: address only; bank is the same as the table itself.
            uint16_t addr = rom.read_word(entry_addr);
            entry.bank = table_bank;
            entry.address = addr;
            entry.flat_address = rom.bank_to_flat(table_bank, addr);
        }
        
        entries_.push_back(entry);
    }
    
    loaded_ = true;
    return true;
}

uint32_t StdScriptsTable::resolve(uint16_t std_id) const {
    if (!loaded_ || std_id >= entries_.size()) {
        return 0;
    }
    return entries_[std_id].flat_address;
}

const StdScriptEntry* StdScriptsTable::get(uint16_t std_id) const {
    if (!loaded_ || std_id >= entries_.size()) {
        return nullptr;
    }
    return &entries_[std_id];
}

// =============================================================================
// EXIT CLASSIFICATION
// =============================================================================

CFGBuilder::ExitClassification CFGBuilder::classify_exit(const CrystalCommand& cmd) {
    ExitClassification result;
    result.kind = ExitKind::Fallthrough;  // Default: continues to next instruction
    result.has_fallthrough = true;
    result.ends_block = false;  // Will be set based on classification
    
    std::visit([&](const auto& c) {
        using T = std::decay_t<decltype(c)>;
        
        // === TERMINAL COMMANDS (no successor) ===
        if constexpr (std::is_same_v<T, Cmd_End> ||
                      std::is_same_v<T, Cmd_Endall> ||
                      std::is_same_v<T, Cmd_Reloadend>) {
            result.kind = ExitKind::Terminal;
            result.has_fallthrough = false;
            result.ends_block = true;
        }
        // === RETURN COMMANDS ===
        else if constexpr (std::is_same_v<T, Cmd_Endcallback>) {
            result.kind = ExitKind::Return;
            result.has_fallthrough = false;
            result.ends_block = true;
        }
        // === UNCONDITIONAL STATIC JUMPS ===
        else if constexpr (std::is_same_v<T, Cmd_Sjump>) {
            result.kind = ExitKind::StaticJump;
            result.primary_target = c.target.rom_address;
            result.has_fallthrough = false;
            result.ends_block = true;
        }
        else if constexpr (std::is_same_v<T, Cmd_Farsjump>) {
            result.kind = ExitKind::StaticJump;
            result.primary_target = c.target.rom_address;
            result.has_fallthrough = false;
            result.ends_block = true;
        }
        else if constexpr (std::is_same_v<T, Cmd_Stopandsjump>) {
            // stopandsjump uses a local pointer (bank inferred from current script)
            // The typed decoder doesn't resolve this - we mark as Unresolved
            // since we don't have the script bank context here
            result.kind = ExitKind::Unresolved;
            result.has_fallthrough = false;
            result.ends_block = true;
        }
        // === TEXT JUMP COMMANDS (terminators that tail-jump to internal scripts) ===
        else if constexpr (std::is_same_v<T, Cmd_Jumptext> ||
                          std::is_same_v<T, Cmd_Jumptextfaceplayer> ||
                          std::is_same_v<T, Cmd_Farjumptext>) {
            // These do jp ScriptJump to internal text display scripts
            // From the script's perspective, they terminate
            result.kind = ExitKind::Terminal;
            result.has_fallthrough = false;
            result.ends_block = true;
        }
        // === JUMPSTD - jump to standard script (static, resolved via table) ===
        else if constexpr (std::is_same_v<T, Cmd_Jumpstd>) {
            // jumpstd is a tail jump to a StdScript - resolve via table
            result.kind = ExitKind::StaticJump;
            result.has_fallthrough = false;
            result.ends_block = true;
            // primary_target will be set during build if StdScripts table is available
        }
        // === CALLSTD - call to standard script (static, resolved via table) ===
        else if constexpr (std::is_same_v<T, Cmd_Callstd>) {
            // callstd is a call to a StdScript - returns to next instruction
            result.kind = ExitKind::StaticCall;
            result.has_fallthrough = true;  // Returns to next instruction
            result.ends_block = true;
            // primary_target will be set during build if StdScripts table is available
        }
        // === FRUITTREE, DESCRIBEDECORATION, SCRIPTTALKAFTER (jp ScriptJump terminators) ===
        else if constexpr (std::is_same_v<T, Cmd_Fruittree> ||
                          std::is_same_v<T, Cmd_Describedecoration> ||
                          std::is_same_v<T, Cmd_Scripttalkafter>) {
            // These use jp ScriptJump to tail-transfer to other scripts
            result.kind = ExitKind::Terminal;
            result.has_fallthrough = false;
            result.ends_block = true;
        }
        // === CONDITIONAL BRANCHES ===
        else if constexpr (std::is_same_v<T, Cmd_Ifequal> ||
                          std::is_same_v<T, Cmd_Ifnotequal> ||
                          std::is_same_v<T, Cmd_Iffalse> ||
                          std::is_same_v<T, Cmd_Iftrue> ||
                          std::is_same_v<T, Cmd_Ifgreater> ||
                          std::is_same_v<T, Cmd_Ifless>) {
            result.kind = ExitKind::Conditional;
            result.primary_target = c.target.rom_address;
            result.has_fallthrough = true;  // Falls through if condition not met
            result.ends_block = true;
        }
        // === STATIC CALL COMMANDS (return to next instruction) ===
        else if constexpr (std::is_same_v<T, Cmd_Scall>) {
            result.kind = ExitKind::StaticCall;
            result.primary_target = c.target.rom_address;
            result.has_fallthrough = true;  // Returns to next instruction
            result.ends_block = true;
        }
        else if constexpr (std::is_same_v<T, Cmd_Farscall>) {
            result.kind = ExitKind::StaticCall;
            result.primary_target = c.target.rom_address;
            result.has_fallthrough = true;
            result.ends_block = true;
        }
        // === NATIVE CALL COMMANDS - control flow MAY be proven ===
        // callasm/memcallasm transfer to native ASM code
        // If the NativeCallRegistry proves Returns control flow, we allow fallthrough
        else if constexpr (std::is_same_v<T, Cmd_Callasm>) {
            result.indirect_address = c.flat_address;  // Native address target
            result.ends_block = true;
            
            // Check native registry for proven control flow
            bool returns = false;
            if (native_registry_) {
                if (auto entry = native_registry_->get(c.flat_address)) {
                    if (entry->control_flow == NativeControlFlow::Returns) {
                        returns = true;
                    }
                }
            }
            
            if (returns) {
                // Proven to return - treat like a call with fallthrough
                result.kind = ExitKind::StaticCall;  // Treat as call, not NativeCall
                result.has_fallthrough = true;
            } else {
                // Control flow unproven - no fallthrough assumed
                result.kind = ExitKind::NativeCall;
                result.has_fallthrough = false;
            }
        }
        else if constexpr (std::is_same_v<T, Cmd_Memcallasm>) {
            result.kind = ExitKind::NativeCall;
            result.indirect_address = c.ram_address;  // RAM address to read target from
            result.has_fallthrough = false;  // DO NOT ASSUME RETURN (computed target)
            result.ends_block = true;
        }
        // === COMPUTED TRANSFERS (via RAM address) - script address ===
        else if constexpr (std::is_same_v<T, Cmd_Memjump>) {
            result.kind = ExitKind::Computed;
            result.indirect_address = c.ram_address;
            result.has_fallthrough = false;
            result.ends_block = true;
        }
        else if constexpr (std::is_same_v<T, Cmd_Memcall>) {
            // memcall reads a script address from RAM and calls it
            // Unlike native calls, script calls are assumed to return
            result.kind = ExitKind::Computed;
            result.indirect_address = c.ram_address;
            result.has_fallthrough = true;  // Script calls return
            result.ends_block = true;
        }
        // === ENDIFJUSTBATTLED - conditional terminator ===
        else if constexpr (std::is_same_v<T, Cmd_Endifjustbattled>) {
            // endifjustbattled: if just battled, end; otherwise continue
            // This is a conditional terminator with fallthrough
            result.kind = ExitKind::Conditional;
            result.has_fallthrough = true;
            result.ends_block = true;
            // No taken target - it terminates if condition met
        }
        // === UNKNOWN - treat as terminal to stop CFG ===
        else if constexpr (std::is_same_v<T, Cmd_Unknown>) {
            result.kind = ExitKind::Terminal;
            result.has_fallthrough = false;
            result.ends_block = true;
        }
        // === Everything else falls through (doesn't end block by itself) ===
        
    }, cmd.data);
    
    return result;
}

std::optional<uint32_t> CFGBuilder::get_static_target(const CrystalCommand& cmd) {
    std::optional<uint32_t> result;
    
    std::visit([&](const auto& c) {
        using T = std::decay_t<decltype(c)>;
        
        // Commands with resolved target field
        if constexpr (std::is_same_v<T, Cmd_Sjump> ||
                      std::is_same_v<T, Cmd_Farsjump> ||
                      std::is_same_v<T, Cmd_Scall> ||
                      std::is_same_v<T, Cmd_Farscall> ||
                      std::is_same_v<T, Cmd_Ifequal> ||
                      std::is_same_v<T, Cmd_Ifnotequal> ||
                      std::is_same_v<T, Cmd_Iffalse> ||
                      std::is_same_v<T, Cmd_Iftrue> ||
                      std::is_same_v<T, Cmd_Ifgreater> ||
                      std::is_same_v<T, Cmd_Ifless>) {
            result = c.target.rom_address;
        }
        // Cmd_Stopandsjump has a local pointer (no target field) - can't resolve statically here
    }, cmd.data);
    
    return result;
}

std::optional<uint32_t> CFGBuilder::resolve_std_script(uint16_t std_id) {
    if (!std_scripts_) {
        return std::nullopt;
    }
    uint32_t addr = std_scripts_->resolve(std_id);
    if (addr == 0) {
        return std::nullopt;
    }
    return addr;
}

// =============================================================================
// CFG CONSTRUCTION
// =============================================================================

void CFGBuilder::identify_leaders(const CrystalScriptIR& ir,
                                  std::unordered_set<uint32_t>& leaders) {
    if (ir.commands.empty()) return;
    
    // Rule 1: First instruction is always a leader (entry point)
    leaders.insert(ir.commands[0].span.rom_address);
    
    for (size_t i = 0; i < ir.commands.size(); ++i) {
        const auto& cmd = ir.commands[i];
        auto exit_class = classify_exit(cmd);
        
        // Rule 2: Target of a branch/jump/call is a leader
        if (exit_class.primary_target.has_value()) {
            leaders.insert(*exit_class.primary_target);
        }
        
        // Handle jumpstd/callstd - resolve via StdScripts table
        std::visit([&](const auto& c) {
            using T = std::decay_t<decltype(c)>;
            if constexpr (std::is_same_v<T, Cmd_Jumpstd>) {
                auto target = resolve_std_script(c.std_id);
                if (target) {
                    leaders.insert(*target);
                }
            }
            else if constexpr (std::is_same_v<T, Cmd_Callstd>) {
                auto target = resolve_std_script(c.std_id);
                if (target) {
                    leaders.insert(*target);
                }
            }
        }, cmd.data);
        
        // Rule 3: Instruction after a branch/jump/call/terminator is a leader
        // (if it exists)
        if (i + 1 < ir.commands.size()) {
            uint32_t next_addr = ir.commands[i + 1].span.rom_address;
            
            // After any block-ending instruction, next is a leader
            if (exit_class.ends_block) {
                leaders.insert(next_addr);
            }
        }
    }
}

void CFGBuilder::build_blocks(const CrystalScriptIR& ir,
                             const std::unordered_set<uint32_t>& leaders,
                             CrystalCFG& cfg) {
    if (ir.commands.empty()) return;
    
    // Build command_boundaries set for validation
    for (const auto& cmd : ir.commands) {
        cfg.command_boundaries.insert(cmd.span.rom_address);
    }
    
    // Scan through commands, creating blocks at leaders
    BasicBlock* current_block = nullptr;
    
    for (size_t i = 0; i < ir.commands.size(); ++i) {
        const auto& cmd = ir.commands[i];
        uint32_t addr = cmd.span.rom_address;
        
        // Start new block at leaders
        if (leaders.contains(addr)) {
            // Finalize previous block if exists and hasn't been ended by a branch
            if (current_block != nullptr && 
                current_block->exit.kind == ExitKind::Fallthrough) {
                // Previous block ended because this command is a leader, not because
                // of a branch/terminator. Create EXPLICIT Fallthrough exit.
                current_block->exit.kind = ExitKind::Fallthrough;
                current_block->exit.primary_target = CFGTarget{
                    .address = addr,
                    .block_id = SIZE_MAX,  // Will be resolved in link phase
                    .symbol = {}
                };
            }
            
            // Create new block
            cfg.blocks.emplace_back();
            current_block = &cfg.blocks.back();
            current_block->id = cfg.blocks.size() - 1;
            current_block->start_address = addr;
            current_block->command_start = i;
            current_block->command_count = 0;
            current_block->is_entry = (cfg.blocks.size() == 1);  // First block is entry
            
            // Register in address map
            cfg.address_to_block[addr] = current_block->id;
        }
        
        // Shouldn't happen if leaders are correct, but guard anyway
        if (current_block == nullptr) {
            cfg.blocks.emplace_back();
            current_block = &cfg.blocks.back();
            current_block->id = cfg.blocks.size() - 1;
            current_block->start_address = addr;
            current_block->command_start = i;
            current_block->command_count = 0;
            cfg.address_to_block[addr] = current_block->id;
        }
        
        // Add command to current block
        current_block->command_count++;
        current_block->end_address = addr + cmd.span.size();
        
        // Classify this command's exit behavior
        auto exit_class = classify_exit(cmd);
        
        // Check if this command ends the block
        bool ends_block = exit_class.ends_block;
        
        // Also check if next instruction is a leader (implicit block end)
        if (!ends_block && i + 1 < ir.commands.size() &&
            leaders.contains(ir.commands[i + 1].span.rom_address)) {
            ends_block = true;
            // This will create a Fallthrough exit at the start of next iteration
        }
        
        if (ends_block) {
            // Set block exit information
            current_block->exit.kind = exit_class.kind;
            current_block->exit.exit_command_address = addr;
            current_block->exit.exit_opcode = cmd.opcode();
            current_block->exit.indirect_address = exit_class.indirect_address;
            
            // Handle primary target
            if (exit_class.primary_target.has_value()) {
                current_block->exit.primary_target = CFGTarget{
                    .address = *exit_class.primary_target,
                    .block_id = SIZE_MAX,  // Will be resolved in link phase
                    .symbol = {}
                };
            }
            
            // Handle jumpstd/callstd - resolve target via StdScripts table
            std::visit([&](const auto& c) {
                using T = std::decay_t<decltype(c)>;
                if constexpr (std::is_same_v<T, Cmd_Jumpstd>) {
                    auto target = resolve_std_script(c.std_id);
                    if (target) {
                        current_block->exit.primary_target = CFGTarget{
                            .address = *target,
                            .block_id = SIZE_MAX,
                            .symbol = {}
                        };
                    } else {
                        // StdScripts table not available - mark as unresolved
                        current_block->exit.kind = ExitKind::Unresolved;
                    }
                }
                else if constexpr (std::is_same_v<T, Cmd_Callstd>) {
                    auto target = resolve_std_script(c.std_id);
                    if (target) {
                        current_block->exit.primary_target = CFGTarget{
                            .address = *target,
                            .block_id = SIZE_MAX,
                            .symbol = {}
                        };
                    } else {
                        // StdScripts table not available - mark as unresolved
                        current_block->exit.kind = ExitKind::Unresolved;
                    }
                }
            }, cmd.data);
            
            // Handle fallthrough target (for Conditional, StaticCall, Computed script calls)
            if (exit_class.has_fallthrough && i + 1 < ir.commands.size()) {
                uint32_t fallthrough_addr = ir.commands[i + 1].span.rom_address;
                current_block->exit.fallthrough_target = CFGTarget{
                    .address = fallthrough_addr,
                    .block_id = SIZE_MAX,
                    .symbol = {}
                };
            }
            
            // If this wasn't an explicit control-flow command but we're ending because
            // next is a leader, we need to set up the Fallthrough
            if (!exit_class.ends_block && i + 1 < ir.commands.size()) {
                // Block ends due to next being a leader - explicit Fallthrough
                current_block->exit.kind = ExitKind::Fallthrough;
                current_block->exit.primary_target = CFGTarget{
                    .address = ir.commands[i + 1].span.rom_address,
                    .block_id = SIZE_MAX,
                    .symbol = {}
                };
            }
            
            current_block = nullptr;
        }
    }
}

void CFGBuilder::link_blocks(CrystalCFG& cfg) {
    for (auto& block : cfg.blocks) {
        // Resolve primary target to block ID
        if (block.exit.primary_target.has_value()) {
            uint32_t target_addr = block.exit.primary_target->address;
            auto it = cfg.address_to_block.find(target_addr);
            if (it != cfg.address_to_block.end()) {
                block.exit.primary_target->block_id = it->second;
                // Add predecessor
                if (it->second < cfg.blocks.size()) {
                    cfg.blocks[it->second].predecessors.push_back(block.id);
                }
            }
        }
        
        // Resolve fallthrough target to block ID
        if (block.exit.fallthrough_target.has_value()) {
            uint32_t target_addr = block.exit.fallthrough_target->address;
            auto it = cfg.address_to_block.find(target_addr);
            if (it != cfg.address_to_block.end()) {
                block.exit.fallthrough_target->block_id = it->second;
                // Add predecessor
                if (it->second < cfg.blocks.size()) {
                    cfg.blocks[it->second].predecessors.push_back(block.id);
                }
            }
        }
    }
}

void CFGBuilder::compute_reachability(CrystalCFG& cfg) {
    if (cfg.blocks.empty()) return;
    
    // BFS from entry block
    std::queue<size_t> worklist;
    worklist.push(0);  // Entry block
    cfg.blocks[0].is_reachable = true;
    
    while (!worklist.empty()) {
        size_t block_id = worklist.front();
        worklist.pop();
        
        const auto& block = cfg.blocks[block_id];
        
        // Visit primary target
        if (block.exit.primary_target.has_value() &&
            block.exit.primary_target->is_resolved()) {
            size_t target_id = block.exit.primary_target->block_id;
            if (target_id < cfg.blocks.size() && !cfg.blocks[target_id].is_reachable) {
                cfg.blocks[target_id].is_reachable = true;
                worklist.push(target_id);
            }
        }
        
        // Visit fallthrough target
        if (block.exit.fallthrough_target.has_value() &&
            block.exit.fallthrough_target->is_resolved()) {
            size_t target_id = block.exit.fallthrough_target->block_id;
            if (target_id < cfg.blocks.size() && !cfg.blocks[target_id].is_reachable) {
                cfg.blocks[target_id].is_reachable = true;
                worklist.push(target_id);
            }
        }
    }
}

CrystalCFG CFGBuilder::build(const CrystalScriptIR& ir) {
    CrystalCFG cfg;
    cfg.entry_address = ir.entry_address;
    cfg.script_name = ir.name;
    cfg.source_ir = &ir;
    
    if (ir.commands.empty()) {
        cfg.validation.valid = true;
        return cfg;
    }
    
    // Phase 1: Identify leaders
    std::unordered_set<uint32_t> leaders;
    identify_leaders(ir, leaders);
    
    // Phase 2: Build blocks
    build_blocks(ir, leaders, cfg);
    
    // Phase 3: Link blocks
    link_blocks(cfg);
    
    // Phase 4: Compute reachability
    compute_reachability(cfg);
    
    // Validate
    cfg.validation = validate(cfg);
    
    return cfg;
}

// =============================================================================
// VALIDATION
// =============================================================================

CFGValidation CFGBuilder::validate(const CrystalCFG& cfg) {
    CFGValidation result;
    result.valid = true;
    
    if (!cfg.source_ir) {
        result.errors.push_back("CFG has no source IR reference");
        result.valid = false;
        return result;
    }
    
    const auto& ir = *cfg.source_ir;
    result.commands_total = ir.commands.size();
    
    // Track which commands are covered by blocks
    std::vector<bool> command_covered(ir.commands.size(), false);
    std::vector<size_t> command_block(ir.commands.size(), SIZE_MAX);
    
    // Validate each block
    for (const auto& block : cfg.blocks) {
        // Check command range validity
        if (block.command_start >= ir.commands.size()) {
            result.errors.push_back("Block " + std::to_string(block.id) + 
                                   " has invalid command_start");
            result.valid = false;
            continue;
        }
        
        if (block.command_start + block.command_count > ir.commands.size()) {
            result.errors.push_back("Block " + std::to_string(block.id) + 
                                   " command range exceeds IR size");
            result.valid = false;
            continue;
        }
        
        // Mark commands as covered, check for overlaps
        for (size_t i = 0; i < block.command_count; ++i) {
            size_t cmd_idx = block.command_start + i;
            if (command_covered[cmd_idx]) {
                result.overlapping_commands++;
                result.valid = false;
            }
            command_covered[cmd_idx] = true;
            command_block[cmd_idx] = block.id;
        }
        
        // Count edge types
        switch (block.exit.kind) {
            case ExitKind::Fallthrough:
                result.fallthrough_edges++;
                break;
            case ExitKind::StaticJump:
                result.static_jump_edges++;
                break;
            case ExitKind::Conditional:
                result.conditional_edges++;
                // Conditional has both taken branch and fallthrough
                break;
            case ExitKind::StaticCall:
                result.static_call_edges++;
                break;
            case ExitKind::Return:
                result.return_edges++;
                break;
            case ExitKind::Terminal:
                result.terminal_exits++;
                break;
            case ExitKind::Computed:
                result.computed_exits++;
                break;
            case ExitKind::NativeCall:
                result.native_call_exits++;
                break;
            case ExitKind::Unresolved:
                result.unresolved_exits++;
                break;
        }
        
        // Validate static targets land on command boundaries
        auto check_target = [&](const std::optional<CFGTarget>& target) {
            if (!target.has_value()) return;
            uint32_t addr = target->address;
            
            // Check if target is a command boundary in THIS script's IR
            if (!cfg.is_command_boundary(addr)) {
                // Target is not a decoded command boundary
                // This could be:
                // 1. A jump to another script (valid, but unresolved in this CFG)
                // 2. A jump into the middle of an instruction (invalid)
                // 3. A jump to data (invalid)
                
                // For now, we only flag it as invalid if the block says
                // the target should be resolved within this CFG
                if (target->is_resolved()) {
                    result.invalid_targets++;
                    result.bad_edges.push_back({block.exit.exit_command_address, addr});
                    result.valid = false;
                }
                // If unresolved, it might be a cross-script reference (valid)
            }
        };
        
        check_target(block.exit.primary_target);
        check_target(block.exit.fallthrough_target);
    }
    
    // Count covered commands
    result.commands_covered = std::count(command_covered.begin(), 
                                         command_covered.end(), true);
    result.orphan_commands = result.commands_total - result.commands_covered;
    
    if (result.orphan_commands > 0) {
        result.warnings.push_back(std::to_string(result.orphan_commands) + 
                                  " commands not in any block");
    }
    
    if (result.overlapping_commands > 0) {
        result.errors.push_back(std::to_string(result.overlapping_commands) + 
                               " commands in multiple blocks");
        result.valid = false;
    }
    
    return result;
}

// =============================================================================
// CorpusCFGStats
// =============================================================================
// Moved from the header (crystal_cfg.hpp) to avoid instantiating the body
// of every CrystalCFG field access in every TU that includes the header.
// The struct definition stays in the header; only the body lives here.

void CorpusCFGStats::accumulate(const CrystalCFG& cfg) {
    total_scripts++;
    total_blocks += cfg.blocks.size();
    total_commands += cfg.source_ir ? cfg.source_ir->commands.size() : 0;

    if (cfg.is_closed())             { closed_cfgs++; }
    if (cfg.has_computed_exits())    { computed_exit_scripts++; }
    if (cfg.has_native_call_exits()) { native_call_scripts++; }
    if (cfg.has_unresolved_exits())  { unresolved_exit_scripts++; }

    fallthrough_edges    += cfg.validation.fallthrough_edges;
    static_jump_edges    += cfg.validation.static_jump_edges;
    conditional_edges    += cfg.validation.conditional_edges;
    static_call_edges    += cfg.validation.static_call_edges;
    return_edges         += cfg.validation.return_edges;
    terminal_exits       += cfg.validation.terminal_exits;
    computed_exits       += cfg.validation.computed_exits;
    native_call_exits    += cfg.validation.native_call_exits;
    unresolved_exits     += cfg.validation.unresolved_exits;
    invalid_target_edges += cfg.validation.invalid_targets;

    if (cfg.validation.orphan_commands > 0)      { orphan_command_scripts++; }
    if (cfg.validation.overlapping_commands > 0) { overlapping_block_scripts++; }

    for (const auto& edge : cfg.validation.bad_edges) {
        if (sample_bad_edges.size() < 10) {
            sample_bad_edges.push_back({cfg.entry_address, edge.second});
        }
    }
}

} // namespace crystal
