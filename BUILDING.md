# Building Guide

You need your own copy of **Panzer Dragoon Orta (USA)**. Nothing from the
game is committed to this repository; the recompiled code is generated from
your `default.xbe` locally, into `gen/`, which git ignores.

## 1. Clone with submodules

```bash
git clone --recurse-submodules https://github.com/wootbeer/ortarecomp.git
# if you forgot --recurse-submodules:
git submodule update --init --recursive
```

This pulls in `external/xboxrecomp` (the recompiler and kernel layer) and
`external/SDL` (SDL 3).

## 2. Install dependencies

All platforms need **CMake 3.20+**, **Ninja** (recommended) and
**Python 3.10+** with the `capstone` module:

```bash
python -m pip install capstone
```

### Windows

Visual Studio 2022 with the "Desktop development with C++" workload, which
includes CMake and Ninja. Build from the "x64 Native Tools Command Prompt".

### Linux (Ubuntu/Debian)

```bash
sudo apt-get install build-essential cmake ninja-build python3-pip libssl-dev \
    libx11-dev libxext-dev libxcursor-dev libxi-dev libxrandr-dev libxss-dev \
    libxtst-dev libxfixes-dev libxkbcommon-dev libwayland-dev libdbus-1-dev \
    libudev-dev libpulse-dev libasound2-dev
```

## 3. Generate the recompiled code

Point the script at your `default.xbe`, or straight at your disc image:

```bash
python tools/recomp.py path/to/default.xbe
python tools/recomp.py path/to/Orta.iso
```

It checks that this is the USA release, then writes almost two million lines
of C into `gen/`. This takes about five minutes.

**Optional:** names from Jet Set Radio Future. Orta shares code with JSRF
(same studio, same period). If you also own JSRF, `tools/xbe_match.py` finds
the shared functions and writes their names to `build/match/names.json`,
which `--names` applies to the generated code:

```bash
python tools/xbe_match.py path/to/orta/default.xbe path/to/jsrf/default.xbe \
    --col path/to/conceptoflove [--jsrf-listing path/to/JSRF-Decompilation]
python tools/recomp.py path/to/default.xbe --names build/match/names.json
```

## 4. Build

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
```

The executable is `build/OrtaRecomp` (`OrtaRecomp.exe` on Windows).
Without step 3, this builds the launcher on its own, which is enough to test
the game-file setup.

If configure stops with "Could not apply ... to external/xboxrecomp" (a
patch changed since it was applied), reset the checkout and configure again;
every file the reset removes came from those patches:

```bash
git -C external/xboxrecomp checkout -- .
git -C external/xboxrecomp clean -fd
```

## 5. Run

```
OrtaRecomp                  first run asks for your disc image or game folder
OrtaRecomp --setup          choose the game files again
OrtaRecomp --import-iso F   import a disc image without the dialogs
OrtaRecomp --game-dir DIR   use an extracted game folder for this run
OrtaRecomp --check          check the game files, print the result, exit
OrtaRecomp --boot           start the title (in development; not playable)
```
