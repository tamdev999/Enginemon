#!/usr/bin/env python3
"""
HOSTILE CHECK Part 4: Can we discover BaseData via code pattern recognition?

The GetBaseData routine uses:
  ld hl, BaseData  (21 24 54)
  call AddNTimes   (cd XX XX)

We found two references to address 0x5424 in home bank:
  0x0386b: 21 24 54
  0x03934: 21 24 54

Can we find this pattern without knowing the address?
"""

import sys

def read_u16_le(rom: bytes, offset: int) -> int:
    return rom[offset] | (rom[offset + 1] << 8)

def find_getbasedata_pattern(rom: bytes):
    """
    Look for the GetBaseData code pattern:
    
    ld a, BANK(BaseData)  ; 3e XX - load bank number
    rst Bankswitch        ; c7/cf/d7/df/e7/ef/f7/ff - switch bank
    ...
    ld bc, 32             ; 01 20 00 - BASE_DATA_SIZE
    ld hl, <table>        ; 21 XX XX
    call AddNTimes        ; cd XX XX
    
    We're looking for:
    - ld bc, 32 (01 20 00) followed by ld hl (21) within ~20 bytes
    - Or ld hl (21) followed by ld de, 32 (11 20 00) and call
    """
    
    print("=== Pattern: ld bc, 32 ... ld hl, addr ===")
    
    # Pattern: 01 20 00 (ld bc, 32)
    candidates = []
    for i in range(len(rom) - 10):
        if rom[i] == 0x01 and rom[i+1] == 0x20 and rom[i+2] == 0x00:
            # Found ld bc, 32 - look for ld hl within next 10 bytes
            for j in range(i+3, min(i+13, len(rom)-3)):
                if rom[j] == 0x21:  # ld hl, imm16
                    addr = read_u16_le(rom, j+1)
                    # Check if address is in banked range
                    if 0x4000 <= addr < 0x8000:
                        candidates.append((i, j, addr))
    
    print(f"  Found {len(candidates)} candidates")
    for bc_off, hl_off, addr in candidates[:20]:
        # Show context
        ctx = rom[bc_off:bc_off+15]
        print(f"    bc@0x{bc_off:05x} hl@0x{hl_off:05x} -> 0x{addr:04x}: {' '.join(f'{b:02x}' for b in ctx)}")

    # Now find the specific GetBaseData by looking for the full pattern
    print("\n=== Full GetBaseData pattern search ===")
    print("Looking for: ld a, <bank> ; rst Bankswitch ; ... ; ld bc, 32 ; ld hl, <addr>")
    
    # The home bank GetBaseData routine at ~0x386a
    # Look at what's actually there
    print("\n=== Disassembly around 0x386a ===")
    disasm_region(rom, 0x3860, 0x3890)
    
    print("\n=== Disassembly around 0x3930 ===")
    disasm_region(rom, 0x3928, 0x3958)


def disasm_region(rom: bytes, start: int, end: int):
    """Simple disassembler for relevant opcodes"""
    opcodes = {
        0x01: ("ld bc, imm16", 3),
        0x11: ("ld de, imm16", 3),
        0x21: ("ld hl, imm16", 3),
        0x31: ("ld sp, imm16", 3),
        0x3e: ("ld a, imm8", 2),
        0xc3: ("jp imm16", 3),
        0xc7: ("rst 00", 1),
        0xcf: ("rst 08", 1),
        0xd7: ("rst 10", 1),
        0xdf: ("rst 18", 1),
        0xe7: ("rst 20", 1),
        0xef: ("rst 28", 1),
        0xf7: ("rst 30", 1),
        0xff: ("rst 38", 1),
        0xcd: ("call imm16", 3),
        0xc9: ("ret", 1),
        0xc5: ("push bc", 1),
        0xd5: ("push de", 1),
        0xe5: ("push hl", 1),
        0xf5: ("push af", 1),
        0xc1: ("pop bc", 1),
        0xd1: ("pop de", 1),
        0xe1: ("pop hl", 1),
        0xf1: ("pop af", 1),
        0xfa: ("ld a, [imm16]", 3),
        0xea: ("ld [imm16], a", 3),
        0xf0: ("ldh a, [imm8]", 2),
        0xe0: ("ldh [imm8], a", 2),
        0xfe: ("cp imm8", 2),
        0x28: ("jr z, rel8", 2),
        0x20: ("jr nz, rel8", 2),
        0x18: ("jr rel8", 2),
        0x3d: ("dec a", 1),
        0x00: ("nop", 1),
    }
    
    i = start
    while i < end and i < len(rom):
        opcode = rom[i]
        if opcode in opcodes:
            name, length = opcodes[opcode]
            if length == 1:
                print(f"  0x{i:05x}: {opcode:02x}          {name}")
            elif length == 2:
                operand = rom[i+1]
                print(f"  0x{i:05x}: {opcode:02x} {operand:02x}       {name} = {operand:02x}")
            elif length == 3:
                operand = read_u16_le(rom, i+1)
                print(f"  0x{i:05x}: {opcode:02x} {rom[i+1]:02x} {rom[i+2]:02x}    {name} = {operand:04x}")
            i += length
        else:
            print(f"  0x{i:05x}: {opcode:02x}          ???")
            i += 1


