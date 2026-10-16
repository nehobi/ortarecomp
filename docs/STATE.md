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

## Next

1. **Match against JSRF** (needs the player's JSRF `default.xbe`):
   `tools/xbe_match.py` with `--col` (and `--jsrf-listing` if available).
   Tells which XDK/CRI functions are byte-identical despite 4134 vs 4928,
   which of Concept of Love's overrides carry over, and the Orta addresses
   of the tables they use. Then `recomp.py --names build/match/names.json`.
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
