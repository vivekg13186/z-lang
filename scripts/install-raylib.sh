#!/usr/bin/env bash
#
# install-raylib.sh — fetch + build + install raylib for z-console.
#
# Reaches for the right package manager on macOS / Debian / Fedora /
# Arch first. Falls back to building raylib from source when the
# distro doesn't ship a -dev package (notably Ubuntu < 24.04).
#
# Usage:
#   ./scripts/install-raylib.sh                # auto
#   ./scripts/install-raylib.sh --from-source  # always build from git
#   ./scripts/install-raylib.sh --prefix /opt  # custom install prefix
#
# Needs sudo for the final `make install` (or the apt/dnf/pacman call).
#
# After this runs, `pkg-config --cflags --libs raylib` should print
# real flags, and `make -C z-console` will pick raylib up automatically.

set -euo pipefail

FROM_SOURCE=0
PREFIX="/usr/local"
RAYLIB_TAG="${RAYLIB_TAG:-5.0}"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --from-source) FROM_SOURCE=1; shift ;;
        --prefix)      PREFIX="$2"; shift 2 ;;
        -h|--help)
            sed -n '2,/^$/ p' "$0" | sed 's/^# \?//'
            exit 0 ;;
        *) echo "unknown arg: $1" >&2; exit 2 ;;
    esac
done

uname_s="$(uname -s)"
have() { command -v "$1" >/dev/null 2>&1; }
log()  { printf '\033[1;36m==>\033[0m %s\n' "$*"; }
die()  { printf '\033[1;31merror:\033[0m %s\n' "$*" >&2; exit 1; }

if [[ "$FROM_SOURCE" == 0 ]]; then
    if [[ "$uname_s" == "Darwin" ]] && have brew; then
        log "macOS detected — installing via Homebrew."
        brew install raylib pkg-config
        exit 0
    fi
    if have apt-get; then
        # Try the distro package first; fall through to source if absent.
        if apt-cache show libraylib-dev >/dev/null 2>&1; then
            log "Debian/Ubuntu — installing libraylib-dev via apt-get."
            sudo apt-get update
            sudo apt-get install -y libraylib-dev pkg-config
            exit 0
        fi
        log "apt-get has no libraylib-dev (typical on Ubuntu < 24.04); falling back to source build."
    elif have dnf; then
        log "Fedora — installing via dnf."
        sudo dnf install -y raylib-devel pkgconf-pkg-config
        exit 0
    elif have pacman; then
        log "Arch — installing via pacman."
        sudo pacman -S --needed raylib pkgconf
        exit 0
    fi
fi

# ----- build from source -----------------------------------------------

if have apt-get; then
    log "Installing raylib's X11/audio/GL build dependencies via apt-get."
    sudo apt-get update
    sudo apt-get install -y --no-install-recommends \
        build-essential git pkg-config \
        libasound2-dev libx11-dev libxrandr-dev libxi-dev \
        libgl1-mesa-dev libglu1-mesa-dev libxcursor-dev libxinerama-dev
fi

src_dir="$(mktemp -d)"
trap 'rm -rf "$src_dir"' EXIT
log "Cloning raylib $RAYLIB_TAG into $src_dir"
git clone --depth 1 --branch "$RAYLIB_TAG" https://github.com/raysan5/raylib.git "$src_dir/raylib"
log "Building raylib (shared lib, desktop platform)."
make -C "$src_dir/raylib/src" PLATFORM=PLATFORM_DESKTOP RAYLIB_LIBTYPE=SHARED -j"$(nproc 2>/dev/null || echo 2)"
log "Installing to $PREFIX (sudo)."
sudo make -C "$src_dir/raylib/src" PLATFORM=PLATFORM_DESKTOP RAYLIB_LIBTYPE=SHARED \
    RAYLIB_INSTALL_PATH="$PREFIX/lib" RAYLIB_H_INSTALL_PATH="$PREFIX/include" install
sudo ldconfig 2>/dev/null || true

# Write a pkg-config file if the upstream Makefile didn't drop one — it
# usually does on Linux, but be defensive.
PC="$PREFIX/lib/pkgconfig/raylib.pc"
if [[ ! -f "$PC" ]]; then
    log "Writing $PC"
    sudo mkdir -p "$(dirname "$PC")"
    sudo tee "$PC" >/dev/null <<EOF
prefix=$PREFIX
exec_prefix=\${prefix}
libdir=\${exec_prefix}/lib
includedir=\${prefix}/include

Name: raylib
Description: Simple and easy-to-use library to enjoy videogames programming
Version: $RAYLIB_TAG
Libs: -L\${libdir} -lraylib -lm -lpthread -ldl -lrt -lX11
Cflags: -I\${includedir}
EOF
fi

log "Done. Verify with:  pkg-config --modversion raylib  →  $(pkg-config --modversion raylib 2>/dev/null || echo '?')"
