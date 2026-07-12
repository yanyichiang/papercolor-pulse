# Security hardening notes

This repository intentionally keeps authentication material out of source code and firmware build definitions.

## Build-time secrets

Do not set `PAPERCOLOR_API_TOKEN` during compilation. Compiler definitions can appear in command lines, logs, ELF files, and firmware binaries. Machine-only diagnostics remain unavailable unless a runtime credential mechanism is explicitly implemented.

## Local checks

Run:

```bash
./tools/public-privacy-scan.sh
```

For personal hostnames, addresses, device identifiers, or other private infrastructure markers:

```bash
cp .privacy-patterns.example .privacy-patterns.local
```

Then add one regular expression per line. The local file is ignored by Git.

## Repository-history checks

The secret-scan workflow uses Gitleaks with full Git history on pushes and pull requests. A clean working tree does not prove that earlier commits, logs, artifacts, screenshots, or release binaries are clean.
