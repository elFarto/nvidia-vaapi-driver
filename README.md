# nvidia-vaapi-driver

This is an VA-API implementation that uses NVDEC as a backend. This implementation is specifically designed to be used by Firefox for accelerated decode of web content, and may not operate correctly in other applications.

This library requires that the `nvidia_drm` kernel module is configured with the parameter `nvidia-drm.modeset=1`

# Codec Support

Only decoding video is currently supported.

| Codec | Supported | Comments |
|---|---|---|
|AV1|:heavy_check_mark:|Firefox 98+ is required.|
|H.264|:heavy_check_mark:||
|HEVC|:heavy_check_mark:||
|VP8|:heavy_check_mark:||
|VP9|:heavy_check_mark:|Requires being compiled with `gstreamer-codecparsers-1.0`|
|MPEG-2|:heavy_check_mark:||
|VC-1|:heavy_check_mark:||
|MPEG-4|:x:|VA-API does not supply enough of the original bitstream to allow NVDEC to decode it.|
|JPEG|:x:|This is unlikely to ever work, the two APIs are too different.|

Currently, YUV444 videos are currently not supported. To view which codecs your card is capable of decoding you can use the `vainfo` command with this driver installed, or visit the NVIDIA website [here](https://developer.nvidia.com/video-encode-and-decode-gpu-support-matrix-new#geforce).

# Building

You'll need the gstreamer-plugins-bad library and the [nv-codec-headers](https://git.videolan.org/?p=ffmpeg/nv-codec-headers.git) installed.

Then run the following commands:
```
meson setup build
meson install -C build
```

## Removing

By default the driver installs itself as `/usr/lib64/dri/nvidia_drv_video.so` (this might be `/usr/lib/x86_64-linux-gnu/dri/nvidia_drv_video.so` on some distros). To uninstall the driver, simply remove this file. In addition, this file is usually symlinked to `/usr/lib64/dri/vdpau_drv_video.so` (or `/usr/lib/x86_64-linux-gnu/dri/vdpau_drv_video.so`) if the VDPAU to VA-API driver is installed, so this symlink will need to be restored for that driver to work normally again.

# Environment Variables

| Variable | Purpose |
|---|---|
|`NVD_LOG`|This variable can be used to control logging, if it's set to `1` it will log to stdout. If it is set to anything else, it will use that as the filename to append to (or stdout if the file can't be opened).|
|`NVD_MAX_INSTANCES`|This variable controls the maximum concurrent instances of the driver will be allowed per-process. This option is only really useful for older GPUs with not much VRAM, especially with Firefox on video heavy websites.|
|`NVD_BACKEND`|This variable controls which backend this library uses, either `egl` (the default), or `direct`. See below for more details on the direct backend.|
# Firefox

To use the driver with firefox you will need at least Firefox 96, the following config options need to be set in the about:config page:
| Option | Value | Reason |
|---|---|---|
| media.ffmpeg.vaapi.enabled | true | Required, enables the use of VA-API.|
| media.rdd-ffmpeg.enabled | true | Required, default on FF97. Forces ffmpeg usage into the RDD process, rather than the content process.|
| media.av1.enabled | false | Optional, disables AV1. If your GPU doesn't support AV1, this will prevent sites using it and falling back to software decoding. Hopefully the site will fall back to using a different codec that is hardware accellerated. |
| gfx.x11-egl.force-enabled | true | This driver requires that Firefox use the EGL backend. If it isn't selecting it by default, it'll need to be forced on using this option or by setting the `MOZ_X11_EGL` environment variable to `1`. It's recommended you try the environment variable method first to test it. |
| widget.dmabuf.force-enabled | true | Required for NVIDIA 470 series drivers, not required at all on 495+. This option has been shown to help getting decoding working on the 470 driver series. However it should be noted that Firefox isn't coded to allow DMA-BUF support without GBM support, so it may not function completely correctly when it's forced on. |

In addition the following environment variables need to be set:
| Variable | Value | Reason |
|---|---|---|
|LIBVA_DRIVER_NAME|nvidia|This forces libva to load the `nvidia` backend, as the current version doesn't know which driver to load for the nvidia-drm driver.|
|MOZ_DISABLE_RDD_SANDBOX|1|This disables the sandbox for the RDD process that the decoder runs in.|
|EGL_PLATFORM|wayland|This option is needed on FF98+ when running on Wayland, due to a regression that has been introduced.|
|__EGL_VENDOR_LIBRARY_FILENAMES|/usr/share/glvnd/egl_vendor.d/10_nvidia.json|This option is needed for the 470 driver series only. It overrides the list of drivers the glvnd library can use to prevent Firefox from using the MESA driver by mistake.|

# MPV

Currently this only works with a build of MPV from git master.

There's no real reason to run it with mpv except for testing, as mpv already supports using nvdec directly. The `test.sh` script will run mpv with the file provided and various environment variables set to use the newly built driver

# Direct Backend

The direct backend is a experimental backend that accesses the NVIDIA kernel driver directly, rather than using EGL to share the buffers. This allows us
a greater degree of control over buffer allocation and freeing. 

Given this backend accesses the NVIDIA driver directly, via NVIDIA's unstable API, this module is likely to break often with new versions of the kernel driver. 
In addition, this backend has only been testing on a NVIDIA GeForce 1060 6GB, so it's compatibility with other cards is unknown at this point.

This backend uses headers files from the NVIDIA [open-gpu-kernel-modules](https://github.com/NVIDIA/open-gpu-kernel-modules)
project. The `extract_headers.sh` script, along with the `headers.in` file list which files we need, and will copy them from a checked out version of the NVIDIA project
to the `nvidia-include` directory. This is done to prevent everyone needing to checkout that project.

# Verifying

To verify that the driver is being used to decode video, you can use nvidia-settings or nvidia-smi:

- nvidia-settings

  By selecting the relevant GPU on the left of the nvidia-settings window, it will show `Video Engine Utilization` on the right. While playing a video this value should be non-zero

- nvidia-smi

  Running `nvidia-smi` while decoding a video should show a Firefox process with `C` in the `Type` column. In addition `nvidia-smi pmon` will show the usage of the decode engine per-process, and `nvidia-smi dmon` will show the usage per-GPU.
