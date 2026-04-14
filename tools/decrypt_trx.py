#!/usr/bin/env python3
"""
CAN Checked TRX File Decryptor
==============================
Decrypts .TRX sensor configuration files from CAN Checked MFD displays
(MFD15, MFD28, MFD32 Gen2 with ESP32 firmware) into plaintext .TRI format.

Encryption: AES-256-ECB
Key source: MFD15 ESP32 firmware, file offset 0x318C (memory 0x3f40318C)
Key ASCII:  3s5v8y/B?E(H+MbQeThWmZq4t7w9z$C&

Verification: AES-256-ECB(key, 0x00 * 16) == 0126cfc4e0665a6130aa8197c821f500

TRX file structure:
  - Header line (plaintext):  info;<version>;...;\r\n  (v1.0) or  info;<version>;...;\n  (v1.1, v1.2)
  - Body: N * 192 bytes, AES-256-ECB encrypted
    Each 192-byte sensor record:
      Bytes   0-95:  null-terminated CAN sensor text (semicolon-delimited fields)
      Bytes  96-191: binary display widget config (position, size, colors)

Sensor text format (semicolon-delimited, 26 fields):
  Protocol;CAN_ID;Format;StartByte;Length;Unsigned;ShiftBit;CANmask;Decimal;
  Name;Scale;Offset;MapperType;Mapper1;Mapper2;Mapper3;
  AINactive;?;Min;Max;RefSensor;?;?;?;?;SensorType

Protocol header values:
  0000 / 0 = PT-CAN broadcast (standard 11-bit CAN frame, no request needed)
  0001 / 1 = BMW UDS/Mode 0x22 diagnostic request (DME/EGS/DSC)
  7DF       = OBD2 broadcast (generic)
  0         + CAN_ID=FFF = MFD analog input (AN1-AN4) or virtual sensor

Usage:
  python3 decrypt_trx.py input.TRX [output.TRI]
  python3 decrypt_trx.py *.TRX            # batch decrypt, output to same directory
  python3 decrypt_trx.py --dir /path/to/  # decrypt all TRX files in directory

Requirements:
  pip install pycryptodome
"""

import sys
import os
import argparse
from pathlib import Path

# AES-256-ECB key (32 bytes)
# Found in CAN Checked MFD15 ESP32 firmware at offset 0x318C
TRX_KEY_HEX = "3373357638792f423f4528482b4d6251655468576d5a7134743777397a244326"
TRX_KEY = bytes.fromhex(TRX_KEY_HEX)

# Each sensor occupies exactly 192 bytes in the encrypted body
RECORD_SIZE = 192
# Sensor text is null-terminated within the first 96 bytes of each record
SENSOR_TEXT_SIZE = 96


def _get_aes():
    """Import AES, providing a clear error if pycryptodome is not installed."""
    try:
        from Crypto.Cipher import AES
        return AES
    except ImportError:
        print("ERROR: pycryptodome not installed.", file=sys.stderr)
        print("  Install with:  pip install pycryptodome", file=sys.stderr)
        sys.exit(1)


def decrypt_trx(input_path: str | Path, output_path: str | Path | None = None) -> tuple[bytes, int]:
    """
    Decrypt a .TRX file into plaintext .TRI format.

    Parameters
    ----------
    input_path  : path to the encrypted .TRX file
    output_path : path for output .TRI file (None = dry run, no file written)

    Returns
    -------
    (tri_content, sensor_count)
      tri_content  : bytes — full plaintext TRI file content (header + sensor lines)
      sensor_count : int  — number of active (non-empty) sensor lines
    """
    AES = _get_aes()

    with open(input_path, "rb") as f:
        data = f.read()

    # Parse header: v1.0 uses CRLF (\r\n), v1.1 and v1.2 use LF only (\n)
    header_end = data.find(b"\r\n")
    if header_end != -1:
        header_end += 2  # include the \r\n
    else:
        header_end = data.find(b"\n")
        if header_end == -1:
            raise ValueError(f"{input_path}: no newline found in header")
        header_end += 1  # include the \n

    header = data[:header_end]
    body = data[header_end:]

    if len(body) == 0:
        raise ValueError(f"{input_path}: empty body after header")
    if len(body) % 16 != 0:
        raise ValueError(
            f"{input_path}: body length {len(body)} is not divisible by 16 "
            f"(file may be truncated or use an unsupported format)"
        )

    # Decrypt AES-256-ECB (no IV, block-by-block substitution)
    cipher = AES.new(TRX_KEY, AES.MODE_ECB)
    plaintext = cipher.decrypt(body)

    # Extract sensor text lines from 192-byte records
    # First 96 bytes of each record = null-terminated sensor specification text
    sensors = []
    num_records = len(plaintext) // RECORD_SIZE
    for i in range(num_records):
        text_block = plaintext[i * RECORD_SIZE : i * RECORD_SIZE + SENSOR_TEXT_SIZE]
        null_pos = text_block.find(b"\x00")
        if null_pos == -1:
            null_pos = SENSOR_TEXT_SIZE
        text = text_block[:null_pos]
        if text and b";" in text:
            sensors.append(text)

    # Reassemble TRI: original header + sensor lines separated by LF
    tri_content = header + b"\n".join(sensors) + b"\n"

    if output_path is not None:
        with open(output_path, "wb") as f:
            f.write(tri_content)

    return tri_content, len(sensors)


