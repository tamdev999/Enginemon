#!/usr/bin/env python3
"""Check house tileset palette extraction."""

import sys

def load_rom(path):
    with open(path, 'rb') as f:
        return f.read()

def bank_to_flat(bank, addr):
    if bank == 0:
        return addr
    return bank * 0x4000 + (addr - 0x4000)

# GBC color format: 5 bits each for R, G, B in little-endian
# gggrrrrr 0bbbbbgg
def gbc_to_rgb(lo, hi):
    word = lo | (hi << 8)
    r = (word & 0x1F) * 8
    g = ((word >> 5) & 0x1F) * 8
    b = ((word >> 10) & 0x1F) * 8
    return (r, g, b)

def main():
    rom_path = sys.argv[1] if len(sys.argv) > 1 else 'references/Pokemon - Crystal Version (UE) (V1.1) [C][!].gbc'
    rom = load_rom(rom_path)
    
    # HousePalette is at 12:55EE (from pokecrystal11.sym)
    # 7 palettes x 4 colors x 2 bytes = 56 bytes
    HOUSE_PALETTE_ADDR = bank_to_flat(0x12, 0x55EE)
    print(f'HousePalette ROM address: 12:55EE = 0x{HOUSE_PALETTE_ADDR:05X}')
    
    # Read 56 bytes (7 palettes)
    pal_data = rom[HOUSE_PALETTE_ADDR:HOUSE_PALETTE_ADDR + 56]
    print(f'Raw palette bytes ({len(pal_data)} bytes):')
    for i in range(0, len(pal_data), 8):
        print(f'  {" ".join(f"{b:02X}" for b in pal_data[i:i+8])}')
    
    print()
    palette_names = ['gray', 'red', 'green', 'water', 'yellow', 'brown', 'roof']
    for pal_idx in range(7):
        base = pal_idx * 8
        print(f'Palette {pal_idx} ({palette_names[pal_idx]}):')
        for color_idx in range(4):
            offset = base + color_idx * 2
            lo, hi = pal_data[offset], pal_data[offset + 1]
            r, g, b = gbc_to_rgb(lo, hi)
            print(f'  Index {color_idx}: 0x{lo:02X} 0x{hi:02X} -> RGB({r:3d}, {g:3d}, {b:3d})')
    
    # Compare to pokecrystal .pal file values
    print()
    print('='*60)
    print('Expected from house.pal (RGB 0-31 scale, need *8 for 0-255):')
    print('='*60)
    expected = [
        # gray
        [(30,28,26), (19,19,19), (13,13,13), (7,7,7)],
        # red  
        [(30,28,26), (31,19,24), (30,10,6), (7,7,7)],
        # green
        [(30,28,26), (15,20,1), (9,13,0), (7,7,7)],
        # water
        [(30,28,26), (15,16,31), (9,9,31), (7,7,7)],
        # yellow
        [(30,28,26), (31,31,7), (31,16,1), (7,7,7)],
        # brown
        [(26,24,17), (21,17,7), (16,13,3), (7,7,7)],
        # roof
        [(30,28,26), (31,19,24), (16,13,3), (7,7,7)],
    ]
    
    for pal_idx, (name, colors) in enumerate(zip(palette_names, expected)):
        print(f'Palette {pal_idx} ({name}):')
        for color_idx, (r5, g5, b5) in enumerate(colors):
            r8, g8, b8 = r5*8, g5*8, b5*8
            print(f'  Index {color_idx}: RGB({r8:3d}, {g8:3d}, {b8:3d})  [from RGB5({r5:2d},{g5:2d},{b5:2d})]')
    
    # Now check if ROM matches expected
    print()
    print('='*60)
    print('COMPARISON: ROM vs Expected')
    print('='*60)
    
    mismatches = []
    for pal_idx in range(7):
        base = pal_idx * 8
        for color_idx in range(4):
            offset = base + color_idx * 2
            lo, hi = pal_data[offset], pal_data[offset + 1]
            r, g, b = gbc_to_rgb(lo, hi)
            
            exp_r5, exp_g5, exp_b5 = expected[pal_idx][color_idx]
            exp_r, exp_g, exp_b = exp_r5*8, exp_g5*8, exp_b5*8
            
            if (r, g, b) != (exp_r, exp_g, exp_b):
                mismatches.append((pal_idx, color_idx, (r,g,b), (exp_r, exp_g, exp_b)))
    
    if not mismatches:
        print('ALL MATCH - ROM palette extraction is correct')
    else:
        print(f'MISMATCHES: {len(mismatches)}')
        for pal_idx, color_idx, got, exp in mismatches[:5]:
            print(f'  Pal {pal_idx} Color {color_idx}: Got RGB{got}, Expected RGB{exp}')

if __name__ == '__main__':
    main()
