#!/usr/bin/env python3
"""
ROM-Only Species Domain Discovery - Architecture Audit Prototype
Analyzes Crystal-compatible ROMs to discover species count without .sym files.

This is an AUDIT ONLY tool - it does not modify any production code.
"""

import sys
import hashlib
from dataclasses import dataclass
from typing import Optional, List, Tuple

# Known vanilla Crystal v1.1 constants for verification
VANILLA_SHA1 = "f2f52230b536214ef7c9924f483392993e226cfb"
VANILLA_BASE_DATA_OFFSET = 0x51424  # 14:5424 = 0x14 * 0x4000 + 0x5424 - 0x4000 = 0x50000 + 0x1424

# BaseData record structure (from pokemon_data_constants.asm):
# NUM_TMS = 50, NUM_HMS = 7, NUM_TUTORS = 3 -> NUM_TM_HM_TUTOR = 60
# BASE_TMHM = (60 + 7) / 8 = 8 bytes
# Total: 1 + 6 + 2 + 1 + 1 + 2 + 1 + 1 + 1 + 1 + 1 + 2 + 2 + 1 + 1 + 8 = 32 bytes
BASE_DATA_SIZE = 32

# Species name length (fixed, padded with 0x50 terminator)
NAME_LENGTH = 10  # Actually NAME_LENGTH - 1 = 9 chars + 0x50 terminator

# Cry record size: dw cry_id, dw pitch, dw length = 6 bytes
MON_CRY_LENGTH = 6

# Palette record size: 4 colors (2 normal + 2 shiny) * 2 bytes each = 8 bytes
PALETTE_ENTRY_SIZE = 8

# Pic pointer entry: dba front, dba back = 6 bytes
PIC_POINTER_SIZE = 6

# EvosAttacks pointer entry: 2 bytes
EVOS_ATTACKS_POINTER_SIZE = 2

# Pokedex entry pointer: 2 bytes
DEX_ENTRY_POINTER_SIZE = 2


@dataclass
class SpeciesDiscoveryResult:
    """Result of species domain discovery."""
    success: bool
    species_count: int
    base_data_location: int
    evidence: dict


def flat_offset(bank: int, addr: int) -> int:
    """Convert bank:address to flat ROM offset."""
    if addr < 0x4000:
        return addr
    return bank * 0x4000 + (addr - 0x4000)


def read_u8(rom: bytes, offset: int) -> int:
    return rom[offset]


def read_u16_le(rom: bytes, offset: int) -> int:
    return rom[offset] | (rom[offset + 1] << 8)


def validate_base_stats(rom: bytes, offset: int) -> Tuple[bool, str]:
    """
    Validate that a BaseData record at offset looks plausible.
    Returns (valid, reason).
    
    BaseData structure:
    - byte 0: species ID (db DEX_NO)
    - bytes 1-6: HP, ATK, DEF, SPD, SAT, SDF (each 1-255)
    - bytes 7-8: types (0-17 each)
    - byte 9: catch rate (1-255)
    - byte 10: base exp (1-255)
    - bytes 11-12: held items
    - byte 13: gender ratio
    - byte 14: unknown (always 100 in vanilla)
    - byte 15: egg cycles (1-120 typical)
    - byte 16: unknown
    - byte 17: pic size
    - bytes 18-21: unused pointers
    - byte 22: growth rate (0-5)
    - byte 23: egg groups (packed nibbles)
    - bytes 24-31: TM/HM flags (8 bytes)
    """
    if offset + BASE_DATA_SIZE > len(rom):
        return False, "out of bounds"
    
    species_id = rom[offset]
    if species_id == 0:
        return False, "zero species ID"
    
    # Check stats are non-zero (base stats are always >= 1)
    hp, atk, def_, spd, sat, sdf = rom[offset+1:offset+7]
    if hp == 0 or atk == 0 or def_ == 0 or spd == 0 or sat == 0 or sdf == 0:
        return False, "zero base stat"
    
    # Check types are valid
    # Crystal types are NOT contiguous 0-17. They are:
    # PHYSICAL: 0-9 (NORMAL through STEEL)
    # UNUSED: 10-18 (gap)
    # SPECIAL: 20-27 (FIRE through DARK)
    # Plus CURSE_TYPE = 19
    type1, type2 = rom[offset+7], rom[offset+8]
    valid_types = set(range(0, 10)) | {19} | set(range(20, 28))
    if type1 not in valid_types or type2 not in valid_types:
        return False, f"invalid type {type1}/{type2}"
    
    # Check catch rate (0 is invalid for regular mons, only Pokeballs have 0)
    catch_rate = rom[offset+9]
    # Actually some mons have catch rate 3 (very rare), but 0 would be weird
    
    # Check growth rate (0-5)
    growth_rate = rom[offset+22]
    if growth_rate > 5:
        return False, f"invalid growth rate {growth_rate}"
    
    return True, "ok"


