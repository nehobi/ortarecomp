# Current state

Panzer Dragoon Orta plays through episodes 1 to 4 and into 5 on Windows
(player's PC, full disc), with music, movies, saves and bosses. Bring-up notes
are in `docs/archive/state-bring-up.md`; facts about the game in
`docs/orta-facts.md`.

## Working

- Boot, menus, movies (Sofdec), new game, episode 1 (stage, canyon, boss)
  and episode 2 (stage, boss), saving.
- Music: stage, boss and cutscene tracks; the episode 1 canyon is silent
  by design.
- Graphics: fixed-function and pixel-shader draws, vertex programs on the
  CPU, render targets and frame copies, bump environment mapping (episode 2
  water), texture transforms, point sprites, PROJECT2D, camera-space
  texture coordinate generation (episode 1 rain: finer now, confirmed;
  it comes and goes in the original too; the player's dragon is shiny
  again, confirmed).
- Shadow maps (look right on Windows so far): the depth-only pass into a
  512x512 LIN_D16 surface, compared in ff.frag (stored GREATER r/q, 2x2
  filtered); in episode 1 they shade the player's dragon.
- xboxrecomp v0.13.1 + patches 0001-0024: 0020-0021 give threads the stack
  they ask for (an exhausted pool deadlocked the cutscene before the
  episode 1 boss); 0022 stops an ID that looks like an address from
  splitting a function (it dropped the boss music); 0023 is the guest CPU
  lock's yield and cooperative suspend; 0024 gives every branch out of a
  lifted body an entry (254 jumps, mostly loop back-edges, went to stubs;
  42 remain, all from data decoded as code).

## To check with the player (built, untested on Windows)

- Episode 1: waterfalls, wing-tip trails and particles (point sprites)
  confirmed.
- Episode 2: dust moves (confirmed). Camouflaged creatures confirmed against a video (the
  gray warping is the original's look), dying enemies keep their colours
  (lights 33-35).

## Open

1. Episode 2: the second boss shows and can be beaten (confirmed; it
   updates through 0x00111BC0, a function behind a jump table that
   recomp.py now seeds). Tentacles confirmed right; some enemies white (a missing
   texture in a fixed-function draw gives white).
2. Sound effects feel loud against the music. The game sets them itself
   (music -26.0 dB, most effects -22.5 dB at equal option levels).
3. Episode 4 crash after event st04_ev81: fixed and confirmed (episode 4
   beaten, no [POOL] reports). A virtual function (0x0015EB10) cut short
   by a false start jumped to a stub instead of its own epilogue, and the
   render callback walk 0x00044A20 followed esi into heap objects.
   0x00044A20 stays in C and logs any callback that changes esi or esp.
   Lifter: 42 jumps still go to stubs, all from data decoded as code.
   `_fixup_icall_esp_save` can take argument pushes of callee-saved
   registers for saves, so a failed ICALL leaks its arguments.
4. Reverb: I3DL2 is ours (src/audio/reverb.c); sounds right in the
   episode 1 canyon on Windows. 3D voices send at their SetI3DL2Source
   room level; the game picks a room per scene (0x00012930 with entries
   1 Bathroom: episode 1 boss; 3 and 6 Stone Corridor) and turns it off
   in open stages. Not done: source HF levels, obstruction/occlusion.

## Options overlay

Confirmed on Windows. F1, or Back+Start on a controller: resolution scale 1-4x, anti-aliasing
(MSAA up to what the GPU takes), 4:3 or stretch, TV softening, master
volume, FPS counter. The game waits in Swap and the sound holds while it
is open; settings go to ortarecomp.ini when it closes.

## Testing

- Cloud: `PDO_NO_DRAW=1 PDO_FAST=4`, `PDO_SCREENSHOT=dir
  PDO_SCREENSHOT_EVERY=n`, `PDO_CAPTURE=frame` (every draw of one frame
  with its state and textures), `PDO_INPUT_SCRIPT` for buttons. The
  standard script reaches episode 1 by frame ~11000.
- Windows: build from a Developer PowerShell (or enter the VS dev shell
  first), else the build fails and a stale exe runs; check the exe's time.
  F12 writes a capture.
