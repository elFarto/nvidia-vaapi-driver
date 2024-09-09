# nvidia-vaapi-driver

This is an VA-API implementation that uses NVDEC as a backend. This implementation is specifically designed to be used by Firefox for accelerated decode of web content, and may not operate correctly in other applications.

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

Hardware decoding only, encoding is [not supported](/../../issues/116).

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

YUV444 is supported but requires:

* \>= Turing (20XX/16XX)
* HEVC
* Direct backend

To view which codecs your card is capable of decoding you can use the `vainfo` command with this driver installed, or visit the NVIDIA website [here](https://developer.nvidia.com/video-encode-and-decode-gpu-support-matrix-new#geforce).

# Installation

To install and use `nvidia-vaapi-driver`, follow the steps in installation and configuration. It is recommended to follow testing as well to verify hardware acceleration is working as intended.

**Requirements**

* NVIDIA driver series 470 or 500+

## Packaging status

<p align="top"><a href="https://repology.org/project/nvidia-vaapi-driver/versions"><img src="https://repology.org/badge/vertical-allrepos/nvidia-vaapi-driver.svg" alt="repology"><a href="https://repology.org/project/libva-nvidia-driver/versions"><img src="https://repology.org/badge/vertical-allrepos/libva-nvidia-driver.svg" alt="repology" align="top" width="%"></p>

[pkgs.org/nvidia-vaapi-driver](https://pkgs.org/search/?q=nvidia-vaapi-driver) [pkgs.org/libva-nvidia-driver](https://pkgs.org/search/?q=libva-nvidia-driver)

openSUSE: [1](https://software.opensuse.org/package/nvidia-vaapi-driver), [2](https://software.opensuse.org/package/libva-nvidia-driver).

Feel free to add your distributions package in an issue/PR, if it isn't on these websites.

## Building

You'll need `meson`, the `gstreamer-plugins-bad` library, and [`nv-codec-headers`](https://git.videolan.org/?p=ffmpeg/nv-codec-headers.git) installed.

| Package manager | Packages                                        | Optional packages for additional codec support |
|-----------------|-------------------------------------------------|------------------------------------------------|
| pacman          | meson gst-plugins-bad ffnvcodec-headers         |                                                |
| apt             | meson gstreamer1.0-plugins-bad libffmpeg-nvenc-dev libva-dev libegl-dev | libgstreamer-plugins-bad1.0-dev   |
| yum/dnf         | meson libva-devel gstreamer1-plugins-bad-freeworld nv-codec-headers | gstreamer1-plugins-bad-free-devel |

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

## Firefox

Due to license, Firefox on Linux does not support HEVC till now.
To use the driver with firefox you will need at least Firefox 96, `ffmpeg` compiled with vaapi support (`ffmpeg -hwaccels` output should include vaapi), and the following config options need to be set in the `about:config` page:

| Option | Value | Reason |
|---|---|---|
| media.ffmpeg.vaapi.enabled | true | Required, enables the use of VA-API. |
| media.rdd-ffmpeg.enabled | true | Required, default on FF97. Forces ffmpeg usage into the RDD process, rather than the content process. |
| media.av1.enabled | false | Optional, disables AV1. If your GPU doesn't support AV1, this will prevent sites using it and falling back to software decoding. |
| gfx.x11-egl.force-enabled | true | Required, this driver requires that Firefox use the EGL backend. It may be enabled by default. It is recommended to test it with the `MOZ_X11_EGL=1` environment variable before enabling it in the Firefox configuration. |
| widget.dmabuf.force-enabled | true | Required on NVIDIA 470 series drivers, and currently **REQUIRED** on 500+ drivers due to a [Firefox change](https://bugzilla.mozilla.org/show_bug.cgi?id=1788573). Note that Firefox isn't coded to allow DMA-BUF support without GBM support, so it may not function completely correctly when it's forced on. |

In addition the following environment variables need to be set. For permanent configuration `/etc/environment` may suffice.

| Variable | Value | Reason |
|---|---|---|
| MOZ_DISABLE_RDD_SANDBOX | 1 | Disables the sandbox for the RDD process that the decoder runs in. |
| LIBVA_DRIVER_NAME | nvidia | Required for libva 2.20+, forces libva to load this driver. |
| __EGL_VENDOR_LIBRARY_FILENAMES | /usr/share/glvnd/egl_vendor.d/10_nvidia.json | Required for the 470 driver series only. It overrides the list of drivers the glvnd library can use to prevent Firefox from using the MESA driver by mistake. |

When libva is used it will log out some information, which can be excessive when Firefox initalises it multiple times per page. This logging can be suppressed by adding the following line to the `/etc/libva.conf` file:
```
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

To verify that the driver is being used to decode video, you can use nvidia-settings or nvidia-smi.

- nvidia-settings

  By selecting the relevant GPU on the left of the nvidia-settings window, it will show `Video Engine Utilization` on the right. While playing a video this value should be non-zero.

- nvidia-smi

  Running `nvidia-smi` while decoding a video should show a Firefox process with `C` in the `Type` column. In addition `nvidia-smi pmon` will show the usage of the decode engine per-process, and `nvidia-smi dmon` will show the usage per-GPU. When using nvidia open gpu kernel modules, the usage of the decode engine may not be displayed correctly.
