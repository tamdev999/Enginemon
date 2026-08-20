; event_flag_vs_engine_flag.asm
; Oracle fixture: EventFlag vs EngineFlag namespace distinction
;
; Historical bug: checkevent and checkflag both decoded to FlagId{value} without
; tracking which namespace (Event vs Engine) the flag belongs to.
; EventFlag{5} and EngineFlag{5} would collapse to the same identifier.
;
; This fixture encodes BOTH commands with the same numeric flag value (5) so that
; any namespace collapse is definitively caught: the decoded namespaces MUST differ.
;
; Byte layout:
;   checkevent opcode = 0x31, dw event_flag (little-endian)
;   checkflag  opcode = 0x34, dw engine_flag (little-endian)
;   end opcode = 0x91
;
; AUTHORITATIVE byte sequence:
;   31 05 00   ; checkevent event_flag=5
;   34 05 00   ; checkflag engine_flag=5
;   91         ; end
;
; Oracle asserts for checkevent result:
;   flag.ns == FlagNamespace::Event
;   flag.value == 5
;
; Oracle asserts for checkflag result:
;   flag.ns == FlagNamespace::Engine
;   flag.value == 5
;
; Mutation check: if both produce FlagNamespace::Event, they MUST NOT compare equal.

SECTION "fixture", ROM0[$0000]

    db $31, $05, $00   ; checkevent event_flag=5
    db $34, $05, $00   ; checkflag engine_flag=5
    db $91              ; end
