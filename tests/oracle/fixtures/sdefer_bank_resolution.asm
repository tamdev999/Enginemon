; sdefer_bank_resolution.asm
; Oracle fixture: sdefer bank-relative pointer → flat address resolution
;
; Historical bug: sdefer's 16-bit pointer is BANK-RELATIVE (local to the
; calling script's bank).  The flat address must be computed as:
;   flat = bank * 0x4000 + (local_ptr - 0x4000)   for local_ptr >= 0x4000
; Incorrect implementations treated local_ptr as a flat address directly,
; or used the wrong bank (e.g., bank 0 for ROM0).
;
; Asymmetric design: the fixture is placed at flat address 0x68100 (bank=0x1A=26)
; and uses local_ptr=0x5100. With correct resolution:
;   flat_target = 26 * 0x4000 + (0x5100 - 0x4000) = 0x68000 + 0x1100 = 0x69100
; With WRONG resolution (treating ptr as flat):
;   flat_target = 0x5100  (wrong — different bank region)
; These two values are far apart, making the fixture definitively asymmetric.
;
; sdefer opcode = 0x8D, dw local_ptr (little-endian)
;
; AUTHORITATIVE byte sequence:
;   8D 00 51   ; sdefer ptr=$5100 (lo=$00, hi=$51)
;   91         ; end
;
; Test must set the TypedScriptDecoder / legalizer entry_address = 0x68100
; so the bank derivation is: bank = 0x68100 / 0x4000 = 26 = 0x1A.
;
; Oracle asserts:
;   Sem_Sdefer.target_script_id == "deferred_69100"
;   (i.e., flat = 0x69100, expressed as hex string with no leading zeros beyond 5 digits)
;
; Mutation check: if wrong_bank=0 is used instead, flat = 0*0x4000 + (0x5100-0x4000) = 0x1100,
; producing target "deferred_1100" which does NOT equal "deferred_69100".

SECTION "fixture", ROM0[$0000]

    db $8D, $00, $51   ; sdefer local_ptr=$5100 (lo=$00, hi=$51)
    db $91              ; end
