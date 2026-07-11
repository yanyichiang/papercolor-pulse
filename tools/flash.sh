#!/usr/bin/env bash
# Flash PaperColor from its USB download mode, then boot the application with
# a watchdog reset. A regular USB hard reset can leave this board in the ROM
# downloader even though esptool reported a successful write.

set -euo pipefail

cd "$(dirname "$0")/.."

MODE="${1:-app-flash}"
PORT_GLOB="${PAPERCOLOR_PORT:-/dev/cu.usbmodem*}"
TIMEOUT_S="${FLASH_TIMEOUT_S:-180}"
BAUD="${PAPERCOLOR_BAUD:-115200}"

case "$MODE" in
    app-flash)
        ARG_FILE="app-flash_args"
        ;;
    full | flash)
        ARG_FILE="flash_args"
        ;;
    erase)
        ARG_FILE="flash_args"
        ;;
    *)
        echo "error: unknown mode '$MODE'" >&2
        echo "valid: app-flash | full | erase" >&2
        exit 2
        ;;
esac

if ! command -v idf.py >/dev/null 2>&1; then
    echo "error: idf.py not on PATH; source the ESP-IDF environment first:" >&2
    echo "  . \$IDF_PATH/export.sh" >&2
    exit 1
fi

if [ ! -f "build/$ARG_FILE" ]; then
    echo ">>> Build artifacts are missing; running idf.py build first."
    idf.py build
fi

echo ">>> Long-press the side button until the USB download port appears."
echo ">>> Waiting up to ${TIMEOUT_S}s for ${PORT_GLOB}"

PORT=""
for ((i = 1; i <= TIMEOUT_S; i++)); do
    # shellcheck disable=SC2086
    PORT=$(ls -1 $PORT_GLOB 2>/dev/null | head -1 || true)
    if [ -n "$PORT" ]; then
        echo ">>> Download port: $PORT (after ${i}s); release the button."
        break
    fi
    sleep 1
done

if [ -z "$PORT" ]; then
    echo "error: no device matching '${PORT_GLOB}' appeared." >&2
    echo "Check the USB cable, re-run, and long-press the side button." >&2
    exit 1
fi

ESPTOOL=(python -m esptool --chip esp32s3 --port "$PORT" --baud "$BAUD")

if [ "$MODE" = "erase" ]; then
    "${ESPTOOL[@]}" --before no_reset --after no_reset erase_flash
fi

(
    cd build
    "${ESPTOOL[@]}" --before no_reset --after watchdog_reset write_flash "@$ARG_FILE"
)

echo ">>> Flash verified and application started with a watchdog reset."
