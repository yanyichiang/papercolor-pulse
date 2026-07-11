#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
ENV_FILE="$ROOT/.env"

command -v docker >/dev/null 2>&1 || {
    echo "Docker is required. Install Docker Engine or Docker Desktop first." >&2
    exit 1
}
docker compose version >/dev/null 2>&1 || {
    echo "Docker Compose v2 is required." >&2
    exit 1
}

if [ ! -f "$ENV_FILE" ]; then
    command -v python3 >/dev/null 2>&1 || {
        echo "python3 is required to generate secure tokens." >&2
        exit 1
    }
    device_token=$(python3 -c 'import secrets; print(secrets.token_urlsafe(48))')
    admin_token=$(python3 -c 'import secrets; print(secrets.token_urlsafe(48))')
    umask 077
    cat >"$ENV_FILE" <<EOF
PAPERCOLOR_DEVICE_ID=papercolor-001
PAPERCOLOR_DEVICE_TOKEN=$device_token
PAPERCOLOR_ADMIN_TOKEN=$admin_token
PAPERCOLOR_FONT_PATH=/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc
PAPERCOLOR_CJK_SERIF_FONT_PATH=/usr/share/fonts/opentype/noto/NotoSerifCJK-Regular.ttc
PAPERCOLOR_LATIN_SERIF_FONT_PATH=/usr/share/fonts/opentype/noto/NotoSerifCJK-Regular.ttc
PAPERCOLOR_TIMEZONE=${TZ:-UTC}
PAPERCOLOR_DATA_DIR=/data
PAPERCOLOR_HOST=0.0.0.0
PAPERCOLOR_PORT=8767
PAPERCOLOR_LOG_LEVEL=INFO
EOF
    chmod 0600 "$ENV_FILE"
    unset device_token admin_token
    echo "Created $ENV_FILE with mode 0600."
else
    echo "Using existing $ENV_FILE."
fi

docker compose -f "$ROOT/compose.yaml" --env-file "$ENV_FILE" up -d --build
echo "Waiting for the gateway..."
i=0
until curl -fsS http://127.0.0.1:8767/health >/dev/null 2>&1; do
    i=$((i + 1))
    if [ "$i" -ge 30 ]; then
        echo "Gateway did not become healthy. Run: docker compose -f $ROOT/compose.yaml logs" >&2
        exit 1
    fi
    sleep 1
done

echo "Gateway is healthy at http://127.0.0.1:8767"
echo "Next: source gateway/.env in your shell and run tools/provision_wifi.py."
