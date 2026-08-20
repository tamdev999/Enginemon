; text_tx_box_and_bcd.asm
; Oracle fixture: TX_BOX (height/width order) and TX_BCD (addr + flags)
;
; TX_BOX (opcode 0x04): draws a text box
;   Operands: dw address, db height, db width
;   Crystal source: home/text.asm TextCommand_BOX
;   NOTE: height comes FIRST, then width — this is the corrected order
;         (historical bug had them transposed: param1=width, param2=height)
;   Fixture: addr=0xC000 height=4 width=18 (0x12)
;   Bytes: 04 00 C0 04 12
;
; TX_BCD (opcode 0x02): display BCD-encoded number from RAM
;   Operands: dw address, db flags
;   Fixture: addr=0xD150 flags=0x01
;   Bytes: 02 50 D1 01
;
; Both followed by DONE (0x57).
;
; Asymmetric address design:
;   TX_BOX addr=0xC000: lo=0x00 != hi=0xC0 (detect byte-swap)
;   TX_BCD addr=0xD150: lo=0x50 != hi=0xD1 (detect byte-swap)
;   height=4 != width=18 (detect param transpose)
;
; Source authority:
;   TX_BOX:  home/text.asm TextCommand_BOX — "push bc" uses B=height, C=width
;            Macro: TX_BOX \1, \2, \3 → dw \1, db \2, db \3 (addr, height, width)
;   TX_BCD:  macros/scripts/text.asm TX_BCD → dw \1, db \2

SECTION "fixture", ROM0[$0000]
    db $04, $00, $C0, $04, $12  ; TX_BOX addr=0xC000 height=4 width=18
    db $02, $50, $D1, $01       ; TX_BCD addr=0xD150 flags=0x01
    db $57                       ; DONE
