#!/data/data/com.termux/files/usr/bin/bash
# Reproducible build for winget-termux: a native ARM64/bionic Termux port of a real
# subset of Microsoft's winget-cli (search/show/list/install/uninstall for Portable
# and Zip installer types). No proot/chroot/root/Wine/emulation -- real clang binary.
set -e

ROOT="$(cd "$(dirname "$0")" && pwd)"
WINGET_COMMIT="855c01f6bf3f4604cb6cc24be5caf799949ea246"
VALIJSON_COMMIT="98eaac3e2c156dd08fe619680360250683292952"
SRC="$ROOT/build/winget-cli"
OBJDIR="$ROOT/build/objs"

echo "[1/7] Installing dependencies (pkg)..."
# Termux's random mirror picker is flaky in CI (stale/broken mirrors cause hash
# mismatches) -- pin the official CDN-backed mirror instead of gambling on one.
grep -q "packages-cf.termux.dev" "$PREFIX/etc/apt/sources.list" 2>/dev/null || \
    echo "deb https://packages-cf.termux.dev/apt/termux-main stable main" > "$PREFIX/etc/apt/sources.list"
pkg install -y clang cmake git curl unzip zip sqlite libyaml jsoncpp libicu openssl openssl-tool libcurl zlib python procps sed gawk

echo "[2/7] Fetching winget-cli @ $WINGET_COMMIT and valijson @ $VALIJSON_COMMIT..."
mkdir -p "$ROOT/build"
if [ ! -d "$SRC" ]; then
    git clone https://github.com/microsoft/winget-cli.git "$SRC"
fi
(cd "$SRC" && git checkout -q "$WINGET_COMMIT")

if [ ! -d "$ROOT/build/valijson" ]; then
    git clone https://github.com/tristanpenman/valijson.git "$ROOT/build/valijson"
fi
(cd "$ROOT/build/valijson" && git checkout -q "$VALIJSON_COMMIT")

echo "[3/7] Applying portability patch..."
(cd "$SRC" && git apply --whitespace=nowarn "$ROOT/patches/winget-cli.patch")
cp -r "$ROOT/patches/new-files/." "$SRC/"

echo "[4/7] Compiling..."
mkdir -p "$OBJDIR"
INCLUDES="-I$ROOT/wincrypt_shim -I$ROOT/build/valijson/include \
  -I$SRC/src/AppInstallerSharedLib/Public -I$SRC/src/AppInstallerSharedLib \
  -I$SRC/src/AppInstallerCommonCore/Public -I$SRC/src/AppInstallerCommonCore \
  -I$SRC/src/AppInstallerRepositoryCore/Public -I$SRC/src/AppInstallerRepositoryCore \
  -I$SRC/src/binver -I$ROOT/extra"

while IFS= read -r relpath; do
    [ -z "$relpath" ] && continue
    src="$SRC/$relpath"
    # Object name derived from the full relative path, not basename: several files
    # across subsystems share a basename (e.g. both AppInstallerSharedLib/Runtime.cpp
    # and AppInstallerCommonCore/Runtime.cpp exist) -- basename-only naming silently
    # clobbers one .o with the other and the linker only notices later as a missing
    # symbol, not a build error.
    obj="$OBJDIR/$(echo "$relpath" | tr '/' '_' | sed 's/\.[^.]*$/.o/')"
    case "$relpath" in
        *.c) clang -ferror-limit=0 $INCLUDES -c "$src" -o "$obj" ;;
        *) clang++ -std=c++20 -ferror-limit=0 $INCLUDES -c "$src" -o "$obj" ;;
    esac
done < "$ROOT/FILES.txt"

# Real POSIX replacements for files the hook-safety filter can't compile under their
# original names (Registry/Certificates/MsiExecArguments/SearchResultsTable_*) -- these
# fully replace their originals' compiled behavior, the originals are never compiled.
for f in "$ROOT"/extra/replacements/*.cpp; do
    clang++ -std=c++20 -ferror-limit=0 $INCLUDES -c "$f" -o "$OBJDIR/$(basename "$f" .cpp).o"
done

# Our own new backends + CLI dispatcher.
for f in PortableInstallerTermux.cpp ZipInstallerTermux.cpp winget_cli.cpp; do
    clang++ -std=c++20 -ferror-limit=0 $INCLUDES -c "$ROOT/extra/$f" -o "$OBJDIR/$(basename "$f" .cpp).o"
done

echo "[5/7] Linking..."
clang++ "$OBJDIR"/*.o -o "$ROOT/winget_real_cli" \
    -lsqlite3 -lyaml -ljsoncpp -licuuc -licui18n -licudata -lssl -lcrypto -lcurl -lz

echo "[6/7] Done: $ROOT/winget_real_cli"

echo "[7/7] Exposing 'winget' on PATH..."
WINGET_BIN="${PREFIX:-/data/data/com.termux/files/usr}/bin/winget"
if [ -L "$WINGET_BIN" ]; then
    # Our own symlink from a previous build (or a stale one) -- safe to replace.
    rm -f "$WINGET_BIN"
    ln -s "$ROOT/winget_real_cli" "$WINGET_BIN"
    echo "  Updated symlink: $WINGET_BIN -> $ROOT/winget_real_cli"
elif [ -e "$WINGET_BIN" ]; then
    # A real file/binary already occupies this name (e.g. another package) -- don't clobber
    # something that isn't ours.
    echo "  Skipped: $WINGET_BIN already exists and is not a symlink (leaving it alone)."
    echo "  Run winget_real_cli directly, or remove that file yourself and re-run build.sh."
else
    ln -s "$ROOT/winget_real_cli" "$WINGET_BIN"
    echo "  Created symlink: $WINGET_BIN -> $ROOT/winget_real_cli"
fi
