#pragma once
// crystal/script/semantic_lua_emitter.hpp
// Stage 7: SemanticScriptIR → generated Lua
//
// Consumes ONLY legal SemanticScriptIR (post-legality, post-linker).
// Emits Lua that targets the ctx.* runtime API.
//
// Contract:
// - No ROM addresses, GB RAM addresses, Crystal opcodes, or raw fallbacks
//   may appear in emitted Lua.
// - Every SemanticOp must have a defined emission.
// - Unknown/unimplemented ops → throw (hard failure — no silent stubs).
// - Output format:
//     script = {}
//     function script.main(ctx)
//       ...
//     end
//     return script
// - Each SemanticBasicBlock → labeled Lua section with ::block_N:: labels.
// - Sem_Call (intra-body) uses goto label inside the same function.
// - Sem_CallStd / Sem_JumpStd → ctx.game:call_std(id, name) / jump_std.
// - Sem_Sdefer → ctx.game:behavior + coroutine.yield("deferred").
//
// Yield protocol (matches existing LuaRuntime yield parsing):
//   coroutine.yield("wait_button")   — dialog/input gate
//   coroutine.yield("wait_frames", N)— timed wait
//   coroutine.yield("movement")      — movement wait
//   coroutine.yield("battle")        — battle wait
//   coroutine.yield("warp")          — warp transition wait
//   coroutine.yield("fade")          — fade wait

#include "engine/scripting/semantic_ir.hpp"
#include <string>
#include <sstream>
#include <stdexcept>

namespace crystal {

class SemanticLuaEmitter {
public:
    SemanticLuaEmitter() = default;

    // Emit Lua for a single SemanticScriptIR body.
    // Returns the Lua source string.
    // Throws std::runtime_error if any op has no implementation.
    std::string emit(const enginemon::SemanticScriptIR& ir) const;

    // Public static helpers — also used by free-function emit_op_part1/2
    static void indent_line(std::ostream& out, int n);
    static std::string escape_lua_string(const std::string& s);
    static std::string direction_name(enginemon::Direction d);
    static std::string emit_text_sequence(const enginemon::SemanticTextSequence& seq);
    static std::string emit_text_element(const enginemon::SemanticTextElement& elem);
    static std::string movement_commands_to_lua(
        const std::vector<enginemon::MovementCommand>& cmds);

private:
    // Emit a single SemanticInstruction
    void emit_instruction(std::ostream& out,
                          const enginemon::SemanticInstruction& inst,
                          int indent) const;

    // Emit a single SemanticOp (dispatcher — calls part1 then part2)
    void emit_op(std::ostream& out,
                 const enginemon::SemanticOp& op,
                 int indent) const;
};

} // namespace crystal
