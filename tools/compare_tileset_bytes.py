#!/usr/bin/env python3
"""
Compare Enginemon's decompressed tileset bytes against pokecrystal source PNGs.

This script:
1. Reads the pokecrystal source PNG and converts to 2bpp bytes
2. Decompresses the ROM tileset using Python LZ3 implementation
3. Compares byte-for-byte and reports differences
"""

import sys
import os
from pathlib import Path

# Add references path for imports
sys.path.insert(0, str(Path(__file__).parent.parent / "references" / "Gen2Recomped"))

def load_rom(rom_path):
    """Load ROM bytes"""
    with open(rom_path, 'rb') as f:
        return f.read()

def bank_to_flat(bank, addr):
    """Convert bank:address to flat ROM offset"""
    if bank == 0:
        return addr
    return bank * 0x4000 + (addr - 0x4000)

# Tileset table address from Enginemon profile (13:5596)
TILESETS_TABLE = bank_to_flat(0x13, 0x5596)
TILESET_ENTRY_SIZE = 15  # 15 bytes per tileset entry

# Tileset indices and PNG paths
TILESETS = {
    'lab': {
        'index': 10,  # TILESET_LAB = 0x0A
        'png_path': 'references/pokecrystal/gfx/tilesets/lab.png'
    },
    'johto': {
        'index': 1,   # TILESET_JOHTO = 0x01
        'png_path': 'references/pokecrystal/gfx/tilesets/johto.png'
    }
}

def get_tileset_gfx_addr(rom, tileset_index):
    """Read GFX bank:address from Tilesets table entry"""
    entry_addr = TILESETS_TABLE + (tileset_index * TILESET_ENTRY_SIZE)
    # Entry format:
    # 0: gfx_bank
    # 1-2: gfx_ptr (little-endian)
    gfx_bank = rom[entry_addr]
    gfx_ptr = rom[entry_addr + 1] | (rom[entry_addr + 2] << 8)
    return gfx_bank, gfx_ptr

def decompress_lz3(rom, addr):
    """
    Pokemon GSC LZ3 decompression.
    Ported from pokecrystal home/decompress.asm
    """
    LZ_END = 0xFF
    
    out = bytearray()
    ptr = addr
    
    def flip_byte(b):
        result = 0
        for i in range(8):
            result = (result << 1) | (b & 1)
            b >>= 1
        return result
    
    while ptr < len(rom):
        control = rom[ptr]
        
        if control == LZ_END:
            break
            
        cmd = (control >> 5) & 0x07
        
        if cmd == 7:  # LZ_LONG
            # Extended length: 10-bit count
            cmd = (control >> 2) & 0x07
            hi = control & 0x03
            ptr += 1
            if ptr >= len(rom):
                break
            lo = rom[ptr]
            count = (hi << 8) | lo
            count += 1
            ptr += 1
        else:
            count = (control & 0x1F) + 1
            ptr += 1
        
        if cmd == 0:  # LZ_LITERAL
            for i in range(count):
                if ptr >= len(rom):
                    break
                out.append(rom[ptr])
                ptr += 1
                
        elif cmd == 1:  # LZ_ITERATE
            if ptr >= len(rom):
                break
            byte = rom[ptr]
            ptr += 1
            for i in range(count):
                out.append(byte)
                
        elif cmd == 2:  # LZ_ALTERNATE
            if ptr + 1 >= len(rom):
                break
            b1 = rom[ptr]
            b2 = rom[ptr + 1]
            ptr += 2
            for i in range(count):
                out.append(b2 if (i & 1) else b1)
                
        elif cmd == 3:  # LZ_ZERO
            for i in range(count):
                out.append(0)
                
        elif cmd in (4, 5, 6):  # LZ_REPEAT, LZ_FLIP, LZ_REVERSE
            if ptr >= len(rom):
                break
            offset_byte = rom[ptr]
            ptr += 1
            
            if offset_byte & 0x80:
                # Negative offset from current position
                neg_offset = offset_byte & 0x7F
                if neg_offset > len(out):
                    break
                src_pos = len(out) - neg_offset - 1
            else:
                # Positive offset from start (15-bit)
                if ptr >= len(rom):
                    break
                lo = rom[ptr]
                ptr += 1
                pos_offset = (offset_byte << 8) | lo
                src_pos = pos_offset
                if src_pos >= len(out):
                    break
            
            if cmd == 4:  # LZ_REPEAT
                for i in range(count):
                    out.append(out[src_pos + i])
            elif cmd == 5:  # LZ_FLIP
                for i in range(count):
                    out.append(flip_byte(out[src_pos + i]))
            elif cmd == 6:  # LZ_REVERSE
                for i in range(count):
                    out.append(out[src_pos - i])
    
    return bytes(out), ptr - addr

