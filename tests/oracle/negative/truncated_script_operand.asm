; truncated_script_operand.asm
; Negative fixture: script command with truncated operand
;
; playmusic (0x7F) requires 2 operand bytes (16-bit music ID).
; This fixture provides only 1 operand byte before ending the ROM.
; The decoder must not silently accept this with music=0x00XY from padding.
;
; Behavior expected: decoder reads byte at offset 2 which is 0xFF (uninitialized
; ROM fill — the ROM is exactly 3 bytes, so any read past byte 2 is out of bounds).
; The test verifies that the decoded music ID is NOT the valid fixture value
; 0x1234 (which would require both operand bytes to be present).
;
; NOTE: Unlike movement opcodes, script truncation in practice leads to
; "garbage" operands from ROM padding rather than a hard throw, because the
; RomData bounds-checked read can still succeed with padding. The oracle
; verifies the decoded value is NOT the intended value — proving truncation
; is detectable via operand mismatch.
;
; Fixture bytes: 7F 34   (playmusic with only lo byte present)
; Expected: decoder reads hi byte from pad/garbage, music != 0x1234

SECTION "fixture", ROM0[$0000]
    db $7F, $34         ; playmusic — ONLY lo byte present (hi byte missing → truncated)
