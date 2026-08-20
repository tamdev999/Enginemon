; event_word_operand_ops.asm
; Oracle fixture: 16-bit word operand commands (data-carrying only)
; jumpstd(0x0C)/special(0x0F) are jump/terminal types - excluded
; Bytes: 7F 34 12 | 85 78 56 | 84 BC 9A | 25 F0 00 | 26 10 01 | 27 80 00 | 91

SECTION "fixture", ROM0[$0000]
    db $7F, $34, $12
    db $85, $78, $56
    db $84, $BC, $9A
    db $25, $F0, $00
    db $26, $10, $01
    db $27, $80, $00
    db $91
