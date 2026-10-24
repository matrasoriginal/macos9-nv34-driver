#!/usr/bin/env python3
"""Resource fork + MacBinary II pre OS 9 'ndrv' súbor.

  macres.py ndrv INPUT.pef NAME OUTDIR [TYPE CREATOR]

Vytvorí:
  OUTDIR/NAME.rsrc   resource fork: 'cfrg' 0 (fragment v data forku) + 'vers' 1/2
  OUTDIR/NAME.bin    MacBinary II (data + resource fork + type/creator) — prenos cez web/FAT/FTP,
                     v OS 9 rozbaliť StuffIt Expanderom

Layout 'cfrg' kopíruje NVIDIA Driver 3.1 (usage = import library, where = data fork).
"""
import struct
import os
import sys
import time
from pathlib import Path


def pstr(s):
    b = s.encode("mac_roman")
    return bytes([len(b)]) + b


def cfrg(name, length):
    nm = pstr(name)
    member_size = (42 + len(nm) + 3) & ~3
    member = struct.pack(">4sHBBIIIHBBIIII", b"pwpc", 0, 0, 0, 0, 0, 0, 0,
                         0,          # usage: kImportLibraryCFrag (ako NVIDIA Driver)
                         1,          # where: kDataForkCFragLocator
                         0, length,  # offset, length
                         0, 0)       # reserved (8 B)
    member = member[:40] + struct.pack(">H", member_size) + nm
    member += bytes(member_size - len(member))
    header = struct.pack(">IIIIIIII", 0, 0, 1, 0, 0, 0, 0, 1)
    return header + member


def vers(major, minor_bug, stage, rev, short, long_):
    return struct.pack(">BBBBH", major, minor_bug, stage, rev, 0) + pstr(short) + pstr(long_)


def resource_fork(resources):
    """resources: list of (type(4 B), id, data)."""
    data = b""
    offsets = []
    for _, _, d in resources:
        offsets.append(len(data))
        data += struct.pack(">I", len(d)) + d
    types = {}
    for i, (t, rid, _) in enumerate(resources):
        types.setdefault(t, []).append((rid, offsets[i]))
    tl_len = 2 + 8 * len(types)
    reflists = b""
    tl = struct.pack(">H", len(types) - 1)
    for t, refs in types.items():
        tl += struct.pack(">4sHH", t, len(refs) - 1, tl_len + len(reflists))
        for rid, off in refs:
            reflists += struct.pack(">hhI", rid, -1, off & 0xFFFFFF) + bytes(4)
    map_ = bytes(16) + bytes(4) + bytes(2) + struct.pack(">HHH", 0, 28, 28 + tl_len + len(reflists))
    map_ += tl + reflists
    data_off = 256
    map_off = data_off + len(data)
    hdr = struct.pack(">IIII", data_off, map_off, len(data), len(map_))
    map_ = hdr + map_[16:]
    return hdr + bytes(240) + data + map_


def crc16_xmodem(b):
    crc = 0
    for c in b:
        crc ^= c << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) if crc & 0x8000 else (crc << 1)
            crc &= 0xFFFF
    return crc


def macbinary(name, type_, creator, data, rsrc, finder_flags=0):
    now = int(time.time()) + 2082844800
    nm = name.encode("mac_roman")[:63]
    h = bytearray(128)
    h[1] = len(nm)
    h[2:2 + len(nm)] = nm
    h[65:69] = type_
    h[69:73] = creator
    h[73] = finder_flags >> 8
    struct.pack_into(">II", h, 83, len(data), len(rsrc))
    struct.pack_into(">II", h, 91, now, now)
    h[101] = finder_flags & 0xFF
    h[122] = 129
    h[123] = 129
    struct.pack_into(">H", h, 124, crc16_xmodem(bytes(h[:124])))

    def pad(b):
        return b + bytes((-len(b)) % 128)

    return bytes(h) + pad(data) + pad(rsrc)


def main():
    a = sys.argv[1:]
    if len(a) < 4 or a[0] != "ndrv":
        print(__doc__)
        sys.exit(1)
    pef = Path(a[1]).read_bytes()
    name, outdir = a[2], Path(a[3])
    type_ = (a[4] if len(a) > 4 else "ndrv").encode()
    creator = (a[5] if len(a) > 5 else "NV34").encode()

    # verziu berieme z TheDriverDescription
    sys.path.insert(0, str(Path(__file__).parent))
    import pef as peflib
    desc = peflib.PEF(pef).export_bytes("TheDriverDescription", 44)
    major, minor_bug, stage, rev = desc[40:44] if desc else (1, 0, 0x80, 0)
    stage_ch = {0x20: "d", 0x40: "a", 0x60: "b"}.get(stage, "")
    short = f"{major}.{minor_bug >> 4}" + (f".{minor_bug & 0xF}" if minor_bug & 0xF else "") + \
            (f"{stage_ch}{rev}" if stage_ch else "")

    # knižnice bez TheDriverDescription (RAVE engine, MiniGL): verzia a popis z prostredia
    info = "NV34 NDRV pre GeForce FX Go5200"
    if os.environ.get("VERS"):
        parts = [int(x) for x in os.environ["VERS"].split(".")] + [0, 0]
        major, minor_bug, stage, rev = parts[0], (parts[1] << 4) | parts[2], 0x80, 0
        short = os.environ["VERS"]
    if os.environ.get("VERS_INFO"):
        info = os.environ["VERS_INFO"]
    rsrc = resource_fork([
        (b"cfrg", 0, cfrg(name, len(pef))),
        (b"vers", 1, vers(major, minor_bug, stage, rev, short, f"{short}, {info}")),
        (b"vers", 2, vers(major, minor_bug, stage, rev, short, "PowerBook 6,2 GeForce FX Go5200")),
    ])
    outdir.mkdir(parents=True, exist_ok=True)
    (outdir / f"{name}.rsrc").write_bytes(rsrc)
    (outdir / f"{name}.bin").write_bytes(macbinary(name, type_, creator, pef, rsrc))
    print(f"-> {outdir / (name + '.rsrc')} ({len(rsrc)} B), {outdir / (name + '.bin')}  vers {short}")


if __name__ == "__main__":
    main()
