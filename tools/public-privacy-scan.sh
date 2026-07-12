#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT"

# Keep this public check generic. Personal infrastructure indicators belong in
# an untracked .privacy-patterns.local file, one extended-regex pattern per line.
PATTERN='(-----BEGIN ([A-Z ]+ )?PRIVATE KEY-----|github_pat_[A-Za-z0-9_]{20,}|gh[pousr]_[A-Za-z0-9]{30,}|sk-[A-Za-z0-9_-]{20,}|AKIA[0-9A-Z]{16}|AIza[0-9A-Za-z_-]{30,}|xox[baprs]-[A-Za-z0-9-]{20,}|glpat-[A-Za-z0-9_-]{20,}|hf_[A-Za-z0-9]{20,}|PAPERCOLOR_(ADMIN|DEVICE|API)_TOKEN[[:space:]]*=[[:space:]]*[^[:space:]#]{16,}|CF-Access-Client-Secret:[[:space:]]*[^[:space:]]{20,}|Authorization:[[:space:]]*Bearer[[:space:]]+[A-Za-z0-9._~+/-]{20,})'

scan_pattern() {
    pattern=$1
    rg -n -i "$pattern" . \
        -g '!build/**' -g '!managed_components/**' -g '!components/**' \
        -g '!tools/public-privacy-scan.sh' \
        -g '!*.png' -g '!*.jpg' -g '!*.jpeg' -g '!*.gif' -g '!*.bin' \
        -g '!package-lock.json' -g '!uv.lock'
}

failed=0
if scan_pattern "$PATTERN"; then
    failed=1
fi

if [ -f .privacy-patterns.local ]; then
    while IFS= read -r local_pattern; do
        case "$local_pattern" in
            ''|'#'*) continue ;;
        esac
        if scan_pattern "$local_pattern"; then
            failed=1
        fi
    done < .privacy-patterns.local
fi

if [ "$failed" -ne 0 ]; then
    echo "privacy scan failed: review the matches above" >&2
    exit 1
fi

echo "privacy scan passed"
