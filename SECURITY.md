# Security Policy

## Reporting a vulnerability

Open a GitHub issue, or if the report involves sensitive details (e.g. a
working exploit), use GitHub's private vulnerability reporting
(Security tab → Report a vulnerability) instead of a public issue.

## Scope

- Path/id/alias/source-name injection into filesystem or shell contexts
- Zip-slip / archive extraction escaping the target directory
- Symlink handling that could overwrite files outside a package's own
  install directory
- TOCTOU or race conditions in install/uninstall/upgrade
- SHA256 verification bypass on downloaded installer payloads

## Out of scope

- Compromise of an upstream GitHub release asset itself (this project
  verifies SHA256 against the manifest, not the publisher's identity)
- Issues requiring root/physical device access
