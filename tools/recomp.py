#!/usr/bin/env python3
"""
Generate the recompiled C for Orta Recomp from your own copy of the game.

    python tools/recomp.py path/to/default.xbe
    python tools/recomp.py path/to/Orta.iso
    python tools/recomp.py path/to/default.xbe --names build/match/names.json

Runs the xboxrecomp pipeline (parse -> disassemble -> identify -> ABI ->
lift) with every intermediate file in build/recomp-work/ and the C in gen/.
Both are git-ignored: nothing derived from the game belongs in the repository.
"""

import argparse
import json
import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
XBOXRECOMP = os.path.join(ROOT, "external", "xboxrecomp")

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import xbe_info  # noqa: E402

# Keep in step with src/xbe.c. Panzer Dragoon Orta (USA): SE-007, XDK 4928.
PDO_TITLE_ID = 0x53450007
KNOWN_FILE_SHA256 = {
    "c1af4eafccc02e11105d606b177263216cb7aa71bf1fbe8486ddfc3b1b848d2c",
}
PDO_LAYOUT = {
    ".text": 0x00011000, "D3D": 0x002AD9A0, "DSOUND": 0x002C1140,
    "XGRPH": 0x002FD740, "XPP": 0x003067A0, ".rdata": 0x0030C8A0,
    "APPDATA": 0x00340900,
}
# Marked executable in the XBE but data (Orta has no .data section). Swept
# as code it yields junk functions, and some do not compile.
DATA_SECTIONS = "APPDATA"


def fail(msg):
    print(f"error: {msg}", file=sys.stderr)
    sys.exit(1)


def apply_patches():
    """Apply the fixes we carry to external/xboxrecomp (patches/xboxrecomp/).

    CMake does the same at configure time; doing it here too matters because
    some patches fix the recompiler itself, and this script runs first.
    """
    patch_dir = os.path.join(ROOT, "patches", "xboxrecomp")
    if not os.path.isdir(patch_dir):
        return
    for name in sorted(os.listdir(patch_dir)):
        if not name.endswith(".patch"):
            continue
        patch = os.path.join(patch_dir, name)
        applied = subprocess.run(["git", "apply", "--reverse", "--check", patch],
                                 cwd=XBOXRECOMP, capture_output=True).returncode == 0
        if applied:
            continue
        if subprocess.run(["git", "apply", patch], cwd=XBOXRECOMP).returncode != 0:
            fail(f"could not apply {name} to external/xboxrecomp; reset it with "
                 "`git -C external/xboxrecomp checkout -- .` and try again")
        print(f"applied {name}")


def run_tool(module, *args):
    cmd = [sys.executable, "-m", f"tools.{module}", *args]
    print(f"\n== {module}", flush=True)
    result = subprocess.run(cmd, cwd=XBOXRECOMP)
    if result.returncode != 0:
        fail(f"tools.{module} failed (exit {result.returncode})")


# ── XBE identification ──────────────────────────────────────────────────

def identify(data):
    """Return (description, image_sha256). Exits for anything not Orta."""
    try:
        x = xbe_info.parse(data)
    except xbe_info.XbeError as e:
        fail(str(e))
    if x["title_id"] != PDO_TITLE_ID:
        fail(f"title ID {x['title_id']:08X} is not Panzer Dragoon Orta (USA, 53450007)")
    found = {s["name"]: s["va"] for s in x["sections"]}
    for name, va in PDO_LAYOUT.items():
        if found.get(name) != va:
            fail(f"section {name} is not where Orta (USA) has it; wrong version?")

    if x["file_sha256"] in KNOWN_FILE_SHA256:
        desc = "known dump"
    else:
        desc = f"layout matches, unseen dump (sha256 {x['file_sha256']})"
    return desc, x["image_sha256"]


# ── Missed functions ───────────────────────────────────────────────────

