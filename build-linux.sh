#!/usr/bin/env bash
set -euo pipefail

CONFIGURATION="${1:-Release}"
CLEAN=0
RUN_AFTER=0
INSTALL_DESKTOP=1

shift || true

for arg in "$@"; do
    case "$arg" in
        --clean)
            CLEAN=1
            ;;
        --run)
            RUN_AFTER=1
            ;;
        --no-desktop)
            INSTALL_DESKTOP=0
            ;;
        *)
            echo "Unknown option: $arg" >&2
            echo "Usage: ./build-linux.sh [Debug|Release] [--clean] [--run] [--no-desktop]" >&2
            exit 2
            ;;
    esac
done

case "$CONFIGURATION" in
    Debug|Release)
        ;;
    *)
        echo "Configuration must be Debug or Release." >&2
        exit 2
        ;;
esac

[[ "$(uname -s)" == "Linux" ]] || {
    echo "build-linux.sh must run natively on Linux." >&2
    exit 3
}

ROOT="$(
    cd -- "$(dirname -- "${BASH_SOURCE[0]}")" &&
    pwd
)"

CONFIG_LOWER="$(
    printf '%s' "$CONFIGURATION" |
    tr '[:upper:]' '[:lower:]'
)"

RETROBEAM_SOURCE="$ROOT/cmake/retroburner-optical"
RETROBEAM_BUILD="$ROOT/build/linux/retrobeam-$CONFIG_LOWER"
CDIRIP_BUILD="$ROOT/build/linux/cdirip-$CONFIG_LOWER"
CDIRIP="$CDIRIP_BUILD/cdirip"
APP_BUILD="$ROOT/build/linux/app-$CONFIG_LOWER"

ABGX_REPOSITORY="https://github.com/hadzz/abgx360.git"
ABGX_COMMIT="0decbd633a050f887ee30d57b24a61d02f1961e4"
ABGX_CHECKOUT="$ROOT/build/linux/_deps/abgx360"
ABGX_SOURCE="$ABGX_CHECKOUT/linux/abgx360"

echo "============================================================"
echo " Retro Burner - Native Linux SINGLE-FILE Build"
echo " Configuration: $CONFIGURATION"
echo "============================================================"
echo

required_commands=(
    cmake
    ninja
    cc
    c++
    pkg-config
    growisofs
    git
    make
    python3
    autoconf
    autoreconf
    automake
    aclocal
)

missing=()

for command in "${required_commands[@]}"; do
    command -v "$command" >/dev/null 2>&1 ||
        missing+=("$command")
done

