# Changelog

All notable project changes are recorded here.

## [Unreleased]

### Planned after 0.5.0

- Broaden Linux distribution, writer, bridge and media testing.
- Complete XGD3 end-to-end validation on known-compatible hardware.
- Complete remaining PS2 DVD9, PS2 CD and Sega Saturn physical regression coverage.
- Add a GameCube DVD-R profile after image/layout validation; reuse the shared single-layer DVD recording layer rather than creating a console-specific writer.
- Add Sega/Mega-CD and Original Xbox profiles after format/layout research.
- Add CHD input through an appropriately licensed conversion path.
- Add PS2 ESR and FreeDVDBoot preparation from suitable open-source implementations with full attribution.
- Add an optional GitHub Releases update check.
- Add an explicit advanced 32 KiB / 64 KiB recording-transfer compatibility control.
- Consider macOS and 32-bit Windows targets after the Windows/Linux architecture is settled.

## [0.5.0] - 2026-08-30

0.5.0 is the first native Windows + Linux release and a major hardening/quality-of-life update over 0.4.0.

### Linux platform

- Added the native Linux SDL3/OpenGL application.
- Added Linux SG_IO optical-device transport and native Linux RetroBeam integration.
- Added Linux-native drive discovery, process runner, verification, BurnerMAX plumbing and texture/audio handling.
- Added single-file Linux bundling for Retro Burner artwork plus native RetroBeam, CDIrip and ABGX360 helpers.
- Kept growisofs/dvd+rw-mediainfo as host Linux dependencies for the selectable DVD backend.
- Added the public `build-linux.sh` build entry point.
- Added user-local Linux desktop launcher registration with safe handling for repository/build paths containing spaces.
- Added Linux build support for repository paths containing spaces.

### Windows/Linux UI parity

- Made Windows `DrawApp()` the canonical UI source and added generated Linux UI synchronization/checking.
- Added parity guards so important burn controls/status text cannot quietly disappear from one platform.
- Added a hidden no-disc UI simulation matrix covering Dreamcast, PS1, PS2 CD/DVD, Saturn, XGD2/XGD3, RetroBeam/growisofs, verification and failure states.
- Unified burn progress, phase, speed, remaining time, FIFO/read-buffer and device-buffer presentation.
- Removed duplicated textual buffer percentages from the compact burn detail row.
- Fixed Windows DVD phase parsing and Windows successful dummy-result parity.
- Added active-job close protection to Linux.
- Reworked Windows close protection to use the same non-blocking in-app warning so the GUI/render loop continues during an active burn.

### Media and write-speed safeguards

- Gated write-speed choices by the actual mounted blank-media MMC profile rather than only by console selection.
- Prevented incompatible CD/DVD speed descriptors from surviving console/media changes.
- Added console-specific conservative recommended speed defaults.
- CD profiles default to the lowest actual speed advertised by the inserted CD-R.
- PS2 DVD prefers approximately 6x where available; dual-layer/Xbox contexts prefer approximately 4x where available, with conservative fallback.
- Preserved explicit user speed/Automatic choices after the recommendation is initially applied for a new media/drive/profile context.
- Added clearer prompts when the selected console profile does not match the inserted blank media.

### PS2 CD safeguards and verification

- Added PS2 CD ISO media-origin probing.
- Rejects ISO images containing DVD/UDF structures from the PS2 CD profile even when their byte size would fit on CD-R.
- Added optional full post-burn optical readback comparison for PS2 CD ISO, default OFF.
- Added the readback progress/status path on Windows and Linux.
- Added clearer frontend failure categorisation while retaining complete backend output in the Burn Log.

### PlayStation / CDRWIN CUE / RetroBeam

