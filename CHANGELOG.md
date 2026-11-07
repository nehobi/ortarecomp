# Changelog

## v0.1.0 (beta)

The first release. Changes are listed against the original Xbox game.

### Added

- Native PC port: the Xbox game statically recompiled, running without an
  emulator.
- Windows and Linux builds; the Linux one runs on the Steam Deck.
- Launcher: pick your game files (disc image or extracted folder), save
  file and settings before starting.
- Three save files (the original keeps one).
- Resolution scale, 1x to 4x.
- Anti-aliasing: MSAA up to 8x, or FXAA.
- Frame interpolation above 60 fps (beta).
- Aspect ratio 4:3 or 16:9 (the game's own widescreen mode), with bars,
  stretch or zoom to fit the window.
- Fullscreen (Alt+Enter) and a resizable window.
- Picture settings: CRT look, brightness, contrast, gamma, saturation, and
  optional TV softening.
- 3D audio for headphones, and a master volume.
- Keyboard and mouse controls, with mouse aim; controller support; every
  button can be rebound.
- Options menu in game (F1, or Back + Start on a controller).
- Pandora's Box option to open every item, without touching the save.
- Skip the boot logos with a press.
- FPS counter.
- Portable mode (a blank portable.txt next to the executable).
- A log of every run, and a crash report in it, to attach to bug reports.

### Changed

- Graphics, sound and input are new PC code in place of the Xbox's.
- Settings are kept in ortarecomp.ini, saves in the data folder.
