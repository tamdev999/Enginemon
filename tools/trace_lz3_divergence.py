#!/usr/bin/env python3
"""
Trace LZ3 decompression command-by-command to find the first divergence.

Reference: pokecrystal/home/decompress.asm, suiCune/home/decompress.c
"""

import sys
import os

# Tileset table address from Enginemon profile (13:5596)
def bank_to_flat(bank, addr):
    if bank == 0:
        return addr
    return bank * 0x4000 + (addr - 0x4000)

TILESETS_TABLE = bank_to_flat(0x13, 0x5596)
TILESET_ENTRY_SIZE = 15

# Command constants from pokecrystal
LZ_END = 0xFF
LZ_CMD = 0b11100000
LZ_LEN = 0b00011111
LZ_LONG_HI = 0b00000011

LZ_LITERAL   = 0 << 5  # 0x00
LZ_ITERATE   = 1 << 5  # 0x20
LZ_ALTERNATE = 2 << 5  # 0x40
LZ_ZERO      = 3 << 5  # 0x60
LZ_REPEAT    = 4 << 5  # 0x80
LZ_FLIP      = 5 << 5  # 0xA0
LZ_REVERSE   = 6 << 5  # 0xC0
LZ_LONG      = 7 << 5  # 0xE0

LZ_RW = 2 + 5  # bit position for rewrite commands

CMD_NAMES = {
    LZ_LITERAL:   "LITERAL",
    LZ_ITERATE:   "ITERATE",
    LZ_ALTERNATE: "ALTERNATE",
    LZ_ZERO:      "ZERO",
    LZ_REPEAT:    "REPEAT",
    LZ_FLIP:      "FLIP",
    LZ_REVERSE:   "REVERSE",
}

def flip_byte(b):
    """Bit-reverse a byte (MSB <-> LSB)"""
    result = 0
    for i in range(8):
        result = (result << 1) | (b & 1)
        b >>= 1
    return result

def get_tileset_gfx_addr(rom, tileset_index):
    entry_addr = TILESETS_TABLE + (tileset_index * TILESET_ENTRY_SIZE)
    gfx_bank = rom[entry_addr]
    gfx_ptr = rom[entry_addr + 1] | (rom[entry_addr + 2] << 8)
    return gfx_bank, gfx_ptr

