#!/usr/bin/env python3
"""
HOSTILE CHECK: BaseData Discovery Probe
Empirically tests the claims from SPECIES_DOMAIN_DISCOVERY.md

This is a PROBE ONLY - not production code.
"""

import sys
from dataclasses import dataclass
from typing import List, Tuple, Set

# Crystal type encoding (from type_constants.asm)
# PHYSICAL: 0-9 (NORMAL, FIGHTING, FLYING, POISON, GROUND, ROCK, BIRD, BUG, GHOST, STEEL)
# Note: BIRD (6) is unused but valid in the encoding
# CURSE_TYPE: 19
# SPECIAL: 20-27 (FIRE, WATER, GRASS, ELECTRIC, PSYCHIC_TYPE, ICE, DRAGON, DARK)
VALID_TYPES = set(range(0, 10)) | {19} | set(range(20, 28))

# Growth rate: 0-5 only
VALID_GROWTH_RATES = set(range(0, 6))

BASE_DATA_SIZE = 32


@dataclass
class CandidateResult:
    offset: int
    run_length: int
    fail_reason: str  # empty if still valid at max scan


def validate_record(rom: bytes, offset: int, expected_dex_no: int) -> Tuple[bool, str]:
    """
    Validate a single BaseData record.
    Returns (valid, reason).
    """
    if offset + BASE_DATA_SIZE > len(rom):
        return False, "out_of_bounds"
    
    # Check DEX_NO matches expected
    dex_no = rom[offset]
    if dex_no != expected_dex_no:
        return False, f"dex_no_mismatch:{dex_no}!={expected_dex_no}"
    
    # Check stats are non-zero (bytes 1-6)
    for i in range(1, 7):
        if rom[offset + i] == 0:
            return False, f"zero_stat_at_byte_{i}"
    
    # Check types are valid (bytes 7-8)
    type1 = rom[offset + 7]
    type2 = rom[offset + 8]
    if type1 not in VALID_TYPES:
        return False, f"invalid_type1:{type1}"
    if type2 not in VALID_TYPES:
        return False, f"invalid_type2:{type2}"
    
    # Check growth rate (byte 22)
    growth = rom[offset + 22]
    if growth not in VALID_GROWTH_RATES:
        return False, f"invalid_growth:{growth}"
    
    return True, "ok"


def scan_candidate(rom: bytes, start_offset: int, max_records: int = 300) -> CandidateResult:
    """
    Scan a candidate starting at start_offset.
    Returns the run length and failure reason.
    """
    for i in range(max_records):
        offset = start_offset + i * BASE_DATA_SIZE
        expected_dex_no = i + 1
        
        valid, reason = validate_record(rom, offset, expected_dex_no)
        if not valid:
            return CandidateResult(start_offset, i, reason)
    
    return CandidateResult(start_offset, max_records, "max_reached")


def full_rom_scan(rom: bytes, min_seed: int) -> List[CandidateResult]:
    """
    Scan entire ROM for BaseData candidates.
    """
    candidates = []
    rom_size = len(rom)
    
    # Quick filter: first byte must be 1 (DEX_NO of first species)
    for offset in range(0, rom_size - BASE_DATA_SIZE * min_seed):
        if rom[offset] != 1:
            continue
        
        result = scan_candidate(rom, offset)
        if result.run_length >= min_seed:
            candidates.append(result)
    
    return candidates


def analyze_type_distribution(rom: bytes):
    """
    Analyze how often valid type bytes appear in the ROM.
    This tests the "uniform random" assumption.
    """
    type_count = 0
    total = len(rom)
    
    for b in rom:
        if b in VALID_TYPES:
            type_count += 1
    
    print(f"\n=== TYPE BYTE DISTRIBUTION ===")
    print(f"Valid type values: {sorted(VALID_TYPES)}")
    print(f"Count of valid types: {len(VALID_TYPES)}")
    print(f"ROM bytes that are valid types: {type_count} / {total} = {type_count/total:.4f}")
    print(f"Expected if uniform random: {len(VALID_TYPES)}/256 = {len(VALID_TYPES)/256:.4f}")


def analyze_growth_distribution(rom: bytes):
    """
    Analyze how often valid growth rate bytes appear in the ROM.
    """
    growth_count = 0
    total = len(rom)
    
    for b in rom:
        if b in VALID_GROWTH_RATES:
            growth_count += 1
    
    print(f"\n=== GROWTH RATE DISTRIBUTION ===")
    print(f"Valid growth values: {sorted(VALID_GROWTH_RATES)}")
    print(f"Count of valid growth rates: {len(VALID_GROWTH_RATES)}")
    print(f"ROM bytes that are valid growth: {growth_count} / {total} = {growth_count/total:.4f}")
    print(f"Expected if uniform random: {len(VALID_GROWTH_RATES)}/256 = {len(VALID_GROWTH_RATES)/256:.4f}")


