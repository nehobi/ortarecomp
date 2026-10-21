# Current state

Panzer Dragoon Orta plays through episodes 1 and 2 on Windows (player's
PC, full disc), with music, movies, saves and both bosses. Bring-up notes
are in `docs/archive/state-bring-up.md`; facts about the game in
`docs/orta-facts.md`.

## Working

- Boot, menus, movies (Sofdec), new game, episode 1 (stage, canyon, boss)
  and episode 2 (stage, boss), saving.
- Music: stage, boss and cutscene tracks; the episode 1 canyon is silent
  by design.
- Graphics: fixed-function and pixel-shader draws, vertex programs on the
  CPU, render targets and frame copies, bump environment mapping (episode 2
  water), texture transforms, point sprites, PROJECT2D.
- xboxrecomp v0.13.1 + patches 0001-0022: 0020-0021 give threads the stack
  they ask for (an exhausted pool deadlocked the cutscene before the
  episode 1 boss); 0022 stops an ID that looks like an address from
  splitting a function (it dropped the boss music).

## To check with the player (built, untested on Windows)

- Episode 1: waterfalls animate, rain finer, wing-tip trails (point
  sprites), sparks.
- Episode 2: camouflaged creatures no longer black (PROJECT2D divide),
  dust.

## Open

1. Episode 2: tentacles solid green (should be black with green accents);
   the boss in the next area not visible; some enemies white (a missing
   texture in a fixed-function draw gives white).
2. Sound effects may be too loud against the music.
3. Lifter: ~120 call targets in generated code are unresolved stubs; audit
   them. `_fixup_icall_esp_save` can take argument pushes of callee-saved
   registers for saves, so a failed ICALL leaks its arguments.
4. Later: graphics options (resolution scale, anti-aliasing, aspect ratio,
   overlay).

## Testing

- Cloud: `PDO_NO_DRAW=1 PDO_FAST=4`, `PDO_SCREENSHOT=dir
  PDO_SCREENSHOT_EVERY=n`, `PDO_CAPTURE=frame` (every draw of one frame
  with its state and textures), `PDO_INPUT_SCRIPT` for buttons. The
  standard script reaches episode 1 by frame ~11000.
- Windows: build from a Developer PowerShell (or enter the VS dev shell
  first), else the build fails and a stale exe runs; check the exe's time.
  F12 writes a capture.