def scan_base_data_table(rom: bytes, start_offset: int, max_species: int = 300) -> int:
    """
    Scan BaseData table starting at offset, return discovered species count.
    Returns 0 if table doesn't validate.
    """
    count = 0
    for i in range(max_species):
        offset = start_offset + i * BASE_DATA_SIZE
        valid, reason = validate_base_stats(rom, offset)
        if not valid:
            # Check if this is end of table (first byte after valid entries)
            # In vanilla, entries 252-255 exist but have different format
            # Let's check if species ID matches expected sequence
            species_id = rom[offset] if offset < len(rom) else 0
            expected_id = i + 1
            if species_id != expected_id:
                break
        count += 1
        
        # Verify species ID matches position
        species_id = rom[offset]
        expected_id = i + 1
        if species_id != expected_id:
            print(f"  Warning: species ID mismatch at index {i}: got {species_id}, expected {expected_id}")
    
    return count


def find_base_data_by_signature(rom: bytes) -> List[int]:
    """
    Scan ROM for potential BaseData table locations using structural signatures.
    
    Signature: Sequential species IDs (1, 2, 3...) at fixed 32-byte intervals,
    with each entry having plausible base stat values.
    """
    candidates = []
    
    # Scan ROM in typical data bank ranges (banks 0x10-0x20 for Crystal)
    for bank in range(0x10, 0x30):
        bank_start = bank * 0x4000
        bank_end = bank_start + 0x4000
        
        for offset in range(bank_start, min(bank_end, len(rom) - BASE_DATA_SIZE * 10)):
            # Quick check: first species ID should be 1 (Bulbasaur)
            if rom[offset] != 1:
                continue
            
            # Check that we have sequential IDs at 32-byte intervals
            sequential = True
            for i in range(1, 10):
                check_offset = offset + i * BASE_DATA_SIZE
                if check_offset >= len(rom) or rom[check_offset] != i + 1:
                    sequential = False
                    break
            
            if not sequential:
                continue
            
            # Validate first 10 entries structurally
            all_valid = True
            for i in range(10):
                valid, _ = validate_base_stats(rom, offset + i * BASE_DATA_SIZE)
                if not valid:
                    all_valid = False
                    break
            
            if all_valid:
                candidates.append(offset)
    
    return candidates


def count_names_table(rom: bytes, offset: int, max_count: int = 300) -> int:
    """
    Count entries in PokemonNames table.
    Each entry is 10 bytes (NAME_LENGTH).
    Names use Crystal encoding (uppercase A-Z = 0x80-0x99, terminator = 0x50).
    """
    name_entry_size = 10  # NAME_LENGTH from profile
    count = 0
    
    for i in range(max_count):
        entry_offset = offset + i * name_entry_size
        if entry_offset + name_entry_size > len(rom):
            break
        
        name_bytes = rom[entry_offset:entry_offset + name_entry_size]
        
        # First char should be a valid Crystal letter (0x80-0x99) or special char
        first = name_bytes[0]
        if first == 0x50 or first == 0x00:
            # Starts with terminator or null - end of real entries
            break
        
        # Valid name chars in Crystal: uppercase 0x80-0x99, special chars, digits
        # Check that there's a terminator (0x50) somewhere in the entry
        if 0x50 not in name_bytes:
            # Names after species might be "?????" which doesn't have normal chars
            # But they should still have terminators. If no terminator, likely garbage.
            break
        
        count += 1
    
    return count


