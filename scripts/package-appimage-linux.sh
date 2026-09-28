#!/usr/bin/env bash
set -euo pipefail

umask 022

ROOT="$(
    cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." &&
    pwd
)"

VERSION="${1:-0.5.1}"
ARCH="x86_64"
APP_ID="io.github.L10N37.RetroBurner"

DIST="$ROOT/dist"
APPDIR="$ROOT/build/appimage/RetroBurner.AppDir"
TOOLS="$ROOT/build/appimage/tools"

APP="$ROOT/build/linux/app-release/bin/RetroBurner"
RETROBEAM="$ROOT/build/linux/retrobeam-release/bin/retrobeam"
ABGX360="$ROOT/build/linux/_deps/abgx360/linux/abgx360/abgx360"
CDIRIP="$ROOT/build/linux/cdirip-release/cdirip"

DESKTOP="$ROOT/packaging/linux/${APP_ID}.desktop"
METAINFO="$ROOT/packaging/linux/${APP_ID}.metainfo.xml"
ICON_SOURCE="$ROOT/assets/RetroBurner.png"
ICON_STAGED="$ROOT/build/appimage/${APP_ID}.png"

OUTPUT="$DIST/Retro-Burner-${VERSION}-${ARCH}.AppImage"
ZSYNC="$OUTPUT.zsync"

LINUXDEPLOY="${LINUXDEPLOY:-$TOOLS/linuxdeploy-${ARCH}.AppImage}"
APPIMAGETOOL="${APPIMAGETOOL:-$TOOLS/appimagetool-${ARCH}.AppImage}"

LINUXDEPLOY_URL="https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-${ARCH}.AppImage"
APPIMAGETOOL_URL="https://github.com/AppImage/appimagetool/releases/download/1.9.1/appimagetool-${ARCH}.AppImage"

UPDATE_INFORMATION="gh-releases-zsync|L10N37|Retro-Burner|latest|Retro-Burner-*-${ARCH}.AppImage.zsync"

required_commands=(
    curl
    desktop-file-validate
    appstreamcli
    file
    ldd
    readelf
    zsyncmake
)

missing=()
for command in "${required_commands[@]}"; do
    command -v "$command" >/dev/null 2>&1 || missing+=("$command")
done

if (("${#missing[@]}" != 0)); then
    echo "[FAIL] Missing AppImage packaging commands:" >&2
    printf '  %s\n' "${missing[@]}" >&2
    exit 2
fi

for required_file in "$DESKTOP" "$METAINFO" "$ICON_SOURCE"; do
    [[ -f "$required_file" ]] || {
        echo "[FAIL] Required packaging file is missing: $required_file" >&2
        exit 3
    }
done

echo "============================================================"
echo " Retro Burner ${VERSION} - AppImage package"
echo "============================================================"
echo

echo "[1/8] Build clean native Linux Release"
"$ROOT/build-linux.sh" Release --clean --no-desktop

for executable in "$APP" "$RETROBEAM" "$ABGX360" "$CDIRIP"; do
    [[ -x "$executable" ]] || {
        echo "[FAIL] Expected executable is missing: $executable" >&2
        exit 4
    }
done

echo
echo "[2/8] Validate desktop and AppStream metadata"
desktop-file-validate "$DESKTOP"
appstreamcli validate --no-net "$METAINFO"

echo
echo "[3/8] Download AppImage tooling"
mkdir -p "$TOOLS" "$DIST"
if [[ ! -x "$LINUXDEPLOY" ]]; then
    curl --fail --location --retry 3         "$LINUXDEPLOY_URL"         --output "$LINUXDEPLOY"
    chmod 0755 "$LINUXDEPLOY"
fi
if [[ ! -x "$APPIMAGETOOL" ]]; then
    curl --fail --location --retry 3         "$APPIMAGETOOL_URL"         --output "$APPIMAGETOOL"
    chmod 0755 "$APPIMAGETOOL"
fi

