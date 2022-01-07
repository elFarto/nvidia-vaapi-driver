# nvidia-vaapi-driver

This is an VA-API implementation that uses NVDEC as a backend. This implementation is specifically designed to be used by Firefox for accelerated decode of web content, and may not operate correctly in other applications.

It's currently in early development, so don't expect it to work well.

This library requires that the `nvidia_drm` kernel module is configured with the parameter `nvidia-drm.modeset=1`

# Codec Support

Only decoding video is currently supported.

| Codec | Supported | Comments |
|---|---|---|
|H.264|:heavy_check_mark:||
|HEVC|:heavy_check_mark:||
|VP8|:heavy_check_mark:||
|VP9|:heavy_check_mark:||
|AV1|:x:|Not completed, my 1060 doesn't support decoding it|
|MPEG-2|:heavy_check_mark:||
|MPEG-4|:x:|Not sure if this can be made work. NVDEC seems to require more information than VA-API supplies|
|VC-1|:heavy_check_mark:||
|JPEG|:x:|This is unlikely to ever work, the two APIs are too different|

Currently, 10-bit videos are not supported due to the NVIDIA driver not allowing R16 and RG1616 surfaces to be imported.

# Building

You'll need the gstreamer-plugins-bad library installed.

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
| media.ffmpeg.vaapi.enabled | true | Required, enables the use of VA-API |
| media.rdd-ffmpeg.enabled | true | Required, enables the use of VA-API |
| media.navigator.mediadatadecoder_vpx_enabled | true | Optional, enables hardware VA-API decoding for WebRTC. |
| media.av1.enabled | false | Optional, disables AV1. The driver doesn't support AV1 playback yet. This will prevent sites attempting to use it and falling back to software decoding |
| gfx.x11-egl.force-enabled | true | This driver requires that Firefox use the EGL backend. If it isn't selecting it by default, it'll need to be forced on using this option or by setting the `MOZ_X11_EGL` environment variable to `1`. It's recommended you try the environment variable method first to test it. |

In addition the following environment variables need to be set:
| Variable | Value | Reason |
|---|---|---|
|LIBVA_DRIVER_NAME|nvidia|This forces libva to load the `nvidia` backend, as the current version doesn't know which driver to load for the nvidia-drm driver.|
|MOZ_DISABLE_RDD_SANDBOX|1|This disables the sandbox for the RDD thread that the decoder runs in.|

# MPV

Currently this only works with a build of MPV from git master.

There's no real reason to run it with mpv except for testing, as mpv already supports using nvdec directly. The `test.sh` script will run mpv with the file provided and various environment variables set to use the newly built driver
