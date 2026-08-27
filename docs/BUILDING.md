# Building Retro Burner

Retro Burner is one source repository with two native desktop targets.

## Public build entry points

These are the only build commands users/developers should need:

### Windows

```powershell
.\build-windows.ps1 -Configuration Release
```

Optional clean build:

```powershell
.\build-windows.ps1 -Configuration Release -Clean
```

The Windows build is native Windows:

- MSVC x64 application
- Win32 window/platform layer
- Direct3D 11 renderer
- Windows SPTI optical transport
- native Windows RetroBeam backend built/staged before the GUI

### Linux

```bash
./build-linux.sh Release
```

Optional clean build:

```bash
./build-linux.sh Release --clean
```

The Linux build is native Linux:

- GCC/Clang x86-64 application
- SDL3 window/platform layer
- OpenGL 3 renderer
- Linux SG_IO optical transport
- native Linux RetroBeam backend built from the same repository source
- SDL3_image for the repository artwork
- dvd+rw-tools/growisofs for the selectable DVD backend

## Source layout

Shared interfaces and application concepts remain in one repository.

### Shared

- `src/burn_engine.h`
- `src/drive_manager.h`
- repository assets
- Dear ImGui
- RetroBeam/cdrtools source under `external/schilytools`
- RetroBeam CMake ownership under `cmake/retroburner-optical`

### Windows-native implementation

- `src/main.cpp`
- `src/burn_engine.cpp`
- `src/drive_manager.cpp`
- `src/texture_loader.cpp`
- `src/embedded_tools.cpp`
- Win32 / D3D11 ImGui backends

### Linux-native implementation

- `src/main_linux.cpp`
- `src/burn_engine_linux.cpp`
- `src/drive_manager_linux.cpp`
- `src/texture_loader_linux.cpp`
- `src/process_runner_linux.cpp`
- `src/retrobeam_linux.cpp`
- `src/optical_verify_linux.cpp`
- SDL3 / OpenGL3 ImGui backends

The platform implementations must stay behind the same shared contracts where practical. A feature change should be implemented in the shared model/API first, then connected to each native platform implementation as required.

## Build output

Windows:

```text
build/msvc-x64/<Configuration>/RetroBurner.exe
```

Linux:

```text
build/linux/app-<configuration>/bin/RetroBurner
build/linux/app-<configuration>/bin/retrobeam
```

Stage/test build directories used during the Linux bring-up are development artifacts only. They are not part of the final build procedure.
