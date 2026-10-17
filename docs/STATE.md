# Current state

Repository set up from Concept of Love (JSRF): launcher, game-file picker,
XISO reader, XBE identification, mixer, renderer (built; draws not wired
yet), vertex-program runner (not built yet), crash report, xboxrecomp
patches 0001-0008 plus 0009-0012 (Orta needed them). See
`docs/orta-facts.md` for facts about the game.

**Boot (cloud, with the boot files the player uploaded):** the title
initialises D3D and input, reads its fonts, sprites, dragon textures and
shaders, then its init (0x00011820) fails because DirectSoundCreate fails
(no audio hardware) and it returns to the dashboard (`HalReturnToFirmware`).
Copies into the HDD cache (`Partition5\...\*.tmp`) fail: non-fatal, as in
JSRF.

D3D stage 1 is in `src/guest/d3d_hle.c`: vblank clock, CreateDevice, Clear,
Swap, BlockOnTime. The cloud container has no Vulkan driver yet, so nothing
is presented here (`gfx: no GPU device`); lavapipe is needed.

## Facts

- Input: Panzer Dragoon Orta (USA) `default.xbe`, 53450007, **XDK 4928**
  (JSRF: 4134). One dump known.
- `tools/recomp.py` (~6 min): 16,959 functions, ~1.81M lines of C in 68
  files; APPDATA excluded as data; every direct call target in code is
  seeded (second disassembly pass). xboxrecomp v0.13.1 (193e299) + 12
  patches (0009 `mov` from debug registers, 0010 `--exclude-sections`,
  0011 `lahf` declares CF, 0012 stmxcsr/ldmxcsr as guest state: send
  upstream), SDL 3.4.16. 11 observed seeds in `tools/pdo_seeds.json`.
- **Names:** Cxbx-Reloaded's XbSymbolDatabase (MIT) CLI, built from a local
  clone, names 446 XDK symbols in Orta (functions, and D3D tables such as
  each render state's address). Facts only; nothing from it is committed.
  Every hand identification so far agreed with it.

## Shared with JSRF (`tools/xbe_match.py`, JSRF USA dump)

- 2,206 of 16,628 Orta functions matched (1,919 byte-identical after
  address masking). By section: XPP 152/187, DSOUND 307/518, D3D 151/287,
  MMATRIX 37/66, XGRPH 16/24, .text 1,542/15,359 (CRT, CRI, maybe more).
  WMADEC and PSGSFD (Sofdec?) have no JSRF counterpart.
- 115 of Concept of Love's 167 overrides/hooks have an Orta counterpart.
  CRI is shared: both `cri_hle.c` overrides match exactly, and so does the
  data address they use. XInput mostly changed in 4928.
- "callgraph" pairs are the weakest; several D3D ones have low similarity
  because 4928 rewrote them. Check each when porting its override.

## Next

1. **DirectSound** (`dsound_hle.c`): Orta's game calls 78 DSOUND entry
   points. Those JSRF also used map one to one (same `ret N`). New in Orta:
   DirectSoundCreateStream and IDirectSoundStream (COM objects the game
   calls through their vtable, so our objects need vtables routed to host
   code through recomp_lookup_manual), Pause, SetLFO/SetEG/SetFilter,
   GetSpeakerConfig.
2. Lavapipe in the cloud container, so frames can be captured here.
3. D3D draws, textures, shaders: port the rest of `d3d_hle.c` with
   XbSymbolDatabase names; render-state indices from its D3DRS_* addresses.
4. Then Concept of Love's order: XInput, vertex programs, pixel shaders,
   culling, render targets, FF lighting, stencil, fog. Debug switches (F11
   thread dump, scripted input) with the first of them.
5. What PSGSFD* (probably Sofdec) and WMADEC are used for: FMVs and music
   need a plan.
