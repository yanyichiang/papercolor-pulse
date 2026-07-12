# Security policy

## Reporting a vulnerability

Please do not publish credentials, private infrastructure identifiers, serial logs, firmware images built with local secrets, or exploit details in a public issue.

Report security concerns privately to the repository owner through GitHub's private vulnerability reporting feature when available.

## Secret handling rules

- Never commit `.env`, `.dev.vars`, `secrets.yaml`, private keys, service-account files, generated NVS images, or local Wrangler configuration.
- Never pass authentication material through compiler definitions or command-line arguments.
- Treat firmware binaries, build logs, serial logs, workflow logs, screenshots, issue text, and release assets as potentially public.
- Rotate a credential immediately if it appears in Git history, logs, artifacts, screenshots, or a distributed firmware image.
- Run `./tools/public-privacy-scan.sh` before publishing, and use Gitleaks for full repository-history scans.
