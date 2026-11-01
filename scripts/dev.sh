#!/bin/sh
# install-dev.sh - the developer installation script.
#
# Default mode builds the toolchain from this checkout, installs it
# under a user-writable prefix, and loads the bundled standard library
# (the `std/` package directory) into the local package cache:
#
#   scripts/install-dev.sh
#   PREFIX=$HOME/.local scripts/install-dev.sh
#
# With --libs-only only the standard library step runs; the release
# installer (install.sh) uses this mode after building the tarball.
#
# Environment knobs:
#   PREFIX             installation prefix (default $HOME/.local)
#   NO_VIM=1           skip the Vim syntax files
#   VIMFILES           Vim runtime directory (default $PREFIX/share/vim/vimfiles)
#   PAXSY_LIBRARY_DIR  package cache (default ~/.paxsy)
#   NO_VERIFY=1        skip `paxsy check std` after installing the library

set -eu

PREFIX="${PREFIX:-$HOME/.local}"
VIMFILES="${VIMFILES:-$PREFIX/share/vim/vimfiles}"
LIBDIR="${PAXSY_LIBRARY_DIR:-$HOME/.paxsy}"
HERE="$(cd "$(dirname "$0")/.." && pwd)"
LIBS_ONLY=0

for arg in "$@"; do
    case "$arg" in
        --libs-only) LIBS_ONLY=1 ;;
        *) printf 'unknown option: %s\n' "$arg" >&2; exit 2 ;;
    esac
done

say()  { printf '\033[92m==>\033[0m %s\n' "$*"; }
die()  { printf '\033[91m==>\033[0m %s\n' "$*" >&2; exit 1; }

[ -d "$HERE/std" ] || die "no std/ package directory next to this script"

# Copies a package directory (one holding a *.toml manifest) into the
# package cache, replacing any previous copy.
install_package() {
    pkgdir="$1"
    name="$(basename "$pkgdir")"
    if ! ls "$pkgdir"/*.toml >/dev/null 2>&1; then
        die "'$pkgdir' holds no package manifest"
    fi
    mkdir -p "$LIBDIR/installed"
    rm -rf "$LIBDIR/installed/$name"
    cp -r "$pkgdir" "$LIBDIR/installed/$name"
    printf 'installed library %s (local)\n' "$name"
}

# Installs every package shipped in the repository: the `std/` tree
# and anything placed under `pkgs/`.
install_libraries() {
    install_package "$HERE/std"
    if [ -d "$HERE/pkgs" ]; then
        for pkg in "$HERE"/pkgs/*/; do
            [ -d "$pkg" ] && install_package "$pkg"
        done
    fi
    # Refresh the index when a usable binary is around; offline this
    # falls back to scanning the local cache, which is fine.
    PAXSY="$PREFIX/bin/paxsy"
    [ -x "$PAXSY" ] || PAXSY="$HERE/paxsy"
    if [ -x "$PAXSY" ]; then
        PAXSY_LIBRARY_DIR="$LIBDIR" "$PAXSY" fetch || true
        if [ "${NO_VERIFY:-0}" != "1" ]; then
            PAXSY_LIBRARY_DIR="$LIBDIR" "$PAXSY" check "$HERE/std" \
                || die "the standard library does not pass 'paxsy check std'"
        fi
    fi
}

if [ "$LIBS_ONLY" = "1" ]; then
    install_libraries
    exit 0
fi

[ -f "$HERE/Makefile" ] || die "run this script from a source checkout"

say "building from $HERE"
make -C "$HERE" >/dev/null

say "installing paxsy to $PREFIX/bin"
install -Dm755 "$HERE/paxsy" "$PREFIX/bin/paxsy"

if [ "${NO_VIM:-0}" != "1" ]; then
    say "installing Vim syntax files to $VIMFILES"
    install -Dm644 "$HERE/highlight/vim/ftdetect/px.vim" \
        "$VIMFILES/ftdetect/px.vim"
    install -Dm644 "$HERE/highlight/vim/syntax/px.vim" \
        "$VIMFILES/syntax/px.vim"
fi

say "installing the standard library"
install_libraries

say "done"
"$PREFIX/bin/paxsy" --version
printf 'export PATH="%s/bin:$PATH"\n' "$PREFIX"
