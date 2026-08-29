# Retro Burner 0.5.0

0.5.0 is a large release: it introduces the first native Linux build while also substantially hardening the Windows application. The update goes well beyond the new Linux build; it includes media-aware speed selection, PS2 CD safeguards, optional verification, burn-result/phase parity, active-burn close protection, a safer PlayStation CDRWIN BIN/CUE path, real dummy-write support and a large amount of build/release engineering.

## Headline changes

- First native **Linux x86-64** Retro Burner release.
- Windows and Linux now share one canonical burn UI/presentation.
- Corrected/validated PlayStation CDRWIN BIN/CUE DAO recording path.
- Authoritative RetroBeam CUE preflight plus no-drive regression suite.
- Real CD-R dummy/test writes with the recording laser off.
- Media-aware write-speed gating and conservative console-specific defaults.
- PS2 CD ISO media-origin safety check.
- Optional PS2 CD ISO post-burn readback verification.
- Better phase/progress/buffer/failure presentation.
- Active-burn exit guard on both platforms without freezing the GUI.
- Modern native Windows/Linux build entry points and release packaging.

## Native Linux release

Linux is now a first-class native build target.

The native Linux application uses SDL3, OpenGL 3, Linux SG_IO, native drive discovery/process execution/readback verification and the same RetroBeam source baseline used by Windows.

The Linux application is built as one Retro Burner ELF containing the application artwork/icon/sound plus native RetroBeam, CDIrip and ABGX360 helpers. The optional DVD growisofs path remains host-provided: `growisofs` and `dvd+rw-mediainfo` must be installed on the system.

A normal Linux source build can also install a user-local desktop launcher. The launcher safely handles build/repository paths containing spaces.

## Windows/Linux UI parity

Windows `DrawApp()` is the source of truth. The Linux burn UI is generated/synchronized from it with checks for required controls/status text.

Both builds now present the same core burn information: overall progress, PHASE, session where applicable, actual speed, remaining time, FIFO/read-buffer health, device-buffer health, completion/failure presentation and the full Burn Log.

A no-disc simulation matrix covers Dreamcast, PlayStation, PS2 CD, PS2 CD verification, PS2 DVD with both backends, Saturn, XGD2/XGD3 with both backends and failure presentation.

## Media-aware write-speed handling

0.5.0 makes the mounted blank-media profile authoritative.

The speed selector is enabled only for compatible inserted media, preventing a DVD speed descriptor from being presented as a CD-console choice.

For a new media/drive/profile context Retro Burner applies a conservative recommendation:

- CD-based consoles: lowest speed actually advertised by the inserted CD-R.
- PS2 DVD: approximately 6x where advertised.
- dual-layer/Xbox 360 contexts: approximately 4x where advertised.
- conservative fallback when the preferred nominal speed is unavailable.

The recommendation is applied when context changes, not every frame, so later explicit user selections remain respected.

## PS2 CD ISO safety and optional verification

An ISO containing DVD/UDF filesystem structures is rejected from the PS2 CD profile even if its byte size would fit on CD-R. The user is directed to the PS2 DVD profile.

For PS2 CD ISO only, optional **Verify disc after burn** can perform a full sector-for-sector readback comparison after recording. Verification is OFF by default because it adds a complete optical read pass.

## PlayStation CDRWIN BIN/CUE validation

A normal CDRWIN BIN/CUE describes mixed-mode main-channel layout but does not automatically provide original 96-byte P-W subchannel data for every sector. A synthetic RAW96R approach was evaluated during 0.5.0 development and rejected before release; it was never part of a public Retro Burner release.

0.5.0 parses the CUE with RetroBeam's real parser before acceptance, preserves the parsed mixed-mode layout and records ordinary PS1 BIN/CUE in DAO/SAO mode.

The DAO/SAO path was physically validated with a 24-track mixed-mode PlayStation BIN/CUE:

- parser regression suite: 10/10.
- full DAO dummy/test write through finalisation.
- real DAO CD-R burn.
- successful boot on physical PlayStation hardware.

## Real CD dummy/test write

0.5.0 exposes RetroBeam's actual MMC test-write mode for compatible CD-R writers as:

**DUMMY WRITE CD-R - LASER OFF**

The normal CD pipeline is exercised—including lead-in, tracks and finalisation—without intentionally recording the disc. Firmware support still varies.

## CD progress and diagnostics

Mixed-mode/multi-track progress is now whole-disc aware. RetroBeam exports disc-level progress instead of making the frontend infer overall completion from a per-track counter that resets at every track.

0.5.0 also adds an 8 MiB CD FIFO, clearer lead-in/finalisation reporting, improved dummy-write results, clearer writer start-position diagnostics and better Windows/Linux phase parsing parity.

## Active-burn close protection

Both platforms refuse an application-close request while a burn/preparation job is active.

The warning is non-blocking on both Windows and Linux, so the render loop and active job continue to update behind the warning.

## Failure presentation

Backend output remains available in full in the Burn Log, while common failures now receive cleaner frontend status. PS2 CD verification failures report the readback stage and mismatch/read-error context.

## Build and release engineering

0.5.0 standardises:

- `build-windows.ps1`
- `build-linux.sh`
- `docs/BUILDING.md`
- native Linux RetroBeam CMake integration
- Linux single-file embedded-bundle generation
- Windows/Linux UI simulation helpers
- CUE parser regression tests
- safe repository-root handling for the internal Windows RetroBeam builder
- build support for paths containing spaces
- safer Linux desktop launcher generation
- separate Windows and Linux release packages with checksums

## Platform notes

### Windows

- Native Win32 / Direct3D 11.
- Windows SPTI optical access.
- Runtime helpers/assets embedded into the EXE.
- Administrator elevation requested for direct optical-device access.

### Linux

- Native x86-64 SDL3 / OpenGL.
- Linux SG_IO optical access.
- RetroBeam/CDIrip/ABGX360 embedded into the Retro Burner ELF.
- `growisofs` and `dvd+rw-mediainfo` are host dependencies.
- Normal native ELF, not AppImage/Flatpak; compatible runtime libraries are required.

## Existing workflow validation status

0.5.0 does not claim that every writer/media/console combination has been physically tested.

- Dreamcast Data+Data / Audio+Data CDI workflows have physical validation.
- PlayStation ordinary mixed-mode BIN/CUE DAO has fresh 0.5.0 physical validation.
- PS2 DVD5 has physical validation.
- PS2 DVD9 remains implemented but not broadly physically validated.
- PS2 CD and Saturn benefit from the shared CUE/media safety work but broader regression coverage is still useful.
- XGD2 physical coverage remains limited.
- XGD3 remains experimental and end-to-end validation is still incomplete.

## Upgrade notes from 0.4.0

- Linux users now have a native Retro Burner build.
- Windows users receive the same safety/preflight/UI improvements; 0.5.0 is not Linux-only.
- Ordinary PS1 BIN/CUE recording now has a physically validated DAO/SAO path.
- Speed recommendations are now media-aware and may differ when new media is inserted.
- PS2 CD ISO can reject images that look DVD-origin; this is intentional wrong-profile protection.
- Optional readback verification adds time but not another write pass.

## Continued testing

- broaden Linux distribution/runtime/writer coverage.
- complete XGD3 end-to-end validation on known-compatible hardware.
- broaden PS2 DVD9 physical burn/boot validation.
- add more PS2 CD and Saturn physical regression reports.

Optical recording remains dependent on writer firmware, media MID/batch, bridge/connection quality, selected speed and the console's optical pickup.
