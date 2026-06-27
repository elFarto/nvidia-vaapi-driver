#!/bin/bash
#
# test_gstreamer.sh — GStreamer VA-API encode integration tests
#
# Requires: gstreamer1-vaapi (Fedora) or gstreamer1.0-vaapi (Ubuntu)
#
# Exit code: 0 = all pass, 1 = failure

set -u

export GST_VAAPI_ALL_DRIVERS=1
export LIBVA_DRIVER_NAME=nvidia

PASS=0
FAIL=0
SKIP=0
TMPDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR"' EXIT

pass() { printf "  %-55s \033[32mPASS\033[0m\n" "$1"; PASS=$((PASS+1)); }
fail() { printf "  %-55s \033[31mFAIL\033[0m (%s)\n" "$1" "$2"; FAIL=$((FAIL+1)); }
skip() { printf "  %-55s \033[33mSKIP\033[0m (%s)\n" "$1" "$2"; SKIP=$((SKIP+1)); }

has_element() { gst-inspect-1.0 "$1" >/dev/null 2>&1; }

echo ""
echo "=== nvidia-vaapi-driver GStreamer tests ==="
echo ""

# --- Check prerequisites ---

echo "Prerequisites:"

if ! has_element vaapih264enc; then
    skip "vaapih264enc available" "gstreamer-vaapi not installed"
    echo ""
    echo "=== Results: $PASS passed, $FAIL failed, $SKIP skipped ==="
    exit 1
fi
pass "vaapih264enc available"

if ! has_element vaapih265enc; then
    skip "vaapih265enc available" "element not found"
else
    pass "vaapih265enc available"
fi

# --- H.264 encode tests ---

echo ""
echo "H.264 Encode:"

# Basic encode to fakesink
if gst-launch-1.0 -e videotestsrc num-buffers=30 \
    ! video/x-raw,width=320,height=240,framerate=30/1 \
    ! vaapih264enc ! h264parse ! fakesink 2>&1 | grep -q "EOS"; then
    pass "H.264 320x240 30 frames → fakesink"
else
    fail "H.264 320x240 30 frames → fakesink" "pipeline error"
fi

# Encode to file and validate
OUT="$TMPDIR/h264.mp4"
if gst-launch-1.0 -e videotestsrc num-buffers=60 \
    ! video/x-raw,width=1920,height=1080,framerate=30/1 \
    ! vaapih264enc bitrate=5000 ! h264parse \
    ! mp4mux ! filesink location="$OUT" 2>&1 | grep -q "EOS"; then
    SIZE=$(stat -c%s "$OUT" 2>/dev/null || echo 0)
    if [ "$SIZE" -gt 1000 ]; then
        pass "H.264 1080p 60 frames → mp4 (${SIZE} bytes)"
    else
        fail "H.264 1080p 60 frames → mp4" "file too small: ${SIZE} bytes"
    fi
else
    fail "H.264 1080p 60 frames → mp4" "pipeline error"
fi

# CBR bitrate control
OUT="$TMPDIR/h264_cbr.mp4"
if gst-launch-1.0 -e videotestsrc num-buffers=90 \
    ! video/x-raw,width=1280,height=720,framerate=30/1 \
    ! vaapih264enc rate-control=cbr bitrate=2000 ! h264parse \
    ! mp4mux ! filesink location="$OUT" 2>&1 | grep -q "EOS"; then
    SIZE=$(stat -c%s "$OUT" 2>/dev/null || echo 0)
    if [ "$SIZE" -gt 1000 ]; then
        pass "H.264 720p CBR 2Mbps 90 frames"
    else
        fail "H.264 720p CBR 2Mbps 90 frames" "file too small"
    fi
else
    fail "H.264 720p CBR 2Mbps 90 frames" "pipeline error"
fi

# Small resolution (GStreamer vaapi requires ~256x256 minimum)
if gst-launch-1.0 -e videotestsrc num-buffers=10 \
    ! video/x-raw,width=256,height=256,framerate=30/1 \
    ! vaapih264enc ! h264parse ! fakesink 2>&1 | grep -q "EOS"; then
    pass "H.264 256x256 small resolution"
else
    fail "H.264 256x256 small resolution" "pipeline error"
fi

