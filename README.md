# Orta Recomp

A native port of **Panzer Dragoon Orta** (Xbox, 2002), made by statically
recompiling the original game with [xboxrecomp](https://github.com/sp00nznet/xboxrecomp).
PC first (Windows, Linux, Steam Deck); Android is planned.

Unofficial, and not affiliated with the rights holders. **No game data is
included.** You need your own copy of the game to build or play this.

## Status

Early development. **Not playable yet.**

1. The exe builds from your own `default.xbe`, and the game-file picker works
2. Boots to the first frame
3. Title screen and menus, with audio
4. First episode playable
5. Full playthrough, polish, then Android

## Game files

You need **Panzer Dragoon Orta (USA)**. On first launch the app asks for
either:

- a disc image (`.iso` / `.xiso`, trimmed or full dump), which it copies into
  its data folder, or
- a folder that already holds the extracted game (`default.xbe` and every
  other file from the disc).

The app checks `default.xbe` before using anything. Other regions are not
supported yet.

You can also put the disc image (or a `game` folder with the extracted files)
next to the executable before the first launch; it is found and imported
automatically. Put a blank `portable.txt` next to the executable to keep
settings, saves and imported game files beside it.

## Building

See [BUILDING.md](BUILDING.md).

## Credits

- [xboxrecomp](https://github.com/sp00nznet/xboxrecomp) by sp00nz and
  contributors: the recompiler and the Xbox kernel layer this port runs on
- [SDL](https://libsdl.org): windowing, file dialogs, rendering, audio, input
- [Concept of Love](https://github.com/wootbeer/conceptoflove), the Jet Set
  Radio Future port this one grew out of

## License

The project's own code is GPL-3.0; see [COPYING](COPYING). Third-party
licenses are in [licenses/](licenses). The game itself remains the property
of its rights holders.
