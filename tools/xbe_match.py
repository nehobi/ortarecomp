#!/usr/bin/env python3
"""
Find the functions Panzer Dragoon Orta shares with Jet Set Radio Future.

    python tools/xbe_match.py ORTA/default.xbe JSRF/default.xbe \\
        --col ../conceptoflove [--jsrf-listing ../JSRF-Decompilation]

Both games are Smilebit titles of the same period, so they likely share XDK
libraries (D3D, DSOUND, XAPI...), CRI middleware and maybe engine code. Every
function both contain is work Concept of Love already did: its name, its
override in conceptoflove/src/guest (re-addressed), its lifter fixes.

How: both XBEs go through xboxrecomp's disassembler (cached under
build/match/). Each function is fingerprinted from its instructions with
image addresses masked out (calls and jumps leaving the function, data
addresses), kernel imports kept by ordinal. Then:

  exact      same fingerprint, unique in both (alone, or together with the
             fingerprints of its callees): the same code.
  exact-order  same fingerprint, several copies in both, paired in address
             order (weaker).
  callgraph  called from the same place in two solid pairs, size within
             1.5x; shown with its 3-gram similarity. Weakest: check it.
  similar    not identical (another XDK build, a small change), but most
             instruction 3-grams shared; only tried where it matters: XDK
             sections, and JSRF functions Concept of Love overrides.

Matched pairs that are identical also pair the data addresses they use, so
JSRF tables the overrides read (D3D__RenderState and friends) get their Orta
address too.

Everything is written under build/match/ (git-ignored) and to the terminal.
Nothing from either game goes in the repository; the names come from your
local clones at run time. names.json there feeds tools/recomp.py --names.
"""

import argparse
import collections
import csv
import hashlib
import json
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import xbe_info  # noqa: E402

try:
    import capstone
    from capstone import x86
except ImportError:
    sys.exit("error: the capstone module is missing; run: python -m pip install capstone")

CODE_SECTIONS_SKIP = {".rdata", ".data", ".data1", "APPDATA", "DOLBY", "XON_RD", ".XTLID", "$$XTIMAGE"}
SIMILAR_MIN = 0.80


# ── Loading ─────────────────────────────────────────────────────────────

class Image:
    """One XBE: bytes by VA, functions from xboxrecomp's disassembler."""

    def __init__(self, path, work, seeds=None):
        with open(path, "rb") as f:
            self.data = f.read()
        self.info = xbe_info.parse(self.data)
        self.sections = self.info["sections"]
        self.lo = min(s["va"] for s in self.sections)
        self.hi = max(s["va"] + s["vsize"] for s in self.sections)
        self.thunks = self._thunks()
        self.functions = self._functions(path, work, seeds)

    def _thunks(self):
        """{slot VA: kernel ordinal} for the kernel import table."""
        out, va = {}, self.info["kernel_thunk"]
        while True:
            raw = self.read(va, 4)
            if raw is None:
                break
            v, = struct.unpack("<I", raw)
            if not v:
                break
            out[va] = v & 0x7FFFFFFF
            va += 4
        return out

    def section_of(self, va):
        for s in self.sections:
            if s["va"] <= va < s["va"] + s["vsize"]:
                return s["name"]
        return None

    def read(self, va, n):
        for s in self.sections:
            if s["va"] <= va and va + n <= s["va"] + s["rsize"]:
                o = s["raw"] + va - s["va"]
                return self.data[o:o + n]
        return None

    def _functions(self, path, work, seeds):
        from recomp import DATA_SECTIONS, apply_patches, run_tool  # same pipeline
        out = os.path.join(work, self.info["image_sha256"][:16])
        fj = os.path.join(out, "disasm", "functions.json")
        if not os.path.isfile(fj):
            os.makedirs(out, exist_ok=True)
            apply_patches()
            analysis = os.path.join(out, "analysis.json")
            src = os.path.abspath(path)
            run_tool("xbe_parser", src, "--json", analysis, "--quiet")
            args = [src, "-o", os.path.join(out, "disasm"), "--analysis-json", analysis,
                    "--exclude-sections", DATA_SECTIONS]
            if seeds:
                args += ["--seed-functions", seeds]
            run_tool("disasm", *args)
        with open(fj) as f:
            funcs = json.load(f)
        result = {}
        for e in funcs:
            start = int(e["start"], 16)
            size = e.get("size") or int(e["end"], 16) - start
            sec = e.get("section") or self.section_of(start)
            if size > 0 and sec not in CODE_SECTIONS_SKIP:
                result[start] = (size, sec)
        return result


