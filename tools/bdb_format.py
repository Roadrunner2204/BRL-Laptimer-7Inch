#!/usr/bin/env python3
"""
VBOX Motorsport Tracks.BDB Format
==================================
Parser für VBOX-Motorsport-Trackdatenbanken (Tracks.BDB).

Dateistruktur
-------------
Die Datei ist KEIN AES-/XOR-verschlüsselter Container, sondern ein
proprietäres TLV-Binärformat (Tag-Länge-Value). Der Dateiname-Suffix
.BDB steht für "Binary DataBase".

Jeder Record hat 4 Byte Header:
    +------+------+------+------+
    | tag  |    length (LE24)   |
    +------+------+------+------+
    | ... payload (length-4) ...|

`length` umfasst Header + Payload.

Bekannte Tags
-------------
    0xA1  FILE         Root-Container (umschließt alles)
                       Payload: 12 Byte Metadaten + child-records
                         Byte  0-1  : Jahr  (uint16 LE)
                         Byte  2    : Monat
                         Byte  3    : Tag
                         Byte  4    : Stunde
                         Byte  5    : Minute
                         Byte  6    : Sekunde
                         Byte  7-11 : Zähler / Version (unbekannt)

    0xA2  GROUP        Land / Region
                       Payload: 16 Byte Bounding-Box (2 × 8-Byte-Point)
                                + child-records (0xA3 Tracks)

    0xA3  TRACK        Einzelner Track
                       Payload: 16 Byte Track-Info (2 × 8-Byte-Point,
                                vermutl. Bounding-Box des Tracks)
                                + 0xA4 Name + 0xA5 Ziellinie
                                + optional 0xA6* Sektor-Linien
                                + optional 0xA7 Flag

    0xA4  NAME         Track-Name (ASCII, null-/nicht-terminiert)

    0xA5  SFLINE       Start/Ziel-Linie (16 Byte = 2 Punkte)

    0xA6  SECTOR       Sektor-Split-Linie (16 Byte = 2 Punkte)
                       0..n Einträge pro Track.

    0xA7  FLAG         1-Byte-Flag (z.B. Richtung / Typ)

    0xEE  FOOTER       Ende-Marker mit 4-Byte-Wert (CRC / Zähler)

Punkt-Encoding
--------------
Jeder 8-Byte-Punkt ist ein Paar (lat, lon) als int32 little-endian.
Der Umrechnungsfaktor lautet:

    grad = int32 / 6_000_000

  (äquivalent zu: minutes × 100_000)

Beispiel:  -190_514_433  →  -31.752°

Sowohl CPU-intern als auch als Integer bleiben die Koordinaten exakt.
"""

from __future__ import annotations

import json
import os
import struct
import zlib
from dataclasses import dataclass, field, asdict
from pathlib import Path
from typing import Optional, List, Iterator, Tuple

# ── .tbrl Verschlüsselung ────────────────────────────────────────────────────
# Gleicher AES-256-CBC-Key wie .brl (Fahrzeugprofile). Die Firmware kann
# beide Formate mit der gleichen AES-Routine entschlüsseln, unterschieden
# am Magic-Header.
TBRL_KEY         = bytes.fromhex(
    "4d358275a7ea02f81df7c5689c073f8edbd464066f563717e6cbaf60addb710a"
)
TBRL_MAGIC       = b"TBRL"   # 4 Byte
TBRL_VERSION     = 1
TBRL_HEADER_SIZE = 32        # Magic(4) + Ver(1)+Res(3) + IV(16) + Size(4) + CRC(4)


def _pkcs7_pad(data: bytes) -> bytes:
    pad = 16 - (len(data) % 16)
    return data + bytes([pad] * pad)


