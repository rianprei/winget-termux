# Contributing

## Build and test

```bash
./build.sh
./run-tests.sh
```

`run-tests.sh` is self-contained: stages its own manifests, serves real
payloads over a local HTTP server, and exercises every command against the
compiled binary. It must pass (`ALL TESTS PASSED`) before a PR is merged.

## Rules

- No fake success. A change that isn't actually run against a real payload,
  real network call, or real filesystem state doesn't count as tested.
- Add a negative test in `run-tests.sh` for any new security-relevant path
  (path sanitization, symlink/zip handling, locking).
- Match existing style: RAII, no raw `new`/`delete`, comments explain *why*
  not *what*.

## Adding a catalog package

See [`catalog/README.md`](catalog/README.md) — the rule there is the same:
an entry stays only if it was actually downloaded, installed, and run.

## Reporting a bug

Open an issue with the exact command, expected vs actual output, and
`winget --info` output.
