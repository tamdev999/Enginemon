#!/usr/bin/env python3
import sys

rom = open('references/Pokemon - Crystal Version (UE) (V1.1) [C][!].gbc', 'rb').read()

addrs = [0x18c03e, 0x18c040, 0x194b43]

for addr in addrs:
    print(f'=== Checking address 0x{addr:x} ===')
    bank = addr // 0x4000
    ptr = (addr % 0x4000) + 0x4000
    print(f'Bank: 0x{bank:02x}, Pointer: 0x{ptr:04x}')
    
    # Look at bytes before this address
    before = rom[addr-16:addr]
    print(f'16 bytes before: {" ".join(f"{b:02x}" for b in before)}')
    
    # And the address itself
    at = rom[addr:addr+16]
    print(f'16 bytes at addr: {" ".join(f"{b:02x}" for b in at)}')
    print()

# Now let's trace where these come from in the map extraction
# We need to find the map object that has this script_rom_address

print("=" * 60)
print("Tracing provenance...")
print("=" * 60)

# These addresses are TEXT, not SCRIPT
# The question is: why is the map extractor collecting these as script roots?

# Let's check what the actual script addresses should be for PewterCity
# The script label 'PewterCityCooltrainerFScript' should be a jumptextfaceplayer

# Actually - let me check the bytes at the LABEL position, not text position
# We need to find where PewterCityCooltrainerFScript is in the ROM

# Search for the jumptextfaceplayer opcode (0x51) near our addresses
print("\nSearching for jumptextfaceplayer (0x51) opcodes near these addresses...")

for addr in addrs:
    # Search in a range around the address
    search_start = max(0, addr - 256)
    search_end = min(len(rom), addr + 256)
    
    found = []
    for i in range(search_start, search_end):
        if rom[i] == 0x51:  # jumptextfaceplayer
            # Check if next 2 bytes form a valid pointer
            if i + 3 <= len(rom):
                ptr_lo = rom[i+1]
                ptr_hi = rom[i+2]
                target_ptr = ptr_lo | (ptr_hi << 8)
                # Calculate what flat address this would point to (same bank)
                script_bank = i // 0x4000
                if target_ptr >= 0x4000:
                    target_flat = script_bank * 0x4000 + (target_ptr - 0x4000)
                else:
                    target_flat = target_ptr
                
                # Check if this points to our problem address
                if target_flat == addr or abs(target_flat - addr) < 16:
                    found.append((i, target_flat))
    
    if found:
        print(f"\nFound potential jumptextfaceplayer for 0x{addr:x}:")
        for script_addr, text_target in found:
            print(f"  Script at 0x{script_addr:x} -> text at 0x{text_target:x}")
            print(f"  Bytes: {' '.join(f'{b:02x}' for b in rom[script_addr:script_addr+4])}")

# Let's also check what byte 0x9b means
print("\n" + "=" * 60)
print("Decoding the prefix bytes at problematic addresses...")
print("=" * 60)

for addr in addrs:
    first_bytes = rom[addr:addr+4]
    print(f"\n0x{addr:x}: {' '.join(f'{b:02x}' for b in first_bytes)}")
    
    if first_bytes[0] == 0x9b:
        # 0x9b is the text_far opcode in Crystal scripting!
        print(f"  FOUND: 0x9b is the 'text_far' / 'text_jump' command!")
        print(f"  This reads far text from a different bank")
        # The next 2 bytes are the pointer, and the byte after that is the bank
        ptr = first_bytes[1] | (first_bytes[2] << 8)
        bank_byte = first_bytes[3] if len(first_bytes) > 3 else 0
        print(f"  Pointer: 0x{ptr:04x}, Bank: 0x{bank_byte:02x}")