- Added an authoritative no-drive `--rb-cue-check` preflight using the exact RetroBeam CDRWIN CUE parser used for recording.
- Added a 10-case CUE parser regression suite.
- Finalised ordinary PlayStation BIN/CUE recording on the parsed CDRWIN DAO/SAO path after rejecting a synthetic RAW96R experiment during 0.5.0 development; that experiment was never part of a public release.
- Preserved the mixed-mode CUE layout while treating ordinary 2352-byte BIN data as main-channel data rather than synthesizing P-W subchannel content.
- Added an 8 MiB CD host FIFO.
- Added whole-disc CD progress telemetry so multi-track jobs do not reset apparent completion at every track.
- Added real MMC CD dummy/test writes surfaced in the GUI as **DUMMY WRITE CD-R - LASER OFF**.
- Improved lead-in/start-sector/fixation diagnostics and dummy-write completion reporting.
- Physically validated the PlayStation DAO/SAO path with a 24-track mixed-mode BIN/CUE: parser regression 10/10, full dummy pass, real CD-R burn and successful boot on physical PlayStation hardware.

### Burn presentation and safety

- Added consistent lifecycle phase text such as Writing Lead-In, Writing Sectors, Finalising Disc and verification.
- Added clearer failure summaries with full backend output retained in the Burn Log.
- Kept graphical FIFO/read and device-buffer bars as the authoritative buffer presentation.
- Added safeguards against closing the application during an active burn on both platforms.
- Close warnings are non-blocking so the burn/UI continues to run while the warning is visible.
- Improved successful dummy-write completion state and status text.

### Build and release engineering

- Added `build-windows.ps1` as the native Windows build entry point and removed obsolete Windows batch build wrappers.
- Added `build-linux.sh` as the native Linux build entry point.
- Added `docs/BUILDING.md`.
- Added native RetroBeam CMake support required by Linux.
- Added Linux embedded-bundle generation.
- Added cross-platform UI simulation helpers.
- Fixed the internal Windows RetroBeam build script's repository-root resolution.
- Fixed Linux and RetroBeam build handling for paths containing spaces.
- Added release packaging updates for 0.5.0 Windows and Linux assets.

## [0.4.0] - 2026-08-20

0.4.0 was the major expansion from the public 0.3.0 release that generalized the original Dreamcast-focused application into Retro Burner.

### Application and architecture

- Renamed/generalized the application from Dreamcast Burner into Retro Burner.
- Added one console/profile-driven interface for Dreamcast, PlayStation, PlayStation 2 CD/DVD, Sega Saturn and Xbox 360 workflows.
- Retained the native C++20 / Dear ImGui / DirectX 11 Windows application architecture.
- Added drive/media detection, real write-speed choices, automatic eject and completion sound.
- Reworked burn monitoring with graphical overall-progress, **Buffer** and **Device Buffer** bars instead of relying on percentage-only buffer text.
- Added capability-driven Advanced Settings for BURN-Free, Force Speed, OPC, MMC streaming/rotation policy and drive-buffer reporting.
- Added single-EXE helper staging architecture, subject to each redistributed helper's licence requirements.

### Recording backends

- Added RetroBeam, derived from the pinned SchilyTools/cdrecord/libscg source, as Retro Burner's default recording backend.
- Added the native Windows host FIFO and SPTI/libscg development path with detailed buffer/transport diagnostics.
- Kept **growisofs as an optional selectable DVD backend** for PS2 DVD and Xbox 360 profiles instead of presenting one backend as universally superior.
- Added backend-aware live monitoring: RetroBeam FIFO/read and drive-buffer reporting feeds the graphical buffer bars, while growisofs `RBU`/`UBU` output is parsed into the same **Buffer** and **Device Buffer** displays.
- Improved growisofs presentation with parsed write percentage, actual speed, estimated remaining time and GUI burn-phase status for lead-in, sector writing and disc finalisation.
- BURN-Free remains disabled by default and is opt-in only when the selected drive advertises support.
- Added explicit console DVD layer-break policy: XGD2 `1913760`, XGD3 `2133520`, and calculated PS2 DVD9 handling.
- Current development code includes profile-dependent 32 KiB/64 KiB transfer behaviour; a user-visible compatibility selector is planned after 0.4.0.

### Dreamcast and CD workflows

- Preserved physically tested Dreamcast Data+Data and Audio+Data self-boot CDI workflows using CDIrip plus RetroBeam.
- Added PlayStation BIN/CUE recording through RetroBeam CUE/SAO; physically tested.
- Added PlayStation 2 CD and Sega Saturn CD profiles; final physical regression validation remains pending for 0.4.0.
- Added CD burn status parsing, completion sound and automatic eject.