def encrypt_tbrl(plaintext: bytes) -> bytes:
    """Verschlüsselt einen .tbrl-Bundle-Blob.

    Layout (identisch zu .brl, nur anderes Magic):
        "TBRL" | ver(1) | res(3) | iv(16) | size(LE32) | crc32(LE32) | ciphertext
    """
    from Crypto.Cipher import AES   # pycryptodome
    iv = os.urandom(16)
    cipher = AES.new(TBRL_KEY, AES.MODE_CBC, iv)
    encrypted = cipher.encrypt(_pkcs7_pad(plaintext))
    crc = zlib.crc32(encrypted) & 0xFFFFFFFF
    header = (
        TBRL_MAGIC
        + bytes([TBRL_VERSION, 0, 0, 0])
        + iv
        + struct.pack("<I", len(encrypted))
        + struct.pack("<I", crc)
    )
    assert len(header) == TBRL_HEADER_SIZE
    return header + encrypted


def decrypt_tbrl(data: bytes) -> bytes:
    """Entschlüsselt und prüft einen .tbrl-Bundle-Blob."""
    from Crypto.Cipher import AES
    if len(data) < TBRL_HEADER_SIZE:
        raise ValueError("Datei zu kurz für TBRL-Header")
    if data[:4] != TBRL_MAGIC:
        raise ValueError(f"Kein gültiges TBRL-Magic: {data[:4]!r}")
    iv           = data[8:24]
    size         = struct.unpack_from("<I", data, 24)[0]
    expected_crc = struct.unpack_from("<I", data, 28)[0]
    encrypted    = data[TBRL_HEADER_SIZE: TBRL_HEADER_SIZE + size]
    if len(encrypted) != size:
        raise ValueError("Unvollständige Payload")
    if (zlib.crc32(encrypted) & 0xFFFFFFFF) != expected_crc:
        raise ValueError("CRC32 stimmt nicht")
    cipher = AES.new(TBRL_KEY, AES.MODE_CBC, iv)
    plain  = cipher.decrypt(encrypted)
    pad    = plain[-1]
    if pad < 1 or pad > 16:
        raise ValueError(f"Ungültiges PKCS7-Padding: {pad}")
    return plain[:-pad]


# ── Konstanten ──────────────────────────────────────────────────────────────
TAG_FILE     = 0xA1
TAG_GROUP    = 0xA2
TAG_TRACK    = 0xA3
TAG_NAME     = 0xA4
TAG_SFLINE   = 0xA5
TAG_SECTOR   = 0xA6
TAG_FLAG     = 0xA7
TAG_FOOTER   = 0xEE

COORD_SCALE  = 6_000_000   # int32 / 6e6  →  degrees


