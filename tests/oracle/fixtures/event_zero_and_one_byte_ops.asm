; event_zero_and_one_byte_ops.asm
; Oracle fixture: zero-operand and single-byte-operand script commands
;
; Covers: commands whose only operand (if any) is a single byte.
; Tests that PC advances exactly one byte for 0-op commands and
; exactly two bytes (opcode + operand) for 1-byte commands.
;
; Byte layout (script sequence starting at $0000):
;
;   $37              ; wildon  — 0 operands
;   $38              ; wildoff — 0 operands
;   $47              ; opentext — 0 operands  (0x47)
;   $49              ; closetext — 0 operands (0x49)
;   $54              ; waitbutton — 0 operands
;   $15 $2A          ; setval  value=42  (0x15 = setval, 0x2A = 42)
;   $16 $0F          ; addval  value=15
;   $17 $1E          ; random  range=30
;   $14 $07          ; setscene scene=7
;   $8B $28          ; pause   length=40 (0x28)
;   $91              ; end
;
; Asymmetric values chosen so no operand equals its opcode.
; Total: 11 bytes of script commands + 1 end = 22 bytes.
;
; Source authority: pokecrystal/macros/scripts/events.asm
;   wildon/wildoff/opentext/closetext/waitbutton = no operands
;   setval \1: db setval_command, \1
;   addval \1: db addval_command, \1
;   random \1: db random_command, \1
;   setscene \1: db setscene_command, \1
;   pause \1: db pause_command, \1
;
; Opcode reference:
;   0x37 = wildon
;   0x38 = wildoff
;   0x47 = opentext
;   0x49 = closetext
;   0x54 = waitbutton
;   0x15 = setval
;   0x16 = addval
;   0x17 = random
;   0x14 = setscene
;   0x8B = pause
;   0x91 = end

SECTION "fixture", ROM0[$0000]
    db $37              ; wildon
    db $38              ; wildoff
    db $47              ; opentext
    db $49              ; closetext
    db $54              ; waitbutton
    db $15, $2A         ; setval value=42
    db $16, $0F         ; addval value=15
    db $17, $1E         ; random range=30
    db $14, $07         ; setscene scene=7
    db $8B, $28         ; pause length=40
    db $91              ; end
