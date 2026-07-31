#!/data/data/com.termux/files/usr/bin/bash
# Real post-install smoke test for every catalog manifest: serves catalog.db + payloads over
# a local HTTP server, installs each package for real through winget, runs `<alias> --version`
# (or --help as fallback), and fails if the command doesn't actually execute. This is the gate
# that should run before merging a new/changed catalog/*.yaml -- "downloaded and hashed
# correctly" is not the bar, running for real is.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CAT="$ROOT/catalog"
PORT=8956
FAILS=0

[ -x "$ROOT/winget_real_cli" ] || { echo "FAIL: $ROOT/winget_real_cli not built -- run ./build.sh first"; exit 1; }
command -v winget > /dev/null || { echo "FAIL: winget not on PATH -- run ./build.sh first"; exit 1; }

(cd "$CAT" && nohup python3 -m http.server "$PORT" > /dev/null 2>&1 &)
sleep 1
winget source add verify-catalog "http://127.0.0.1:$PORT/catalog.db" > /dev/null 2>&1

for manifest in "$CAT"/*.yaml; do
    id=$(grep '^PackageIdentifier:' "$manifest" | head -1 | awk '{print $2}')
    alias=$(grep '^Moniker:' "$manifest" | head -1 | awk '{print $2}')
    [ -z "$id" ] || [ -z "$alias" ] && { echo "FAIL: $manifest missing PackageIdentifier or Moniker"; FAILS=$((FAILS+1)); continue; }

    winget install "$id" > /dev/null 2>&1
    out=$("$alias" --version 2>&1) || out=$("$alias" --help 2>&1)
    if echo "$out" | grep -qiE "[0-9]+\.[0-9]+|$alias"; then
        echo "PASS: $id -> $alias: $(echo "$out" | head -1)"
    else
        echo "FAIL: $id -> $alias produced no recognizable output: $out"
        FAILS=$((FAILS+1))
    fi
    winget uninstall "$id" > /dev/null 2>&1
done

winget source remove verify-catalog > /dev/null 2>&1
pkill -f "http.server $PORT" > /dev/null 2>&1

if [ "$FAILS" -eq 0 ]; then
    echo "ALL CATALOG PACKAGES VERIFIED"
    exit 0
else
    echo "$FAILS CATALOG PACKAGE(S) FAILED"
    exit 1
fi
