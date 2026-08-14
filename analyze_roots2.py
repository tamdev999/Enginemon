#!/usr/bin/env python3
"""
Trace the provenance of problematic script root addresses.
These addresses contain TEXT data but are being collected as script roots.
"""

rom = open('references/Pokemon - Crystal Version (UE) (V1.1) [C][!].gbc', 'rb').read()

# From Crystal ROM format:
# MapGroup pointers at bank 0x25, starting at offset in profile

# Let's manually trace PewterCity (where the text "Have you visited PEWTER GYM" is from)
# PewterCity should be in MapGroup_Kanto

# From pokecrystal/data/maps/maps.asm:
# MapGroup_Kanto is group 4

# MapGroup pointer table at 0x25:4000
# Each group entry is a 2-byte pointer to the group's map list
MAP_GROUP_POINTERS = 0x94000  # bank 0x25:4000

print("=== Tracing PewterCity map ===")
print()

# Read group 4's pointer (0-indexed, so group 4 is index 3)
# Actually Crystal groups are 1-indexed in the ROM
group_ptr_addr = MAP_GROUP_POINTERS + (3 * 2)  # Group 4 = index 3
group_ptr_lo = rom[group_ptr_addr]
group_ptr_hi = rom[group_ptr_addr + 1]
group_ptr = group_ptr_lo | (group_ptr_hi << 8)
print(f"Group 4 pointer at 0x{group_ptr_addr:x}: 0x{group_ptr:04x}")

# Convert to flat address (bank 0x25)
group_flat = 0x25 * 0x4000 + (group_ptr - 0x4000)
print(f"Group 4 data at flat addr: 0x{group_flat:x}")

# Each map entry is 9 bytes (MAP_LENGTH from pokecrystal)
# Format: bank, tileset, environment, attr_ptr(2), location, music, phone_palette, fishgroup

# Let's find PewterCity in this group
# From maps.asm, PewterCity should be map index 14 in MapGroup_Kanto
map_index = 14  # 1-indexed
map_entry_addr = group_flat + ((map_index - 1) * 9)

print(f"\nPewterCity map entry at 0x{map_entry_addr:x}:")
entry = rom[map_entry_addr:map_entry_addr+9]
print(f"  Raw bytes: {' '.join(f'{b:02x}' for b in entry)}")

attr_bank = entry[0]
tileset = entry[1]
environment = entry[2]
attr_ptr = entry[3] | (entry[4] << 8)
print(f"  Attributes bank: 0x{attr_bank:02x}")
print(f"  Attributes pointer: 0x{attr_ptr:04x}")

# Calculate flat address of MapAttributes
attr_flat = attr_bank * 0x4000 + (attr_ptr - 0x4000)
print(f"  Attributes flat addr: 0x{attr_flat:x}")

# MapAttributes header format (12 bytes):
# 0: border_block
# 1: height
# 2: width
# 3-4: blocks_ptr
# 5: blocks_bank
# 6-7: scripts_ptr
# 8: scripts_bank
# 9: connections
# 10-11: events_ptr

header = rom[attr_flat:attr_flat+12]
print(f"\nMapAttributes header at 0x{attr_flat:x}:")
print(f"  Raw bytes: {' '.join(f'{b:02x}' for b in header)}")

border = header[0]
height = header[1]
width = header[2]
blocks_ptr = header[3] | (header[4] << 8)
blocks_bank = header[5]
scripts_ptr = header[6] | (header[7] << 8)
scripts_bank = header[8]
connections = header[9]
events_ptr = header[10] | (header[11] << 8)

print(f"  Dimensions: {width}x{height}")
print(f"  Scripts bank: 0x{scripts_bank:02x}, ptr: 0x{scripts_ptr:04x}")
print(f"  Events ptr: 0x{events_ptr:04x} (in scripts bank)")

# Calculate events flat address
events_flat = scripts_bank * 0x4000 + (events_ptr - 0x4000)
print(f"  Events flat addr: 0x{events_flat:x}")

# Read events header
# Format: 2 filler bytes, then:
#   warp_count, warp_data...
#   coord_count, coord_data...
#   bg_count, bg_data...
#   object_count, object_data...

print(f"\nEvents at 0x{events_flat:x}:")
filler = rom[events_flat:events_flat+2]
print(f"  Filler: {filler[0]:02x} {filler[1]:02x}")

ptr = events_flat + 2

# Skip warps
warp_count = rom[ptr]
print(f"  Warp count: {warp_count}")
ptr += 1 + (warp_count * 5)  # 5 bytes per warp

# Skip coord events  
coord_count = rom[ptr]
print(f"  Coord event count: {coord_count}")
ptr += 1 + (coord_count * 8)  # 8 bytes per coord event

# Skip BG events
bg_count = rom[ptr]
print(f"  BG event count: {bg_count}")
ptr += 1 + (bg_count * 5)  # 5 bytes per bg event

# Now read object events
obj_count = rom[ptr]
print(f"  Object event count: {obj_count}")
ptr += 1

print(f"\nObject events:")
# Each object event is 13 bytes
for i in range(min(obj_count, 5)):
    obj = rom[ptr:ptr+13]
    print(f"  Object {i}:")
    print(f"    Raw: {' '.join(f'{b:02x}' for b in obj)}")
    
    sprite = obj[0]
    y = obj[1] - 4
    x = obj[2] - 4
    movement = obj[3]
    obj_type = obj[7] & 0x0F
    sight_range = obj[8]
    script_ptr = obj[9] | (obj[10] << 8)
    flag = obj[11] | (obj[12] << 8)
    
    print(f"    Sprite: 0x{sprite:02x}, Pos: ({x},{y})")
    print(f"    Object type: {obj_type} ({'SCRIPT' if obj_type == 0 else 'TRAINER' if obj_type == 2 else 'ITEMBALL' if obj_type == 1 else 'OTHER'})")
    print(f"    Script ptr: 0x{script_ptr:04x}")
    
    # Calculate flat script address
    script_flat = scripts_bank * 0x4000 + (script_ptr - 0x4000)
    print(f"    Script flat addr: 0x{script_flat:x}")
    
    # Show first few bytes at script address
    script_bytes = rom[script_flat:script_flat+8]
    print(f"    Bytes at script: {' '.join(f'{b:02x}' for b in script_bytes)}")
    
    # Check if this matches our problem addresses
    if script_flat in [0x18c03e, 0x18c040, 0x194b43]:
        print(f"    *** MATCH: This is one of the problem addresses! ***")
    
    ptr += 13

print()
print("=" * 60)
print("Now checking if problem addresses are actually object script pointers...")
print("=" * 60)

# The problem addresses
problem_addrs = [0x18c03e, 0x18c040, 0x194b43]

for prob_addr in problem_addrs:
    prob_bank = prob_addr // 0x4000
    prob_ptr = (prob_addr % 0x4000) + 0x4000
    print(f"\n0x{prob_addr:x} = bank 0x{prob_bank:02x}:0x{prob_ptr:04x}")
    
    # Search all map objects for this script pointer
    # This is brute force but will find it
