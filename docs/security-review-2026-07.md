# Security review — July 2026

A repository and Git-history review found no committed live credentials in the current public history. The hardening changes in this branch address preventive weaknesses discovered during that review:

- repository-wide secret-file ignore rules;
- full-history Gitleaks scanning on pushes and pull requests;
- generic privacy scanning without publishing owner-specific infrastructure fingerprints;
- removal of unused EzData code that logged device tokens and token-bearing payloads;
- removal of the build-time API-token compiler-definition path;
- explicit incident-response and credential-rotation guidance.

This review does not cover unpushed local files, deleted external workflow logs, screenshots, private messages, or firmware binaries distributed outside GitHub.
