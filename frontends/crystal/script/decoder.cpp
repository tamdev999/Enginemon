// crystal/script/decoder.cpp
// Crystal script bytecode decoder - uses actual Crystal opcodes from events.asm
// AUTHORITATIVE SOURCE: pokecrystal/macros/scripts/events.asm

#include "crystal/script/decoder.hpp"
#include <stdexcept>
#include <sstream>
#include <iomanip>

namespace crystal {

ScriptDecoder::ScriptDecoder(const RomData& rom, const SymbolMap& symbols)
    : rom_(rom), symbols_(symbols) {
    init_charmap();
}

void ScriptDecoder::init_charmap() {
    // Crystal character map from pokecrystal/constants/charmap.asm
    // AUTHORITATIVE SOURCE: pokecrystal/constants/charmap.asm
    
    // Control characters
    charmap_[0x00] = "";       // <NULL> / TX_START
    charmap_[0x14] = "<PLAYER>"; // <PLAY_G> - gendered player name
    charmap_[0x4E] = "\n";     // <NEXT>
    charmap_[0x4F] = "\n";     // <LINE>
    charmap_[0x50] = "";       // @ - string terminator
    charmap_[0x51] = "\n\n";   // <PARA>
    charmap_[0x52] = "<PLAYER>";
    charmap_[0x53] = "<RIVAL>";
    charmap_[0x54] = "POK\xc3\xa9";   // # - displays as "POKé" (UTF-8 encoded)
    charmap_[0x55] = "\n";     // <CONT>
    charmap_[0x56] = "...";    // <......> - ellipsis (using ASCII dots for safety)
    charmap_[0x57] = "";       // <DONE>
    charmap_[0x58] = "";       // <PROMPT>
    
    // Special text characters from font_extra.png
    charmap_[0x70] = "PO";     // <PO>
    charmap_[0x71] = "KE";     // <KE>
    charmap_[0x72] = "\"";     // opening quote
    charmap_[0x73] = "\"";     // closing quote
    charmap_[0x74] = ".";      // middle dot (using ASCII period)
    charmap_[0x75] = "...";    // ellipsis (using ASCII dots)
    
    // Box drawing characters (using ASCII substitutes for safety)
    charmap_[0x79] = "+";      // top-left corner
    charmap_[0x7A] = "-";      // horizontal line
    charmap_[0x7B] = "+";      // top-right corner
    charmap_[0x7C] = "|";      // vertical line
    charmap_[0x7D] = "+";      // bottom-left corner
    charmap_[0x7E] = "+";      // bottom-right corner
    charmap_[0x7F] = " ";      // space
    
    // Uppercase A-Z (0x80-0x99)
    for (int i = 0; i < 26; i++) {
        charmap_[0x80 + i] = std::string(1, 'A' + i);
    }
    
    // Punctuation after Z
    charmap_[0x9A] = "(";
    charmap_[0x9B] = ")";
    charmap_[0x9C] = ":";
    charmap_[0x9D] = ";";
    charmap_[0x9E] = "[";
    charmap_[0x9F] = "]";

    // Lowercase a-z (0xA0-0xB9)
    for (int i = 0; i < 26; i++) {
        charmap_[0xA0 + i] = std::string(1, 'a' + i);
    }
    
    // German umlauts (UTF-8 encoded)
    charmap_[0xC0] = "\xc3\x84";  // Ä
    charmap_[0xC1] = "\xc3\x96";  // Ö  
    charmap_[0xC2] = "\xc3\x9c";  // Ü
    charmap_[0xC3] = "\xc3\xa4";  // ä
    charmap_[0xC4] = "\xc3\xb6";  // ö
    charmap_[0xC5] = "\xc3\xbc";  // ü
    
    // Contractions (0xD0-0xD6) - CRITICAL for correct text
    charmap_[0xD0] = "'d";
    charmap_[0xD1] = "'l";
    charmap_[0xD2] = "'m";
    charmap_[0xD3] = "'r";
    charmap_[0xD4] = "'s";
    charmap_[0xD5] = "'t";
    charmap_[0xD6] = "'v";
    
    // Arrow left
    charmap_[0xDF] = "<-";     // Using ASCII substitute
    
    // Special characters (0xE0-0xF5)
    charmap_[0xE0] = "'";      // apostrophe
    charmap_[0xE1] = "PK";     // <PK> - part of POKéMON ligature
    charmap_[0xE2] = "MN";     // <MN> - part of POKéMON ligature
    charmap_[0xE3] = "-";      // hyphen
    // 0xE4, 0xE5 unused in English
    charmap_[0xE6] = "?";
    charmap_[0xE7] = "!";
    charmap_[0xE8] = ".";
    charmap_[0xE9] = "&";
    charmap_[0xEA] = "\xc3\xa9";  // é (UTF-8 encoded) - CRITICAL for POKéMON
    charmap_[0xEB] = "->";     // right arrow (ASCII substitute)
    charmap_[0xEC] = ">";      // triangle right (ASCII substitute)
    charmap_[0xED] = ">";      // filled triangle right (ASCII substitute)
    charmap_[0xEE] = "v";      // down arrow (ASCII substitute)
    charmap_[0xEF] = "(M)";    // male symbol (ASCII substitute)
    charmap_[0xF0] = "$";      // Poké Dollar sign (using $ for ASCII safety)
    charmap_[0xF1] = "x";      // multiplication sign (ASCII substitute)
    charmap_[0xF2] = ".";      // <DOT> - decimal point
    charmap_[0xF3] = "/";
    charmap_[0xF4] = ",";
    charmap_[0xF5] = "(F)";    // female symbol (ASCII substitute)
    
    // Numbers 0-9 (0xF6-0xFF)
    for (int i = 0; i < 10; i++) {
        charmap_[0xF6 + i] = std::string(1, '0' + i);
    }
}

uint8_t ScriptDecoder::read_byte(DecoderContext& ctx) {
    return rom_.read_byte(ctx.pc++);
}

uint16_t ScriptDecoder::read_word(DecoderContext& ctx) {
    uint16_t val = rom_.read_word(ctx.pc);
    ctx.pc += 2;
    return val;
}

uint32_t ScriptDecoder::read_pointer(DecoderContext& ctx) {
    // 3-byte far pointer: bank (1) + addr (2)
    uint8_t bank = read_byte(ctx);
    uint16_t addr = read_word(ctx);
    return rom_.bank_addr_to_flat(bank, addr);
}

uint32_t ScriptDecoder::read_local_pointer(DecoderContext& ctx) {
    // 2-byte local pointer within current bank
    uint16_t addr = read_word(ctx);
    return rom_.bank_addr_to_flat(ctx.bank, addr);
}

uint16_t ScriptDecoder::read_map_id(DecoderContext& ctx) {
    // Map ID = group << 8 | map
    uint8_t group = read_byte(ctx);
    uint8_t map = read_byte(ctx);
    return (group << 8) | map;
}

LabelRef ScriptDecoder::make_label_ref(uint32_t address) {
    LabelRef ref;
    ref.rom_address = address;
    
    if (auto name = symbols_.name_at(address)) {
        ref.name = *name;
    } else {
        std::ostringstream ss;
        ss << "loc_" << std::hex << address;
        ref.name = ss.str();
    }
    
    return ref;
}

std::string ScriptDecoder::decode_text(uint32_t address) {
    std::string result;
    result.reserve(256);
    
    uint32_t pos = address;
    
    // Crystal text starts with TX_START (0x00)
    uint8_t first = rom_.read_byte(pos);
    if (first == 0x00) {
        pos++;  // Skip TX_START
    }
    
    while (true) {
        uint8_t ch = rom_.read_byte(pos++);
        
        // String terminator
        if (ch == 0x50 || ch == 0x57 || ch == 0x58) break;
        
        auto it = charmap_.find(ch);
        if (it != charmap_.end()) {
            result += it->second;
        } else {
            // Unknown character - emit placeholder
            result += "?";
        }
        
        // Safety limit
        if (result.size() > 2000) break;
    }
    
    return result;
}

TextSequence ScriptDecoder::decode_text_sequence(uint32_t address) {
    // SEMANTIC TEXT DECODING — Crystal Outer-Command / Literal-Body architecture
    //
    // AUTHORITATIVE SOURCE: pokecrystal/macros/scripts/text.asm, home/text.asm
    //
    // Crystal text resources use a two-mode parser:
    //
    //   OUTER COMMAND STREAM:
    //     Bytes 0x00-0x16 → TX_* commands (dispatched via TextCommands jump table)
    //     TX_START (0x00) → enter LITERAL BODY mode
    //     Bytes 0x4B-0x58 → flow control (SCROLL/NEXT/LINE/PARA/CONT/DONE/PROMPT)
    //
    //   LITERAL BODY mode (PlaceString, entered by TX_START):
    //     Bytes are interpreted as Crystal charmap characters / control codes
    //     0x14 here = <PLAY_G> charmap entry (gendered player name), NOT TX_STRINGBUFFER
    //     0x50 ('@') terminates literal body and returns to OUTER COMMAND STREAM
    //     Other flow-control bytes (0x4E/0x4F/0x51/0x55/0x57/0x58) end the resource
    //
    // This prevents in-literal bytes whose numeric value overlaps TX opcodes (e.g. 0x14)
    // from being incorrectly consumed as TX commands with operands.
    //
    // Reference: home/text.asm TextCommand_START / PlaceString / PlaceNextChar
    //
    // Example: NewBarkTownSignText in ROM:
    //   0x00 "NEW BARK TOWN" 0x51 "The Town Where the" 0x4F "Winds of a New" 0x55 "Beginning Blow" 0x57
    // Outer: 0x00 = TX_START → literal body until '@'
    //   "NEW BARK TOWN" → Text("NEW BARK TOWN")
    //   0x51 → back in outer → Para
    //   ... etc.
    
    TextSequence seq;
    seq.rom_address = address;
    
    uint32_t pos = address;
    std::string current_text;
    current_text.reserve(128);
    
    auto flush_text = [&]() {
        if (!current_text.empty()) {
            seq.elements.push_back(TextElement::make_text(current_text));
            current_text.clear();
        }
    };
    
    // Helper: decode one literal/inner byte (PlaceString mode)
    // Returns true if literal body continues, false if '@' was hit (return to outer)
    // Flow-control terminators within a literal body end the resource and return false
    auto decode_literal_byte = [&](uint8_t byte) -> bool {
        if (byte == 0x50) {
            // '@' (TX_END) — end of this literal segment, return to outer command stream
            return false;
        }
        // Flow control codes that terminate the resource even inside a literal
        if (byte == 0x57 || byte == 0x58) {
            flush_text();
            seq.elements.push_back(byte == 0x57 ? TextElement::make_done() : TextElement::make_prompt());
            return false;  // terminates resource
        }
        // Printable/control bytes from the charmap — NOT TX commands
        auto it = charmap_.find(byte);
        if (it != charmap_.end() && !it->second.empty()) {
            current_text += it->second;
        } else if (byte != 0x00) {
            // Unknown non-null byte in literal — emit placeholder
            current_text += "?";
        }
        return true;
    };
    
    // Outer command stream loop
    bool running = true;
    while (running) {
        uint8_t ch = rom_.read_byte(pos++);
        
        switch (ch) {
            // TX_START (0x00): enter literal body mode
            // Source: TextCommand_START in home/text.asm — calls PlaceString
            // In the inline-text layout, TX_START is immediately followed by
            // the character bytes of the literal string, terminated by '@' (0x50).
            case 0x00: {
                // Enter literal body: read characters until '@' returns to outer mode
                while (true) {
                    uint8_t lit = rom_.read_byte(pos++);
                    bool continues = decode_literal_byte(lit);
                    if (!continues) {
                        // '@' hit: flush and return to outer stream
                        // resource-terminating control codes also handled in decode_literal_byte
                        flush_text();
                        break;
                    }
                    if (current_text.size() > 2000) {
                        flush_text();
                        seq.elements.push_back(TextElement::make_done());
                        return seq;
                    }
                }
                // After '@': continue in outer command stream (not resource termination)
                continue;
            }
            
            // TX_RAM (0x01): text_ram - display RAM contents
            // Operands: dw address (little-endian)
            case 0x01: {
                flush_text();
                uint16_t addr = rom_.read_byte(pos) | (rom_.read_byte(pos + 1) << 8);
                pos += 2;
                seq.elements.push_back(TextElement::make_text_ram(addr));
                continue;
            }
            
            // TX_BCD (0x02): text_bcd - display BCD number
            // Operands: dw address, db flags
            case 0x02: {
                flush_text();
                uint16_t addr = rom_.read_byte(pos) | (rom_.read_byte(pos + 1) << 8);
                pos += 2;
                uint8_t flags = rom_.read_byte(pos++);
                seq.elements.push_back(TextElement::make_text_bcd(addr, flags));
                continue;
            }
            
            // TX_MOVE (0x03): text_move - move cursor
            // Operands: dw position (tilemap address)
            case 0x03: {
                flush_text();
                uint16_t addr = rom_.read_byte(pos) | (rom_.read_byte(pos + 1) << 8);
                pos += 2;
                TextElement elem{TextOp::TextMove, ""};
                elem.addr = addr;
                seq.elements.push_back(elem);
                continue;
            }
            
            // TX_BOX (0x04): text_box - draw text box
            // Operands: dw address, db HEIGHT, db WIDTH
            // Source-proven from home/text.asm TextCommand_BOX comment: "(height, width)"
            //   third byte  → B register = height  (param1)
            //   fourth byte → C register = width   (param2)
            case 0x04: {
                flush_text();
                uint16_t addr = rom_.read_byte(pos) | (rom_.read_byte(pos + 1) << 8);
                pos += 2;
                uint8_t height = rom_.read_byte(pos++);  // first dimension byte = height
                uint8_t width  = rom_.read_byte(pos++);  // second dimension byte = width
                TextElement elem{TextOp::TextBox, ""};
                elem.addr   = addr;
                elem.param1 = height;  // param1 = height (first byte after address)
                elem.param2 = width;   // param2 = width  (second byte after address)
                seq.elements.push_back(elem);
                continue;
            }
            
            // TX_LOW (0x05): text_low - move to bottom of screen
            case 0x05:
                flush_text();
                seq.elements.push_back(TextElement{TextOp::TextLow, ""});
                continue;
            
            // TX_PROMPT_BUTTON (0x06): text_promptbutton - wait for button
            case 0x06:
                flush_text();
                seq.elements.push_back(TextElement{TextOp::TextPromptButton, ""});
                continue;
            
            // TX_SCROLL (0x07): text_scroll - scroll text up
            case 0x07:
                flush_text();
                seq.elements.push_back(TextElement{TextOp::TextScroll, ""});
                continue;
            
            // TX_START_ASM (0x08): text_asm - start inline assembly
            // Terminates text parsing — execution transfers to code after this
            case 0x08:
                flush_text();
                seq.elements.push_back(TextElement{TextOp::TextAsm, ""});
                return seq;
            
            // TX_DECIMAL (0x09): text_decimal - display decimal number
            // Operands: dw address, dn bytes|digits (packed nibble)
            case 0x09: {
                flush_text();
                uint16_t addr = rom_.read_byte(pos) | (rom_.read_byte(pos + 1) << 8);
                pos += 2;
                uint8_t bytes_digits = rom_.read_byte(pos++);
                seq.elements.push_back(TextElement::make_text_decimal(addr, bytes_digits));
                continue;
            }
            
            // TX_PAUSE (0x0a): text_pause - pause briefly
            case 0x0a:
                flush_text();
                seq.elements.push_back(TextElement{TextOp::TextPause, ""});
                continue;
            
            // TX_SOUND_DEX_FANFARE_50_79 (0x0b) - lossless preserve
            case 0x0b: {
                flush_text();
                seq.elements.push_back(TextElement::make_text_raw({0x0b}));
                continue;
            }
            
            // TX_DOTS (0x0c): text_dots - display dots
            // Operands: db count
            case 0x0c: {
                flush_text();
                uint8_t count = rom_.read_byte(pos++);
                seq.elements.push_back(TextElement::make_text_raw({0x0c, count}));
                continue;
            }
            
            // TX_WAIT_BUTTON (0x0d): text_waitbutton
            case 0x0d:
                flush_text();
                seq.elements.push_back(TextElement{TextOp::TextPromptButton, ""});
                continue;
            
            // TX_SOUND_DEX_FANFARE_20_49 (0x0e) - lossless preserve
            case 0x0e: {
                flush_text();
                seq.elements.push_back(TextElement::make_text_raw({0x0e}));
                continue;
            }
            
            // TX_SOUND_ITEM (0x0f)
            case 0x0f:
                flush_text();
                seq.elements.push_back(TextElement{TextOp::TextSoundItem, ""});
                continue;
            
            // TX_SOUND_CAUGHT_MON (0x10)
            case 0x10:
                flush_text();
                seq.elements.push_back(TextElement{TextOp::TextSoundCaught, ""});
                continue;
            
            // TX_SOUND_DEX_FANFARE_80_109 (0x11) - lossless preserve
            case 0x11: {
                flush_text();
                seq.elements.push_back(TextElement::make_text_raw({0x11}));
                continue;
            }
            
            // TX_SOUND_FANFARE (0x12)
            case 0x12:
                flush_text();
                seq.elements.push_back(TextElement{TextOp::TextSoundFanfare, ""});
                continue;
            
            // TX_SOUND_SLOT_MACHINE_START (0x13) - lossless preserve
            case 0x13: {
                flush_text();
                seq.elements.push_back(TextElement::make_text_raw({0x13}));
                continue;
            }
            
            // TX_STRINGBUFFER (0x14): text_buffer - display string buffer
            // NOTE: 0x14 is ONLY TX_STRINGBUFFER in the outer command stream.
            // In literal body mode (PlaceString), 0x14 = <PLAY_G> charmap char.
            // This case only fires in outer mode — the mode distinction is correct.
            // Operands: db buffer_id
            case 0x14: {
                flush_text();
                uint8_t buffer_id = rom_.read_byte(pos++);
                seq.elements.push_back(TextElement::make_text_string_buffer(buffer_id));
                continue;
            }
            
            // TX_DAY (0x15)
            case 0x15:
                flush_text();
                seq.elements.push_back(TextElement{TextOp::TextDay, ""});
                continue;
            
            // TX_FAR (0x16): text_far - far text pointer
            // Operands: dw address (little-endian), db bank
            // Source: macros/scripts/text.asm: db TX_FAR / dw \1 / db BANK(\1)
            case 0x16: {
                flush_text();
                uint16_t addr = rom_.read_byte(pos) | (rom_.read_byte(pos + 1) << 8);
                pos += 2;
                uint8_t bank = rom_.read_byte(pos++);
                seq.elements.push_back(TextElement::make_text_far(addr, bank));
                continue;
            }
            
            // Flow control codes (outer stream)
            case 0x4B:  // <SCROLL>
                flush_text();
                seq.elements.push_back(TextElement::make_scroll());
                continue;
                
            case 0x4E:  // <NEXT>
                flush_text();
                seq.elements.push_back(TextElement::make_next());
                continue;
                
            case 0x4F:  // <LINE>
                flush_text();
                seq.elements.push_back(TextElement::make_line());
                continue;
            
            case 0x50:  // '@' — TX_END — terminates resource in outer mode
                flush_text();
                seq.elements.push_back(TextElement::make_done());
                return seq;
                
            case 0x51:  // <PARA>
                flush_text();
                seq.elements.push_back(TextElement::make_para());
                continue;
                
            case 0x55:  // <CONT>
                flush_text();
                seq.elements.push_back(TextElement::make_cont());
                continue;
                
            case 0x57:  // <DONE>
                flush_text();
                seq.elements.push_back(TextElement::make_done());
                return seq;
                
            case 0x58:  // <PROMPT>
                flush_text();
                seq.elements.push_back(TextElement::make_prompt());
                return seq;
                
            default:
                // In outer command stream, non-TX-command bytes that are not
                // flow-control codes are unexpected. Preserve losslessly.
                {
                    flush_text();
                    std::vector<uint8_t> raw{ch};
                    seq.elements.push_back(TextElement::make_text_raw(raw));
                    continue;
                }
        }
    }
    
    flush_text();
    return seq;
}

// TextSequence debug helper - for logging only, NOT for rendering
std::string TextSequence::debug_string() const {
    std::string result;
    for (const auto& elem : elements) {
        switch (elem.op) {
            case TextOp::Text:   result += elem.text; break;
            case TextOp::Line:   result += "[LINE]"; break;
            case TextOp::Next:   result += "[NEXT]"; break;
            case TextOp::Para:   result += "[PARA]"; break;
            case TextOp::Cont:   result += "[CONT]"; break;
            case TextOp::Scroll: result += "[SCROLL]"; break;
            case TextOp::Done:   result += "[DONE]"; break;
            case TextOp::Prompt: result += "[PROMPT]"; break;
            case TextOp::TextRam: result += "[RAM:0x" + std::to_string(elem.addr) + "]"; break;
            case TextOp::TextBcd: result += "[BCD:0x" + std::to_string(elem.addr) + "]"; break;
            case TextOp::TextMove: result += "[MOVE:0x" + std::to_string(elem.addr) + "]"; break;
            case TextOp::TextBox: result += "[BOX]"; break;
            case TextOp::TextLow: result += "[LOW]"; break;
            case TextOp::TextPromptButton: result += "[WAITBUTTON]"; break;
            case TextOp::TextScroll: result += "[TXSCROLL]"; break;
            case TextOp::TextAsm: result += "[ASM]"; break;
            case TextOp::TextDecimal: result += "[DEC:0x" + std::to_string(elem.addr) + "]"; break;
            case TextOp::TextPause: result += "[PAUSE]"; break;
            case TextOp::TextStringBuffer: result += "[BUF:" + std::to_string(elem.param1) + "]"; break;
            case TextOp::TextDay: result += "[DAY]"; break;
            case TextOp::TextFar: result += "[FAR:" + std::to_string(elem.param2) + ":0x" + std::to_string(elem.addr) + "]"; break;
            case TextOp::TextSoundItem: result += "[SND:ITEM]"; break;
            case TextOp::TextSoundCaught: result += "[SND:CAUGHT]"; break;
            case TextOp::TextSoundFanfare: result += "[SND:FANFARE]"; break;
            case TextOp::TextRaw: result += "[RAW]"; break;
        }
    }
    return result;
}

std::vector<uint8_t> ScriptDecoder::decode_movement_data(uint32_t address) {
    // AUTHORITATIVE SOURCE: pokecrystal/macros/scripts/movement.asm
    // Decodes raw movement bytes including parameter bytes for commands that have them.
    //
    // Terminators: step_end (0x47), remove_object (0x49), step_loop (0x4A), 
    //              step_stop (0x4B), skyfall_top (0x59)
    // Note: step_wait_end (0x48) is ALSO a terminator but has a length param AFTER opcode
    
    std::vector<uint8_t> movements;
    uint32_t pos = address;
    
    while (true) {
        uint8_t cmd = rom_.read_byte(pos++);
        movements.push_back(cmd);
        
        // Terminators without params
        if (cmd == 0x47 || cmd == 0x49 || cmd == 0x4A || cmd == 0x4B || cmd == 0x59) {
            break;
        }
        
        // step_wait_end (0x48) - terminator WITH length param
        if (cmd == 0x48) {
            movements.push_back(rom_.read_byte(pos++));  // length param
            break;  // It's still a terminator
        }
        
        // Commands with parameter bytes (non-terminators)
        // step_sleep with extended param (0x46)
        if (cmd == 0x46) {
            movements.push_back(rom_.read_byte(pos++));
        }
        // step_dig (0x4F) - length param
        if (cmd == 0x4F) {
            movements.push_back(rom_.read_byte(pos++));
        }
        // step_shake (0x55) - displacement param
        if (cmd == 0x55) {
            movements.push_back(rom_.read_byte(pos++));
        }
        // rock_smash (0x57) - length param
        if (cmd == 0x57) {
            movements.push_back(rom_.read_byte(pos++));
        }
        // return_dig (0x58) - length param
        if (cmd == 0x58) {
            movements.push_back(rom_.read_byte(pos++));
        }
        
        // Safety limit
        if (movements.size() > 256) break;
    }
    
    return movements;
}

// Parse raw movement bytes into semantic MovementCommand array
// AUTHORITATIVE SOURCE: pokecrystal/macros/scripts/movement.asm
//
// CRITICAL: No silent degradation. Unknown/invalid movement bytes MUST NOT
// be converted to StepEnd. They must either:
// 1. Be handled correctly with their full semantic meaning
// 2. Throw an error that will be caught at a higher level
std::vector<MovementCommand> ScriptDecoder::parse_movement_commands(const std::vector<uint8_t>& raw) {
    std::vector<MovementCommand> commands;
    
    for (size_t i = 0; i < raw.size(); ++i) {
        uint8_t byte = raw[i];
        MovementCommand cmd;
        cmd.param = 0;
        cmd.direction = Direction::Down;
        
        // Directional commands (0x00-0x37) are base + direction
        // direction = byte & 0x03, type = byte >> 2
        if (byte < 0x38) {
            uint8_t dir = byte & 0x03;
            uint8_t type = byte >> 2;
            
            cmd.direction = static_cast<Direction>(dir);
            cmd.type = static_cast<MovementType>(type);
        }
        // Control commands (0x38-0x3D)
        else if (byte >= 0x38 && byte <= 0x3D) {
            // RemoveSliding=0x38, SetSliding=0x39, RemoveFixedFacing=0x3A, 
            // FixFacing=0x3B, ShowObject=0x3C, HideObject=0x3D
            switch (byte) {
                case 0x38: cmd.type = MovementType::RemoveSliding; break;
                case 0x39: cmd.type = MovementType::SetSliding; break;
                case 0x3A: cmd.type = MovementType::RemoveFixedFacing; break;
                case 0x3B: cmd.type = MovementType::FixFacing; break;
                case 0x3C: cmd.type = MovementType::ShowObject; break;
                case 0x3D: cmd.type = MovementType::HideObject; break;
            }
        }
        // step_sleep 1-8 (0x3E-0x45)
        else if (byte >= 0x3E && byte <= 0x45) {
            cmd.type = MovementType::StepSleep;
            cmd.param = byte - 0x3E + 1;  // 1-8 frames
        }
        // step_sleep with extended param (0x46)
        else if (byte == 0x46) {
            cmd.type = MovementType::StepSleep;
            if (i + 1 < raw.size()) {
                cmd.param = raw[++i];
            }
        }
        // step_end (0x47)
        else if (byte == 0x47) {
            cmd.type = MovementType::StepEnd;
        }
        // step_wait_end (0x48) - has length param
        else if (byte == 0x48) {
            cmd.type = MovementType::StepWaitEnd;
            if (i + 1 < raw.size()) {
                cmd.param = raw[++i];  // length param
            }
        }
        // remove_object (0x49)
        else if (byte == 0x49) {
            cmd.type = MovementType::RemoveObject;
        }
        // step_loop (0x4A)
        else if (byte == 0x4A) {
            cmd.type = MovementType::StepLoop;
        }
        // step_stop (0x4B)
        else if (byte == 0x4B) {
            cmd.type = MovementType::StepStop;
        }
        // teleport_from (0x4C)
        else if (byte == 0x4C) {
            cmd.type = MovementType::TeleportFrom;
        }
        // teleport_to (0x4D)
        else if (byte == 0x4D) {
            cmd.type = MovementType::TeleportTo;
        }
        // skyfall (0x4E)
        else if (byte == 0x4E) {
            cmd.type = MovementType::Skyfall;
        }
        // step_dig (0x4F) - has length param
        else if (byte == 0x4F) {
            cmd.type = MovementType::StepDig;
            if (i + 1 < raw.size()) {
                cmd.param = raw[++i];
            }
        }
        // step_bump (0x50)
        else if (byte == 0x50) {
            cmd.type = MovementType::StepBump;
        }
        // fish_got_bite (0x51)
        else if (byte == 0x51) {
            cmd.type = MovementType::FishGotBite;
        }
        // fish_cast_rod (0x52)
        else if (byte == 0x52) {
            cmd.type = MovementType::FishCastRod;
        }
        // hide_emote (0x53)
        else if (byte == 0x53) {
            cmd.type = MovementType::HideEmote;
        }
        // show_emote (0x54)
        else if (byte == 0x54) {
            cmd.type = MovementType::ShowEmote;
        }
        // step_shake (0x55) - has displacement param
        else if (byte == 0x55) {
            cmd.type = MovementType::StepShake;
            if (i + 1 < raw.size()) {
                cmd.param = raw[++i];
            }
        }
        // tree_shake (0x56)
        else if (byte == 0x56) {
            cmd.type = MovementType::TreeShake;
        }
        // rock_smash (0x57) - has length param
        else if (byte == 0x57) {
            cmd.type = MovementType::RockSmash;
            if (i + 1 < raw.size()) {
                cmd.param = raw[++i];
            }
        }
        // return_dig (0x58) - has length param
        else if (byte == 0x58) {
            cmd.type = MovementType::ReturnDig;
            if (i + 1 < raw.size()) {
                cmd.param = raw[++i];
            }
        }
        // skyfall_top (0x59) - terminal
        else if (byte == 0x59) {
            cmd.type = MovementType::SkyfallTop;
        }
        // INVALID: byte >= 0x5A - no such movement command exists
        // Per Item 4 requirements: NO silent degradation to StepEnd
        else {
            // Movement opcode out of valid range [0x00, 0x59]
            // This is a hard error - the decoder should have produced valid bytes only
            throw std::runtime_error("Invalid movement opcode 0x" + 
                std::to_string(static_cast<int>(byte)) + " at index " + std::to_string(i) +
                " - valid range is 0x00-0x59");
        }
        
        commands.push_back(cmd);
    }
    
    return commands;
}

// ============================================================================
// Opcode handlers - implement each Crystal opcode
// ============================================================================

Operation ScriptDecoder::decode_end(DecoderContext& ctx) {
    return Op_End{};
}

Operation ScriptDecoder::decode_endcallback(DecoderContext& ctx) {
    return Op_End{};  // Same as end for our purposes
}

Operation ScriptDecoder::decode_scall(DecoderContext& ctx) {
    Op_Call op;
    uint32_t target = read_local_pointer(ctx);
    op.target = make_label_ref(target);
    
    if (!ctx.visited.contains(target)) {
        ctx.pending.push_back(target);
    }
    return op;
}

Operation ScriptDecoder::decode_farscall(DecoderContext& ctx) {
    Op_Call op;
    uint32_t target = read_pointer(ctx);
    op.target = make_label_ref(target);
    
    if (!ctx.visited.contains(target)) {
        ctx.pending.push_back(target);
    }
    return op;
}

Operation ScriptDecoder::decode_sjump(DecoderContext& ctx) {
    Op_Jump op;
    uint32_t target = read_local_pointer(ctx);
    op.target = make_label_ref(target);
    
    if (!ctx.visited.contains(target)) {
        ctx.pending.push_back(target);
    }
    return op;
}

Operation ScriptDecoder::decode_farsjump(DecoderContext& ctx) {
    Op_Jump op;
    uint32_t target = read_pointer(ctx);
    op.target = make_label_ref(target);
    
    if (!ctx.visited.contains(target)) {
        ctx.pending.push_back(target);
    }
    return op;
}

Operation ScriptDecoder::decode_iftrue(DecoderContext& ctx) {
    Op_JumpIf op;
    op.condition = "true";
    uint32_t target = read_local_pointer(ctx);
    op.target = make_label_ref(target);
    
    if (!ctx.visited.contains(target)) {
        ctx.pending.push_back(target);
    }
    return op;
}

Operation ScriptDecoder::decode_iffalse(DecoderContext& ctx) {
    Op_JumpIf op;
    op.condition = "false";
    uint32_t target = read_local_pointer(ctx);
    op.target = make_label_ref(target);
    
    if (!ctx.visited.contains(target)) {
        ctx.pending.push_back(target);
    }
    return op;
}

Operation ScriptDecoder::decode_ifequal(DecoderContext& ctx) {
    Op_JumpIf op;
    uint8_t value = read_byte(ctx);
    op.condition = "== " + std::to_string(value);
    uint32_t target = read_local_pointer(ctx);
    op.target = make_label_ref(target);
    
    if (!ctx.visited.contains(target)) {
        ctx.pending.push_back(target);
    }
    return op;
}

Operation ScriptDecoder::decode_ifnotequal(DecoderContext& ctx) {
    Op_JumpIf op;
    uint8_t value = read_byte(ctx);
    op.condition = "!= " + std::to_string(value);
    uint32_t target = read_local_pointer(ctx);
    op.target = make_label_ref(target);
    
    if (!ctx.visited.contains(target)) {
        ctx.pending.push_back(target);
    }
    return op;
}

Operation ScriptDecoder::decode_ifgreater(DecoderContext& ctx) {
    Op_JumpIf op;
    uint8_t value = read_byte(ctx);
    op.condition = "> " + std::to_string(value);
    uint32_t target = read_local_pointer(ctx);
    op.target = make_label_ref(target);
    
    if (!ctx.visited.contains(target)) {
        ctx.pending.push_back(target);
    }
    return op;
}

Operation ScriptDecoder::decode_ifless(DecoderContext& ctx) {
    Op_JumpIf op;
    uint8_t value = read_byte(ctx);
    op.condition = "< " + std::to_string(value);
    uint32_t target = read_local_pointer(ctx);
    op.target = make_label_ref(target);
    
    if (!ctx.visited.contains(target)) {
        ctx.pending.push_back(target);
    }
    return op;
}

Operation ScriptDecoder::decode_jumpstd(DecoderContext& ctx) {
    Op_JumpStd op;
    op.std_id = read_word(ctx);
    op.name = "std_" + std::to_string(op.std_id);
    return op;
}

Operation ScriptDecoder::decode_callstd(DecoderContext& ctx) {
    Op_CallStd op;
    op.std_id = read_word(ctx);
    op.name = "std_" + std::to_string(op.std_id);
    return op;
}

Operation ScriptDecoder::decode_special(DecoderContext& ctx) {
    Op_Special op;
    op.special_id = read_word(ctx);
    op.name = "special_" + std::to_string(op.special_id);
    return op;
}

Operation ScriptDecoder::decode_setval(DecoderContext& ctx) {
    Op_SetVar op;
    op.var = 0;  // wScriptVar
    op.value = read_byte(ctx);
    return op;
}

Operation ScriptDecoder::decode_addval(DecoderContext& ctx) {
    Op_AddVar op;
    op.var = 0;  // wScriptVar
    op.delta = read_byte(ctx);
    return op;
}

// Events vs Flags: Crystal has two flag systems
// - Events: story progress flags (checkevent/setevent/clearevent)
// - Engine flags: system flags (checkflag/setflag/clearflag)

Operation ScriptDecoder::decode_checkevent(DecoderContext& ctx) {
    Op_CheckFlag op;
    op.flag = read_word(ctx);
    return op;
}

Operation ScriptDecoder::decode_setevent(DecoderContext& ctx) {
    Op_SetFlag op;
    op.flag = read_word(ctx);
    return op;
}

Operation ScriptDecoder::decode_clearevent(DecoderContext& ctx) {
    Op_ClearFlag op;
    op.flag = read_word(ctx);
    return op;
}

Operation ScriptDecoder::decode_checkflag(DecoderContext& ctx) {
    Op_CheckFlag op;
    op.flag = read_word(ctx) | 0x8000;  // High bit distinguishes engine flags
    return op;
}

Operation ScriptDecoder::decode_setflag(DecoderContext& ctx) {
    Op_SetFlag op;
    op.flag = read_word(ctx) | 0x8000;
    return op;
}

Operation ScriptDecoder::decode_clearflag(DecoderContext& ctx) {
    Op_ClearFlag op;
    op.flag = read_word(ctx) | 0x8000;
    return op;
}

// Items
Operation ScriptDecoder::decode_giveitem(DecoderContext& ctx) {
    Op_GiveItem op;
    op.item = read_byte(ctx);
    op.count = read_byte(ctx);
    return op;
}

Operation ScriptDecoder::decode_takeitem(DecoderContext& ctx) {
    Op_TakeItem op;
    op.item = read_byte(ctx);
    op.count = read_byte(ctx);
    return op;
}

Operation ScriptDecoder::decode_checkitem(DecoderContext& ctx) {
    Op_CheckItem op;
    op.item = read_byte(ctx);
    return op;
}

Operation ScriptDecoder::decode_givepoke(DecoderContext& ctx) {
    Op_GivePokemon op;
    op.species = read_byte(ctx);
    op.level = read_byte(ctx);
    op.held_item = read_byte(ctx);
    uint8_t trainer = read_byte(ctx);
    if (trainer) {
        // Has nickname and OT name pointers
        read_word(ctx);  // nickname pointer (skip)
        read_word(ctx);  // OT name pointer (skip)
    }
    return op;
}

// Text operations
Operation ScriptDecoder::decode_opentext(DecoderContext& ctx) {
    return Op_OpenText{};
}

Operation ScriptDecoder::decode_closetext(DecoderContext& ctx) {
    return Op_CloseText{};
}

Operation ScriptDecoder::decode_writetext(DecoderContext& ctx) {
    Op_Text op;
    uint32_t text_addr = read_local_pointer(ctx);
    op.sequence = decode_text_sequence(text_addr);
    op.scroll = false;
    return op;
}

Operation ScriptDecoder::decode_farwritetext(DecoderContext& ctx) {
    Op_Text op;
    uint32_t text_addr = read_pointer(ctx);
    op.sequence = decode_text_sequence(text_addr);
    op.scroll = false;
    return op;
}

// jumptext: compound command that opens text box, shows text, waits, closes, ends
Operation ScriptDecoder::decode_jumptext(DecoderContext& ctx) {
    Op_JumpText op;
    op.text_address = read_local_pointer(ctx);
    op.sequence = decode_text_sequence(op.text_address);
    return op;
}

Operation ScriptDecoder::decode_farjumptext(DecoderContext& ctx) {
    Op_JumpText op;
    op.text_address = read_pointer(ctx);
    op.sequence = decode_text_sequence(op.text_address);
    return op;
}

// jumptextfaceplayer: faceplayer + jumptext
Operation ScriptDecoder::decode_jumptextfaceplayer(DecoderContext& ctx) {
    Op_JumpTextFacePlayer op;
    op.text_address = read_local_pointer(ctx);
    op.sequence = decode_text_sequence(op.text_address);
    return op;
}

Operation ScriptDecoder::decode_waitbutton(DecoderContext& ctx) {
    return Op_WaitButton{};  // Explicit wait for button press
}

Operation ScriptDecoder::decode_yesorno(DecoderContext& ctx) {
    return Op_YesNo{};
}

Operation ScriptDecoder::decode_faceplayer(DecoderContext& ctx) {
    Op_FacePlayer op;
    op.object_id = 0;  // Current NPC faces player
    return op;
}

Operation ScriptDecoder::decode_applymovement(DecoderContext& ctx) {
    Op_ApplyMovement op;
    op.object_id = read_byte(ctx);
    uint32_t mov_addr = read_local_pointer(ctx);
    op.movements = decode_movement_data(mov_addr);
    op.commands = parse_movement_commands(op.movements);
    return op;
}

Operation ScriptDecoder::decode_applymovementlasttalked(DecoderContext& ctx) {
    Op_ApplyMovement op;
    op.object_id = 0xFF;  // Special: last talked NPC
    uint32_t mov_addr = read_local_pointer(ctx);
    op.movements = decode_movement_data(mov_addr);
    op.commands = parse_movement_commands(op.movements);
    return op;
}

Operation ScriptDecoder::decode_turnobject(DecoderContext& ctx) {
    Op_FaceObject op;
    op.object_id = read_byte(ctx);
    op.direction = static_cast<Direction>(read_byte(ctx));
    return op;
}

Operation ScriptDecoder::decode_appear(DecoderContext& ctx) {
    Op_ShowSprite op;
    op.object_id = read_byte(ctx);
    return op;
}

Operation ScriptDecoder::decode_disappear(DecoderContext& ctx) {
    Op_HideSprite op;
    op.object_id = read_byte(ctx);
    return op;
}

Operation ScriptDecoder::decode_warp(DecoderContext& ctx) {
    Op_Warp op;
    op.map = read_map_id(ctx);
    op.x = read_byte(ctx);
    op.y = read_byte(ctx);
    return op;
}

Operation ScriptDecoder::decode_warpfacing(DecoderContext& ctx) {
    Op_Warp op;
    read_byte(ctx);  // facing (skip for now)
    op.map = read_map_id(ctx);
    op.x = read_byte(ctx);
    op.y = read_byte(ctx);
    return op;
}

Operation ScriptDecoder::decode_playmusic(DecoderContext& ctx) {
    Op_PlayMusic op;
    op.music = read_word(ctx);
    return op;
}

Operation ScriptDecoder::decode_playsound(DecoderContext& ctx) {
    Op_PlaySfx op;
    op.sfx = read_word(ctx);
    return op;
}

Operation ScriptDecoder::decode_cry(DecoderContext& ctx) {
    Op_PlayCry op;
    op.species = read_word(ctx);
    return op;
}

Operation ScriptDecoder::decode_waitsfx(DecoderContext& ctx) {
    return Op_WaitSfx{};
}

Operation ScriptDecoder::decode_pause(DecoderContext& ctx) {
    Op_Wait op;
    op.frames = read_byte(ctx);
    return op;
}

Operation ScriptDecoder::decode_wait(DecoderContext& ctx) {
    Op_Wait op;
    op.frames = read_byte(ctx);
    return op;
}

// ============================================================================
// Main instruction decoder
// ============================================================================

Instruction ScriptDecoder::decode_instruction(DecoderContext& ctx) {
    Instruction inst;
    inst.rom_address = ctx.pc;
    
    if (auto name = symbols_.name_at(ctx.pc)) {
        inst.source_label = *name;
    }
    
    uint8_t opcode = read_byte(ctx);
    stats_.opcode_counts[opcode]++;
    
    switch (opcode) {
        // Control flow
        case CrystalOp::scall:       inst.op = decode_scall(ctx); break;
        case CrystalOp::farscall:    inst.op = decode_farscall(ctx); break;
        case CrystalOp::sjump:       inst.op = decode_sjump(ctx); break;
        case CrystalOp::farsjump:    inst.op = decode_farsjump(ctx); break;
        case CrystalOp::ifequal:     inst.op = decode_ifequal(ctx); break;
        case CrystalOp::ifnotequal:  inst.op = decode_ifnotequal(ctx); break;
        case CrystalOp::iffalse:     inst.op = decode_iffalse(ctx); break;
        case CrystalOp::iftrue:      inst.op = decode_iftrue(ctx); break;
        case CrystalOp::ifgreater:   inst.op = decode_ifgreater(ctx); break;
        case CrystalOp::ifless:      inst.op = decode_ifless(ctx); break;
        case CrystalOp::jumpstd:     inst.op = decode_jumpstd(ctx); break;
        case CrystalOp::callstd:     inst.op = decode_callstd(ctx); break;
        
        // Special/ASM
        case CrystalOp::special:     inst.op = decode_special(ctx); break;
        
        // Variables
        case CrystalOp::setval:      inst.op = decode_setval(ctx); break;
        case CrystalOp::addval:      inst.op = decode_addval(ctx); break;
        
        // Events/Flags
        case CrystalOp::checkevent:  inst.op = decode_checkevent(ctx); break;
        case CrystalOp::clearevent:  inst.op = decode_clearevent(ctx); break;
        case CrystalOp::setevent:    inst.op = decode_setevent(ctx); break;
        case CrystalOp::checkflag:   inst.op = decode_checkflag(ctx); break;
        case CrystalOp::clearflag:   inst.op = decode_clearflag(ctx); break;
        case CrystalOp::setflag:     inst.op = decode_setflag(ctx); break;

        // Items
        case CrystalOp::giveitem:    inst.op = decode_giveitem(ctx); break;
        case CrystalOp::takeitem:    inst.op = decode_takeitem(ctx); break;
        case CrystalOp::checkitem:   inst.op = decode_checkitem(ctx); break;
        
        // Pokemon
        case CrystalOp::givepoke:    inst.op = decode_givepoke(ctx); break;
        
        // Text
        case CrystalOp::opentext:    inst.op = decode_opentext(ctx); break;
        case CrystalOp::closetext:   inst.op = decode_closetext(ctx); break;
        case CrystalOp::writetext:   inst.op = decode_writetext(ctx); break;
        case CrystalOp::farwritetext: inst.op = decode_farwritetext(ctx); break;
        case CrystalOp::jumptext:    inst.op = decode_jumptext(ctx); break;
        case CrystalOp::farjumptext: inst.op = decode_farjumptext(ctx); break;
        case CrystalOp::jumptextfaceplayer: inst.op = decode_jumptextfaceplayer(ctx); break;
        case CrystalOp::waitbutton:  inst.op = decode_waitbutton(ctx); break;
        case CrystalOp::yesorno:     inst.op = decode_yesorno(ctx); break;
        
        // Movement
        case CrystalOp::faceplayer:  inst.op = decode_faceplayer(ctx); break;
        case CrystalOp::applymovement: inst.op = decode_applymovement(ctx); break;
        case CrystalOp::applymovementlasttalked: inst.op = decode_applymovementlasttalked(ctx); break;
        case CrystalOp::turnobject:  inst.op = decode_turnobject(ctx); break;
        case CrystalOp::appear:      inst.op = decode_appear(ctx); break;
        case CrystalOp::disappear:   inst.op = decode_disappear(ctx); break;
        
        // Warps
        case CrystalOp::warp:        inst.op = decode_warp(ctx); break;
        case CrystalOp::warpfacing:  inst.op = decode_warpfacing(ctx); break;
        
        // Audio
        case CrystalOp::playmusic:   inst.op = decode_playmusic(ctx); break;
        case CrystalOp::playsound:   inst.op = decode_playsound(ctx); break;
        case CrystalOp::cry:         inst.op = decode_cry(ctx); break;
        case CrystalOp::waitsfx:     inst.op = decode_waitsfx(ctx); break;
        
        // Timing
        case CrystalOp::pause:       inst.op = decode_pause(ctx); break;
        case CrystalOp::wait:        inst.op = decode_wait(ctx); break;
        
        // End commands
        case CrystalOp::end:         inst.op = decode_end(ctx); break;
        case CrystalOp::endcallback: inst.op = decode_endcallback(ctx); break;

        // No-op commands (skip parameter bytes appropriately)
        case CrystalOp::memcall:     read_word(ctx); inst.op = Op_Raw{opcode, {}, "memcall"}; break;
        case CrystalOp::memjump:     read_word(ctx); inst.op = Op_Raw{opcode, {}, "memjump"}; break;
        case CrystalOp::callasm:     read_pointer(ctx); inst.op = Op_Raw{opcode, {}, "callasm"}; break;
        case CrystalOp::memcallasm:  read_word(ctx); inst.op = Op_Raw{opcode, {}, "memcallasm"}; break;
        case CrystalOp::checkmapscene: read_map_id(ctx); inst.op = Op_Raw{opcode, {}, "checkmapscene"}; break;
        case CrystalOp::setmapscene: read_map_id(ctx); read_byte(ctx); inst.op = Op_Raw{opcode, {}, "setmapscene"}; break;
        case CrystalOp::checkscene:  inst.op = Op_Raw{opcode, {}, "checkscene"}; break;
        case CrystalOp::setscene:    read_byte(ctx); inst.op = Op_Raw{opcode, {}, "setscene"}; break;
        case CrystalOp::random:      read_byte(ctx); inst.op = Op_Raw{opcode, {}, "random"}; break;
        case CrystalOp::checkver:    inst.op = Op_Raw{opcode, {}, "checkver"}; break;
        case CrystalOp::readmem:     read_word(ctx); inst.op = Op_Raw{opcode, {}, "readmem"}; break;
        case CrystalOp::writemem:    read_word(ctx); inst.op = Op_Raw{opcode, {}, "writemem"}; break;
        case CrystalOp::loadmem:     read_word(ctx); read_byte(ctx); inst.op = Op_Raw{opcode, {}, "loadmem"}; break;
        case CrystalOp::readvar:     read_byte(ctx); inst.op = Op_Raw{opcode, {}, "readvar"}; break;
        case CrystalOp::writevar:    read_byte(ctx); inst.op = Op_Raw{opcode, {}, "writevar"}; break;
        case CrystalOp::loadvar:     read_byte(ctx); read_byte(ctx); inst.op = Op_Raw{opcode, {}, "loadvar"}; break;
        case CrystalOp::givemoney:   read_byte(ctx); read_byte(ctx); read_byte(ctx); read_byte(ctx); inst.op = Op_Raw{opcode, {}, "givemoney"}; break;
        case CrystalOp::takemoney:   read_byte(ctx); read_byte(ctx); read_byte(ctx); read_byte(ctx); inst.op = Op_Raw{opcode, {}, "takemoney"}; break;
        case CrystalOp::checkmoney:  read_byte(ctx); read_byte(ctx); read_byte(ctx); read_byte(ctx); inst.op = Op_Raw{opcode, {}, "checkmoney"}; break;
        case CrystalOp::givecoins:   read_word(ctx); inst.op = Op_Raw{opcode, {}, "givecoins"}; break;
        case CrystalOp::takecoins:   read_word(ctx); inst.op = Op_Raw{opcode, {}, "takecoins"}; break;
        case CrystalOp::checkcoins:  read_word(ctx); inst.op = Op_Raw{opcode, {}, "checkcoins"}; break;
        case CrystalOp::addcellnum:  read_byte(ctx); inst.op = Op_Raw{opcode, {}, "addcellnum"}; break;
        case CrystalOp::delcellnum:  read_byte(ctx); inst.op = Op_Raw{opcode, {}, "delcellnum"}; break;
        case CrystalOp::checkcellnum: read_byte(ctx); inst.op = Op_Raw{opcode, {}, "checkcellnum"}; break;
        case CrystalOp::checktime:   read_byte(ctx); inst.op = Op_Raw{opcode, {}, "checktime"}; break;
        case CrystalOp::checkpoke:   read_byte(ctx); inst.op = Op_Raw{opcode, {}, "checkpoke"}; break;
        case CrystalOp::giveegg:     read_byte(ctx); read_byte(ctx); inst.op = Op_Raw{opcode, {}, "giveegg"}; break;

        case CrystalOp::givepokemail: read_word(ctx); inst.op = Op_Raw{opcode, {}, "givepokemail"}; break;
        case CrystalOp::checkpokemail: read_word(ctx); inst.op = Op_Raw{opcode, {}, "checkpokemail"}; break;
        case CrystalOp::wildon:      inst.op = Op_Raw{opcode, {}, "wildon"}; break;
        case CrystalOp::wildoff:     inst.op = Op_Raw{opcode, {}, "wildoff"}; break;
        case CrystalOp::xycompare:   read_word(ctx); inst.op = Op_Raw{opcode, {}, "xycompare"}; break;
        case CrystalOp::warpmod:     read_byte(ctx); read_map_id(ctx); inst.op = Op_Raw{opcode, {}, "warpmod"}; break;
        case CrystalOp::blackoutmod: read_map_id(ctx); inst.op = Op_Raw{opcode, {}, "blackoutmod"}; break;
        case CrystalOp::getmoney:    read_byte(ctx); read_byte(ctx); inst.op = Op_Raw{opcode, {}, "getmoney"}; break;
        case CrystalOp::getcoins:    read_byte(ctx); inst.op = Op_Raw{opcode, {}, "getcoins"}; break;
        case CrystalOp::getnum:      read_byte(ctx); inst.op = Op_Raw{opcode, {}, "getnum"}; break;
        case CrystalOp::getmonname:  read_byte(ctx); read_byte(ctx); inst.op = Op_Raw{opcode, {}, "getmonname"}; break;
        case CrystalOp::getitemname: read_byte(ctx); read_byte(ctx); inst.op = Op_Raw{opcode, {}, "getitemname"}; break;
        case CrystalOp::getcurlandmarkname: read_byte(ctx); inst.op = Op_Raw{opcode, {}, "getcurlandmarkname"}; break;
        case CrystalOp::gettrainername: read_byte(ctx); read_byte(ctx); read_byte(ctx); inst.op = Op_Raw{opcode, {}, "gettrainername"}; break;
        case CrystalOp::getstring:   read_byte(ctx); read_word(ctx); inst.op = Op_Raw{opcode, {}, "getstring"}; break;
        case CrystalOp::itemnotify:  inst.op = Op_Raw{opcode, {}, "itemnotify"}; break;
        case CrystalOp::pocketisfull: inst.op = Op_Raw{opcode, {}, "pocketisfull"}; break;
        case CrystalOp::reanchormap: read_byte(ctx); inst.op = Op_Raw{opcode, {}, "reanchormap"}; break;
        case CrystalOp::writeunusedbyte: read_byte(ctx); inst.op = Op_Raw{opcode, {}, "writeunusedbyte"}; break;
        case CrystalOp::repeattext:  read_byte(ctx); read_byte(ctx); inst.op = Op_Raw{opcode, {}, "repeattext"}; break;
        case CrystalOp::loadmenu:    read_word(ctx); inst.op = Op_Raw{opcode, {}, "loadmenu"}; break;
        case CrystalOp::closewindow: inst.op = Op_Raw{opcode, {}, "closewindow"}; break;
        case CrystalOp::promptbutton: inst.op = Op_Raw{opcode, {}, "promptbutton"}; break;
        case CrystalOp::pokepic:     read_byte(ctx); inst.op = Op_Raw{opcode, {}, "pokepic"}; break;
        case CrystalOp::closepokepic: inst.op = Op_Raw{opcode, {}, "closepokepic"}; break;
        case CrystalOp::_2dmenu:     inst.op = Op_Raw{opcode, {}, "2dmenu"}; break;
        case CrystalOp::verticalmenu: inst.op = Op_Raw{opcode, {}, "verticalmenu"}; break;
        case CrystalOp::loadpikachudata: inst.op = Op_Raw{opcode, {}, "loadpikachudata"}; break;
        case CrystalOp::randomwildmon: inst.op = Op_Raw{opcode, {}, "randomwildmon"}; break;

        case CrystalOp::loadtemptrainer: inst.op = Op_Raw{opcode, {}, "loadtemptrainer"}; break;
        case CrystalOp::loadwildmon: read_byte(ctx); read_byte(ctx); inst.op = Op_Raw{opcode, {}, "loadwildmon"}; break;
        case CrystalOp::loadtrainer: read_byte(ctx); read_byte(ctx); inst.op = Op_Raw{opcode, {}, "loadtrainer"}; break;
        case CrystalOp::startbattle: inst.op = Op_Raw{opcode, {}, "startbattle"}; break;
        case CrystalOp::reloadmapafterbattle: inst.op = Op_Raw{opcode, {}, "reloadmapafterbattle"}; break;
        case CrystalOp::catchtutorial: read_byte(ctx); inst.op = Op_Raw{opcode, {}, "catchtutorial"}; break;
        case CrystalOp::trainertext: read_byte(ctx); inst.op = Op_Raw{opcode, {}, "trainertext"}; break;
        case CrystalOp::trainerflagaction: read_byte(ctx); inst.op = Op_Raw{opcode, {}, "trainerflagaction"}; break;
        case CrystalOp::winlosstext: read_word(ctx); read_word(ctx); inst.op = Op_Raw{opcode, {}, "winlosstext"}; break;
        case CrystalOp::scripttalkafter: inst.op = Op_Raw{opcode, {}, "scripttalkafter"}; break;
        case CrystalOp::endifjustbattled: inst.op = Op_Raw{opcode, {}, "endifjustbattled"}; break;
        case CrystalOp::checkjustbattled: inst.op = Op_Raw{opcode, {}, "checkjustbattled"}; break;
        case CrystalOp::setlasttalked: read_byte(ctx); inst.op = Op_Raw{opcode, {}, "setlasttalked"}; break;
        case CrystalOp::faceobject:  read_byte(ctx); read_byte(ctx); inst.op = Op_Raw{opcode, {}, "faceobject"}; break;
        case CrystalOp::variablesprite: read_byte(ctx); read_byte(ctx); inst.op = Op_Raw{opcode, {}, "variablesprite"}; break;
        case CrystalOp::follow:      read_byte(ctx); read_byte(ctx); inst.op = Op_Raw{opcode, {}, "follow"}; break;
        case CrystalOp::stopfollow:  inst.op = Op_Raw{opcode, {}, "stopfollow"}; break;
        case CrystalOp::moveobject:  read_byte(ctx); read_byte(ctx); read_byte(ctx); inst.op = Op_Raw{opcode, {}, "moveobject"}; break;
        case CrystalOp::writeobjectxy: read_byte(ctx); inst.op = Op_Raw{opcode, {}, "writeobjectxy"}; break;
        case CrystalOp::loademote:   read_byte(ctx); inst.op = Op_Raw{opcode, {}, "loademote"}; break;
        case CrystalOp::showemote:   read_byte(ctx); read_byte(ctx); read_byte(ctx); inst.op = Op_Raw{opcode, {}, "showemote"}; break;
        case CrystalOp::follownotexact: read_byte(ctx); read_byte(ctx); inst.op = Op_Raw{opcode, {}, "follownotexact"}; break;
        case CrystalOp::earthquake:  read_byte(ctx); inst.op = Op_Raw{opcode, {}, "earthquake"}; break;
        case CrystalOp::changemapblocks: read_pointer(ctx); inst.op = Op_Raw{opcode, {}, "changemapblocks"}; break;
        case CrystalOp::changeblock: read_byte(ctx); read_byte(ctx); read_byte(ctx); inst.op = Op_Raw{opcode, {}, "changeblock"}; break;
        case CrystalOp::reloadmap:   inst.op = Op_Raw{opcode, {}, "reloadmap"}; break;
        case CrystalOp::refreshmap:  inst.op = Op_Raw{opcode, {}, "refreshmap"}; break;
        case CrystalOp::writecmdqueue: read_word(ctx); inst.op = Op_Raw{opcode, {}, "writecmdqueue"}; break;
        case CrystalOp::delcmdqueue: read_byte(ctx); inst.op = Op_Raw{opcode, {}, "delcmdqueue"}; break;

        case CrystalOp::encountermusic: inst.op = Op_Raw{opcode, {}, "encountermusic"}; break;
        case CrystalOp::musicfadeout: read_word(ctx); read_byte(ctx); inst.op = Op_Raw{opcode, {}, "musicfadeout"}; break;
        case CrystalOp::playmapmusic: inst.op = Op_Raw{opcode, {}, "playmapmusic"}; break;
        case CrystalOp::dontrestartmapmusic: inst.op = Op_Raw{opcode, {}, "dontrestartmapmusic"}; break;
        case CrystalOp::warpsound:   inst.op = Op_Raw{opcode, {}, "warpsound"}; break;
        case CrystalOp::specialsound: inst.op = Op_Raw{opcode, {}, "specialsound"}; break;
        case CrystalOp::autoinput:   read_pointer(ctx); inst.op = Op_Raw{opcode, {}, "autoinput"}; break;
        case CrystalOp::newloadmap:  read_byte(ctx); inst.op = Op_Raw{opcode, {}, "newloadmap"}; break;
        case CrystalOp::deactivatefacing: read_byte(ctx); inst.op = Op_Raw{opcode, {}, "deactivatefacing"}; break;
        case CrystalOp::sdefer:      read_word(ctx); inst.op = Op_Raw{opcode, {}, "sdefer"}; break;
        case CrystalOp::warpcheck:   inst.op = Op_Raw{opcode, {}, "warpcheck"}; break;
        case CrystalOp::stopandsjump: read_word(ctx); inst.op = Op_Raw{opcode, {}, "stopandsjump"}; break;
        case CrystalOp::reloadend:   read_byte(ctx); inst.op = Op_Raw{opcode, {}, "reloadend"}; break;
        case CrystalOp::endall:      inst.op = Op_End{}; break;
        case CrystalOp::pokemart:    read_byte(ctx); read_word(ctx); inst.op = Op_Raw{opcode, {}, "pokemart"}; break;
        case CrystalOp::elevator:    read_word(ctx); inst.op = Op_Raw{opcode, {}, "elevator"}; break;
        case CrystalOp::trade:       read_byte(ctx); inst.op = Op_Raw{opcode, {}, "trade"}; break;
        case CrystalOp::askforphonenumber: read_byte(ctx); inst.op = Op_Raw{opcode, {}, "askforphonenumber"}; break;
        case CrystalOp::phonecall:   read_word(ctx); inst.op = Op_Raw{opcode, {}, "phonecall"}; break;
        case CrystalOp::hangup:      inst.op = Op_Raw{opcode, {}, "hangup"}; break;
        case CrystalOp::describedecoration: read_byte(ctx); inst.op = Op_Raw{opcode, {}, "describedecoration"}; break;
        case CrystalOp::fruittree:   read_byte(ctx); inst.op = Op_Raw{opcode, {}, "fruittree"}; break;
        case CrystalOp::specialphonecall: read_word(ctx); inst.op = Op_Raw{opcode, {}, "specialphonecall"}; break;
        case CrystalOp::checkphonecall: inst.op = Op_Raw{opcode, {}, "checkphonecall"}; break;
        case CrystalOp::verbosegiveitem: read_byte(ctx); read_byte(ctx); inst.op = Op_Raw{opcode, {}, "verbosegiveitem"}; break;
        case CrystalOp::verbosegiveitemvar: read_byte(ctx); read_byte(ctx); inst.op = Op_Raw{opcode, {}, "verbosegiveitemvar"}; break;
        case CrystalOp::swarm:       read_byte(ctx); read_map_id(ctx); inst.op = Op_Raw{opcode, {}, "swarm"}; break;
        case CrystalOp::halloffame:  inst.op = Op_Raw{opcode, {}, "halloffame"}; break;
        case CrystalOp::credits:     inst.op = Op_Raw{opcode, {}, "credits"}; break;
        case CrystalOp::battletowertext: read_byte(ctx); inst.op = Op_Raw{opcode, {}, "battletowertext"}; break;
        case CrystalOp::getlandmarkname: read_byte(ctx); read_byte(ctx); inst.op = Op_Raw{opcode, {}, "getlandmarkname"}; break;
        case CrystalOp::gettrainerclassname: read_byte(ctx); read_byte(ctx); inst.op = Op_Raw{opcode, {}, "gettrainerclassname"}; break;
        case CrystalOp::getname:     read_byte(ctx); read_byte(ctx); read_byte(ctx); inst.op = Op_Raw{opcode, {}, "getname"}; break;
        case CrystalOp::checksave:   inst.op = Op_Raw{opcode, {}, "checksave"}; break;

        default:
            // Unknown opcode - this should never happen for valid Crystal scripts
            // If we reach here, either:
            // 1. A legitimate Crystal opcode is missing from the decoder
            // 2. The script address was invalid (pointing to non-script data)
            //
            // Throw with full diagnostic info to identify the root cause.
            stats_.unknown_opcodes++;
            {
                std::ostringstream err;
                err << "Unknown opcode 0x" << std::hex << std::uppercase 
                    << std::setw(2) << std::setfill('0') << (int)opcode
                    << " at ROM address 0x" << inst.rom_address
                    << " (bank " << std::dec << (int)ctx.bank << ")";
                
                // Include context bytes for diagnosis
                err << "\nContext bytes at 0x" << std::hex << inst.rom_address << ": ";
                uint32_t ctx_start = (inst.rom_address >= 8) ? inst.rom_address - 8 : 0;
                for (uint32_t i = ctx_start; i < inst.rom_address + 8 && i < rom_.size(); ++i) {
                    if (i == inst.rom_address) err << "[";
                    err << std::setw(2) << std::setfill('0') << (int)rom_.read_byte(i);
                    if (i == inst.rom_address) err << "]";
                    err << " ";
                }
                
                // Check if this looks like text data (common mis-parse)
                bool looks_like_text = (opcode >= 0x80 && opcode <= 0xB9) || // A-Z, a-z
                                       (opcode >= 0xF6 && opcode <= 0xFF);   // 0-9
                if (looks_like_text) {
                    err << "\n(Opcode looks like text character - likely invalid script address)";
                }
                
                throw std::runtime_error(err.str());
            }
    }
    
    stats_.instructions_decoded++;
    return inst;
}

// ============================================================================
// Script decoder entry point
// ============================================================================

ScriptIR ScriptDecoder::decode_script(uint32_t address, const std::string& name) {
    ScriptIR script;
    script.rom_start = address;
    script.name = name.empty() ? ("script_" + std::to_string(address)) : name;
    
    DecoderContext ctx{rom_, symbols_, address};
    ctx.bank = rom_.flat_to_bank(address);
    ctx.pending.push_back(address);
    
    // Safety limits to prevent infinite loops on malformed scripts
    constexpr size_t MAX_INSTRUCTIONS = 1000;
    constexpr size_t MAX_PC_ADVANCE = 0x4000;  // 16KB max script size per branch
    
    while (!ctx.pending.empty()) {
        uint32_t addr = ctx.pending.back();
        ctx.pending.pop_back();
        
        if (ctx.visited.contains(addr)) continue;
        ctx.visited.insert(addr);
        
        ctx.pc = addr;
        ctx.bank = rom_.flat_to_bank(addr);
        uint32_t start_pc = ctx.pc;
        
        // Decode until terminating instruction
        while (true) {
            // Safety: check for runaway decoding
            if (script.instructions.size() >= MAX_INSTRUCTIONS) {
                throw std::runtime_error("Script exceeded max instruction limit: " + name);
            }
            if (ctx.pc - start_pc >= MAX_PC_ADVANCE) {
                throw std::runtime_error("Script exceeded max size limit: " + name);
            }
            if (ctx.pc >= rom_.size()) {
                throw std::runtime_error("Script PC exceeded ROM bounds: " + name);
            }
            
            Instruction inst = decode_instruction(ctx);
            script.instructions.push_back(inst);
            
            // Check for control flow termination
            bool is_terminator = std::visit([](const auto& op) -> bool {
                using T = std::decay_t<decltype(op)>;
                if constexpr (std::is_same_v<T, Op_End>) return true;
                if constexpr (std::is_same_v<T, Op_Return>) return true;
                if constexpr (std::is_same_v<T, Op_Jump>) return true;
                if constexpr (std::is_same_v<T, Op_JumpText>) return true;
                if constexpr (std::is_same_v<T, Op_JumpTextFacePlayer>) return true;
                if constexpr (std::is_same_v<T, Op_JumpStd>) return true;
                return false;
            }, inst.op);
            
            if (is_terminator) break;
        }
    }
    
    script.rom_end = ctx.pc;
    stats_.scripts_decoded++;
    
    return script;
}

} // namespace crystal