# ── Land-Mapping für VBOX-Gruppen-Indizes ────────────────────────────────────
# Die Gruppen in Tracks.BDB sind alphabetisch nach englischen Ländernamen
# sortiert. Diese Tabelle wurde aus den enthaltenen Track-Namen +
# Bounding-Box-Zentren rückentwickelt (904 Tracks, 71 Gruppen).
#
# Key   : VBOX-Gruppen-Index (1-basiert, wie in der Datei)
# Value : (englischer Name, deutscher Name)
#
# Der deutsche Name matcht das Schema, das die BRL-Laptimer-Firmware
# intern verwendet (siehe main/data/track_db.h: "Deutschland", "Österreich" …).
VBOX_COUNTRY_MAP: dict = {
     1: ("Argentina",           "Argentinien"),
     2: ("Australia",            "Australien"),
     3: ("Austria",              "Österreich"),
     4: ("Azerbaijan",           "Aserbaidschan"),
     5: ("Bahrain",              "Bahrain"),
     6: ("Barbados",             "Barbados"),
     7: ("Belgium",              "Belgien"),
     8: ("Brazil",               "Brasilien"),
     9: ("Bulgaria",             "Bulgarien"),
    10: ("Canada",               "Kanada"),
    11: ("Canary Islands",       "Kanarische Inseln"),
    12: ("Chile",                "Chile"),
    13: ("China",                "China"),
    14: ("Colombia",             "Kolumbien"),
    15: ("Costa Rica",           "Costa Rica"),
    16: ("Croatia",              "Kroatien"),
    17: ("Cyprus",               "Zypern"),
    18: ("Czech Republic",       "Tschechien"),
    19: ("Denmark",              "Dänemark"),
    20: ("Dominican Republic",   "Dominikanische Republik"),
    21: ("El Salvador",          "El Salvador"),
    22: ("Estonia",              "Estland"),
    23: ("Finland",              "Finnland"),
    24: ("France",               "Frankreich"),
    25: ("Georgia",              "Georgien"),
    26: ("Germany",              "Deutschland"),
    27: ("Greece",                "Griechenland"),
    28: ("Hungary",              "Ungarn"),
    29: ("India",                "Indien"),
    30: ("Indonesia",            "Indonesien"),
    31: ("Ireland",              "Irland"),
    32: ("Isle of Man",          "Isle of Man"),
    33: ("Israel",               "Israel"),
    34: ("Italy",                "Italien"),
    35: ("Japan",                "Japan"),
    36: ("Kuwait",               "Kuwait"),
    37: ("Latvia",               "Lettland"),
    38: ("Malaysia",             "Malaysia"),
    39: ("Mexico",               "Mexiko"),
    40: ("Morocco",              "Marokko"),
    41: ("Mozambique",           "Mosambik"),
    42: ("Netherlands",          "Niederlande"),
    43: ("New Zealand",          "Neuseeland"),
    44: ("Norway",               "Norwegen"),
    45: ("Panama",               "Panama"),
    46: ("Peru",                 "Peru"),
    47: ("Philippines",          "Philippinen"),
    48: ("Poland",               "Polen"),
    49: ("Portugal",             "Portugal"),
    50: ("Qatar",                "Katar"),
    51: ("Romania",              "Rumänien"),
    52: ("Russia",               "Russland"),
    53: ("Saudi Arabia",         "Saudi-Arabien"),
    54: ("Serbia",               "Serbien"),
    55: ("Singapore",            "Singapur"),
    56: ("Slovakia",             "Slowakei"),
    57: ("Slovenia",             "Slowenien"),
    58: ("South Africa",         "Südafrika"),
    59: ("South Korea",          "Südkorea"),
    60: ("Spain",                "Spanien"),
    61: ("Sweden",               "Schweden"),
    62: ("Switzerland",          "Schweiz"),
    63: ("Taiwan",               "Taiwan"),
    64: ("Thailand",             "Thailand"),
    65: ("Turkey",               "Türkei"),
    66: ("Ukraine",              "Ukraine"),
    67: ("United Arab Emirates", "Vereinigte Arabische Emirate"),
    68: ("United Kingdom",       "Großbritannien"),
    69: ("United States",        "USA"),
    70: ("Vietnam",              "Vietnam"),
    71: ("Zimbabwe",             "Simbabwe"),
}


def country_name(group_index, language: str = "de") -> str:
    """Liefert den Ländernamen für eine VBOX-Gruppe.
    language: 'de' (Standard) oder 'en'."""
    try:
        idx = int(group_index)
    except (TypeError, ValueError):
        return str(group_index)
    entry = VBOX_COUNTRY_MAP.get(idx)
    if not entry:
        return f"Gruppe {idx}"
    return entry[1] if language == "de" else entry[0]


# ── Dataclasses ─────────────────────────────────────────────────────────────
@dataclass
class Point:
    """Ein GPS-Punkt, dekodiert aus 8 Byte (2 × int32 LE)."""
    lat: float
    lon: float
    raw_lat: int = 0
    raw_lon: int = 0

    @classmethod
    def decode(cls, data: bytes, off: int = 0) -> "Point":
        if len(data) - off < 8:
            raise ValueError("Point benötigt 8 Byte")
        raw_lat, raw_lon = struct.unpack_from("<ii", data, off)
        return cls(
            lat=raw_lat / COORD_SCALE,
            lon=raw_lon / COORD_SCALE,
            raw_lat=raw_lat,
            raw_lon=raw_lon,
        )

    def as_tuple(self) -> Tuple[float, float]:
        return (self.lat, self.lon)


