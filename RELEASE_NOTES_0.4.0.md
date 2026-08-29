# Retro Burner 0.4.0

Released: 20 August 2026
Previous public release: 0.3.0

Retro Burner 0.4.0 is the release that expanded the original Dreamcast-focused burner into a broader console-oriented optical-disc application.

## Highlights

- Rebranded and generalised the application as **Retro Burner**.
- Added a console/profile-driven interface for Dreamcast, PlayStation, PlayStation 2 CD/DVD, Sega Saturn and Xbox 360.
- Added **RetroBeam** as the default recording backend, based on pinned SchilyTools/cdrecord/libscg source with Retro Burner Windows SPTI, FIFO and diagnostic integration.
- Retained **growisofs** as an optional DVD backend for supported PS2 DVD and Xbox 360 workflows.
- Added automatic optical-drive/media detection and write-speed selection from drive-reported capabilities.
- Added capability-driven Advanced Settings for BURN-Free, Force Speed, OPC, MMC streaming/rotation policy and drive-buffer reporting.
- Added automatic eject and completion sound after successful burns.
- Added graphical burn monitoring with overall progress plus host/read **Buffer** and optical **Device Buffer** bars.
- Added backend-aware status parsing so RetroBeam and growisofs report through a consistent frontend.
- Added Xbox 360 XGD2 support and the first experimental XGD3/BurnerMAX preparation and capacity-testing workflow.
- Expanded build, licensing, third-party source and provenance documentation.

## Supported workflows in 0.4.0

| Console | Image / media workflow | 0.4.0 status |
| --- | --- | --- |
| Dreamcast | CDI to CD-R | Physically validated Data+Data and Audio+Data self-boot workflows |
| PlayStation | BIN/CUE to CD-R | Physically validated |
| PlayStation 2 CD | BIN/CUE or ISO to CD-R | Implemented; broader physical regression coverage still required |
| PlayStation 2 DVD | ISO to DVD5/DVD9 | DVD5 physically validated; DVD9 implemented but not physically validated |
| Sega Saturn | BIN/CUE to CD-R | Implemented; broader physical regression coverage still required |
| Xbox 360 XGD2 | ISO to DVD+R DL | Implemented; physical validation remained limited |
| Xbox 360 XGD3 | Prepared ISO to DVD+R DL with BurnerMAX path | Experimental; end-to-end burn validation incomplete |

## RetroBeam recording backend

0.4.0 introduced RetroBeam as Retro Burner's primary recording engine.

RetroBeam uses a pinned SchilyTools/cdrecord/libscg source baseline with Retro Burner-specific Windows integration for:

- native SPTI optical access;
- host FIFO support;
- detailed transport and failure diagnostics;
- live FIFO/read-buffer reporting;
- optical drive-buffer reporting;
- CD and DVD recording paths;
- console-specific DVD layer-break policy.

BURN-Free remains opt-in and is exposed only when the selected drive reports compatible support.

## growisofs backend

`growisofs` remains available as an optional DVD backend for supported PlayStation 2 DVD and Xbox 360 profiles.

Retro Burner parses growisofs write percentage, actual speed, estimated remaining time and `RBU` / `UBU` values into the same graphical burn-monitoring interface used for RetroBeam.

Backend choice is intentionally retained as a compatibility option. Optical results can vary with burner model, firmware, USB/SATA bridge, media MID/batch, selected speed and the target console's optical pickup.

## Dreamcast

The physically tested Dreamcast workflows from earlier releases were retained:

- Data+Data self-boot CDI.
- Audio+Data self-boot CDI.

CDIrip is used to extract the DiscJuggler image and RetroBeam performs the recording passes.

The extraction path deliberately avoids CDIrip's `-cdrecord` preset because that preset also enables track cutting that can damage audio tracks.

## PlayStation and Sega Saturn

0.4.0 added general BIN/CUE console profiles using RetroBeam's CUE/SAO path.

PlayStation BIN/CUE recording was physically tested during the 0.4.0 development line.

Sega Saturn support was implemented using the same general mixed-mode BIN/CUE recording architecture, although broader physical regression coverage was still pending.

