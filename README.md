# nvidia-vaapi-driver

This is an VA-API implementation that uses NVDEC as a backend.

It's currently in early development, so don't expect it to work well.

Building
========

You'll need the [NVIDIA Video Codec SDK](https://developer.nvidia.com/nvidia-video-codec-sdk/download) to build it.

Update the Makefile with the location to the SDK, and use `make` to build the library.
You'll also need the gstreamer-plugins-bad library, as the bitstream parser is needed from it.

The `test.sh` script will run mpv with the file provided and various environment variables set to use the newly built driver.