def _derive_output_path(input_path: Path) -> Path:
    """Replace .TRX extension with .TRI (case-preserving on the stem)."""
    return input_path.with_suffix(".TRI")


def main():
    parser = argparse.ArgumentParser(
        description="Decrypt CAN Checked .TRX sensor config files to plaintext .TRI",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    parser.add_argument("inputs", nargs="*", help=".TRX file(s) to decrypt")
    parser.add_argument(
        "-o", "--output", help="Output .TRI file (only valid with a single input file)"
    )
    parser.add_argument(
        "--dir", help="Decrypt all .TRX files in this directory"
    )
    parser.add_argument(
        "--outdir", help="Write decrypted .TRI files to this directory (default: same as input)"
    )
    parser.add_argument(
        "--verify", action="store_true",
        help="Verify key by encrypting zero block and checking known test vector"
    )
    args = parser.parse_args()

    # Key verification mode
    if args.verify:
        AES = _get_aes()
        cipher = AES.new(TRX_KEY, AES.MODE_ECB)
        result = cipher.encrypt(b"\x00" * 16).hex()
        expected = "0126cfc4e0665a6130aa8197c821f500"
        if result == expected:
            print(f"Key verification OK: AES-256-ECB(key, 0x00*16) = {result}")
        else:
            print(f"Key verification FAILED!")
            print(f"  Expected: {expected}")
            print(f"  Got:      {result}")
            sys.exit(1)
        return

    # Build list of input files
    input_files: list[Path] = []

    if args.dir:
        d = Path(args.dir)
        if not d.is_dir():
            print(f"ERROR: not a directory: {args.dir}", file=sys.stderr)
            sys.exit(1)
        input_files = sorted(d.glob("*.TRX")) + sorted(d.glob("*.trx"))

    for inp in args.inputs:
        p = Path(inp)
        if not p.exists():
            print(f"WARNING: file not found: {inp}", file=sys.stderr)
            continue
        if p.is_dir():
            input_files.extend(sorted(p.glob("*.TRX")) + sorted(p.glob("*.trx")))
        else:
            input_files.append(p)

    if not input_files:
        parser.print_help()
        sys.exit(0)

    if args.output and len(input_files) > 1:
        print("ERROR: -o/--output can only be used with a single input file", file=sys.stderr)
        sys.exit(1)

    out_dir = Path(args.outdir) if args.outdir else None
    if out_dir:
        out_dir.mkdir(parents=True, exist_ok=True)

    success = 0
    failed = 0

    for inp in input_files:
        if args.output:
            out = Path(args.output)
        elif out_dir:
            out = out_dir / inp.with_suffix(".TRI").name
        else:
            out = _derive_output_path(inp)

        try:
            _, count = decrypt_trx(inp, out)
            print(f"  OK  {inp.name} → {out.name}  ({count} sensors)")
            success += 1
        except Exception as exc:
            print(f"FAIL  {inp.name}: {exc}", file=sys.stderr)
            failed += 1

    if len(input_files) > 1:
        print(f"\nDone: {success} succeeded, {failed} failed")


if __name__ == "__main__":
    main()
