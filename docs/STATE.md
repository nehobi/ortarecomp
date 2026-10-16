# Current state

Repository set up from Concept of Love (JSRF): launcher, game-file picker,
XISO reader, XBE identification, mixer, renderer and vertex-program runner
(the last two not built yet), crash report, xboxrecomp patches 0001-0008
plus 0009-0011 (Orta needed them). First recompile builds; see
`docs/orta-facts.md`.

## Facts

- Input: Panzer Dragoon Orta (USA) `default.xbe`, 53450007, **XDK 4928**
  (JSRF: 4134). One dump known.
- `tools/recomp.py` (~4 min): 16,643 functions, ~1.78M lines of C in 67
  files; APPDATA excluded as data. xboxrecomp v0.13.1 (193e299) + 11
  patches (0009 `mov` from debug registers, 0010 `--exclude-sections`,
  0011 `lahf` declares CF: send upstream), SDL 3.4.16.

## Shared with JSRF (`tools/xbe_match.py`, JSRF USA dump)

- 2,206 of 16,628 Orta functions matched (1,919 byte-identical after
  address masking). By section: XPP 152/187, DSOUND 307/518, D3D 151/287,
  MMATRIX 37/66, XGRPH 16/24, .text 1,542/15,359 (CRT, CRI, maybe more).
  WMADEC and PSGSFD (Sofdec?) have no JSRF counterpart.
- 115 of Concept of Love's 167 overrides/hooks have an Orta counterpart.
  CRI is shared: both `cri_hle.c` overrides match exactly, and so does the
  data address they use. D3D: 3 of the 4 tables `d3d_hle.c` uses are paired
  (one by 33 identical uses). XInput: only XGetDevices exact and
  XInputPoll similar; the rest changed in 4928.
- "callgraph" pairs (position in a matched caller) are the weakest; several
  D3D ones (Swap, SetPixelShader, CopyRects) have low similarity because
  4928 rewrote them. Check each when porting its override. Whole library
  objects are linked in another order than in 4134.
- JSRF's four runtime seeds have no Orta match (game code).

## Next

1. Port overrides in Concept of Love's order, starting with those matched
   exactly; re-derive render-state indices from Orta's setters.
2. Identify what JSRF cannot name: Cxbx-Reloaded's XbSymbolDatabase from a
   local clone (has 4928 signatures), or the D3D setters by structure.
3. Boot to the kernel (`--boot`), collect `[ICALL] Failed to resolve VA`
   lines into `tools/pdo_seeds.json` as `"observed": true`.
4. Replace in Concept of Love's order: vblank/Swap -> device, clear, draws
   -> textures -> DirectSound -> XInput -> vertex programs -> pixel shaders
   -> culling -> render targets -> FF lighting -> stencil -> fog. Build the
   debug switches (F12 capture, F11 thread dump, scripted input, periodic
   screenshots) with the first of them.
5. Find out what PSGSFD* (probably Sofdec) and WMADEC are used for: FMVs
   and music need a plan.
