# Credential rotation checklist

Use this checklist whenever a credential may have appeared in Git history, workflow logs, serial output, screenshots, release assets, or a firmware image.

1. Revoke or rotate the exposed credential at its issuer first.
2. Replace it in the private runtime secret store.
3. Re-provision affected devices if the credential was stored in NVS.
4. Delete public workflow artifacts, release assets, logs, screenshots, and issue text containing the value.
5. Rewrite Git history only after rotation; history rewriting does not revoke a credential.
6. Re-run the full-history Gitleaks workflow and the local privacy scan.

For PaperColor Pulse, treat these values as separate credentials and rotate them independently:

- `PAPERCOLOR_DEVICE_TOKEN`
- `PAPERCOLOR_ADMIN_TOKEN`
- Cloudflare Worker `CLIENT_TOKEN`
- Cloudflare Worker `ORIGIN_ADMIN_TOKEN`
- Cloudflare Access service-token ID and secret

Never reuse the admin token as the device or Worker client token.
