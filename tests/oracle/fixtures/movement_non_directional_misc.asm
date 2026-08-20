; movement_non_directional_misc.asm
; Oracle fixture: non-directional 0-param non-terminal movement commands
; remove_object(0x49) is a TERMINATOR - excluded
; Movement: 3C show_object | 3D hide_object | 4E skyfall | 50 step_bump | 47 step_end

SECTION "script", ROM0[$0000]
    db $69, $02, $0A, $00
    db $91

SECTION "pad", ROM0[$0005]
    ds 5, $00

SECTION "movement", ROM0[$000A]
    db $3C
    db $3D
    db $4E
    db $50
    db $47
