# Fork statistics

- Upstream: `microsoft/winget-cli` @ `855c01f6bf3f4604cb6cc24be5caf799949ea246`
- Diff vs upstream: **46 files changed, 600 insertions(+), 4143 deletions(-)**
  (net code *shrinks* by a wide margin — most changes cut Windows-only
  branches and dead files, not add equivalents; the deletion count includes
  7 whole files removed as dead code, see below)
- New files not in upstream: 4 (`wil/` compat shim × 3 headers,
  `DeviceCodeFlowAuthenticator.h/.cpp`)
- Files compiled from the patched upstream tree (`FILES.txt`): 69
- New project-authored files (`extra/`): 13 (`PortableInstallerTermux.*`,
  `ZipInstallerTermux.*`, `winget_cli.cpp`, 8 filename-workaround
  replacements)
- Third-party vendored: `valijson` @ `98eaac3e2c156dd08fe619680360250683292952`
  (header-only, cloned by `build.sh`, not modified)
- Deleted dead files (real cleanup, part of the patch): `Registry.cpp`,
  `Certificates.cpp`, `SearchResultsTable_{1_0,1_1,1_2,1_6,2_0}.cpp` — never
  compiled, fully replaced by `extra/replacements/*`
- One real schema extension: `InstallerTypeEnum::Script` added to
  `ManifestCommon.h/.cpp` (not upstream winget.run schema, disclosed fork
  extension for shell-script installers on Termux)

Regenerate with the compile-and-diff steps in `build.sh` plus:
```
git -C build/winget-cli diff --shortstat
git -C build/winget-cli status --porcelain
```
