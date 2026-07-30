#!/data/data/com.termux/files/usr/bin/bash
# Builds a real, installable Termux .deb package from the already-built winget_real_cli.
# Run ./build.sh first. Output: winget-termux_<version>_aarch64.deb, installable via
# `dpkg -i` (or `pkg install ./winget-termux_*.deb`) on any Termux, not just this checkout --
# it carries the compiled binary itself, not a symlink back to this source tree.
set -e

ROOT="$(cd "$(dirname "$0")" && pwd)"
VERSION="1.0.0"
ARCH="aarch64"
PKGDIR="$ROOT/build/pkgroot"
PREFIX="${PREFIX:-/data/data/com.termux/files/usr}"

[ -x "$ROOT/winget_real_cli" ] || { echo "winget_real_cli not built -- run ./build.sh first"; exit 1; }

echo "[1/3] Staging package tree..."
rm -rf "$PKGDIR"
mkdir -p "$PKGDIR/DEBIAN" "$PKGDIR$PREFIX/bin"
chmod 755 "$PKGDIR/DEBIAN"
cp "$ROOT/winget_real_cli" "$PKGDIR$PREFIX/bin/winget_real_cli"
chmod 755 "$PKGDIR$PREFIX/bin/winget_real_cli"
ln -s winget_real_cli "$PKGDIR$PREFIX/bin/winget"

cat > "$PKGDIR/DEBIAN/control" <<EOF
Package: winget-termux
Version: $VERSION
Architecture: $ARCH
Maintainer: winget-termux
Depends: sqlite, libyaml, jsoncpp, libicu, openssl, libcurl, zlib, unzip
Section: utils
Priority: optional
Homepage: https://github.com/microsoft/winget-cli
Description: Native ARM64/bionic port of a real subset of winget-cli
 Real Portable/Zip/Script installer support, real remote source add/update,
 no proot/chroot/root/Wine/emulation. See README.md and docs/ARCHITECTURE.md
 in the source checkout for the full technical writeup.
EOF

echo "[2/3] Building .deb..."
DEB="$ROOT/winget-termux_${VERSION}_${ARCH}.deb"
dpkg-deb --root-owner-group -b "$PKGDIR" "$DEB"

echo "[3/3] Done: $DEB"
echo "Install with: dpkg -i $DEB   (or: pkg install $DEB)"
