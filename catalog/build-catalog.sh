#!/data/data/com.termux/files/usr/bin/bash
# Build catalog.db from YAML manifests using the real SQLiteIndex::CreateNew + AddManifest

set -e

ROOT="$(cd "$(dirname "$0")" && pwd)"
WINGET_ROOT="$(dirname "$ROOT")"
SRC="$WINGET_ROOT/build/winget-cli"
OBJDIR="$WINGET_ROOT/build/objs"

echo "[1/3] Compiling build-catalog.cpp..."

INCLUDES="-I$WINGET_ROOT/wincrypt_shim -I$WINGET_ROOT/build/valijson/include \
  -I$SRC/src/AppInstallerSharedLib/Public -I$SRC/src/AppInstallerSharedLib \
  -I$SRC/src/AppInstallerCommonCore/Public -I$SRC/src/AppInstallerCommonCore \
  -I$SRC/src/AppInstallerRepositoryCore/Public -I$SRC/src/AppInstallerRepositoryCore \
  -I$SRC/src/binver -I$WINGET_ROOT/extra"

clang++ -std=c++20 -ferror-limit=0 $INCLUDES -c "$ROOT/build-catalog.cpp" -o "$ROOT/build-catalog.o"

echo "[2/3] Linking..."

clang++ "$ROOT/build-catalog.o" $(ls "$OBJDIR"/*.o | grep -v winget_cli.o) \
    -o "$ROOT/build-catalog" -lsqlite3 -lyaml -ljsoncpp -licuuc -licui18n -licudata -lssl -lcrypto -lcurl -lz

echo "[3/3] Building catalog.db from manifests..."

"$ROOT/build-catalog" "$ROOT/catalog.db" "$ROOT"

echo "Done! catalog.db created at $ROOT/catalog.db"