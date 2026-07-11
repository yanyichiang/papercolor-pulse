#!/usr/bin/env python3
"""Write PaperColor Wi-Fi credentials directly to its NVS partition."""

from __future__ import annotations

import argparse
import csv
import getpass
import glob
import os
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from urllib.parse import urlsplit


NVS_OFFSET = "0x9000"
NVS_SIZE = "0x6000"


def validate_credentials(ssid: str, password: str) -> None:
    ssid_size = len(ssid.encode("utf-8"))
    if not 1 <= ssid_size <= 32:
        raise ValueError("SSID must contain 1 to 32 UTF-8 bytes")
    if password and not 8 <= len(password.encode("utf-8")) <= 63:
        raise ValueError("Wi-Fi password must contain 8 to 63 UTF-8 bytes")


def validate_gateway(url: str, token: str, poll_minutes: int) -> None:
    if not url:
        if token:
            raise ValueError("gateway token requires a gateway URL")
        return
    parsed = urlsplit(url)
    if parsed.scheme not in {"http", "https"} or not parsed.hostname or parsed.query or parsed.fragment:
        raise ValueError("gateway URL must be an http(s) origin without query or fragment")
    if len(url.encode("utf-8")) >= 192:
        raise ValueError("gateway URL is too long")
    token_size = len(token.encode("utf-8"))
    if not 16 <= token_size < 96:
        raise ValueError("gateway token must contain 16 to 95 UTF-8 bytes")
    if not 1 <= poll_minutes <= 1440:
        raise ValueError("gateway poll interval must be between 1 and 1440 minutes")


def nvs_rows(
    ssid: str,
    password: str,
    device_name: str,
    gateway_url: str = "",
    gateway_token: str = "",
    gateway_poll_minutes: int = 15,
) -> list[list[str]]:
    gateway_enabled = bool(gateway_url)
    return [
        ["key", "type", "encoding", "value"],
        ["papercolor", "namespace", "", ""],
        ["wifi_ssid", "data", "string", ssid],
        ["wifi_pass", "data", "string", password],
        ["rotation", "data", "u8", "1"],
        ["auto_slide", "data", "u8", "0"],
        ["interval", "data", "u16", "60"],
        ["cur_mode", "data", "string", "mode_1"],
        ["boot_sound", "data", "u8", "0"],
        ["low_power", "data", "u8", "1" if gateway_enabled else "0"],
        ["device_name", "data", "string", device_name],
        ["gw_url", "data", "string", gateway_url],
        ["gw_token", "data", "string", gateway_token],
        ["gw_enabled", "data", "u8", "1" if gateway_enabled else "0"],
        ["gw_poll", "data", "u16", str(gateway_poll_minutes)],
    ]


def wait_for_port(pattern: str, timeout_s: int) -> str:
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        matches = sorted(glob.glob(pattern))
        if matches:
            return matches[0]
        time.sleep(1)
    raise TimeoutError(f"no device matching {pattern!r} appeared")


def run_checked(command: list[str], *, cwd: Path | None = None, quiet: bool = False) -> None:
    result = subprocess.run(command, cwd=cwd, text=True, capture_output=quiet, check=False)
    if result.returncode != 0:
        raise RuntimeError(f"command failed with exit code {result.returncode}: {command[0]}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ssid", default=os.environ.get("PAPERCOLOR_WIFI_SSID", ""))
    parser.add_argument("--device-name", default="papercolor")
    parser.add_argument("--port", default=os.environ.get("PAPERCOLOR_PORT", "/dev/cu.usbmodem*"))
    parser.add_argument("--baud", type=int, default=int(os.environ.get("PAPERCOLOR_BAUD", "115200")))
    parser.add_argument("--timeout", type=int, default=180)
    parser.add_argument(
        "--app-image",
        type=Path,
        help="Optional app binary to flash at 0x10000 in the same download-mode session",
    )
    parser.add_argument("--gateway-url", default=os.environ.get("PAPERCOLOR_GATEWAY_URL", ""))
    parser.add_argument(
        "--gateway-poll-minutes",
        type=int,
        default=int(os.environ.get("PAPERCOLOR_GATEWAY_POLL_MINUTES", "15")),
    )
    args = parser.parse_args()

    ssid = args.ssid or input("Wi-Fi SSID: ").strip()
    password = os.environ.get("PAPERCOLOR_WIFI_PASSWORD")
    if password is None:
        password = getpass.getpass("Wi-Fi password (empty for an open network): ")

    gateway_token = os.environ.get("PAPERCOLOR_DEVICE_TOKEN", "")
    if args.gateway_url and not gateway_token:
        gateway_token = getpass.getpass("PaperColor gateway device token: ")

    try:
        validate_credentials(ssid, password)
        validate_gateway(args.gateway_url, gateway_token, args.gateway_poll_minutes)
    except ValueError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2

    idf_path = os.environ.get("IDF_PATH")
    if not idf_path:
        print("error: IDF_PATH is unset; source the ESP-IDF environment first", file=sys.stderr)
        return 1

    if args.app_image is not None and not args.app_image.is_file():
        print(f"error: app image not found: {args.app_image}", file=sys.stderr)
        return 1

    generator = Path(idf_path) / "components/nvs_flash/nvs_partition_generator/nvs_partition_gen.py"
    if not generator.is_file():
        print(f"error: NVS generator not found under IDF_PATH: {generator}", file=sys.stderr)
        return 1

    print(f">>> Preparing credentials for SSID {ssid!r}; the password will not be printed.")
    print(">>> Long-press the side button until the USB download port appears.")

    old_umask = os.umask(0o077)
    try:
        with tempfile.TemporaryDirectory(prefix="papercolor-nvs-") as temp_dir:
            temp = Path(temp_dir)
            csv_path = temp / "wifi.csv"
            image_path = temp / "wifi-nvs.bin"
            with csv_path.open("w", encoding="utf-8", newline="") as handle:
                csv.writer(handle).writerows(
                    nvs_rows(
                        ssid,
                        password,
                        args.device_name,
                        args.gateway_url,
                        gateway_token,
                        args.gateway_poll_minutes,
                    )
                )

            try:
                run_checked(
                    [sys.executable, str(generator), "generate", str(csv_path), str(image_path), NVS_SIZE],
                    quiet=True,
                )
                port = wait_for_port(args.port, args.timeout)
                print(f">>> Download port: {port}; release the button.")
                flash_command = [
                        sys.executable,
                        "-m",
                        "esptool",
                        "--chip",
                        "esp32s3",
                        "--port",
                        port,
                        "--baud",
                        str(args.baud),
                        "--before",
                        "no_reset",
                        "--after",
                        "watchdog_reset",
                        "write_flash",
                        NVS_OFFSET,
                        str(image_path),
                    ]
                if args.app_image is not None:
                    flash_command.extend(["0x10000", str(args.app_image.resolve())])
                run_checked(flash_command)
            except (RuntimeError, TimeoutError) as exc:
                print(f"error: {exc}", file=sys.stderr)
                return 1
    finally:
        os.umask(old_umask)

    if args.app_image is None:
        print(">>> Wi-Fi and gateway settings were written to NVS; the application was started.")
    else:
        print(">>> Application plus NVS settings were verified and the new firmware was started.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
