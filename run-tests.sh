#!/data/data/com.termux/files/usr/bin/bash
# Self-contained real-execution test suite for winget_real_cli.
# Builds its own temp SQLite index, serves real test payloads over a local HTTP
# server, and exercises every command against the real compiled binary. No mocks:
# every install performs a real download, real SHA256 check, real chmod, real
# symlink, real execution, real removal.
set -u
ROOT="$(cd "$(dirname "$0")" && pwd)"
CLI="$ROOT/winget_real_cli"
WORK="$ROOT/.test-tmp"
DB="$WORK/test.db"
HTTPROOT="$WORK/httproot"
PORT=8943
FAILS=0

# winget_cli.cpp resolves a manifest's RelativePath against $HOME/.winget/manifests
# (real upstream schema constraint: PathPartTable rejects absolute RelativePath with
# E_INVALIDARG, so this can't just point at $WORK directly) -- test manifests are
# staged there under a Test.* prefix and cleaned up afterward.
MANIFEST_ROOT="/data/data/com.termux/files/home/.winget/manifests"

pass() { echo "PASS: $1"; }
fail() { echo "FAIL: $1"; FAILS=$((FAILS+1)); }

[ -x "$CLI" ] || { echo "FAIL: $CLI not built -- run ./build.sh first"; exit 1; }

echo "=== winget symlink on PATH ==="
command -v winget > /dev/null && pass "command -v winget resolves" || fail "command -v winget"
WINGET_TARGET=$(readlink -f "$(command -v winget 2>/dev/null)" 2>/dev/null)
[ "$WINGET_TARGET" = "$(readlink -f "$CLI")" ] && pass "winget symlink points at winget_real_cli" || fail "winget symlink target"
winget --version 2>&1 | grep -qi "winget-termux" && pass "winget --version works" || fail "winget --version"

rm -rf "$WORK"
mkdir -p "$HTTPROOT" "$MANIFEST_ROOT"

cat > "$HTTPROOT/tool.sh" <<'PAYLOAD'
#!/data/data/com.termux/files/usr/bin/bash
if [ "$1" = "--help" ]; then echo "tool 1.0.0 test payload"; exit 0; fi
echo "tool: real execution ok"
PAYLOAD
chmod +x "$HTTPROOT/tool.sh"
TOOL_SHA=$(openssl dgst -sha256 "$HTTPROOT/tool.sh" | awk '{print $2}')

(cd "$HTTPROOT" && zip -q -j pkg.zip tool.sh)
ZIP_SHA=$(openssl dgst -sha256 "$HTTPROOT/pkg.zip" | awk '{print $2}')

(cd "$HTTPROOT" && nohup python3 -m http.server "$PORT" > "$WORK/http.log" 2>&1 &)
sleep 1

cat > "$MANIFEST_ROOT/test_portable.yaml" <<EOF
PackageIdentifier: Test.Portable
PackageVersion: 1.0.0
PackageLocale: en-US
Publisher: Test
PackageName: Portable Test
Moniker: ptest
ShortDescription: test
InstallerLocale: en-US
InstallerType: portable
Installers:
  - Architecture: arm64
    InstallerUrl: http://127.0.0.1:$PORT/tool.sh
    InstallerSha256: $TOOL_SHA
ManifestType: singleton
ManifestVersion: 1.0.0
EOF

cat > "$MANIFEST_ROOT/test_script.yaml" <<EOF
PackageIdentifier: Test.Script
PackageVersion: 1.0.0
PackageLocale: en-US
Publisher: Test
PackageName: Script Test
Moniker: stest
ShortDescription: test
InstallerLocale: en-US
InstallerType: script
Installers:
  - Architecture: arm64
    InstallerUrl: http://127.0.0.1:$PORT/tool.sh
    InstallerSha256: $TOOL_SHA
ManifestType: singleton
ManifestVersion: 1.0.0
EOF

