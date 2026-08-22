// crystal/script/text_registry.cpp
// Text extraction and semantic ID assignment implementation

#include "crystal/script/text_registry.hpp"
#include <sstream>
#include <iomanip>

namespace crystal {

enginemon::SemanticTextSequence TextDefinition::to_semantic_sequence() const {
    enginemon::SemanticTextSequence sem;
    for (const auto& elem : sequence.elements) {
        switch (elem.op) {
            // ---------------------------------------------------------------
            // Flow control — 1:1 structural mapping, no information loss
            // ---------------------------------------------------------------
            case TextOp::Text:
                sem.elements.push_back(enginemon::SemanticTextElement::make_text(elem.text));
                break;
            case TextOp::Line:
                sem.elements.push_back(enginemon::SemanticTextElement::make_line());
                break;
            case TextOp::Next:
                sem.elements.push_back(enginemon::SemanticTextElement::make_next());
                break;
            case TextOp::Para:
                sem.elements.push_back(enginemon::SemanticTextElement::make_para());
                break;
            case TextOp::Cont:
                sem.elements.push_back(enginemon::SemanticTextElement::make_cont());
                break;
            case TextOp::Scroll:
                sem.elements.push_back(enginemon::SemanticTextElement::make_scroll());
                break;
            case TextOp::Done:
                sem.elements.push_back(enginemon::SemanticTextElement::make_done());
                break;
            case TextOp::Prompt:
                sem.elements.push_back(enginemon::SemanticTextElement::make_prompt());
                break;

            // ---------------------------------------------------------------
            // TX_STRINGBUFFER (0x14)
            // Source: home/text.asm TextCommand_STRINGBUFFER (line 993):
            //   "0: wStringBuffer3, 1: wStringBuffer4, 2: wStringBuffer5,
            //    3: wStringBuffer2, 4: wStringBuffer1, 5: wEnemyMonNickname,
            //    6: wBattleMonNickname"
            // data/text_buffers.asm StringBufferPointers: 7 entries, 0-indexed.
            //
            // Crystal encodes buffer_id as a 0-indexed byte (0..6).
            // HARD-FAIL on id >= 7: no such entry in StringBufferPointers.
            // The legality gate will reject any script whose text sequence
            // contains an invalid TX_STRINGBUFFER id.
            // ---------------------------------------------------------------
            case TextOp::TextStringBuffer: {
                const uint8_t id = elem.param1;
                if (id > 6) {
                    // Invalid StringBufferPointers index — hard-fail by returning
                    // empty sequence.  The legality gate rejects empty sequences.
                    return enginemon::SemanticTextSequence{};
                }
                // id is the direct 0-indexed slot into StringBufferPointers.
                // The corresponding Sem_PrepareTextArg populated this slot before
                // the text was displayed.
                sem.elements.push_back(enginemon::SemanticTextElement::make_arg(id));
                break;
            }

            // ---------------------------------------------------------------
            // TX_RAM (0x01): display string from classified runtime text source.
            // addr = 16-bit WRAM address.  We carry it as a semantic identity
            // (classified RAM address) — not as a raw GB RAM pointer surviving
            // into runtime.  The runtime maps known addresses to their semantic
            // text source (player name, nickname, etc.).
            // ---------------------------------------------------------------
            case TextOp::TextRam:
                sem.elements.push_back(
                    enginemon::SemanticTextElement::make_ram(elem.addr));
                break;

            // ---------------------------------------------------------------
            // TX_BCD (0x02): display BCD number from WRAM.
            // addr = WRAM address, param1 = flags (high nibble = byte width,
            // low nibble = digit count).  Source: home/text.asm TextCommand_BCD.
            // ---------------------------------------------------------------
            case TextOp::TextBcd:
                sem.elements.push_back(
                    enginemon::SemanticTextElement::make_bcd(elem.addr, elem.param1));
                break;

            // ---------------------------------------------------------------
            // TX_DECIMAL (0x09): display decimal number from WRAM.
            // Same operand layout as TX_BCD.
            // Source: home/text.asm TextCommand_DECIMAL.
            // ---------------------------------------------------------------
            case TextOp::TextDecimal:
                sem.elements.push_back(
                    enginemon::SemanticTextElement::make_decimal(elem.addr, elem.param1));
                break;

            // ---------------------------------------------------------------
            // TX_FAR (0x16): inline far text reference.
            // Crystal macro: db TX_FAR / dw \1 / db BANK(\1)
            // elem.addr = local 16-bit pointer, elem.param2 = bank.
            // Resolved to flat at frontend using crystal_bank_to_flat().
            // The raw bank/pointer pair does NOT survive into semantic IR —
            // only the resolved flat address is stored.
            // ---------------------------------------------------------------
            case TextOp::TextFar: {
                // Convert bank:local to flat address (same formula as bank_utils.hpp)
                const uint32_t bank      = elem.param2;
                const uint16_t local_ptr = elem.addr;
                uint32_t flat = (local_ptr < 0x4000u)
                    ? local_ptr
                    : bank * 0x4000u + (local_ptr - 0x4000u);
                sem.elements.push_back(
                    enginemon::SemanticTextElement::make_far_text(flat));
                break;
            }

            // ---------------------------------------------------------------
            // TX_DAY (0x15): display current day of week.
            // No operands — runtime queries calendar.
            // ---------------------------------------------------------------
            case TextOp::TextDay:
                sem.elements.push_back(enginemon::SemanticTextElement::make_day());
                break;

            // ---------------------------------------------------------------
            // Text sound effects: preserve the opcode as a sound identity.
            // These trigger audio cues mid-text (item jingle, fanfare, etc.).
            // ---------------------------------------------------------------
            case TextOp::TextSoundItem:
                sem.elements.push_back(
                    enginemon::SemanticTextElement::make_sound(0x0f));
                break;
            case TextOp::TextSoundCaught:
                sem.elements.push_back(
                    enginemon::SemanticTextElement::make_sound(0x10));
                break;
            case TextOp::TextSoundFanfare:
                sem.elements.push_back(
                    enginemon::SemanticTextElement::make_sound(0x12));
                break;

            // ---------------------------------------------------------------
            // Presentation-only TX commands with no semantic content:
            // TX_MOVE (0x03), TX_BOX (0x04), TX_LOW (0x05),
            // TX_PROMPT_BUTTON (0x06), TX_SCROLL (0x07), TX_PAUSE (0x0a),
            // TX_START_ASM (0x08).
            //
            // These affect rendering/cursor position only and carry no
            // data that the semantic model needs to preserve beyond the opcode.
            // Stored as Raw so the element is not silently dropped.
            // ---------------------------------------------------------------
            case TextOp::TextMove:
                sem.elements.push_back(
                    enginemon::SemanticTextElement::make_raw(0x03));
                break;
            case TextOp::TextBox:
                // Box has height/width but these are presentation layout only.
                sem.elements.push_back(
                    enginemon::SemanticTextElement::make_raw(0x04));
                break;
            case TextOp::TextLow:
                sem.elements.push_back(
                    enginemon::SemanticTextElement::make_raw(0x05));
                break;
            case TextOp::TextPromptButton:
                sem.elements.push_back(
                    enginemon::SemanticTextElement::make_raw(0x06));
                break;
            case TextOp::TextScroll:
                sem.elements.push_back(
                    enginemon::SemanticTextElement::make_raw(0x07));
                break;
            case TextOp::TextAsm:
                // TX_START_ASM terminates text parsing; no runtime action needed.
                sem.elements.push_back(
                    enginemon::SemanticTextElement::make_raw(0x08));
                break;
            case TextOp::TextPause:
                sem.elements.push_back(
                    enginemon::SemanticTextElement::make_raw(0x0a));
                break;

            // ---------------------------------------------------------------
            // TextRaw: lossless round-trip container for unrecognized opcodes.
            // raw_bytes[0] = opcode, raw_bytes[1] = optional param.
            // We store as Raw(opcode, param) — sufficient for known corpus.
            // ---------------------------------------------------------------
            case TextOp::TextRaw: {
                uint8_t tx_op  = elem.raw_bytes.empty() ? 0 : elem.raw_bytes[0];
                uint8_t tx_prm = (elem.raw_bytes.size() > 1) ? elem.raw_bytes[1] : 0;
                sem.elements.push_back(
                    enginemon::SemanticTextElement::make_raw(tx_op, tx_prm));
                break;
            }
        }
    }
    return sem;
}

std::string TextDefinition::plain_text() const {
    std::string result;
    for (const auto& elem : sequence.elements) {
        if (elem.op == TextOp::Text) {
            result += elem.text;
        }
    }
    return result;
}

std::string TextDefinition::identity_string() const {
    // Canonical structural identity from typed text operation sequence
    // CRITICAL: Must distinguish all source-semantic differences:
    // - Different control codes (Line, Next, Para, Cont, Scroll, Done, Prompt)
    // - Different TX_RAM/TX_DECIMAL/TX_BCD addresses
    // - Different TX_STRINGBUFFER IDs
    // - Different TX_FAR pointers
    // - Different TX_BOX dimensions
    //
    // DO NOT normalize semantically distinct controls to '\n'
    
    std::ostringstream ss;
    for (const auto& elem : sequence.elements) {
        switch (elem.op) {
            case TextOp::Text:
                ss << "T[" << elem.text << "]";
                break;
            case TextOp::Line:
                ss << "<LINE>";
                break;
            case TextOp::Next:
                ss << "<NEXT>";
                break;
            case TextOp::Para:
                ss << "<PARA>";
                break;
            case TextOp::Cont:
                ss << "<CONT>";
                break;
            case TextOp::Scroll:
                ss << "<SCROLL>";
                break;
            case TextOp::Done:
                ss << "<DONE>";
                break;
            case TextOp::Prompt:
                ss << "<PROMPT>";
                break;
            case TextOp::TextRam:
                ss << "<RAM:" << std::hex << elem.addr << ">";
                break;
            case TextOp::TextBcd:
                ss << "<BCD:" << std::hex << elem.addr << "," << (int)elem.param1 << ">";
                break;
            case TextOp::TextDecimal:
                ss << "<DEC:" << std::hex << elem.addr << "," << (int)elem.param1 << ">";
                break;
            case TextOp::TextStringBuffer:
                ss << "<BUF:" << (int)elem.param1 << ">";
                break;
            case TextOp::TextFar:
                // TX_FAR: addr=local-address, param2=bank
                // CRITICAL: bank is stored in param2, NOT param1.
                // Both addr AND bank must be in the identity — different banks at the same
                // local address are completely different text resources.
                // Source: macros/scripts/text.asm: db TX_FAR / dw \1 / db BANK(\1)
                ss << "<FAR:" << (int)elem.param2 << "," << std::hex << elem.addr << ">";
                break;
            case TextOp::TextBox:
                // param1=height, param2=width (source: home/text.asm TextCommand_BOX)
                ss << "<BOX:" << std::hex << elem.addr << "," 
                   << (int)elem.param1 << "h" << (int)elem.param2 << "w>";
                break;
            case TextOp::TextMove:
                ss << "<MOVE:" << std::hex << elem.addr << ">";
                break;
            case TextOp::TextLow:
                ss << "<LOW>";
                break;
            case TextOp::TextPause:
                ss << "<PAUSE>";
                break;
            case TextOp::TextPromptButton:
                ss << "<WAITBTN>";
                break;
            case TextOp::TextDay:
                ss << "<DAY>";
                break;
            case TextOp::TextAsm:
                ss << "<ASM>";
                break;
            case TextOp::TextSoundItem:
                ss << "<SND_ITEM>";
                break;
            case TextOp::TextSoundCaught:
                ss << "<SND_CAUGHT>";
                break;
            case TextOp::TextSoundFanfare:
                ss << "<SND_FANFARE>";
                break;
            case TextOp::TextRaw:
                // CRITICAL: identity must include the actual byte content, not just length.
                // Two TextRaw elements with the same length but different bytes are
                // semantically different (e.g. text_dots 2 vs text_dots 7).
                ss << "<RAW:";
                for (size_t i = 0; i < elem.raw_bytes.size(); ++i) {
                    if (i > 0) ss << ",";
                    ss << std::hex << std::setw(2) << std::setfill('0') << (int)elem.raw_bytes[i];
                }
                ss << ">";
                break;
            default:
                ss << "<UNK>";
                break;
        }
    }
    return ss.str();
}

TextRegistry::TextRegistry(TextDecoder decoder)
    : decoder_(std::move(decoder))
{
}

enginemon::TextId TextRegistry::register_definition(TextDefinition def) {
    // Check for content-based deduplication
    auto dedup_it = content_to_id_.find(def);
    if (dedup_it != content_to_id_.end()) {
        // Already registered with same content - add address mapping
        address_to_id_[def.source_rom_address] = dedup_it->second;
        ++dedup_count_;
        return dedup_it->second;
    }
    
    // New unique content - assign sequential ID
    enginemon::TextId new_id = static_cast<enginemon::TextId>(definitions_.size());
    def.id = new_id;
    
    definitions_.push_back(def);
    address_to_id_[def.source_rom_address] = new_id;
    content_to_id_[def] = new_id;
    
    return new_id;
}

enginemon::TextId TextRegistry::extract(uint32_t rom_address) {
    if (rom_address == 0) {
        return enginemon::TEXT_NONE;
    }
    
    // Check if already extracted
    auto it = address_to_id_.find(rom_address);
    if (it != address_to_id_.end()) {
        return it->second;
    }
    
    // Extract text using provided decoder
    TextDefinition def;
    def.source_rom_address = rom_address;
    def.sequence = decoder_(rom_address);
    
    // Empty sequence = failed extraction
    if (def.sequence.empty()) {
        return enginemon::TEXT_NONE;
    }
    
    return register_definition(std::move(def));
}

std::optional<enginemon::TextId> TextRegistry::lookup(uint32_t rom_address) const {
    auto it = address_to_id_.find(rom_address);
    if (it != address_to_id_.end()) {
        return it->second;
    }
    return std::nullopt;
}

const TextDefinition* TextRegistry::get(enginemon::TextId id) const {
    if (id < definitions_.size()) {
        return &definitions_[id];
    }
    return nullptr;
}

bool TextRegistry::has(enginemon::TextId id) const {
    return id < definitions_.size();
}

} // namespace crystal