echo
echo "[4/8] Assemble AppDir and deploy runtime libraries"
rm -rf "$APPDIR"
mkdir -p "$(dirname "$ICON_STAGED")"
cp "$ICON_SOURCE" "$ICON_STAGED"
mkdir -p     "$APPDIR/usr/bin"     "$APPDIR/usr/share/metainfo"

# Passing the embedded helper build outputs to linuxdeploy is intentional:
# Retro Burner stores these helpers inside the main ELF, but executes extracted
# copies at runtime. Having linuxdeploy inspect them ensures their shared
# library dependencies are present in the AppImage too.
APPIMAGE_EXTRACT_AND_RUN=1 "$LINUXDEPLOY"     --appdir "$APPDIR"     --executable "$APP"     --executable "$RETROBEAM"     --executable "$ABGX360"     --executable "$CDIRIP"     --desktop-file "$DESKTOP"     --icon-file "$ICON_STAGED"

# The helper executables themselves are already embedded in RetroBurner.
# Keep only the dependency libraries linuxdeploy discovered for them.
rm -f     "$APPDIR/usr/bin/retrobeam"     "$APPDIR/usr/bin/abgx360"     "$APPDIR/usr/bin/cdirip"

install -m 0644     "$METAINFO"     "$APPDIR/usr/share/metainfo/${APP_ID}.appdata.xml"

echo
echo "[5/8] Validate AppDir"
[[ -x "$APPDIR/AppRun" ]] || {
    echo "[FAIL] linuxdeploy did not create AppRun." >&2
    exit 5
}
[[ -x "$APPDIR/usr/bin/RetroBurner" ]] || {
    echo "[FAIL] RetroBurner was not deployed into the AppDir." >&2
    exit 6
}

desktop-file-validate "$APPDIR/usr/share/applications/${APP_ID}.desktop"
appstreamcli validate --no-net "$APPDIR/usr/share/metainfo/${APP_ID}.appdata.xml"

VERSION_OUTPUT="$("$APPDIR/AppRun" --version)"
EXPECTED_VERSION="Retro Burner ${VERSION} Linux"
if [[ "$VERSION_OUTPUT" != "$EXPECTED_VERSION" ]]; then
    echo "[FAIL] Unexpected AppDir version output:" >&2
    echo "  $VERSION_OUTPUT" >&2
    echo "Expected:" >&2
    echo "  $EXPECTED_VERSION" >&2
    exit 7
fi
echo "  $VERSION_OUTPUT"

echo
echo "[6/8] Build update-enabled AppImage"
rm -f "$OUTPUT" "$ZSYNC"
ARCH="$ARCH" VERSION="$VERSION" APPIMAGE_EXTRACT_AND_RUN=1 "$APPIMAGETOOL"     -u "$UPDATE_INFORMATION"     "$APPDIR"     "$OUTPUT"

[[ -x "$OUTPUT" ]] || chmod 0755 "$OUTPUT"

# appimagetool writes the .zsync next to the current working directory when
# an explicit AppImage output path is supplied. Normalize it into dist/.
GENERATED_ZSYNC="$(basename "$OUTPUT").zsync"
if [[ ! -f "$ZSYNC" && -f "$GENERATED_ZSYNC" ]]; then
    mv "$GENERATED_ZSYNC" "$ZSYNC"
fi

echo
echo "[7/8] Smoke-test AppImage"
APPIMAGE_EXTRACT_AND_RUN=1 "$OUTPUT" --version | tee "$DIST/appimage-version.txt"
grep -Fx "$EXPECTED_VERSION" "$DIST/appimage-version.txt" >/dev/null

if [[ ! -f "$ZSYNC" ]]; then
    echo "[FAIL] AppImage update information was embedded, but no .zsync was generated." >&2
    exit 8
fi

echo
echo "[8/8] Final package summary"
file "$OUTPUT"
echo "AppImage: $OUTPUT"
echo "zsync:    $ZSYNC"
sha256sum "$OUTPUT"
