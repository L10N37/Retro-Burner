# Retro Burner Roadmap

This file describes intended work only. Items are not considered supported until they are implemented, documented and physically validated where practical.

## 0.5.x validation / hardening

0.5.0 established the native Windows + Linux architecture. The next hardening work is:

- collect Linux distribution, writer, USB/SATA bridge and media compatibility reports.
- complete XGD3 testing on known-compatible Lite-On/cross-flash-capable hardware and retain BurnerMAX as experimental until repeatable end-to-end burns exist.
- complete broader PS2 DVD9, PS2 CD and Sega Saturn physical regression coverage.
- continue refining actionable frontend failure messages while retaining complete backend logs.
- add an optional startup GitHub Releases update check with a user-disable option.
- add an explicit advanced 32 KiB / 64 KiB recording-transfer compatibility selector and log the effective value for every burn.
- continue structured drive/media/backend reporting.

## More console workflows

Priority additions:

1. **Nintendo GameCube** — DVD-R workflow. Support should accept the appropriate GameCube image format and reuse Retro Burner's shared single-layer DVD recording layer. Historically, backups were written to 8 cm mini DVD-R media; 12 cm DVD-R was also used with replacement/modified top shells that physically allowed a full-size disc.
2. **Sega/Mega-CD** — reuse the CDRWIN BIN/CUE/DAO foundation only after layout/boot validation.
3. **Original Xbox** — DVD workflow; research required for image preparation/layout and real-hardware validation.
4. **Neo Geo CD**.
5. **PC Engine CD / TurboGrafx-CD**.
6. **3DO**.
7. **NEC PC-FX**.
8. **Amiga CD32 / CDTV**.
9. **Philips CD-i**.
10. **Atari Jaguar CD**.
11. **FM Towns Marty**.

### Shared recording architecture

Console profiles should describe **what must be validated and prepared**, while the recording layer should describe **how sectors are physically written**.

Several consoles therefore converge on the same underlying writer path. A GameCube DVD-R profile, a single-layer PS2 DVD profile and future single-layer DVD systems can share the same generic DVD recording machinery where their physical recording requirements match, while retaining separate image validation, media rules, boot/security notes and console-specific warnings.

For GameCube specifically, 8 cm versus 12 cm DVD-R is primarily a physical-media/form-factor distinction from Retro Burner's point of view. The 12 cm option historically required enough physical clearance in the console, commonly provided by replacement or modified upper shells.

A console does not become “supported” merely because its media is a CD. Mixed-mode layouts, pregaps, subchannel requirements, filesystem expectations and boot behaviour must be checked individually.

## Image preparation

- **CHD:** support `.chd` input by converting to a temporary recording layout via `chdman` or an equivalent appropriately licensed implementation; never modify the user's source image.
- **PS2 ESR:** integrate only from a suitable open-source implementation after licence/provenance review; retain upstream credits/notices.
- **PS2 FreeDVDBoot:** same rule—use an appropriately licensed implementation/data path and clearly distinguish preparation from the recording engine.

## Platforms

- **Windows x64:** shipped.
- **Linux x86-64:** shipped in 0.5.0; broaden portability/runtime packaging and hardware coverage.
- **macOS:** possible after the Windows/Linux architecture and optical transport abstractions are considered stable.
- **Windows 32-bit:** possible “retro PC” build if dependencies/toolchains remain maintainable.

## Project goal

Retro Burner should reduce the need to remember which legacy program handles which console while still respecting that optical recording is hardware- and media-dependent. Feature requests and useful burn/coaster reports are part of that goal.
