#!/bin/bash
set -u

# test_vp9_loop_filter_deltas.sh — VP9 state that persists across frames.
#
# VP9 loop-filter ref/mode deltas and segment feature data stay in effect
# until a frame header updates them, and libvpx codes the deltas only on key
# frames. The GStreamer header parser reports just the current frame's
# updates, so passing its output straight to NVDEC filtered every inter frame
# with zero deltas (and dropped unchanged segment features): the decode drifted
# from the second inter frame on. VP9 decoding is normative, so this decodes
# through the driver and through FFmpeg's software decoder and requires them to
# be bit-exact.
#
# The error-resilient stream resets the state on every frame and passes even
# without the fix; it guards the ordinary path, so a failure in the others
# points at the persisted state rather than at VP9 decode in general.

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
echo "=== nvidia-vaapi-driver VP9 loop-filter delta tests ==="
echo ""

RENDER_NODE="${RENDER_NODE:-/dev/dri/renderD128}"

# $1 = file name, $2 = pixel format, remaining = libvpx-vp9 options
encode() {
    local name="$1" pix_fmt="$2"
    shift 2
    ffmpeg -hide_banner -loglevel error -f lavfi \
        -i "testsrc2=size=640x360:rate=30:duration=3" -vf "noise=alls=6:allf=t" \
        -pix_fmt "$pix_fmt" -c:v libvpx-vp9 "$@" \
        -y "$TMPDIR/$name" 2>"$TMPDIR/enc.log"
}

if ! encode realtime.webm yuv420p -deadline realtime -cpu-used 8 -b:v 1M; then
    skip "VP9 fixtures" "no libvpx-vp9 encoder in this ffmpeg"
    echo ""
    echo "=== VP9 loop-filter delta tests: 0 passed, 0 failed, 1 skipped ==="
    exit 77
fi

# $1 = label, $2 = fixture, $3 = pixel format
compare_decode() {
    local label="$1" fixture="$TMPDIR/$2" pix_fmt="$3"

    ffmpeg -hide_banner -loglevel error -i "$fixture" \
        -pix_fmt "$pix_fmt" -f framemd5 -y "$TMPDIR/sw.md5" 2>"$TMPDIR/sw.log" || {
        fail "$label" "software decode failed"
        return
    }

    if ! ffmpeg -hide_banner -loglevel error \
            -hwaccel vaapi -hwaccel_device "$RENDER_NODE" \
            -hwaccel_output_format vaapi \
            -i "$fixture" -vf "hwdownload,format=$pix_fmt" \
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

compare_decode "VP9, deltas coded on key frames only" realtime.webm nv12

if encode segmentation.webm yuv420p -deadline realtime -cpu-used 8 -aq-mode 3 -b:v 1M; then
    compare_decode "VP9, segmentation (aq-mode 3)" segmentation.webm nv12
else
    skip "VP9, segmentation (aq-mode 3)" "encoder rejected -aq-mode 3"
fi

if encode altref.webm yuv420p -deadline good -cpu-used 5 -auto-alt-ref 1 -lag-in-frames 16 -b:v 1M; then
    compare_decode "VP9, hidden alt-ref frames" altref.webm nv12
else
    skip "VP9, hidden alt-ref frames" "encoder rejected alt-ref options"
fi

if encode profile2.webm yuv420p10le -deadline realtime -cpu-used 8 -b:v 1M; then
    compare_decode "VP9 profile 2, 10-bit" profile2.webm p010le
else
    skip "VP9 profile 2, 10-bit" "no 10-bit libvpx-vp9 encoder"
fi

if encode resilient.webm yuv420p -deadline realtime -cpu-used 8 -error-resilient 1 -b:v 1M; then
    compare_decode "VP9, error-resilient (control)" resilient.webm nv12
else
    skip "VP9, error-resilient (control)" "encoder rejected -error-resilient"
fi

echo ""
echo "=== VP9 loop-filter delta tests: ${PASS} passed, ${FAIL} failed, ${SKIP} skipped ==="
echo ""

if [ "$FAIL" -gt 0 ]; then
    exit 1
fi
if [ "$PASS" -eq 0 ]; then
    exit 77
fi
exit 0