# ── Fingerprints ────────────────────────────────────────────────────────

class Func:
    __slots__ = ("va", "size", "section", "tokens", "hash", "hash2", "calls", "refs", "grams")


def fingerprint(img, md, va, size, section):
    code = img.read(va, size) or b""
    f = Func()
    f.va, f.size, f.section = va, size, section
    f.tokens, f.calls, f.refs, f.grams = [], [], [], None

    def addr(v):
        """Token for a constant: masked when it is an image address."""
        v &= 0xFFFFFFFF
        if v in img.thunks:
            return f"K{img.thunks[v]}"
        if img.lo <= v < img.hi:
            f.refs.append(v)
            return "A"
        return hex(v)

    for insn in md.disasm(code, va):
        ops = []
        branch = insn.group(x86.X86_GRP_JUMP) or insn.group(x86.X86_GRP_CALL)
        for op in insn.operands:
            if op.type == x86.X86_OP_REG:
                ops.append(insn.reg_name(op.reg))
            elif op.type == x86.X86_OP_IMM:
                if branch:
                    t = op.imm & 0xFFFFFFFF
                    if va <= t < va + size:
                        ops.append(f"L{t - va}")
                    else:
                        ops.append("F")
                        if insn.group(x86.X86_GRP_CALL):
                            f.calls.append(t)
                else:
                    ops.append(addr(op.imm))
            elif op.type == x86.X86_OP_MEM:
                m = op.mem
                ops.append("[%s%s%s*%d+%s]" % (
                    insn.reg_name(m.segment) + ":" if m.segment else "",
                    insn.reg_name(m.base) if m.base else "",
                    insn.reg_name(m.index) if m.index else "",
                    m.scale, addr(m.disp)))
        f.tokens.append(insn.mnemonic + " " + ",".join(ops))
    f.hash = hashlib.sha1("\n".join(f.tokens).encode()).hexdigest()
    return f


def fingerprint_all(img):
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.detail = True
    funcs = {va: fingerprint(img, md, va, size, sec)
             for va, (size, sec) in img.functions.items()}
    # A second fingerprint that includes what each function calls. Masking
    # call targets makes thin wrappers (push args, call the implementation)
    # all look alike; their callees tell them apart.
    for f in funcs.values():
        callees = [funcs[c].hash if c in funcs else "?" for c in f.calls]
        f.hash2 = hashlib.sha1((f.hash + "".join(callees)).encode()).hexdigest()
    return funcs


def grams(f):
    if f.grams is None:
        t = f.tokens
        f.grams = {hash((t[i], t[i + 1], t[i + 2])) for i in range(len(t) - 2)} or {hash(tuple(t))}
    return f.grams


# ── Names from Concept of Love and the JSRF decomp listing ──────────────

SUB_RE = re.compile(r"^void sub_([0-9A-Fa-f]{8})(_enter)?\s*\(void\)")
TRACE_RE = re.compile(r'd3d_trace_call\(0x([0-9A-Fa-f]{8})u?,\s*"([^"]*)"')
CALL_NAME_RE = re.compile(r"([A-Za-z_][A-Za-z0-9_]*)\s*\(")
HEX_RE = re.compile(r"\b0x([0-9A-Fa-f]{6,8})u?\b")
C_TYPES = {"VOID", "void", "DWORD", "BOOL", "HANDLE", "HRESULT", "ULONG", "LONG",
           "int", "unsigned", "WORD", "BYTE", "PVOID", "D3DCOLOR", "UINT", "FLOAT"}


