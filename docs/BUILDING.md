# Building and distributing Nitemare 3D

## Requirements

The supported target is Windows 11 x64. Install Visual Studio 2022 Community or Build Tools with:

- **Desktop development with C++**
- **MSVC v143** x64/x86 build tools
- **Windows SDK 10.0.26100.0** (Visual Studio Installer → Individual components)
- **CMake 3.25 or newer**; Visual Studio's C++ CMake tools are suitable

Open **Developer PowerShell for VS 2022**. The commands below assume the repository is your current directory. The build needs neither purchased game data nor a package manager. CMake does not download dependencies.

## Release build

```powershell
cmake --preset windows-x64
cmake --build .\out\build\windows-x64 --config Release
```

The preset selects Visual Studio 2022, x64, and the specified Windows SDK. The result is:

```text
out\build\windows-x64\bin\Release\Nitemare3D.exe
```

The default build includes the native game, its static libraries, and `n3d_tests`. There are no separate verifier or research-tool targets.

## Debug build

Use the same configured build directory:

```powershell
cmake --build .\out\build\windows-x64 --config Debug
```

The executable is in `out\build\windows-x64\bin\Debug`. Use Release for distribution. Visual Studio is a multi-configuration generator: select the configuration with `--config`, not `CMAKE_BUILD_TYPE`.

## Regression tests

The full synthetic suite requires no purchased game data:

```powershell
ctest --test-dir .\out\build\windows-x64 -C Release --output-on-failure
```

CTest runs one aggregate `n3d_tests` target covering the data readers, rendering, simulation, menus, saves, input, and audio contracts. Test filesystem writes use temporary directories. To build only the game, add `--target Nitemare3D` to the build command; to omit tests entirely, configure with `-DBUILD_TESTING=OFF`.

## Building outside the repository

To keep generated files elsewhere, override the preset's build directory:

```powershell
cmake --preset windows-x64 -B "D:\Builds\Nitemare3D"
cmake --build "D:\Builds\Nitemare3D" --config Release
```

Its executable is `D:\Builds\Nitemare3D\bin\Release\Nitemare3D.exe`. Do not generate build output inside your purchased-data folder.

## Getting the game

You do not need a game copy to compile or run the synthetic tests. If you do not own a copy and want to try the original game, the [shareware distribution is freely available on Internet Archive](https://archive.org/details/Nitemare-3D). You can purchase the full trilogy through [Gray Design Associates' Nitemare-3D page](https://www.dgray.com/n3dpage.htm).

The linked shareware is the DOS release and cannot supply data for this native build. Running the compiled application requires the supported registered Windows data from the full trilogy; select the Windows files, not the bundled DOS version.

## Run the result

```powershell
.\out\build\windows-x64\bin\Release\Nitemare3D.exe --data "D:\OwnedGame\N3W"
```

Omit `--data` to use the folder picker when no bundled or saved data path is available. See [Playing and settings](PLAYING.md) for controls and data selection.

## Portable distribution

Create a clean folder containing:

```text
Nitemare3D.exe
README.md
docs/
  BUILDING.md
  PLAYING.md
```

Copy the **Release** executable. Keep `docs/` beside README so its links work. Compress that folder with Windows Explorer's ZIP option if desired. The C++ runtime is linked statically; no project DLLs, emulator, installer, or network service is required. Effects and music use Windows-inbox APIs.

**Do not include purchased game data.** Each player supplies their own supported Windows release. Also exclude build libraries, object files, debug symbols, private saves, and settings.

## Build troubleshooting

- **`cmake` is not recognized:** install the C++ CMake tools and reopen Developer PowerShell.
- **Visual Studio generator or compiler not found:** install the Desktop development with C++ workload and v143 tools; reopen the developer shell afterward.
- **Requested Windows SDK not found:** add SDK 10.0.26100.0 through Visual Studio Installer. The preset deliberately selects this version.
- **Executable not where expected:** check the selected configuration and build directory. Release and Debug have separate `bin` subdirectories.

## Source layout

- `include/n3d/`: shared types and interfaces
- `src/core/`: checked data readers and configuration
- `src/game/`: game state, simulation, demos, and saves
- `src/render/`: indexed software rendering and menus
- `src/platform/`: Win32 window, input, audio, filesystem, and map browser
- `tests/`: synthetic parser, state, rendering, and platform regressions

The runtime reads game assets once into owned memory. Rendering and simulation remain separate; Win32-specific code stays in the platform layer.
