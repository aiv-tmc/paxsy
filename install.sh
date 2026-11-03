#!/bin/sh
# install.sh - one-line installer for the Paxsy compiler.
#
#   curl -fsSL https://paxsy.org/install.sh | sh
#
# Downloads the release tarball from paxsy.org, builds the compiler
# with the system C compiler and installs the binary, the Vim syntax
# files and the Paxsy-written standard library. The compiler emits
# x86-64 machine code in-process; no assembler, external linker or
# runtime archive is involved, so the installer ships none of them.
#
# Environment knobs:
#   INSTALL_PATH  destination for the binary (default /usr/local/bin)
#   VERSION       release to fetch (default: the latest one)
#   NO_VIM=1      skip the Vim syntax files
#   VIMFILES      Vim runtime directory (default /usr/share/vim/vimfiles)

set -eu

BASE="https://paxsy.org/releases"
VERSION="${VERSION:-}"
INSTALL_PATH="${INSTALL_PATH:-/usr/local/bin}"
VIMFILES="${VIMFILES:-/usr/share/vim/vimfiles}"

say()  { printf '\033[92m==>\033[0m %s\n' "$*"; }
die()  { printf '\033[91m==>\033[0m %s\n' "$*" >&2; exit 1; }

# Pick an available downloader.
if command -v curl >/dev/null 2>&1; then
    fetch() { curl -fsSL "$1"; }
elif command -v wget >/dev/null 2>&1; then
    fetch() { wget -qO- "$1"; }
else
    die "neither curl nor wget is available"
fi

# The binary and the Vim files may require elevated rights; the
# standard library lands next to the binary under the same prefix.
SUDO=""
if [ ! -w "$INSTALL_PATH" ]; then
    if command -v sudo >/dev/null 2>&1; then
        SUDO="sudo"
    else
        die "$INSTALL_PATH is not writable and sudo is unavailable"
    fi
fi

if [ -n "$VERSION" ]; then
    TARBALL="$BASE/paxsy-$VERSION.tar.gz"
else
    TARBALL="$BASE/paxsy.tar.gz"
fi

say "downloading $TARBALL"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT INT TERM
fetch "$TARBALL" | tar -xz -C "$TMP"
SRC="$(find "$TMP" -maxdepth 1 -mindepth 1 -type d | head -n 1)"
[ -n "$SRC" ] || die "failed to unpack the release"

say "building"
make -C "$SRC" >/dev/null

say "installing paxsy to $INSTALL_PATH"
$SUDO install -Dm755 "$SRC/paxsy" "$INSTALL_PATH/paxsy"

# The standard library is Paxsy source compiled on every build; the
# compiler locates it relative to its own executable path, so it is
# installed into the conventional system location beside the binary.
if [ -d "$SRC/lib/std" ]; then
    LIBDIR="$INSTALL_PATH/../lib/paxsy"
    $SUDO mkdir -p "$LIBDIR"
    $SUDO cp -r "$SRC/lib/std" "$LIBDIR/std"
fi

# Vim support is installed unless explicitly disabled.
if [ "${NO_VIM:-0}" != "1" ]; then
    say "installing Vim syntax files to $VIMFILES"
    $SUDO install -Dm644 "$SRC/editors/vim/ftdetect/px.vim" \
        "$VIMFILES/ftdetect/px.vim"
    $SUDO install -Dm644 "$SRC/editors/vim/syntax/px.vim" \
        "$VIMFILES/syntax/px.vim"
fi

say "done"
"$INSTALL_PATH/paxsy" --version || true