def load_col(col):
    """JSRF addresses Concept of Love overrides or hooks, with what it calls them.

    Returns ({va: [(kind, name, file)]}, {constant va: [file]}).
    """
    overrides = collections.defaultdict(list)
    constants = collections.defaultdict(set)
    src = os.path.join(col, "src")
    for base, _, files in os.walk(src):
        for name in sorted(files):
            if not name.endswith(".c"):
                continue
            path = os.path.join(base, name)
            rel = os.path.relpath(path, col)
            with open(path, encoding="utf-8", errors="replace") as f:
                lines = f.read().splitlines()
            for i, line in enumerate(lines):
                for m in HEX_RE.finditer(line):
                    constants[int(m.group(1), 16)].add(rel)
                m = TRACE_RE.search(line)
                if m:
                    overrides[int(m.group(1), 16)].append(("trace", m.group(2), rel))
                    continue
                m = SUB_RE.match(line)
                if not m:
                    continue
                overrides[int(m.group(1), 16)].append(
                    ("hook" if m.group(2) else "override", _comment_name(lines, i), rel))
    return overrides, constants


def _comment_name(lines, i):
    """The API name in the comment block just above line i, if there is one."""
    j = i - 1
    block = []
    while j >= 0 and (lines[j].lstrip().startswith(("/*", "*", "//")) or lines[j].rstrip().endswith("*/")):
        block.insert(0, lines[j])
        j -= 1
    for line in block:
        for m in CALL_NAME_RE.finditer(line):
            if m.group(1) not in C_TYPES:
                return m.group(1)
    return ""


def load_listing(path):
    tsv = path
    if os.path.isdir(path):
        tsv = os.path.join(path, "ghidra", "import", "listing.tsv")
    if not os.path.isfile(tsv):
        sys.exit(f"error: no listing.tsv at {tsv}")
    names = {}
    with open(tsv, newline="", encoding="utf-8") as f:
        for row in csv.reader(f, delimiter="\t"):
            if len(row) >= 9 and row[1] == "func" and not row[8].startswith(("FUN_", "LAB_")):
                ns = row[7] if row[7] and row[7] != "Global" else ""
                names[int(row[0], 16)] = f"{ns}::{row[8]}" if ns else row[8]
    return names


# ── Matching ────────────────────────────────────────────────────────────

def similarity(a, b):
    ga, gb = grams(a), grams(b)
    return len(ga & gb) / len(ga | gb)


