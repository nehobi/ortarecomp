"""Builds a release and zips it for players.

    python tools/package.py

Needs gen/ (tools/recomp.py) first. Builds into build-release/ with no
console window on Windows and the C runtime linked in, then writes
dist/OrtaRecomp-v<version>-<platform>.zip: the executable, README,
CHANGELOG and the licenses. Run it from a Visual Studio developer shell
on Windows, like a normal build.
"""
import os
import platform
import re
import subprocess
import sys
import zipfile

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
BUILD = os.path.join(ROOT, "build-release")
DIST = os.path.join(ROOT, "dist")


def run(*cmd):
    print("+", " ".join(cmd), flush=True)
    if subprocess.run(cmd, cwd=ROOT).returncode != 0:
        sys.exit(f"error: {cmd[0]} failed")


def main():
    if not os.path.exists(os.path.join(ROOT, "gen", "recomp_dispatch.c")):
        sys.exit("error: no generated code; run tools/recomp.py on your default.xbe first")
    with open(os.path.join(ROOT, "CMakeLists.txt")) as f:
        version = re.search(r"project\(OrtaRecomp VERSION ([0-9.]+)", f.read()).group(1)

    # RelWithDebInfo: the configuration every test so far ran; the symbols
    # stay in build-release/ (the .pdb), for crash reports.
    run("cmake", "-S", ".", "-B", BUILD, "-G", "Ninja",
        "-DCMAKE_BUILD_TYPE=RelWithDebInfo", "-DPDO_RELEASE=ON")
    run("cmake", "--build", BUILD)

    windows = platform.system() == "Windows"
    exe = "OrtaRecomp.exe" if windows else "OrtaRecomp"
    tag = "windows-x64" if windows else f"{platform.system().lower()}-{platform.machine().lower()}"
    os.makedirs(DIST, exist_ok=True)
    out = os.path.join(DIST, f"OrtaRecomp-v{version}-{tag}.zip")
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        z.write(os.path.join(BUILD, exe), exe)
        for name in ("README.md", "CHANGELOG.md", "COPYING"):
            z.write(os.path.join(ROOT, name), name)
        for name in sorted(os.listdir(os.path.join(ROOT, "licenses"))):
            z.write(os.path.join(ROOT, "licenses", name), f"licenses/{name}")
    print(f"wrote {out}")


if __name__ == "__main__":
    main()
