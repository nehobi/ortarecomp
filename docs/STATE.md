# Current state

Repository set up from Concept of Love (JSRF): launcher, game-file picker,
XISO reader, XBE identification, mixer, renderer (built; draws not wired
yet), vertex-program runner (not built yet), crash report, xboxrecomp
patches 0001-0008 plus 0009-0012 (Orta needed them). See
`docs/orta-facts.md` for facts about the game.

**Boot (cloud, with the boot files the player uploaded):** the title gets
through D3D, input and sound init and runs its main loop (~39 fps on
lavapipe, frames black: draws are not replaced yet). It then asks for
`Sprite\pdbox_main_menu.spr` and `st01\s01_bgm1.wav`, which the cloud
copy does not have. Copies into the HDD cache (`Partition5\...\*.tmp`)
fail: non-fatal, as in JSRF.

- D3D (`src/guest/d3d_hle.c`): vblank clock, CreateDevice, Clear, Swap,
  BlockOnTime. Lavapipe works in the cloud (`mesa-vulkan-drivers`).
- DirectSound (`src/guest/dsound_hle.c`): Concept of Love's 50 entry
  points at Orta's addresses, plus GetSpeakerConfig, SetMixBinVolumes,
  buffer Pause and streams (DirectSoundCreateStream, the
  CDirectSoundStream methods, all IDirectSoundStream setters). Streams
  play packets back to back on a mixer queue (`pdo_voice_queue`); Orta
  streams WMA-decoded music (2 packets, no callback). Packet callbacks and
  completion events are not wired: the log says so if the game uses them.

## Facts

- Input: Panzer Dragoon Orta (USA) `default.xbe`, 53450007, **XDK 4928**
  (JSRF: 4134). One dump known.
- `tools/recomp.py` (~6 min): 17,125 functions, ~1.82M lines of C in 69
  files; APPDATA excluded as data; a second disassembly pass seeds every
  direct call target in code and every data-table pointer that lands in a
  function right after a ret or padding (318 functions). xboxrecomp
  v0.13.1 (193e299) + 12 patches (0009 `mov` from debug registers, 0010 `--exclude-sections`,
  0011 `lahf` declares CF, 0012 stmxcsr/ldmxcsr as guest state: send
  upstream), SDL 3.4.16. 14 observed seeds in `tools/pdo_seeds.json`.
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

1. More boot files from the player (main menu sprites, stage 1 music),
   then check music plays (PDO_AUDIO_DUMP or the mixer stats).
2. D3D draws, textures, shaders: port the rest of `d3d_hle.c` with
   XbSymbolDatabase names; render-state indices from its D3DRS_* addresses.
3. Then Concept of Love's order: XInput, vertex programs, pixel shaders,
   culling, render targets, FF lighting, stencil, fog. Debug switches (F11
   thread dump, scripted input) with the first of them.
4. What PSGSFD* (probably Sofdec) and WMADEC are used for: FMVs and music
   need a plan.