def missed_targets(disasm_dir, xbe):
    """Code addresses that real code calls (or tail-jumps to) but that are
    not function starts.

    The recompiler stubs every such target with an empty function that pops
    nothing, so each caller's stack ends up off by the callee's arguments.
    Orta's XInitDevices (0x003068D4, reached through a jmp thunk) was one:
    its caller then read a neighbouring stack slot as a COM object.

    Calls count from anywhere a function was detected, to anywhere in a code
    section; a jump, conditional or not, counts only when it lands outside
    every function (inside one it is an ordinary branch). The lifter turns
    such a jump into a tail call, so an undetected target is a stub that
    pops 4 bytes, the caller's return address: 0x0026E940 (a file object's
    GetName-like method) was cut after its first `ret 0xc`, and its `je` to
    the shared error exit at 0x0026E96E went into such a stub. The disassembler's own seed guard rejects
    a target that falls mid-instruction in an existing function, which is
    what calls decoded from data mostly do.

    So do function addresses taken as data: pointers in data sections
    (vtables, callback tables) and immediates in code (a callback being
    registered), when they land on a 16-byte boundary right after a ret or
    padding, inside a detected function (the sweep folded it into the one
    before) or in a gap no function covers. A call through such a pointer
    fails and pops no arguments either. Orta's sound objects had several;
    0x0003E080, registered with `push offset`, was another. Switch jump
    tables, the other pointers into a function, sit in .text after their
    function, and are reached by an indexed jump, not an immediate. A
    function can start right after such a table instead of after a ret: the
    calls to 0x00111BC0, after 0x00111990's tables, failed from the
    start of the second boss fight of episode 2, whose boss was not drawn.
    """
    with open(os.path.join(disasm_dir, "functions.json")) as f:
        funcs = json.load(f)
    with open(os.path.join(disasm_dir, "xrefs.json")) as f:
        xrefs = json.load(f)
    with open(xbe, "rb") as f:
        image = f.read()
    info = xbe_info.parse(image)
    code = [(s["va"], s["va"] + s["rsize"]) for s in info["sections"]
            if s["flags"] & xbe_info.SECT_EXECUTABLE and s["name"] not in DATA_SECTIONS.split(",")
            and s["name"] not in (".rdata", ".data", "DOLBY", "$$XTIMAGE")]
    starts = {int(e["start"], 16) for e in funcs}
    spans = sorted((int(e["start"], 16), int(e["start"], 16) + e["size"]) for e in funcs)

    import bisect
    lo = [a for a, _ in spans]

    def owner(va):
        """Start of the function whose span holds va, or None."""
        i = bisect.bisect_right(lo, va) - 1
        return spans[i][0] if i >= 0 and va < spans[i][1] else None

    def inside(va):
        return owner(va) is not None

    def byte_at(va):
        for sec in info["sections"]:
            if sec["va"] <= va < sec["va"] + sec["rsize"]:
                return image[sec["raw"] + va - sec["va"]]
        return None

    import capstone
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)

    def decodes(va):
        """The first few instructions at va decode: not data that a stray
        pointer happens to name (0x00201E40 in Orta was a table)."""
        for sec in info["sections"]:
            if sec["va"] <= va < sec["va"] + sec["rsize"]:
                o = sec["raw"] + va - sec["va"]
                return len(list(md.disasm(image[o:o + 32], va, 4))) == 4
        return False

    def taken_address(va):
        """A function start that only a pointer leads to."""
        return (va % 16 == 0 and va not in starts and any(a <= va < b for a, b in code)
                and (byte_at(va - 1) in (0xCC, 0x90, 0xC3) or byte_at(va - 3) == 0xC2
                     or after_jump_table(info, image, va))
                and decodes(va))

    missed = set()
    # A code section starts with a function. Orta's Sofdec decoder sections
    # (PSGSFD_I, PSGSFD_P) are reached only through pointers set up at run
    # time, and the start of a section follows no padding of its own.
    for a, _ in code:
        if a not in starts and not inside(a) and decodes(a):
            missed.add(a)

    data_secs = [".rdata"] + DATA_SECTIONS.split(",")
    for sec in info["sections"]:
        if sec["name"] not in data_secs:
            continue
        raw = image[sec["raw"]:sec["raw"] + sec["rsize"]]
        for off in range(0, len(raw) - 3, 4):
            va = int.from_bytes(raw[off:off + 4], "little")
            if taken_address(va):
                missed.add(va)

    for x in xrefs:
        if x["type"] == "data_imm":
            if inside(int(x["from"], 16)) and taken_address(int(x["to"], 16)):
                missed.add(int(x["to"], 16))
            continue
        if x["type"] not in ("call", "jump", "cond_jump"):
            continue
        src, dst = int(x["from"], 16), int(x["to"], 16)
        if dst in starts or not inside(src) or not any(a <= dst < b for a, b in code):
            continue
        # A jump into another function's span is a tail call into code the
        # sweep folded into the function before it, when it lands where a
        # function would start (Sofdec's 0x00244600, after 0x00244590's ret
        # and padding: its stub returned non-zero and stopped the movie's
        # stream callbacks). A jump within a function is an ordinary branch.
        if x["type"] != "call" and inside(dst) and not (
                owner(dst) != owner(src) and taken_address(dst)):
            continue
        missed.add(dst)
    return sorted(missed)