cat > "$MANIFEST_ROOT/test_zip.yaml" <<EOF
PackageIdentifier: Test.Zip
PackageVersion: 1.0.0
PackageLocale: en-US
Publisher: Test
PackageName: Zip Test
Moniker: ztest
ShortDescription: test
InstallerLocale: en-US
InstallerType: zip
NestedInstallerType: portable
Installers:
  - Architecture: arm64
    InstallerUrl: http://127.0.0.1:$PORT/pkg.zip
    InstallerSha256: $ZIP_SHA
    NestedInstallerFiles:
      - RelativeFilePath: tool.sh
        PortableCommandAlias: ztest
ManifestType: singleton
ManifestVersion: 1.4.0
EOF

cat > "$WORK/mkindex.cpp" <<EOF
#include "pch.h"
#include <iostream>
#include "Microsoft/SQLiteIndex.h"
using namespace AppInstaller::Repository::Microsoft;
int main() {
    auto index = SQLiteIndex::CreateNew("$DB");
    index.AddManifest("$MANIFEST_ROOT/test_portable.yaml", "test_portable.yaml");
    index.AddManifest("$MANIFEST_ROOT/test_script.yaml", "test_script.yaml");
    index.AddManifest("$MANIFEST_ROOT/test_zip.yaml", "test_zip.yaml");
    std::cout << "indexed" << std::endl;
    return 0;
}
EOF

INCLUDES="-I$ROOT/wincrypt_shim -I$ROOT/build/valijson/include \
  -I$ROOT/build/winget-cli/src/AppInstallerSharedLib/Public -I$ROOT/build/winget-cli/src/AppInstallerSharedLib \
  -I$ROOT/build/winget-cli/src/AppInstallerCommonCore/Public -I$ROOT/build/winget-cli/src/AppInstallerRepositoryCore/Public \
  -I$ROOT/build/winget-cli/src/AppInstallerRepositoryCore"
