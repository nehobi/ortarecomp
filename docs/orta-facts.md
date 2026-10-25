# Panzer Dragoon Orta: facts

Facts about the game's executable that the port depends on. Addresses are
virtual addresses in the USA `default.xbe`.

## The executable

- Panzer Dragoon ORTA (USA), title ID 53450007 (SE-007), North America,
  retail build, version 1. One dump known (hash in `src/xbe.c`).
- Entry 0x00231E6C, kernel thunk table 0x0030C8A0 (start of `.rdata`).
- **XDK 4928** (QFE 1) for every library: XAPILIB, D3D8, D3DX8, DSOUND,
  XGRAPHC, LIBCMT, LIBCPMT, XBOXKRNL. JSRF is 4134, so D3D render-state and
  texture-stage-state numbering must be re-derived from Orta's setters.

| Section | VA | Size | Notes |
|---|---|---|---|
| .text | 0x00011000 | 2,732,144 | game code, CRT, likely CRI |
| MMATRIX | 0x002AC080 | 6,432 | XDK matrix library |
| D3D | 0x002AD9A0 | 78,224 | Direct3D 8 |
| D3DX | 0x002C0B40 | 1,520 | |
| DSOUND | 0x002C1140 | 126,684 | DirectSound |
| WMADEC | 0x002E0020 | 102,624 | WMA decoder (not in JSRF) |
| PSGSFD00, _I, _B, _P | 0x002F9100.. | ~17,900 | probably CRI Sofdec (SFD movies): to confirm |
| XGRPH | 0x002FD740 | 36,960 | XGraphics (swizzling) |
| XPP | 0x003067A0 | 24,821 | XAPI input (XInput) |
| .rdata | 0x0030C8A0 | 213,060 | |
| APPDATA | 0x00340900 | 1,895,388 | writable data (no `.data` section); flagged executable, excluded from disassembly |
| DOLBY | 0x0050F4E0 | 29,056 | Dolby Digital encoder microcode |
| $$XTIMAGE | 0x00516660 | 10,240 | title image |

## Recompilation (xboxrecomp v0.13.1 + patches 0001-0012)

- The CRT saves MXCSR with `stmxcsr` at boot (0x00269711): patch 0012.
- 16,647 functions, all translated; ~1.78M lines of C in 67 files; 382
  call targets not detected as functions (stubbed).
- APPDATA is data but flagged executable: swept as code it gave 129 junk
  "functions" (no prologues, calling only each other), one of which did not
  compile (`lahf` after `add`, patch 0011). `recomp.py` excludes it (patch
  0010, `--exclude-sections`); its vtables then add ~660 real functions.
- `mov eax, dr0` twice in .text near 0x00211199 (data decoded as code) broke
  the build: patch 0009. Untranslated-instruction sites (`out`, `in`,
  `lcall`, `aas`...) are linear-sweep reads over data, as in JSRF.

## Bring-up findings

- Missed functions are dangerous, not just missing: the recompiler stubs a
  direct-call target it did not detect with an empty function that pops no
  arguments. XInitDevices (0x003068D4, at the start of XPP, reached through
  a jmp thunk at 0x003075AF) was one; its caller's stack shifted 8 bytes
  and a COM call five frames later read a wrong slot as its object.
  `recomp.py` now seeds every direct call target in code sections and
  disassembles again (154 more functions). The 245 stubs left are calls
  decoded from data.
