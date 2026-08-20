; movement_skyfall_top.asm
; Oracle fixture: skyfall_top terminal movement command
;
; Historical bug: skyfall_top (0x59) was not recognized as a terminal command.
; Decoders either silently degraded it to StepEnd, continued reading past it
; and consumed the following byte as another movement command, or threw a
; "movement opcode out of range" error because it was added in a later fix pass.
;
; skyfall_top (0x59): terminal — ends the movement sequence with no parameter.
; There is no step_end (0x47) after it — the sequence terminates at 0x59.
;
; ROM layout:
; Script section at $0000:
;   69 01 0A 00   ; applymovement object=1, movement_ptr=$000A
;   91            ; end
; Movement section at $000A:
;   59            ; skyfall_top (terminal — no step_end follows)
;
; AUTHORITATIVE byte sequence (full fixture ROM):
;   Index: 00 01 02 03 04 05 06 07 08 09  0A
;   Bytes: 69 01 0A 00 91 00 00 00 00 00  59
;
; Oracle asserts (after decoding applymovement):
;   cmd.object_id == 1
;   cmd.commands contains exactly:
;     [0]: type=SkyfallTop
;   No step_end present (skyfall_top is its own terminal).

SECTION "script",  ROM0[$0000]
    db $69, $01, $0A, $00   ; applymovement object=1, ptr=$000A
    db $91                   ; end

SECTION "pad1", ROM0[$0005]
    ds 5, $00                ; padding bytes $0005-$0009

SECTION "movement", ROM0[$000A]
    db $59                   ; skyfall_top (terminal)