### PlayStation 2 DVD

- Added PS2 DVD ISO support with `dvd+rw-mediainfo` read-only media/capacity interrogation.
- Added RetroBeam and optional growisofs DVD recording paths.
- Added DVD5/DVD9 classification and dual-layer handling/layer-break calculation.
- Physically validated a PS2 DVD5 burn on real hardware.
- PS2 DVD9 remains implemented but not physically release-validated.

### Xbox 360

- Added XGD2 and experimental XGD3 DVD+R DL profiles.
- Added native BurnerMAX capability/signature/capacity probing and a no-game-sector-write test/enable path.
- Added XGD3 temporary-working-copy preparation/verification workflow and GUI progress parsing.
- Added an in-session prepared-image cache so a successful preflight can be reused while live media/BurnerMAX checks are repeated immediately before recording.
- XGD3 remains **end-to-end untested** for 0.4.0: the GP60NB50/PE00 development drive accepted the payload and exposed expanded capacity, but the subsequent disc write did not proceed. The drive was later rendered unusable during separate manual firmware experimentation before a full XGD3 test could be completed.
- Compatible Lite-On iHAS and cross-flash-capable drives intended for permanent C4EVA firmware are planned for the next validation round.
- The ASUS SDRW-08U9M-U B201 is safely rejected by the current MTK-style path; non-MTK experimentation remains planned rather than advertised as supported.

### Drive/media findings

- Reproduced a late-disc failure with ASUS SDRW-08U9M-U B201 + Sony `SONY16D1` DVD-R using RetroBeam, growisofs and ImgBurn on the same general test workload.
- Documented that backend results are affected by burner/firmware, media/MID, bridge, speed and console optical condition; backend selection is a user compatibility choice, not a universal ranking.
- Added a structured burn/coaster-report template so failed burns can be compared meaningfully.

### Release engineering and documentation

- Updated release packaging to use Retro Burner naming and versioned release archives.
- Prepared removal of user-specific hard-coded source paths from the RetroBeam CMake bridge.
- Added a release-pruning script with dry-run default and explicit `-Apply` mode.
- Expanded credits, third-party provenance and release-status wording.
- Added a licence audit gate: 0.4.0 must not ship a third-party executable/source snapshot whose redistribution terms have not been verified.

### Physically validated for this development line

- Dreamcast Data+Data self-boot CDI burn.
- Dreamcast Audio+Data self-boot CDI burn.
- PlayStation BIN/CUE burn, completion sound and auto-eject.
- PlayStation 2 DVD5 burn on real hardware.
- BurnerMAX payload/capacity stage on the GP60NB50/PE00 — **not** a completed XGD3 burn.

### Still required before calling a workflow fully validated

- Complete XGD3 burn and console verification using known-compatible hardware/firmware.
- PS2 DVD9 physical burn/boot validation.
- Final PS2 CD and Sega Saturn physical regression burns.
- Confirm the final third-party redistribution inventory/licences, especially the Xbox 360 image-verification helper.

## [0.3.0] - 2026-08-10

### Added

- Reliable mapping between Windows optical drives and the actual device addresses reported by `cdrecord -scanbus`.
- Always-visible in-application burn log with copy-to-clipboard support.
- Explicit support/documentation for Data+Data and Audio+Data Dreamcast self-boot CDI layouts.

### Changed

- Updated the bundled recording backend to `Cdrecord-ProDVD-ProBD-Clone 3.02a10 2021/07/23`.
- CDI extraction requests ISO conversion for data tracks without CDIrip's `-cdrecord` preset, avoiding its `-cutall` behaviour on audio tracks.
- Drive selection relies on cdrecord's reported bus/target/LUN mapping instead of predicting numbering solely from Windows adapter data.

### Fixed

- Audio+Data CDI session-one recording on tested hardware.
- Incorrect burner selection with multiple attached writers.
- Burn-log visibility regressions during backend testing.

## [0.2.0]

