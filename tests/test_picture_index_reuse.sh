#!/bin/bash
set -u

# test_picture_index_reuse.sh — decoding into more than 32 distinct surfaces.
#
# NVDEC addresses at most 32 decode surfaces per decoder, but VA-API lets a
# client render into as many surfaces as it likes. Chromium/Electron and FFmpeg
# create the context without render targets and grow their surface pool as
# needed, so a stream with a deep reference list runs past 32 distinct render
# targets. The driver used to hand out picture indices from a counter that only
# grew, and the 33rd surface failed vaBeginPicture with
# VA_STATUS_ERROR_MAX_NUM_EXCEEDED ("list argument exceeds maximum number",
# issue #397).
#
# Reusing indices is only half of it: an index reassigned while its frame is
# still referenced corrupts the picture without any error. So this test decodes
# through the driver and through FFmpeg's software decoder and requires them to
# be bit-exact. H.264 decoding is normative.

export LIBVA_DRIVER_NAME=nvidia

PASS=0
FAIL=0
SKIP=0
TMPDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR"' EXIT

pass() { printf "  %-55s \033[32mPASS\033[0m %s\n" "$1" "${2:-}"; PASS=$((PASS+1)); }
fail() { printf "  %-55s \033[31mFAIL\033[0m (%s)\n" "$1" "$2"; FAIL=$((FAIL+1)); }
skip() { printf "  %-55s \033[33mSKIP\033[0m (%s)\n" "$1" "$2"; SKIP=$((SKIP+1)); }

echo ""
echo "=== nvidia-vaapi-driver picture index reuse tests ==="
echo ""

RENDER_NODE="${RENDER_NODE:-/dev/dri/renderD128}"

# 16 reference frames and B-frames keep FFmpeg's H.264 surface pool well above
# 32 surfaces even without extra frames.
if ! ffmpeg -hide_banner -loglevel error -f lavfi \
        -i "testsrc2=size=320x180:rate=30:duration=4" \
        -c:v libx264 -x264-params ref=16:bframes=3:keyint=120 \
        -y "$TMPDIR/h264.mp4" 2>"$TMPDIR/enc.log"; then
    skip "H.264 fixture" "no libx264 encoder in this ffmpeg"
    echo ""
    echo "=== Picture index reuse tests: 0 passed, 0 failed, 1 skipped ==="
    exit 77
fi

# $1 = label, $2 = -extra_hw_frames
compare_decode() {
    local label="$1" extra="$2"

    ffmpeg -hide_banner -loglevel error -i "$TMPDIR/h264.mp4" \
        -pix_fmt nv12 -f framemd5 -y "$TMPDIR/sw.md5" 2>"$TMPDIR/sw.log" || {
        fail "$label" "software decode failed"
        return
    }

    if ! ffmpeg -hide_banner -loglevel error \
            -hwaccel vaapi -hwaccel_device "$RENDER_NODE" \
            -hwaccel_output_format vaapi -extra_hw_frames "$extra" \
            -i "$TMPDIR/h264.mp4" -vf 'hwdownload,format=nv12' \
            -f framemd5 -y "$TMPDIR/hw.md5" 2>"$TMPDIR/hw.log"; then
        fail "$label" "hardware decode failed: $(head -1 "$TMPDIR/hw.log")"
        return
    fi

    local frames bad
    frames=$(grep -vc '^#' "$TMPDIR/sw.md5")
    bad=$(diff <(grep -v '^#' "$TMPDIR/hw.md5") <(grep -v '^#' "$TMPDIR/sw.md5") | grep -c '^<')
    if [ "$(grep -vc '^#' "$TMPDIR/hw.md5")" -ne "$frames" ]; then
        fail "$label" "decoded $(grep -vc '^#' "$TMPDIR/hw.md5") of $frames frames"
    elif [ "$bad" -ne 0 ]; then
        fail "$label" "$bad of $frames frames differ from software decode"
    else
        pass "$label" "(bit-exact, $frames frames)"
    fi
}

compare_decode "H.264, 16 refs, default surface pool" 0
compare_decode "H.264, 16 refs, 48 extra surfaces" 48

echo ""
echo "=== Picture index reuse tests: ${PASS} passed, ${FAIL} failed, ${SKIP} skipped ==="
echo ""

if [ "$FAIL" -gt 0 ]; then
    exit 1
fi
if [ "$PASS" -eq 0 ]; then
    exit 77
fi
exit 0
