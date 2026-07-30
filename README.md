# winget-termux

Native ARM64/bionic Termux port of a real subset of Microsoft's `winget-cli`.
No proot, chroot, root, Wine, Box64, or emulation.

## Quick start

```
git clone https://github.com/rianprei/winget-termux.git
cd winget-termux
./build.sh
winget --version
```

## Docs

- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) — technical writeup, what's
  real vs. cut, POSIX/Win32 conversion table.
- [docs/STATS.md](docs/STATS.md) — diff stats vs. upstream winget-cli.
- [docs/AUDIT.md](docs/AUDIT.md) — dead code, real bugs found and fixed,
  known limitations.

## Build

```
./build.sh
```

Clones winget-cli at a pinned commit, applies `patches/winget-cli.patch`,
compiles the real (patched) subset listed in `FILES.txt`, links
`./winget_real_cli`, and creates/refreshes a `winget` symlink in
`$PREFIX/bin` pointing at it. Requires Termux with network access; installs
its own `pkg` dependencies (clang, cmake, sqlite, libyaml, jsoncpp, libicu,
openssl, libcurl, zlib, curl, unzip, zip, git).

If `$PREFIX/bin/winget` already exists as a real file (not our symlink --
e.g. a different package), `build.sh` leaves it alone and tells you so; use
`./winget_real_cli` directly in that case, or remove the conflicting file
yourself and re-run `build.sh`.

## Test

```
./run-tests.sh
```

Self-contained: stages its own test manifests, serves real payloads over a
local HTTP server, and exercises `source list` / `search` / `show` / `list`
/ `install` / `uninstall` for both Portable and Zip installer types against
the real compiled binary. Exits non-zero on any failure.

## Use

After `build.sh`, `winget` is on `$PATH` (a symlink to `./winget_real_cli`
— the real binary itself is untouched and still directly runnable):

```
winget --version
winget index <manifest.yaml>
winget install-url <url> [alias]   # no manifest at all, self-verifying
winget pin <PackageIdentifier>     # skip on upgrade/upgrade --all
winget unpin <PackageIdentifier>
winget export [file]               # dump installed packages as JSON (default: stdout)
winget import <file>               # reinstall everything from an export
winget source list
winget source add <name> <url-to-sqlite-catalog>
winget source update <name>
winget source remove <name>
winget search <query>
winget show <PackageIdentifier>
winget install <PackageIdentifier>
winget uninstall <PackageIdentifier>
winget upgrade <PackageIdentifier>|--all
winget list
```

To remove just the shortcut (keeping the build): `rm $PREFIX/bin/winget`.
Running `./build.sh` again recreates it.

## Package (real .deb, installable on any Termux)

```
./package.sh
dpkg -i winget-termux_1.0.0_aarch64.deb    # or: pkg install ./winget-termux_1.0.0_aarch64.deb
```

Builds a real Termux `.deb` from the already-compiled `winget_real_cli`
(run `build.sh` first). Unlike the dev `winget` symlink, the package
installs the actual binary into `$PREFIX/bin/winget_real_cli` plus a
`winget` symlink next to it -- no dependency on this source checkout, so it
works on a Termux install that never cloned this repo. Remove with
`dpkg -r winget-termux` (or `pkg uninstall winget-termux`).

Write a manifest YAML (see `manifests/` for real examples), then:

```
winget index my_package.yaml
winget install <PackageIdentifier from the manifest>
```

`winget index` stages the file into the real manifest root and adds it to
the local SQLite catalog with one command — no compiling required. There is
no `source add`/sync against the real winget.run catalog in this port.

## License

MIT, same as upstream `winget-cli` (Copyright (c) Microsoft Corporation).
See [LICENSE](LICENSE).
