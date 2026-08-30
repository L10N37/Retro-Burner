# Retro Burner 0.5.1

Retro Burner 0.5.1 is a focused BurnerMAX compatibility fix for the 0.5.0 release.

## BurnerMAX detection fix

- Fixed false rejection of drives permanently flashed with C4EVA/iXtreme BurnerMAX-capable firmware.
- Retro Burner now recognizes an already-enabled XGD3 writer from its externally observable MMC state:
  - XGD3 layer boundary `2133520`
  - writable capacity sufficient for a complete XGD3 image (`4267015` sectors or greater)
- Permanently flashed drives no longer need to match the temporary RAM-payload register signature pattern.
- Existing strict F1/DF register verification remains in place for drives that require temporary BurnerMAX payload activation.
- No payload is written when the drive already exposes valid expanded XGD3 capacity.

## Hardware validation

Validated on physical Lite-On drives on both Windows and Fedora Linux:

- revision B drive with permanent iXtreme/BurnerMAX firmware: correctly detected as already enabled
- revision C drive requiring temporary BurnerMAX activation: payload still applied and verified correctly

This release otherwise retains the recording behaviour and feature set of Retro Burner 0.5.0.
