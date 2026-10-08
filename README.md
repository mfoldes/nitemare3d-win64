# Nitemare 3D for Windows 11

*An Unofficial native Windows 11 x64 rewrite of Nitemare 3D.*

A native Windows 11 x64 rewrite of the Nitemare 3D trilogy, with original visuals and optional modern controls. **No game assets are included.** Running this rewrite requires the supported registered Windows release; no emulator or legacy game DLL is needed.

The game includes native saves, audio, and a read-only map browser. Full original-versus-native trilogy parity and clean-machine deployment are not yet verified.

## Get the game

If you do not own a copy, you can try the freely available [shareware distribution on Internet Archive](https://archive.org/details/Nitemare-3D). To purchase the full trilogy, visit [Gray Design Associates' Nitemare-3D page](https://www.dgray.com/n3dpage.htm).

**Compatibility:** the linked shareware is the original DOS release, not a data set supported by this native rewrite. For this build, use the registered Windows files included with the purchased trilogy.

## Build

Install Visual Studio 2022 Community or Build Tools with **Desktop development with C++**, MSVC v143, Windows SDK **10.0.26100.0**, and CMake 3.25 or newer. Select the SDK under Visual Studio Installer → Individual components. Open Developer PowerShell for VS 2022 and run from the repository root:

```powershell
cmake --preset windows-x64
cmake --build .\out\build\windows-x64 --config Release
ctest --test-dir .\out\build\windows-x64 -C Release --output-on-failure
```

The executable is `out\build\windows-x64\bin\Release\Nitemare3D.exe`. Building requires no game data, package manager, or network download.

## Run

Launch `Nitemare3D.exe` and select your purchased Windows game folder when prompted, or specify it:

```powershell
.\out\build\windows-x64\bin\Release\Nitemare3D.exe --data "D:\OwnedGame\N3W"
```

Only the exact supported Windows data set is accepted. DOS files, mixed releases, missing files, and different file hashes are rejected. The native game never modifies the purchased folder or loads the original executable/DLLs as code.

Arrow keys move/turn, Ctrl fires, Space uses/opens, and F4/F5 saves/loads. Add `--modern` for WASD and relative mouse input, or `--maps` for the read-only map browser. New games start unarmed.

Settings and saves default to `%LOCALAPPDATA%\Nitemare3D`; use `--user-dir` for a separate writable profile outside your purchased-data folder.

## Guides

- [Building and distribution](docs/BUILDING.md): prerequisites, Release/Debug builds, external build folders, and portable distribution.
- [Playing and settings](docs/PLAYING.md): controls, data selection, saves, audio, map browser, and troubleshooting.

The original Nitemare-3D was made by David Grey. © [Gray Design Associates](https://www.dgray.com/index.html)