class LZ3Tracer:
    """Reference LZ3 decompressor with full command tracing."""
    
    def __init__(self, rom, start_addr):
        self.rom = rom
        self.start_addr = start_addr
        self.ptr = start_addr
        self.out = bytearray()
        self.commands = []
        self.lz_address = 0  # Output start address (for positive offsets)
    
    def read_byte(self):
        b = self.rom[self.ptr]
        self.ptr += 1
        return b
    
    def decompress_with_trace(self):
        """Decompress and trace each command."""
        self.lz_address = 0  # Simulated output start
        
        while self.ptr < len(self.rom):
            cmd_start = self.ptr
            control = self.rom[self.ptr]
            
            if control == LZ_END:
                self.commands.append({
                    'compressed_offset': cmd_start - self.start_addr,
                    'type': 'END',
                    'raw_bytes': [control],
                    'output_start': len(self.out),
                    'output_len': 0,
                })
                break
            
            cmd = control & LZ_CMD
            
            if cmd == LZ_LONG:
                # Extended length: 10-bit count
                # 111xxxyy yyyyyyyy
                new_cmd = (control << 3) & LZ_CMD
                hi = control & LZ_LONG_HI
                self.ptr += 1
                lo = self.rom[self.ptr]
                self.ptr += 1
                count = ((hi << 8) | lo) + 1
                cmd = new_cmd
                raw_bytes = [control, lo]
            else:
                count = (control & LZ_LEN) + 1
                self.ptr += 1
                raw_bytes = [control]
            
            output_start = len(self.out)
            
            # Check if it's a rewrite command (bit 7 set in cmd >> 5)
            is_rewrite = (cmd >> 5) >= 4
            
            if is_rewrite:
                # Read offset
                offset_byte = self.rom[self.ptr]
                raw_bytes.append(offset_byte)
                self.ptr += 1
                
                if offset_byte & 0x80:
                    # Negative offset (7-bit) from current output position
                    neg_offset = offset_byte & 0x7F
                    src_pos = len(self.out) - neg_offset - 1
                    offset_type = 'negative'
                    offset_value = -(neg_offset + 1)
                else:
                    # Positive offset (15-bit) from start
                    lo = self.rom[self.ptr]
                    raw_bytes.append(lo)
                    self.ptr += 1
                    pos_offset = (offset_byte << 8) | lo
                    src_pos = pos_offset
                    offset_type = 'positive'
                    offset_value = pos_offset
                
                if cmd == LZ_REPEAT:
                    for i in range(count):
                        if src_pos + i < len(self.out):
                            self.out.append(self.out[src_pos + i])
                        else:
                            self.out.append(0)  # Out of bounds
                elif cmd == LZ_FLIP:
                    for i in range(count):
                        if src_pos + i < len(self.out):
                            self.out.append(flip_byte(self.out[src_pos + i]))
                        else:
                            self.out.append(0)
                elif cmd == LZ_REVERSE:
                    for i in range(count):
                        if src_pos - i >= 0:
                            self.out.append(self.out[src_pos - i])
                        else:
                            self.out.append(0)
                
                self.commands.append({
                    'compressed_offset': cmd_start - self.start_addr,
                    'type': CMD_NAMES.get(cmd, f'UNKNOWN_{cmd:02X}'),
                    'count': count,
                    'raw_bytes': raw_bytes,
                    'offset_type': offset_type,
                    'offset_value': offset_value,
                    'src_pos': src_pos,
                    'output_start': output_start,
                    'output_len': len(self.out) - output_start,
                    'output_bytes': list(self.out[output_start:]),
                })
            else:
                if cmd == LZ_LITERAL:
                    for i in range(count):
                        raw_bytes.append(self.rom[self.ptr])
                        self.out.append(self.rom[self.ptr])
                        self.ptr += 1
                elif cmd == LZ_ITERATE:
                    byte = self.rom[self.ptr]
                    raw_bytes.append(byte)
                    self.ptr += 1
                    for i in range(count):
                        self.out.append(byte)
                elif cmd == LZ_ALTERNATE:
                    b1 = self.rom[self.ptr]
                    b2 = self.rom[self.ptr + 1]
                    raw_bytes.extend([b1, b2])
                    self.ptr += 2
                    for i in range(count):
                        self.out.append(b2 if (i & 1) else b1)
                elif cmd == LZ_ZERO:
                    for i in range(count):
                        self.out.append(0)
                
                self.commands.append({
                    'compressed_offset': cmd_start - self.start_addr,
                    'type': CMD_NAMES.get(cmd, f'UNKNOWN_{cmd:02X}'),
                    'count': count,
                    'raw_bytes': raw_bytes,
                    'output_start': output_start,
                    'output_len': len(self.out) - output_start,
                    'output_bytes': list(self.out[output_start:]),
                })
        
        return bytes(self.out)


