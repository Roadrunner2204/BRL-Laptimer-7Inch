#!/usr/bin/env python3
"""
BRL – BMW Data Display Fahrzeugprofil-Format
=============================================
Erstellt, liest und konvertiert verschlüsselte .brl Dateien.

Dateistruktur:
  [0:4]   Magic:         42 52 4C 01  ("BRL\x01")
  [4:5]   Version:       01
  [5:8]   Reserved:      00 00 00
  [8:24]  AES-IV:        16 zufällige Bytes (neu pro Datei)
  [24:28] Payload-Größe: uint32 little-endian (Länge des verschlüsselten Blocks)
  [28:32] CRC32:         des verschlüsselten Payloads
  [32:]   Payload:       AES-256-CBC + PKCS7, enthält JSON

JSON-Payload Struktur:
  {
    "v": 1,
    "make": "BMW",
    "model": "3er",
    "engine": "N47D20",
    "name": "N47 2.0d 143PS",
    "year_from": 2007,
    "year_to": 2015,
    "can": "PT-CAN",
    "bitrate": 500,
    "sensors": [ { ...TRI-Felder... }, ... ]
  }

Voraussetzungen:
  pip install pycryptodome

CLI-Verwendung:
  # TRX → BRL konvertieren
  python3 brl_format.py convert B-S65E.TRX --make BMW --engine S65B40 \
      --name "S65 V8 420PS" --model "M3 E90" --year-from 2007 --year-to 2013 \
      --can PT-CAN --bitrate 500 --out S65.brl

  # BRL-Inhalt anzeigen
  python3 brl_format.py info S65.brl

  # BRL entschlüsseln → JSON
  python3 brl_format.py dump S65.brl
"""

import json
import struct
import zlib
import os
import sys
from pathlib import Path

# ── Schlüssel (AES-256, 32 Bytes) ────────────────────────────────────────────
# WICHTIG: Diesen Key geheim halten! Er muss identisch in der Firmware sein.
# Nie öffentlich veröffentlichen oder in Git committen ohne .gitignore-Schutz.
BRL_KEY = bytes.fromhex(
    "4d358275a7ea02f81df7c5689c073f8edbd464066f563717e6cbaf60addb710a"
)

# ── Konstanten ────────────────────────────────────────────────────────────────
BRL_MAGIC   = b"BRL\x01"
BRL_VERSION = 1
HEADER_SIZE = 32  # Bytes (plaintext)

# ── TRX/TRI Felddefinitionen (für Konvertierung) ──────────────────────────────
FIELD_NAMES = [
    "proto", "can_id", "fmt", "start", "len",
    "unsigned", "shift", "mask", "decimals", "name",
    "scale", "offset", "mapper_type", "mapper1", "mapper2",
    "mapper3", "mapper4", "ain", "min", "max",
    "ref_sensor", "ref_val", "unused1", "popup", "unused2", "type",
]
TRX_KEY_AES = bytes.fromhex(
    "3373357638792f423f4528482b4d6251655468576d5a7134743777397a244326"
)
RECORD_SIZE      = 192
SENSOR_TEXT_SIZE = 96


# ── Hilfsfunktionen ───────────────────────────────────────────────────────────

def _pad(data: bytes) -> bytes:
    """PKCS7-Padding auf 16-Byte-Blöcke."""
    pad = 16 - (len(data) % 16)
    return data + bytes([pad] * pad)


def _unpad(data: bytes) -> bytes:
    """PKCS7-Padding entfernen."""
    pad = data[-1]
    if pad < 1 or pad > 16:
        raise ValueError(f"Ungültiges PKCS7-Padding: {pad}")
    return data[:-pad]


def _encrypt(plaintext: bytes) -> tuple[bytes, bytes]:
    """AES-256-CBC verschlüsseln. Gibt (iv, ciphertext) zurück."""
    from Crypto.Cipher import AES
    iv = os.urandom(16)
    cipher = AES.new(BRL_KEY, AES.MODE_CBC, iv)
    return iv, cipher.encrypt(_pad(plaintext))


