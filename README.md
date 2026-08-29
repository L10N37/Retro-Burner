![Retro Burner](Images/dreamcastburner.png)

# Retro Burner

Retro Burner is a native **Windows and Linux** optical-disc burning frontend for classic game consoles. The goal is to replace a pile of console-specific burning utilities, command lines and old guides with one profile-driven application while keeping enough backend choice, logging and diagnostics to troubleshoot difficult writer/media combinations.

**Current version: 0.5.0**

Retro Burner is intended only for images and backups that you are legally entitled to use.

## 0.5.0 at a glance

0.5.0 is a substantial release. Linux is now a first-class native target, and the Windows build also receives a large set of safety, preflight, burn-monitoring and quality-of-life improvements.

Highlights include:

- **Native Linux release** using SDL3, OpenGL and Linux SG_IO optical access.
- **Windows/Linux UI parity**: Windows `DrawApp()` is the canonical UI and the Linux burn UI is generated/checked from it so the two platforms do not silently drift apart.
- **Single-file application design on both platforms** for Retro Burner-owned/bundled runtime pieces. Windows ships as one EXE; Linux ships as one native ELF containing the application artwork plus its native RetroBeam, CDIrip and ABGX360 helpers.
- **Media-aware write-speed selection**: speed choices are gated by the actual mounted blank-media profile instead of just the selected console.
- **Console-specific conservative speed defaults** while still allowing an explicit user choice or firmware-controlled Automatic mode.
- **PS2 CD ISO safety gate** that rejects DVD/UDF-origin images accidentally selected under the CD profile.
- **Optional post-burn readback verification for PS2 CD ISO**, default OFF.
- **Improved failure presentation** with concise frontend errors while preserving the complete backend log.
- **Canonical burn presentation** across Windows and Linux: progress, phase, actual speed, remaining time, FIFO/read buffer and device buffer are presented consistently.
- **Active-burn exit protection** on both platforms. Closing Retro Burner while a job is active is refused with a non-blocking warning so the GUI keeps updating.
- **Real dummy/test-write support** for CD-R where the writer supports MMC test mode, allowing the complete pipeline to be exercised with the recording laser off.
- **Authoritative CDRWIN CUE preflight** using the same RetroBeam parser used for recording.
- **PlayStation BIN/CUE validation**: ordinary PS1 CDRWIN BIN/CUE uses the parsed CDRWIN layout and the physically validated DAO/SAO recording path.
- **Whole-disc CD progress telemetry** so mixed-mode/multi-track jobs no longer appear to finish when only an early track completes.
- **8 MiB CD FIFO**, improved lead-in/finalisation status and clearer dummy/start-sector diagnostics.
- **No-disc UI simulation matrix** for Dreamcast, PS1, PS2 CD/DVD, Saturn, XGD2/XGD3, both DVD backends, verification and failure states.
- Modern native build entry points: `build-windows.ps1` and `build-linux.sh`.
- Linux desktop launcher installation with safe handling for repository/build paths containing spaces.

See [`RELEASE_NOTES_0.5.0.md`](RELEASE_NOTES_0.5.0.md) for the detailed release summary.

## Downloads

Release assets are published on the GitHub Releases page.

### Windows x64

The Windows build is a native Win32/Direct3D 11 application and uses Windows SPTI for optical access. The release executable requests administrator rights because direct optical-device access requires them.

The Windows release embeds the runtime helpers used by its supported workflows.

### Linux x86-64

The Linux build is a native SDL3/OpenGL application using Linux SG_IO for optical access.

The Linux executable embeds Retro Burner's artwork plus native RetroBeam, CDIrip and ABGX360 helper binaries. The selectable DVD `growisofs` path and `dvd+rw-mediainfo` remain **host Linux dependencies** rather than being embedded.

The 0.5.0 Linux asset is a normal native ELF, not an AppImage/Flatpak. A compatible SDL3, SDL3_image, OpenGL and libc environment is required.

## Supported console profiles

| Console | Image format | Recording path | 0.5.0 status |
| --- | --- | --- | --- |
| Dreamcast | CDI | CDIrip + RetroBeam | Physically tested workflow |
| PlayStation | BIN/CUE | RetroBeam CDRWIN CUE + DAO/SAO | **0.5.0 DAO path physically validated** |
| PlayStation 2 CD | BIN/CUE or ISO | RetroBeam | Implemented; ISO media-type guard and optional ISO readback verification added |
| PlayStation 2 DVD | ISO, DVD5/DVD9 | RetroBeam or growisofs | DVD5 physically tested; DVD9 implemented but not physically validated |
| Sega Saturn | BIN/CUE | RetroBeam CDRWIN CUE + DAO/SAO | Implemented; broader physical regression coverage still welcome |
| Xbox 360 XGD2 | ISO to DVD+R DL | RetroBeam or growisofs | Implemented; physical validation still limited |
| Xbox 360 XGD3 | ISO to DVD+R DL | image preparation/verification + BurnerMAX path + selected DVD backend | **Experimental; end-to-end validation still incomplete** |