@dataclass
class Line:
    """Eine Linie aus zwei Punkten (z.B. Start-/Ziel-Linie, Sektorlinie)."""
    p1: Point
    p2: Point
    raw: bytes = b""

    @classmethod
    def decode(cls, payload: bytes) -> "Line":
        if len(payload) < 16:
            raise ValueError(f"Line benötigt 16 Byte, hat {len(payload)}")
        return cls(
            p1=Point.decode(payload, 0),
            p2=Point.decode(payload, 8),
            raw=bytes(payload[:16]),
        )

    @property
    def mid_lat(self) -> float:
        return (self.p1.lat + self.p2.lat) / 2.0

    @property
    def mid_lon(self) -> float:
        return (self.p1.lon + self.p2.lon) / 2.0


@dataclass
class Track:
    name: str = ""
    bbox: Optional[Line] = None        # 16-Byte-Block im Track-Header
    sf_line: Optional[Line] = None     # 0xA5
    sectors: List[Line] = field(default_factory=list)  # 0xA6 × n
    flags: List[int] = field(default_factory=list)     # 0xA7 × n
    group: str = ""                    # Landes-/Gruppenindex (1-basiert)
    offset: int = 0                    # Byte-Offset in der Original-BDB

    @property
    def center_lat(self) -> float:
        if self.sf_line:
            return self.sf_line.mid_lat
        if self.bbox:
            return self.bbox.mid_lat
        return 0.0

    @property
    def center_lon(self) -> float:
        if self.sf_line:
            return self.sf_line.mid_lon
        if self.bbox:
            return self.bbox.mid_lon
        return 0.0

    def to_tbrl_dict(self, country_name: str = "", length_km: float = 0.0,
                     is_circuit: bool = True) -> dict:
        """
        Konvertiert in BRL-Laptimer-Track-Format (.tbrl / .t-brl).

        Das Schema entspricht genau dem, was der Laptimer in
        session_store.cpp (load_user_tracks) erwartet:

            {
              "name":       str,
              "country":    str,
              "length_km":  float,
              "is_circuit": bool,
              "sf":         [lat1, lon1, lat2, lon2],
              "sectors":    [ { "lat": …, "lon": …, "name": "S1" } ],
              "fin":        [lat1, lon1, lat2, lon2]  (nur A-B)
            }

        Zusätzlich werden VBOX-spezifische Daten unter "_vbox" abgelegt,
        damit keine Information verloren geht (der Laptimer ignoriert
        unbekannte Felder).
        """
        d: dict = {
            "name":       self.name,
            "country":    country_name or self.group,
            "length_km":  float(length_km),
            "is_circuit": bool(is_circuit),
        }
        if self.sf_line:
            d["sf"] = [
                round(self.sf_line.p1.lat, 7), round(self.sf_line.p1.lon, 7),
                round(self.sf_line.p2.lat, 7), round(self.sf_line.p2.lon, 7),
            ]
        # VBOX-Sektor-Linien 1:1 als 2-Punkt-Linien (lat1/lon1/lat2/lon2).
        # Die Laptimer-Firmware wird später so angepasst, dass sie dieses
        # Format nativ verarbeitet -- damit geht keine VBOX-Präzision verloren.
        if self.sectors:
            d["sectors"] = [
                {
                    "name": f"S{i+1}",
                    "lat1": round(s.p1.lat, 7), "lon1": round(s.p1.lon, 7),
                    "lat2": round(s.p2.lat, 7), "lon2": round(s.p2.lon, 7),
                }
                for i, s in enumerate(self.sectors)
            ]

        # Zusätzliche VBOX-Metadaten (Bounding-Box, Flags)
        vbox: dict = {
            "group_index": self.group,
            "offset":      f"0x{self.offset:x}",
        }
        if self.bbox:
            vbox["bbox"] = [
                round(self.bbox.p1.lat, 7), round(self.bbox.p1.lon, 7),
                round(self.bbox.p2.lat, 7), round(self.bbox.p2.lon, 7),
            ]
        if self.flags:
            vbox["flags"] = list(self.flags)
        d["_vbox"] = vbox
        return d


