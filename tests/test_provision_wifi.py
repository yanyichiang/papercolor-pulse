import importlib.util
import unittest
from pathlib import Path
from unittest import mock


SPEC = importlib.util.spec_from_file_location(
    "provision_wifi", Path(__file__).parents[1] / "tools" / "provision_wifi.py"
)
assert SPEC and SPEC.loader
provision_wifi = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(provision_wifi)


class ProvisionWifiTest(unittest.TestCase):
    def test_validates_wifi_limits(self):
        provision_wifi.validate_credentials("home", "12345678")
        provision_wifi.validate_credentials("open-network", "")
        with self.assertRaises(ValueError):
            provision_wifi.validate_credentials("x" * 33, "12345678")
        with self.assertRaises(ValueError):
            provision_wifi.validate_credentials("home", "short")

    def test_nvs_rows_keep_credentials_in_string_fields(self):
        rows = provision_wifi.nvs_rows(
            "wifi,with,commas",
            "quoted\"password",
            "papercolor",
            "http://127.0.0.1:8767",
            "device-token-1234567890",
            15,
        )
        values = {row[0]: row[3] for row in rows[1:]}
        self.assertEqual(values["wifi_ssid"], "wifi,with,commas")
        self.assertEqual(values["wifi_pass"], 'quoted"password')
        self.assertEqual(values["cur_mode"], "mode_1")
        self.assertEqual(values["gw_enabled"], "1")
        self.assertEqual(values["gw_poll"], "15")
        self.assertEqual(values["low_power"], "1")

    def test_gateway_validation_requires_bounded_token(self):
        provision_wifi.validate_gateway("http://127.0.0.1:8767", "x" * 16, 15)
        provision_wifi.validate_gateway("", "", 15)
        with self.assertRaises(ValueError):
            provision_wifi.validate_gateway("http://127.0.0.1:8767?token=bad", "x" * 16, 15)
        with self.assertRaises(ValueError):
            provision_wifi.validate_gateway("http://127.0.0.1:8767", "short", 15)
        with self.assertRaises(ValueError):
            provision_wifi.validate_gateway("", "x" * 16, 15)

    def test_run_checked_raises_without_echoing_secret_arguments(self):
        completed = mock.Mock(returncode=1)
        with mock.patch.object(provision_wifi.subprocess, "run", return_value=completed):
            with self.assertRaisesRegex(RuntimeError, "command failed with exit code 1: python") as raised:
                provision_wifi.run_checked(["python", "secret-value"], quiet=True)
        self.assertNotIn("secret-value", str(raised.exception))


if __name__ == "__main__":
    unittest.main()
