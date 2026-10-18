# Current state

Repository set up from Concept of Love (JSRF): launcher, game-file picker,
XISO reader, XBE identification, mixer, renderer (built; draws not wired
yet), vertex-program runner (not built yet), crash report, xboxrecomp
patches 0001-0008 plus 0009-0019 (Orta needed them). See
`docs/orta-facts.md` for facts about the game.

**Boot (cloud, with the files the player uploaded):** the SEGA, Smilebit
and Sofdec splash screens and the title screen ("PLEASE PRESS START TO
BEGIN") draw, ~15 fps on lavapipe. Wrong so far: text boxes have grey
backgrounds, the title logo looks incomplete. The attract movie
(Sofdec) plays with sound and returns to the title. **Windows (player's
PC, full disc):** builds and runs from PowerShell; logos, intro movie and
title loop with no hang.
- **Input** (`src/guest/input_hle.c`, from Concept of Love): XAPI's
  XGetDevices/XInputOpen/GetState/SetState/Poll/GetCapabilities replaced,
  fed from SDL gamepads and keyboard (Enter is Start, Space is A, arrows
  the d-pad). Start on the title opens the main menu.
  `PDO_INPUT_SCRIPT=frame:button:frames,...` presses buttons headless.
  NEW GAME -> difficulty menu -> the prologue text (cloud). Menu graphics
  stay drawn over later screens (not cleared).
  `tools/recomp.py --trace-functions all` emits entry hooks for bring-up. The title
makes its HDD cache folders (`Partition5`) on first boot, for the folders
on the disc: after adding files to a partial copy, delete `saves/Cache`.
Files the boot reads beyond the main menu set: `st11\`, `messageevent\`,
`Movie\tgs_demover.sfd`.

- **Most geometry bypasses D3D's draw calls:** the game's renderer object
  writes vertices inline into the push buffer (Begin 0x00279D30, GetSpace
  0x00279E10, End 0x00279E50). `d3d_hle.c` wraps Begin and End and draws
  the inline vertices like DrawVerticesUP. `PDO_D3D_TRACE=first-last`
  logs every D3D call (hooks from `tools/gen_d3d_calltrace.py`, names from
  a local XbSymbolDatabase run).
- D3D (`src/guest/d3d_hle.c`): all of Concept of Love's replacements at
  Orta's addresses (vblank clock, device, clear, swap, draws, immediate
  mode, textures, render targets, lighting, vertex programs on the CPU,
  pixel shaders, gamma). Not seen drawing yet: the cloud copy stops at the
  missing main menu sprite. 4928 differences: render-state indices
  (deferred +10, complex +19, see the enum), back buffers in the device at
  +0x21C0 (GetBackBuffer2 returns one), vertex constants read at draw time
  from the library's shadow at 0x002BEB08. Lavapipe works in the cloud
  (`mesa-vulkan-drivers`).
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
  v0.13.1 (193e299) + 14 patches (0001 includes the case resolver's "dir/" fix; 0009 `mov`
  from debug registers, 0010 `--exclude-sections`, 0011 `lahf` declares
  CF, 0012 stmxcsr/ldmxcsr as guest state, 0013 file rename, 0014 missing
  parent is PATH_NOT_FOUND, 0015 thread objects from
  ObReferenceObjectByHandle, 0016 Windows: a partition's root is a
  directory, 0017-0018 Windows directory create/listing (logged; listing
  path kept from the open; zero-length pattern is "*"), 0019 no
  immediate-reference function start where code falls through: send
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

1. Fix what the title screen shows wrong (blending of the text boxes, the
   logo), check the attract movie plays (Sofdec, `PDO_PEEK` helps) and
   music plays (PDO_AUDIO_DUMP or PDO_AUDIO_STATS).
2. Then Concept of Love's order: XInput, vertex programs, pixel shaders,
   culling, render targets, FF lighting, stencil, fog. Debug switches (F11
   thread dump, scripted input) with the first of them.
3. What PSGSFD* (probably Sofdec) and WMADEC are used for: FMVs and music
   need a plan.
