#!/usr/bin/env python3
"""Regenerate the embedded resize PTX; nvcc is needed only for this step."""

import re
import subprocess
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "src" / "resize-kernel.cu"
OUTPUT = ROOT / "src" / "resize-ptx.inc"

with tempfile.TemporaryDirectory() as tmp:
    ptx_path = Path(tmp) / "resize.ptx"
    subprocess.run(
        ["nvcc", "-ptx", "-arch=compute_75", "-O3", "-o", str(ptx_path), str(SOURCE)],
        check=True,
    )
    ptx = ptx_path.read_text()

# The generated kernels use only instructions available on sm_30. Keep the
# embedded PTX compatible with the driver project's existing sm_30 kernels.
# Review instructions and JIT-test the result after updating the CUDA toolkit.
ptx, version_count = re.subn(r"(?m)^\.version \S+$", ".version 3.2", ptx)
ptx, target_count = re.subn(r"(?m)^\.target \S+$", ".target sm_30", ptx)
if version_count != 1 or target_count != 1:
    raise RuntimeError("unexpected nvcc PTX header")

OUTPUT.write_text(
    "/* Generated from resize-kernel.cu by tools/generate-resize-ptx.py. */\n"
    + "\n".join('"' + line.replace("\\", "\\\\").replace('"', '\\"') + '\\n"'
                for line in ptx.splitlines())
    + "\n"
)
