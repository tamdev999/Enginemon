#!/usr/bin/env python3
"""
HOSTILE CHECK Part 2: Why did shorter sequential patterns fail?
"""

import sys

VALID_TYPES = set(range(0, 10)) | {19} | set(range(20, 28))
VALID_GROWTH_RATES = set(range(0, 6))
BASE_DATA_SIZE = 32

def validate_record_verbose(rom: bytes, offset: int, expected_dex_no: int) -> tuple:
    """Returns (valid, reason, details)"""
    if offset + BASE_DATA_SIZE > len(rom):
        return False, "out_of_bounds", {}
    
    dex_no = rom[offset]
    stats = list(rom[offset+1:offset+7])
    type1 = rom[offset + 7]
    type2 = rom[offset + 8]
    growth = rom[offset + 22]
    
    details = {
        'dex_no': dex_no,
        'stats': stats,
        'type1': type1,
        'type2': type2,
        'growth': growth
    }
    
    if dex_no != expected_dex_no:
        return False, f"dex_no_mismatch", details
    
    for i, s in enumerate(stats):
        if s == 0:
            return False, f"zero_stat_{i}", details
    
    if type1 not in VALID_TYPES:
        return False, f"invalid_type1", details
    if type2 not in VALID_TYPES:
        return False, f"invalid_type2", details
    
    if growth not in VALID_GROWTH_RATES:
        return False, f"invalid_growth", details
    
    return True, "ok", details


def analyze_candidate(rom: bytes, offset: int, label: str):
    """Analyze why a sequential pattern passed or failed full validation"""
    print(f"\n=== {label}: offset=0x{offset:05x} ===")
    
    for i in range(10):
        rec_offset = offset + i * BASE_DATA_SIZE
        expected = i + 1
        valid, reason, details = validate_record_verbose(rom, rec_offset, expected)
        
        status = "✓" if valid else "✗"
        print(f"  [{i+1:2d}] {status} DEX={details.get('dex_no', '?'):3d} "
              f"stats={details.get('stats', [])} "
              f"types={details.get('type1', '?')},{details.get('type2', '?')} "
              f"growth={details.get('growth', '?')} "
              f"| {reason}")
        
        if not valid:
            break


def main():
    if len(sys.argv) < 2:
        print("Usage: python basedata_probe2.py <rom.gbc>")
        sys.exit(1)
    
    rom_path = sys.argv[1]
    with open(rom_path, "rb") as f:
        rom = f.read()
    
    # Patterns found with sequential DEX_NO at stride 32
    patterns = [
        (0x51424, 251, "WINNER - BaseData"),
        (0xd14fa, 5, "5-length pattern"),
        (0xd12a0, 4, "4-length pattern #1"),
        (0x11dae6, 4, "4-length pattern #2"),
        (0x12c35a, 4, "4-length pattern #3"),
        (0x4d0a6, 3, "3-length pattern #1"),
        (0x71171, 3, "3-length pattern #2"),
    ]
    
    for offset, length, label in patterns:
        analyze_candidate(rom, offset, f"{label} (seq_len={length})")
    
    # Probability analysis
    print("\n" + "="*60)
    print("PROBABILITY MODEL CRITIQUE")
    print("="*60)
    
    # Count actual occurrences in ROM
    type_bytes = sum(1 for b in rom if b in VALID_TYPES)
    growth_bytes = sum(1 for b in rom if b in VALID_GROWTH_RATES)
    byte_1_count = sum(1 for b in rom if b == 1)
    
    print(f"\nROM byte distributions (empirical vs uniform):")
    print(f"  Byte == 1:           {byte_1_count:7d} / {len(rom)} = {byte_1_count/len(rom):.6f}  (uniform: {1/256:.6f})")
    print(f"  Byte in valid_types: {type_bytes:7d} / {len(rom)} = {type_bytes/len(rom):.6f}  (uniform: {19/256:.6f})")
    print(f"  Byte in valid_growth:{growth_bytes:7d} / {len(rom)} = {growth_bytes/len(rom):.6f}  (uniform: {6/256:.6f})")
    
    print(f"\n  Type byte deviation:   {type_bytes/len(rom) / (19/256):.2f}x higher than uniform")
    print(f"  Growth byte deviation: {growth_bytes/len(rom) / (6/256):.2f}x higher than uniform")
    
    # The crucial test: count sequential 1,2,3... patterns at stride 32
    seq_count = 0
    for offset in range(len(rom) - 32 * 3):
        if rom[offset] == 1 and rom[offset + 32] == 2 and rom[offset + 64] == 3:
            seq_count += 1
    
    print(f"\n  Sequential 1,2,3 at stride 32: {seq_count} occurrences")
    print(f"  Expected if uniform random: {len(rom) / (256**3):.2f}")
    
    # Count patterns that pass just sequential + nonzero stats
    partial_candidates = 0
    for offset in range(len(rom) - 32 * 3):
        if rom[offset] != 1:
            continue
        valid = True
        for i in range(3):
            rec = offset + i * 32
            if rom[rec] != i + 1:
                valid = False
                break
            if any(rom[rec + j] == 0 for j in range(1, 7)):
                valid = False
                break
        if valid:
            partial_candidates += 1
    
    print(f"\n  Patterns passing (seq_dex + nonzero_stats) for 3 records: {partial_candidates}")


if __name__ == "__main__":
    main()