class EnginemonLZ3Tracer:
    """
    Enginemon's LZ3 decompressor (from tilesets.cpp) with full command tracing.
    This replicates the EXACT logic from Enginemon to find divergences.
    """
    
    def __init__(self, rom, start_addr):
        self.rom = rom
        self.start_addr = start_addr
        self.ptr = start_addr
        self.out = bytearray()
        self.commands = []
    
    def decompress_with_trace(self):
        """Decompress using Enginemon's exact algorithm."""
        
        while self.ptr < len(self.rom):
            cmd_start = self.ptr
            control = self.rom[self.ptr]
            
            if control == LZ_END:
                self.commands.append({
                    'compressed_offset': cmd_start - self.start_addr,
                    'type': 'END',
                    'raw_bytes': [control],
                    'output_start': len(self.out),
                    'output_len': 0,
                })
                break
            
            cmd = (control & LZ_CMD) >> 5
            
            if cmd == 7:  # LZ_LONG
                # Enginemon: cmd = ((control >> 2) & 0x07)
                cmd = (control >> 2) & 0x07
                hi = control & LZ_LONG_HI
                self.ptr += 1
                lo = self.rom[self.ptr]
                self.ptr += 1
                count = ((hi << 8) | lo) + 1
                raw_bytes = [control, lo]
            else:
                count = (control & LZ_LEN) + 1
                self.ptr += 1
                raw_bytes = [control]
            
            output_start = len(self.out)
            
            # Commands 0-3: simple data operations
            if cmd == 0:  # LZ_LITERAL
                for i in range(count):
                    if self.ptr >= len(self.rom):
                        break
                    raw_bytes.append(self.rom[self.ptr])
                    self.out.append(self.rom[self.ptr])
                    self.ptr += 1
                self.commands.append({
                    'compressed_offset': cmd_start - self.start_addr,
                    'type': 'LITERAL',
                    'count': count,
                    'raw_bytes': raw_bytes,
                    'output_start': output_start,
                    'output_len': len(self.out) - output_start,
                    'output_bytes': list(self.out[output_start:]),
                })
            elif cmd == 1:  # LZ_ITERATE
                byte = self.rom[self.ptr]
                raw_bytes.append(byte)
                self.ptr += 1
                for i in range(count):
                    self.out.append(byte)
                self.commands.append({
                    'compressed_offset': cmd_start - self.start_addr,
                    'type': 'ITERATE',
                    'count': count,
                    'raw_bytes': raw_bytes,
                    'output_start': output_start,
                    'output_len': len(self.out) - output_start,
                    'output_bytes': list(self.out[output_start:]),
                })
            elif cmd == 2:  # LZ_ALTERNATE
                b1 = self.rom[self.ptr]
                b2 = self.rom[self.ptr + 1]
                raw_bytes.extend([b1, b2])
                self.ptr += 2
                for i in range(count):
                    self.out.append(b2 if (i & 1) else b1)
                self.commands.append({
                    'compressed_offset': cmd_start - self.start_addr,
                    'type': 'ALTERNATE',
                    'count': count,
                    'raw_bytes': raw_bytes,
                    'output_start': output_start,
                    'output_len': len(self.out) - output_start,
                    'output_bytes': list(self.out[output_start:]),
                })
            elif cmd == 3:  # LZ_ZERO
                for i in range(count):
                    self.out.append(0)
                self.commands.append({
                    'compressed_offset': cmd_start - self.start_addr,
                    'type': 'ZERO',
                    'count': count,
                    'raw_bytes': raw_bytes,
                    'output_start': output_start,
                    'output_len': len(self.out) - output_start,
                    'output_bytes': list(self.out[output_start:]),
                })
            # Commands 4-6: rewrite from decompressed output
            else:
                offset_byte = self.rom[self.ptr]
                raw_bytes.append(offset_byte)
                self.ptr += 1
                
                if offset_byte & 0x80:
                    # Negative offset from current position
                    neg_offset = offset_byte & 0x7F
                    if neg_offset > len(self.out):
                        break
                    src_pos = len(self.out) - neg_offset
                    offset_type = 'negative'
                    offset_value = -neg_offset
                else:
                    # Positive offset from start (15-bit)
                    lo = self.rom[self.ptr]
                    raw_bytes.append(lo)
                    self.ptr += 1
                    pos_offset = (offset_byte << 8) | lo
                    src_pos = pos_offset
                    offset_type = 'positive'
                    offset_value = pos_offset
                
                cmd_name = {4: 'REPEAT', 5: 'FLIP', 6: 'REVERSE'}.get(cmd, f'UNKNOWN_{cmd}')
                
                if cmd == 4:  # LZ_REPEAT
                    for i in range(count):
                        self.out.append(self.out[src_pos + i])
                elif cmd == 5:  # LZ_FLIP
                    for i in range(count):
                        self.out.append(flip_byte(self.out[src_pos + i]))
                elif cmd == 6:  # LZ_REVERSE
                    for i in range(count):
                        self.out.append(self.out[src_pos - i])
                
                self.commands.append({
                    'compressed_offset': cmd_start - self.start_addr,
                    'type': cmd_name,
                    'count': count,
                    'raw_bytes': raw_bytes,
                    'offset_type': offset_type,
                    'offset_value': offset_value,
                    'src_pos': src_pos,
                    'output_start': output_start,
                    'output_len': len(self.out) - output_start,
                    'output_bytes': list(self.out[output_start:]),
                })
        
        return bytes(self.out)


