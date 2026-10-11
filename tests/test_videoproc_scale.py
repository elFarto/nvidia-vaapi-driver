#!/usr/bin/env python3
"""GPU smoke test for VideoProc scaling and readback.

Run after building the driver: python3 tests/test_videoproc_scale.py [build-dir]
Requires ffmpeg with libx264 and an accessible NVIDIA render node.
"""

import os
import signal
import subprocess
import sys
import tempfile
from pathlib import Path


def run(command, env, timeout=15):
    process = subprocess.Popen(
        command, env=env, start_new_session=True,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    )
    try:
        out, err = process.communicate(timeout=timeout)
    except subprocess.TimeoutExpired as exc:
        os.killpg(process.pid, signal.SIGKILL)
        process.communicate()
        raise RuntimeError(f"timed out: {' '.join(command)}") from exc
    if process.returncode:
        raise RuntimeError(
            f"exit {process.returncode}: {' '.join(command)}\n"
            f"{err.decode(errors='replace')[-2000:]}"
        )
    return out


def frames(output):
    return [line for line in output.splitlines() if line.startswith(b"0,")]


def main():
    repo = Path(__file__).resolve().parents[1]
    build = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else repo / "build-release"
    env = os.environ.copy()
    env.update(LIBVA_DRIVER_NAME="nvidia", LIBVA_DRIVERS_PATH=str(build),
               NVD_LOG_VERBOSE="0")
    with tempfile.TemporaryDirectory(prefix="nvd-videoproc-") as tmp:
        source = Path(tmp) / "source.mp4"
        env["NVD_LOG"] = str(Path(tmp) / "driver.log")
        run([
            "ffmpeg", "-nostdin", "-hide_banner", "-loglevel", "error", "-y",
            "-f", "lavfi", "-i", "smptebars=duration=3:size=640x360:rate=30",
            "-c:v", "libx264", "-bf", "0", "-g", "30", "-pix_fmt", "yuv420p",
            str(source),
        ], env)

        decode_input = [
            "ffmpeg", "-nostdin", "-hide_banner", "-loglevel", "error",
            "-hwaccel", "vaapi", "-hwaccel_device", "/dev/dri/renderD128",
        ]
        readback_and_scale = decode_input + [
            "-hwaccel_output_format", "vaapi", "-i", str(source),
            "-vf", "scale_vaapi=w=320:h=180,hwdownload,format=nv12",
            "-f", "framemd5", "-",
        ]
        scaled_output = run(readback_and_scale, env)
        if b"#dimensions 0: 320x180" not in scaled_output or len(frames(scaled_output)) != 90:
            raise RuntimeError("VideoProc scaling/readback did not produce 90 320x180 frames")

        # A frame count alone also accepts a scaler that writes blank frames.
        # Compare the first GPU-scaled frame with FFmpeg's software bilinear
        # result. The algorithms differ slightly at sharp edges, so compare
        # their average byte difference rather than demanding exact equality.
        gpu_frame = run(decode_input + [
            "-hwaccel_output_format", "vaapi", "-i", str(source),
            "-vf", "scale_vaapi=w=320:h=180,hwdownload,format=nv12",
            "-frames:v", "1", "-f", "rawvideo", "-",
        ], env)
        software_frame = run([
            "ffmpeg", "-nostdin", "-hide_banner", "-loglevel", "error",
            "-i", str(source), "-vf", "scale=320:180:flags=bilinear,format=nv12",
            "-frames:v", "1", "-f", "rawvideo", "-",
        ], env)
        if len(gpu_frame) != 320 * 180 * 3 // 2 or len(gpu_frame) != len(software_frame):
            raise RuntimeError("unexpected scaled frame size")
        mean_difference = sum(abs(a - b) for a, b in zip(gpu_frame, software_frame)) / len(gpu_frame)
        if mean_difference > 3:
            raise RuntimeError(f"GPU resize differs from software bilinear: mean byte difference {mean_difference:.2f}")

    print(f"90 scaled readback frames passed; mean pixel difference {mean_difference:.2f}")


if __name__ == "__main__":
    main()