- Initial native Windows Dreamcast Burner implementation.
- DiscJuggler CDI workflow.
- Drive/media detection.
- CDIrip extraction.
- cdrecord session sequencing.
- Burn progress, write-speed display and logging.

<!-- RB_LIBSCG_RETROBURNER_01_CHANGELOG -->
- Successfully built cdrecord 3.02a10 from pinned Schily source using the
  modern MSYS2 MinGW32/GCC toolchain.
- Verified the pristine source-built `schily-0.9` baseline read-only:
  HL-DT-ST GP60NB50 detected; ASUS SDRW-08U9M-U not detected.
- Added the first RetroBurner libscg identity: author `RetroBurner`,
  version `retroburner-0.1` (displayed by cdrecord as
  `RetroBurner-retroburner-0.1`).
- Added a Windows-only USB/IEEE-1394 discovery update using the documented
  Windows storage-property query API before the legacy SCSI-address query.
- Verified `retroburner-0.1` read-only against both the HL-DT-ST GP60NB50 and
  ASUS SDRW-08U9M-U.
- No cdrecord WRITE command path was modified and no physical media was
  written during this milestone.

<!-- RB_LIBSCG_RETROBURNER_02_CHANGELOG -->
- Advanced custom libscg identity to `RetroBurner-retroburner-0.2`.
- Added opt-in, Windows-only SPTI tracing controlled by
  `RETROBURNER_SPTI_TRACE`.
- Added drive/open/enumeration diagnostics, setup-command logging, sampled
  successful WRITE checkpoints, and full failed-command CDB/status/sense/
  timing records.
- Captures `GetLastError` immediately after `DeviceIoControl` so diagnostic
  formatting cannot perturb the existing media-change/invalid-handle retry
  decision.
- No disc-writing policy, CDB construction, write size, speed policy, OPC
  policy, layer-break policy, or BURN-Free policy was intentionally changed.
- Verified read-only enumeration of both HL-DT-ST GP60NB50 (K:) and
  ASUS SDRW-08U9M-U (L:) with tracing enabled.

<!-- RB_WIN32_FIFO_01_CHANGELOG -->
- Restored cdrecord's host FIFO for the native MinGW build.
- Added a Windows-only `VirtualAlloc` + `_beginthreadex` producer/consumer
  implementation while preserving Schily's original fork/shared-memory FIFO
  on Linux, macOS, Cygwin, and other platforms.
- Preserved the verified `RetroBurner-retroburner-0.2` libscg transport and
  SPTI tracing unchanged.
- Added a no-drive/no-disc 32 MiB FIFO/thread self-test.
- Incremental rebuild touches only cdrecord's FIFO/main objects and relinks the
  executable; no configure pass or full Schily-suite rebuild is required.
- No physical media was written during FIFO verification.

<!-- RB_BUILD_03_CHANGELOG -->
- Added explicit executable identity `RetroBurner cdrtools build 0.3` while
  retaining upstream cdrecord `3.02a10` and custom libscg
  `RetroBurner-retroburner-0.2` as separate diagnostic identities.
- Removed Cygwin from the user-facing README. RetroBurner's Windows cdrecord
  path is native MinGW, including the native Win32 32 MiB host FIFO.
- Historical third-party provenance remains documented separately.

<!-- RB_SPTI64K_04 -->
- Black-box comparison on the ASUS SDRW-08U9M-U B201 with SONY16D1 media
  showed ImgBurn issuing DVD WRITE(10) commands as 32 sectors / 65,536 bytes.
- RetroBurner's previous source-built cdrecord path used the upstream Windows
  63 KiB ceiling, producing 31 sectors / 63,488 bytes per DVD WRITE(10).
- Native Windows SPTI now exposes a 64 KiB maximum transfer and cdrecord
  requests a 64 KiB default buffer, yielding 32 x 2,048-byte DVD sectors.
- The legacy ASPI ceiling remains 63 KiB. Cygwin/POSIX defaults remain 63 KiB;
  Linux/macOS code paths are unchanged.
- Advanced custom libscg identity to `RetroBurner-retroburner-0.3` and helper
  identity to `RetroBurner cdrtools build 0.4`.