def main():
    if len(sys.argv) < 2:
        print("Usage: python trace_lz3_divergence.py <rom_path>")
        sys.exit(1)
    
    rom_path = sys.argv[1]
    with open(rom_path, 'rb') as f:
        rom = f.read()
    
    # Lab tileset
    gfx_bank, gfx_ptr = get_tileset_gfx_addr(rom, 10)  # TILESET_LAB
    flat_addr = bank_to_flat(gfx_bank, gfx_ptr)
    
    print(f"Lab tileset GFX: {gfx_bank:02X}:{gfx_ptr:04X} = 0x{flat_addr:05X}")
    print()
    
    # Run both decompressors
    ref_tracer = LZ3Tracer(rom, flat_addr)
    ref_output = ref_tracer.decompress_with_trace()
    
    eng_tracer = EnginemonLZ3Tracer(rom, flat_addr)
    eng_output = eng_tracer.decompress_with_trace()
    
    print(f"Reference output: {len(ref_output)} bytes, {len(ref_tracer.commands)} commands")
    print(f"Enginemon output: {len(eng_output)} bytes, {len(eng_tracer.commands)} commands")
    print()
    
    # Find first divergence in output
    first_diff = None
    for i in range(min(len(ref_output), len(eng_output))):
        if ref_output[i] != eng_output[i]:
            first_diff = i
            break
    
    if first_diff is None and len(ref_output) != len(eng_output):
        first_diff = min(len(ref_output), len(eng_output))
    
    if first_diff is None:
        print("✓ No divergence found - outputs are identical!")
        return
    
    print(f"FIRST DIVERGENCE at output offset 0x{first_diff:04X} ({first_diff})")
    print()
    
    # Find which command produced this divergence
    bad_ref_cmd = None
    bad_eng_cmd = None
    
    for cmd in ref_tracer.commands:
        if cmd['output_start'] <= first_diff < cmd['output_start'] + cmd['output_len']:
            bad_ref_cmd = cmd
            break
    
    for cmd in eng_tracer.commands:
        if cmd['output_start'] <= first_diff < cmd['output_start'] + cmd['output_len']:
            bad_eng_cmd = cmd
            break
    
    print("="*70)
    print("REFERENCE COMMAND (expected correct)")
    print("="*70)
    if bad_ref_cmd:
        print(f"  Compressed offset: 0x{bad_ref_cmd['compressed_offset']:04X}")
        print(f"  Command type: {bad_ref_cmd['type']}")
        print(f"  Count: {bad_ref_cmd.get('count', 'N/A')}")
        print(f"  Raw bytes: {' '.join(f'{b:02X}' for b in bad_ref_cmd['raw_bytes'])}")
        if 'offset_type' in bad_ref_cmd:
            print(f"  Offset type: {bad_ref_cmd['offset_type']}")
            print(f"  Offset value: {bad_ref_cmd['offset_value']}")
            print(f"  Source position: {bad_ref_cmd['src_pos']}")
        print(f"  Output range: 0x{bad_ref_cmd['output_start']:04X} - 0x{bad_ref_cmd['output_start'] + bad_ref_cmd['output_len'] - 1:04X}")
        print(f"  First 16 output bytes: {' '.join(f'{b:02X}' for b in bad_ref_cmd['output_bytes'][:16])}")
    
    print()
    print("="*70)
    print("ENGINEMON COMMAND (divergent)")
    print("="*70)
    if bad_eng_cmd:
        print(f"  Compressed offset: 0x{bad_eng_cmd['compressed_offset']:04X}")
        print(f"  Command type: {bad_eng_cmd['type']}")
        print(f"  Count: {bad_eng_cmd.get('count', 'N/A')}")
        print(f"  Raw bytes: {' '.join(f'{b:02X}' for b in bad_eng_cmd['raw_bytes'])}")
        if 'offset_type' in bad_eng_cmd:
            print(f"  Offset type: {bad_eng_cmd['offset_type']}")
            print(f"  Offset value: {bad_eng_cmd['offset_value']}")
            print(f"  Source position: {bad_eng_cmd['src_pos']}")
        print(f"  Output range: 0x{bad_eng_cmd['output_start']:04X} - 0x{bad_eng_cmd['output_start'] + bad_eng_cmd['output_len'] - 1:04X}")
        print(f"  First 16 output bytes: {' '.join(f'{b:02X}' for b in bad_eng_cmd['output_bytes'][:16])}")
    
    print()
    print("="*70)
    print("BYTE-LEVEL COMPARISON AT DIVERGENCE")
    print("="*70)
    
    print(f"Output offset 0x{first_diff:04X}:")
    print(f"  Expected: 0x{ref_output[first_diff]:02X}")
    print(f"  Enginemon: 0x{eng_output[first_diff]:02X}")
    
    # Show context around divergence
    print()
    print("Context (8 bytes before and after):")
    start = max(0, first_diff - 8)
    end = min(len(ref_output), first_diff + 8)
    
    print(f"  Expected:  {' '.join(f'{ref_output[i]:02X}' if i < len(ref_output) else '--' for i in range(start, end))}")
    print(f"  Enginemon: {' '.join(f'{eng_output[i]:02X}' if i < len(eng_output) else '--' for i in range(start, end))}")
    print(f"             {' '.join('^^' if i == first_diff else '  ' for i in range(start, end))}")
    
    # Analyze the semantic difference
    print()
    print("="*70)
    print("SEMANTIC ANALYSIS")
    print("="*70)
    
    if bad_ref_cmd and bad_eng_cmd:
        if bad_ref_cmd['type'] != bad_eng_cmd['type']:
            print(f"COMMAND TYPE MISMATCH: Reference={bad_ref_cmd['type']}, Enginemon={bad_eng_cmd['type']}")
        elif 'offset_type' in bad_ref_cmd and 'offset_type' in bad_eng_cmd:
            if bad_ref_cmd['offset_type'] != bad_eng_cmd['offset_type']:
                print(f"OFFSET TYPE MISMATCH: Reference={bad_ref_cmd['offset_type']}, Enginemon={bad_eng_cmd['offset_type']}")
            elif bad_ref_cmd['src_pos'] != bad_eng_cmd['src_pos']:
                print(f"SOURCE POSITION MISMATCH: Reference={bad_ref_cmd['src_pos']}, Enginemon={bad_eng_cmd['src_pos']}")
                if bad_ref_cmd['offset_type'] == 'negative':
                    ref_neg = -(bad_ref_cmd['offset_value'])
                    eng_neg = -(bad_eng_cmd['offset_value'])
                    print(f"  Reference negative offset: {ref_neg} -> src_pos = output_len({len(ref_output)}) - {ref_neg} - 1 = {bad_ref_cmd['src_pos']}")
                    print(f"  Enginemon negative offset: {eng_neg} -> src_pos = output_len - {eng_neg} = {bad_eng_cmd['src_pos']}")
                    if bad_ref_cmd['src_pos'] == bad_eng_cmd['src_pos'] - 1:
                        print()
                        print("*** BUG FOUND: Enginemon's negative offset calculation is OFF BY ONE! ***")
                        print("    Reference: src_pos = output.size() - neg_offset - 1")
                        print("    Enginemon: src_pos = output.size() - neg_offset  (WRONG)")

    # Also check Johto as control
    print("\n" + "="*70)
    print("CONTROL: JOHTO TILESET")
    print("="*70)
    
    johto_bank, johto_ptr = get_tileset_gfx_addr(rom, 1)  # TILESET_JOHTO
    johto_addr = bank_to_flat(johto_bank, johto_ptr)
    
    johto_ref = LZ3Tracer(rom, johto_addr)
    johto_ref.decompress_with_trace()
    
    # Count negative offset commands in Johto
    neg_offset_cmds = [c for c in johto_ref.commands 
                       if c.get('offset_type') == 'negative']
    print(f"Johto: {len(neg_offset_cmds)} commands with negative offsets")
    
    if neg_offset_cmds:
        print("Sample negative offset commands:")
        for c in neg_offset_cmds[:3]:
            print(f"  {c['type']} at 0x{c['compressed_offset']:04X}, offset={c['offset_value']}, src_pos={c['src_pos']}")

if __name__ == "__main__":
    main()