@dataclass
class Group:
    index: int = 0
    bbox_raw: bytes = b""
    bbox: Optional[Line] = None
    tracks: List[Track] = field(default_factory=list)


@dataclass
class TrackDatabase:
    date: str = ""
    raw_header: bytes = b""
    groups: List[Group] = field(default_factory=list)
    footer_value: int = 0
    source_file: str = ""
    size_bytes: int = 0

    @property
    def tracks(self) -> Iterator[Track]:
        for g in self.groups:
            yield from g.tracks

    @property
    def track_count(self) -> int:
        return sum(len(g.tracks) for g in self.groups)


# ── TLV-Walker ───────────────────────────────────────────────────────────────
def _read_tlv(buf: bytes, off: int) -> Tuple[int, int, int]:
    """Liefert (tag, length, next_offset). length umfasst den Header."""
    if off + 4 > len(buf):
        raise ValueError(f"TLV-Header endet außerhalb des Puffers (off={off})")
    tag = buf[off]
    length = int.from_bytes(buf[off + 1: off + 4], "little")
    if length < 4:
        raise ValueError(f"TLV length<4 bei off=0x{off:x} (tag=0x{tag:02x})")
    if off + length > len(buf):
        raise ValueError(
            f"TLV überschreitet Puffer: tag=0x{tag:02x} len={length} off=0x{off:x}"
        )
    return tag, length, off + length


# ── Hauptparser ──────────────────────────────────────────────────────────────
def parse_bdb(path_or_bytes) -> TrackDatabase:
    """Parst eine Tracks.BDB Datei und liefert eine TrackDatabase."""
    if isinstance(path_or_bytes, (str, Path)):
        p = Path(path_or_bytes)
        data = p.read_bytes()
        src = str(p)
    else:
        data = bytes(path_or_bytes)
        src = "<bytes>"

    db = TrackDatabase(source_file=src, size_bytes=len(data))

    if len(data) < 4:
        raise ValueError("Datei zu klein")
    if data[0] != TAG_FILE:
        raise ValueError(
            f"Kein gültiger BDB-Header: erwartet 0x{TAG_FILE:02x}, "
            f"gefunden 0x{data[0]:02x}"
        )

    # ── A1 File-Container ────────────────────────────────────────────────
    f_tag, f_len, f_next = _read_tlv(data, 0)
    hdr_meta = data[4:16]                   # 12 Byte
    db.raw_header = hdr_meta
    if len(hdr_meta) >= 7:
        year = struct.unpack_from("<H", hdr_meta, 0)[0]
        month, day, hour, minute, second = hdr_meta[2:7]
        try:
            db.date = f"{year:04d}-{month:02d}-{day:02d} {hour:02d}:{minute:02d}:{second:02d}"
        except Exception:
            db.date = hdr_meta.hex()

    inner_off = 16                          # nach den 12 Metadaten + 4 Header
    inner_end = f_len                       # Ende des A1-Containers
    gidx = 0
    while inner_off < inner_end:
        tag, length, next_off = _read_tlv(data, inner_off)

        if tag == TAG_GROUP:
            gidx += 1
            g = Group(index=gidx)
            g.bbox_raw = data[inner_off + 4: inner_off + 4 + 16]
            try:
                g.bbox = Line.decode(g.bbox_raw)
            except Exception:
                g.bbox = None

            # Child-Tracks
            c_off = inner_off + 4 + 16
            c_end = inner_off + length
            while c_off < c_end:
                t_tag, t_len, t_next = _read_tlv(data, c_off)
                if t_tag == TAG_TRACK:
                    g.tracks.append(_parse_track(data, c_off, t_len, gidx))
                c_off = t_next
            db.groups.append(g)

        elif tag == TAG_FOOTER:
            payload = data[inner_off + 4: inner_off + length]
            if len(payload) >= 4:
                db.footer_value = struct.unpack_from("<I", payload, 0)[0]

        inner_off = next_off

    return db


