; text_literal_overlap_opcodes.asm
; Oracle fixture: literal text bytes that overlap with TX opcode values
;
; Crystal charmap bytes can coincide with TX opcode values.
; This tests that the parser correctly distinguishes outer-command mode
; (where 0x01-0x16 are TX commands) from continuation inside a text run.
;
; The fixture exercises the boundary between TX_RAM (0x01) at the start,
; followed by literal text containing byte 0x57 which is ALSO the DONE code.
; A correct parser sees DONE (0x57) as the terminator, not as a character.
; A broken mode-collapse parser might treat the second 0x57 as character data.
;
; Also includes TX_SCROLL (0x07) which is a single-byte TX command followed
; by a literal text byte (0x80='A') to prove no parameter consumption.
;
; Byte sequence:
;   01 3E D1         TX_RAM addr=0xD13E (lo=$3E, hi=$D1)
;   80               literal 'A' (Crystal charmap $80)
;   07               TX_SCROLL (single-byte TX command)
;   81               literal 'B' (Crystal charmap $81)
;   4F               LINE  (0x4F flow control, continues same text box)
;   82               literal 'C' (Crystal charmap $82)
;   57               DONE
;
; Key oracle assertions:
;   elements[0]: TextRam, addr=0xD13E
;   elements[1]: Text containing 'A'
;   elements[2]: Scroll (TX_SCROLL recognized as flow command)
;   elements[3]: Text containing 'B'
;   elements[4]: Line  (0x4F flow control)
;   elements[5]: Text containing 'C'
;   elements[6]: Done
;
; The byte 0x57 MUST be recognized as DONE, not as a charmap character.
; The byte 0x07 MUST be recognized as TX_SCROLL, not a charmap character.
; The byte 0x4F MUST be recognized as LINE, not a charmap character.
;
; Source authority:
;   TX_RAM:    macros/scripts/text.asm opcode 0x01
;   TX_SCROLL: macros/scripts/text.asm opcode 0x07 (single byte)
;   LINE:      home/text.asm control code 0x4F
;   DONE:      home/text.asm control code 0x57

SECTION "fixture", ROM0[$0000]
    db $01, $3E, $D1    ; TX_RAM addr=0xD13E
    db $80               ; literal 'A'
    db $07               ; TX_SCROLL (no param)
    db $81               ; literal 'B'
    db $4F               ; LINE
    db $82               ; literal 'C'
    db $57               ; DONE