def _decrypt(iv: bytes, ciphertext: bytes) -> bytes:
    """AES-256-CBC entschlüsseln."""
    from Crypto.Cipher import AES
    cipher = AES.new(BRL_KEY, AES.MODE_CBC, iv)
    return _unpad(cipher.decrypt(ciphertext))


# ── BRL erstellen ─────────────────────────────────────────────────────────────

def create_brl(vehicle: dict, sensors: list[dict]) -> bytes:
    """
    Erstellt eine verschlüsselte .brl Datei.

    vehicle = {
        "make": "BMW", "model": "3er", "engine": "N47D20",
        "name": "N47 2.0d 143PS", "year_from": 2007, "year_to": 2015,
        "can": "PT-CAN", "bitrate": 500
    }
    sensors = Liste von Dicts mit TRI-Feldern (aus decrypt_trx oder manuell)
    """
    payload = {
        "v": BRL_VERSION,
        "make":      vehicle.get("make", ""),
        "model":     vehicle.get("model", ""),
        "engine":    vehicle.get("engine", ""),
        "name":      vehicle.get("name", ""),
        "year_from": int(vehicle.get("year_from", 0)),
        "year_to":   int(vehicle.get("year_to", 9999)),
        "can":       vehicle.get("can", "PT-CAN"),
        "bitrate":   int(vehicle.get("bitrate", 500)),
        "sensors":   sensors,
    }
    json_bytes = json.dumps(payload, ensure_ascii=False, separators=(",", ":")).encode("utf-8")

    iv, encrypted = _encrypt(json_bytes)
    crc = zlib.crc32(encrypted) & 0xFFFFFFFF

    header = (
        BRL_MAGIC
        + bytes([BRL_VERSION, 0, 0, 0])  # version + 3 reserved
        + iv                              # 16 bytes AES-IV
        + struct.pack("<I", len(encrypted))
        + struct.pack("<I", crc)
    )
    assert len(header) == HEADER_SIZE
    return header + encrypted


def parse_brl(data: bytes) -> tuple[dict, list[dict]]:
    """
    Liest und entschlüsselt eine .brl Datei.
    Gibt (vehicle_info, sensors) zurück.
    """
    if len(data) < HEADER_SIZE:
        raise ValueError("Datei zu kurz für BRL-Header")
    if data[:4] != BRL_MAGIC:
        raise ValueError(f"Kein gültiger BRL-Magic: {data[:4]!r}")

    version        = data[4]
    iv             = data[8:24]
    payload_size   = struct.unpack_from("<I", data, 24)[0]
    expected_crc   = struct.unpack_from("<I", data, 28)[0]
    encrypted      = data[HEADER_SIZE: HEADER_SIZE + payload_size]

    if len(encrypted) != payload_size:
        raise ValueError("Unvollständige Payload-Daten")

    actual_crc = zlib.crc32(encrypted) & 0xFFFFFFFF
    if actual_crc != expected_crc:
        raise ValueError(f"CRC32-Fehler: erwartet {expected_crc:08X}, got {actual_crc:08X}")

    json_bytes = _decrypt(iv, encrypted)
    payload    = json.loads(json_bytes.decode("utf-8"))

    vehicle = {
        "make":      payload.get("make", ""),
        "model":     payload.get("model", ""),
        "engine":    payload.get("engine", ""),
        "name":      payload.get("name", ""),
        "year_from": payload.get("year_from", 0),
        "year_to":   payload.get("year_to", 9999),
        "can":       payload.get("can", "PT-CAN"),
        "bitrate":   payload.get("bitrate", 500),
    }
    sensors = payload.get("sensors", [])
    return vehicle, sensors


# ── TRX → BRL Konvertierung ───────────────────────────────────────────────────