# 4K resolution
if gst-launch-1.0 -e videotestsrc num-buffers=5 \
    ! video/x-raw,width=3840,height=2160,framerate=30/1 \
    ! vaapih264enc ! h264parse ! fakesink 2>&1 | grep -q "EOS"; then
    pass "H.264 4K 5 frames"
else
    fail "H.264 4K 5 frames" "pipeline error"
fi

# --- HEVC encode tests ---

echo ""
echo "HEVC Encode:"

if has_element vaapih265enc; then
    # Basic encode
    if gst-launch-1.0 -e videotestsrc num-buffers=30 \
        ! video/x-raw,width=320,height=240,framerate=30/1 \
        ! vaapih265enc ! h265parse ! fakesink 2>&1 | grep -q "EOS"; then
        pass "HEVC 320x240 30 frames → fakesink"
    else
        fail "HEVC 320x240 30 frames → fakesink" "pipeline error"
    fi

    # Encode to file
    OUT="$TMPDIR/hevc.mp4"
    if gst-launch-1.0 -e videotestsrc num-buffers=60 \
        ! video/x-raw,width=1920,height=1080,framerate=30/1 \
        ! vaapih265enc bitrate=5000 ! h265parse \
        ! mp4mux ! filesink location="$OUT" 2>&1 | grep -q "EOS"; then
        SIZE=$(stat -c%s "$OUT" 2>/dev/null || echo 0)
        if [ "$SIZE" -gt 1000 ]; then
            pass "HEVC 1080p 60 frames → mp4 (${SIZE} bytes)"
        else
            fail "HEVC 1080p 60 frames → mp4" "file too small: ${SIZE} bytes"
        fi
    else
        fail "HEVC 1080p 60 frames → mp4" "pipeline error"
    fi

    # 4K
    if gst-launch-1.0 -e videotestsrc num-buffers=5 \
        ! video/x-raw,width=3840,height=2160,framerate=30/1 \
        ! vaapih265enc ! h265parse ! fakesink 2>&1 | grep -q "EOS"; then
        pass "HEVC 4K 5 frames"
    else
        fail "HEVC 4K 5 frames" "pipeline error"
    fi
else
    skip "HEVC tests" "vaapih265enc not available"
fi

# --- Decode regression ---

echo ""
echo "Decode regression:"

if has_element vaapih264dec; then
    pass "vaapih264dec still available"
else
    fail "vaapih264dec still available" "element missing"
fi

if has_element vaapih265dec; then
    pass "vaapih265dec still available"
else
    fail "vaapih265dec still available" "element missing"
fi

# Decode an encoded file (round-trip)
if [ -f "$TMPDIR/h264.mp4" ]; then
    if gst-launch-1.0 -e filesrc location="$TMPDIR/h264.mp4" \
        ! qtdemux ! h264parse ! vaapih264dec ! fakesink 2>&1 | grep -q "EOS"; then
        pass "H.264 encode → decode round-trip"
    else
        fail "H.264 encode → decode round-trip" "decode pipeline error"
    fi
fi

# --- Stress ---

echo ""
echo "Stress:"

# Sequential pipeline restarts (leak check)
ALL_OK=1
for i in $(seq 1 10); do
    if ! gst-launch-1.0 -e videotestsrc num-buffers=10 \
        ! video/x-raw,width=320,height=240,framerate=30/1 \
        ! vaapih264enc ! fakesink 2>&1 | grep -q "EOS"; then
        ALL_OK=0
        break
    fi
done
if [ "$ALL_OK" = "1" ]; then
    pass "10 sequential H.264 pipeline restarts"
else
    fail "10 sequential H.264 pipeline restarts" "failed at iteration $i"
fi

# Long encode (300 frames)
if gst-launch-1.0 -e videotestsrc num-buffers=300 \
    ! video/x-raw,width=1920,height=1080,framerate=60/1 \
    ! vaapih264enc bitrate=8000 ! h264parse ! fakesink 2>&1 | grep -q "EOS"; then
    pass "H.264 1080p60 300 frames sustained"
else
    fail "H.264 1080p60 300 frames sustained" "pipeline error"
fi

# --- Summary ---

echo ""
echo "=== Results: $PASS passed, $FAIL failed, $SKIP skipped ==="
echo ""
exit $FAIL
