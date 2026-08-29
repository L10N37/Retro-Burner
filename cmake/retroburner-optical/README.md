# RetroBeam Optical Engine - Modern Build

This directory is Retro Burner's owned CMake/integration layer for the pinned SchilyTools/cdrtools recording source.

The upstream cdrtools authorship/licence remains preserved. Retro Burner owns the modern build glue, platform adaptation and frontend integration in this directory.

## Current 0.5.0 state

The bridge builds the RetroBeam-linked source set natively for both supported desktop targets:

- **Windows:** native RetroBeam helper with Windows SPTI/libscg transport integration.
- **Linux:** native RetroBeam helper with Linux SG_IO/libscg transport.

The required `cdrecord`, `libscg`, `libscgcmd`, `librscg`, `libschily`, `libdeflt`, `libcdrdeflt` and `libedc` pieces are built directly through CMake rather than invoking the historical Schily RULES/SMakefile build system.

Retro Burner integration includes platform feature/config generation, native transport selection, CDRWIN CUE preflight, frontend progress/buffer telemetry, the Windows native host-FIFO adaptation and Linux path-with-spaces build support.

The pinned recording source baseline and original upstream copyright/licence headers remain preserved.
