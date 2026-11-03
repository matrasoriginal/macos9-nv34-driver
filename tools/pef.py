#!/usr/bin/env python3
"""PEF (Preferred Executable Format) helper for Mac OS 9 NDRVs.

  pef.py info FILE              sections, imports, exports, TheDriverDescription
  pef.py scan FILE...           finds PEF containers embedded in any file
                                (OS X kext binary, ioreg dump, ROM image)
  pef.py extract FILE OUTDIR    extracts all PEF containers found as .pef
  pef.py setinit FILE INIT [TERM]
                                CFM init/term routine = exported tvector (the RAVE engine
                                registers itself in init like ATI TtRegister); main = none
  pef.py fixhash FILE           reorders exports by hash table slot (MakePEF writes them
                                unsorted → with >1 slot CFM cannot find symbols)
  pef.py ioreg FILE OUTDIR      extracts the property
                                "driver,AAPL,MacOS,PowerPC" (hex <...>) from `ioreg -l -w0 -x` output as .pef

The output of `extract`/`ioreg` is a bare data fork; tools/mkndrv.sh sets
the OS 9 type/creator.
"""
import re
import struct
import sys
from pathlib import Path

MAGIC = b"Joy!peff"

SECTION_KINDS = {0: "code", 1: "unpacked-data", 2: "pattern-data", 3: "constant",
                 4: "loader", 5: "debug", 6: "exec-data", 7: "exception", 8: "traceback"}
SYMBOL_CLASSES = {0: "code", 1: "data", 2: "tvector", 3: "toc", 4: "glue"}


def u32(b, o):
    return struct.unpack_from(">I", b, o)[0]


def cstr(b, o):
    e = b.index(b"\0", o)
    return b[o:e].decode("mac_roman")


def unpack_pattern(raw, total_len):
    out = bytearray()
    i = 0

    def varint():
        nonlocal i
        v = 0
        while True:
            c = raw[i]
            i += 1
            v = (v << 7) | (c & 0x7F)
            if not c & 0x80:
                return v

    while i < len(raw):
        op = raw[i] >> 5
        count = raw[i] & 0x1F
        i += 1
        if count == 0:
            count = varint()
        if op == 0:
            out += bytes(count)
        elif op == 1:
            out += raw[i:i + count]
            i += count
        elif op == 2:
            rep = varint()
            blk = raw[i:i + count]
            i += count
            out += blk * (rep + 1)
        elif op in (3, 4):
            custom = varint()
            rep = varint()
            if op == 3:
                common = raw[i:i + count]
                i += count
            else:
                common = bytes(count)
            out += common
            for _ in range(rep):
                out += raw[i:i + custom]
                i += custom
                out += common
        else:
            raise ValueError(f"unknown pattern opcode {op}")
    if len(out) < total_len:
        out += bytes(total_len - len(out))
    return bytes(out)


class PEF:
    def __init__(self, data):
        if data[:8] != MAGIC:
            raise ValueError("not a PEF")
        self.data = data
        (_, _, self.arch, self.fmt, self.stamp, self.oldDef, self.oldImp, self.cur,
         nsec, ninst, _) = struct.unpack_from(">4s4s4sIIIIIHHI", data, 0)
        self.sections = []
        strtab = 40 + nsec * 28
        for n in range(nsec):
            (name_off, addr, total, unpacked, packed, off, kind, share, align, _) = \
                struct.unpack_from(">iIIIIIBBBB", data, 40 + n * 28)
            name = cstr(data, strtab + name_off) if name_off >= 0 else ""
            self.sections.append(dict(name=name, addr=addr, total=total, unpacked=unpacked,
                                      packed=packed, off=off, kind=kind))
        self.size = max(s["off"] + s["packed"] for s in self.sections)
        self.imports, self.exports, self.libs = [], [], []
        ldr = next((s for s in self.sections if s["kind"] == 4), None)
        if ldr:
            self._parse_loader(data[ldr["off"]:ldr["off"] + ldr["packed"]])

    def section_bytes(self, idx):
        s = self.sections[idx]
        raw = self.data[s["off"]:s["off"] + s["packed"]]
        if s["kind"] == 2:
            return unpack_pattern(raw, s["total"])
        return raw + bytes(max(0, s["total"] - len(raw)))

    def _parse_loader(self, L):
        (self.mainSec, self.mainOff, self.initSec, self.initOff, self.termSec, self.termOff,
         nlib, nimp, nrel, _, strOff, hashOff, hashPow, nexp) = struct.unpack_from(">iIiIiIIIIIIIII", L, 0)
        o = 56
        for _ in range(nlib):
            name_off, old, cur, cnt, first, opts = struct.unpack_from(">IIIIIB", L, o)
            self.libs.append(dict(name=cstr(L, strOff + name_off), old=old, cur=cur,
                                  count=cnt, first=first, weak=bool(opts & 0x40)))
            o += 24
        syms = []
        for _ in range(nimp):
            v = u32(L, o)
            syms.append((SYMBOL_CLASSES.get((v >> 24) & 0xF, "?"), cstr(L, strOff + (v & 0xFFFFFF))))
            o += 4
        for lib in self.libs:
            for k in range(lib["first"], lib["first"] + lib["count"]):
                self.imports.append((lib["name"], syms[k][1], syms[k][0]))
        keys = hashOff + 4 * (1 << hashPow)
        tab = keys + 4 * nexp
        for n in range(nexp):
            namelen = u32(L, keys + 4 * n) >> 16
            cn, val, sec = struct.unpack_from(">IIh", L, tab + 10 * n)
            no = strOff + (cn & 0xFFFFFF)
            self.exports.append(dict(name=L[no:no + namelen].decode("mac_roman"),
                                     cls=SYMBOL_CLASSES.get((cn >> 24) & 0xF, "?"),
                                     value=val, section=sec))

    def export_bytes(self, name, length):
        e = next((x for x in self.exports if x["name"] == name), None)
        if not e:
            return None
        return self.section_bytes(e["section"])[e["value"]:e["value"] + length]


