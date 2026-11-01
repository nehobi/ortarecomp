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
2. Sound balance: matches xemu (episode 1 flying, in-game options at
   defaults: -11.4 LUFS in both). The loud effects were in-game options
   turned down. The game sets music -26.0 dB, most effects -22.5 dB.
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
   in open stages. Episode 1 trace: the game leaves source HF levels,
   obstruction and occlusion at no effect and uses no envelopes, LFOs or
   filters. It does turn HRTF on (Sensaura, via DirectSound): for phones
   with headphones later, as "3D audio: Off / Headphones" -- first see
   whether the XBE's own HRTF tables are usable at run time.
5. Hang skipping the cutscenes after episode 1 (Start): fix to confirm.
   The wait tracer caught it: the main thread, stopping a worker
   (0x00225B80, worker routine 0x002266B0), asked "am I that worker?" and
   got yes, then waited on an event only the worker's own path sees set.
   Every thread shared one fake thread object, so GetCurrentThreadId was
   the same everywhere, and PsCreateSystemThreadEx never wrote ThreadId.
   Patch 0025 gives each thread its own object and id. It hung again
   later in the stream destructor (0x0029FB40: SetEvent on the worker's
   stop event, ResumeThread, wait): the worker never woke. Likely cause,
   fixed by patch 0026: the kernel's handle table was claimed without
   locking, so two threads could share a token. Workers asked for
   suspended are now created suspended (0021, 0029; CRI's 8 workers).
6. Title red bars: a shadow map's memory reused for a colour texture was
   still taken as the depth target (b86af73; not seen since). Unbound
   stages read white (menu highlights need it; black turned them black).

## Performance

Vertex programs run on the CPU (src/guest/vsh.c): compiled once per
program and run four vertices at a time with SSE2, bit-identical to the
reference interpreter (PDO_VSH_CHECK=1 runs both and reports any
difference; none through episode 1). About 5x the old interpreter; the
episode 1 boss went from 42 to 55 fps average on a 4-core test machine.
Player's PC in play (timing lines, ms/s), after the texture hash fix, at
up to 4x with MSAA: draws 333 (fixed-function 165, vertex programs 122),
renderer 216 (textures 47), game code 258; 57 fps average.
GPU vertex programs (overlay: "Vertex progs", ini gpu_vertex; on by
default, confirmed in play on Windows): shaders/vsh.vert runs the compiled program on the
game's raw stream bytes; same pixels as the CPU in an episode 1 capture
(PDO_CAPTURE_CPUVSH=1 draws both); vertex programs 91-196 -> 5-10 ms/s
here. GPU fixed-function vertices (overlay: "Fixed-func", ini gpu_fixed; on
by default, confirmed in play): shaders/fvf.vert does fetch_fvf
and light_vertex from the raw bytes and per-draw uniforms; an episode 1
capture differs from the CPU in 7 pixels by 1 level; fixed-function
300 -> 5 ms/s here. CPU still: points, programs that write constants.
CPU: xboxrecomp's NV2A register thread spun a whole core (Sleep(0)); the
game thread spun in D3D CDevice::KickOff on its flush handshake until that
thread cleared the bit. KickOff is overridden without the wait and the
thread sleeps 1 ms (patch 0027): 445 -> 122 s CPU over a 330 s run here.
"Game code" in the timing log is wall time: perf on Linux (perf works
here, gprofng loses samples) shows part of it is our draw submission.
Linux only: win32_compat's WaitForMultipleObjects polls with sleeps
(three stream workers at ~58 ms/s each); matters for Android.
Threads: the game runs on its own thread; the main thread owns the
window and draws (renderer.c, frame_data pool; the game never waits on
drawing). All game threads share one recording frame (rec_F): kept per
thread, a second game thread recorded into a slot already drawn and its
texture uploads were lost (black dragon, rainbow sky; fixed, confirmed).
Above 60: "Frame interp" (F1, ini frame_interp) draws between the last two
game frames, one behind, timed after the window's image is acquired, over
the gap between their Swap vblanks. match_frames: 3D draws pair nearest
first by place (matrices or constants, motion guess for fixed-function),
data address only a tie-break -- the game reuses vertex buffers between
fighters (packs warped: fixed de4713b, player: looked good once, watch);
screen-space draws pair by address (menu words). Not blended: on-screen
jumps, draws whose vertices stray over half their size (glide wing
reticle; a fifth made episode 5's dragon wings flicker), all on a camera
cut. Blending holds off 5 s when the screen is under 1.5x the game rate.
Log: `main:` (fps, recording/waiting ms), `interp:` (cuts, close calls,
pairs undone). Debug: PDO_INTERP_TINT (3D only), PDO_CAPTURE_INTERP
(+_SEQ=n, PDO_DBG_GROUPS), PDO_CAPTURE_TEXCHECK, PDO_TEX_ALWAYS/LEVEL0.
Glide wing: script `13000:y:8,13100:y:8`. Display: aspect 4:3 or 16:9 (the
game's own anamorphic mode, src/guest/widescreen.c, read at start), fit
bars/stretch/zoom, Alt+Enter fullscreen. PDO_BENCHMARK=1
runs the game unpaced (measuring only). Player's PC, benchmark at 2x:
~190 fps in play (game thread bound); at 4x+4xAA the GPU caps ~60-80.
Goal: headroom on lesser PCs; phones later (Vulkan/Turnip). For that:
1. Vertex work on the GPU: done, both on by default (toggles kept). 2. Texture change tracking by writes instead
   of hashing every texture's mip chain each frame. 3. Measure the
   recompiled code and the guest CPU lock on ARM.

## Options overlay

Confirmed on Windows. F1, or Back+Start on a controller: resolution scale 1-4x, anti-aliasing
(MSAA up to what the GPU takes), 4:3 or stretch, TV softening, master
volume, FPS counter, vertex programs and fixed-function vertices on
CPU or GPU. The game waits in Swap and the sound holds while it
is open; settings go to ortarecomp.ini when it closes.

## Testing

- Cloud: `PDO_NO_DRAW=1 PDO_FAST=4`, `PDO_SCREENSHOT=dir
  PDO_SCREENSHOT_EVERY=n`, `PDO_CAPTURE=frame` (every draw of one frame
  with its state and textures), `PDO_INPUT_SCRIPT` for buttons. The
  standard script reaches episode 1 by frame ~11000.
- Windows: build from a Developer PowerShell (or enter the VS dev shell
  first), else the build fails and a stale exe runs; check the exe's time.
  F12 writes a capture.
