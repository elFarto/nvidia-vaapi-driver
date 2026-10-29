#!/bin/bash
set -u

export LIBVA_DRIVER_NAME=nvidia

PASS=0
FAIL=0
SKIP=0
TMPDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR"' EXIT

pass() { printf "  %-55s \033[32mPASS\033[0m %s\n" "$1" "${2:-}"; PASS=$((PASS+1)); }
fail() { printf "  %-55s \033[31mFAIL\033[0m (%s)\n" "$1" "$2"; FAIL=$((FAIL+1)); }
skip() { printf "  %-55s \033[33mSKIP\033[0m (%s)\n" "$1" "$2"; SKIP=$((SKIP+1)); }

finish() {
    echo ""
    echo "=== Render node selection tests: ${PASS} passed, ${FAIL} failed, ${SKIP} skipped ==="
    echo ""
    if [ "$FAIL" -gt 0 ]; then
        exit 1
    fi
    if [ "$PASS" -eq 0 ]; then
        exit 77
    fi
    exit 0
}

nvidia_render_nodes() {
    local node driver
    for node in /dev/dri/renderD*; do
        [ -e "$node" ] || continue
        driver=$(readlink -f "/sys/class/drm/${node##*/}/device/driver" 2>/dev/null)
        [ "${driver##*/}" = "nvidia" ] && echo "$node"
    done
}

echo ""
echo "=== nvidia-vaapi-driver render node selection tests ==="
echo ""

mapfile -t NODES < <(nvidia_render_nodes)
if [ "${#NODES[@]}" -eq 0 ]; then
    skip "NVIDIA render nodes" "none found"
    finish
fi

if ! ffmpeg -hide_banner -loglevel error -f lavfi \
        -i "testsrc2=size=320x180:rate=30:duration=2" \
        -c:v libx264 -pix_fmt yuv420p \
        -y "$TMPDIR/h264.mp4" 2>"$TMPDIR/enc.log"; then
    skip "H.264 fixture" "no libx264 encoder in this ffmpeg"
    finish
fi

if ! ffmpeg -hide_banner -loglevel error -i "$TMPDIR/h264.mp4" \
        -pix_fmt nv12 -f framemd5 -y "$TMPDIR/sw.md5" 2>"$TMPDIR/sw.log"; then
    fail "software reference" "software decode failed"
    finish
fi
FRAMES=$(grep -vc '^#' "$TMPDIR/sw.md5")

for node in "${NODES[@]}"; do
    pci=$(basename "$(readlink -f "/sys/class/drm/${node##*/}/device")")
    label="H.264 on ${node##*/} (${pci})"

    if ! ffmpeg -hide_banner -loglevel error \
            -hwaccel vaapi -hwaccel_device "$node" \
            -hwaccel_output_format vaapi \
            -i "$TMPDIR/h264.mp4" -vf 'hwdownload,format=nv12' \
            -f framemd5 -y "$TMPDIR/hw.md5" 2>"$TMPDIR/hw.log"; then
        fail "$label" "hardware decode failed: $(head -1 "$TMPDIR/hw.log")"
        continue
    fi

    decoded=$(grep -vc '^#' "$TMPDIR/hw.md5")
    bad=$(diff <(grep -v '^#' "$TMPDIR/hw.md5") <(grep -v '^#' "$TMPDIR/sw.md5") | grep -c '^<')
    if [ "$decoded" -ne "$FRAMES" ]; then
        fail "$label" "decoded $decoded of $FRAMES frames"
    elif [ "$bad" -ne 0 ]; then
        fail "$label" "$bad of $FRAMES frames differ from software decode"
    else
        pass "$label" "(bit-exact, $FRAMES frames)"
    fi
done

if [ "${#NODES[@]}" -lt 2 ]; then
    skip "more than one NVIDIA GPU" "only ${#NODES[@]} render node found"
fi

finish
