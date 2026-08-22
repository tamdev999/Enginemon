// crystal/script/text_registry.cpp
// Text extraction and semantic ID assignment implementation

#include "crystal/script/text_registry.hpp"
#include <sstream>
#include <iomanip>

namespace crystal {

enginemon::SemanticTextSequence TextDefinition::to_semantic_sequence(
    const TextRegistry* registry,
    int far_depth) const {

    // Inline helper: append all elements from a sub-sequence into dest.
    // Used for TX_FAR inlining.
    auto inline_seq = [](enginemon::SemanticTextSequence& dest,
                         const enginemon::SemanticTextSequence& src) {
        for (const auto& e : src.elements) {
            dest.elements.push_back(e);
        }
        // args are not merged — TX_FAR text does not cross-reference parent args
    };

    // Recursion guard: TX_FAR chains can theoretically cycle.
    // Crystal ROM cannot actually contain text cycles (text is ROM data, not
    // self-modifying), but we bound depth conservatively for robustness.
    constexpr int MAX_FAR_DEPTH = 8;

    enginemon::SemanticTextSequence sem;

    for (const auto& elem : sequence.elements) {
        switch (elem.op) {
            // ---------------------------------------------------------------
            // Flow control — 1:1 structural mapping, no information loss.
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
            // Source: data/text_buffers.asm StringBufferPointers + home/text.asm
            // TextCommand_STRINGBUFFER comment:
            //   0=wStringBuffer3, 1=wStringBuffer4, 2=wStringBuffer5,
            //   3=wStringBuffer2, 4=wStringBuffer1, 5=wEnemyMonNickname,
            //   6=wBattleMonNickname
            // Valid range: 0–6. Any id >= 7 → hard-fail.
            //
            // Note: the text_buffer macro (TX_STRINGBUFFER) is never used in
            // vanilla Crystal script text (text_buffer has 0 macro call sites).
            // This case exists for correctness; in practice the decoder already
            // removes case 0x14 from the TX_* dispatch (it is always <PLAY_G>
            // charmap). Reaching here means decoder decoded a genuine TX_STRINGBUFFER
            // from a context that is valid (e.g., ROM hack compatibility).
            // ---------------------------------------------------------------
            case TextOp::TextStringBuffer: {
                const uint8_t id = elem.param1;
                if (id > 6) {
                    return enginemon::SemanticTextSequence{};
                }
                sem.elements.push_back(enginemon::SemanticTextElement::make_arg(id));
                break;
            }

            // ---------------------------------------------------------------
            // TX_RAM (0x01)
            // Source: home/text.asm TextCommand_RAM — reads 2-byte WRAM address,
            //   calls PlaceString from DE.
            //
            // In script-level text (maps/*.asm), TX_RAM appears ONLY for:
            //   wStringBuffer3 (0xD099) → Arg(slot=0)
            //   wStringBuffer4 (0xD0AC) → Arg(slot=1)
            //   wStringBuffer5 (0xD0BF) → Arg(slot=2)
            // These are the strbuf=0/1/2 slots used by GetStringBuffer.
            //
            // Source: pokecrystal/engine/overworld/scripting.asm GetStringBuffer:
            //   ld hl, wStringBuffer3
            //   ld bc, STRING_BUFFER_LENGTH   ; 19
            //   call AddNTimes
            //   → strbuf=0 → wStringBuffer3, strbuf=1 → wStringBuffer4, strbuf=2 → wStringBuffer5
            //
            // Any other address is not a valid script text buffer slot.
            // Hard-fail: unknown TX_RAM address is a semantic error.
            //
            // wStringBuffer addresses (from pokecrystal11.sym):
            //   wStringBuffer3 = 0xD099   wStringBuffer4 = 0xD0AC   wStringBuffer5 = 0xD0BF
            //
            // Note: wStringBuffer1 (0xD073) and wStringBuffer2 (0xD086) appear only
            // in battle text / mobile paths, not in map script text sequences.
            // ---------------------------------------------------------------
            case TextOp::TextRam: {
                // TX_RAM WRAM address → typed semantic text source.
                //
                // TWO distinct domains:
                //
                // Domain A — GetStringBuffer destination slots (Arg):
                //   These are populated by a preceding Sem_PrepareTextArg(buffer_slot=N).
                //   GetStringBuffer: ld hl, wStringBuffer3; AddNTimes(strbuf) → copy.
                //   strbuf=0 → wStringBuffer3 (0xD099)  → Arg(0)
                //   strbuf=1 → wStringBuffer4 (0xD0AC)  → Arg(1)
                //   strbuf=2 → wStringBuffer5 (0xD0BF)  → Arg(2)
                //   NUM_STRING_BUFFERS = 3; no strbuf > 2 reaches here via GetStringBuffer.
                //
                // Domain B — Direct WRAM reads (RamSource):
                //   These buffers are NOT GetStringBuffer destinations.
                //   No Sem_PrepareTextArg produces buffer_slot 3/4/5/6 in vanilla Crystal.
                //   They are read directly by TX_RAM in text bodies (often via TX_FAR expansion).
                //   Source: StringBufferPointers[3..6] + corpus TX_RAM analysis.
                //   wStringBuffer2  (0xD086) → RamSource(PreparedString2)
                //   wStringBuffer1  (0xD073) → RamSource(PreparedString1)
                //   wEnemyMonNickname (0xC616) → RamSource(EnemyNickname)
                //   wBattleMonNickname (0xC621) → RamSource(BattleNickname)
                //
                // Addresses from pokecrystal11.sym (authoritative):
                //   wStringBuffer1=0xD073  wStringBuffer2=0xD086
                //   wStringBuffer3=0xD099  wStringBuffer4=0xD0AC  wStringBuffer5=0xD0BF
                //   wEnemyMonNickname=0xC616  wBattleMonNickname=0xC621
                constexpr uint16_t WSTRINGBUFFER1     = 0xD073;
                constexpr uint16_t WSTRINGBUFFER2     = 0xD086;
                constexpr uint16_t WSTRINGBUFFER3     = 0xD099;
                constexpr uint16_t WSTRINGBUFFER4     = 0xD0AC;
                constexpr uint16_t WSTRINGBUFFER5     = 0xD0BF;
                constexpr uint16_t WENEMYMONNICKNAME  = 0xC616;
                constexpr uint16_t WBATTLEMONNICKNAME = 0xC621;
                switch (elem.addr) {
                    // Domain A: GetStringBuffer destination slots → Arg(slot)
                    case WSTRINGBUFFER3:
                        sem.elements.push_back(enginemon::SemanticTextElement::make_arg(0));
                        break;
                    case WSTRINGBUFFER4:
                        sem.elements.push_back(enginemon::SemanticTextElement::make_arg(1));
                        break;
                    case WSTRINGBUFFER5:
                        sem.elements.push_back(enginemon::SemanticTextElement::make_arg(2));
                        break;
                    // Domain B: direct WRAM reads → RamSource(typed identity)
                    case WSTRINGBUFFER2:
                        sem.elements.push_back(
                            enginemon::SemanticTextElement::make_ram_source(
                                enginemon::TextRamSource::PreparedString2));
                        break;
                    case WSTRINGBUFFER1:
                        sem.elements.push_back(
                            enginemon::SemanticTextElement::make_ram_source(
                                enginemon::TextRamSource::PreparedString1));
                        break;
                    case WENEMYMONNICKNAME:
                        sem.elements.push_back(
                            enginemon::SemanticTextElement::make_ram_source(
                                enginemon::TextRamSource::EnemyNickname));
                        break;
                    case WBATTLEMONNICKNAME:
                        sem.elements.push_back(
                            enginemon::SemanticTextElement::make_ram_source(
                                enginemon::TextRamSource::BattleNickname));
                        break;
                    default:
                        // Unknown TX_RAM address — not a classified text source.
                        // Hard-fail: the legality gate will reject the empty sequence.
                        return enginemon::SemanticTextSequence{};
                }
                break;
            }

            // ---------------------------------------------------------------
            // TX_BCD (0x02)
            // Source: home/text.asm TextCommand_BCD
            //
            // The text_bcd macro is NEVER used in vanilla Crystal map/script text.
            // TX_BCD appears only in battle text engine internals (home/text.asm)
            // that are not processed through to_semantic_sequence().
            //
            // Hard-fail: any TX_BCD in a script text sequence is unexpected and
            // has no valid semantic form in the current model.
            // ---------------------------------------------------------------
            case TextOp::TextBcd:
                return enginemon::SemanticTextSequence{};

            // ---------------------------------------------------------------
            // TX_DECIMAL (0x09)
            // Source: home/text.asm TextCommand_DECIMAL
            //   high nibble of param = byte width (number of bytes to read)
            //   low nibble of param  = digit count
            //
            // Vanilla stock uses: exactly 2 occurrences, both in BattleTower1F.asm:
            //   text_decimal wScriptVar, 1, 3   → addr=0xC2DD, param=0x13
            //
            // Only wScriptVar (0xC2DD) is valid. Any other address hard-fails.
            // The semantic form is ScriptVarDecimal(bytes_digits) — no address
            // survives into Semantic IR. The runtime always reads ScriptVar context.
            // ---------------------------------------------------------------
            case TextOp::TextDecimal: {
                constexpr uint16_t WSCRIPTVAR = 0xC2DD;
                if (elem.addr != WSCRIPTVAR) {
                    // Unknown TX_DECIMAL source — hard-fail.
                    return enginemon::SemanticTextSequence{};
                }
                sem.elements.push_back(
                    enginemon::SemanticTextElement::make_script_var_decimal(elem.param1));
                break;
            }

            // ---------------------------------------------------------------
            // TX_FAR (0x16)
            // Source: home/text.asm TextCommand_FAR — saves ROM bank, switches
            //   to far bank, calls DoTextUntilTerminator recursively in that bank,
            //   then restores the original bank.
            //
            // Semantic model: TX_FAR inlines the referenced text's elements at
            // this position. The flat ROM address is frontend-only evidence and
            // must NOT survive into Semantic IR.
            //
            // Resolution path:
            //   elem.addr (local ptr) + elem.param2 (bank)
            //   → flat address via crystal_bank_to_flat()
            //   → TextRegistry::extract() / get()
            //   → referenced TextDefinition::to_semantic_sequence() (recursive)
            //   → inline elements into parent sequence
            //
            // Hard-fail conditions:
            //   - registry == nullptr (no registry available)
            //   - far_depth >= MAX_FAR_DEPTH (recursion guard)
            //   - Registry returns TEXT_NONE (failed to extract text)
            //   - Referenced text's to_semantic_sequence() returns empty
            // ---------------------------------------------------------------
            case TextOp::TextFar: {
                if (!registry || far_depth >= MAX_FAR_DEPTH) {
                    return enginemon::SemanticTextSequence{};
                }
                // Resolve bank:local to flat address
                const uint32_t bank      = elem.param2;
                const uint16_t local_ptr = elem.addr;
                const uint32_t flat = (local_ptr < 0x4000u)
                    ? local_ptr
                    : bank * 0x4000u + (local_ptr - 0x4000u);
                auto text_id = registry->lookup(flat);
                if (!text_id.has_value() || text_id.value() == enginemon::TEXT_NONE) {
                    // Not yet in registry — attempt extraction
                    auto extracted_id = const_cast<TextRegistry*>(registry)->extract(flat);
                    if (extracted_id == enginemon::TEXT_NONE) {
                        return enginemon::SemanticTextSequence{};
                    }
                    text_id = extracted_id;
                }
                const auto* far_def = registry->get(text_id.value());
                if (!far_def) {
                    return enginemon::SemanticTextSequence{};
                }
                auto far_sem = far_def->to_semantic_sequence(registry, far_depth + 1);
                if (far_sem.empty()) {
                    return enginemon::SemanticTextSequence{};
                }
                inline_seq(sem, far_sem);
                break;
            }

            // ---------------------------------------------------------------
            // TX_DAY (0x15): display current day of week.
            // No operands. Runtime queries calendar. Clean semantic.
            // ---------------------------------------------------------------
            case TextOp::TextDay:
                sem.elements.push_back(enginemon::SemanticTextElement::make_day());
                break;

            // ---------------------------------------------------------------
            // Text sound effects.
            // Source-proven from text.asm const declarations:
            //   TX_SOUND_ITEM (0x0f)         → TextSoundKind::ItemJingle
            //   TX_SOUND_CAUGHT_MON (0x10)   → TextSoundKind::CaughtMonJingle
            //   TX_SOUND_FANFARE (0x12)       → TextSoundKind::Fanfare
            // No raw opcode survives — TextSoundKind is the semantic identity.
            // ---------------------------------------------------------------
            case TextOp::TextSoundItem:
                sem.elements.push_back(
                    enginemon::SemanticTextElement::make_sound(
                        enginemon::TextSoundKind::ItemJingle));
                break;
            case TextOp::TextSoundCaught:
                sem.elements.push_back(
                    enginemon::SemanticTextElement::make_sound(
                        enginemon::TextSoundKind::CaughtMonJingle));
                break;
            case TextOp::TextSoundFanfare:
                sem.elements.push_back(
                    enginemon::SemanticTextElement::make_sound(
                        enginemon::TextSoundKind::Fanfare));
                break;

            // ---------------------------------------------------------------
            // TX_PROMPT_BUTTON (0x06) — non-terminating in-stream input gate.
            // Source: home/text.asm TextCommand_PROMPT_BUTTON:
            //   LoadBlinkingCursor → PromptButton (waits A/B) → UnloadBlinkingCursor
            //   (In link-battle modes: falls through to WAIT_BUTTON, same effect)
            //
            // DISTINCT from Prompt (which terminates the text stream):
            //   This waits for input, then text processing CONTINUES.
            //
            // Corpus-reachable: 11 occurrences in data/text/ and maps/.
            //   Including maps/BattleTower1F.asm (Battle Tower prize text),
            //   data/text/common_1.asm (level-up, item-receive),
            //   data/text/common_3.asm (NPC trade fanfare text), etc.
            // ---------------------------------------------------------------
            case TextOp::TextPromptButton:
                sem.elements.push_back(
                    enginemon::SemanticTextElement::make_inline_prompt_button());
                break;

            // ---------------------------------------------------------------
            // TX_PAUSE (0x0a) — timed/button-skip delay between text elements.
            // Source: home/text.asm TextCommand_PAUSE:
            //   GetJoypad; if A|B held → immediate return (0 frames)
            //   else DelayFrames(30)   → ~0.5 second wait at 60fps
            //
            // Observable behavior: ~0.5s pause with button-skip.
            // Used for dramatic pacing (Radio Tower, Lucky Channel countdown,
            // level-up move-learning, NPC trade fanfare).
            //
            // Corpus-reachable: 12 occurrences in data/text/ reachable from
            //   overworld scripts. Dropping it silently removes intended pacing.
            //
            // frames = 30 for all vanilla Crystal occurrences (preserved explicitly).
            // ---------------------------------------------------------------
            case TextOp::TextPause:
                // Source-proven: all vanilla Crystal uses have the same 30-frame delay.
                sem.elements.push_back(
                    enginemon::SemanticTextElement::make_pause(30));
                break;

            // ---------------------------------------------------------------
            // Confirmed presentation-only TX commands — safe to drop.
            // Source-proven: 0 macro call sites in text data corpus for each.
            //
            //   TX_MOVE (0x03): repositions text cursor (BC register only)
            //   TX_BOX  (0x04): draws a bordered textbox at tilemap coordinate
            //   TX_LOW  (0x05): moves cursor to bottom row (3 uses: HM item text)
            //   TX_SCROLL (0x07): visual scroll (0 corpus uses)
            //   TX_START_ASM (0x08): 0 uses in maps/data/ corpus
            //
            // TX_LOW has 3 corpus-reachable uses (_ItemUsedText etc.) but only
            // repositions the print cursor — the text content itself is preserved
            // and renders correctly at any cursor position on a modern renderer.
            // ---------------------------------------------------------------
            case TextOp::TextMove:
            case TextOp::TextBox:
            case TextOp::TextLow:
            case TextOp::TextScroll:
            case TextOp::TextAsm:
                // Intentionally dropped — confirmed 0 semantic content.
                break;

            // ---------------------------------------------------------------
            // TextRaw: lossless round-trip container for unrecognized TX opcodes.
            //
            // Any unrecognized TX opcode reaching this path is an error.
            // Hard-fail: the legality gate rejects the resulting empty sequence.
            // Unknown opcodes must not silently pass as empty text.
            // ---------------------------------------------------------------
            case TextOp::TextRaw:
                // Unrecognized text command — hard-fail.
                return enginemon::SemanticTextSequence{};
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
