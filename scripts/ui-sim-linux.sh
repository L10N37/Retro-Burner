#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
EXE="$ROOT/dist/linux/RetroBurner"
SCENARIO="${1:-all}"

case "$SCENARIO" in
  all|dreamcast|ps1|ps2cd|ps2cd-verify|ps2dvd-retrobeam|ps2dvd-growisofs|saturn|xgd2-retrobeam|xgd2-growisofs|xgd3-retrobeam|xgd3-growisofs|ps2cd-failure)
    ;;
  *)
    echo "Unknown scenario: $SCENARIO" >&2
    exit 2
    ;;
esac

if [[ ! -x "$EXE" ]]; then
  echo "Build Linux Release first: ./build-linux.sh Release" >&2
  exit 3
fi

echo "Retro Burner UI simulation: $SCENARIO"
echo "NO optical backend is started. NO disc WRITE command is issued."
echo "Close the app when inspection is complete."

RETROBURNER_UI_SIM="$SCENARIO" "$EXE"