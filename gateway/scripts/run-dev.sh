#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
ENV_FILE=${1:-"$ROOT/.env"}

if [ ! -f "$ENV_FILE" ]; then
    echo "environment file not found: $ENV_FILE" >&2
    exit 1
fi
if [ ! -x "$ROOT/.venv/bin/papercolor-gateway" ]; then
    echo "virtual environment missing; run: uv sync --project '$ROOT' --extra test" >&2
    exit 1
fi

set -a
# The environment file is trusted local configuration and may contain secrets.
. "$ENV_FILE"
set +a

exec "$ROOT/.venv/bin/papercolor-gateway"
