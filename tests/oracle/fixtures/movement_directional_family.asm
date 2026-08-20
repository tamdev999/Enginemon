; movement_directional_family.asm
; Oracle fixture: directional movement commands (TurnHead/SlowStep/Step families)
;
; Directional movement opcodes encode direction in bits 1:0 of the opcode byte.
; Each family of 4 opcode bytes covers all 4 directions in order:
;   +0 = Down, +1 = Up, +2 = Left, +3 = Right
;
; TurnHead family: opcodes 0x00-0x03 (turn to face, no step)
;   0x00 = turn_head_down, 0x01 = turn_head_up
;   0x02 = turn_head_left, 0x03 = turn_head_right
;
; SlowStep family: opcodes 0x08-0x0B (slow walk)
;   0x08 = slow_step_down, 0x09 = slow_step_up
;
; Step family: opcodes 0x0C-0x0F (normal walk)
;   0x0C = step_down, 0x0D = step_up
;   0x0E = step_left, 0x0F = step_right
;
; None of these take a parameter byte — they are single-byte commands.
; A decoder that wrongly reads a parameter byte would consume the next
; movement opcode as data, causing a type mismatch.
;
; Script section: applymovement object=1, movement_ptr=$000A
; Movement section at $000A:
;   0x00  turn_head_down   (TurnHead, Down)
;   0x01  turn_head_up     (TurnHead, Up)
;   0x02  turn_head_left   (TurnHead, Left)
;   0x03  turn_head_right  (TurnHead, Right)
;   0x08  slow_step_down   (SlowStep, Down)
;   0x09  slow_step_up     (SlowStep, Up)
;   0x0C  step_down        (Step, Down)
;   0x0D  step_up          (Step, Up)
;   0x0E  step_left        (Step, Left)
;   0x0F  step_right       (Step, Right)
;   0x47  step_end
;
; Source authority: pokecrystal/macros/scripts/movement.asm
;   turn_head_down = $00, turn_head_up = $01, turn_head_left = $02, turn_head_right = $03
;   slow_step_down = $08, slow_step_up = $09
;   step_down = $0C, step_up = $0D, step_left = $0E, step_right = $0F
;   step_end = $47

SECTION "script", ROM0[$0000]
    db $69, $01, $0A, $00   ; applymovement object=1, ptr=$000A
    db $91                   ; end

SECTION "pad", ROM0[$0005]
    ds 5, $00                ; padding to $000A

SECTION "movement", ROM0[$000A]
    db $00                   ; turn_head_down
    db $01                   ; turn_head_up
    db $02                   ; turn_head_left
    db $03                   ; turn_head_right
    db $08                   ; slow_step_down
    db $09                   ; slow_step_up
    db $0C                   ; step_down
    db $0D                   ; step_up
    db $0E                   ; step_left
    db $0F                   ; step_right
    db $47                   ; step_end
