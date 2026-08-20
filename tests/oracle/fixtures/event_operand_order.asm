; event_operand_order.asm
; Oracle fixture: gettrainername operand order
;
; Historical bug: gettrainername was decoded with strbuf first (macro arg order),
; but the ROM byte layout places trainer_group first, then trainer_id, then strbuf.
; Using identical values for any two operands would hide a swap — so we use
; three distinct asymmetric values: trainer_group=3, trainer_id=17, strbuf=201.
;
; ROM byte layout (Crystal macro: gettrainername \1, \2, \3):
;   db $43              ; gettrainername opcode
;   db \2               ; trainer_group (first in ROM, second in macro)
;   db \3               ; trainer_id    (second in ROM, third in macro)
;   db \1               ; strbuf        (third in ROM, first in macro)
;   db $91              ; end
;
; Source: pokecrystal/macros/scripts/events.asm
;   gettrainername MACRO
;     db gettrainername_command, \2, \3, \1
;   ENDM
;
; AUTHORITATIVE byte sequence (hand-derived from macro definition):
;   43 03 11 C9 91
;
; Oracle asserts:
;   cmd.trainer_group == 3    (first ROM byte after opcode)
;   cmd.trainer_id    == 17   (second ROM byte after opcode, decimal 0x11)
;   cmd.strbuf        == 201  (third ROM byte after opcode, decimal 0xC9)

SECTION "fixture", ROM0[$0000]

    db $43, $03, $11, $C9  ; gettrainername group=3, id=17, strbuf=201
    db $91                  ; end
