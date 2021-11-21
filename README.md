# nvidia-vaapi-driver

This is an VA-API implementation that uses NVDEC as a backend.

It's currently in early development, so don't expect it to work well.

Building
========

You'll need the gstreamer-plugins-bad library installs.

Then run the following commands:
```
meson setup build
meson install -C build
```

The `test.sh` script will run mpv with the file provided and various environment variables set to use the newly built driver.
