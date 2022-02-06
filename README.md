# nvidia-vaapi-driver

This is an VA-API implementation that uses NVDEC as a backend. This implementation is specifically designed to be used by Firefox for accelerated decode of web content, and may not operate correctly in other applications.

This library requires that the `nvidia_drm` kernel module is configured with the parameter `nvidia-drm.modeset=1`

# Codec Support

Only decoding video is currently supported.

| Codec | Supported | Comments |
|---|---|---|
|AV1|:o:|Experimental support. Firefox 98+ is required.|
|H.264|:heavy_check_mark:||
|HEVC|:heavy_check_mark:||
|VP8|:heavy_check_mark:||
|VP9|:heavy_check_mark:||
|MPEG-2|:heavy_check_mark:||
|VC-1|:heavy_check_mark:||
|MPEG-4|:x:|VA-API does not supply enough of the original bitstream to allow NVDEC to decode it.|
|JPEG|:x:|This is unlikely to ever work, the two APIs are too different.|

Currently, 10-bit and 12-bit videos are not supported due to the NVIDIA driver not allowing R16 and RG1616 surfaces to be imported.
To view which codecs your card is capable of decoding you can use the `vainfo` command with this driver installed, or visit the NVIDIA website [here](https://developer.nvidia.com/video-encode-and-decode-gpu-support-matrix-new#geforce).

# Building

You'll need the gstreamer-plugins-bad library and the [nv-codec-headers](https://git.videolan.org/?p=ffmpeg/nv-codec-headers.git) installed.

Then run the following commands:
```
meson setup build
meson install -C build
```

# Debugging

The `NVD_LOG` environment variable can be used to control logging, `NVD_LOG=1` will log to stdout, and `NVD_LOG=<filename>` will append to the specified file (or stdout if the file can't be opened).

# Firefox

To use the driver with firefox you will need at least Firefox 96 (currently in beta), the following config options need to be set in the about:config page:
| Option | Value | Reason |
|---|---|---|
| media.ffmpeg.vaapi.enabled | true | Required, enables the use of VA-API.|
| media.rdd-ffmpeg.enabled | true | Required, default on FF97. Forces ffmpeg usage into the RDD process, rather than the content process.|
| media.av1.enabled | false | Optional, disables AV1. The driver doesn't support AV1 playback yet. This will prevent sites attempting to use it and falling back to software decoding |
| gfx.x11-egl.force-enabled | true | This driver requires that Firefox use the EGL backend. If it isn't selecting it by default, it'll need to be forced on using this option or by setting the `MOZ_X11_EGL` environment variable to `1`. It's recommended you try the environment variable method first to test it. |
| widget.dmabuf.force-enabled | true | Required for NVIDIA 470 series drivers, not required at all on 495+. This option has been shown to help getting decoding working on the 470 driver series. However it should be noted that Firefox isn't coded to allow DMA-BUF support without GBM support, so it may not function completely correctly when it's forced on. |

In addition the following environment variables need to be set:
| Variable | Value | Reason |
|---|---|---|
|LIBVA_DRIVER_NAME|nvidia|This forces libva to load the `nvidia` backend, as the current version doesn't know which driver to load for the nvidia-drm driver.|
|MOZ_DISABLE_RDD_SANDBOX|1|This disables the sandbox for the RDD process that the decoder runs in.|

# MPV

Currently this only works with a build of MPV from git master.

There's no real reason to run it with mpv except for testing, as mpv already supports using nvdec directly. The `test.sh` script will run mpv with the file provided and various environment variables set to use the newly built driver