def after_jump_table(info, image, va):
    """va follows a switch jump table: its last entry, the dword before va,
    points back into the code just above it."""
    for sec in info["sections"]:
        if sec["va"] <= va - 4 and va <= sec["va"] + sec["rsize"]:
            o = sec["raw"] + va - 4 - sec["va"]
            entry = int.from_bytes(image[o:o + 4], "little")
            return va - 0x10000 <= entry < va - 4
    return False


def false_splits(disasm_dir, xbe, candidates):
    """Seeds that cut a function in two: the function before the address runs
    straight into it, so it is that function's own code, not a start.

    The disassembler accepts a seed at any instruction boundary. A call
    decoded from a data table (a decoder's lookup table in .text) seeded
    0x00262123, three bytes into Sofdec's block decoder 0x00262120, and
    0x0025E792 in the middle of the other one; the cut-off pieces fell
    through to tail calls, edi was not preserved, and the movie decoder
    crashed on a garbage context pointer in the opening movie. Decoding the
    preceding function from its start shows the instruction that ends at the
    address: anything but a ret, an unconditional jmp, a call (it may not
    return), int3/hlt/ud2 or padding means control falls through into it.
    """
    with open(os.path.join(disasm_dir, "functions.json")) as f:
        funcs = json.load(f)
    with open(xbe, "rb") as f:
        image = f.read()
    info = xbe_info.parse(image)
    spans = sorted((int(e["start"], 16), int(e["start"], 16) + e["size"]) for e in funcs)
    lo = [a for a, _ in spans]

    import bisect
    import capstone
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)

    def read(va, n):
        for sec in info["sections"]:
            if sec["va"] <= va < sec["va"] + sec["rsize"]:
                o = sec["raw"] + va - sec["va"]
                return image[o:o + min(n, sec["va"] + sec["rsize"] - va)]
        return b""

    padding = re.compile(r"(\w+), (?:dword ptr )?\[?\1(?: \+ 0)?\]?")
    bad = set()
    for va in candidates:
        # The function the address follows on from: it starts before va and
        # reaches it (a split piece ends exactly at the next start).
        i = bisect.bisect_left(lo, va) - 1
        if i < 0 or spans[i][1] < va:
            continue
        start = spans[i][0]
        if va - start > 0x10000 or after_jump_table(info, image, va):
            continue    # data before va: nothing falls through into it
        last = None
        for insn in md.disasm(read(start, va - start), start):
            last = insn
            if insn.address + insn.size >= va:
                break
        if last is None or last.address + last.size != va:
            continue
        m = last.mnemonic
        if (m in ("ret", "retf", "jmp", "call", "int3", "hlt", "ud2", "nop")
                or (m in ("lea", "mov") and padding.fullmatch(last.op_str))):
            continue
        bad.add(va)
    return bad


# ── Names carried over from JSRF ────────────────────────────────────────

def apply_names(names_json, functions_json):
    """Rename sub_XXXXXXXX entries tools/xbe_match.py found a JSRF name for.

    Every name keeps its address as a suffix: unique, never collides with a
    host C library symbol, and still greppable by address.
    """
    sys.path.insert(0, XBOXRECOMP)
    from tools.ghidra_naming.merge_names import sanitize

    with open(names_json) as f:
        names = {int(va, 16): e["name"] for va, e in json.load(f).items()}
    with open(functions_json) as f:
        entries = json.load(f)
    renamed = 0
    for e in entries:
        va = int(e["start"], 16)
        name = names.get(va)
        if not name:
            continue
        name = sanitize(name.replace("::", "_").replace("~", "dtor_"))
        if name:
            e["name"] = f"{name}_{va:08X}"
            renamed += 1
    with open(functions_json, "w") as f:
        json.dump(entries, f, indent=1)
    return renamed


# ── Main ────────────────────────────────────────────────────────────────

def collect_overrides(path):
    """Concatenate our hand-written C into one file for --exclude-manual.

    The recompiler scans it for `void sub_XXXXXXXX(void)` definitions (skipped
    in the generated code so ours links instead), `sub_XXXXXXXX_gen` wrappers
    and `sub_XXXXXXXX_enter` hooks. Scanning every source under src/ keeps the
    code itself the single source of truth for what is overridden.
    """
    parts = []
    for base, dirs, files in os.walk(os.path.join(ROOT, "src")):
        dirs.sort()
        for name in sorted(files):
            if name.endswith(".c"):
                with open(os.path.join(base, name), encoding="utf-8") as f:
                    parts.append(f.read())
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(parts))


