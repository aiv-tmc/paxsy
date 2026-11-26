# Maintainer: Paxsy contributors <team@paxsy.org>
# PKGBUILD for the Paxsy compiler (Arch Linux / AUR).
# Builds straight from the git repository and installs the binary,
# the Paxsy-written standard library, the manual page source
# (docs/ToolchainUserManual.md) and the Vim support files. The compiler emits x86-64
# machine code in-process and needs no runtime archive, so none is
# packaged.

pkgname=paxsy
pkgver=1.0.0
pkgrel=1
pkgdesc="Compiler for the Paxsy systems programming language"
arch=("x86_64" "aarch64")
url="https://github.com/aiv-tmc/paxsy"
license=("Apache-2.0")
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
    install -Dm644 "$srcdir/paxsy/docs/ToolchainUserManual.md" \
        "$pkgdir/usr/share/doc/paxsy/ToolchainUserManual.md"
    install -Dm644 "$srcdir/paxsy/docs/LanguageSpecification.md" \
        "$pkgdir/usr/share/doc/paxsy/LanguageSpecification.md"
    install -Dm644 "$srcdir/paxsy/docs/LanguageDeveloperGuide.md" \
        "$pkgdir/usr/share/doc/paxsy/LanguageDeveloperGuide.md"
    install -Dm644 "$srcdir/paxsy/docs/LLMIntegrationGuide.md" \
        "$pkgdir/usr/share/doc/paxsy/LLMIntegrationGuide.md"
    install -Dm644 "$srcdir/paxsy/NOTICE" \
        "$pkgdir/usr/share/doc/paxsy/NOTICE"
    install -Dm644 "$srcdir/paxsy/LICENSE" \
        "$pkgdir/usr/share/licenses/paxsy/LICENSE"
    install -Dm644 "$srcdir/paxsy/editors/vim/ftdetect/px.vim" \
        "$pkgdir/usr/share/vim/vimfiles/ftdetect/px.vim"
    install -Dm644 "$srcdir/paxsy/editors/vim/syntax/px.vim" \
        "$pkgdir/usr/share/vim/vimfiles/syntax/px.vim"
}