def match(pf, jf, interesting):
    """{orta va: (jsrf va, how)}"""
    pairs, used_j = {}, set()

    def add(p, j, how):
        if p in pairs or j in used_j or p not in pf or j not in jf:
            return False
        pairs[p] = (j, how)
        used_j.add(j)
        return True

    def groups(key):
        gp, gj = collections.defaultdict(list), collections.defaultdict(list)
        for f in pf.values():
            if f.va not in pairs:
                gp[getattr(f, key)].append(f.va)
        for f in jf.values():
            if f.va not in used_j:
                gj[getattr(f, key)].append(f.va)
        return gp, gj

    # Identical code, unique on both sides; then identical code calling
    # identical code, which separates look-alike wrappers.
    for key in ("hash", "hash2"):
        gp, gj = groups(key)
        for h, ps in gp.items():
            js = gj.get(h)
            if js and len(ps) == 1 and len(js) == 1:
                add(ps[0], js[0], "exact")

    # Call-graph propagation: two identical (or near-identical) functions
    # call corresponding functions, in order. Only solid pairs propagate:
    # a guess made from a guess is not evidence. A callee pair whose sizes
    # differ wildly is no pair at all.
    def solid(how):
        return how == "exact" or (how.startswith("similar") and float(how.split()[1]) >= 0.9)

    def propagate():
        queue = [p for p, (_, how) in pairs.items() if solid(how)]
        while queue:
            p = queue.pop()
            j, _ = pairs[p]
            cp, cj = pf[p].calls, jf[j].calls
            if len(cp) != len(cj):
                continue
            for a, b in zip(cp, cj):
                if a not in pf or b not in jf:
                    continue
                if pf[a].hash == jf[b].hash:
                    if add(a, b, "exact"):
                        queue.append(a)
                elif 0.67 <= jf[b].size / pf[a].size <= 1.5:
                    add(a, b, f"callgraph {similarity(pf[a], jf[b]):.2f}")

    propagate()

    # Identical functions that occur more than once and that no caller
    # placed: pair them in address order when both sides have the same
    # number left. Linkers keep library order, but this is weaker evidence.
    gp, gj = groups("hash2")
    for h, ps in gp.items():
        js = gj.get(h, [])
        if len(ps) == len(js):
            for a, b in zip(sorted(ps), sorted(js)):
                add(a, b, "exact-order")

    # Similar: by 3-gram overlap, only for code worth the time and long
    # enough for the overlap to mean something.
    cand = collections.defaultdict(list)
    for j in jf.values():
        if (j.va not in used_j and len(j.tokens) >= 12
                and (j.section != ".text" or j.va in interesting)):
            cand[j.section if j.section != ".text" else "*"].append(j)
    for p in sorted(pf.values(), key=lambda f: f.va):
        if p.va in pairs or len(p.tokens) < 12:
            continue
        pool = cand.get(p.section, []) + cand.get("*", [])
        best, best_s = None, 0.0
        for j in pool:
            if j.va in used_j or not (0.75 <= j.size / p.size <= 1.33):
                continue
            s = similarity(p, j)
            if s > best_s:
                best, best_s = j, s
        if best and best_s >= SIMILAR_MIN:
            add(p.va, best.va, f"similar {best_s:.2f}")

    propagate()
    return pairs


def data_map(pairs, pf, jf):
    """{jsrf data va: (orta data va, votes, total)} from identical pairs."""
    votes = collections.defaultdict(collections.Counter)
    for p, (j, how) in pairs.items():
        if how.startswith("exact") and len(pf[p].refs) == len(jf[j].refs):
            for a, b in zip(pf[p].refs, jf[j].refs):
                votes[b][a] += 1
    out = {}
    for j, c in votes.items():
        (p, n), = c.most_common(1)
        out[j] = (p, n, sum(c.values()))
    return out


# ── Report ──────────────────────────────────────────────────────────────

