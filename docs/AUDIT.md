# Audit

## TODO / FIXME

None found in `extra/` or the patched upstream diff. Every deliberate
simplification is marked with a `ponytail:` comment naming the cut and why
(grep `ponytail:` across `extra/` and `build/winget-cli/src` — ~35 hits, all
reviewed, all real architectural cuts with documented rationale, not silent
stubs).

## Stubs (real limitations, all documented)

- `ManifestSchemaValidation.cpp` `LoadSchemaDoc`/`ValidateAgainstSchema` —
  honest no-op; the separate `ValidateYamlManifestsSchemaHeader` (unaffected)
  still does the real schema-header check.
- `MsixManifestValidation.cpp` — MSIX packages can't install on Android
  regardless, so validation is a no-op that always passes.
- `Certificates.cpp` `PinningConfiguration::LoadFrom` — returns `false`
  ("not supported, falling back to standard TLS trust"), not a fake accept.
- `Registry.cpp` — every method reports "not found"/empty (no Windows
  Registry on Android; this is the real, intended architecture, not a
  placeholder for future work).

None of these sit on the Portable/Zip install data path.

## Dead code

**Removed** (was: kept as a diff reference, now deleted for real — the
deletion is part of `patches/winget-cli.patch`, so it survives every clean
rebuild):
- `src/AppInstallerSharedLib/Registry.cpp` and `Certificates.cpp` — original,
  unmodified, Windows-dependent versions, never compiled (their
  `AddManifest`/install-relevant behavior was fully replaced by
  `extra/replacements/registry_posix.cpp` and `certs_pinning.cpp`).
- `src/AppInstallerRepositoryCore/Microsoft/Schema/{1_0,1_1,1_2,1_6,2_0}/SearchResultsTable_*.cpp`
  — same situation, replaced by `extra/replacements/srt{10,11,12,16,20}.cpp`.

## Duplications

- `Runtime.cpp` exists as two distinct files (`AppInstallerSharedLib/Runtime.cpp`
  and `AppInstallerCommonCore/Runtime.cpp`) with unrelated content — a real
  upstream naming collision, not a copy-paste duplicate. `build.sh`'s object
  naming is path-derived specifically to avoid clobbering one with the other
  (see the comment at the `obj=` line) — verified by `nm` that both sets of
  symbols (`GetPathDetailsFor` vs `IsRunningInPackagedContext`) are present
  in the final binary.

## Resource leaks

- `PortableInstallerTermux.cpp` / `ZipInstallerTermux.cpp`: `curl_easy_init`
  paired 1:1 with `curl_easy_cleanup` in both files (checked via grep count).
  `std::ofstream` destructors close files via RAII; no raw `FILE*`/`new`.

## Regressions

None found — `run-tests.sh` covers Portable install/uninstall/reinstall, Zip
install/uninstall/reinstall, search, show, list, source list, and 3 error
paths (unknown package, not-installed uninstall, unknown command), all
passing against the from-scratch `build.sh` output.

## Real bugs found and fixed during this hardening pass

1. **Zip vs Portable backend confusion**: a zip installer whose nested type
   is Portable also reports `EffectiveInstallerType()==Portable`; checking
   Portable before Zip handed the raw zip archive to the Portable backend.
   Fixed by checking `BaseInstallerType==Zip` first.
2. **Uninstall backend misrouting**: `IsCommandOnPath` is identical between
   the Portable and Zip backends (both just check the `$PREFIX/bin` symlink),
   so probing "which backend's `IsCommandOnPath` returns true first" always
   picked Portable — silently no-oping on the wrong directory while still
   reporting "Successfully uninstalled." Fixed by dispatching uninstall on
   manifest installer type, mirroring install-time resolution.
3. **`.c`/`.cpp` compiler flag bug** in `build.sh`: `SQLiteICU.c` was being
   compiled with `-std=c++20`, which clang rejects for C sources.
4. **Object-name collision**: `Runtime.cpp` exists in two subsystems;
   basename-only object naming silently let one clobber the other's `.o`.
   Fixed with path-derived object names.
5. **Missing `-I` for `binver/version.h`**: real header exists in the
   upstream tree (`src/binver/binver/version.h`), just needed the include
   path, not a stub.
6. **`RelativePath` must be relative**: `PathPartTable::EnsurePathExistsInternal`
   throws `E_INVALIDARG` on any root path — discovered via `gdb catch throw`
   after absolute-path indexing (used throughout earlier ad-hoc dev testing)
   started failing on a from-scratch build. Fixed `winget_cli.cpp`'s
   manifest resolution to use a real relative-path convention
   (`$HOME/.winget/manifests/<name>.yaml`), matching actual upstream schema
   behavior instead of relying on absolute paths that happened to survive in
   stale, previously-compiled dev objects.
7. **Version marker inside the backend's own directory broke uninstall**:
   the first `upgrade` implementation wrote `.version` inside
   `~/.winget/programfiles/<id>/`, which made `UninstallPortable`'s
   "directory ended up empty" success check fail forever after the first
   install (a real leftover-file false negative, same failure class as bug
   #2). Fixed by moving the marker to a separate
   `$HOME/.winget/versions/<id>.version` path, outside any backend-owned
   directory, so the validated backends needed zero changes.
8. **`Search()` does not return the latest version**: with two versions of
   the same package indexed, `SQLiteIndex::Search()` returns one
   representative manifest row per package, not necessarily the highest
   version — confirmed by `GetVersionKeysById` returning both versions
   correctly sorted (latest first) for the same package. Fixed
   `ResolveManifestById` to call `GetVersionKeysById` and take the first
   (latest) result instead of trusting `Search()`'s arbitrary pick, which
   is what `upgrade` needed to actually detect a newer version.
9. **Every command required the local db to already exist**: `search`/`show`
   against a remote-only source failed with "No source found" because
   `main()` hard-required `TermuxLocal`'s db file to exist before opening
   anything, even for commands that don't need local install state. Fixed
   by lazily creating a real, empty `SQLiteIndex` when missing, instead of
   erroring out.
10. **`uninstall` required live manifest resolution**: after adding
    persisted source tracking, uninstalling a package whose source had
    been removed failed outright ("No package found") even though the
    package was still installed on disk and perfectly removable — the old
    `CmdUninstall` always re-resolved the manifest to compute the backend
    and alias. Fixed by rewriting it to determine backend from which
    package directory exists on disk, and alias from the persisted
    `InstalledState` (falling back to manifest resolution only if that
    state is missing) — no network or source dependency for a purely local
    operation.
11. **Test-only**: `upgrade --all` test reused a shared `tool.sh` payload
    file/URL that a later test block overwrote with different content,
    breaking the earlier manifest's pinned SHA256 (real hash-mismatch
    rejection working correctly — not a product bug, just a test fixture
    needing its own dedicated payload file instead of sharing one across
    unrelated test sections).
12. **Test-only**: forgot to call `source update` after adding a new package
    version to a remote source's *origin* catalog file during test setup —
    `source add` had already downloaded a *copy* into
    `~/.winget/sources/<name>.db`, so editing the origin file directly had
    no effect until an explicit refresh. Not a product bug (this is exactly
    the real, intended separation between "the source's live catalog" and
    "our last-synced copy of it") — just a reminder that test fixtures
    simulating a remote update need the same `source update` step a real
    user would run.