def extract_xbe_from_iso(iso, work):
    run_tool("xiso", "get", os.path.abspath(iso), "default.xbe", "-o", work)
    return os.path.join(work, "default.xbe")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("input", help="default.xbe or a disc image (.iso/.xiso)")
    ap.add_argument("--names", help="names.json from tools/xbe_match.py (JSRF names)")
    ap.add_argument("--work", default=os.path.join(ROOT, "build", "recomp-work"))
    ap.add_argument("--gen", default=os.path.join(ROOT, "gen"))
    ap.add_argument("--split", default="250", help="functions per generated .c file")
    ap.add_argument("--trace-functions", metavar="JSON|all",
                    help="bring-up: emit an entry trace hook in these functions "
                         "(a JSON address list, or 'all'); see recomp_trace.c")
    args = ap.parse_args()

    if not os.path.isfile(os.path.join(XBOXRECOMP, "tools", "recomp", "__main__.py")):
        fail("external/xboxrecomp is empty; run: git submodule update --init")
    try:
        import capstone  # noqa: F401
    except ImportError:
        fail("the capstone module is missing; run: python -m pip install capstone")

    apply_patches()

    work = os.path.abspath(args.work)
    gen = os.path.abspath(args.gen)
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)

    src = os.path.abspath(args.input)
    if src.lower().endswith((".iso", ".xiso")):
        src = extract_xbe_from_iso(src, work)
    xbe = os.path.join(work, "default.xbe")
    if src != xbe:
        shutil.copyfile(src, xbe)

    with open(xbe, "rb") as f:
        desc, image_sha = identify(f.read())
    print(f"Panzer Dragoon Orta: {desc}")

    d = lambda name: os.path.join(work, name)  # noqa: E731
    run_tool("xbe_parser", xbe, "--json", d("default_analysis.json"), "--quiet")

    disasm_args = [xbe, "-o", d("disasm"), "--analysis-json", d("default_analysis.json"),
                   "--exclude-sections", DATA_SECTIONS,
                   # Function entries seen at runtime that the static sweep misses.
                   "--seed-functions", os.path.join(ROOT, "tools", "pdo_seeds.json")]
    run_tool("disasm", *disasm_args)

    # Direct calls (and tail jumps) to code the sweep did not turn into a
    # function: seed them and disassemble again until none are left.
    seeded, rejected = set(), set()
    for _ in range(5):
        missed = set(missed_targets(d("disasm"), xbe)) - seeded - rejected
        # Checked against the latest disassembly every round: a start found
        # in the same round as the seed can make it a split only afterwards.
        cut = false_splits(d("disasm"), xbe, missed | seeded)
        rejected |= cut
        missed -= cut
        if not missed and not (seeded & cut):
            break
        print(f"\n{len(missed)} call/jump targets are not functions yet; seeding them"
              + (f" ({len(cut)} rejected: they would cut a function)" if cut else ""))
        seeded = (seeded - cut) | missed
        with open(d("missed_seeds.json"), "w") as f:
            json.dump([{"start": f"0x{a:08X}", "note": "direct call target"} for a in sorted(seeded)], f)
        run_tool("disasm", *disasm_args, "--seed-functions", d("missed_seeds.json"), "--force")

    if args.names:
        n = apply_names(args.names, d("disasm/functions.json"))
        print(f"named {n} functions from {args.names}")

    run_tool("func_id", xbe, "--functions", d("disasm/functions.json"),
             "--strings", d("disasm/strings.json"), "--xrefs", d("disasm/xrefs.json"),
             "-o", d("func_id"))
    run_tool("abi_analysis", xbe, "--disasm-dir", d("disasm"),
             "--func-id-dir", d("func_id"), "--output-dir", d("abi"))

    collect_overrides(d("pdo_overrides.c"))
    shutil.rmtree(gen, ignore_errors=True)
    trace = []
    if args.trace_functions:
        trace = ["--trace-functions", d("disasm/functions.json")
                 if args.trace_functions == "all" else args.trace_functions]
    run_tool("recomp", xbe, "--all", "--split", args.split,
             "--disasm-dir", d("disasm"), "--func-id-dir", d("func_id"),
             "--abi-dir", d("abi"), "-o", d("recomp"), "--gen-dir", gen,
             "--exclude-manual", d("pdo_overrides.c"), *trace)

    with open(os.path.join(gen, "pdo_build_info.h"), "w") as f:
        f.write("/* Generated by tools/recomp.py. Do not edit. */\n"
                "#pragma once\n"
                f'#define PDO_BUILD_IMAGE_SHA256 "{image_sha}"\n')

    count = len([n for n in os.listdir(gen) if n.endswith(".c")])
    print(f"\nDone: {count} C files in {gen}. Now build with CMake (see BUILDING.md).")


if __name__ == "__main__":
    main()