- Game init (0x00011820) creates DirectSound through 0x0021E1E0 (same code
  as JSRF's 0x001680D0) and downloads `dsstdfx.bin` through 0x0021DE70
  (JSRF's 0x00168130); a failure sends it back to the dashboard.
- D3D device fields moved by 0x124 from 4134 (vblank event +0x2554, vblank
  callback +0x2550).

- A function can also start right after the switch jump tables of the one
  before it: 0x00111BC0, reached only through a pointer, follows
  0x00111990's tables. Undetected, every call to it failed from the start
  of episode 2's second boss fight, and the boss was not drawn. `recomp.py`
  seeds such addresses too (6 functions).
- Alias entries (a tail jump into the middle of a function) are lifted as
  their own body from the entry to the enclosing function's end. 224 of
  Orta's had a loop back-edge to before the entry (0x000B7082 loops to
  0x000B7050 in sub_000B6EA0), which became a call to a stub: the loop ran
  once and esp came back 4 bytes off. Patch 0024 makes such targets
  aliases too, until no new one turns up.

- Render callbacks: 0x00044830(list, func, arg, key) takes a 0x50-byte
  node (func +0, arg +4, next +8, key +0xC, matrix +0x10) from a pool of
  0x2000 nodes (base 0x419FBC, next free 0x419FB8, used 0x419FB4,
  capacity 0x419FB0; made by 0x00044790, reset every frame by 0x00044800,
  freed by 0x000447D0) without checking the capacity. Nine lists
  (head/tail pairs from 0x419F68) are sorted by key by 0x00044D50 (a
  merge sort, 0x00044970, through a static array at 0x3F8580) and walked
  by 0x00044A20. Episode 1 uses at most about 340 nodes a frame.

- Orta's threads assume one CPU. Run truly in parallel, the sound request
  processor (0x000692C0) walked a list (0x0021E570, 0x00220520) another
  thread was reallocating and called through a stale entry (a crash after
  episode 4). guest.c turns xboxrecomp's guest CPU lock on: one guest
  thread at a time, switching at kernel calls. CRI's spin thread
  (0x00233CB0) loops without a kernel call and is overridden to give the
  CPU up; CRI's region mutex (cri_hle.c) is waited on without it. CRI
  also suspends and resumes its threads around every locked region; under
  the lock that suspension is cooperative (patch 0023), since a host
  SuspendThread froze threads holding the guest CPU or that mutex. Guest
  code the kernel runs itself (file I/O completion APCs, exception
  handlers, DPCs) takes the guest CPU too, and our vblank wait gives it
  up while it sleeps.

## Episode 1 music (checked against a longplay)

- Music keys map to sound numbers through the table at 0x00340F70 (key,
  sound). Episode 1: 0x2777 = stage music (`s01_bgm1`), 0x2774 = boss
  music (`s01_bgm2`), 0x2775 = a short cue started and faded by zone
  triggers 303/304.
- The canyon is silent by design: the ramp event (0xF6AEA, started by
  trigger 204) fades 0x2777 over 600 frames and its completion callback
  0x000D66F0 jumps to segment 6. Trigger 406 at 3500 in that segment's list
  (0x00373B20) creates a fader whose callback 0x000D68A0 requests the boss
  scene 0xF6B58. The scene table at 0x00340DD8 (scene, keys, count) starts
  0x2774 for that scene through 0x00013CC0.
- Requests go through the queue at 0x0046D480 (enqueue 0x00068F60 and
  0x00068FB0, processed by 0x000692C0 into sound manager vtable 0x0032D4A0).
- Orta passes IDs that look like code addresses (scene 0x000F6B58, the
  argument 0x00225574 to 0x001B25B0). The disassembler took 0x00225574 for a
  function and cut the play method 0x00225270 there; every sound played
  through that path left the stack 44 bytes low, and the queue processor then
  dropped the requests after it in the same frame, the boss music among them
  (patch 0022).

## Rendering

- No multisampling (MultiSampleType 0x11, D3DMULTISAMPLE_NONE), a 16-bit
  640x480 back buffer, presentation flags 0x2E. The 3D scene is drawn into
  a 512x384 R5G6B5 target and stretched over the screen by the game;
  on hardware the TV encoder's flicker filter then softens it.

- Episode 2's camouflaged creatures mix 64% of a 128x128 copy of the frame
  (copied from the back buffer each frame) into their texture, through a
  pixel shader with texture mode PROJECT2D; their vertex program writes
  screen coordinates as (x, y, 0, w), so the lookup needs the divide by q.
- Particles are point sprites (states 116-123, set by the reset code at
  0x0027D539): sparks in episode 1 come from a vertex program that writes
  oPts; runs of 15 and 30 points use a 32x32 sprite texture.
- Lights: the sun is light 0; per-object point lights 6-9 (picked by range
  against the object's bounding sphere, 0x0027F450); presets switch
  between the sun and lights 33-35 (episode 2's creatures: sun off, 33 and
  34 on). The device keeps the enabled ones in a list at +0x398.
- Episode 2 water uses bump environment mapping (V8U8 textures, texture
  modes BUMPENVMAP and BUMPENVMAP_LUM).

## Compared with JSRF (player notes)

- Not cel-shaded: no inverted-hull outlines to get right.
- Runs well on xemu: use it for reference screenshots and audio.