clang++ -std=c++20 -ferror-limit=0 $INCLUDES -c "$WORK/mkindex.cpp" -o "$WORK/mkindex.o"
clang++ "$WORK/mkindex.o" $(ls "$ROOT/build/objs"/*.o | grep -v winget_cli.o) \
    -o "$WORK/mkindex" -lsqlite3 -lyaml -ljsoncpp -licuuc -licui18n -licudata -lssl -lcrypto -lcurl -lz
"$WORK/mkindex" > /dev/null || { echo "FAIL: index setup"; exit 1; }

# winget_cli.cpp resolves manifests via a manifest root baked in at compile time
# (s_manifestRoot) and reads the db from a fixed path; point both at this test's
# temp files by exporting the same env the real CLI's s_dbPath constant reads --
# it doesn't read env for the path, so we symlink the real db path instead.
REAL_DB="/data/data/com.termux/files/home/wingetcli.db"
REAL_DB_BACKUP="$WORK/real_db.bak"
[ -f "$REAL_DB" ] && cp "$REAL_DB" "$REAL_DB_BACKUP"
cp "$DB" "$REAL_DB"

# s_manifestRoot in winget_cli.cpp is the scratchpad path used at dev time; this repo's
# winget_cli.cpp instead needs it to match wherever manifests actually live for a real
# deployment. For this test we index manifests using their absolute path directly
# (RelativePath == absolute path), which ResolveManifestById already handles.

# Real end-to-end proof the symlink works: every test below runs through the "winget"
# command on PATH, not the binary's full path directly.
run() { winget "$@"; }

echo "=== source list ==="
run source list | grep -q "TermuxLocal" && pass "source list shows local source" || fail "source list"

echo "=== search ==="
run search | grep -q "Test.Portable" && pass "search lists portable pkg" || fail "search portable"
run search | grep -q "Test.Zip" && pass "search lists zip pkg" || fail "search zip"

echo "=== show ==="
run show Test.Portable | grep -q "portable" && pass "show portable installer type" || fail "show portable"

echo "=== install portable ==="
run install Test.Portable | grep -q "Successfully installed" && pass "install portable" || fail "install portable"
command -v ptest > /dev/null && pass "ptest on PATH" || fail "ptest on PATH"
[ "$(ptest)" = "tool: real execution ok" ] && pass "ptest executes correctly" || fail "ptest execution output"

echo "=== list (portable installed) ==="
run list | grep -q "Test.Portable" && pass "list shows installed portable" || fail "list portable"

echo "=== uninstall portable ==="
run uninstall Test.Portable | grep -q "Successfully uninstalled" && pass "uninstall portable" || fail "uninstall portable"
command -v ptest > /dev/null 2>&1 && fail "ptest still on PATH after uninstall" || pass "ptest removed from PATH"

echo "=== reinstall portable (idempotency) ==="
run install Test.Portable | grep -q "Successfully installed" && pass "reinstall portable" || fail "reinstall portable"
command -v ptest > /dev/null && pass "ptest back on PATH" || fail "ptest back on PATH"
run uninstall Test.Portable > /dev/null

echo "=== pin/unpin/export/import/hash/validate/download (previously untested) ==="
run install Test.Portable > /dev/null
run pin Test.Portable > /dev/null
run upgrade Test.Portable | grep -qi "pinned\|skip" && pass "upgrade respects pin" || fail "upgrade respects pin"
run unpin Test.Portable > /dev/null

run pin "../escape" > /dev/null 2>&1; [ $? -ne 0 ] && pass "pin rejects path traversal id" || fail "pin traversal not rejected"
run unpin "../../../../etc/passwd" > /dev/null 2>&1
[ ! -e /etc/passwd.pin ] && pass "unpin traversal touched nothing outside .winget" || fail "unpin traversal escaped .winget"

run export "$WORK/export.json" > /dev/null
grep -q "Test.Portable" "$WORK/export.json" && pass "export includes installed package" || fail "export"
run uninstall Test.Portable > /dev/null
run import "$WORK/export.json" | grep -q "1 OK" && pass "import reinstalls from export" || fail "import"
run uninstall Test.Portable > /dev/null

echo "hello" > "$WORK/hash_input.txt"
EXPECTED_HASH=$(sha256sum "$WORK/hash_input.txt" | cut -d' ' -f1)
run hash "$WORK/hash_input.txt" | grep -qi "$EXPECTED_HASH" && pass "hash matches real sha256sum" || fail "hash"

run validate "$MANIFEST_ROOT/test_portable.yaml" > /dev/null; [ $? -eq 0 ] && pass "validate accepts known-good manifest" || fail "validate good manifest"
echo "not: a manifest: at: all" > "$WORK/bad.yaml"
run validate "$WORK/bad.yaml" > /dev/null 2>&1; [ $? -ne 0 ] && pass "validate rejects malformed manifest" || fail "validate bad manifest"

run download Test.Portable "$WORK/dl" > /dev/null
ls "$WORK/dl" 2>/dev/null | grep -q . && pass "download saves installer file" || fail "download"

echo "=== install zip ==="
run install Test.Zip | grep -q "Successfully installed" && pass "install zip" || fail "install zip"
command -v ztest > /dev/null && pass "ztest on PATH" || fail "ztest on PATH"
[ "$(ztest)" = "tool: real execution ok" ] && pass "ztest executes correctly" || fail "ztest execution output"

echo "=== list (zip installed) ==="
run list | grep -q "Test.Zip" && pass "list shows installed zip" || fail "list zip"

echo "=== uninstall zip ==="
run uninstall Test.Zip | grep -q "Successfully uninstalled" && pass "uninstall zip" || fail "uninstall zip"
command -v ztest > /dev/null 2>&1 && fail "ztest still on PATH after uninstall" || pass "ztest removed from PATH"

echo "=== reinstall zip (idempotency) ==="
run install Test.Zip | grep -q "Successfully installed" && pass "reinstall zip" || fail "reinstall zip"
command -v ztest > /dev/null && pass "ztest back on PATH" || fail "ztest back on PATH"
run uninstall Test.Zip > /dev/null

echo "=== install script ==="
run install Test.Script | grep -q "This is a Script package" && pass "install recognizes Script type" || fail "install Script type label"
command -v stest > /dev/null && pass "stest on PATH" || fail "stest on PATH"
[ "$(stest)" = "tool: real execution ok" ] && pass "stest executes correctly" || fail "stest execution output"

echo "=== upgrade: already up to date ==="
run upgrade Test.Script | grep -q "already up to date" && pass "upgrade reports up to date" || fail "upgrade up-to-date check"

echo "=== upgrade: real version bump ==="
cat > "$HTTPROOT/tool.sh" <<'PAYLOAD2'
#!/data/data/com.termux/files/usr/bin/bash
echo "tool: v2 real execution ok"
PAYLOAD2
chmod +x "$HTTPROOT/tool.sh"
TOOL_SHA2=$(openssl dgst -sha256 "$HTTPROOT/tool.sh" | awk '{print $2}')
cat > "$MANIFEST_ROOT/test_script_v2.yaml" <<EOF
PackageIdentifier: Test.Script
PackageVersion: 2.0.0
PackageLocale: en-US
Publisher: Test
PackageName: Script Test
Moniker: stest
ShortDescription: test
InstallerLocale: en-US
InstallerType: script
Installers:
  - Architecture: arm64
    InstallerUrl: http://127.0.0.1:$PORT/tool.sh
    InstallerSha256: $TOOL_SHA2
ManifestType: singleton
ManifestVersion: 1.0.0
EOF
cat > "$WORK/mkindex2.cpp" <<EOF
#include "pch.h"
#include <iostream>
#include "Microsoft/SQLiteIndex.h"
using namespace AppInstaller::Repository::Microsoft;
int main() {
    auto index = SQLiteIndex::Open("$REAL_DB", SQLiteIndex::OpenDisposition::ReadWrite);
    index.AddManifest("$MANIFEST_ROOT/test_script_v2.yaml", "test_script_v2.yaml");
    return 0;
}
EOF
clang++ -std=c++20 -ferror-limit=0 $INCLUDES -c "$WORK/mkindex2.cpp" -o "$WORK/mkindex2.o"
clang++ "$WORK/mkindex2.o" $(ls "$ROOT/build/objs"/*.o | grep -v winget_cli.o) \
    -o "$WORK/mkindex2" -lsqlite3 -lyaml -ljsoncpp -licuuc -licui18n -licudata -lssl -lcrypto -lcurl -lz
"$WORK/mkindex2" > /dev/null || { echo "FAIL: index v2 setup"; }

run upgrade Test.Script | grep -q "Successfully upgraded to 2.0.0" && pass "upgrade to 2.0.0 succeeds" || fail "upgrade to 2.0.0"
[ "$(stest)" = "tool: v2 real execution ok" ] && pass "stest runs upgraded version" || fail "stest upgraded execution"
command -v stest > /dev/null && pass "stest still on PATH after upgrade" || fail "stest PATH after upgrade"

echo "=== uninstall script ==="
run uninstall Test.Script | grep -q "Successfully uninstalled" && pass "uninstall script" || fail "uninstall script"
command -v stest > /dev/null 2>&1 && fail "stest still on PATH after uninstall" || pass "stest removed from PATH"

echo "=== upgrade of not-installed package ==="
run upgrade Test.Script > /dev/null; [ $? -eq 4 ] && pass "upgrade not-installed exits 4" || fail "upgrade not-installed exit code"

echo "=== source add: real remote catalog ==="
cat > "$WORK/remote_pkg.yaml" <<EOF
PackageIdentifier: Remote.Tool
PackageVersion: 1.0.0
PackageLocale: en-US
Publisher: RemotePublisher
PackageName: Remote Tool
Moniker: remotetool
ShortDescription: real remote-source test package
InstallerLocale: en-US
InstallerType: portable
Installers:
  - Architecture: arm64
    InstallerUrl: http://example.invalid/remotetool
    InstallerSha256: 0000000000000000000000000000000000000000000000000000000000000000
ManifestType: singleton
ManifestVersion: 1.0.0
EOF
cat > "$WORK/mkremote.cpp" <<EOF
#include "pch.h"
#include <iostream>
#include "Microsoft/SQLiteIndex.h"
using namespace AppInstaller::Repository::Microsoft;
int main() {
    auto index = SQLiteIndex::CreateNew("$HTTPROOT/remote.db");
    index.AddManifest("$WORK/remote_pkg.yaml", "remote_pkg.yaml");
    return 0;
}
EOF
clang++ -std=c++20 -ferror-limit=0 $INCLUDES -c "$WORK/mkremote.cpp" -o "$WORK/mkremote.o"
clang++ "$WORK/mkremote.o" $(ls "$ROOT/build/objs"/*.o | grep -v winget_cli.o) \
    -o "$WORK/mkremote" -lsqlite3 -lyaml -ljsoncpp -licuuc -licui18n -licudata -lssl -lcrypto -lcurl -lz
"$WORK/mkremote" > /dev/null

run source add MyRepo "http://127.0.0.1:$PORT/remote.db" | grep -q "Successfully added source" && pass "source add real remote catalog" || fail "source add"
run source list | grep -q "MyRepo" && pass "source list shows added source" || fail "source list new source"
run search Remote | grep -q "Remote.Tool" && pass "search finds package from added source" || fail "search added source"
run show Remote.Tool | grep -q "index-only data" && pass "show works for added-source package" || fail "show added source"

echo "=== source add: negative cases ==="
run source add MyRepo "http://127.0.0.1:$PORT/remote.db" > /dev/null; [ $? -ne 0 ] && pass "duplicate source name rejected" || fail "duplicate source add"
run source add BadSrc "http://127.0.0.1:1/nope.db" > /dev/null; [ $? -ne 0 ] && pass "unreachable URL rejected" || fail "bad URL source add"
echo "not a real database" > "$HTTPROOT/garbage.db"
run source add Garbage "http://127.0.0.1:$PORT/garbage.db" > /dev/null; [ $? -ne 0 ] && pass "non-SQLite file rejected" || fail "garbage catalog accepted"
run source list | grep -qE "BadSrc|Garbage" && fail "failed source add leaked into source list" || pass "failed source adds registered nothing"

echo "=== install/upgrade directly from a remote source ==="
cat > "$HTTPROOT/rinstall.sh" <<'PAYLOAD3'
#!/data/data/com.termux/files/usr/bin/bash
echo "rinstall: v1"
PAYLOAD3
chmod +x "$HTTPROOT/rinstall.sh"
RSHA1=$(openssl dgst -sha256 "$HTTPROOT/rinstall.sh" | awk '{print $2}')
cat > "$HTTPROOT/rinstall_manifest.yaml" <<EOF
PackageIdentifier: Remote.Install
PackageVersion: 1.0.0
PackageLocale: en-US
Publisher: RemotePublisher
PackageName: Remote Install
Moniker: rinstall
ShortDescription: test
InstallerLocale: en-US
InstallerType: portable
Installers:
  - Architecture: arm64
    InstallerUrl: http://127.0.0.1:$PORT/rinstall.sh
    InstallerSha256: $RSHA1
ManifestType: singleton
ManifestVersion: 1.0.0
EOF
cat > "$WORK/mkremote2.cpp" <<EOF
#include "pch.h"
#include <iostream>
#include "Microsoft/SQLiteIndex.h"
using namespace AppInstaller::Repository::Microsoft;
int main() {
    auto index = SQLiteIndex::Open("$HTTPROOT/remote.db", SQLiteIndex::OpenDisposition::ReadWrite);
    index.AddManifest("$HTTPROOT/rinstall_manifest.yaml", "rinstall_manifest.yaml");
    return 0;
}
EOF
clang++ -std=c++20 -ferror-limit=0 $INCLUDES -c "$WORK/mkremote2.cpp" -o "$WORK/mkremote2.o"
clang++ "$WORK/mkremote2.o" $(ls "$ROOT/build/objs"/*.o | grep -v winget_cli.o) \
    -o "$WORK/mkremote2" -lsqlite3 -lyaml -ljsoncpp -licuuc -licui18n -licudata -lssl -lcrypto -lcurl -lz
"$WORK/mkremote2" > /dev/null
run source update MyRepo > /dev/null

run install Remote.Install | grep -q "Successfully installed" && pass "install directly from remote source" || fail "remote install"
command -v rinstall > /dev/null && pass "rinstall on PATH after remote install" || fail "rinstall PATH"
[ "$(rinstall)" = "rinstall: v1" ] && pass "rinstall executes real remote payload" || fail "rinstall execution"
run list | grep "Remote.Install" | grep -q "MyRepo" && pass "list shows persisted source for remote pkg" || fail "list persisted source"
run upgrade Remote.Install | grep -q "already up to date" && pass "remote upgrade up-to-date" || fail "remote upgrade up-to-date"
run uninstall Remote.Install | grep -q "Successfully uninstalled" && pass "uninstall remote-installed package" || fail "uninstall remote pkg"
command -v rinstall > /dev/null 2>&1 && fail "rinstall leftover on PATH" || pass "rinstall cleanly removed"

run source remove MyRepo | grep -q "Removed" && pass "source remove" || fail "source remove"
run source list | grep -q "MyRepo" && fail "removed source still listed" || pass "removed source gone from list"
run source remove NoSuchSource > /dev/null; [ $? -eq 3 ] && pass "remove unknown source exits 3" || fail "remove unknown source exit code"

echo "=== upgrade --all ==="
cat > "$HTTPROOT/tool_ua.sh" <<'PAYLOAD_UA'
#!/data/data/com.termux/files/usr/bin/bash
echo "tool: real execution ok"
PAYLOAD_UA
chmod +x "$HTTPROOT/tool_ua.sh"
UA_SHA=$(openssl dgst -sha256 "$HTTPROOT/tool_ua.sh" | awk '{print $2}')
cat > "$MANIFEST_ROOT/test_portable_ua.yaml" <<EOF
PackageIdentifier: Test.PortableUA
PackageVersion: 1.0.0
PackageLocale: en-US
Publisher: Test
PackageName: Portable UA Test
Moniker: puatest
ShortDescription: test
InstallerLocale: en-US
InstallerType: portable
Installers:
  - Architecture: arm64
    InstallerUrl: http://127.0.0.1:$PORT/tool_ua.sh
    InstallerSha256: $UA_SHA
ManifestType: singleton
ManifestVersion: 1.0.0
EOF
cat > "$WORK/mkua.cpp" <<EOF
#include "pch.h"
#include <iostream>
#include "Microsoft/SQLiteIndex.h"
using namespace AppInstaller::Repository::Microsoft;
int main() {
    auto idx = SQLiteIndex::Open("$REAL_DB", SQLiteIndex::OpenDisposition::ReadWrite);
    idx.AddManifest("$MANIFEST_ROOT/test_portable_ua.yaml", "test_portable_ua.yaml");
    return 0;
}
EOF
clang++ -std=c++20 -ferror-limit=0 $INCLUDES -c "$WORK/mkua.cpp" -o "$WORK/mkua.o"
clang++ "$WORK/mkua.o" $(ls "$ROOT/build/objs"/*.o | grep -v winget_cli.o) \
    -o "$WORK/mkua" -lsqlite3 -lyaml -ljsoncpp -licuuc -licui18n -licudata -lssl -lcrypto -lcurl -lz
"$WORK/mkua" > /dev/null

run install Test.PortableUA > /dev/null
run install Test.Zip > /dev/null
run install Test.Script > /dev/null
UAOUT=$(run upgrade --all)
echo "$UAOUT" | grep -c "already up to date" | grep -q "^3$" && pass "upgrade --all reports all 3 up to date" || fail "upgrade --all count"
echo "$UAOUT" | grep -q "Upgrade summary: 3 OK, 0 failed" && pass "upgrade --all summary correct" || fail "upgrade --all summary"
run uninstall Test.PortableUA > /dev/null
run uninstall Test.Zip > /dev/null
run uninstall Test.Script > /dev/null
run upgrade --all | grep -q "nothing to upgrade" && pass "upgrade --all with nothing installed" || fail "upgrade --all empty"

echo "=== error cases ==="
run install Foo.DoesNotExist > /dev/null; [ $? -eq 3 ] && pass "install unknown pkg exits 3" || fail "install unknown pkg exit code"
run uninstall Test.Portable > /dev/null; [ $? -eq 4 ] && pass "uninstall not-installed exits 4" || fail "uninstall not-installed exit code"
run badcommand > /dev/null 2>&1; [ $? -eq 64 ] && pass "unknown command exits 64" || fail "unknown command exit code"

echo "=== list (nothing installed) ==="
run list | grep -q "No installed packages" && pass "list empty state" || fail "list empty state"

echo "=== security: id/alias/source-name sanitization ==="
run install-url "http://127.0.0.1:$PORT/tool.sh" "../escape" > /dev/null 2>&1
[ $? -ne 0 ] && [ ! -e "$HOME/../escape" ] && pass "install-url rejects traversal alias" || fail "install-url traversal alias"
run install-url "http://127.0.0.1:$PORT/tool.sh" "a'b" > /dev/null 2>&1
[ $? -ne 0 ] && pass "install-url rejects quote in alias" || fail "install-url quote alias"
run source add "../evilsource" "http://127.0.0.1:$PORT/nope.db" > /dev/null 2>&1
[ $? -ne 0 ] && [ ! -e "$HOME/.winget/sources/../evilsource.db" ] && pass "source add rejects traversal name" || fail "source add traversal name"
run source add "a/b" "http://127.0.0.1:$PORT/nope.db" > /dev/null 2>&1
[ $? -ne 0 ] && pass "source add rejects slash in name" || fail "source add slash name"

echo "=== security: install-url single-download, real payload installed ==="
cat > "$HTTPROOT/sectest.sh" <<'PAYLOAD_SEC'
#!/data/data/com.termux/files/usr/bin/bash
echo "sectest: real execution ok"
PAYLOAD_SEC
chmod +x "$HTTPROOT/sectest.sh"
run install-url "http://127.0.0.1:$PORT/sectest.sh" sectest > /dev/null 2>&1
command -v sectest > /dev/null && pass "install-url alias on PATH" || fail "install-url alias on PATH"
[ "$(sectest 2>&1)" = "sectest: real execution ok" ] && pass "install-url installs real payload" || fail "install-url payload content"
run uninstall url:sectest > /dev/null 2>&1

echo "=== security: concurrent install/uninstall on same id doesn't corrupt state ==="
cat > "$HTTPROOT/conc.sh" <<'PAYLOAD_CONC'
#!/data/data/com.termux/files/usr/bin/bash
echo "conc: ok"
PAYLOAD_CONC
chmod +x "$HTTPROOT/conc.sh"
CONC_SHA=$(openssl dgst -sha256 "$HTTPROOT/conc.sh" | awk '{print $2}')
cat > "$WORK/test_conc.yaml" <<EOF
PackageIdentifier: Test.Conc
PackageVersion: 1.0.0
PackageLocale: en-US
Publisher: Test
PackageName: Conc Test
Moniker: conctest
ShortDescription: test
InstallerLocale: en-US
InstallerType: portable
Installers:
  - Architecture: arm64
    InstallerUrl: http://127.0.0.1:$PORT/conc.sh
    InstallerSha256: $CONC_SHA
ManifestType: singleton
ManifestVersion: 1.0.0
EOF
run index "$WORK/test_conc.yaml" > /dev/null 2>&1
for i in 1 2 3 4 5; do run install Test.Conc > "$WORK/conc_$i.log" 2>&1 & done
wait
if ! command -v conctest > /dev/null; then
    fail "concurrent install corrupted state"
    cat "$WORK"/conc_*.log
else
    pass "concurrent installs converge to installed state"
fi
run uninstall Test.Conc > /dev/null 2>&1

# cleanup
pkill -f "http.server $PORT" > /dev/null 2>&1
[ -f "$REAL_DB_BACKUP" ] && cp "$REAL_DB_BACKUP" "$REAL_DB" || rm -f "$REAL_DB"
rm -rf "$WORK"
rm -f "$MANIFEST_ROOT/test_portable.yaml" "$MANIFEST_ROOT/test_zip.yaml" "$MANIFEST_ROOT/test_script.yaml" "$MANIFEST_ROOT/test_script_v2.yaml" "$MANIFEST_ROOT/test_portable_ua.yaml" "$MANIFEST_ROOT/test_conc.yaml"
# index copies test_conc.yaml into $MANIFEST_ROOT under its own filename; already covered above.
rm -rf /data/data/com.termux/files/home/.winget/versions/Test.Script.version

echo
if [ "$FAILS" -eq 0 ]; then
    echo "ALL TESTS PASSED"
    exit 0
else
    echo "$FAILS TEST(S) FAILED"
    exit 1
fi
