# winget-termux

Native ARM64/bionic Termux port of a real subset of Microsoft's `winget-cli`.
No proot, chroot, root, Wine, Box64, or emulation. See
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the technical writeup.

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

Manifests must be indexed into the SQLite catalog first (see
`docs/ARCHITECTURE.md` — RelativePath must be a real relative path, not
absolute) — there is no `source add`/sync against the real winget.run
catalog in this port.