def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("orta", help="Panzer Dragoon Orta default.xbe")
    ap.add_argument("jsrf", help="Jet Set Radio Future default.xbe")
    ap.add_argument("--col", help="local clone of Concept of Love (its overrides and seeds)")
    ap.add_argument("--jsrf-listing", help="local JSRF-Decompilation clone or its listing.tsv")
    ap.add_argument("--work", default=os.path.join(ROOT, "build", "match"))
    args = ap.parse_args()

    work = os.path.abspath(args.work)
    seeds = os.path.join(args.col, "tools", "jsrf_seeds.json") if args.col else None
    print("== Orta")
    orta = Image(args.orta, work)
    print("== JSRF")
    jsrf = Image(args.jsrf, work, seeds if seeds and os.path.isfile(seeds) else None)

    overrides, constants = load_col(args.col) if args.col else ({}, {})
    listing = load_listing(args.jsrf_listing) if args.jsrf_listing else {}

    print("\nfingerprinting...", flush=True)
    pf, jf = fingerprint_all(orta), fingerprint_all(jsrf)
    pairs = match(pf, jf, set(overrides))
    dmap = data_map(pairs, pf, jf)

    def jname(j):
        names = [n for _, n, _ in overrides.get(j, []) if n and n != "?"]
        return listing.get(j) or (names[0] if names else "")

    lines = ["# Orta vs JSRF function match",
             "",
             f"Orta: {orta.info['title']} {orta.info['title_id']:08X}, XDK {orta.info['xdk_build']}, "
             f"{len(pf)} functions",
             f"JSRF: {jsrf.info['title']} {jsrf.info['title_id']:08X}, XDK {jsrf.info['xdk_build']}, "
             f"{len(jf)} functions",
             ""]

    lines += ["## By section", "", "| section | Orta functions | matched | exact |", "|---|---|---|---|"]
    per = collections.defaultdict(lambda: [0, 0, 0])
    for f in pf.values():
        per[f.section][0] += 1
        if f.va in pairs:
            per[f.section][1] += 1
            per[f.section][2] += pairs[f.va][1].startswith("exact")
    for sec, (n, m, e) in sorted(per.items(), key=lambda x: -x[1][0]):
        lines.append(f"| {sec} | {n} | {m} | {e} |")

    rev = {j: p for p, (j, _) in pairs.items()}
    lines += ["", "## Concept of Love overrides and hooks", "",
              "| JSRF | name | kind | file | Orta | how |", "|---|---|---|---|---|---|"]
    carried = 0
    for j in sorted(overrides):
        kinds = sorted({k for k, _, _ in overrides[j]})
        files = sorted({fl for _, _, fl in overrides[j]})
        p = rev.get(j)
        carried += p is not None
        lines.append(f"| {j:08X} | {jname(j)} | {','.join(kinds)} | {','.join(files)} | "
                     f"{f'{p:08X}' if p else '-'} | {pairs[p][1] if p else ''} |")

    if args.col:
        lines += ["", "## Data addresses in Concept of Love's code", "",
                  "Constants in its sources that are JSRF data addresses, paired "
                  "through identical functions (votes / uses).", "",
                  "| JSRF | Orta | votes | files |", "|---|---|---|---|"]
        for j in sorted(constants):
            if j in dmap:
                p, n, tot = dmap[j]
                lines.append(f"| {j:08X} | {p:08X} | {n}/{tot} | {', '.join(sorted(constants[j]))} |")

    if seeds and os.path.isfile(seeds):
        with open(seeds) as f:
            js = [int(e["start"], 16) for e in json.load(f)]
        lines += ["", "## JSRF runtime seeds found in Orta (candidates for tools/pdo_seeds.json)", ""]
        for j in js:
            lines.append(f"- {j:08X} -> {rev[j]:08X}" if j in rev else f"- {j:08X} -> not matched")

    lines += ["", "## All matches", "", "| Orta | JSRF | section | how | name |", "|---|---|---|---|---|"]
    for p in sorted(pairs):
        j, how = pairs[p]
        lines.append(f"| {p:08X} | {j:08X} | {pf[p].section} | {how} | {jname(j)} |")

    names = {}
    for p, (j, how) in pairs.items():
        n = jname(j)
        if n:
            names[f"0x{p:08X}"] = {"name": n, "jsrf": f"0x{j:08X}", "how": how}

    os.makedirs(work, exist_ok=True)
    report = os.path.join(work, "report.md")
    with open(report, "w") as f:
        f.write("\n".join(lines) + "\n")
    with open(os.path.join(work, "names.json"), "w") as f:
        json.dump(names, f, indent=1, sort_keys=True)

    how = collections.Counter(h.split()[0] for _, h in pairs.values())
    print(f"\nOrta XDK {orta.info['xdk_build']}, JSRF XDK {jsrf.info['xdk_build']}")
    print(f"matched {len(pairs)} of {len(pf)} Orta functions: "
          + ", ".join(f"{n} {k}" for k, n in how.most_common()))
    for sec, (n, m, e) in sorted(per.items(), key=lambda x: -x[1][0]):
        print(f"  {sec:<10} {m:>6} / {n:<6} ({e} exact)")
    if overrides:
        print(f"Concept of Love overrides/hooks found in Orta: {carried} of {len(overrides)}")
    print(f"\nreport: {report}\nnames : {os.path.join(work, 'names.json')} "
          f"({len(names)} names, for tools/recomp.py --names)")


if __name__ == "__main__":
    main()