if ((${#missing[@]} != 0)); then
    echo "Missing required commands:"
    printf '  %s\n' "${missing[@]}"
    exit 4
fi

for pc in libcurl zlib; do
    pkg-config --exists "$pc" || {
        echo "Missing native ABGX360 development dependency: $pc" >&2
        exit 5
    }
done

echo "[0/5] Sync Linux UI from Windows DrawApp"

python3 \
    "$ROOT/scripts/internal/sync-linux-ui.py"

if ((CLEAN)); then
    rm -rf \
        "$RETROBEAM_BUILD" \
        "$APP_BUILD"
fi

echo
echo "[0.5/5] Build native Linux cdirip"

mkdir -p "$CDIRIP_BUILD"

cc \
    -O2 \
    -DNDEBUG \
    -DMAX_PATH=PATH_MAX \
    -include strings.h \
    -I"$ROOT/external/cdirip" \
    "$ROOT/external/cdirip/audio.c" \
    "$ROOT/external/cdirip/buffer.c" \
    "$ROOT/external/cdirip/cdi.c" \
    "$ROOT/external/cdirip/cdirip.c" \
    "$ROOT/external/cdirip/common.c" \
    -lm \
    -o "$CDIRIP"

[[ -x "$CDIRIP" ]] || {
    echo "cdirip build did not produce: $CDIRIP" >&2
    exit 5
}

echo "[1/5] Build native Linux RetroBeam"

cmake \
    -S "$RETROBEAM_SOURCE" \
    -B "$RETROBEAM_BUILD" \
    -G Ninja \
    -DCMAKE_BUILD_TYPE="$CONFIGURATION"

cmake \
    --build "$RETROBEAM_BUILD" \
    --target retrobeam_record \
    --parallel

RETROBEAM="$RETROBEAM_BUILD/bin/retrobeam"

[[ -x "$RETROBEAM" ]] || {
    echo "RetroBeam build did not produce: $RETROBEAM" >&2
    exit 6
}

echo
echo "[2/5] Build pinned native Linux ABGX360"

if [[ ! -d "$ABGX_CHECKOUT/.git" ]]; then
    mkdir -p \
        "$(dirname "$ABGX_CHECKOUT")"

    git clone \
        "$ABGX_REPOSITORY" \
        "$ABGX_CHECKOUT"
fi

git -C "$ABGX_CHECKOUT" \
    fetch --quiet origin

git -C "$ABGX_CHECKOUT" \
    checkout --quiet --detach \
    "$ABGX_COMMIT"

pushd "$ABGX_SOURCE" >/dev/null

rm -f \
    Makefile \
    config.status \
    config.log \
    stamp-h1

autoreconf -fiv

chmod +x ./configure
./configure

make -j"$(nproc)"

popd >/dev/null

ABGX360="$ABGX_SOURCE/abgx360"

[[ -x "$ABGX360" ]] || {
    echo "ABGX360 build did not produce: $ABGX360" >&2
    exit 7
}

echo
echo "[3/5] Build ONE native Linux RetroBurner ELF"

cmake \
    -S "$ROOT" \
    -B "$APP_BUILD" \
    -G Ninja \
    -DCMAKE_BUILD_TYPE="$CONFIGURATION" \
    -DRETROBURNER_EMBED_RETROBEAM="$RETROBEAM" \
    -DRETROBURNER_EMBED_ABGX360="$ABGX360" \
    -DRETROBURNER_EMBED_CDIRIP="$CDIRIP"

cmake \
    --build "$APP_BUILD" \
    --target RetroBurner \
    --parallel

APP="$APP_BUILD/bin/RetroBurner"

[[ -x "$APP" ]] || {
    echo "Retro Burner build did not produce: $APP" >&2
    exit 8
}

echo
echo "[4/5] Remove legacy sidecar output"

rm -rf \
    "$APP_BUILD/bin/assets" \
    "$APP_BUILD/bin/retrobeam" \
    "$APP_BUILD/bin/abgx360"

mapfile -t release_entries < <(
    find "$APP_BUILD/bin" \
        -mindepth 1 \
        -maxdepth 1 \
        -printf '%f\n' |
    sort
)

if ((${#release_entries[@]} != 1)) ||
   [[ "${release_entries[0]}" != "RetroBurner" ]]; then
    echo "[FAIL] Linux release bin directory is not single-file:"
    printf '  %s\n' "${release_entries[@]}"
    exit 9
fi

echo "[PASS] Release bin contains only RetroBurner."

echo
echo "[5/5] Runtime smoke + optional desktop registration"

"$APP" --version
"$APP" --growisofs-probe >/dev/null

if ((INSTALL_DESKTOP)); then
    APP_ID="io.github.L10N37.RetroBurner"
    ICON_DIR="${HOME}/.local/share/icons/hicolor/256x256/apps"
    DESKTOP_DIR="${HOME}/.local/share/applications"

    mkdir -p \
        "$ICON_DIR" \
        "$DESKTOP_DIR"

    install -m 0644 \
        "$ROOT/assets/RetroBurner.png" \
        "$ICON_DIR/${APP_ID}.png"

    cat > "$DESKTOP_DIR/${APP_ID}.desktop" <<EOF
[Desktop Entry]
Type=Application
Version=1.0
Name=Retro Burner
Comment=Native retro console optical disc burner
Exec=${APP}
Icon=${APP_ID}
Terminal=false
Categories=Utility;
StartupNotify=true
StartupWMClass=${APP_ID}
EOF

    chmod 0644 \
        "$DESKTOP_DIR/${APP_ID}.desktop"

    command -v gtk-update-icon-cache >/dev/null 2>&1 &&
        gtk-update-icon-cache \
            "${HOME}/.local/share/icons/hicolor" \
            >/dev/null 2>&1 || true

    command -v update-desktop-database >/dev/null 2>&1 &&
        update-desktop-database \
            "$DESKTOP_DIR" \
            >/dev/null 2>&1 || true

    command -v kbuildsycoca6 >/dev/null 2>&1 &&
        kbuildsycoca6 \
            >/dev/null 2>&1 || true
fi

echo
echo "[PASS] Native Linux SINGLE-FILE build complete."
echo "Application: $APP"
echo "Size:        $(stat -c '%s' "$APP") bytes"
echo
echo "Embedded inside RetroBurner:"
echo "  application artwork"
echo "  static-disc icon"
echo "  native Linux RetroBeam"
echo "  native Linux ABGX360"
echo
echo "Host Linux dependencies:"
echo "  SDL3 / SDL3_image / OpenGL / libc"
echo "  growisofs + dvd+rw-mediainfo"
echo

if ((RUN_AFTER)); then
    exec "$APP"
fi