Hardware/media validation is necessarily narrower than the number of implemented code paths. Optical recording depends on writer firmware, media MID/batch, USB/SATA bridge, selected speed and the target console's optical pickup.

## Recording backends

### RetroBeam

RetroBeam is Retro Burner's default recording backend. It is built from the pinned SchilyTools/cdrecord/libscg source baseline with Retro Burner integration for:

- Windows SPTI transport.
- Linux SG_IO transport.
- host/read FIFO telemetry.
- optical drive-buffer telemetry.
- CDRWIN CUE parsing/preflight.
- CD DAO/SAO and test-write handling.
- DVD recording paths.
- whole-disc progress and frontend-oriented diagnostics.

RetroBeam remains conservative about BURN-Free: underrun recovery is opt-in and capability-gated rather than silently forced on.

### growisofs

`growisofs` remains an optional DVD backend for supported PS2 DVD and Xbox 360 workflows.

Retro Burner parses growisofs percentage/speed plus `RBU`/`UBU` telemetry into the same user-facing progress/buffer presentation used for RetroBeam.

Backend choice is a compatibility option, not a claim that one engine is universally better.

## Console workflows

### Dreamcast CDI

Dreamcast DiscJuggler images are extracted with CDIrip and recorded with RetroBeam. The tested self-boot Data+Data and Audio+Data workflows are retained.

Retro Burner requests the required CDIrip conversion without enabling CDIrip's `-cdrecord` preset, because that preset also enables track cutting that can damage audio tracks.

### PlayStation BIN/CUE

0.5.0 tightens the PS1 path substantially.

The exact RetroBeam CDRWIN CUE parser can now be run as a no-drive preflight before the GUI accepts the image. Ordinary BIN/CUE recording preserves the parsed mixed-mode layout and records in DAO/SAO mode.

Ordinary 2352-byte BIN/CUE sets are treated as main-channel data described by the CUE. True subchannel-aware support remains a separate future path for image formats that genuinely provide or require that material.

The DAO/SAO path was validated with a 24-track mixed-mode PlayStation BIN/CUE: parser regression tests passed, a complete MMC dummy write/fixation pass completed, a real CD-R was burned and the disc booted on physical PlayStation hardware.

### PlayStation 2 CD

PS2 CD supports BIN/CUE and ISO input.

For ISO input, 0.5.0 adds a media-origin sanity check. An ISO that contains DVD/UDF structures is rejected from the CD profile even if its byte size would fit on CD-R, preventing a common wrong-profile mistake.

PS2 CD ISO also offers optional full readback comparison after a successful burn. Verification is off by default because it adds a complete second optical read pass.

### PlayStation 2 DVD

PS2 DVD ISO images can be recorded with RetroBeam or the optional growisofs backend. `dvd+rw-mediainfo` is used for read-only media/capacity interrogation.

DVD5 has been physically tested. DVD9 handling and layer-break calculation are implemented, but broad physical validation remains incomplete.

### Sega Saturn

Saturn BIN/CUE uses the same authoritative CDRWIN CUE/DAO architecture as the other mixed-mode CD profiles. More physical writer/media/console reports are still useful.

### Xbox 360 XGD2 / XGD3

XGD2 uses DVD+R DL with layer break:

```text
1913760
```

XGD3 uses layer break:

```text
2133520
```

XGD3 also requires a correctly prepared image and enough writable DVD+R DL capacity. Retro Burner performs image preparation/verification on a temporary working copy and never modifies the user's source ISO.

The native BurnerMAX interoperability path remains experimental. A compatible-capacity state has been reached during development, but XGD3 should still be treated as an experimental workflow until repeatable complete burns and console verification are available.

## Write-speed behaviour

0.5.0 no longer shows a console-derived speed list when the inserted media is from the wrong family. The mounted MMC media profile is authoritative.

When a new compatible media/drive/profile context is detected, Retro Burner chooses a conservative recommendation:

- CD-based profiles: lowest actual speed advertised by the inserted CD-R.
- PS2 DVD: prefers approximately 6x where the media/drive advertises it, otherwise falls back conservatively.
- dual-layer/Xbox 360 contexts: prefers approximately 4x where advertised, otherwise falls back conservatively.

The recommendation is applied when the media/drive/profile context changes. It does **not** continuously overwrite a later explicit user choice.

## Burn monitoring and safety

During an active write Retro Burner can show:

- overall whole-disc progress.
- current phase such as **Writing Lead-In**, **Writing Sectors**, **Finalising Disc** or verification.
- actual write speed.
- estimated remaining time where available.
- host FIFO/read-buffer health.
- optical device-buffer health.
- full backend log.

The compact burn detail row and the graphical buffer bars have separate roles; buffer percentages are not duplicated into profile-specific text.

Windows and Linux also refuse an application-close request while `BurnEngine` is busy. The warning is rendered non-blockingly so the active burn and GUI continue to progress.

## Dummy/test write

