# Playing, settings, and troubleshooting

## Choose your game data

If you do not own a copy, you can try the freely available [shareware distribution on Internet Archive](https://archive.org/details/Nitemare-3D). To purchase the full trilogy, visit [Gray Design Associates' Nitemare-3D page](https://www.dgray.com/n3dpage.htm), whose download includes Windows and DOS versions.

**The linked shareware is the original DOS game and is not compatible with this native rewrite.** Use it to try the original game; use the supported registered Windows data to run `Nitemare3D.exe` below.

For this native build, use the complete legally owned **registered Windows** game folder, not the DOS release, an installer, or a folder assembled from multiple versions. The loader checks exact supported file identities; renamed or modified substitutes are rejected.

Launch `Nitemare3D.exe`. If no data location is available, choose the purchased folder in the dialog. To select it explicitly:

```powershell
.\Nitemare3D.exe --data "D:\OwnedGame\N3W"
```

Selection order is:

1. The `--data` argument.
2. A `data` folder beside `Nitemare3D.exe`.
3. The saved data path.
4. A folder picker.

An invalid explicit or bundled path stops with a diagnostic rather than silently trying another folder. A stale saved path is explained before the picker opens. The native game reads your purchased folder without modifying it and does not execute the original game or DLLs.

## Controls

| Action | Control |
| --- | --- |
| Move forward/back | Up/Down |
| Turn | Left/Right |
| Fire | Ctrl |
| Use/open | Space |
| Select an owned weapon | 1–4 |
| Save/load | F4/F5 |
| Open Instructions | F1 |
| Browse Instructions | Up/PageUp, Down/PageDown; Esc returns |
| Toggle music/effects | F2/F3 |
| Navigate menus | Arrows, Enter, Esc |
| Toggle borderless fullscreen | Alt+Enter |

New games start unarmed. In E1L1, pick up the blue gun near the fireplace before expecting a held weapon or firing. A picked-up weapon takes several updates to raise.

### Optional modern controls

```powershell
.\Nitemare3D.exe --data "D:\OwnedGame\N3W" --modern
```

This adds W/S for forward/back, A/D for turning, and relative mouse input. These map to the existing game actions; A/D are not a new strafing mechanic. Original controls remain available.

### Display

The default presentation is nearest-neighbor scaling with a 4:3 letterbox. Add `--square-pixels` for square-pixel presentation. Resize the window or use Alt+Enter for borderless fullscreen.

## Saves and settings

Writable state defaults to `%LOCALAPPDATA%\Nitemare3D`. Saves are stored under its `saves` subdirectory. Use a separate location for a portable or isolated profile:

```powershell
.\Nitemare3D.exe --data "D:\OwnedGame\N3W" --user-dir "D:\My Games\Nitemare3D"
```

The location must be writable and outside the purchased-data folder. Separate profiles use separate settings and saves.

F4 opens Save and F5 opens Load during live gameplay. Native saves use version 2. Version 1 is explicitly rejected without migration, and native saves are not interchangeable with the original game's `USER.SAV`. Keep a backup before moving saves between builds; corrupt or incompatible files report an error instead of replacing live game state.

Configure → Hardware → Save persists hardware/audio configuration. Exiting that menu another way does not save it. Cheats → Done also writes configuration. F2/F3 toggle music/effects immediately; use Hardware Save to retain those enable flags.

## Map browser

```powershell
.\Nitemare3D.exe --data "D:\OwnedGame\N3W" --maps
```

- **Up/PageUp:** next stored level
- **Down/PageDown:** previous stored level
- **Home/End:** first/last stored level
- **Episode menu:** select an episode
- **W/O/F:** toggle walls, objects, and floors
- **Q/Esc:** close the browser

E1L11 is stored for demo playback, not a campaign level. The browser is read-only and does not change map data or a campaign. Keep the purchased Windows `maps.hlp` file available for Maps Help.

## Troubleshooting

### Data error or wrong release

The diagnostic names the file, byte offset, and failed check. Verify that you selected the complete supported Windows release. Do not mix DOS and Windows files, edit the supplied data, or bypass hash validation. An unsupported edition needs explicit code support; the loader does not guess a compatible layout.

### No weapon visible

A fresh game starts unarmed. Pick up a weapon first, allow its raise animation to finish, and select only a weapon you own. No sprite at startup is not by itself a rendering failure.

### Audio warning or silence

Effects use Windows-inbox XAudio2; music requires an available Windows General MIDI mapper. Check Windows output-device settings and the game's music/effects toggles. Real device errors display a warning without changing simulation. Some original effect slots are intentionally silent.

### Saves or settings cannot be written

Choose a writable `--user-dir` outside the purchased folder. Installing the executable in a read-only location is compatible with storing user state elsewhere. Do not overwrite an old save to work around a version or data-identity error.

### Compatibility expectations

Windows 11 x64 is the supported target. Full original-versus-native trilogy parity and independent clean-machine deployment are not yet verified; a successful launch does not establish every campaign route's fidelity.
