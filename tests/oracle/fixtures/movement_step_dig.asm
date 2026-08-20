; movement_step_dig.asm
; Oracle fixture: step_dig movement command with parameter byte
;
; Historical bug: step_dig (0x4F) was decoded without consuming its length
; parameter byte, causing the parameter to be misinterpreted as the next
; movement opcode.
;
; step_dig (0x4F): requires 1 length parameter byte (number of dig frames).
; step_end (0x47): terminates the movement sequence, no parameter.
;
; Asymmetric value: param=7 (not 1, not 0 — catches off-by-one and zero cases).
;
; ROM layout:
; Script section at $0000:
;   69 02 0A 00   ; applymovement object=2, movement_ptr=$000A (little-endian)
;   91            ; end
; [padding to $000A]
; Movement section at $000A:
;   4F 07         ; step_dig length=7
;   47            ; step_end
;
; AUTHORITATIVE byte sequence (full fixture ROM):
;   Index: 00 01 02 03 04 05 06 07 08 09  0A 0B 0C
;   Bytes: 69 02 0A 00 91 00 00 00 00 00  4F 07 47
;
; Oracle asserts (after decoding applymovement):
;   cmd.object_id == 2
;   cmd.commands contains:
;     [0]: type=StepDig, param=7
;     [1]: type=StepEnd

SECTION "script",  ROM0[$0000]
    db $69, $02, $0A, $00   ; applymovement object=2, ptr=$000A
    db $91                   ; end

SECTION "pad1", ROM0[$0005]
    ds 5, $00                ; padding bytes $0005-$0009

SECTION "movement", ROM0[$000A]
    db $4F, $07              ; step_dig param=7
    db $47                   ; step_end
