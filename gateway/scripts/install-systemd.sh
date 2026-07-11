#!/bin/sh
set -eu

SOURCE_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
INSTALL_DIR=${INSTALL_DIR:-/opt/papercolor-gateway}
SERVICE_USER=${SERVICE_USER:-papercolor-gateway}
ENV_FILE=${ENV_FILE:-/etc/papercolor-gateway/environment}
UNIT_DIR=${UNIT_DIR:-/etc/systemd/system}
UNIT_NAME=papercolor-gateway.service
VOICE_UNIT_NAME=papercolor-voice-processor.service

if [ "$(id -u)" -ne 0 ]; then
    echo "run this installer as root" >&2
    exit 1
fi
if ! command -v python3.12 >/dev/null 2>&1; then
    echo "python3.12 is required" >&2
    exit 1
fi
if [ ! -f "$ENV_FILE" ]; then
    echo "create the root-owned environment file first: $ENV_FILE" >&2
    exit 1
fi
if [ "$(stat -c '%U:%G' "$ENV_FILE")" != "root:root" ] || [ "$(stat -c '%a' "$ENV_FILE")" != "600" ]; then
    echo "$ENV_FILE must be owned by root:root with mode 0600" >&2
    exit 1
fi

if ! getent group "$SERVICE_USER" >/dev/null 2>&1; then
    groupadd --system "$SERVICE_USER"
fi
if ! getent passwd "$SERVICE_USER" >/dev/null 2>&1; then
    useradd --system --gid "$SERVICE_USER" --home-dir /nonexistent \
        --shell /usr/sbin/nologin "$SERVICE_USER"
fi

install -d -o root -g root -m 0755 "$INSTALL_DIR"
python3.12 -m venv "$INSTALL_DIR/.venv"
"$INSTALL_DIR/.venv/bin/python" -m pip install --upgrade "$SOURCE_ROOT"
install -o root -g root -m 0644 "$SOURCE_ROOT/README.md" "$INSTALL_DIR/README.md"

TEMP_UNIT=$(mktemp)
trap 'rm -f "$TEMP_UNIT"' EXIT HUP INT TERM
sed \
    -e "s|@INSTALL_DIR@|$INSTALL_DIR|g" \
    -e "s|@SERVICE_USER@|$SERVICE_USER|g" \
    -e "s|@ENV_FILE@|$ENV_FILE|g" \
    "$SOURCE_ROOT/systemd/papercolor-gateway.service.in" > "$TEMP_UNIT"
install -o root -g root -m 0644 "$TEMP_UNIT" "$UNIT_DIR/$UNIT_NAME"
sed \
    -e "s|@INSTALL_DIR@|$INSTALL_DIR|g" \
    -e "s|@SERVICE_USER@|$SERVICE_USER|g" \
    "$SOURCE_ROOT/systemd/papercolor-voice-processor.service.in" \
    > "$TEMP_UNIT"
install -o root -g root -m 0644 "$TEMP_UNIT" "$UNIT_DIR/$VOICE_UNIT_NAME"
systemctl daemon-reload

echo "installed $UNIT_DIR/$UNIT_NAME"
echo "review it, then run: systemctl enable --now $UNIT_NAME"
