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

## Compared with JSRF (player notes)

- Not cel-shaded: no inverted-hull outlines to get right.
- Runs well on xemu: use it for reference screenshots and audio.
