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
    run_tool("recomp", xbe, "--all", "--split", args.split,
             "--disasm-dir", d("disasm"), "--func-id-dir", d("func_id"),
             "--abi-dir", d("abi"), "-o", d("recomp"), "--gen-dir", gen,
             "--exclude-manual", d("pdo_overrides.c"))

    with open(os.path.join(gen, "pdo_build_info.h"), "w") as f:
        f.write("/* Generated by tools/recomp.py. Do not edit. */\n"
                "#pragma once\n"
                f'#define PDO_BUILD_IMAGE_SHA256 "{image_sha}"\n')

    count = len([n for n in os.listdir(gen) if n.endswith(".c")])
    print(f"\nDone: {count} C files in {gen}. Now build with CMake (see BUILDING.md).")


if __name__ == "__main__":
    main()