def _decrypt_trx(trx_path: Path) -> tuple[str, list[dict]]:
    """Entschlüsselt eine CAN-Checked .TRX Datei."""
    from Crypto.Cipher import AES
    data    = trx_path.read_bytes()
    hdr_end = data.find(b"\r\n")
    if hdr_end != -1:
        hdr_end += 2
    else:
        hdr_end = data.find(b"\n") + 1

    header  = data[:hdr_end].decode("latin-1", errors="replace").strip()
    body    = data[hdr_end:]
    if len(body) % 16 != 0:
        raise ValueError("TRX-Körperlänge nicht durch 16 teilbar")

    plain   = AES.new(TRX_KEY_AES, AES.MODE_ECB).decrypt(body)
    sensors = []
    for i in range(len(plain) // RECORD_SIZE):
        block = plain[i * RECORD_SIZE: i * RECORD_SIZE + SENSOR_TEXT_SIZE]
        null  = block.find(b"\x00")
        text  = block[:null if null != -1 else SENSOR_TEXT_SIZE].decode("latin-1", errors="replace").rstrip(";")
        if not text or ";" not in text:
            continue
        parts = (text.split(";") + [""] * 26)[:26]
        s     = {FIELD_NAMES[j]: parts[j] for j in range(26)}
        if s.get("name", "").lower() == "empty":
            continue
        s["slot"] = i
        sensors.append(s)
    return header, sensors


def trx_to_brl(trx_path: Path, vehicle: dict) -> bytes:
    """Konvertiert eine .TRX Datei in eine verschlüsselte .brl Datei."""
    _, sensors = _decrypt_trx(trx_path)
    return create_brl(vehicle, sensors)


# ── CLI ───────────────────────────────────────────────────────────────────────

def _cli():
    import argparse

    parser = argparse.ArgumentParser(description="BRL Fahrzeugprofil-Tool")
    sub    = parser.add_subparsers(dest="cmd")

    # convert
    p_conv = sub.add_parser("convert", help="TRX → BRL konvertieren")
    p_conv.add_argument("trx", type=Path)
    p_conv.add_argument("--make",       default="BMW")
    p_conv.add_argument("--model",      default="")
    p_conv.add_argument("--engine",     required=True)
    p_conv.add_argument("--name",       required=True, help='z.B. "N47 2.0d 143PS"')
    p_conv.add_argument("--year-from",  type=int, default=0)
    p_conv.add_argument("--year-to",    type=int, default=9999)
    p_conv.add_argument("--can",        default="PT-CAN")
    p_conv.add_argument("--bitrate",    type=int, default=500)
    p_conv.add_argument("--out",        type=Path, default=None)

    # info
    p_info = sub.add_parser("info", help="BRL-Datei Informationen anzeigen")
    p_info.add_argument("brl", type=Path)

    # dump
    p_dump = sub.add_parser("dump", help="BRL-Datei als JSON ausgeben")
    p_dump.add_argument("brl", type=Path)

    args = parser.parse_args()

    if args.cmd == "convert":
        vehicle = {
            "make": args.make, "model": args.model,
            "engine": args.engine, "name": args.name,
            "year_from": args.year_from, "year_to": args.year_to,
            "can": args.can, "bitrate": args.bitrate,
        }
        out = args.out or args.trx.with_suffix(".brl")
        data = trx_to_brl(args.trx, vehicle)
        out.write_bytes(data)
        print(f"Erstellt: {out}  ({len(data)} Bytes)")

    elif args.cmd == "info":
        vehicle, sensors = parse_brl(args.brl.read_bytes())
        print(f"Fahrzeug: {vehicle['make']} {vehicle['model']}")
        print(f"Motor:    {vehicle['engine']}  –  {vehicle['name']}")
        print(f"Baujahr:  {vehicle['year_from']} – {vehicle['year_to']}")
        print(f"CAN-Bus:  {vehicle['can']}  {vehicle['bitrate']} kBit/s")
        print(f"Sensoren: {len(sensors)}")

    elif args.cmd == "dump":
        vehicle, sensors = parse_brl(args.brl.read_bytes())
        print(json.dumps({"vehicle": vehicle, "sensors": sensors}, indent=2, ensure_ascii=False))

    else:
        parser.print_help()


if __name__ == "__main__":
    _cli()
