#!/usr/bin/env python3
"""
HOSTILE CHECK Part 3: Code Reference Discovery

Can we locate species tables by finding code that accesses them?
Look for patterns like:
- ld hl, <table_addr>
- call AddNTimes with known stride
- dba references (bank:addr)
"""

import sys
from collections import defaultdict

def read_u16_le(rom: bytes, offset: int) -> int:
    return rom[offset] | (rom[offset + 1] << 8)

def flat_to_bank_addr(flat: int) -> tuple:
    """Convert flat offset to bank:address"""
    if flat < 0x4000:
        return 0, flat
    bank = flat // 0x4000
    addr = 0x4000 + (flat % 0x4000)
    return bank, addr

def bank_addr_to_flat(bank: int, addr: int) -> int:
    """Convert bank:address to flat offset"""
    if addr < 0x4000:
        return addr
    return bank * 0x4000 + (addr - 0x4000)


def find_ld_hl_immediate(rom: bytes) -> list:
    """
    Find all 'ld hl, imm16' instructions (opcode 0x21)
    Returns list of (instruction_offset, target_addr)
    """
    results = []
    for i in range(len(rom) - 3):
        if rom[i] == 0x21:  # ld hl, imm16
            addr = read_u16_le(rom, i + 1)
            results.append((i, addr))
    return results


def find_dba_references(rom: bytes) -> list:
    """
    Find all 3-byte dba sequences (bank, addr_lo, addr_hi)
    that look like valid ROM pointers.
    Returns list of (offset, bank, addr, flat_target)
    """
    results = []
    for i in range(len(rom) - 3):
        bank = rom[i]
        addr = read_u16_le(rom, i + 1)
        
        # Valid banked address range
        if addr < 0x4000 or addr >= 0x8000:
            continue
        
        # Bank must be reasonable
        if bank > 0x7F:
            continue
        
        flat = bank_addr_to_flat(bank, addr)
        if flat < len(rom):
            results.append((i, bank, addr, flat))
    
    return results


def find_known_table_references(rom: bytes, known_offset: int, known_name: str):
    """
    Given a known table offset, find code that references it.
    """
    bank, addr = flat_to_bank_addr(known_offset)
    
    print(f"\n=== References to {known_name} (0x{known_offset:05x} = {bank:02x}:{addr:04x}) ===")
    
    # Look for ld hl, addr (same bank)
    ld_hl_refs = []
    for i in range(len(rom) - 3):
        if rom[i] == 0x21:  # ld hl, imm16
            target = read_u16_le(rom, i + 1)
            if target == addr:
                ref_bank, ref_addr = flat_to_bank_addr(i)
                ld_hl_refs.append((i, ref_bank, ref_addr))
    
    print(f"  'ld hl, 0x{addr:04x}' found {len(ld_hl_refs)} times:")
    for flat, ref_bank, ref_addr in ld_hl_refs[:10]:
        print(f"    at 0x{flat:05x} ({ref_bank:02x}:{ref_addr:04x})")
    
    # Look for dba references
    dba_refs = []
    for i in range(len(rom) - 3):
        if rom[i] == bank and read_u16_le(rom, i + 1) == addr:
            ref_bank, ref_addr = flat_to_bank_addr(i)
            dba_refs.append((i, ref_bank, ref_addr))
    
    print(f"  dba {bank:02x}:{addr:04x} found {len(dba_refs)} times:")
    for flat, ref_bank, ref_addr in dba_refs[:10]:
        print(f"    at 0x{flat:05x} ({ref_bank:02x}:{ref_addr:04x})")


