#!/usr/bin/env python3
"""Create an opt-in native Chrome/Chromium desktop override from a system entry."""

import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile


DESKTOP_NAMES = (
    "google-chrome.desktop", "google-chrome-stable.desktop",
    "com.google.Chrome.desktop", "chromium.desktop", "chromium-browser.desktop",
)
BROWSERS = {"google-chrome", "google-chrome-stable", "chrome", "chromium", "chromium-browser"}
FEATURES = ("AcceleratedVideoDecodeLinuxGL", "VaapiOnNvidiaGPUs")
SWITCHES = ("--ignore-gpu-blocklist", "--use-gl=angle", "--use-angle=gl")
RESERVED = set(' \t\n\r"\'\\><~|&;$*?#()`')
FIELD_CODES = {"%f", "%F", "%u", "%U", "%i", "%c", "%k"}


def parse_exec(value):
    # Desktop entries have two escaping layers: string values, then Exec
    # double-quoted arguments. Shell parsing (including single quotes) differs.
    # https://specifications.freedesktop.org/desktop-entry/latest/exec-variables.html
    escapes = {"s": " ", "n": "\n", "t": "\t", "r": "\r", "\\": "\\"}

    def unescape(match):
        if match[1] not in escapes:
            raise ValueError("Invalid desktop string escape")
        return escapes[match[1]]

    value = re.sub(r"\\(.)", unescape, value)

    def unquote(match):
        if match[1] not in '"`$\\':
            raise ValueError("Invalid quoted Exec escape")
        return match[1]

    args = []
    while value:
        value = value.lstrip()
        if not value:
            break
        match = re.match(r'"((?:\\.|[^"\\])*)"|([^\s]+)', value)
        if not match:
            raise ValueError("Invalid Exec quoting")
        if match[1] is not None:
            arg = re.sub(r'\\(.)', unquote, match[1])
        else:
            arg = match[2]
            if RESERVED.intersection(arg):
                raise ValueError("Reserved Exec characters must be double-quoted")
        value = value[match.end():]
        if value and not value[0].isspace():
            raise ValueError("Exec arguments must be separated by whitespace")
        if re.search(r"%(?!%)", arg.replace("%%", "")) and arg not in FIELD_CODES:
            raise ValueError("Only standalone desktop field codes are supported")
        args.append(arg)
    return args


def quote_exec(arg):
    """Encode one argument, preserving existing desktop field codes."""
    if not arg or RESERVED.intersection(arg):
        arg = '"' + re.sub(r'(["`$\\])', r'\\\1', arg) + '"'
    return arg.replace("\\", "\\\\").replace("\n", r"\n").replace("\t", r"\t").replace("\r", r"\r")


def configure_exec(value, driver_dir):
    args = parse_exec(value)
    if not args or Path(args[0]).name not in BROWSERS or "=" in args[0]:
        raise ValueError("System template must launch a native Chrome/Chromium executable directly")
    if args[0].startswith("/snap/") or any(arg == "--file-forwarding" or arg.startswith("@@") for arg in args):
        raise ValueError("Sandboxed browser launchers are not supported")
    features = []
    remaining = []
    for arg in args[1:]:
        if arg.startswith("--enable-features="):
            features.extend(filter(None, arg.split("=", 1)[1].split(",")))
        elif arg.split("=", 1)[0] not in {s.split("=", 1)[0] for s in SWITCHES}:
            remaining.append(arg)
    for feature in FEATURES:
        if feature not in features:
            features.append(feature)
    command = [
        "/usr/bin/env", "LIBVA_DRIVER_NAME=nvidia",
        "LIBVA_DRIVERS_PATH=" + str(driver_dir).replace("%", "%%"),
        "NVD_BACKEND=direct", args[0],
        "--enable-features=" + ",".join(features), *SWITCHES, *remaining,
    ]
    return " ".join(quote_exec(arg) for arg in command)


def configure_template(text, driver_dir):
    lines = []
    section = ""
    main_exec = False
    application = False
    for line in text.splitlines():
        if line.startswith("[") and line.endswith("]"):
            section = line[1:-1]
        if line.startswith(("X-Flatpak=", "X-SnapInstanceName=")):
            raise ValueError("Sandboxed browser launchers are not supported")
        if section == "Desktop Entry" and line == "Type=Application":
            application = True
        if section == "Desktop Entry" or section.startswith("Desktop Action "):
            if line.startswith("Exec="):
                line = "Exec=" + configure_exec(line[5:], driver_dir)
                if section == "Desktop Entry":
                    main_exec = True
            elif line.startswith("DBusActivatable="):
                # D-Bus activation would bypass the environment in Exec.
                line = "DBusActivatable=false"
        lines.append(line)
    if not application or not main_exec:
        raise ValueError("Template must contain an Application desktop entry with Exec")
    return "\n".join(lines) + "\n"


def configure(name, driver_dir, restore=False):
    if name not in DESKTOP_NAMES:
        raise ValueError("Unsupported desktop filename: " + name)
    driver_dir = Path(driver_dir)
    user_data = Path(os.environ.get("XDG_DATA_HOME") or Path.home() / ".local/share")
    if not driver_dir.is_absolute() or not user_data.is_absolute():
        raise ValueError("Driver directory and XDG_DATA_HOME must be absolute paths")
    target = user_data / "applications" / name
    previous = target.read_bytes() if target.exists() or target.is_symlink() else None
    if previous is not None and not restore:
        raise ValueError("User launcher already exists; use --restore-chrome-launcher to back it up and replace it")
    template = None
    for directory in (os.environ.get("XDG_DATA_DIRS") or "/usr/local/share:/usr/share").split(":"):
        candidate = Path(directory) / "applications" / name
        if not candidate.is_absolute() or candidate.resolve() == target.resolve():
            continue
        if candidate.is_file():
            template = candidate
            break
    if template is None:
        raise ValueError("No system template found for " + name)
    result = configure_template(template.read_text(encoding="utf-8"), driver_dir)

    target.parent.mkdir(parents=True, exist_ok=True)
    backup = None
    if previous is not None:
        fd, backup = tempfile.mkstemp(prefix=name + ".nvidia-vaapi-backup-", dir=target.parent)
        with os.fdopen(fd, "wb") as stream:
            stream.write(previous)
        print("Backed up existing launcher to " + backup)
    fd, temporary = tempfile.mkstemp(prefix=".nvidia-vaapi-", dir=target.parent)
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as stream:
            stream.write(result)
            os.fchmod(stream.fileno(), 0o644)
        if restore:
            os.replace(temporary, target)
        else:
            # Exclusive creation: do not overwrite a concurrently created entry.
            os.link(temporary, target)
    finally:
        Path(temporary).unlink(missing_ok=True)
    if shutil.which("update-desktop-database"):
        try:
            subprocess.run(["update-desktop-database", str(target.parent)], check=False)
        except OSError as error:
            print("Could not refresh the desktop database: " + str(error), file=sys.stderr)
    print("Configured " + str(target) + "; fully restart Chrome to use these settings.")
    return target, backup


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("desktop", choices=DESKTOP_NAMES)
    parser.add_argument("--driver-dir", required=True)
    parser.add_argument("--restore", action="store_true")
    args = parser.parse_args()
    try:
        configure(args.desktop, args.driver_dir, args.restore)
    except (OSError, ValueError) as error:
        parser.exit(1, "Chrome launcher unchanged: " + str(error) + "\n")


if __name__ == "__main__":
    main()