def pstr(b, o, maxlen=32):
    n = b[o]
    return b[o + 1:o + 1 + min(n, maxlen - 1)].decode("mac_roman", "replace")


def describe_driver(pef):
    d = pef.export_bytes("TheDriverDescription", 200)
    if not d or len(d) < 128 or d[0:4] != b"mtej":
        return None
    sig = d[0:4].decode("mac_roman")
    ver = u32(d, 4)
    name = pstr(d, 8)
    nv = d[40:44]
    runtime = u32(d, 44)
    drvname = pstr(d, 48)
    nsvc = u32(d, 112)
    svcs = []
    for n in range(min(nsvc, 8)):
        o = 116 + n * 12
        svcs.append((d[o:o + 4].decode("mac_roman"), d[o + 4:o + 8].decode("mac_roman"), d[o + 8:o + 12].hex()))
    return dict(signature=sig, descVersion=ver, deviceType=name,
                version="%x.%x.%x stage %02x rel %d" % (nv[0], nv[1] >> 4, nv[1] & 0xF, nv[2], nv[3]),
                runtime="0x%x" % runtime, driverName=drvname, services=svcs)


def cmd_info(path):
    data = Path(path).read_bytes()
    pef = PEF(data[data.index(MAGIC):])
    print(f"{path}: arch={pef.arch.decode()} size={pef.size}")
    for n, s in enumerate(pef.sections):
        print(f"  sec {n}: {SECTION_KINDS.get(s['kind'], s['kind']):14} total={s['total']:#x} packed={s['packed']:#x} {s['name']}")
    for lib in pef.libs:
        print(f"  import lib {lib['name']}{' (weak)' if lib['weak'] else ''}: {lib['count']} symbols")
    for lib, sym, cls in pef.imports:
        print(f"    {lib}::{sym} [{cls}]")
    for e in pef.exports:
        print(f"  export {e['name']} [{e['cls']}] sec {e['section']} +{e['value']:#x}")
    desc = describe_driver(pef)
    if desc:
        print("  TheDriverDescription:")
        for k, v in desc.items():
            print(f"    {k}: {v}")


def find_pefs(data):
    pos = 0
    while True:
        i = data.find(MAGIC, pos)
        if i < 0:
            return
        try:
            pef = PEF(data[i:])
            yield i, pef
            pos = i + max(8, pef.size)
        except Exception:
            pos = i + 8


def cmd_scan(paths, outdir=None):
    for path in paths:
        data = Path(path).read_bytes()
        for n, (off, pef) in enumerate(find_pefs(data)):
            desc = describe_driver(pef)
            tag = f"{desc['deviceType']} / {desc['driverName']} v{desc['version']}" if desc else \
                ", ".join(e["name"] for e in pef.exports[:4])
            print(f"{path} @ {off:#x}: {pef.arch.decode()} {pef.size} B  {tag}")
            if outdir:
                stem = re.sub(r"[^A-Za-z0-9_.,-]+", "_", (desc["deviceType"] if desc else Path(path).name))
                out = Path(outdir) / f"{stem}_{n}.pef"
                out.parent.mkdir(parents=True, exist_ok=True)
                out.write_bytes(data[off:off + pef.size])
                print(f"  -> {out}")


