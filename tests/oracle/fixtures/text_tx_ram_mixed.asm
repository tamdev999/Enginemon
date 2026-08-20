; text_tx_ram_mixed.asm
; Oracle fixture: TX_RAM command interleaved in literal text
;
; Historical bug: TX_RAM (opcode 0x01) was not recognized in the outer-command
; (literal body) parsing mode, or was confused with character 0x01 in the
; Crystal charmap, causing it to be interpreted as literal text data.
;
; The asymmetric design: the address 0xD47E is chosen so that:
;   lo byte = 0x7E != 0xD4 hi byte  (not a palindrome)
;   0x7E is a valid Crystal character code (→ 'd' / some glyph), so a
;   mode-confused decoder that reads it as character data would produce
;   wrong results rather than silently passing.
;
; This fixture is text-sequence data, NOT a script sequence.
; It must be decoded by ScriptDecoder::decode_text_sequence().
;
; Crystal charmap (from pokecrystal/charmap.asm):
;   'H' = $87
;   'i' = $96
;
; Byte layout (text sequence starting at address 0x0000):
;   01 7E D4   ; TX_RAM addr=0xD47E  (lo=0x7E, hi=0xD4)
;   87 96      ; literal "Hi" (H=$87, i=$96)
;   57         ; DONE
;
; Oracle asserts:
;   elements[0]: op=TextRam, addr=0xD47E
;   elements[1]: op=Text, text contains 'H' and 'i' (decoded from $87 $96)
;   elements[2]: op=Done

SECTION "fixture", ROM0[$0000]

    db $01, $7E, $D4   ; TX_RAM addr=$D47E (lo, hi)
    db $87, $96        ; literal "Hi" (Crystal charmap H=$87, i=$96)
    db $57             ; DONE
