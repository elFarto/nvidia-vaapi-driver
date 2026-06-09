# nvidia-vaapi-driver

This is a VA-API implementation that uses NVDEC for decode and includes experimental NVENC slice encode support (H.264/HEVC/AV1). This implementation is specifically designed to be used by Firefox for accelerated decode of web content, and may not operate correctly in other applications.

# Table of contents

- [nvidia-vaapi-driver](#nvidia-vaapi-driver)
- [Table of contents](#table-of-contents)
- [Codec Support](#codec-support)
- [Installation](#installation)
  - [Packaging status](#packaging-status)
  - [Building](#building)
  - [Removal](#removal)
- [Configuration](#configuration)
  - [Upstream regressions](#upstream-regressions)
  - [Kernel parameters](#kernel-parameters)
  - [Environment Variables](#environment-variables)
  - [Firefox](#firefox)
  - [Chrome](#chrome)
  - [MPV](#mpv)
  - [Direct Backend](#direct-backend)
- [Testing](#testing)

# Codec Support

Hardware decoding is the primary target. Experimental H.264/HEVC/AV1 slice encode (`VAEntrypointEncSlice`) is available.

Encode entrypoints are capability-gated at runtime (NVENC GUID/CAPS probes), so `vainfo` is the source of truth for the exact profile list on your system.

Current encode limitations:

- Implemented encode profiles:
  - H.264: Constrained Baseline / Main / High, plus High10 when NVENC API and hardware support it.
  - HEVC: Main / Main10 / Main422_10 / Main444 / Main444_10 (capability-dependent).
  - AV1: Profile 0 (8-bit always; 10-bit input when capability is available).
- Capability-dependent paths:
  - H.264 High may expose YUV444 input (`VA_RT_FORMAT_YUV444`) when NVENC `YUV444` capability is available.
  - H.264 High10 and HEVC Main422_10 require newer `nv-codec-headers` (NVENC API 13+).
  - HEVC Main422_10 requires NVENC `YUV422` capability.
  - AV1 Profile 0 may expose 10-bit 4:2:0 input (`VA_RT_FORMAT_YUV420_10`) when NVENC `10BIT_ENCODE` capability is available.
- HEVC Main12 and Main444_12 are decode-only (NVENC public API does not expose 12-bit HEVC encode).
- VP8/VP9/MPEG-2/VC-1/MPEG-4/JPEG/MJPEG encode is not supported.
- Rate control modes:
  - CBR / VBR / CQP are implemented.
  - CBR/VBR parse `VAEncMiscParameterRateControl` (`bits_per_second`, `target_percentage`, `initial_qp`, `min_qp`, `max_qp`) and map to NVENC bitrate/QP bounds.
  - CQP maps to NVENC `CONSTQP` and applies QP hints from misc/picture/slice parameters where provided.
- Client-side format negotiation caveat:
  - Some ffmpeg paths may renegotiate HEVC rext + `yuv444p10le` to `yuv420p10le` before frames reach the driver. Verify effective output with `ffprobe`.
- Intended for transcoding-style workloads; broader VA-API encode coverage is not implemented.

| Codec | Supported | Comments |
|---|---|---|
|AV1|:heavy_check_mark:|Firefox 98+ is required.|
|H.264|:heavy_check_mark:||
|HEVC|:heavy_check_mark:|Some distros are shipping Firefox and/or FFMPEG with HEVC support disabled due to patent concerns.|
|VP8|:heavy_check_mark:||
|VP9|:heavy_check_mark:|Requires being compiled with `gstreamer-codecparsers-1.0`|
|MPEG-2|:heavy_check_mark:||
|VC-1|:heavy_check_mark:||
|MPEG-4|:x:|VA-API does not supply enough of the original bitstream to allow NVDEC to decode it.|
|JPEG|:x:|This is unlikely to ever work, the two APIs are too different.|

YUV444 paths are supported but generally require:

- \>= Turing (20XX/16XX)
- profile/capability support on the target GPU
- Direct backend

To view which codecs and entrypoints your card is currently advertising (decode `VAEntrypointVLD` and encode `VAEntrypointEncSlice`), use `vainfo` with this driver installed, or visit the NVIDIA website [here](https://developer.nvidia.com/video-encode-and-decode-gpu-support-matrix-new#geforce).

# Installation

To install and use `nvidia-vaapi-driver`, follow the steps in installation and configuration. It is recommended to follow testing as well to verify hardware acceleration is working as intended.

**Requirements**

- NVIDIA driver series 470 or 500+

## Packaging status

<p align="top"><a href="https://repology.org/project/nvidia-vaapi-driver/versions"><img src="https://repology.org/badge/vertical-allrepos/nvidia-vaapi-driver.svg" alt="repology"><a href="https://repology.org/project/libva-nvidia-driver/versions"><img src="https://repology.org/badge/vertical-allrepos/libva-nvidia-driver.svg" alt="repology" align="top" width="%"></p>

[pkgs.org/nvidia-vaapi-driver](https://pkgs.org/search/?q=nvidia-vaapi-driver) [pkgs.org/libva-nvidia-driver](https://pkgs.org/search/?q=libva-nvidia-driver)

openSUSE: [1](https://software.opensuse.org/package/nvidia-vaapi-driver), [2](https://software.opensuse.org/package/libva-nvidia-driver).

Feel free to add your distributions package in an issue/PR, if it isn't on these websites.

## Building

You'll need `meson`, the `gstreamer-plugins-bad` library, and [`nv-codec-headers`](https://git.videolan.org/?p=ffmpeg/nv-codec-headers.git) installed.

For newer encode paths (for example H.264 High10 and HEVC Main422_10), use recent `nv-codec-headers` (NVENC API 13+).

| Package manager | Packages                                        | Optional packages for additional codec support |
|-----------------|-------------------------------------------------|------------------------------------------------|
| pacman          | meson gst-plugins-bad ffnvcodec-headers         |                                                |
| apt             | meson gstreamer1.0-plugins-bad libffmpeg-nvenc-dev libva-dev libegl-dev libdrm-dev | libgstreamer-plugins-bad1.0-dev   |
| yum/dnf         | meson libva-devel gstreamer1-plugins-bad-freeworld nv-codec-headers libdrm-devel | gstreamer1-plugins-bad-free-devel |

Then run the following commands:

```sh
meson setup build
meson install -C build
```

## Removal

By default the driver installs itself as `/usr/lib64/dri/nvidia_drv_video.so` (this might be `/usr/lib/x86_64-linux-gnu/dri/nvidia_drv_video.so` on some distros). To uninstall the driver, simply remove this file. In addition, this file is usually symlinked to `/usr/lib64/dri/vdpau_drv_video.so` (or `/usr/lib/x86_64-linux-gnu/dri/vdpau_drv_video.so`) if the VDPAU to VA-API driver is installed, so this symlink will need to be restored for that driver to work normally again.

# Configuration

## Upstream regressions

The EGL backend is broken on driver versions 525 or later due to a regression. Users running these drivers should use the [direct backend](#direct-backend) instead.

For more information read the [upstream bug report](https://forums.developer.nvidia.com/t/cueglstreamproducerconnect-returns-error-801-on-525-53-driver/233610) or [issue #126](/../../issues/126).

## Kernel parameters

This library requires that the `nvidia_drm` kernel module is [configured with the parameter](https://wiki.archlinux.org/title/Kernel_parameters) `nvidia-drm.modeset=1`

## Environment Variables

Environment variables used to control the behavior of this library.

| Variable | Purpose |
|---|---|
| `NVD_LOG` | Used to control logging. `1` to log to stdout, anything else to append to the given file. |
| `NVD_MAX_INSTANCES` | Controls the maximum concurrent instances of the driver will be allowed per-process. This option is only really useful for older GPUs with not much VRAM, especially with Firefox on video heavy websites. |
| `NVD_BACKEND` | Controls which backend this library uses. Either `egl`, or `direct` (default). See [direct backend](#direct-backend) for more details. |
| `NVD_ENCODE_PROBE_CACHE` | Controls encode capability probe cache usage. Default enabled. Set `0` to disable both in-process and persistent probe caches. |
| `NVD_PREFER_GPU_COPY_IMPORT` | Direct backend import policy override. Prefer GPU-copy path over direct-import for external DRM PRIME surfaces. |
| `NVD_FORCE_CPU_COPY_IMPORT` | Direct backend import policy override. Force CPU-copy fallback path for external DRM PRIME surface import. |
| `NVD_DIRECT_IMPORT_444P_MODE` | Direct backend 444P external import policy override. One of `auto`, `buffer`, `array`, or `gpu-copy`. |
| `NVD_PREFER_DIRECT_DMABUF_CUDA_IMPORT` | Allow trying direct raw dma-buf CUDA import even when startup capability probe does not report support. |
| `NVD_DISABLE_DIRECT_DMABUF_CUDA_IMPORT` | Disable direct raw dma-buf CUDA import path and use fallback import paths. |
| `NVD_USE_PRIMARY_CUDA_CONTEXT` | Prefer CUDA primary context (`cuDevicePrimaryCtxRetain`) instead of creating a private context. If unset, the driver still auto-retries with primary context when `cuCtxCreate` fails. |
| `NVD_DRM_PRIME_ASSUME_LINEAR` | Opt-in compatibility mode for plain `DRM_PRIME` external imports that do not carry explicit modifiers. When set to `1`, the driver admits plain `DRM_PRIME` input and reconstructs it as `DRM_FORMAT_MOD_LINEAR`. Default disabled. |
| `NVD_DECODE_SURFACE_COUNT` | Override the default decode surface count used when clients create a decode context without render targets. Valid range is `1` to `32`; default is `32`. |
| `NVD_ENABLE_CLIENT_PACKED_HEADERS` | Opt into prepending client-supplied H.264/HEVC packed header data. Default disabled because NVENC-generated SPS/PPS is safer for NVENC slice output. |
| `NVD_DISABLE_CLIENT_PACKED_HEADERS` | Deprecated compatibility spelling. Client-supplied H.264/HEVC packed header data is ignored by default while packed-header capability advertisement remains enabled. |

### `NVD_ENCODE_PROBE_CACHE`

Controls caching for NVENC capability probing during driver initialization.

- `1` (default): enable both in-process cache and persistent cache file.
- `0`: disable cache and run the full NVENC capability probe on each initialization.

Example:

```bash
NVD_ENCODE_PROBE_CACHE=0 vainfo --display drm --device /dev/dri/renderD128
```

### `NVD_DRM_PRIME_ASSUME_LINEAR`

Controls the driver's behavior when a client uses plain `DRM_PRIME`
(`VASurfaceAttribExternalBuffers`) instead of `DRM_PRIME_2`.

Plain `DRM_PRIME` does not carry explicit DRM modifier metadata. By default, the
driver rejects such imports to avoid rendering corrupted output from tiled or
otherwise non-linear buffers.

- unset / `0` (default): reject plain `DRM_PRIME` external imports and require
  `DRM_PRIME_2`
- `1`: admit plain `DRM_PRIME` external imports and treat them as
  `DRM_FORMAT_MOD_LINEAR`

This is only safe for callers that are known to provide truly linear buffers.
Do not assume it is correct for all browsers or compositors.

Example:

```bash
NVD_DRM_PRIME_ASSUME_LINEAR=1 \
LIBVA_DRIVER_NAME=nvidia \
LIBVA_DRIVERS_PATH=/path/to/nvidia-vaapi-driver/build \
vainfo --display drm --device /dev/dri/renderD128
```

### `NVD_DIRECT_IMPORT_444P_MODE`

Controls the direct backend external-import path for `444P` surfaces.

- unset (default): use `gpu-copy`, matching the previous default behavior
- `auto`: allow direct import and let the driver choose the current best path
- `buffer`: force buffer-direct import for linear `444P`; if the layout is non-linear or the import fails, the driver falls back to GPU-copy
- `array`: force CUDA array direct-import (`CUmipmappedArray` path); if that import fails, the driver falls back to GPU-copy
- `gpu-copy`: bypass direct-import and use GPU-copy import immediately

## Firefox

Due to license, Firefox on Linux does not support HEVC till now.
To use the driver with firefox you will need at least Firefox 96, `ffmpeg` compiled with vaapi support (`ffmpeg -hwaccels` output should include vaapi), and the following config options need to be set in the `about:config` page:

| Option | Value | Reason |
|---|---|---|
| media.ffmpeg.vaapi.enabled | true | Required until Firefox 137, enables the use of VA-API. |
| media.hardware-video-decoding.force-enabled | true | Required since Firefox 137, enables hardware acceleration. |
| media.rdd-ffmpeg.enabled | true | Required, default on FF97. Forces ffmpeg usage into the RDD process, rather than the content process. |
| media.av1.enabled | false | Optional, disables AV1. If your GPU doesn't support AV1, this will prevent sites using it and falling back to software decoding. |
| gfx.x11-egl.force-enabled | true | Required, this driver requires that Firefox use the EGL backend. It may be enabled by default. It is recommended to test it with the `MOZ_X11_EGL=1` environment variable before enabling it in the Firefox configuration. |
| widget.dmabuf.force-enabled | true | Required on NVIDIA 470 series drivers. Note that Firefox isn't coded to allow DMA-BUF support without GBM support, so it may not function completely correctly when it's forced on. |

In addition the following environment variables need to be set. For permanent configuration `/etc/environment` may suffice.

| Variable | Value | Reason |
|---|---|---|
| MOZ_DISABLE_RDD_SANDBOX | 1 | Disables the sandbox for the RDD process that the decoder runs in. |
| LIBVA_DRIVER_NAME | nvidia | Required for libva 2.20+, forces libva to load this driver. |
| __EGL_VENDOR_LIBRARY_FILENAMES | /usr/share/glvnd/egl_vendor.d/10_nvidia.json | Required for the 470 driver series only. It overrides the list of drivers the glvnd library can use to prevent Firefox from using the MESA driver by mistake. |
| CUDA_DISABLE_PERF_BOOST | 1 | Optional. Requires NVIDIA driver >= 580.105.08. Disables the forced power boost the GPU gets when CUDA is activated. This should reduce the power usage when decoding video. This setting is the equivilent of the 'CUDA Force P2' NVIDIA Profile Inspector setting on Windows. |

When libva is used it will log out some information, which can be excessive when Firefox initalises it multiple times per page. This logging can be suppressed by adding the following line to the `/etc/libva.conf` file:

```bash
LIBVA_MESSAGING_LEVEL=1
```

If you're using the Snap version of Firefox, it will be unable to access the host version of the driver that is installed.

## Chrome

Chrome is currently unsupported, and will not function.

## MPV

Currently this only works with a recent MPV version (at least 0.36.0).

There's no real reason to run it with mpv except for testing, as mpv already supports using nvdec directly. The `test.sh` script will run mpv with the file provided and various environment variables set to use the newly built driver

## Direct Backend

The direct backend is a experimental backend that accesses the NVIDIA kernel driver directly, rather than using EGL to share the buffers. This allows us
a greater degree of control over buffer allocation and freeing.

The direct backend has been tested on a variety of hardware from the Kepler to Lovelace generations, and seems to be working fine. If you find any compatibility issues, please leave a comment [here](/../../issues/126).

Given this backend accesses the NVIDIA driver directly, via NVIDIA's unstable API, this module is likely to break often with new versions of the kernel driver. If you encounter issues using this backend raise an issue and including logs generated by `NVD_LOG=1`.

This backend uses headers files from the NVIDIA [open-gpu-kernel-modules](https://github.com/NVIDIA/open-gpu-kernel-modules)
project. The `extract_headers.sh` script, along with the `headers.in` file list which files we need, and will copy them from a checked out version of the NVIDIA project to the `nvidia-include` directory. This is done to prevent everyone needing to checkout that project.

# Testing

To verify that the driver is being used, first check profile/entrypoint exposure with `vainfo`, then run decode/encode commands and monitor `nvidia-smi`.

Use your built driver explicitly during testing:

```sh
export LIBVA_DRIVER_NAME=nvidia
export LIBVA_DRIVERS_PATH=/path/to/nvidia-vaapi-driver/build
```

```sh
vainfo --display drm --device /dev/dri/renderD128
```

Minimal ffmpeg checks:

```sh
# Decode check (force VAAPI decode path)
ffmpeg -hide_banner \
  -hwaccel vaapi -hwaccel_output_format vaapi \
  -vaapi_device /dev/dri/renderD128 \
  -i input.mp4 -f null -

# Encode check (H.264 example)
ffmpeg -hide_banner \
  -vaapi_device /dev/dri/renderD128 \
  -f lavfi -i testsrc2=size=192x192:rate=30 -t 3 \
  -vf 'format=nv12,hwupload' \
  -c:v h264_vaapi -profile:v high -rc_mode CBR -b:v 2M \
  -y out_h264.mp4

# Decode the produced stream using VAAPI
ffmpeg -hide_banner \
  -hwaccel vaapi -hwaccel_output_format vaapi \
  -vaapi_device /dev/dri/renderD128 \
  -i out_h264.mp4 -f null -
```

CBR/VBR/CQP sanity checks:

```sh
# H.264 High (8-bit 4:2:0)
ffmpeg -hide_banner -vaapi_device /dev/dri/renderD128 \
  -f lavfi -i testsrc2=size=1920x1080:rate=30 -t 5 \
  -vf 'format=nv12,hwupload' \
  -c:v h264_vaapi -profile:v high -rc_mode CBR -b:v 2M -qmin 18 -qmax 40 \
  -y out_h264_cbr.mp4

ffmpeg -hide_banner -vaapi_device /dev/dri/renderD128 \
  -f lavfi -i testsrc2=size=1920x1080:rate=30 -t 5 \
  -vf 'format=nv12,hwupload' \
  -c:v h264_vaapi -profile:v high -rc_mode VBR -b:v 2M -maxrate 4M -qmin 18 -qmax 40 \
  -y out_h264_vbr.mp4

ffmpeg -hide_banner -vaapi_device /dev/dri/renderD128 \
  -f lavfi -i testsrc2=size=1920x1080:rate=30 -t 5 \
  -vf 'format=nv12,hwupload' \
  -c:v h264_vaapi -profile:v high -rc_mode CQP -qp 30 -qmin 20 -qmax 40 \
  -y out_h264_cqp.mp4

# HEVC Main10 (10-bit 4:2:0)
ffmpeg -hide_banner -vaapi_device /dev/dri/renderD128 \
  -f lavfi -i testsrc2=size=1920x1080:rate=30 -t 5 \
  -vf 'format=p010le,hwupload' \
  -c:v hevc_vaapi -profile:v main10 -rc_mode CBR -b:v 2M -qmin 18 -qmax 40 \
  -y out_hevc_main10_cbr.mp4

# HEVC rext (8-bit 4:4:4)
ffmpeg -hide_banner -vaapi_device /dev/dri/renderD128 \
  -f lavfi -i testsrc2=size=1920x1080:rate=30 -t 5 \
  -vf 'format=yuv444p,hwupload' \
  -c:v hevc_vaapi -profile:v rext -rc_mode CBR -b:v 2M -qmin 18 -qmax 40 \
  -y out_hevc_rext444_cbr.mp4
```

For HEVC/AV1 and high-bit-depth/rext profiles, use matching input formats (`p010le`, `yuv444p`, `yuv444p10le`, etc.) and verify the final stream profile with `ffprobe`, since some clients may negotiate a fallback pixel format:

```sh
ffprobe -v error -select_streams v:0 \
  -show_entries stream=codec_name,profile,pix_fmt,width,height \
  -of default=noprint_wrappers=1 out_hevc_rext444_cbr.mp4
```

Meson runtime matrix (codec/profile progress + per-case result):

```sh
meson setup build --reconfigure -Druntime_tests=true
meson test -C build driver-runtime-vaapi-smoke --print-errorlogs --no-rebuild -v
```

`meson test` default mode captures test output. Use `-v` to stream runtime progress.

Runtime matrix output semantics:

- `RUNNING`: case in progress (`profile + rc mode + encoder + pixfmt`)
- `OK`: encode+decode passed
- `FAIL`: encode and/or decode failed (reason printed)
- `SKIP`: case intentionally skipped (for example unmapped profile or missing ffmpeg encoder)
- Interactive terminal defaults to a compact dashboard where each case status transitions:
  - `PENDING -> RUNNING -> OK/FAIL` (or `SKIP`)

Runtime matrix UI mode options:

- `NVD_RUNTIME_UI=auto` (default; dashboard on interactive terminal, plain in non-interactive logs/CI)
- `NVD_RUNTIME_UI=dashboard`
- `NVD_RUNTIME_UI=plain`

Runtime matrix codec filter options:

- `NVD_RUNTIME_CODECS=all` (default)
- `NVD_RUNTIME_CODECS=h264`
- `NVD_RUNTIME_CODECS=h265` (`hevc` alias)
- `NVD_RUNTIME_CODECS=h264,av1` (comma/space separated list)

Examples:

```sh
# H.264 only
NVD_RUNTIME_CODECS=h264 \
meson test -C build driver-runtime-vaapi-smoke --print-errorlogs --no-rebuild -v

# HEVC only (h265 alias)
NVD_RUNTIME_CODECS=h265 \
meson test -C build driver-runtime-vaapi-smoke --print-errorlogs --no-rebuild -v

# Run script directly with dashboard (interactive terminal)
NVD_RUNTIME_UI=dashboard ./tests/driver-runtime-vaapi-smoke.sh
```

Reference validation done on March 2, 2026 (Ubuntu 24.04, ffmpeg 6.1.1, libva 2.12.0):

- Functional matrix: 51/51 pass
  - 7 advertised encode profiles x (CBR/VBR/CQP) across 720p + 1080p
  - plus 20-second long-run checks for representative profiles
- Stress suite: 20/20 pass
  - 4K sweep (H.264 High, HEVC Main10, HEVC rext444) x (CBR/VBR/CQP)
  - 45-second 1080p long-run for representative profiles x (CBR/VBR/CQP)
  - parallel two-session encode (H.264 CBR + HEVC Main10 VBR)
- Observed caveat in this ffmpeg path:
  - HEVC rext with `yuv444p10le` input may be negotiated to `yuv420p10le` by ffmpeg.

- nvidia-settings

  By selecting the relevant GPU on the left of the nvidia-settings window, it will show `Video Engine Utilization` on the right. While playing a video this value should be non-zero.

- nvidia-smi

  Running `nvidia-smi` while decoding a video should show a Firefox process with `C` in the `Type` column. In addition `nvidia-smi pmon` will show the usage of the decode engine per-process, and `nvidia-smi dmon` will show the usage per-GPU. When using nvidia open gpu kernel modules, the usage of the decode engine may not be displayed correctly.
