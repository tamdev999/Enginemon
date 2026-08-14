#pragma once
// crystal/script/typed_decoder.hpp
// Stage 1 typed decoder - produces CrystalCommand with round-trip validation
//
// This decoder:
// - Produces typed CrystalCommand for all opcodes 0x00-0xA9
// - Preserves exact bytes in SourceSpan for round-trip validation
// - Never produces Cmd_Unknown for valid opcodes (only for >= 0xAA)
// - Validates round-trip encoding for every decoded command

#include "crystal/script/crystal_command.hpp"
#include "crystal/rom/loader.hpp"
#include "crystal/rom/symbol_map.hpp"
#include "engine/core/types.hpp"  // For MovementCommand
#include <unordered_set>
#include <unordered_map>

namespace crystal {

// Decoder context for typed decoding (does not hold ROM reference)
struct TypedDecoderContext {
    uint32_t pc = 0;            // Current position
    uint8_t bank = 0;           // Current bank for local pointers
    
    std::unordered_set<uint32_t> visited;
    std::vector<uint32_t> pending;
};

// Corpus-wide statistics
struct TypedDecoderStats {
    size_t scripts_decoded = 0;
    size_t commands_decoded = 0;
    size_t unknown_opcodes = 0;     // Opcodes >= 0xAA
    size_t round_trip_failures = 0;
    std::unordered_map<uint8_t, size_t> opcode_counts;
};

// Typed decoder that produces CrystalScriptIR
class TypedScriptDecoder {
public:
    TypedScriptDecoder(const RomData& rom, const SymbolMap& symbols);
    
    // Decode a script to typed CrystalScriptIR
    CrystalScriptIR decode_script(uint32_t address, const std::string& name = "");
    
    // Decode a single command at current position
    CrystalCommand decode_command(TypedDecoderContext& ctx);
    
    // Get statistics
    TypedDecoderStats get_stats() const;
    
    // Reset statistics
    void reset_stats() { stats_ = {}; }
    
    // Round-trip validation
    bool validate_round_trip(const CrystalCommand& cmd);
    bool validate_script_round_trip(const CrystalScriptIR& ir, std::vector<std::string>* errors = nullptr);
    
private:
    const RomData& rom_;
    const SymbolMap& symbols_;
    
    // Internal stats accumulator
    struct Stats {
        size_t scripts_decoded = 0;
        size_t instructions_decoded = 0;
        size_t unknown_opcodes = 0;
        std::unordered_map<uint8_t, size_t> opcode_counts;
    } stats_;
    
    // Read helpers that DON'T advance PC (caller manages span recording)
    uint8_t peek_byte(TypedDecoderContext& ctx, size_t offset = 0) const;
    uint16_t peek_word(TypedDecoderContext& ctx, size_t offset = 0) const;
    
    // Read helpers that DO advance PC and record to span
    uint8_t read_byte(TypedDecoderContext& ctx, std::vector<uint8_t>& span);
    uint16_t read_word(TypedDecoderContext& ctx, std::vector<uint8_t>& span);
    
    // Map ID reading
    CrystalMapId read_map_id(TypedDecoderContext& ctx, std::vector<uint8_t>& span);
    
    // Pointer resolution
    uint32_t resolve_local_pointer(TypedDecoderContext& ctx, uint16_t ptr) const;
    uint32_t resolve_far_pointer(uint8_t bank, uint16_t ptr) const;
    
    // Label reference creation
    CrystalLabelRef make_label_ref(uint32_t address);
    
    // Movement data decoding
    std::vector<uint8_t> decode_movement_data(uint32_t address) const;
    std::vector<enginemon::MovementCommand> parse_movement_commands(const std::vector<uint8_t>& raw) const;
    
    // Master decode dispatch - returns typed command data for given opcode
    CrystalCommandData dispatch_decode(uint8_t opcode, TypedDecoderContext& ctx, 
                                        std::vector<uint8_t>& span);
};

} // namespace crystal
