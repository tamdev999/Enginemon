; movement_parameterized_family.asm
; Oracle fixture: step_dig (0x4F) with param byte
; step_wait_end(0x48)/return_dig(0x58) are terminators - excluded
; Movement: 4F 09 step_dig param=9 | 47 step_end

SECTION "script", ROM0[$0000]
    db $69, $01, $0A, $00
    db $91

SECTION "pad", ROM0[$0005]
    ds 5, $00

SECTION "movement", ROM0[$000A]
    db $4F, $09
    db $47
