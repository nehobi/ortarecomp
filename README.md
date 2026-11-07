# Orta Recomp: Panzer Dragoon Orta (2002 Xbox) Recompiled and Port

You will need the game files from a licensed copy in order to play it.

A native PC port of Panzer Dragoon Orta, made by statically recompiling the
Xbox game with [xboxrecomp](https://github.com/sp00nznet/xboxrecomp), with
graphics, audio and input of its own on [SDL 3](https://libsdl.org).

Unofficial, and not affiliated with the rights holders. **No game data is
included.** You need your own copy of the game to play this.

**Beta.** The whole game plays through, but expect issues, mostly with frame
interpolation (smooth motion above 60 fps). If something looks wrong, turn
Frame interp. off in F1 > Advanced and see if it goes away, then report it.

Android build is planned.

## Game Files

You need Panzer Dragoon Orta (USA), either:

- a disc image (`.iso` / `.xiso`), or
- a folder with the extracted game (`default.xbe` and the rest of the disc).

default.xbe SHA-256:
c1af4eafccc02e11105d606b177263216cb7aa71bf1fbe8486ddfc3b1b848d2c

On first launch, choose Game files in the launcher and pick your disc image
or folder. A disc image is copied into the app's data folder.

Add a blank portable.txt in the application's folder to make portable if you
want to save settings, saves and game files locally.

## Playing

- F1, or Back + Start on a controller: the options menu (resolution,
  anti-aliasing, aspect ratio, picture, sound, controls and more).
- Alt+Enter: fullscreen.
- Save file 1, 2 or 3 is picked in the launcher before starting.
- Needs a 64-bit Windows 10/11 PC with a Vulkan graphics driver.

## Reporting Issues

Every run writes `ortarecomp.log` to the data folder
(`%APPDATA%\OrtaRecomp\OrtaRecomp`, or the app's folder when portable); the
run before is kept as `ortarecomp-previous.log`. After a crash or a freeze,
attach that log (the previous one if you have started the game again) to
your report in the GitHub Issues section, with what you were doing.

## Building

See [BUILDING.md](BUILDING.md).

## Issues and Limitations

Known:
- Beta status. Tested on Windows.
- Frame interpolation can show odd frames in some scenes.
- The first time an effect appears it can stutter once while its shaders are
  built; they are kept for next time.
- USA version only.

Else:
- Please see the "Issues" section in GitHub.

## Credits

- [xboxrecomp](https://github.com/sp00nznet/xboxrecomp) by sp00nz and
  contributors: the recompiler and the Xbox kernel layer this port runs on
- [SDL](https://libsdl.org): windowing, graphics, audio and input
- [Concept of Love](https://github.com/wootbeer/conceptoflove), the Jet Set
  Radio Future port this one grew out of

## License

The project's own code is GPL-3.0; see [COPYING](COPYING). Third-party
licenses are in [licenses/](licenses). The game itself remains the property
of its rights holders.
