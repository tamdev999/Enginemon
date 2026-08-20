; event_multi_byte_ops.asm
; Oracle fixture: commands with 3+ operand bytes (asymmetric values)
;
; Tests correct operand ordering and consumption for complex commands.
; Every operand uses a distinct value so transposition is detectable.
;
; Byte layout (script sequence starting at $0000):
;
;   $22 $01 $00 $10 $00  ; givemoney  account=1, BCD hi=$00, mid=$10, lo=$00
;                         ; amount = 0x001000 = 4096 (in BCD: $0010.00 = 10.00)
;   $1F $19 $03          ; giveitem   item=25 (Potion), quantity=3
;   $20 $19 $01          ; takeitem   item=25, quantity=1
;   $21 $2C              ; checkitem  item=44
;   $5E $02 $05          ; loadtrainer trainer_group=2, trainer_id=5
;   $72 $03 $07 $0B      ; moveobject object_id=3, x=7, y=11
;   $75 $02 $04 $14      ; showemote  bubble=2, object_id=4, time=20 (0x14)
;   $91                  ; end
;
; givemoney byte layout (pokecrystal/macros/scripts/events.asm):
;   db givemoney_command, \1, HIGH(\2 >> 8), HIGH(\2), LOW(\2)
;   \1 = account, \2 = amount in BCD
;   Fixture: account=1, hi=$00, mid=$10, lo=$00 → amount 0x001000
;
; giveitem / takeitem (events.asm):
;   db giveitem_command, \1, \2   (\1=item, \2=quantity)
;
; checkitem (events.asm):
;   db checkitem_command, \1
;
; loadtrainer (events.asm):
;   db loadtrainer_command, \1, \2  (\1=group, \2=id)
;
; moveobject (events.asm):
;   db moveobject_command, \1, \2, \3  (\1=object, \2=x, \3=y)
;
; showemote (events.asm):
;   db showemote_command, \1, \2, \3  (\1=bubble, \2=object, \3=time)
;
; Opcode reference:
;   0x22 = givemoney
;   0x1F = giveitem
;   0x20 = takeitem
;   0x21 = checkitem
;   0x5E = loadtrainer
;   0x72 = moveobject
;   0x75 = showemote
;   0x91 = end

SECTION "fixture", ROM0[$0000]
    db $22, $01, $00, $10, $00  ; givemoney account=1 amount=BCD($001000)
    db $1F, $19, $03            ; giveitem item=25 qty=3
    db $20, $19, $01            ; takeitem item=25 qty=1
    db $21, $2C                 ; checkitem item=44
    db $5E, $02, $05            ; loadtrainer group=2 id=5
    db $72, $03, $07, $0B       ; moveobject obj=3 x=7 y=11
    db $75, $02, $04, $14       ; showemote bubble=2 obj=4 time=20
    db $91                      ; end
