#!/usr/bin/env python3
"""
Identify an Xbox executable: title, title ID, region, XDK libraries, sections
and hashes. Step 1 of the port (docs/porting-another-title.md in Concept of
Love): the XDK build decides which of JSRF's facts still hold.

    python tools/xbe_info.py path/to/default.xbe
    python tools/xbe_info.py path/to/default.xbe --json

Prints to the terminal only. Paste the summary into a session if asked; the
dump hashes and section addresses go into src/xbe.c and tools/recomp.py,
nothing else from the output is committed.
"""

import argparse
import hashlib
import json
import struct
import sys

RETAIL_ENTRY_KEY = 0xA8FC57AB
DEBUG_ENTRY_KEY = 0x94859D4B
RETAIL_THUNK_KEY = 0x5B6D40B6
DEBUG_THUNK_KEY = 0xEFB1F152

REGIONS = {0x1: "North America", 0x2: "Japan", 0x4: "Rest of world",
           0x80000000: "Manufacturing"}

# Section flags.
SECT_WRITABLE, SECT_PRELOAD, SECT_EXECUTABLE = 0x1, 0x2, 0x4


class XbeError(Exception):
    pass


def parse(data):
    """Everything about an XBE the port cares about, as a dict."""
    if len(data) < 0x178 or data[:4] != b"XBEH":
        raise XbeError("not an Xbox executable (no XBEH header)")
    base, = struct.unpack_from("<I", data, 0x104)
    cert, nsect, sect = struct.unpack_from("<III", data, 0x118)
    raw_entry, = struct.unpack_from("<I", data, 0x128)
    raw_thunk, = struct.unpack_from("<I", data, 0x158)
    nlib, lib = struct.unpack_from("<II", data, 0x160)

    def off(va, need=1):
        o = va - base
        if va < base or o + need > len(data):
            raise XbeError(f"header address 0x{va:08X} outside the file")
        return o

    c = off(cert, 0xB0)
    title_id, = struct.unpack_from("<I", data, c + 0x08)
    title = data[c + 0x0C:c + 0x0C + 80].decode("utf-16-le", "replace").split("\0")[0]
    region, = struct.unpack_from("<I", data, c + 0xA0)
    disc, version = struct.unpack_from("<II", data, c + 0xA8)

    sections = []
    s = off(sect, nsect * 0x38)
    for i in range(nsect):
        o = s + i * 0x38
        flags, va, vsize, raw, rsize, name_va = struct.unpack_from("<IIIIII", data, o)
        n = off(name_va)
        name = data[n:data.index(b"\0", n)].decode("ascii", "replace")
        if raw + rsize > len(data):
            raise XbeError(f"section {name} data outside the file")
        sections.append({"name": name, "va": va, "vsize": vsize, "raw": raw,
                         "rsize": rsize, "flags": flags})

    libraries = []
    if 0 < nlib <= 64:
        l0 = off(lib, nlib * 16)
        for i in range(nlib):
            name, major, minor, build, flags = struct.unpack_from("<8sHHHH", data, l0 + i * 16)
            libraries.append({"name": name.rstrip(b"\0").decode("ascii", "replace"),
                              "version": f"{major}.{minor}.{build}", "build": build,
                              "qfe": flags & 0x1FFF, "debug": bool(flags & 0x8000)})

    def in_image(va):
        return any(x["va"] <= va < x["va"] + x["vsize"] for x in sections)

    entry, thunk, kind = raw_entry ^ RETAIL_ENTRY_KEY, raw_thunk ^ RETAIL_THUNK_KEY, "retail"
    if not in_image(entry):
        entry, thunk, kind = raw_entry ^ DEBUG_ENTRY_KEY, raw_thunk ^ DEBUG_THUNK_KEY, "debug"

    image = hashlib.sha256()
    for x in sections:
        image.update(data[x["raw"]:x["raw"] + x["rsize"]])

    return {
        "title": title, "title_id": title_id,
        "title_id_text": f"{chr((title_id >> 24) & 0xFF)}{chr((title_id >> 16) & 0xFF)}"
                         f"-{title_id & 0xFFFF:03d}",
        "regions": [n for bit, n in REGIONS.items() if region & bit],
        "disc": disc, "version": version, "build_kind": kind,
        "base": base, "entry": entry, "kernel_thunk": thunk,
        "xdk_build": max((x["build"] for x in libraries), default=0),
        "libraries": libraries, "sections": sections,
        "file_sha256": hashlib.sha256(data).hexdigest(),
        "image_sha256": image.hexdigest(),
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("xbe", help="default.xbe")
    ap.add_argument("--json", action="store_true", help="machine-readable output")
    args = ap.parse_args()

    with open(args.xbe, "rb") as f:
        try:
            x = parse(f.read())
        except (XbeError, struct.error, ValueError) as e:
            sys.exit(f"error: {e}")

    if args.json:
        json.dump(x, sys.stdout, indent=1)
        print()
        return

    print(f"title        : {x['title']}")
    print(f"title ID     : {x['title_id']:08X} ({x['title_id_text']})")
    print(f"regions      : {', '.join(x['regions']) or 'none'}")
    print(f"disc/version : {x['disc']} / {x['version']}")
    print(f"build        : {x['build_kind']}, entry 0x{x['entry']:08X}, "
          f"kernel thunk 0x{x['kernel_thunk']:08X}")
    print(f"XDK build    : {x['xdk_build']} (JSRF: 4134)")
    print(f"file sha256  : {x['file_sha256']}")
    print(f"image sha256 : {x['image_sha256']}")
    print("\nlibraries:")
    for lib in x["libraries"]:
        extra = (f" QFE {lib['qfe']}" if lib["qfe"] else "") + (" debug" if lib["debug"] else "")
        print(f"  {lib['name']:<8} {lib['version']}{extra}")
    print("\nsections:")
    for s in x["sections"]:
        fl = "".join(c for bit, c in ((SECT_EXECUTABLE, "x"), (SECT_WRITABLE, "w"),
                                      (SECT_PRELOAD, "p")) if s["flags"] & bit)
        print(f"  {s['name']:<10} 0x{s['va']:08X}  {s['vsize']:>9,} bytes  {fl}")


if __name__ == "__main__":
    main()
