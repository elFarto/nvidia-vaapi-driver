#!/usr/bin/env python3
"""Launcher tests using temporary XDG directories; no driver installation."""

import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("launcher", ROOT / "scripts/chrome-launcher.py")
launcher = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(launcher)
TEMPLATE = """[Desktop Entry]
Type=Application
Name=Google Chrome
Name[ja]=Chrome
Exec=/usr/bin/google-chrome-stable %U
DBusActivatable=true
Actions=new-window;private;

[Desktop Action new-window]
Name=New Window
Exec=/usr/bin/google-chrome-stable --enable-features=ExistingFeature --ozone-platform=wayland

[Desktop Action private]
Name=Incognito
Exec=/usr/bin/google-chrome-stable --incognito %U
"""


class LauncherTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.user = self.root / "user data"
        self.system = self.root / "system data"
        self.target = self.user / "applications/google-chrome.desktop"
        self.template = self.system / "applications/google-chrome.desktop"
        self.template.parent.mkdir(parents=True)
        self.template.write_text(TEMPLATE)
        self.driver = self.root / "driver"
        self.driver.mkdir()
        env = patch.dict(os.environ, {
            "XDG_DATA_HOME": str(self.user),
            "XDG_DATA_DIRS": str(self.user) + ":" + str(self.system),
            "NVD_DRIVER_DIR": str(self.driver),
        })
        env.start()
        self.addCleanup(env.stop)

    def configure(self, restore=False):
        return launcher.configure("google-chrome.desktop", self.driver, restore)

    def commands(self):
        return [launcher.parse_exec(line[5:]) for line in self.target.read_text().splitlines() if line.startswith("Exec=")]

    def test_create_all_actions(self):
        target, backup = self.configure()
        self.assertEqual(target, self.target)
        self.assertIsNone(backup)
        self.assertEqual(self.template.read_text(), TEMPLATE)
        commands = self.commands()
        self.assertEqual(len(commands), 3)
        for command in commands:
            self.assertEqual(command[:5], ["/usr/bin/env", "LIBVA_DRIVER_NAME=nvidia",
                "LIBVA_DRIVERS_PATH=" + str(self.driver), "NVD_BACKEND=direct", "/usr/bin/google-chrome-stable"])
            self.assertIn("--use-angle=gl", command)
            self.assertFalse(any("NVD_EXPORT_LAYOUT" in arg for arg in command))
        self.assertEqual(commands[0][-1], "%U")
        self.assertIn("--incognito", commands[2])
        self.assertIn("--enable-features=ExistingFeature,AcceleratedVideoDecodeLinuxGL,VaapiOnNvidiaGPUs", commands[1])
        self.assertIn("--ozone-platform=wayland", commands[1])
        self.assertIn("DBusActivatable=false", target.read_text())
        self.assertIn("Name[ja]=Chrome", target.read_text())

    def test_restore_backups_and_other_entries(self):
        self.target.parent.mkdir(parents=True)
        broken = b"[Desktop Entry]\nExec=/opt/broken-hotpatch %U\n"
        self.target.write_bytes(broken)
        other = self.target.with_name("chromium.desktop")
        other.write_bytes(broken)
        with self.assertRaisesRegex(ValueError, "already exists"):
            self.configure()
        self.assertEqual(self.target.read_bytes(), broken)
        _, first = self.configure(restore=True)
        self.assertEqual(Path(first).read_bytes(), broken)
        restored = self.target.read_bytes()
        _, second = self.configure(restore=True)
        self.assertNotEqual(first, second)
        self.assertEqual(Path(first).read_bytes(), broken)
        self.assertEqual(Path(second).read_bytes(), restored)
        self.assertEqual(self.target.read_bytes(), restored)
        self.assertEqual(other.read_bytes(), broken)

    def test_backup_failure_leaves_original(self):
        self.target.parent.mkdir(parents=True)
        self.target.write_text("original")
        with patch.object(launcher.tempfile, "mkstemp", side_effect=OSError("backup denied")):
            with self.assertRaisesRegex(OSError, "backup denied"):
                self.configure(restore=True)
        self.assertEqual(self.target.read_text(), "original")

    def test_write_failure_leaves_original_and_backup(self):
        self.target.parent.mkdir(parents=True)
        self.target.write_text("original")
        with patch.object(launcher.os, "replace", side_effect=OSError("replace denied")):
            with self.assertRaisesRegex(OSError, "replace denied"):
                self.configure(restore=True)
        self.assertEqual(self.target.read_text(), "original")
        backups = list(self.target.parent.glob("*.nvidia-vaapi-backup-*"))
        self.assertEqual(len(backups), 1)
        self.assertEqual(backups[0].read_text(), "original")
        self.assertFalse(list(self.target.parent.glob(".nvidia-vaapi-*")))

    def test_missing_template_does_not_use_override(self):
        self.target.parent.mkdir(parents=True)
        self.target.write_text("original")
        self.template.unlink()
        with self.assertRaisesRegex(ValueError, "No system template"):
            self.configure(restore=True)
        self.assertEqual(self.target.read_text(), "original")

    def test_unsupported_templates_leave_original(self):
        self.target.parent.mkdir(parents=True)
        self.target.write_text("original")
        for text in (
            TEMPLATE.replace("/usr/bin/google-chrome-stable", "/usr/bin/flatpak run com.google.Chrome"),
            TEMPLATE + "X-Flatpak=com.google.Chrome\n",
            TEMPLATE.replace("/usr/bin/google-chrome-stable", "/snap/bin/chromium"),
            TEMPLATE.replace("Type=Application", "Type=Link"),
            TEMPLATE.replace("Exec=", "NoExec="),
            TEMPLATE.replace("/usr/bin/google-chrome-stable", "/opt/unknown-wrapper"),
        ):
            with self.subTest(template=text):
                self.template.write_text(text)
                with self.assertRaises(ValueError):
                    self.configure(restore=True)
                self.assertEqual(self.target.read_text(), "original")

    def test_invalid_filename_and_paths(self):
        for name in ("../google-chrome.desktop", "/tmp/google-chrome.desktop", "unknown.desktop"):
            with self.assertRaises(ValueError):
                launcher.configure(name, self.driver)
        with self.assertRaises(ValueError):
            launcher.configure("google-chrome.desktop", "relative")
        self.assertFalse(self.target.exists())

    def test_exec_escaping(self):
        literal = 'LIBVA_DRIVERS_PATH=/tmp/driver space\'"`$\\%%folder'
        self.assertEqual(launcher.parse_exec(launcher.quote_exec(literal)), [literal])
        for value in ('"unfinished', 'chrome --bad=%Z', 'chrome "x"tail', 'chrome a;b'):
            with self.subTest(value=value), self.assertRaises(ValueError):
                launcher.parse_exec(value)

    def test_installer_modes_do_not_build(self):
        # Trap any attempt to build/install or obtain privileges.
        fake_bin = self.root / "bin"
        fake_bin.mkdir()
        marker = self.root / "unexpected-build"
        for name in ("meson", "sudo", "pkg-config"):
            script = fake_bin / name
            script.write_text("#!/bin/sh\ntouch '" + str(marker) + "'\nexit 99\n")
            script.chmod(0o755)
        env = dict(os.environ, PATH=str(fake_bin) + ":" + os.environ["PATH"])
        for mode in ("--configure-chrome-launcher", "--restore-chrome-launcher"):
            result = subprocess.run([str(ROOT / "install.sh"), mode, "google-chrome.desktop"], env=env, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
        for args in (
            ["--restore-chrome-launcher"],
            ["--restore-chrome-launcher", "../bad"],
            ["--restore-chrome-launcher", "google-chrome.desktop", "--clean"],
            ["--configure-chrome-launcher", "google-chrome.desktop", "--deps"],
            ["--configure-chrome-launcher", "google-chrome.desktop", "--restore-chrome-launcher", "google-chrome.desktop"],
        ):
            result = subprocess.run([str(ROOT / "install.sh"), *args], env=env, capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0, args)
        self.assertFalse(marker.exists())

    @unittest.skipUnless(shutil.which("desktop-file-validate"), "desktop-file-utils unavailable")
    def test_desktop_validation(self):
        self.driver = self.root / 'driver spaces\'"`$\\%folder'
        self.configure()
        subprocess.run(["desktop-file-validate", str(self.target)], check=True, capture_output=True)

    @unittest.skipUnless(shutil.which("gio"), "GIO unavailable")
    def test_native_launcher_argument_delivery(self):
        # An actual desktop launcher checks both escaping layers independently
        # of our parser. The fake browser records argv/environment, not a GUI.
        capture = self.root / "capture.json"
        browser_dir = self.root / 'browser space\'"`$\\folder'
        browser_dir.mkdir()
        browser = browser_dir / "chrome"
        browser.write_text("#!/usr/bin/env python3\nimport os,json,sys\n"
            + "open(" + repr(str(capture)) + ", 'w').write(json.dumps({'args':sys.argv[1:], 'driver':os.environ['LIBVA_DRIVERS_PATH']}))\n")
        browser.chmod(0o755)
        self.driver = self.root / 'driver space\'"`$\\%folder'
        text = TEMPLATE.replace("/usr/bin/google-chrome-stable", launcher.quote_exec(str(browser)))
        self.template.write_text(text)
        self.configure()
        url = "https://example.test/?x=1&y=2"
        result = subprocess.run(["gio", "launch", str(self.target), url], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        for _ in range(50):
            if capture.exists():
                break
            time.sleep(0.1)
        self.assertTrue(capture.exists(), result.stderr)
        data = json.loads(capture.read_text())
        self.assertEqual(data["driver"], str(self.driver))
        self.assertIn(url, data["args"])
        self.assertIn("--enable-features=AcceleratedVideoDecodeLinuxGL,VaapiOnNvidiaGPUs", data["args"])


if __name__ == "__main__":
    unittest.main()
