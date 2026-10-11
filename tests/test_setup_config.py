#!/usr/bin/env python3
"""Exercise the actual setup script with temporary config and mocked privileged commands."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[1]


class SetupConfigTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="linuxcampam-setup-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.config = self.root / "config.ini"
        self.log = self.root / "attributes.log"
        self.config.write_text("[Security]\nkde_lockscreen=auto\n")
        script = (REPO / "scripts/setup_config.sh").read_text()
        script = script.replace('CONFIG_FILE="/etc/linuxcampam/config.ini"', f'CONFIG_FILE="{self.config}"')
        self.script = self.root / "setup.sh"
        self.script.write_text(script)
        self.bin = self.root / "bin"
        self.bin.mkdir()
        self.mock("id", 'echo 0')
        self.mock("linuxcampam", 'exit 0')
        self.mock("lsattr", 'echo "----i--------- $1"')
        self.mock("chattr", 'echo "$1" >> "$TEST_ATTRIBUTE_LOG"; test "$1" != +i || test "${TEST_FAIL:-}" != restore')
        for command in ["cp", "mktemp", "chmod", "mv"]:
            self.mock(command, f'test "${{TEST_FAIL:-}}" != {command} || exit 1\nexec /usr/bin/{command} "$@"')
        self.mock("chown", 'test "${TEST_FAIL:-}" != chown')
        self.env = {**os.environ, "PATH": f"{self.bin}:/usr/bin:/bin", "TEST_ATTRIBUTE_LOG": str(self.log)}

    def mock(self, name, body):
        file = self.bin / name
        file.write_text("#!/bin/bash\n" + body + "\n")
        file.chmod(0o755)

    def run_setup(self, failure=""):
        return subprocess.run(["bash", str(self.script), "--configure-kde", "legacy"],
                              env={**self.env, "TEST_FAIL": failure}, capture_output=True, text=True)

    def test_success_preserves_sections_permissions_and_immutable_flag(self):
        self.config.write_text(" [Security] ; settings\nkde_lockscreen=auto\n [Other]\nkde_lockscreen=single_enter\n")
        self.config.chmod(0o640)
        result = self.run_setup()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("kde_lockscreen = legacy", self.config.read_text())
        self.assertIn("kde_lockscreen=single_enter", self.config.read_text())
        self.assertEqual(self.config.stat().st_mode & 0o777, 0o640)
        self.assertEqual(self.log.read_text().splitlines(), ["-i", "+i"])
        self.assertEqual(len(list(self.root.glob("*.bak"))), 1)
        self.assertEqual(list(self.root.glob(".config.ini.*")), [])

    def test_each_failure_restores_immutable_flag_and_removes_temporary_file(self):
        original = self.config.read_text()
        for operation in ["mktemp", "cp", "chmod", "chown", "mv"]:
            with self.subTest(operation=operation):
                self.log.unlink(missing_ok=True)
                result = self.run_setup(operation)
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(self.log.read_text().splitlines(), ["-i", "+i"])
                self.assertEqual(self.config.read_text(), original)
                self.assertEqual(list(self.root.glob(".config.ini.*")), [])

    def test_signal_restores_immutable_flag(self):
        self.mock("cp", 'kill -TERM "$PPID"; exit 143')
        result = self.run_setup()
        self.assertEqual(result.returncode, 143)
        self.assertEqual(self.log.read_text().splitlines(), ["-i", "+i"])
        self.assertEqual(list(self.root.glob(".config.ini.*")), [])

    def test_restore_failure_is_reported(self):
        result = self.run_setup("restore")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Cannot restore immutable flag", result.stderr)

    def test_unlocked_config_stays_unlocked(self):
        self.mock("lsattr", 'echo "-------------- $1"')
        self.assertEqual(self.run_setup().returncode, 0)
        self.assertFalse(self.log.exists())


if __name__ == "__main__":
    unittest.main()
