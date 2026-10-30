# Maintainer: Paxsy contributors <team@paxsy.org>
# PKGBUILD for the paxsy frontend toolchain (Arch Linux / AUR).
# Builds straight from the git repository and installs the binary,
# the manual page source (docs/cli.md) and the Vim support files.

pkgname=paxsy
pkgver=1.0.0
pkgrel=1
pkgdesc="Frontend toolchain for the Paxsy systems programming language"
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
    install -Dm644 "$srcdir/paxsy/docs/cli.md" \
        "$pkgdir/usr/share/doc/paxsy/cli.md"
    install -Dm644 "$srcdir/paxsy/editors/vim/ftdetect/px.vim" \
        "$pkgdir/usr/share/vim/vimfiles/ftdetect/px.vim"
    install -Dm644 "$srcdir/paxsy/editors/vim/syntax/px.vim" \
        "$pkgdir/usr/share/vim/vimfiles/syntax/px.vim"
    install -Dm644 "$srcdir/paxsy/install.sh" \
        "$pkgdir/usr/share/paxsy/install.sh"
}
