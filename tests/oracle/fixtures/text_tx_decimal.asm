; text_tx_decimal.asm
; Oracle fixture: TX_DECIMAL operand layout
;
; Historical bug: TX_DECIMAL operands were misread — the bytes|digits nibble
; pack was treated as two separate bytes, or the address was read wrong-endian.
;
; TX_DECIMAL (opcode 0x09):
;   dw address     (2 bytes, little-endian)
;   dn bytes|digits (1 byte, upper nibble = number of bytes to read,
;                            lower nibble = number of digits to display)
;
; Asymmetric values (chosen so no two bytes are equal):
;   addr = 0xD109  (lo=0x09, hi=0xD1)
;   bytes|digits = 0x12  (1 byte to read, 2 digits to display)
;
; AUTHORITATIVE byte sequence:
;   09 09 D1 12 57
;
; Note: lo=0x09 is the same as the TX_DECIMAL opcode byte (0x09). This is
; intentional — a decoder that fails to advance past the opcode correctly
; would re-read the opcode as the address low byte, which is a detectable error
; because 0x09 != 0xD1 (the expected high byte would be 0xD1, not some other value).
;
; Oracle asserts:
;   elements[0]: op=TextDecimal, addr=0xD109, param1=0x12 (bytes|digits)
;   elements[1]: op=Done

SECTION "fixture", ROM0[$0000]

    db $09, $09, $D1, $12   ; TX_DECIMAL addr=$D109 (lo=$09, hi=$D1), bytes|digits=$12
    db $57                   ; DONE
