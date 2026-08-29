#!/usr/bin/env bash
set -euo pipefail

umask 022

ROOT="$(
    cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." &&
    pwd
)"

VERSION="0.5.0"
PACKAGE_NAME="RetroBurner-${VERSION}-linux-x86_64"

DIST="$ROOT/dist"
ARCHIVE="$DIST/${PACKAGE_NAME}.tar.gz"
HASH_FILE="$ARCHIVE.sha256"

APP_BUILD="$ROOT/build/linux/app-release"
APP="$APP_BUILD/bin/RetroBurner"

RETROBEAM="$ROOT/build/linux/retrobeam-release/bin/retrobeam"
ABGX360="$ROOT/build/linux/_deps/abgx360/linux/abgx360/abgx360"
CDIRIP="$ROOT/build/linux/cdirip-release/cdirip"

required_commands=(
    cmake
    file
    find
    install
    mktemp
    python3
    sha256sum
    strings
    strip
    tar
)

missing=()

for command in "${required_commands[@]}"; do
    command -v "$command" >/dev/null 2>&1 ||
        missing+=("$command")
done

if ((${#missing[@]} != 0)); then
    echo "[FAIL] Missing release-packaging commands:" >&2
    printf '  %s\n' "${missing[@]}" >&2
    exit 2
fi

echo "============================================================"
echo " Retro Burner ${VERSION} - Linux Release Package"
echo "============================================================"
echo

echo "[1/8] Clean native Linux Release build"

"$ROOT/build-linux.sh" \
    Release \
    --clean \
    --no-desktop

for helper in \
    "$RETROBEAM" \
    "$ABGX360" \
    "$CDIRIP"
do
    [[ -f "$helper" ]] || {
        echo "[FAIL] Expected embedded helper is missing:" >&2
        echo "  $helper" >&2
        exit 3
    }
done

[[ -x "$APP" ]] || {
    echo "[FAIL] Release build did not produce:" >&2
    echo "  $APP" >&2
    exit 4
}

echo
echo "[2/8] Strip embedded Linux helpers"

strip --strip-unneeded "$RETROBEAM"
strip --strip-unneeded "$ABGX360"
strip --strip-unneeded "$CDIRIP"

echo "  RetroBeam stripped"
echo "  ABGX360 stripped"
echo "  CDIrip stripped"

echo
echo "[3/8] Regenerate single-file embedded bundle"

rm -f \
    "$APP_BUILD/generated/embedded_bundle_linux.h" \
    "$APP_BUILD/generated/embedded_bundle_linux.cpp"

cmake \
    --build "$APP_BUILD" \
    --target RetroBurner \
    --parallel

[[ -x "$APP" ]] || {
    echo "[FAIL] Rebuilt RetroBurner is missing." >&2
    exit 5
}

echo
echo "[4/8] Release regression and runtime smoke"

python3 \
    "$ROOT/scripts/test-retrobeam-cuecheck.py" \
    --retrobeam "$RETROBEAM"

"$APP" --version
"$APP" --growisofs-probe >/dev/null

echo
echo "[5/8] Stage package on native Linux filesystem"

STAGE_ROOT="$(mktemp -d -t retroburner-release-XXXXXXXX)"
trap 'rm -rf "$STAGE_ROOT"' EXIT

PACKAGE_ROOT="$STAGE_ROOT/$PACKAGE_NAME"
PACKAGE_LICENSES="$PACKAGE_ROOT/licenses"

install -d -m 0755 \
    "$PACKAGE_ROOT" \
    "$PACKAGE_LICENSES"

install -m 0755 \
    "$APP" \
    "$PACKAGE_ROOT/RetroBurner"

# Strip only the public application copy. The normal build-tree executable
# may retain symbols useful for local diagnosis.
strip --strip-unneeded \
    "$PACKAGE_ROOT/RetroBurner"

for document in \
    LICENSE \
    README.md \
    RELEASE_NOTES_0.5.0.md \
    THIRD_PARTY.md
do
    [[ -f "$ROOT/$document" ]] || {
        echo "[FAIL] Required release document missing: $document" >&2
        exit 6
    }

    install -m 0644 \
        "$ROOT/$document" \
        "$PACKAGE_ROOT/$document"
done

while IFS= read -r -d '' license_file; do
    install -m 0644 \
        "$license_file" \
        "$PACKAGE_LICENSES/$(basename "$license_file")"
done < <(
    find "$ROOT/licenses" \
        -maxdepth 1 \
        -type f \
        -print0
)

echo
echo "[6/8] Validate staged release"

VERSION_OUTPUT="$("$PACKAGE_ROOT/RetroBurner" --version)"

if [[ "$VERSION_OUTPUT" != "Retro Burner 0.5.0 Linux" ]]; then
    echo "[FAIL] Unexpected packaged version:" >&2
    echo "  $VERSION_OUTPUT" >&2
    exit 7
fi

echo "  $VERSION_OUTPUT"

APP_MODE="$(stat -c '%a' "$PACKAGE_ROOT/RetroBurner")"

if [[ "$APP_MODE" != "755" ]]; then
    echo "[FAIL] RetroBurner mode is $APP_MODE; expected 755." >&2
    exit 8
fi

bad_modes=0

while IFS= read -r -d '' release_file; do
    [[ "$release_file" == "$PACKAGE_ROOT/RetroBurner" ]] &&
        continue

    mode="$(stat -c '%a' "$release_file")"

    if [[ "$mode" != "644" ]]; then
        echo "[FAIL] Wrong file mode $mode:" >&2
        echo "  $release_file" >&2
        bad_modes=1
    fi
done < <(
    find "$PACKAGE_ROOT" \
        -type f \
        -print0
)

((bad_modes == 0)) || exit 9

if strings "$PACKAGE_ROOT/RetroBurner" |
    grep -F "$ROOT" >/dev/null
then
    echo "[FAIL] Local repository path remains inside packaged ELF:" >&2

    strings "$PACKAGE_ROOT/RetroBurner" |
        grep -F "$ROOT" |
        head -20 >&2

    exit 10
fi

echo "  file permissions: PASS"
echo "  local repository path check: PASS"

FILE_INFO="$(file "$PACKAGE_ROOT/RetroBurner")"
echo "  $FILE_INFO"

echo
echo "[7/8] Create release archive"

mkdir -p "$DIST"

rm -f \
    "$ARCHIVE" \
    "$HASH_FILE"

tar \
    --sort=name \
    --owner=0 \
    --group=0 \
    --numeric-owner \
    -C "$STAGE_ROOT" \
    -czf "$ARCHIVE" \
    "$PACKAGE_NAME"

(
    cd "$DIST"

    sha256sum \
        "$(basename "$ARCHIVE")" \
        > "$(basename "$HASH_FILE")"
)

echo
echo "[8/8] Verify archive"

(
    cd "$DIST"

    sha256sum \
        -c "$(basename "$HASH_FILE")"
)

echo
echo "Archive permissions:"
tar -tvzf "$ARCHIVE"

echo
echo "============================================================"
echo " [PASS] Linux release package ready"
echo "============================================================"
echo "Archive: $ARCHIVE"
echo "SHA256:  $(sha256sum "$ARCHIVE" | awk '{print $1}')"
echo "Hash:    $HASH_FILE"
