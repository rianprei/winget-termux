# winget-termux

[![CI](https://github.com/rianprei/winget-termux/actions/workflows/ci.yml/badge.svg)](https://github.com/rianprei/winget-termux/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![Catalog](https://img.shields.io/badge/catalog-19_packages-blue.svg)](catalog/README.md)

A native ARM64 port of a working subset of Microsoft's `winget-cli`, built
to run directly on Termux — no proot, no chroot, no root, no Wine, no
emulation layer. The binary is a real clang build targeting bionic libc,
using the same manifest schema and SQLite index format as upstream winget.

## Quick start

```bash
git clone https://github.com/rianprei/winget-termux.git
cd winget-termux
./build.sh
winget --version
```

## What it does

- `install`, `uninstall`, `upgrade` (single or `--all`), `search`, `show`,
  `list` — the core winget command surface.
- `source add/update/remove/list` — register and sync remote SQLite
  catalogs, same schema as this project's own index.
- `index` — add a manifest to the local catalog in one command.
- `install-url` — install a single script or binary directly from a URL,
  no manifest required; the hash is computed from what was actually
  downloaded.
- `pin` / `unpin` — exclude a package from upgrades.
- `export` / `import` — snapshot and restore installed packages.
- `hash` — compute a manifest-ready SHA256 for a local file.
- `validate` — schema-check a manifest without indexing it.
- `download` — fetch a catalog entry's installer without installing it.
- Portable, Zip, and Script installer types, including automatic `.tar.gz`
  extraction alongside `.zip`.

## What it can't do (and won't fake)

- `.exe`, `.msi`, `.msix` installers — no path to running Win32 binaries on
  Android without Wine/emulation, which this project deliberately avoids.
- `msstore` source — talks to a Microsoft Store API with no Android equivalent.
- `winget configure` (DSC) — Windows-only configuration engine, no ARM64/Linux port exists.
- COM API / PowerShell module (`Microsoft.WinGet.Client`) — no COM surface on Android.
- Group Policy / MDM enterprise controls — Windows-only management stack.

Everything else in real winget-cli's CLI surface (install, search, source
management, pin, export/import, hash, validate, upgrade, list, completions)
is implemented natively above.

## Build

```bash
./build.sh
```

Clones `winget-cli` at a pinned commit, applies `patches/winget-cli.patch`,
compiles the patched subset listed in `FILES.txt`, links
`./winget_real_cli`, and points a `winget` symlink at it from `$PREFIX/bin`.
Installs its own `pkg` dependencies (clang, cmake, sqlite, libyaml,
jsoncpp, libicu, openssl, libcurl, zlib, curl, unzip, zip, git).

If `$PREFIX/bin/winget` is already a real file — not this project's
symlink — `build.sh` leaves it alone and says so. Remove the conflict
yourself and rerun, or just call `./winget_real_cli` directly.

## Test

```bash
./run-tests.sh
```

Self-contained: stages its own manifests, serves real payloads over a local
HTTP server, and exercises every command against the compiled binary. Exits
non-zero on failure.

## Use

```bash
winget search <query>
winget show <PackageIdentifier>
winget install <PackageIdentifier>
winget uninstall <PackageIdentifier>
winget upgrade <PackageIdentifier>       # or: winget upgrade --all
winget list                              # or: winget list --upgrade-available

winget index <manifest.yaml>
winget install-url <url> [alias]

winget pin <PackageIdentifier>
winget unpin <PackageIdentifier>
winget export [file]
winget import <file>
winget hash <file>
winget validate <manifest.yaml>
winget download <PackageIdentifier> [dest-dir]

winget source add <name> <url-to-sqlite-catalog>
winget source update <name>
winget source remove <name>
winget source list
winget source export [file]

winget --info
winget complete
```

To install something, it needs to be in the catalog first. Either write a
manifest and run `winget index`:

```bash
winget index my_package.yaml
winget install <PackageIdentifier>
```

or skip the manifest entirely for a single binary:

```bash
winget install-url https://example.com/tool
```

There is no sync against the official winget.run catalog — this project
maintains its own, see [catalog/](catalog/).

To remove just the `winget` shortcut (keeping the build):
`rm $PREFIX/bin/winget`. `./build.sh` recreates it.

## Package

```bash
./package.sh
dpkg -i winget-termux_1.0.0_aarch64.deb
```

Builds a real Termux `.deb` from the compiled binary. Unlike the dev
symlink, it installs the binary itself into `$PREFIX/bin`, so it works on
a Termux install that never cloned this repository. Remove with
`dpkg -r winget-termux`.

## Documentation

- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) — how the port works, what
  was rewritten, what was cut, and why.
- [docs/STATS.md](docs/STATS.md) — diff against upstream winget-cli.
- [docs/AUDIT.md](docs/AUDIT.md) — known limitations and bugs found during
  development.
- [catalog/](catalog/) — nineteen verified ARM64 command-line tools.
- [CONTRIBUTING.md](CONTRIBUTING.md) — build, test, and contribution rules.
- [SECURITY.md](SECURITY.md) — vulnerability reporting.

## License

MIT, same as upstream `winget-cli` (Copyright (c) Microsoft Corporation).
See [LICENSE](LICENSE).