def png_to_2bpp(png_path):
    """
    Convert a grayscale PNG to Game Boy 2bpp format.
    pokecrystal PNGs use 4 shades: white=0, light=1, dark=2, black=3
    """
    try:
        from PIL import Image
    except ImportError:
        print("ERROR: PIL/Pillow not installed. Install with: pip install Pillow")
        sys.exit(1)
    
    img = Image.open(png_path)
    if img.mode != 'L':
        img = img.convert('L')
    
    width, height = img.size
    pixels = list(img.getdata())
    
    # pokecrystal uses: white(255)=0, light(170)=1, dark(85)=2, black(0)=3
    def pixel_to_index(p):
        if p > 200: return 0  # white
        if p > 128: return 1  # light gray
        if p > 64: return 2   # dark gray
        return 3              # black
    
    out = bytearray()
    tiles_x = width // 8
    tiles_y = height // 8
    
    for ty in range(tiles_y):
        for tx in range(tiles_x):
            # Extract 8x8 tile
            for row in range(8):
                lo_byte = 0
                hi_byte = 0
                for col in range(8):
                    px = ty * 8 + row
                    py = tx * 8 + col
                    idx = pixels[px * width + py]
                    color = pixel_to_index(idx)
                    
                    bit = 7 - col
                    lo_byte |= ((color & 1) << bit)
                    hi_byte |= (((color >> 1) & 1) << bit)
                
                out.append(lo_byte)
                out.append(hi_byte)
    
    return bytes(out)

def compare_bytes(expected, actual, name):
    """Compare two byte arrays and report differences."""
    print(f"\n{'='*60}")
    print(f"TILESET: {name}")
    print(f"{'='*60}")
    
    print(f"Expected size: {len(expected)} bytes")
    print(f"Actual size:   {len(actual)} bytes")
    
    if len(expected) != len(actual):
        print(f"SIZE MISMATCH: {len(actual) - len(expected):+d} bytes")
    
    # Find all differences
    min_len = min(len(expected), len(actual))
    differences = []
    
    for i in range(min_len):
        if expected[i] != actual[i]:
            differences.append((i, expected[i], actual[i]))
    
    # Count extra bytes as differences too
    if len(actual) > len(expected):
        for i in range(len(expected), len(actual)):
            differences.append((i, None, actual[i]))
    elif len(expected) > len(actual):
        for i in range(len(actual), len(expected)):
            differences.append((i, expected[i], None))
    
    print(f"Total differing bytes: {len(differences)}")
    
    if not differences:
        print("✓ BYTE-IDENTICAL")
        return True
    
    print(f"\nFirst differing byte offset: 0x{differences[0][0]:04X} ({differences[0][0]})")
    
    print(f"\nFirst 32 differences:")
    print(f"{'Offset':>8} | {'Expected':>8} | {'Actual':>8}")
    print("-" * 30)
    
    for i, (offset, exp, act) in enumerate(differences[:32]):
        exp_str = f"0x{exp:02X}" if exp is not None else "N/A"
        act_str = f"0x{act:02X}" if act is not None else "N/A"
        print(f"0x{offset:06X} | {exp_str:>8} | {act_str:>8}")
    
    return False

def main():
    if len(sys.argv) < 2:
        print("Usage: python compare_tileset_bytes.py <rom_path>")
        sys.exit(1)
    
    rom_path = sys.argv[1]
    if not os.path.exists(rom_path):
        print(f"ROM not found: {rom_path}")
        sys.exit(1)
    
    print("Loading ROM...")
    rom = load_rom(rom_path)
    print(f"ROM size: {len(rom)} bytes")
    
    results = {}
    
    for name, info in TILESETS.items():
        print(f"\n{'='*60}")
        print(f"Processing tileset: {name}")
        
        # Get GFX address from Tilesets table
        gfx_bank, gfx_ptr = get_tileset_gfx_addr(rom, info['index'])
        flat_addr = bank_to_flat(gfx_bank, gfx_ptr)
        print(f"Tileset index: {info['index']}")
        print(f"ROM address: {gfx_bank:02X}:{gfx_ptr:04X} = 0x{flat_addr:05X}")
        
        decompressed, compressed_size = decompress_lz3(rom, flat_addr)
        print(f"Compressed size: {compressed_size} bytes")
        print(f"Decompressed size: {len(decompressed)} bytes")
        print(f"Tiles: {len(decompressed) // 16}")
        
        # Convert reference PNG to 2bpp
        png_path = info['png_path']
        if not os.path.exists(png_path):
            print(f"WARNING: Reference PNG not found: {png_path}")
            continue
        
        print(f"Converting reference PNG: {png_path}")
        expected = png_to_2bpp(png_path)
        
        # Compare
        match = compare_bytes(expected, decompressed, name)
        results[name] = match
    
    print("\n" + "="*60)
    print("SUMMARY")
    print("="*60)
    
    for name, match in results.items():
        status = "✓ MATCH" if match else "✗ DIFFERS"
        print(f"{name}: {status}")
    
    # Answer the question
    print("\n" + "="*60)
    lab_ok = results.get('lab', False)
    outdoor_ok = results.get('johto', False)
    
    if lab_ok and outdoor_ok:
        print("CONCLUSION: Decompression is CORRECT for both Lab and Johto.")
        print("Compression is EXONERATED - look elsewhere for rendering bug.")
    elif not lab_ok and outdoor_ok:
        print("CONCLUSION: Lab DIFFERS but Johto MATCHES.")
        print("DECOMPRESSION IS THE BUG CLASS for indoor tilesets.")
    elif not lab_ok and not outdoor_ok:
        print("CONCLUSION: BOTH tilesets have decompression errors.")
        print("Fundamental LZ3 decompression bug.")
    else:
        print("CONCLUSION: Johto DIFFERS but Lab MATCHES - unexpected pattern.")

if __name__ == "__main__":
    main()
