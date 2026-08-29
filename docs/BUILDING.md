# Building Retro Burner

Retro Burner is one source repository with two native desktop targets.

## Windows x64

Requirements:

- Windows 10/11.
- Visual Studio 2022 with Desktop development with C++.
- CMake 3.24+.
- PowerShell.
- Git.
- MSYS2/MinGW as required by native RetroBeam/helper build steps.

Release build:

```powershell
.\build-windows.ps1 -Configuration Release
```

Clean Release build:

```powershell
.\build-windows.ps1 -Configuration Release -Clean
```

Windows is native MSVC/Win32/Direct3D 11 with Windows SPTI optical access. Runtime helpers/assets are embedded into the finished EXE.

Output:

```text
build/msvc-x64/Release/RetroBurner.exe
```

## Linux x86-64

Generic requirements include:

- CMake 3.24+ and Ninja.
- GCC or Clang with C++20.
- SDL3 and SDL3_image development files.
- OpenGL development files.
- Python 3.
- Git, make, autoconf/automake.
- libcurl and zlib development files.
- `growisofs` and `dvd+rw-mediainfo` for the selectable DVD backend.

Release build:

```bash
./build-linux.sh Release
```

Clean Release build:

```bash
./build-linux.sh Release --clean
```

Build without installing/refreshing the desktop launcher:

```bash
./build-linux.sh Release --clean --no-desktop
```

The Linux build creates native CDIrip, RetroBeam and pinned ABGX360 helpers, embeds them plus the application assets into the final Retro Burner ELF, then removes development sidecars from the release `bin` directory.

Output:

```text
build/linux/app-release/bin/RetroBurner
```

The final release `bin` directory is intentionally single-file.

A normal build also installs/refreshes:

```text
~/.local/bin/retroburner-current
~/.local/share/applications/io.github.L10N37.RetroBurner.desktop
```

The wrapper keeps the desktop entry safe even when the repository path contains spaces.

## UI simulation / parity testing

These modes synthesize burn/drive state. They do not start an optical backend or issue a disc WRITE command.

Windows:

```powershell
.\scripts\ui-sim-windows.ps1 -Scenario all
```

Linux:

```bash
./scripts/ui-sim-linux.sh all
```

Scenarios:

```text
dreamcast
ps1
ps2cd
ps2cd-verify
ps2dvd-retrobeam
ps2dvd-growisofs
saturn
xgd2-retrobeam
xgd2-growisofs
xgd3-retrobeam
xgd3-growisofs
ps2cd-failure
```

## CUE parser regression

```bash
python3 scripts/test-retrobeam-cuecheck.py <path-to-retrobeam>
```

This uses the same CDRWIN CUE parser as recording without opening a writer.

## Release packages

Windows:

```powershell
.\package-release.bat
```

Produces:

```text
dist/RetroBurner-0.5.0-windows-x64.zip
dist/RetroBurner-0.5.0-windows-x64.zip.sha256
```

Linux:

```bash
./package-release-linux.sh
```

Produces:

```text
dist/RetroBurner-0.5.0-linux-x86_64.tar.gz
dist/RetroBurner-0.5.0-linux-x86_64.tar.gz.sha256
```

Each archive contains the application plus README/release/licence documentation.

## Shared architecture

Shared interfaces/concepts include:

- `src/burn_engine.h`
- `src/drive_manager.h`
- `src/ui_burn_simulation.h`
- `src/ps2_media_probe.h`
- `src/retrobeam_failure.h`
- `src/retrobeam_progress.*`
- Dear ImGui
- RetroBeam/cdrtools source under `external/schilytools`
- RetroBeam CMake integration under `cmake/retroburner-optical`

Windows-native implementation includes `src/main.cpp`, `src/burn_engine.cpp`, `src/drive_manager.cpp`, `src/texture_loader.cpp`, `src/embedded_tools.cpp` and Win32/D3D11 ImGui backends.

Linux-native implementation includes `src/main_linux.cpp`, `src/burn_engine_linux.cpp`, `src/drive_manager_linux.cpp`, `src/texture_loader_linux.cpp`, `src/process_runner_linux.cpp`, `src/retrobeam_linux.cpp`, `src/optical_verify_linux.cpp` and SDL3/OpenGL ImGui backends.

## UI source-of-truth rule

`src/main.cpp :: DrawApp()` is canonical.

`scripts/internal/sync-linux-ui.py` generates:

```text
src/draw_app_linux.generated.inl
```

with only required native platform substitutions. `build-linux.sh` runs the synchronization before compiling. Do not hand-edit the generated Linux DrawApp include.