def _parse_track(data: bytes, off: int, length: int, group_idx: int) -> Track:
    t = Track(group=str(group_idx), offset=off)
    bbox_raw = data[off + 4: off + 4 + 16]
    try:
        t.bbox = Line.decode(bbox_raw)
    except Exception:
        t.bbox = None

    c_off = off + 4 + 16
    c_end = off + length
    while c_off < c_end:
        tag, tlen, nxt = _read_tlv(data, c_off)
        payload = data[c_off + 4: c_off + tlen]

        if tag == TAG_NAME:
            # Trim Nullbytes / führende/nachfolgende Whitespaces
            nul = payload.find(b"\x00")
            if nul != -1:
                payload = payload[:nul]
            try:
                t.name = payload.decode("utf-8").strip()
            except UnicodeDecodeError:
                t.name = payload.decode("latin-1", errors="replace").strip()

        elif tag == TAG_SFLINE:
            try:
                t.sf_line = Line.decode(payload)
            except Exception:
                pass

        elif tag == TAG_SECTOR:
            try:
                t.sectors.append(Line.decode(payload))
            except Exception:
                pass

        elif tag == TAG_FLAG:
            if payload:
                t.flags.append(payload[0])

        c_off = nxt

    return t


# ── Export-Helfer ───────────────────────────────────────────────────────────
def track_to_tbrl_bytes(track: Track, pretty: bool = True) -> bytes:
    """Serialisiert einen einzelnen Track als .tbrl (JSON, UTF-8)."""
    d = track.to_tbrl_dict()
    return json.dumps(
        d, ensure_ascii=False,
        indent=2 if pretty else None,
        separators=(",", ": ") if pretty else (",", ":"),
    ).encode("utf-8")


def database_to_tbrl_bundle(db: TrackDatabase, pretty: bool = True,
                            country_map: Optional[dict] = None,
                            language: str = "de",
                            filter_groups: Optional[set] = None) -> bytes:
    """
    Serialisiert die gesamte Datenbank als ein .tbrl-Bundle (JSON).

    filter_groups : optional set/list von Gruppen-Indizes (int oder str).
                    Wenn gesetzt, werden nur Tracks dieser Länder aufgenommen.
    country_map   : optional dict group_idx -> Ländername (überschreibt Default).
    language      : 'de' oder 'en' für den Default-Ländernamen.
    """
    country_map = country_map or {}
    if filter_groups is not None:
        filter_groups = {str(g) for g in filter_groups}

    def cname(track):
        if track.group in country_map:
            return country_map[track.group]
        return country_name(track.group, language)

    tracks = [t for t in db.tracks
              if filter_groups is None or str(t.group) in filter_groups]

    bundle = {
        "format":       "t-brl-bundle",
        "version":      1,
        "source":       "VBOX Motorsport Tracks.BDB",
        "source_file":  Path(db.source_file).name if db.source_file else "",
        "date":         db.date,
        "track_count":  len(tracks),
        "group_count":  len({t.group for t in tracks}),
        "tracks":       [t.to_tbrl_dict(country_name=cname(t)) for t in tracks],
    }
    return json.dumps(
        bundle, ensure_ascii=False,
        indent=2 if pretty else None,
        separators=(",", ": ") if pretty else (",", ":"),
    ).encode("utf-8")


def sanitize_filename(name: str) -> str:
    bad = '<>:"/\\|?*'
    out = "".join("_" if c in bad or ord(c) < 32 else c for c in name)
    return out.strip().rstrip(".") or "track"


