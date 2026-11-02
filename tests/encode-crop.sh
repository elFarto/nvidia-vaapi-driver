#!/bin/sh
set -eu

for command in ffprobe ffmpeg; do
    if ! command -v "$command" >/dev/null 2>&1; then
        echo "$command is required for crop verification" >&2
        exit 77
    fi
done
tmpdir=$(mktemp -d)
trap 'rm -rf "$tmpdir"' EXIT
"$1" "$tmpdir"
for size in 1920x1080 1600x900 640x360 638x358 640x384; do
    actual=$(ffprobe -v error -select_streams v:0 -show_entries stream=width,height \
        -of csv=p=0:s=x "$tmpdir/$size.h264")
    if [ "$actual" != "$size" ]; then
        echo "SPS visible dimensions: expected $size, got $actual" >&2
        exit 1
    fi
    ffmpeg -v error -xerror -err_detect explode -i "$tmpdir/$size.h264" -f null -
    echo "verified visible dimensions and software decode: $size"
done
"$1" "$tmpdir" --invalid-only
