#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

if command -v uv >/dev/null 2>&1; then
    uv sync --project "$ROOT" --python 3.12 --extra test
elif [ ! -x "$ROOT/.venv/bin/pytest" ]; then
    python3.12 -m venv "$ROOT/.venv"
    "$ROOT/.venv/bin/python" -m pip install -e "$ROOT[test]"
fi

exec "$ROOT/.venv/bin/pytest" "$ROOT/tests"
