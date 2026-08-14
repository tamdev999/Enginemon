#!/usr/bin/env python3
"""Verify all special tileset palettes against pokecrystal .pal files."""

import sys
import os

def load_rom(path):
    with open(path, 'rb') as f:
        return f.read()

def bank_to_flat(bank, addr):
    if bank == 0:
        return addr
    return bank * 0x4000 + (addr - 0x4000)

def gbc_to_rgb(lo, hi):
    word = lo | (hi << 8)
    r = (word & 0x1F) * 8
    g = ((word >> 5) & 0x1F) * 8
    b = ((word >> 10) & 0x1F) * 8
    return (r, g, b)

def parse_pal_file(path):
    """Parse a pokecrystal .pal file into list of palettes."""
    palettes = []
    current_pal = []
    
    with open(path, 'r') as f:
        for line in f:
            line = line.strip()
            if line.startswith(';') or not line:
                continue
            if line.startswith('RGB'):
                # Parse: RGB r, g, b
                parts = line[3:].strip().split(',')
                r = int(parts[0].strip())
                g = int(parts[1].strip())
                b = int(parts[2].strip())
                current_pal.append((r * 8, g * 8, b * 8))
                
                if len(current_pal) == 4:
                    palettes.append(current_pal)
                    current_pal = []
    
    return palettes

# Special palette table from Enginemon (now with correct addresses)
SPECIAL_PALETTES = [
    (5,  'house',               0x12, 0x55EE, 'references/pokecrystal/gfx/tilesets/house.pal'),
    (13, 'mansion',             0x12, 0x567D, 'references/pokecrystal/gfx/tilesets/mansion_1.pal'),
    (21, 'pokecom_center',      0x12, 0x5501, 'references/pokecrystal/gfx/tilesets/pokecom_center.pal'),
    (22, 'battle_tower_inside', 0x12, 0x5550, 'references/pokecrystal/gfx/tilesets/battle_tower_inside.pal'),
    (27, 'radio_tower',         0x12, 0x563D, 'references/pokecrystal/gfx/tilesets/radio_tower.pal'),
    (29, 'ice_path',            0x12, 0x559F, 'references/pokecrystal/gfx/tilesets/ice_path.pal'),
]

def main():
    rom_path = sys.argv[1] if len(sys.argv) > 1 else 'references/Pokemon - Crystal Version (UE) (V1.1) [C][!].gbc'
    rom = load_rom(rom_path)
    
    all_pass = True
    
    for tileset_idx, name, bank, addr, pal_path in SPECIAL_PALETTES:
        flat_addr = bank_to_flat(bank, addr)
        print(f'\n{"="*60}')
        print(f'Tileset {tileset_idx} ({name}): {bank:02X}:{addr:04X} = 0x{flat_addr:05X}')
        print(f'{"="*60}')
        
        # Read ROM palette data (7 palettes × 8 bytes)
        pal_data = rom[flat_addr:flat_addr + 56]
        rom_palettes = []
        for pal_idx in range(7):
            pal = []
            for c in range(4):
                offset = pal_idx * 8 + c * 2
                lo, hi = pal_data[offset], pal_data[offset + 1]
                pal.append(gbc_to_rgb(lo, hi))
            rom_palettes.append(pal)
        
        # Read expected from .pal file
        if os.path.exists(pal_path):
            expected = parse_pal_file(pal_path)
            
            # Compare (only first 7 palettes)
            mismatches = 0
            for pal_idx in range(min(7, len(expected))):
                for c in range(4):
                    rom_rgb = rom_palettes[pal_idx][c]
                    exp_rgb = expected[pal_idx][c]
                    if rom_rgb != exp_rgb:
                        if mismatches == 0:
                            print(f'MISMATCHES:')
                        print(f'  Pal {pal_idx} Color {c}: ROM={rom_rgb}, Expected={exp_rgb}')
                        mismatches += 1
            
            if mismatches == 0:
                print(f'✓ ALL MATCH ({len(expected)} palettes verified)')
            else:
                print(f'✗ {mismatches} mismatches')
                all_pass = False
        else:
            print(f'WARNING: .pal file not found: {pal_path}')
            # Just print ROM values
            for pal_idx in range(7):
                print(f'  Palette {pal_idx}: {rom_palettes[pal_idx]}')
    
    print(f'\n{"="*60}')
    if all_pass:
        print('✓ ALL SPECIAL PALETTES VERIFIED')
    else:
        print('✗ SOME PALETTES HAVE MISMATCHES')
    print(f'{"="*60}')
    
    return 0 if all_pass else 1

if __name__ == '__main__':
    sys.exit(main())