def count_nonzero_runs(rom: bytes, run_length: int = 6):
    """
    Count how many 6-byte runs have all non-zero bytes.
    """
    count = 0
    total = len(rom) - run_length + 1
    
    for i in range(total):
        if all(rom[i+j] != 0 for j in range(run_length)):
            count += 1
    
    print(f"\n=== NON-ZERO STAT RUNS ===")
    print(f"6-byte runs with all non-zero: {count} / {total} = {count/total:.4f}")
    print(f"Expected if uniform random: (255/256)^6 = {(255/256)**6:.4f}")


def find_sequential_dex_patterns(rom: bytes, stride: int = 32, min_length: int = 3):
    """
    Find patterns where bytes at fixed stride are sequential 1,2,3,...
    This is the core BaseData signature.
    """
    candidates = []
    rom_size = len(rom)
    
    for offset in range(rom_size - stride * min_length):
        if rom[offset] != 1:
            continue
        
        # Count how long the sequential pattern holds
        length = 1
        for i in range(1, 300):
            check_offset = offset + i * stride
            if check_offset >= rom_size:
                break
            if rom[check_offset] != i + 1:
                break
            length += 1
        
        if length >= min_length:
            candidates.append((offset, length))
    
    return candidates


def main():
    if len(sys.argv) < 2:
        print("Usage: python basedata_probe.py <rom.gbc>")
        sys.exit(1)
    
    rom_path = sys.argv[1]
    with open(rom_path, "rb") as f:
        rom = f.read()
    
    print(f"ROM: {rom_path}")
    print(f"Size: {len(rom)} bytes ({len(rom) // 1024} KB)")
    
    # Distribution analysis
    analyze_type_distribution(rom)
    analyze_growth_distribution(rom)
    count_nonzero_runs(rom)
    
    # Sequential DEX_NO pattern analysis (stride 32)
    print(f"\n=== SEQUENTIAL DEX_NO PATTERNS (stride=32) ===")
    seq_patterns = find_sequential_dex_patterns(rom, stride=32, min_length=3)
    print(f"Found {len(seq_patterns)} patterns of length >= 3:")
    for offset, length in sorted(seq_patterns, key=lambda x: -x[1])[:20]:
        print(f"  offset=0x{offset:05x} length={length}")
    
    # Full BaseData scan with various min_seed values
    print(f"\n{'='*60}")
    print("FULL BASEDATA SCAN RESULTS")
    print(f"{'='*60}")
    
    for min_seed in [3, 4, 5, 8, 10, 20, 50]:
        candidates = full_rom_scan(rom, min_seed)
        print(f"\n--- min_seed={min_seed} ---")
        print(f"Candidates found: {len(candidates)}")
        
        for c in sorted(candidates, key=lambda x: -x.run_length)[:10]:
            print(f"  offset=0x{c.offset:05x} run={c.run_length:3d} fail_reason={c.fail_reason}")
        
        if len(candidates) == 1:
            print(f"  >>> UNIQUE CANDIDATE at 0x{candidates[0].offset:05x} with {candidates[0].run_length} records")
        elif len(candidates) == 0:
            print(f"  >>> NO CANDIDATES")
        else:
            print(f"  >>> {len(candidates)} CANDIDATES (AMBIGUOUS)")
    
    # Detailed analysis of winning candidate
    print(f"\n{'='*60}")
    print("DETAILED WINNING CANDIDATE ANALYSIS")
    print(f"{'='*60}")
    
    candidates = full_rom_scan(rom, min_seed=3)
    if candidates:
        winner = max(candidates, key=lambda x: x.run_length)
        print(f"\nWinning candidate: offset=0x{winner.offset:05x} run={winner.run_length}")
        print(f"Fail reason: {winner.fail_reason}")
        
        # Show first few and last few records
        print(f"\nFirst 5 records:")
        for i in range(min(5, winner.run_length)):
            offset = winner.offset + i * BASE_DATA_SIZE
            dex_no = rom[offset]
            stats = list(rom[offset+1:offset+7])
            type1, type2 = rom[offset+7], rom[offset+8]
            growth = rom[offset+22]
            print(f"  [{i+1}] DEX={dex_no} stats={stats} types={type1},{type2} growth={growth}")
        
        if winner.run_length > 5:
            print(f"\nLast 3 records:")
            for i in range(max(5, winner.run_length-3), winner.run_length):
                offset = winner.offset + i * BASE_DATA_SIZE
                dex_no = rom[offset]
                stats = list(rom[offset+1:offset+7])
                type1, type2 = rom[offset+7], rom[offset+8]
                growth = rom[offset+22]
                print(f"  [{i+1}] DEX={dex_no} stats={stats} types={type1},{type2} growth={growth}")
        
        # Show the failing record
        if winner.run_length < 300:
            fail_offset = winner.offset + winner.run_length * BASE_DATA_SIZE
            if fail_offset + BASE_DATA_SIZE <= len(rom):
                print(f"\nFirst failing record (would be #{winner.run_length + 1}):")
                dex_no = rom[fail_offset]
                stats = list(rom[fail_offset+1:fail_offset+7])
                type1, type2 = rom[fail_offset+7], rom[fail_offset+8]
                growth = rom[fail_offset+22]
                print(f"  DEX={dex_no} stats={stats} types={type1},{type2} growth={growth}")


if __name__ == "__main__":
    main()
