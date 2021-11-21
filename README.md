# nvidia-vaapi-driver

This is an VA-API implementation that uses NVDEC as a backend.

It's currently in early development, so don't expect it to work well.

# Building

You'll need the gstreamer-plugins-bad library installed.

Then run the following commands:
```
meson setup build
meson install -C build
```

# Firefox

To use the driver with firefox, the following config options need to be set:
| Option | Value | Reason |
|--------|-------|--------|
| media.ffmpeg.vaapi.enabled | true | Required, enables the use of VA-API |
| security.sandbox.content.level | 0 | Required, disables the sandboxing for the content process. *Note* This is not recommended for general use as it reduces security | 
| media.navigator.mediadatadecoder_vpx_enabled | true | Recommended, enables hardware VA-API decoding for WebRTC |
| media.ffvpx.enabled | false | Recommended, disables the internal software decoders for VP8/VP9 |
| media.rdd-vpx.enabled | false | Required, disables the remote data decoder process for VP8/VP9 |
| media.av1.enabled | false | Optional, disables AV1. The driver doesn't support AV1 playback yet. This will prevent sites attempting to use it. |

# MPV

There's no real reason to run it with mpv except for testing, as mpv already supports using nvdec directly. The `test.sh` script will run mpv with the file provided and various environment variables set to use the newly built driver.