For compatible CD-R writers, **DUMMY WRITE CD-R - LASER OFF** runs the real RetroBeam CD recording pipeline in MMC test mode.

It exercises track setup, lead-in, sector streaming and finalisation without intentionally recording the disc. Drive support for test mode/fixation behaviour still varies by firmware.

## Linux notes

The public Linux build is new in 0.5.0. It uses:

- SDL3 + OpenGL 3 for the GUI.
- Linux SG_IO for native optical commands.
- the same RetroBeam source baseline as Windows.
- generated UI parity from the canonical Windows `DrawApp()`.
- a single-file release binary for Retro Burner-owned/bundled assets/helpers.
- host `growisofs` and `dvd+rw-mediainfo` for the optional DVD backend.

A normal `./build-linux.sh Release` build also installs/refreshes a user-local desktop launcher. Use `--no-desktop` when building a test/release copy without changing the desktop entry.

## Building from source

Detailed instructions are in [`docs/BUILDING.md`](docs/BUILDING.md).

Windows Release:

```powershell
.\build-windows.ps1 -Configuration Release
```

Linux Release:

```bash
./build-linux.sh Release
```

Windows release package:

```powershell
.\package-release.bat
```

Linux release package:

```bash
./package-release-linux.sh
```

## Project layout

```text
Retro-Burner/
|-- .github/ISSUE_TEMPLATE/          report/request templates
|-- Images/                          README/release screenshots
|-- assets/                          embedded console art, icon and sound
|-- cmake/                           RetroBeam and platform build integration
|-- docs/                            build/developer documentation
|-- external/                        third-party source snapshots
|-- licenses/                        third-party licence texts/notices
|-- scripts/                         build, release, test and UI-parity helpers
|-- src/                             Retro Burner C++ source
|-- CMakeLists.txt
|-- README.md
|-- RELEASE_NOTES_0.5.0.md
|-- THIRD_PARTY.md
|-- CHANGELOG.md
`-- LICENSE
```

## Planned work

The next development line can focus on:

- broader Linux drive/media testing and portability.
- completed XGD3 validation on known-compatible hardware.
- PS2 DVD9, PS2 CD and Saturn physical regression coverage.
- GameCube DVD-R workflow after image/profile validation, including 8 cm mini DVD-R and the historically used 12 cm DVD-R option for consoles with suitable full-size-disc shell clearance.
- Sega/Mega-CD and Original Xbox workflows.
- CHD input through an appropriately licensed conversion path.
- PS2 ESR and FreeDVDBoot preparation from suitable open-source implementations.
- optional application update checking.
- an explicit advanced 32 KiB / 64 KiB transfer compatibility control.
- possible macOS and 32-bit Windows targets after the Windows/Linux architecture is settled.

See [`ROADMAP.md`](ROADMAP.md).

## Drive/media reports

A failed disc can be useful data if the hardware/media details and complete log are preserved. Use the **Burn / coaster report** issue template and include:

- Retro Burner version/commit and operating system.
- console/profile and image format.
- selected recording backend.
- writer model, firmware and connection type.
- blank-media type, brand and MID if available.
- selected speed/settings.
- failure percentage/stage and final error.
- complete Retro Burner burn log.
- whether the same image/media behaved differently with another backend/application.

## Licensing

Original Retro Burner source code is released under the **MIT License**. That grant applies only to original Retro Burner code and does not relicense third-party source, helper executables, artwork or sounds.

For redistributed third-party components, the release must retain the applicable upstream licences/notices and corresponding source/provenance as required.

See [`THIRD_PARTY.md`](THIRD_PARTY.md) and `licenses/`.

## Credits

Retro Burner stands on a large amount of prior open-source and optical-disc work. Thanks to:

- **Omar Cornut and Dear ImGui contributors** — Dear ImGui.
- **DeXT / Lawrence Williams and CDIrip contributors/maintainers** — DiscJuggler CDI extraction.
- **Jörg Schilling and SchilyTools/cdrtools contributors** — cdrecord, libscg and related optical-recording foundations used by RetroBeam.
- **Andy Polyakov and dvd+rw-tools contributors** — DVD media interrogation and growisofs.
- **Seacrest, Hadzz, BakasuraRCE and later ABGX360 community contributors** — Xbox 360 image verification/preparation.
- **C4EVA, Team Jungle and Team Xecuter researchers/developers** — historical BurnerMAX work and research. Retro Burner does not distribute the original BurnerMax executable.
- **Mixkit** — current completion sound asset, subject to the retained asset notice/licence terms.
- Everyone testing burns, reporting failed media combinations and documenting old optical hardware.

Third-party names are credits/provenance only and do not imply endorsement.

## Disclaimer

Retro Burner is an independent community project. It is not affiliated with, endorsed by, sponsored by, or produced by Sega, Sony, Microsoft or the publishers/developers of supported games.

Console names and trademarks belong to their respective owners. No BIOS files, console firmware, game data or copyrighted game images are included.

Use Retro Burner only with software and disc images that you have the legal right to use.