def count_cries_table(rom: bytes, offset: int, max_count: int = 300) -> int:
    """
    Count entries in PokemonCries table.
    Each entry is 6 bytes: dw cry_id, dw pitch, dw length
    Table has 251 real entries then 4 padding entries to 255.
    """
    count = 0
    
    for i in range(max_count):
        entry_offset = offset + i * MON_CRY_LENGTH
        if entry_offset + MON_CRY_LENGTH > len(rom):
            break
        
        cry_id = read_u16_le(rom, entry_offset)
        pitch = read_u16_le(rom, entry_offset + 2)
        length = read_u16_le(rom, entry_offset + 4)
        
        # Cry IDs are 1-based indices into cry data, should be small (< 50 unique)
        # BUT pitch can be negative (signed), and length can be large
        # Looking at cries.asm, cry_id is a constant like CRY_BULBASAUR
        # In vanilla there are about 38 unique cries
        
        # Check for obviously invalid entries (all zeros)
        if cry_id == 0 and pitch == 0 and length == 0:
            # This could be a padding entry (like 252-255)
            # Keep counting but note it
            pass
        
        count += 1
        
        # Stop at obviously non-cry data
        if cry_id > 100:  # Unreasonably high cry ID
            break
    
    return count


def count_evos_attacks_pointers(rom: bytes, offset: int, max_count: int = 300) -> int:
    """
    Count entries in EvosAttacksPointers table.
    Each entry is a 2-byte pointer.
    """
    count = 0
    
    for i in range(max_count):
        entry_offset = offset + i * EVOS_ATTACKS_POINTER_SIZE
        if entry_offset + EVOS_ATTACKS_POINTER_SIZE > len(rom):
            break
        
        ptr = read_u16_le(rom, entry_offset)
        
        # Pointers should be in ROM range (0x4000-0x7FFF for banked)
        if ptr < 0x4000 or ptr >= 0x8000:
            break
        
        count += 1
    
    return count


def count_pic_pointers(rom: bytes, offset: int, max_count: int = 300) -> int:
    """
    Count entries in PokemonPicPointers table.
    Each entry is 6 bytes: dba front, dba back
    
    Special cases:
    - Unown (entry 201) has dba_pics ; empty = 0xff:ffff, 0xff:ffff
    - Entry 252 is unused (same 0xff pattern)
    - Entry 253 is Egg (only front sprite, back = 0xff:ffff)
    """
    count = 0
    
    for i in range(max_count):
        entry_offset = offset + i * PIC_POINTER_SIZE
        if entry_offset + PIC_POINTER_SIZE > len(rom):
            break
        
        # dba format: bank, addr_lo, addr_hi
        front_bank = rom[entry_offset]
        front_addr = read_u16_le(rom, entry_offset + 1)
        back_bank = rom[entry_offset + 3]
        back_addr = read_u16_le(rom, entry_offset + 4)
        
        # Special marker 0xff:ffff means placeholder entry
        if front_bank == 0xFF and front_addr == 0xFFFF:
            count += 1
            continue
        
        # Banks should be reasonable (< 0x80 for 2MB ROM)
        if front_bank > 0x7F:
            break
        
        # Addresses should be banked (0x4000-0x7FFF)
        if front_addr < 0x4000 or front_addr >= 0x8000:
            break
        
        # Back can also be 0xff:ffff for entries with no back sprite (like Egg)
        if back_bank != 0xFF:
            if back_bank > 0x7F:
                break
            if back_addr < 0x4000 or back_addr >= 0x8000:
                break
        
        count += 1
    
    return count