## PlayStation 2 CD

PS2 CD support accepts BIN/CUE and ISO input and records through RetroBeam.

The profile was implemented in 0.4.0, with additional physical regression coverage still planned after release.

## PlayStation 2 DVD

0.4.0 added:

- PS2 DVD ISO input.
- read-only media/capacity interrogation with `dvd+rw-mediainfo`.
- RetroBeam as the default DVD backend.
- optional growisofs recording.
- automatic DVD5/DVD9 classification from image size.
- dual-layer handling and calculated layer-break support.

A PS2 DVD5 burn was physically validated on real hardware.

PS2 DVD9 support was implemented but remained physically unvalidated in 0.4.0.

## Xbox 360

### XGD2

0.4.0 added Xbox 360 XGD2 DVD+R DL support using the standard layer break:

```text
1913760
```

Both RetroBeam and growisofs were available as recording backends.

### XGD3 — experimental

0.4.0 also introduced the first XGD3 workflow, including:

- temporary working-copy preparation;
- ABGX360-based preparation/verification;
- native BurnerMAX capability/signature probing;
- no-game-sector-write BurnerMAX test/enable action;
- expanded-capacity verification;
- in-session caching of a successfully prepared working image;
- XGD3 layer break:

```text
2133520
```

XGD3 remained **experimental** in 0.4.0.

During development, the HL-DT-ST GP60NB50/PE00 test drive accepted the BurnerMAX payload and exposed the expected expanded writable capacity, but a complete XGD3 disc write was not finished before that drive became unavailable during separate manual firmware experimentation.

That firmware incident was separate from Retro Burner's normal disc-recording path.

## Burn monitoring and quality-of-life changes

0.4.0 substantially improved the user-facing burn process:

- graphical overall burn progress;
- graphical host/read Buffer bar;
- graphical optical Device Buffer bar;
- parsed actual write speed;
- estimated remaining time where available;
- burn-phase status such as lead-in, sector writing and finalisation;
- automatic drive/media detection;
- real drive-reported write-speed choices;
- automatic eject after successful burns;
- completion sound;
- always-available detailed burn logging.

## Advanced drive controls

Where supported by the selected writer, 0.4.0 exposes advanced RetroBeam controls for:

- BURN-Free;
- Force Speed;
- OPC policy;
- MMC streaming/rotation policy;
- drive-buffer reporting.

These controls are capability-driven rather than assumed to work on every writer.

## Hardware and media variability

Optical-disc recording results are hardware dependent.

A successful or failed result on one development system should not be interpreted as a universal ranking of RetroBeam, growisofs or another recording engine. Important variables include:

- burner model and firmware;
- USB/SATA bridge and connection quality;
- recordable-media type, MID and manufacturing batch;
- selected write speed and the drive's write strategy;
- target console model and optical-pickup condition.

Retro Burner therefore keeps backend choice and detailed logging available for troubleshooting.

## Validation status at release

Physically validated during the 0.4.0 development line:

- Dreamcast Data+Data self-boot CDI.
- Dreamcast Audio+Data self-boot CDI.
- PlayStation BIN/CUE.
- PlayStation 2 DVD5.
- BurnerMAX payload/capacity stage on the GP60NB50/PE00.

Still requiring broader or complete validation:

- complete XGD3 burn and console verification;
- PS2 DVD9 physical burn/boot validation;
- additional PS2 CD regression coverage;
- additional Sega Saturn regression coverage.

## Licensing and third-party software

Retro Burner original source code is MIT licensed. Third-party components retain their own upstream licences and copyright terms.

0.4.0 expanded the repository's third-party source, licence and provenance documentation for Dear ImGui, CDIrip, SchilyTools/cdrtools components, dvd+rw-tools/growisofs, ABGX360, BurnerMAX-related research and bundled assets.

See `THIRD_PARTY.md` and the `licenses/` directory for details.

## Disclaimer

Retro Burner is an independent community project and is not affiliated with, endorsed by, sponsored by or produced by Sega, Sony, Microsoft or the publishers/developers of supported games.

Use Retro Burner only with software and disc images that you have the legal right to use.
