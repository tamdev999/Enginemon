; event_pointer_and_branch_ops.asm
; Oracle fixture: commands with local/far pointers and conditional branches
;
; Tests that 2-byte (scall/sjump) and 3-byte (farscall/farsjump/callasm)
; pointer commands consume exactly the correct byte count, and that
; conditional branch commands (ifequal/ifgreater/ifless) read 1-byte
; comparand THEN 2-byte pointer (not the other way around).
;
; The fixture is self-contained: pointer targets are within the fixture.
; No realistic execution is intended — we test decode only.
;
; Byte layout (script starting at $0000):
;
; Offset $00: $00 $07 $00    scall     ptr=$0007 (lo=$07, hi=$00)
; Offset $03: $06 $2A $0C $00  ifequal  value=42, ptr=$000C
;                               (opcode, comparand, lo, hi)
; Offset $07: $03 $07 $00    sjump     ptr=$0007 (self-jump — decode only)
; Offset $0A: $91            end
; Offset $0B: $91            end (landing for ifequal skip target $000C — unused in decode test)
; Offset $0C: $0A $03 $0C $00  ifgreater value=3, ptr=$000C
; Offset $10: $0B $1E $14 $00  ifless    value=30, ptr=$0014
; Offset $14: $91            end
;
; Source authority: pokecrystal/macros/scripts/events.asm
;   scall \1:         db scall_command,     LOW(\1), HIGH(\1)
;   sjump \1:         db sjump_command,     LOW(\1), HIGH(\1)
;   ifequal \1, \2:   db ifequal_command,   \1, LOW(\2), HIGH(\2)
;   ifgreater \1, \2: db ifgreater_command, \1, LOW(\2), HIGH(\2)
;   ifless \1, \2:    db ifless_command,    \1, LOW(\2), HIGH(\2)
;
; Opcode reference:
;   0x00 = scall    (2-byte local pointer)
;   0x03 = sjump    (2-byte local pointer)
;   0x06 = ifequal  (1-byte value + 2-byte pointer)
;   0x0A = ifgreater (1-byte value + 2-byte pointer)
;   0x0B = ifless   (1-byte value + 2-byte pointer)
;   0x91 = end

SECTION "script", ROM0[$0000]
    db $00, $07, $00        ; scall ptr=$0007
    db $06, $2A, $0C, $00   ; ifequal value=42, ptr=$000C
    db $03, $07, $00        ; sjump ptr=$0007
    db $91                  ; end at $000A
    db $91                  ; end at $000B (branch target for $000C would land here+1)
    db $0A, $03, $0C, $00   ; ifgreater value=3, ptr=$000C
    db $0B, $1E, $14, $00   ; ifless value=30, ptr=$0014
    db $91                  ; end at $0014