def discover_species_domain(rom: bytes) -> SpeciesDiscoveryResult:
    """
    Main discovery function. Returns species domain discovered from ROM.
    """
    evidence = {}
    
    # Step 1: Compute SHA1 for reference
    sha1 = hashlib.sha1(rom).hexdigest()
    evidence["sha1"] = sha1
    evidence["is_vanilla_v11"] = sha1 == VANILLA_SHA1
    
    # Step 2: Find BaseData table by signature scanning
    candidates = find_base_data_by_signature(rom)
    evidence["base_data_candidates"] = [hex(c) for c in candidates]
    
    if len(candidates) == 0:
        return SpeciesDiscoveryResult(
            success=False, species_count=0, base_data_location=0,
            evidence=evidence
        )
    
    if len(candidates) > 1:
        evidence["warning"] = "Multiple BaseData candidates found"
        # Could fail here, but let's try the first one for now
    
    base_data_offset = candidates[0]
    evidence["base_data_location"] = hex(base_data_offset)
    
    # Step 3: Count species in BaseData table
    base_data_count = scan_base_data_table(rom, base_data_offset)
    evidence["base_data_count"] = base_data_count
    
    # Step 4: Cross-validate with other tables (if we had their offsets)
    # For vanilla ROM, we know the offsets from the profile
    if evidence["is_vanilla_v11"]:
        # PokemonNames at 14:7384
        names_offset = flat_offset(0x14, 0x7384)
        names_count = count_names_table(rom, names_offset)
        evidence["names_offset"] = hex(names_offset)
        evidence["names_count"] = names_count
        
        # EvosAttacksPointers at 10:65b1
        evos_offset = flat_offset(0x10, 0x65b1)
        evos_count = count_evos_attacks_pointers(rom, evos_offset)
        evidence["evos_offset"] = hex(evos_offset)
        evidence["evos_count"] = evos_count
        
        # PokemonPicPointers at 48:4000
        pics_offset = flat_offset(0x48, 0x4000)
        pics_count = count_pic_pointers(rom, pics_offset)
        evidence["pics_offset"] = hex(pics_offset)
        evidence["pics_count"] = pics_count
        
        # Cross-validation
        evidence["cross_validation"] = {
            "base_data": base_data_count,
            "names": names_count,
            "evos_pointers": evos_count,
            "pic_pointers": pics_count
        }
        
        # All should agree (or have documented padding differences)
        # Names has extra entries (?????, EGG, etc.) up to 256
        # Pics has extra entries (unused, Egg) up to 253
        # EvosAttacks should match exactly
    
    # Step 5: Determine final species count
    # BaseData is the primary anchor - it has the species ID embedded
    species_count = base_data_count
    
    return SpeciesDiscoveryResult(
        success=True,
        species_count=species_count,
        base_data_location=base_data_offset,
        evidence=evidence
    )


def main():
    if len(sys.argv) < 2:
        print("Usage: python species_domain_audit.py <rom.gbc>")
        print("\nThis tool discovers species count from ROM structure without .sym files.")
        sys.exit(1)
    
    rom_path = sys.argv[1]
    
    with open(rom_path, "rb") as f:
        rom = f.read()
    
    print(f"ROM: {rom_path}")
    print(f"Size: {len(rom)} bytes ({len(rom) // 1024} KB)")
    print()
    
    result = discover_species_domain(rom)
    
    print("=" * 60)
    print("SPECIES DOMAIN DISCOVERY RESULTS")
    print("=" * 60)
    print()
    
    print(f"Success: {result.success}")
    print(f"Discovered Species Count: {result.species_count}")
    print(f"BaseData Location: {hex(result.base_data_location) if result.base_data_location else 'not found'}")
    print()
    
    print("Evidence:")
    for key, value in result.evidence.items():
        if isinstance(value, dict):
            print(f"  {key}:")
            for k, v in value.items():
                print(f"    {k}: {v}")
        else:
            print(f"  {key}: {value}")
    print()
    
    # Additional analysis for vanilla ROM
    if result.evidence.get("is_vanilla_v11"):
        print("VANILLA CRYSTAL v1.1 VERIFICATION:")
        print(f"  Expected BaseData offset: {hex(VANILLA_BASE_DATA_OFFSET)}")
        print(f"  Discovered offset: {hex(result.base_data_location)}")
        print(f"  Match: {result.base_data_location == VANILLA_BASE_DATA_OFFSET}")
        print()
        
        # Check cross-validation consistency
        if "cross_validation" in result.evidence:
            cv = result.evidence["cross_validation"]
            print("CROSS-VALIDATION:")
            
            # BaseData should be exactly 251
            base_ok = cv["base_data"] == 251
            print(f"  BaseData: {cv['base_data']} (expected 251) {'OK' if base_ok else 'FAIL'}")
            
            # EvosAttacks should be exactly 251
            evos_ok = cv["evos_pointers"] == 251
            print(f"  EvosAttacks: {cv['evos_pointers']} (expected 251) {'OK' if evos_ok else 'FAIL'}")
            
            # Names has 251 real + "?????" + "EGG" + padding to 256
            names_ok = cv["names"] >= 251
            print(f"  Names: {cv['names']} (expected >=251) {'OK' if names_ok else 'FAIL'}")
            
            # Pics has 251 real + unused + Egg = 253
            pics_ok = cv["pic_pointers"] >= 251
            print(f"  PicPointers: {cv['pic_pointers']} (expected >=251) {'OK' if pics_ok else 'FAIL'}")


if __name__ == "__main__":
    main()
