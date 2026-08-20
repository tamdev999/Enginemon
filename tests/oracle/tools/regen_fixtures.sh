#!/usr/bin/env bash
# regen_fixtures.sh
# Regenerate all binary fixtures from .asm sources using pinned RGBDS.
#
# RGBDS version: 0.7.0  (must match the provenance in README.md)
# Run from the Enginemon workspace root.
#
# NOTE: This script is run MANUALLY before committing changes to .asm files.
# Normal CI uses the checked-in .bin files directly — RGBDS is NOT required
# at test time.
#
# Usage:
#   ./tests/oracle/tools/regen_fixtures.sh

set -e

ORACLE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
FIXTURES_DIR="$ORACLE_DIR/fixtures"
TMP_DIR="$(mktemp -d)"
RGBASM="rgbasm"
RGBLINK="rgblink"

# Verify RGBDS version
RGBDS_VER=$("$RGBASM" --version 2>&1 | head -1 | grep -oP '\d+\.\d+\.\d+' | head -1)
EXPECTED_VER="0.7.0"
if [ "$RGBDS_VER" != "$EXPECTED_VER" ]; then
    echo "ERROR: RGBDS version $RGBDS_VER found, expected $EXPECTED_VER"
    echo "Pin RGBDS to 0.7.0 to ensure byte-identical fixture output."
    exit 1
fi

echo "RGBDS $RGBDS_VER confirmed."

assemble_fixture() {
    local name="$1"
    local asm_file="$FIXTURES_DIR/${name}.asm"
    local bin_file="$FIXTURES_DIR/${name}.bin"
    local obj_file="$TMP_DIR/${name}.o"

    if [ ! -f "$asm_file" ]; then
        echo "SKIP: $asm_file not found"
        return
    fi

    echo "Assembling $name..."
    "$RGBASM" -o "$obj_file" "$asm_file"
    "$RGBLINK" -o "${bin_file}.new" -x "$obj_file"

    if diff -q "$bin_file" "${bin_file}.new" >/dev/null 2>&1; then
        echo "  OK: $name.bin unchanged"
        rm "${bin_file}.new"
    else
        echo "  UPDATED: $name.bin"
        mv "${bin_file}.new" "$bin_file"
    fi
}

assemble_fixture "event_operand_order"
assemble_fixture "event_flag_vs_engine_flag"
assemble_fixture "text_tx_ram_mixed"
assemble_fixture "text_tx_decimal"
assemble_fixture "movement_step_dig"
assemble_fixture "movement_skyfall_top"
assemble_fixture "sdefer_bank_resolution"

rm -rf "$TMP_DIR"
echo "Done."