def analyze_getbasedata_discovery(rom: bytes):
    """
    Try to discover BaseData table address from code patterns alone.
    """
    print("\n" + "="*60)
    print("INDEPENDENT TABLE DISCOVERY VIA CODE")
    print("="*60)
    
    # Strategy: Find routines that:
    # 1. Use ld bc, 32 (BASE_DATA_SIZE)
    # 2. Load an address into hl
    # 3. Call AddNTimes or similar
    # 4. The hl address should be in a data bank (0x40-0x80)
    
    # First find AddNTimes by looking for it being called with de=32
    print("\n=== Finding AddNTimes address ===")
    
    # Look for: ld de, 32 ; call XXXX
    addntimes_candidates = set()
    for i in range(len(rom) - 6):
        if rom[i] == 0x11 and rom[i+1] == 0x20 and rom[i+2] == 0x00:  # ld de, 32
            if rom[i+3] == 0xcd:  # call
                call_addr = read_u16_le(rom, i+4)
                addntimes_candidates.add(call_addr)
    
    print(f"  Addresses called after 'ld de, 32': {[hex(a) for a in sorted(addntimes_candidates)]}")
    
    # Now find routines that set up hl before calling these
    print("\n=== BaseData table candidates via code analysis ===")
    
    table_candidates = {}
    
    for i in range(len(rom) - 20):
        # Look for pattern: ld hl, XXXX followed eventually by call to one of AddNTimes candidates
        if rom[i] == 0x21:  # ld hl, imm16
            hl_addr = read_u16_le(rom, i+1)
            if hl_addr < 0x4000 or hl_addr >= 0x8000:
                continue
            
            # Check if within next 15 bytes we call an AddNTimes candidate
            for j in range(i+3, min(i+18, len(rom)-3)):
                if rom[j] == 0xcd:
                    call_target = read_u16_le(rom, j+1)
                    if call_target in addntimes_candidates:
                        # Check if ld de, 32 or ld bc, 32 is between hl and call
                        has_stride_32 = False
                        for k in range(i+3, j):
                            if k+2 < len(rom):
                                if (rom[k] == 0x11 or rom[k] == 0x01) and rom[k+1] == 0x20 and rom[k+2] == 0x00:
                                    has_stride_32 = True
                                    break
                        
                        if has_stride_32:
                            if hl_addr not in table_candidates:
                                table_candidates[hl_addr] = []
                            table_candidates[hl_addr].append(i)
    
    print(f"  Addresses loaded into hl before stride-32 + AddNTimes call:")
    for addr, refs in sorted(table_candidates.items(), key=lambda x: -len(x[1])):
        print(f"    0x{addr:04x}: {len(refs)} references")
        if len(refs) <= 5:
            for ref in refs:
                print(f"      at 0x{ref:05x}")


def main():
    if len(sys.argv) < 2:
        print("Usage: python code_pattern_probe.py <rom.gbc>")
        sys.exit(1)
    
    rom_path = sys.argv[1]
    with open(rom_path, "rb") as f:
        rom = f.read()
    
    print(f"ROM: {rom_path}")
    print(f"Size: {len(rom)} bytes")
    
    find_getbasedata_pattern(rom)
    analyze_getbasedata_discovery(rom)


if __name__ == "__main__":
    main()