def cmd_ioreg(path, outdir):
    """`ioreg -l -w0 -x -p IODeviceTree` prints the data as "driver,AAPL,MacOS,PowerPC" = <4a6f7921...>."""
    text = Path(path).read_text(errors="replace")
    out_dir = Path(outdir)
    out_dir.mkdir(parents=True, exist_ok=True)
    n = 0
    for m in re.finditer(r'"driver,AAPL,MacOS,PowerPC"\s*=\s*<([0-9a-fA-F]+)>', text):
        blob = bytes.fromhex(m.group(1))
        node = text.rfind("+-o ", 0, m.start())
        node_name = text[node + 4:text.index("<", node)].strip() if node >= 0 else "node"
        out = out_dir / f"{re.sub(r'[^A-Za-z0-9_.,@-]+', '_', node_name)}_{n}.pef"
        out.write_bytes(blob)
        print(f"{node_name}: {len(blob)} B -> {out}")
        n += 1
    if not n:
        print("no driver,AAPL,MacOS,PowerPC property (run ioreg with -x and -w0)")


def cmd_setinit(path, init, term=None):
    data = bytearray(Path(path).read_bytes())
    pef = PEF(bytes(data))
    ldr = next(s for s in pef.sections if s["kind"] == 4)

    def find(name):
        e = next((x for x in pef.exports if x["name"] == name), None)
        if not e or e["cls"] != "tvector":
            raise SystemExit(f"{name}: export (tvector) not found")
        return e["section"], e["value"]

    hdr = [-1, 0, -1, 0, -1, 0]
    hdr[2], hdr[3] = find(init)
    if term:
        hdr[4], hdr[5] = find(term)
    struct.pack_into(">iIiIiI", data, ldr["off"], *hdr)
    Path(path).write_bytes(bytes(data))
    print(f"{path}: init {init} sec {hdr[2]} +{hdr[3]:#x}" + (f", term {term} +{hdr[5]:#x}" if term else ""))


def pef_hash(name):
    h = 0
    for c in name:
        h = ((h << 1) - (h >> 16)) ^ c          # SInt32 arithmetic (PseudoRotate)
        h = (h + (1 << 31)) % (1 << 32) - (1 << 31)
    return (len(name) << 16) | ((h ^ (h >> 16)) & 0xFFFF)


def cmd_fixhash(path):
    data = bytearray(Path(path).read_bytes())
    pef = PEF(bytes(data))
    lo = next(s for s in pef.sections if s["kind"] == 4)["off"]
    str_off, hash_off, power, nexp = struct.unpack_from(">IIII", data, lo + 40)
    nslots = 1 << power
    key_off = lo + hash_off + 4 * nslots
    sym_off = key_off + 4 * nexp
    ents = []
    for i in range(nexp):
        key = u32(data, key_off + 4 * i)
        sym = bytes(data[sym_off + 10 * i:sym_off + 10 * i + 10])
        name = bytes(data[lo + str_off + (u32(sym, 0) & 0xFFFFFF):][:key >> 16])
        if pef_hash(name) != key:
            raise SystemExit(f"{name}: hash {key:#x} != {pef_hash(name):#x}")
        ents.append(((key ^ (key >> power)) & (nslots - 1), key, sym))
    ents.sort(key=lambda e: e[0])
    for i in range(nslots):
        idx = [j for j, e in enumerate(ents) if e[0] == i]
        struct.pack_into(">I", data, lo + hash_off + 4 * i, (len(idx) << 18) | (idx[0] if idx else 0))
    for j, (_, key, sym) in enumerate(ents):
        struct.pack_into(">I", data, key_off + 4 * j, key)
        data[sym_off + 10 * j:sym_off + 10 * j + 10] = sym
    Path(path).write_bytes(bytes(data))
    print(f"{path}: {nexp} exports sorted into {nslots} slots")


if __name__ == "__main__":
    a = sys.argv[1:]
    if len(a) >= 2 and a[0] == "info":
        for p in a[1:]:
            cmd_info(p)
    elif len(a) >= 2 and a[0] == "scan":
        cmd_scan(a[1:])
    elif len(a) == 3 and a[0] == "extract":
        cmd_scan([a[1]], a[2])
    elif len(a) in (3, 4) and a[0] == "setinit":
        cmd_setinit(*a[1:])
    elif len(a) == 2 and a[0] == "fixhash":
        cmd_fixhash(a[1])
    elif len(a) == 3 and a[0] == "ioreg":
        cmd_ioreg(a[1], a[2])
    else:
        print(__doc__)
        sys.exit(1)