# ── CLI ──────────────────────────────────────────────────────────────────────
def _cli() -> int:
    import argparse
    ap = argparse.ArgumentParser(
        description="VBOX Motorsport Tracks.BDB Parser & Exporter"
    )
    sub = ap.add_subparsers(dest="cmd")

    p_info = sub.add_parser("info", help="Datei-Infos anzeigen")
    p_info.add_argument("bdb", type=Path)

    p_list = sub.add_parser("list", help="Alle Tracks ausgeben")
    p_list.add_argument("bdb", type=Path)
    p_list.add_argument("--group", type=int, help="Nur Gruppen-Index")

    p_dump = sub.add_parser("dump", help="Dump als JSON")
    p_dump.add_argument("bdb", type=Path)
    p_dump.add_argument("--out", type=Path, default=None)

    p_exp = sub.add_parser("export", help="Tracks als .tbrl / .t-brl exportieren")
    p_exp.add_argument("bdb", type=Path)
    p_exp.add_argument("--out-dir", type=Path, required=True,
                       help="Zielverzeichnis")
    p_exp.add_argument("--ext", choices=["tbrl", "t-brl"], default="tbrl")
    p_exp.add_argument("--bundle", action="store_true",
                       help="Alle Tracks in eine einzige Datei exportieren")
    p_exp.add_argument("--groups", type=str, default=None,
                       help="Gruppen-Indizes kommagetrennt (z.B. 3,24,26 "
                            "für Österreich/Frankreich/Deutschland)")
    p_exp.add_argument("--lang", choices=["de", "en"], default="de",
                       help="Sprache der Ländernamen (Default: de)")

    p_cty = sub.add_parser("countries", help="Länder-Mapping ausgeben")
    p_cty.add_argument("bdb", type=Path)
    p_cty.add_argument("--lang", choices=["de", "en"], default="de")

    args = ap.parse_args()

    if args.cmd == "info":
        db = parse_bdb(args.bdb)
        print(f"Datei:         {db.source_file}")
        print(f"Größe:         {db.size_bytes} Byte")
        print(f"Datum:         {db.date}")
        print(f"Gruppen:       {len(db.groups)}")
        print(f"Tracks gesamt: {db.track_count}")
        print(f"Footer-Wert:   0x{db.footer_value:08x}")
        return 0

    if args.cmd == "list":
        db = parse_bdb(args.bdb)
        for g in db.groups:
            if args.group and g.index != args.group:
                continue
            print(f"[Gruppe {g.index}] {len(g.tracks)} Track(s)")
            for t in g.tracks:
                lat, lon = t.center_lat, t.center_lon
                print(f"   {t.name:<40s}  "
                      f"({lat:+9.5f}, {lon:+10.5f})  "
                      f"Sektoren={len(t.sectors)}")
        return 0

    if args.cmd == "countries":
        db = parse_bdb(args.bdb)
        for g in db.groups:
            cn = country_name(g.index, getattr(args, "lang", "de"))
            print(f"  Gruppe {g.index:2d}: {cn:<30s}  {len(g.tracks):3d} Tracks")
        return 0

    if args.cmd == "dump":
        db = parse_bdb(args.bdb)
        blob = database_to_tbrl_bundle(db)
        if args.out:
            args.out.write_bytes(blob)
            print(f"→ {args.out} ({len(blob)} Byte)")
        else:
            import sys
            sys.stdout.write(blob.decode("utf-8"))
        return 0

    if args.cmd == "export":
        db = parse_bdb(args.bdb)
        args.out_dir.mkdir(parents=True, exist_ok=True)

        filter_groups = None
        if args.groups:
            filter_groups = {g.strip() for g in args.groups.split(",") if g.strip()}

        lang = args.lang

        def filt(t):
            return filter_groups is None or str(t.group) in filter_groups

        def cname(t):
            return country_name(t.group, lang)

        selected = [t for t in db.tracks if filt(t)]

        if args.bundle:
            out = args.out_dir / f"Tracks.{args.ext}"
            out.write_bytes(database_to_tbrl_bundle(
                db, filter_groups=filter_groups, language=lang))
            print(f"→ {out}  ({len(selected)} Tracks)")
        else:
            for t in selected:
                name = sanitize_filename(t.name) or f"track_{t.offset:x}"
                out = args.out_dir / f"{name}.{args.ext}"
                d = t.to_tbrl_dict(country_name=cname(t))
                out.write_bytes(json.dumps(
                    d, ensure_ascii=False, indent=2).encode("utf-8"))
            print(f"→ {len(selected)} Dateien in {args.out_dir}")
        return 0

    ap.print_help()
    return 1


if __name__ == "__main__":
    raise SystemExit(_cli())
