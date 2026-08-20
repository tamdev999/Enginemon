; connection_offset_direction.asm
; Oracle fixture: Crystal map connection direction-dependent offset byte selection
;
; Historical bug: MapExtractor::extract_connections() must select a DIFFERENT
; offset byte depending on the connection direction:
;   North/South → data[9] (X offset along the X axis)
;   East/West   → data[8] (Y offset along the Y axis)
; The old bug selected the wrong byte (data[8] for all directions, or
; data[9] for all directions), making the strip_offset wrong for half the cases.
;
; Source authority:
;   pokecrystal/data/maps/attributes.asm — connection macro definition
;   pokecrystal/macros/scripts/maps.asm  — connection macro arguments
;
; Connection record format (12 bytes, from connection macro):
;   db target_group              ; byte 0
;   db target_map                ; byte 1
;   dw blocks_ptr_offset         ; bytes 2-3 (source map block pointer offset)
;   dw map_ptr_offset            ; bytes 4-5 (target map block pointer offset)
;   db strip_length              ; byte 6  (_len - _src)
;   db target_width              ; byte 7
;   db y_offset                  ; byte 8  (_y) — for East/West strip_offset
;   db x_offset                  ; byte 9  (_x) — for North/South strip_offset
;   dw window_ptr_offset         ; bytes 10-11
;
; Asymmetric design:
;   North record: data[8]=0x11, data[9]=0xAB (distinct, non-palindrome)
;   East  record: data[8]=0xCD, data[9]=0x22 (distinct, non-palindrome)
;
; Oracle asserts (after calling extract_connections with conn_byte=0x09 = North+East):
;   connections[0] (North): strip_offset == -85 == (int8_t)0xAB   [= data[9], x offset]
;     NOT 17 == (int8_t)0x11 which would indicate wrong byte data[8] was used
;
;   connections[1] (East):  strip_offset == -51 == (int8_t)0xCD   [= data[8], y offset]
;     NOT 34 == (int8_t)0x22 which would indicate wrong byte data[9] was used
;
; The two connection records are placed sequentially starting at byte 0.
; The test calls extract_connections with:
;   map_attr_addr = 0  (header_size subtracted by extractor → conn_ptr = conn data start)
;   conn_byte = 0x09   (bits: NORTH(0x08) | EAST(0x01))
; but because we're calling via a mock ROM and the extractor computes
; conn_ptr = map_attr_addr + fmt.header_size (12), we place dummy header
; bytes first (12 zero bytes), then the two connection records.

SECTION "header_pad", ROM0[$0000]
    ds 12, $00              ; 12-byte dummy map attributes header (not parsed by fixture test)

SECTION "north_connection", ROM0[$000C]
    db $1A, $03             ; target: group=0x1A, map=0x03
    dw $0000                ; blocks ptr offset (source)
    dw $0000                ; blocks ptr offset (target)
    db $08                  ; strip length
    db $14                  ; target width
    db $11                  ; y offset (data[8]) — used by E/W, NOT by N/S
    db $AB                  ; x offset (data[9]) — used by N/S → expected strip_offset=-85
    dw $0000                ; window ptr offset

SECTION "east_connection", ROM0[$0018]
    db $18, $04             ; target: group=0x18, map=0x04 (new_bark_town)
    dw $0000                ; blocks ptr offset (source)
    dw $0000                ; blocks ptr offset (target)
    db $09                  ; strip length
    db $14                  ; target width
    db $CD                  ; y offset (data[8]) — used by E/W → expected strip_offset=-51
    db $22                  ; x offset (data[9]) — used by N/S, NOT by E/W
    dw $0000                ; window ptr offset
