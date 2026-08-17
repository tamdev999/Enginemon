#!/usr/bin/env python3
"""
HOSTILE CHECK Part 4b: Deeper code pattern analysis

The GetBaseData uses ld bc, 32 not ld de, 32.
Let's find what's called after that.
"""

import sys
from collections import defaultdict

def read_u16_le(rom: bytes, offset: int) -> int:
    return rom[offset] | (rom[offset + 1] << 8)


def find_stride32_calls(rom: bytes):
    """
    Find all patterns: ld bc/de, 32 ... call XXXX
    """
    print("=== Stride-32 Call Analysis ===")
    
    call_targets = defaultdict(list)
    
    for i in range(len(rom) - 10):
        # ld bc, 32 (01 20 00) or ld de, 32 (11 20 00)
        if rom[i] in (0x01, 0x11) and rom[i+1] == 0x20 and rom[i+2] == 0x00:
            reg = "bc" if rom[i] == 0x01 else "de"
            
            # Look for call within next 10 bytes
            for j in range(i+3, min(i+13, len(rom)-3)):
                if rom[j] == 0xcd:  # call
                    target = read_u16_le(rom, j+1)
                    # Only count home bank targets (< 0x4000)
                    if target < 0x4000:
                        call_targets[target].append((i, reg))
                    break
    
    print(f"Home bank routines called after stride-32 load:")
    for addr, refs in sorted(call_targets.items(), key=lambda x: -len(x[1])):
        if len(refs) >= 2:
            bc_count = sum(1 for _, r in refs if r == "bc")
            de_count = sum(1 for _, r in refs if r == "de")
            print(f"  0x{addr:04x}: {len(refs)} calls (bc={bc_count}, de={de_count})")


def find_table_accesses(rom: bytes):
    """
    Find ld hl, <banked_addr> followed by call to 0x30fe (AddNTimes with bc)
    """
    print("\n=== Table Access via hl + call 0x30fe ===")
    
    # From the disassembly, 0x30fe is called after ld bc, 32; ld hl, 0x5424
    # This is AddNTimes: hl += a * bc
    
    table_refs = defaultdict(list)
    
    for i in range(len(rom) - 15):
        if rom[i] == 0x21:  # ld hl, imm16
            hl_addr = read_u16_le(rom, i+1)
            if hl_addr < 0x4000 or hl_addr >= 0x8000:
                continue
            
            # Look for call 0x30fe within next 15 bytes
            for j in range(i+3, min(i+18, len(rom)-3)):
                if rom[j] == 0xcd:
                    target = read_u16_le(rom, j+1)
                    if target == 0x30fe:  # AddNTimes
                        # Check for ld bc, XX before the call
                        for k in range(i+3, j):
                            if rom[k] == 0x01:  # ld bc, imm16
                                stride = read_u16_le(rom, k+1)
                                table_refs[(hl_addr, stride)].append(i)
                                break
                        break
    
    print(f"Tables accessed via AddNTimes (0x30fe):")
    for (addr, stride), refs in sorted(table_refs.items(), key=lambda x: -len(x[1])):
        print(f"  hl=0x{addr:04x} stride={stride}: {len(refs)} accesses")
        for ref in refs[:5]:
            print(f"    at 0x{ref:05x}")


def find_bank_switch_before_table_access(rom: bytes):
    """
    Look for: ld a, <bank> ; rst <X> ; ... ; ld hl, <addr>
    This would tell us the bank of the table.
    """
    print("\n=== Bank Switch + Table Access ===")
    
    # Common rst Bankswitch patterns
    RST_OPCODES = {0xc7, 0xcf, 0xd7, 0xdf, 0xe7, 0xef, 0xf7, 0xff}
    
    # From disassembly, the bank switch is rst 08 (0xcf)
    # Look for: ld a, bank (3e XX) ; rst ; ... ; ld hl, addr
    
    bank_table_pairs = defaultdict(list)
    
    for i in range(len(rom) - 20):
        if rom[i] == 0x3e:  # ld a, imm8
            bank = rom[i+1]
            if bank > 0x7f:  # Invalid bank
                continue
            
            # Look for rst within next 5 bytes
            rst_found = False
            rst_pos = 0
            for j in range(i+2, min(i+7, len(rom))):
                if rom[j] in RST_OPCODES:
                    rst_found = True
                    rst_pos = j
                    break
            
            if not rst_found:
                continue
            
            # Look for ld hl within next 20 bytes after rst
            for j in range(rst_pos+1, min(rst_pos+25, len(rom)-3)):
                if rom[j] == 0x21:  # ld hl, imm16
                    addr = read_u16_le(rom, j+1)
                    if 0x4000 <= addr < 0x8000:
                        bank_table_pairs[(bank, addr)].append(i)
                    break
    
    print(f"Bank:Address pairs found via ld a, bank; rst; ld hl, addr:")
    for (bank, addr), refs in sorted(bank_table_pairs.items(), key=lambda x: -len(x[1])):
        if len(refs) >= 2:
            flat = bank * 0x4000 + (addr - 0x4000)
            print(f"  {bank:02x}:{addr:04x} (flat=0x{flat:05x}): {len(refs)} refs")


def analyze_basedata_discovery_confidence(rom: bytes):
    """
    Summarize what we can discover from code alone.
    """
    print("\n" + "="*60)
    print("CODE-BASED DISCOVERY SUMMARY")
    print("="*60)
    
    # Known from earlier probing:
    # 0x5424 is referenced twice in home bank with ld hl
    # Both are followed by ld bc, 32 and call 0x30fe
    
    # Count ld hl, 0x5424
    hl_5424_count = 0
    for i in range(len(rom) - 3):
        if rom[i] == 0x21 and rom[i+1] == 0x24 and rom[i+2] == 0x54:
            hl_5424_count += 1
    
    # Count ld hl, XXXX in home bank (< 0x4000) with banked addresses
    home_hl_banked = defaultdict(int)
    for i in range(min(0x4000, len(rom)) - 3):
        if rom[i] == 0x21:
            addr = read_u16_le(rom, i+1)
            if 0x4000 <= addr < 0x8000:
                home_hl_banked[addr] += 1
    
    print(f"\nHome bank 'ld hl, <banked_addr>' frequency:")
    for addr, count in sorted(home_hl_banked.items(), key=lambda x: -x[1])[:10]:
        marker = " <-- BaseData" if addr == 0x5424 else ""
        print(f"  0x{addr:04x}: {count} times{marker}")
    
    print(f"\nCan we uniquely identify BaseData from code patterns alone?")
    print(f"  'ld hl, 0x5424' appears {hl_5424_count} times total in ROM")
    print(f"  In home bank with stride-32 context: 2 times (GetBaseData routine)")
    print(f"  Most common banked hl load in home bank: 0x{max(home_hl_banked.items(), key=lambda x: x[1])[0]:04x}")
    
    if hl_5424_count == 2:
        print(f"\n  CONCLUSION: Code reference discovery is WEAK for BaseData.")
        print(f"              Only 2 references, other addresses have more.")
        print(f"              Cannot uniquely identify without knowing stride or bank.")


def main():
    if len(sys.argv) < 2:
        print("Usage: python code_pattern_probe2.py <rom.gbc>")
        sys.exit(1)
    
    rom_path = sys.argv[1]
    with open(rom_path, "rb") as f:
        rom = f.read()
    
    print(f"ROM: {rom_path}")
    
    find_stride32_calls(rom)
    find_table_accesses(rom)
    find_bank_switch_before_table_access(rom)
    analyze_basedata_discovery_confidence(rom)


if __name__ == "__main__":
    main()
