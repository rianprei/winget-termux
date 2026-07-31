# winget-termux catalog

Nineteen ARM64-native command-line tools, installable through `winget install`.
Every entry was downloaded, hashed, installed, and executed on-device before
being added here — no placeholder hashes, no untested URLs.

| ID | Command | Version | Description |
|----|---------|---------|-------------|
| `junegunn.fzf` | `fzf` | 0.74.1 | Fuzzy finder |
| `ajeetdsouza.zoxide` | `zoxide` | 0.10.0 | Faster `cd` |
| `BurntSushi.ripgrep` | `rg` | 15.2.0 | Fast recursive grep |
| `aristocratos.btop` | `btop` | 1.4.7 | Resource monitor |
| `dalance.procs` | `procs` | 0.14.12 | Modern `ps` |
| `sxyazi.yazi` | `yazi` | 26.5.6 | Terminal file manager |
| `micro-editor.micro` | `micro` | 2.0.15 | Terminal text editor |
| `starship.starship` | `starship` | 1.26.0 | Cross-shell prompt |
| `tealdeer-rs.tealdeer` | `tldr` | 1.8.1 | Fast tldr client |
| `ducaale.xh` | `xh` | 0.26.2 | Friendly HTTP client |
| `ClementTsang.bottom` | `btm` | 0.14.7 | Graphical process/system monitor |
| `chmln.sd` | `sd` | 1.1.0 | Intuitive find & replace |
| `Byron.dua-cli` | `dua` | 2.39.1 | Disk usage analyzer (TUI) |
| `casey.just` | `just` | 1.57.0 | Command runner |
| `orf.gping` | `gping` | 1.20.4 | Ping with a graph |
| `dundee.gdu` | `gdu` | 5.36.1 | Fast disk usage analyzer (TUI) |
| `schollz.croc` | `croc` | 10.7.0 | Secure file transfer |
| `muesli.duf` | `duf` | 0.9.1 | Disk usage/free utility |
| `FiloSottile.age` | `age` | 1.3.1 | Simple modern file encryption |

## Why not every real GitHub release binary

Android's bionic C library isn't glibc. A GitHub release binary built and
linked against glibc (most `-gnu` targets) fails to execute at all on
Termux, even on the right CPU architecture. Only binaries that are fully
static or built against musl run without a compatibility layer. A few Go
Go binaries (`gh`, `lazygit`, `glow`) also failed here with
`SIGSYS: bad system call` — Android's seccomp policy rejects a syscall
their runtime issues (`faccessat2` in `glow`'s case); that's outside
anything this project controls. Other Go binaries (`gdu`, `croc`, `duf`,
`age`) don't hit that code path and run fine — it's not "Go never works",
it's per-binary, and the only way to know is to actually run it.

The rule for this catalog: an entry stays only if it was actually run and
produced real output. `eza`, `bat`, `delta`, `hyperfine`, `dust`, `jq`,
`gh`, and `lazygit` were tested and dropped for exactly this reason —
downloading and hashing correctly is not the bar, running is.

## Try it

```bash
cd catalog
python3 -m http.server 8942 &
winget source add termux-catalog http://127.0.0.1:8942/catalog.db
winget install junegunn.fzf
fzf --version
```

## How it's built

`catalog.db` is a real SQLite index built by `build-catalog.sh`, using the
same `SQLiteIndex::CreateNew` / `AddManifest` code the main `winget` binary
uses. Each `*.yaml` is a winget manifest singleton pointing at the real
upstream GitHub release asset, with `InstallerType: zip` and
`NestedInstallerFiles` telling the Zip backend which file inside the
archive to extract and symlink (the backend also transparently handles
`.tar.gz`, not just `.zip`, by checking real magic bytes).

Rebuild after editing a manifest:

```bash
cd ..            # winget-termux root
./catalog/build-catalog.sh
```

`verify-on-device.sh` is the gate before merging a new/changed manifest:
serves the catalog locally, installs every package through `winget` for
real, runs `<alias> --version` (or `--help`), and fails if any command
doesn't produce real output. Not a build-error check — this is the same
"downloaded and hashed correctly is not the bar, running is" rule as the
curation process, just automated:

```bash
./catalog/verify-on-device.sh
```

## Adding a package

1. Find a real aarch64/arm64 Linux release asset — prefer `musl` or
   statically-linked builds; `gnu`-linked ones will not run.
2. Download it and compute `sha256sum`.
3. Inspect the archive contents (`tar -tzf` / `unzip -l`) to get the exact
   in-archive path of the binary.
4. Write `catalog/<id>.yaml` matching the existing files.
5. Run `./catalog/build-catalog.sh`.
6. Serve it and install it for real — `winget install <id>`, then run the
   command. Only keep it if it actually runs.
