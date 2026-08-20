; text_tx_stringbuffer_and_far.asm
; Oracle fixture: TX_STRINGBUFFER and TX_FAR commands
;
; TX_STRINGBUFFER (opcode 0x14): display contents of a string buffer
;   Operand: db buffer_id
;   Fixture: buffer_id=2
;   Bytes: 14 02
;   NOTE: 0x14 is ONLY valid as a TX command when inside literal/body mode.
;         In outer script mode, 0x14 = setscene. This fixture is text data
;         decoded by decode_text_sequence(), NOT the script decoder.
;
; TX_FAR (opcode 0x16): far text pointer
;   Operands: dw address, db bank
;   Fixture: addr=0x4200 bank=0x3E
;   Bytes: 16 00 42 3E
;   HISTORICAL BUG: early version used param1=bank instead of param2=bank,
;   and used ptr=0 instead of actual pointer. The correct identity is
;   <FAR:bank,addr> using param2=bank and addr=actual_ptr.
;
; Followed by literal text "AB" (Crystal charmap: A=$80, B=$81) then DONE.
; The literal bytes 0x80 and 0x81 are distinct from all TX opcodes (0x01-0x16)
; so they should decode as Text, not as commands.
;
; Byte sequence:
;   14 02          TX_STRINGBUFFER buffer_id=2
;   16 00 42 3E    TX_FAR addr=0x4200 bank=0x3E
;   80 81          literal "AB" (A=$80 in Crystal charmap, B=$81)
;   57             DONE
;
; Source authority:
;   TX_STRINGBUFFER: macros/scripts/text.asm — TX_STRINGBUFFER \1 → db $14, \1
;   TX_FAR:          macros/scripts/text.asm — TX_FAR \1, \2 → db $16, LOW(\1), HIGH(\1), \2
;                    where \1 = address, \2 = bank

SECTION "fixture", ROM0[$0000]
    db $14, $02             ; TX_STRINGBUFFER buffer_id=2
    db $16, $00, $42, $3E   ; TX_FAR addr=0x4200 bank=0x3E
    db $80, $81             ; literal "AB" (Crystal charmap)
    db $57                  ; DONE
