# Maintainer: Paxsy contributors <team@paxsy.org>
# PKGBUILD for the Paxsy compiler (Arch Linux / AUR).
# Builds straight from the git repository and installs the binary,
# the Paxsy-written standard library, the manual page source
# (docs/cli.md) and the Vim support files. The compiler emits x86-64
# machine code in-process and needs no runtime archive, so none is
# packaged.

pkgname=paxsy
pkgver=1.0.0
pkgrel=1
pkgdesc="Compiler for the Paxsy systems programming language"
arch=("x86_64" "aarch64")
url="https://github.com/aiv-tmc/paxsy"
license=("MIT")
depends=("glibc")
makedepends=("git" "make")
source=("git+$url.git")
sha256sums=("SKIP")

build() {
    make -C "$srcdir/paxsy"
}

package() {
    install -Dm755 "$srcdir/paxsy/paxsy" "$pkgdir/usr/bin/paxsy"
    mkdir -p "$pkgdir/usr/lib/paxsy"
    cp -r "$srcdir/paxsy/lib/std" "$pkgdir/usr/lib/paxsy/std"
    install -Dm644 "$srcdir/paxsy/docs/cli.md" \
        "$pkgdir/usr/share/doc/paxsy/cli.md"
    install -Dm644 "$srcdir/paxsy/editors/vim/ftdetect/px.vim" \
        "$pkgdir/usr/share/vim/vimfiles/ftdetect/px.vim"
    install -Dm644 "$srcdir/paxsy/editors/vim/syntax/px.vim" \
        "$pkgdir/usr/share/vim/vimfiles/syntax/px.vim"
}
