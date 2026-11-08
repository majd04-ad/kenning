#!/bin/bash
# SPDX-FileCopyrightText: 2026 Rishon Jonathan R
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Makes packaging/aur/kenning (the tagged release) and kenning-git (master) from the PKGBUILD;
# each is one AUR repository. Run after tagging and pushing a release.
set -eu
cd "$(dirname "$0")/../.."
version=$(sed -n 's/^pkgver=//p' PKGBUILD)
sum=$(curl -fsSL "https://github.com/RishonDev/kenning/archive/refs/tags/v$version.tar.gz" | sha256sum | cut -d' ' -f1)
mkdir -p packaging/aur/kenning packaging/aur/kenning-git

python3 -I - "$sum" << 'PY'
import sys
release_sum = sys.argv[1]
original = open("PKGBUILD").read()

def make(git):
    header, _, body = original.partition("\npkgbase=")
    s = ("# Maintainer: Rishon Jonathan R <mithicalaviator85@gmail.com>\n"
         "# Generated from the Kenning repository's PKGBUILD by packaging/aur/update.sh\n"
         + header + "\npkgbase=" + body)
    def once(old, new):
        nonlocal s
        assert s.count(old) == 1, old
        s = s.replace(old, new)
    source = "$srcdir/kenning" if git else "$srcdir/kenning-$pkgver"
    if git:
        once("source=(", 'source=("kenning::git+https://github.com/RishonDev/kenning.git"\n        ')
        once("sha256sums=(", "sha256sums=('SKIP'\n            ")
        once("makedepends=(", "makedepends=(git ")
        once("pkgbase=kenning\n", "pkgbase=kenning-git\n")
        once("pkgname=(kenning plasma-login-manager-kenning)", "pkgname=(kenning-git plasma-login-manager-kenning-git)")
        once("\nprepare() {", """
pkgver() {
  cd kenning
  git describe --long --tags | sed 's/^v//; s/\\([^-]*-g\\)/r\\1/; s/-/./g'
}

prepare() {""")
        once("package_kenning() {", "package_kenning-git() {\n  provides=(\"kenning=${pkgver%%.r*}\")")
        once("  conflicts=(plasma-faceunlock)", "  conflicts=(kenning plasma-faceunlock)")
        # The hooks name the package they belong to: without this, upgrading kenning-git wouldn't
        # redo the lock screen add-on, and removing it wouldn't put the stock lock screen back.
        once('  install -Dm644 -t "$pkgdir/usr/share/libalpm/hooks" "$startdir"/packaging/arch/*.hook\n',
             '  local hook\n'
             '  for hook in "$startdir"/packaging/arch/*.hook; do\n'
             '    sed "s/^Target = kenning$/Target = kenning-git/" "$hook" | install -Dm644 /dev/stdin "$pkgdir/usr/share/libalpm/hooks/${hook##*/}"\n'
             '  done\n')
        once("package_plasma-login-manager-kenning() {", "package_plasma-login-manager-kenning-git() {")
        once('  provides=("plasma-login-manager=$_plmver")', '  provides=("plasma-login-manager=$_plmver" "plasma-login-manager-kenning=${pkgver%%.r*}")')
        once("  conflicts=(plasma-login-manager plasma-login-manager-faceunlock)",
             "  conflicts=(plasma-login-manager plasma-login-manager-kenning plasma-login-manager-faceunlock)")
    else:
        once("source=(", 'source=("kenning-$pkgver.tar.gz::https://github.com/RishonDev/kenning/archive/refs/tags/v$pkgver.tar.gz"\n        ')
        once("sha256sums=(", f"sha256sums=('{release_sum}'\n            ")
    once("install=packaging/arch/kenning.install", "install=kenning.install")
    s = s.replace('"$startdir"', f'"{source}"').replace("$startdir/", f"{source}/")
    assert "$startdir" not in s, "a $startdir use the AUR build can't follow"
    return s

open("packaging/aur/kenning/PKGBUILD", "w").write(make(False))
open("packaging/aur/kenning-git/PKGBUILD", "w").write(make(True))
PY
for dir in packaging/aur/kenning packaging/aur/kenning-git; do
    cp packaging/arch/kenning.install "$dir/"
    rm -rf "$dir/keys" && cp -r keys "$dir/"
    (cd "$dir" && makepkg --printsrcinfo > .SRCINFO)
done
echo "packaging/aur: kenning $version ($sum) and kenning-git"