def find_addntimes_pattern(rom: bytes):
    """
    Look for the AddNTimes pattern used to index into tables.
    AddNTimes multiplies a by de and adds to hl.
    
    Pattern: 
      ld de, <stride>
      call AddNTimes  (or jp)
    """
    # Find all 'ld de, imm16' (opcode 0x11)
    de_loads = []
    for i in range(len(rom) - 3):
        if rom[i] == 0x11:
            stride = read_u16_le(rom, i + 1)
            de_loads.append((i, stride))
    
    # Count stride usage
    stride_counts = defaultdict(list)
    for offset, stride in de_loads:
        stride_counts[stride].append(offset)
    
    print(f"\n=== AddNTimes Stride Analysis ===")
    print(f"Total 'ld de, imm16' instructions: {len(de_loads)}")
    
    # Show strides that match known table record sizes
    known_strides = {
        32: "BaseData",
        10: "PokemonNames (NAME_LENGTH)",
        6: "PokemonCries/PokemonPicPointers",
        8: "PokemonPalettes",
        2: "EvosAttacksPointers/PokedexPointers",
    }
    
    print(f"\nStrides matching known table sizes:")
    for stride, name in known_strides.items():
        refs = stride_counts.get(stride, [])
        print(f"  stride={stride:3d} ({name}): {len(refs)} occurrences")
        for offset in refs[:5]:
            bank, addr = flat_to_bank_addr(offset)
            print(f"    at 0x{offset:05x} ({bank:02x}:{addr:04x})")


def find_farcall_getbasedata_pattern(rom: bytes):
    """
    Look for the GetBaseData pattern:
    - farcall or call to a routine that loads BaseData
    - The routine would use AddNTimes with stride 32
    """
    # Look for sequences: ld de, 32 followed by call/jp
    print(f"\n=== Searching for BaseData access pattern ===")
    
    for i in range(len(rom) - 6):
        # ld de, 32 = 0x11 0x20 0x00
        if rom[i] == 0x11 and rom[i+1] == 0x20 and rom[i+2] == 0x00:
            # Check what comes next
            next_bytes = list(rom[i+3:i+8])
            bank, addr = flat_to_bank_addr(i)
            print(f"  'ld de, 32' at 0x{i:05x} ({bank:02x}:{addr:04x}), next: {[hex(b) for b in next_bytes]}")


def scan_for_table_address_in_code(rom: bytes, table_flat: int, table_name: str):
    """
    More aggressive search: find the table address anywhere in code banks.
    """
    bank, addr = flat_to_bank_addr(table_flat)
    
    # Search for the address bytes in sequence (little endian)
    addr_lo = addr & 0xFF
    addr_hi = (addr >> 8) & 0xFF
    
    occurrences = []
    for i in range(len(rom) - 2):
        if rom[i] == addr_lo and rom[i+1] == addr_hi:
            ref_bank, ref_addr = flat_to_bank_addr(i)
            occurrences.append((i, ref_bank, ref_addr))
    
    print(f"\n=== Raw address bytes for {table_name} ({addr_lo:02x} {addr_hi:02x}) ===")
    print(f"  Found {len(occurrences)} occurrences")
    
    # Filter to likely code references (in home bank or same bank)
    code_likely = [o for o in occurrences if o[1] == 0 or o[1] == bank]
    print(f"  In home/same bank: {len(code_likely)}")
    for flat, ref_bank, ref_addr in code_likely[:10]:
        # Show context
        ctx_start = max(0, flat - 3)
        ctx = rom[ctx_start:flat+5]
        print(f"    0x{flat:05x} ({ref_bank:02x}:{ref_addr:04x}): {' '.join(f'{b:02x}' for b in ctx)}")


def main():
    if len(sys.argv) < 2:
        print("Usage: python code_reference_probe.py <rom.gbc>")
        sys.exit(1)
    
    rom_path = sys.argv[1]
    with open(rom_path, "rb") as f:
        rom = f.read()
    
    print(f"ROM: {rom_path}")
    print(f"Size: {len(rom)} bytes")
    
    # Known table offsets from source analysis
    known_tables = {
        0x51424: "BaseData",
        0x425b1: "EvosAttacksPointers",
        0x53a04: "PokemonNames",
        0x120000: "PokemonPicPointers",
    }
    
    for offset, name in known_tables.items():
        find_known_table_references(rom, offset, name)
    
    find_addntimes_pattern(rom)
    find_farcall_getbasedata_pattern(rom)
    
    # More aggressive search for BaseData
    scan_for_table_address_in_code(rom, 0x51424, "BaseData")


if __name__ == "__main__":
    main()
