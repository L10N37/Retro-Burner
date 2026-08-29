#!/usr/bin/env python3
# No-drive regression tests for RetroBeam's CDRWIN CUE parser.

from __future__ import annotations

import argparse
from pathlib import Path
import subprocess
import tempfile

SECTOR = 2352


def sparse_binary(path: Path, sectors: int) -> None:
    with path.open("wb") as stream:
        stream.truncate(sectors * SECTOR)


def run_case(
    retrobeam: Path,
    root: Path,
    name: str,
    cue: str,
    files: dict[str, int],
    should_pass: bool,
    expected_tracks: int | None = None,
) -> bool:
    case_dir = root / name
    case_dir.mkdir()

    for filename, sectors in files.items():
        sparse_binary(case_dir / filename, sectors)

    cue_path = case_dir / f"{name}.cue"
    cue_path.write_text(cue.strip() + "\n", encoding="utf-8")

    result = subprocess.run(
        [str(retrobeam), "--rb-cue-check", str(cue_path)],
        cwd=case_dir,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        timeout=15,
    )

    marker = "RB_CUECHECK_OK"
    accepted = result.returncode == 0 and marker in result.stdout

    if should_pass:
        expected = (
            f"RB_CUECHECK_OK tracks={expected_tracks}"
            if expected_tracks is not None
            else marker
        )
        ok = accepted and expected in result.stdout
    else:
        ok = not accepted

    print(f"[{'PASS' if ok else 'FAIL'}] {name}")
    if not ok:
        print("----- RetroBeam output -----")
        print(result.stdout.rstrip())
        print("----------------------------")
    return ok


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--retrobeam",
        required=True,
        type=Path,
        help="Path to the freshly built RetroBeam executable",
    )
    args = parser.parse_args()

    retrobeam = args.retrobeam.expanduser().resolve()
    if not retrobeam.is_file():
        print(f"[FAIL] RetroBeam not found: {retrobeam}")
        return 2

    cases = [
        (
            "simple_mode2_2352",
            """
FILE "simple.bin" BINARY
  TRACK 01 MODE2/2352
    INDEX 01 00:00:00
""",
            {"simple.bin": 400},
            True,
            1,
        ),
        (
            "mixed_mode_index00",
            """
FILE "mixed.bin" BINARY
  TRACK 01 MODE2/2352
    INDEX 01 00:00:00
  TRACK 02 AUDIO
    INDEX 00 00:02:00
    INDEX 01 00:04:00
  TRACK 03 AUDIO
    INDEX 00 00:06:00
    INDEX 01 00:08:00
""",
            {"mixed.bin": 1000},
            True,
            3,
        ),
        (
            "explicit_pregap",
            """
FILE "pregap.bin" BINARY
  TRACK 01 MODE2/2352
    PREGAP 00:02:00
    INDEX 01 00:00:00
  TRACK 02 AUDIO
    PREGAP 00:02:00
    INDEX 01 00:04:00
""",
            {"pregap.bin": 700},
            True,
            2,
        ),
        (
            "multiple_binary_files",
            """
FILE "track 01.bin" BINARY
  TRACK 01 MODE2/2352
    INDEX 01 00:00:00
FILE "track 02.bin" BINARY
  TRACK 02 AUDIO
    INDEX 01 00:00:00
FILE "track 03.bin" BINARY
  TRACK 03 AUDIO
    INDEX 01 00:00:00
""",
            {
                "track 01.bin": 300,
                "track 02.bin": 300,
                "track 03.bin": 300,
            },
            True,
            3,
        ),
        (
            "higher_indexes",
            """
FILE "indexes.bin" BINARY
  TRACK 01 MODE2/2352
    INDEX 01 00:00:00
  TRACK 02 AUDIO
    INDEX 00 00:02:00
    INDEX 01 00:04:00
    INDEX 02 00:05:00
    INDEX 03 00:06:00
""",
            {"indexes.bin": 800},
            True,
            2,
        ),
        (
            "missing_referenced_file",
            """
FILE "does-not-exist.bin" BINARY
  TRACK 01 MODE2/2352
    INDEX 01 00:00:00
""",
            {},
            False,
            None,
        ),
        (
            "missing_index01",
            """
FILE "missing-index.bin" BINARY
  TRACK 01 MODE2/2352
    INDEX 00 00:00:00
""",
            {"missing-index.bin": 300},
            False,
            None,
        ),
        (
            "nonsequential_track",
            """
FILE "tracks.bin" BINARY
  TRACK 01 MODE2/2352
    INDEX 01 00:00:00
  TRACK 03 AUDIO
    INDEX 01 00:02:00
""",
            {"tracks.bin": 500},
            False,
            None,
        ),
        (
            "nonsequential_index",
            """
FILE "bad-index.bin" BINARY
  TRACK 01 MODE2/2352
    INDEX 01 00:00:00
  TRACK 02 AUDIO
    INDEX 00 00:02:00
    INDEX 02 00:04:00
""",
            {"bad-index.bin": 600},
            False,
            None,
        ),
        (
            "unsupported_keyword",
            """
FILE "keyword.bin" BINARY
  TRACK 01 MODE2/2352
    FOOBAR 123
    INDEX 01 00:00:00
""",
            {"keyword.bin": 300},
            False,
            None,
        ),
    ]

    passed = 0
    with tempfile.TemporaryDirectory(prefix="retrobeam-cuecheck-") as temp:
        root = Path(temp)
        for name, cue, files, should_pass, expected_tracks in cases:
            if run_case(
                retrobeam,
                root,
                name,
                cue,
                files,
                should_pass,
                expected_tracks,
            ):
                passed += 1

    total = len(cases)
    print()
    print(f"RetroBeam CUE parser regression: {passed}/{total} passed")
    print("Optical drive access: NONE")
    print("Disc writes: NONE")

    if passed != total:
        return 1

    print("RB_STAGE44AC_CUECHECK_SELFTEST_OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
