#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT"

PATTERN='(/Users/|@gmail\.|aptissimorum|cloude-box|racknerd|192\.168\.1\.100|100\.87\.|44:1b:f6|papercolor-c17fb4|PAPERCOLOR_ADMIN_TOKEN=[A-Za-z0-9_-]{32,}|PAPERCOLOR_DEVICE_TOKEN=[A-Za-z0-9_-]{32,}|CF-Access-Client-Secret:[[:space:]]*[A-Za-z0-9_-]{20,})'

if rg -n -i "$PATTERN" . \
    -g '!build/**' -g '!managed_components/**' -g '!components/**' \
    -g '!tools/public-privacy-scan.sh' \
    -g '!*.png' -g '!*.jpg' -g '!*.jpeg'; then
    echo "privacy scan failed: review the matches above" >&2
    exit 1
fi

echo "privacy scan passed"
