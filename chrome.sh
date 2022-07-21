#!/bin/sh

NVD_LOG=1 chromium-freeworld --enable-features=VaapiVideoDecoder --password-store=basic --use-gl=desktop --enable-logging=stderr --vmodule=vaapi_wrapper=4,vaapi_video_decode_accelerator=4 --disable-features=UseChromeOSDirectVideoDecoder
