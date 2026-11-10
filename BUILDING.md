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

The game can use Lossless Scaling's frame generation (LSFG 3.1) with your own
copy of Lossless Scaling's `Lossless.dll` (`lib/lsfg`). Building it needs
SPIR-V's headers (`spirv/unified1/spirv.hpp`): the
[Vulkan SDK](https://vulkan.lunarg.com/) has them (found through
`VULKAN_SDK`), or point `-DPDO_SPIRV_HEADERS=...` at the `include` folder of
a [SPIRV-Headers](https://github.com/KhronosGroup/SPIRV-Headers) clone. On
Linux, `sudo apt-get install spirv-headers` puts them in `/usr/include`
(Ubuntu 24.04's are new enough; 22.04's are too old for the shader compiler,
so there point `PDO_SPIRV_HEADERS` at a SPIRV-Headers clone's `include`).
Without them the game builds without it; CMake says which.

### Windows

Visual Studio 2022 with the "Desktop development with C++" workload, which
includes CMake and Ninja. Build from the "x64 Native Tools Command Prompt".

### Linux (Ubuntu/Debian)

```bash
sudo apt-get install build-essential g++ cmake ninja-build python3-pip libssl-dev \
    libx11-dev libxext-dev libxcursor-dev libxi-dev libxrandr-dev libxss-dev \
    libxtst-dev libxfixes-dev libxkbcommon-dev libwayland-dev libdecor-0-dev \
    libdbus-1-dev libudev-dev libpulse-dev libpipewire-0.3-dev libasound2-dev \
    spirv-headers
```

SDL loads X11, Wayland, the sound servers and Vulkan at run time, so the
executable needs only the C library; the packages above are what lets SDL
build that support in. For a release that runs on the Steam Deck and other
distributions, build on Ubuntu 22.04 (WSL2 on Windows works): the binary
then needs no newer glibc than 2.35 (SteamOS has newer).

## 3. Generate the recompiled code

Point the script at your `default.xbe`, or straight at your disc image:

```bash
python tools/recomp.py path/to/default.xbe
python tools/recomp.py path/to/Orta.iso
```

It checks that this is the USA release, then writes almost two million lines
of C into `gen/`. This takes about five minutes.

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
OrtaRecomp --boot           start the game at once, without the launcher
OrtaRecomp --log FILE       write the log straight into FILE (on Windows,
                            use this instead of piping through PowerShell,
                            which can stall the game)
```

## 6. Package a release

```bash
python tools/package.py
```

This builds into `build-release/` with no console window on Windows and the
C runtime linked in (players need no Visual C++ redistributable), then writes
`dist/OrtaRecomp-v<version>-windows-x64.zip` with the executable, README,
CHANGELOG and licenses. On Linux it writes
`dist/OrtaRecomp-v<version>-linux-x86_64.tar.gz` (a tarball keeps the
executable bit) with the binary stripped; the unstripped one stays in
`build-release/` to read crash reports (`addr2line -f -e OrtaRecomp 0xOFFSET`). Release builds always write `ortarecomp.log` in the
data folder; keep `build-release/OrtaRecomp.pdb` from the build you publish,
to read crash reports by their executable offsets.

The app icon is drawn by `tools/make_icon.py` (needs Pillow) into `icons/`.
