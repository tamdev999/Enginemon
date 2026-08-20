; truncated_tx_operand.asm
; Negative fixture: TX command with truncated operand bytes
;
; TX_RAM (0x01) requires a 2-byte little-endian address.
; This fixture provides opcode 0x01 followed by only 1 byte (lo).
; The hi byte is missing — the sequence ends immediately after lo=0xAB.
;
; Expected behavior: the decoder reads lo=0xAB then reads 0xFF from ROM
; padding (the ROM is exactly 2 bytes), producing addr != 0xD4AB
; (which would require hi=0xD4). The oracle verifies the decoded addr
; does NOT match the intended value 0xD4AB, proving truncation is detectable.
;
; Fixture bytes: 01 AB   (TX_RAM with only lo byte present)

SECTION "fixture", ROM0[$0000]
    db $01, $AB         ; TX_RAM — only lo byte present (hi byte missing)
