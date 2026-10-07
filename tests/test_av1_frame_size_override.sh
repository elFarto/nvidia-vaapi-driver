#!/bin/bash
set -u

# test_av1_frame_size_override.sh — AV1 decode when frames are coded smaller
# than the sequence maximum.
#
# AV1 lets a sequence header declare a maximum frame size and then code the
# individual frames smaller than it (frame_size_override_flag). Hardware
# encoders do this routinely — NVENC works in 16-pixel-aligned blocks, so for
# 1080p it declares a 1920x1088 maximum and codes every frame at 1920x1080.
# VA-API clients create the context and its surfaces at the maximum size, which
# is correct, and the driver used to assume every frame was that size too: it
# passed the context size to NVDEC as the frame size and each reference's
# *surface* size as the reference size. Prediction then ran against the wrong
# geometry, so the picture stayed fine for a while after each keyframe and then
# smeared into growing block corruption. On top of that NVDEC does not crop a
# smaller frame, it stretches it to fill the decoder's display area.
#
# Both halves are invisible to an "it decoded and produced frames" check, so
# this test decodes the same stream through the driver and through libdav1d and
# requires them to agree. A correct AV1 hardware decode is bit-exact with a
# software one — AV1 decoding is normative — and the broken driver scored
# around 13 dB on this fixture.
#
# The fixture is generated rather than checked in (samples/*.mp4 are
# .gitignore'd here), using SVT-AV1's forced-max-frame-width/height, which is
# the only widely available encoder knob that produces the layout. A control
# stream whose frames match the sequence maximum guards the ordinary path.

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
echo "=== nvidia-vaapi-driver AV1 frame_size_override decode tests ==="
echo ""

WIDTH=320
HEIGHT=180
MAX_HEIGHT=192          # 16-aligned, like a hardware encoder's padded maximum
RENDER_NODE="${RENDER_NODE:-/dev/dri/renderD128}"
THRESHOLD="${PSNR_THRESHOLD:-40.0}"

ffmpeg -hide_banner -loglevel error -f lavfi \
    -i "testsrc2=size=${WIDTH}x${HEIGHT}:rate=30:duration=2" \
    -pix_fmt yuv420p -y "$TMPDIR/src.y4m" 2>/dev/null || {
    skip "fixture source" "ffmpeg cannot generate testsrc2"
    echo ""
    echo "=== AV1 frame_size_override tests: 0 passed, 0 failed, 1 skipped ==="
    exit 77
}

# $1 = output .ivf, $2 = extra svtav1 params ("" for the control stream)
encode() {
    local out="$1" params="$2" args=()
    [ -n "$params" ] && args=(-svtav1-params "$params")
    ffmpeg -hide_banner -loglevel error -i "$TMPDIR/src.y4m" \
        -c:v libsvtav1 -preset 10 "${args[@]}" -b:v 300k -g 30 \
        -y "$out" 2>"$TMPDIR/enc.log"
}

# Confirm the stream really is what this test is about: frames coded smaller
# than the sequence maximum. Without this the test could silently pass on a
# stream that never exercises the bug.
coded_smaller_than_max() {
    local stream="$1"
    ffmpeg -hide_banner -loglevel trace -i "$stream" -c:v copy \
        -bsf:v trace_headers -f null - 2>&1 |
        sed -E 's/^\[[^]]*\] *//' |
        awk '
            /max_frame_height_minus_1/ { if (max == "") max = $NF }
            /^Frame Header/            { f++ }
            f >= 2 && /frame_height_minus_1/ { if (h == "") h = $NF }
            /frame_size_override_flag/ { if ($NF == 1) ovr = 1 }
            END { exit !(ovr == 1 && max != "" && h != "" && max > h) }'
}

# Decode through the driver and through libdav1d, compare frame-for-frame.
compare_decode() {
    local label="$1" stream="$2"

    if ! ffmpeg -hide_banner -loglevel error -c:v libdav1d -i "$stream" \
            -pix_fmt yuv420p -y "$TMPDIR/sw.yuv" 2>"$TMPDIR/sw.log"; then
        skip "$label" "no libdav1d decoder in this ffmpeg"
        return
    fi

    if ! ffmpeg -hide_banner -loglevel error \
            -hwaccel vaapi -hwaccel_device "$RENDER_NODE" \
            -hwaccel_output_format vaapi -i "$stream" \
            -vf 'hwdownload,format=nv12' -pix_fmt yuv420p \
            -y "$TMPDIR/hw.yuv" 2>"$TMPDIR/hw.log"; then
        fail "$label" "hardware decode failed"
        cat "$TMPDIR/hw.log"
        return
    fi

    local sw_size hw_size
    sw_size=$(stat -c%s "$TMPDIR/sw.yuv" 2>/dev/null || echo 0)
    hw_size=$(stat -c%s "$TMPDIR/hw.yuv" 2>/dev/null || echo 0)
    if [ "$hw_size" -eq 0 ] || [ "$hw_size" -ne "$sw_size" ]; then
        fail "$label" "decoded ${hw_size}B vs software ${sw_size}B"
        return
    fi

    if cmp -s "$TMPDIR/hw.yuv" "$TMPDIR/sw.yuv"; then
        pass "$label" "(bit-exact)"
        return
    fi

    local psnr
    psnr=$(ffmpeg -hide_banner \
        -s "${WIDTH}x${HEIGHT}" -pix_fmt yuv420p -i "$TMPDIR/hw.yuv" \
        -s "${WIDTH}x${HEIGHT}" -pix_fmt yuv420p -i "$TMPDIR/sw.yuv" \
        -lavfi psnr -f null - 2>&1 | grep -oP 'average:\K[0-9.]+|average:\Kinf' | tail -1)

    if [ -z "${psnr:-}" ]; then
        fail "$label" "could not measure PSNR"
    elif awk "BEGIN{exit !(\"$psnr\" != \"inf\" && $psnr < $THRESHOLD)}"; then
        fail "$label" "PSNR=${psnr} dB < ${THRESHOLD} dB -> wrong geometry passed to NVDEC"
    else
        pass "$label" "(${psnr} dB)"
    fi
}

# --- frames smaller than the sequence maximum (the regression) --------------
if ! encode "$TMPDIR/override.ivf" \
        "forced-max-frame-width=${WIDTH}:forced-max-frame-height=${MAX_HEIGHT}"; then
    skip "frame_size_override fixture" "no libsvtav1 encoder in this ffmpeg"
elif ! coded_smaller_than_max "$TMPDIR/override.ivf"; then
    skip "frame_size_override fixture" \
         "this SVT-AV1 build ignored forced-max-frame-height"
else
    compare_decode "AV1 ${WIDTH}x${HEIGHT} frames in ${WIDTH}x${MAX_HEIGHT} sequence" \
                   "$TMPDIR/override.ivf"
fi

# --- control: frames equal to the sequence maximum --------------------------
if ! encode "$TMPDIR/plain.ivf" ""; then
    skip "control fixture" "no libsvtav1 encoder in this ffmpeg"
else
    compare_decode "AV1 frames equal to the sequence maximum" "$TMPDIR/plain.ivf"
fi

echo ""
echo "=== AV1 frame_size_override tests: ${PASS} passed, ${FAIL} failed, ${SKIP} skipped ==="
echo ""

if [ "$FAIL" -gt 0 ]; then
    exit 1
fi
if [ "$PASS" -eq 0 ]; then
    exit 77
fi
exit 0
