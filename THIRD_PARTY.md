# Third-Party Software and Assets

Retro Burner original code is MIT licensed. **That does not relicense any third-party code, executable or asset.** This inventory must match the final release packages and helper binaries embedded into each platform build.

## Dear ImGui

- Purpose: GUI framework/backends.
- Upstream: `ocornut/imgui`.
- Licence: MIT.
- Used by Windows and Linux.
- Release requirement: retain the MIT copyright/licence notice.
- Credit: Omar Cornut and contributors.

## CDIrip

- Purpose: Dreamcast DiscJuggler CDI extraction.
- Upstream lineage: CDIrip by DeXT / Lawrence Williams; maintained source snapshot carries GPL-2.0 terms.
- Licence: GPL-2.0.
- Vendored source: `external/cdirip/`.
- Windows: rebuilt/staged and embedded into `RetroBurner.exe`.
- Linux: built natively by `build-linux.sh` and embedded into the final Retro Burner ELF.
- Release requirement: retain GPL-2.0 text and make corresponding source for the shipped modified executable available while retaining upstream notices.

## RetroBeam / SchilyTools cdrtools components

- Purpose: primary optical recording engine.
- Pinned source: SchilyTools tag `2021-09-18`, commit `90e8f68220698ce0dc132a9f7e7e25f0b9382f64`.
- Applicable linked components include `cdrecord`, `libscg`, `libscgcmd`, `librscg`, `libschily`, `libedc`, `libcdrdeflt` and `libdeflt`.
- Licence for those components in the vendored SchilyTools `COPYING` inventory: CDDL.
- Vendored source: `external/schilytools/`.
- Windows: native RetroBeam with Windows SPTI/libscg integration, embedded into `RetroBurner.exe`.
- Linux: native RetroBeam with Linux SG_IO/libscg integration, embedded into the Retro Burner ELF.
- Release requirement: preserve applicable CDDL headers/text and corresponding source/modifications.

## dvd+rw-tools / growisofs / dvd+rw-mediainfo

- Purpose: DVD media interrogation plus optional DVD recording backend.
- Windows source snapshot: `external/dvd-rw-tools-windows/`.
- Licence: GPL-2.0 in the vendored/upstream Windows port.
- Windows: `growisofs.exe` and `dvd+rw-mediainfo.exe` are embedded runtime helpers.
- Linux: Retro Burner invokes system-installed `growisofs` and `dvd+rw-mediainfo`; they are not embedded into the Linux Retro Burner ELF.
- Release requirement: retain GPL-2.0 notices/source provenance for Windows binaries distributed by this project.
- Credits: Andy Polyakov/dvd+rw-tools contributors and Windows-port contributors.

## ABGX360

- Purpose: Xbox 360 ISO verification/preparation used by the XGD3 workflow.
- Licence: GPL-2.0.
- Windows provenance: vendored `external/abgx360/` community source line at commit `c38475cc23f077ecfab72c1f881f50d82f28d50e`; `tools/abgx360.exe` is embedded into the Windows application.
- Linux provenance: `build-linux.sh` pins `https://github.com/hadzz/abgx360.git` at commit `0decbd633a050f887ee30d57b24a61d02f1961e4`, builds it natively and embeds it into the Linux Retro Burner ELF.
- Credits: Seacrest, Hadzz, BakasuraRCE and later community contributors.
- Release requirement: retain GPL-2.0 notice plus exact source/provenance for the platform helper shipped.

## Native BurnerMAX interoperability code

- Purpose: capability/signature/capacity probing and payload interoperability in Retro Burner-owned code.
- The original C4E `BurnerMax.exe` is not distributed.
- Credits/provenance: C4EVA, Team Jungle and Team Xecuter historical BurnerMAX work/research.
- Release requirement: keep `licenses/BurnerMAX-NOTICE.txt` accurate and avoid implying endorsement/authorship by those projects.

## Mixkit completion sound

- Purpose: success/completion sound.
- Current notice: `licenses/Mixkit-Sound-Effects-NOTICE.txt`.
- Used by Windows and Linux.
- Release requirement: retain the source/licence provenance used when the asset was obtained.

## Project artwork and screenshots

Before release, verify that every asset under `assets/` and `Images/` is project-created or has documented permission/licensing. Console/platform names/logos may also be trademarks; credits do not imply endorsement.

## 0.5.1 packaging notes

Windows and Linux release archives should include the application, `README.md`, `RELEASE_NOTES_0.5.1.md`, project `LICENSE`, `THIRD_PARTY.md` and the `licenses/` directory.

The tagged GitHub repository is the corresponding project/source state for the release. Platform-specific helper provenance above must remain accurate.

## Release audit checklist

- [ ] Final helper list matches the Windows resources actually shipped.
- [ ] Final embedded-helper list matches the Linux embedded bundle.
- [ ] Every helper has a verified upstream licence/redistribution basis.
- [ ] Required licence/copyright notices are packaged and readable.
- [ ] GPL/CDDL corresponding source/modifications for project-distributed helpers are available.
- [ ] CDIrip modified files retain required upstream/change notices.
- [ ] RetroBeam-linked Schily components are described using their applicable CDDL terms.
- [ ] ABGX360 Windows/Linux source pins match their actual release binaries.
- [ ] Asset/sound provenance is verified.
- [ ] Credits do not imply upstream endorsement.

This review is a practical release-engineering inventory, not legal advice.
