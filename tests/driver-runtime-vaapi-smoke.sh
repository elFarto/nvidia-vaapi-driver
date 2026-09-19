#!/bin/sh
set -eu

: "${VAAPI_DEVICE:=/dev/dri/renderD128}"
: "${NVD_RUNTIME_SIZE:=192x192}"
: "${NVD_RUNTIME_DURATION:=1}"
: "${NVD_RUNTIME_RC_MODES:=CBR VBR CQP}"
: "${NVD_RUNTIME_CODECS:=all}"
: "${NVD_RUNTIME_UI:=auto}"

if ! command -v vainfo >/dev/null 2>&1; then
    echo "vainfo is required for runtime tests" >&2
    exit 1
fi
if ! command -v ffmpeg >/dev/null 2>&1; then
    echo "ffmpeg is required for runtime tests" >&2
    exit 1
fi
if [ ! -e "$VAAPI_DEVICE" ]; then
    echo "VAAPI device not found: $VAAPI_DEVICE" >&2
    exit 1
fi

tmpdir="$(mktemp -d)"
dashboard_active=0

dashboard_teardown() {
    if [ "$dashboard_active" -eq 1 ]; then
        # Show cursor and return to normal screen buffer.
        printf '\033[?25h\033[?1049l' > /dev/tty 2>/dev/null || true
        dashboard_active=0
    fi
}

cleanup() {
    dashboard_teardown
    rm -rf "$tmpdir"
}
trap cleanup EXIT

vainfo_log="$tmpdir/vainfo.log"
state_tsv="$tmpdir/runtime-state.tsv"
queue_tsv="$tmpdir/runtime-queue.tsv"
summary_tsv="$tmpdir/runtime-summary.tsv"

tty_available=0
if (: > /dev/tty) 2>/dev/null; then
    tty_available=1
fi

ui_mode="plain"
case "$NVD_RUNTIME_UI" in
    auto)
        if [ "$tty_available" -eq 1 ]; then
            ui_mode="dashboard"
        fi
        ;;
    dashboard)
        if [ "$tty_available" -eq 1 ]; then
            ui_mode="dashboard"
        else
            ui_mode="plain"
        fi
        ;;
    plain)
        ui_mode="plain"
        ;;
    *)
        echo "unsupported NVD_RUNTIME_UI: $NVD_RUNTIME_UI (supported: auto plain dashboard)" >&2
        exit 1
        ;;
esac

plain_log() {
    if [ "$ui_mode" = "plain" ]; then
        printf '%s\n' "$1"
    fi
}

dashboard_setup() {
    if [ "$ui_mode" != "dashboard" ]; then
        return 0
    fi
    # Enter alternate screen buffer and hide cursor so redraws do not pollute main scrollback.
    printf '\033[?1049h\033[?25l' > /dev/tty 2>/dev/null || true
    dashboard_active=1
}

render_dashboard() {
    current_msg="$1"
    if [ "$ui_mode" != "dashboard" ]; then
        return 0
    fi

    {
        printf '\033[H\033[J'
        printf 'nvidia-vaapi-driver runtime matrix\n'
        printf 'Progress: [%d/%d]  filter=%s  rc=%s  size=%s  duration=%ss\n' \
            "$run_index" "$run_total" "$selected_codecs" "$NVD_RUNTIME_RC_MODES" "$NVD_RUNTIME_SIZE" "$NVD_RUNTIME_DURATION"
        printf 'Current : %s\n' "$current_msg"
        printf '\n'
        printf '%-3s %-5s %-33s %-4s %-8s %-18s %-10s %s\n' \
            "#" "codec" "profile" "rc" "status" "encoder" "pixfmt" "reason"
        awk -F'\t' '
            NR == 1 { next }
            {
                printf("%-3s %-5s %-33s %-4s %-8s %-18s %-10s %s\n",
                    $1, $2, $3, $4, $8, $5, $7, $9)
            }
        ' "$state_tsv"
    } > /dev/tty
}

set_case_state() {
    case_id="$1"
    new_status="$2"
    new_reason="$3"
    tmp_state="$tmpdir/runtime-state.next.tsv"
    awk -F'\t' -v OFS='\t' -v case_id="$case_id" -v new_status="$new_status" -v new_reason="$new_reason" '
        NR == 1 { print; next }
        $1 == case_id { $8 = new_status; $9 = new_reason }
        { print }
    ' "$state_tsv" >"$tmp_state"
    mv "$tmp_state" "$state_tsv"
}

format_case_msg() {
    step="$1"
    idx="$2"
    total="$3"
    codec="$4"
    profile="$5"
    rc="$6"
    printf '[%s] [%s/%s] codec=%s profile=%s rc=%s' "$step" "$idx" "$total" "$codec" "$profile" "$rc"
}

vainfo --display drm --device "$VAAPI_DEVICE" >"$vainfo_log" 2>&1
if ! grep -q "VA-API NVDEC/NVENC driver" "$vainfo_log"; then
    echo "unexpected vainfo output; this driver does not appear active" >&2
    cat "$vainfo_log" >&2
    exit 1
fi

encode_profiles="$(awk '/VAEntrypointEncSlice/ { gsub(":", "", $1); print $1 }' "$vainfo_log" | sort -u)"
if [ -z "$encode_profiles" ]; then
    echo "no encode profiles advertised by vainfo" >&2
    cat "$vainfo_log" >&2
    exit 1
fi

available_encoders="$(
    ffmpeg -hide_banner -encoders 2>/dev/null \
        | awk '/^[[:space:]]*V/ && $2 ~ /_vaapi$/ { print $2 }' \
        | sort -u
)"

encoder_available() {
    encoder_name="$1"
    printf '%s\n' "$available_encoders" | grep -Fxq "$encoder_name"
}

normalize_codec_name() {
    codec_in="$(printf '%s' "$1" | tr '[:upper:]' '[:lower:]' | tr -d '[:space:]')"
    case "$codec_in" in
        all)
            echo "all"
            ;;
        h264|h.264|avc)
            echo "h264"
            ;;
        h265|h.265|hevc)
            echo "hevc"
            ;;
        av1|vp8|vp9)
            echo "$codec_in"
            ;;
        *)
            return 1
            ;;
    esac
}

selected_codecs=""
for raw_codec in $(printf '%s' "$NVD_RUNTIME_CODECS" | tr ',/' '  '); do
    normalized_codec="$(normalize_codec_name "$raw_codec")" || {
        echo "unsupported codec selector in NVD_RUNTIME_CODECS: $raw_codec" >&2
        echo "supported selectors: all h264 h265 hevc av1 vp8 vp9" >&2
        exit 1
    }
    case " $selected_codecs " in
        *" $normalized_codec "*) ;;
        *)
            selected_codecs="$selected_codecs $normalized_codec"
            ;;
    esac
done
if [ -z "$selected_codecs" ]; then
    selected_codecs=" all"
fi
selected_codecs="$(printf '%s' "$selected_codecs" | sed 's/^ *//;s/ *$//')"

codec_selected() {
    codec_name="$1"
    case " $selected_codecs " in
        *" all "*) return 0 ;;
        *" $codec_name "*) return 0 ;;
    esac
    return 1
}

map_profile() {
    va_profile="$1"
    codec=""
    encoder=""
    ff_profile=""
    pixfmts=""

    case "$va_profile" in
        VAProfileH264ConstrainedBaseline)
            codec="h264"
            encoder="h264_vaapi"
            ff_profile="constrained_baseline"
            pixfmts="nv12"
            ;;
        VAProfileH264Main)
            codec="h264"
            encoder="h264_vaapi"
            ff_profile="main"
            pixfmts="nv12"
            ;;
        VAProfileH264High)
            codec="h264"
            encoder="h264_vaapi"
            ff_profile="high"
            pixfmts="nv12"
            ;;
        VAProfileH264High10)
            codec="h264"
            encoder="h264_vaapi"
            ff_profile="high10"
            pixfmts="p010le"
            ;;
        VAProfileHEVCMain)
            codec="hevc"
            encoder="hevc_vaapi"
            ff_profile="main"
            pixfmts="nv12"
            ;;
        VAProfileHEVCMain10)
            codec="hevc"
            encoder="hevc_vaapi"
            ff_profile="main10"
            pixfmts="p010le"
            ;;
        VAProfileHEVCMain422_10)
            codec="hevc"
            encoder="hevc_vaapi"
            ff_profile="rext"
            pixfmts="p210le nv16"
            ;;
        VAProfileHEVCMain444)
            codec="hevc"
            encoder="hevc_vaapi"
            ff_profile="rext"
            pixfmts="yuv444p"
            ;;
        VAProfileHEVCMain444_10)
            codec="hevc"
            encoder="hevc_vaapi"
            ff_profile="rext"
            pixfmts="yuv444p10le"
            ;;
        VAProfileAV1Profile0)
            codec="av1"
            encoder="av1_vaapi"
            ff_profile="main"
            pixfmts="nv12"
            ;;
        VAProfileVP8Version0_3)
            codec="vp8"
            encoder="vp8_vaapi"
            ff_profile=""
            pixfmts="nv12"
            ;;
        VAProfileVP9Profile0)
            codec="vp9"
            encoder="vp9_vaapi"
            ff_profile=""
            pixfmts="nv12"
            ;;
        *)
            return 1
            ;;
    esac

    if [ -z "$pixfmts" ]; then
        return 1
    fi

    return 0
}

printf 'idx\tcodec\tva_profile\trc\tencoder\tff_profile\tpixfmt\tstatus\treason\n' >"$state_tsv"
case_id=0
for va_profile in $encode_profiles; do
    if map_profile "$va_profile"; then
        for rc_mode in $NVD_RUNTIME_RC_MODES; do
            for pixfmt in $pixfmts; do
                case_id=$((case_id + 1))
                status="PENDING"
                reason="-"
                if ! codec_selected "$codec"; then
                    status="SKIP"
                    reason="codec_filtered"
                elif ! encoder_available "$encoder"; then
                    status="SKIP"
                    reason="missing_ffmpeg_encoder"
                fi
                printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
                    "$case_id" "$codec" "$va_profile" "$rc_mode" "$encoder" "${ff_profile:--}" "$pixfmt" "$status" "$reason" >>"$state_tsv"
            done
        done
    else
        for rc_mode in $NVD_RUNTIME_RC_MODES; do
            case_id=$((case_id + 1))
            printf '%s\t-\t%s\t%s\t-\t-\t-\tSKIP\tunmapped_profile\n' \
                "$case_id" "$va_profile" "$rc_mode" >>"$state_tsv"
        done
    fi
done

cp "$state_tsv" "$queue_tsv"

run_total="$(awk -F'\t' 'NR > 1 && $8 == "PENDING" { c++ } END { print c + 0 }' "$state_tsv")"
run_index=0
failures=0
skips=0

plain_log "runtime codec filter: $selected_codecs"
plain_log "runtime planned cases: total=$case_id runnable=$run_total skipped=$((case_id - run_total))"

printf 'codec\tva_profile\trc\tencoder\tff_profile\tpixfmt\tstatus\tencode\tdecode\treason\n' >"$summary_tsv"

dashboard_setup
render_dashboard "-"

run_encode_decode_case() {
    va_profile="$1"
    rc_mode="$2"
    encoder="$3"
    ff_profile="$4"
    pixfmt="$5"

    output_file="$tmpdir/${va_profile}_${rc_mode}.mkv"
    encode_log="$tmpdir/${va_profile}_${rc_mode}_encode.log"
    decode_log="$tmpdir/${va_profile}_${rc_mode}_decode.log"

    set -- ffmpeg -hide_banner -nostdin -loglevel warning \
        -vaapi_device "$VAAPI_DEVICE" \
        -f lavfi -i "testsrc2=size=$NVD_RUNTIME_SIZE:rate=30" -t "$NVD_RUNTIME_DURATION" \
        -vf "format=$pixfmt,hwupload" \
        -c:v "$encoder"
    if [ "$ff_profile" != "-" ]; then
        set -- "$@" -profile:v "$ff_profile"
    fi
    case "$rc_mode" in
        CBR)
            set -- "$@" -rc_mode CBR -b:v 2M -qmin 18 -qmax 40
            ;;
        VBR)
            set -- "$@" -rc_mode VBR -b:v 2M -maxrate 4M -qmin 18 -qmax 40
            ;;
        CQP)
            set -- "$@" -rc_mode CQP
            case "$encoder" in
                h264_vaapi|hevc_vaapi)
                    set -- "$@" -qp 30 -qmin 20 -qmax 40
                    ;;
            esac
            ;;
        *)
            echo "unsupported runtime RC mode: $rc_mode" >&2
            return 2
            ;;
    esac
    set -- "$@" -y "$output_file"

    if ! "$@" >"$encode_log" 2>&1; then
        return 1
    fi

    if ! ffmpeg -hide_banner -nostdin -loglevel warning \
        -hwaccel vaapi -hwaccel_output_format vaapi \
        -vaapi_device "$VAAPI_DEVICE" \
        -i "$output_file" -f null - >"$decode_log" 2>&1; then
        return 3
    fi

    return 0
}

while IFS="$(printf '\t')" read -r row_id codec va_profile rc_mode encoder ff_profile pixfmt status reason; do
    if [ "$row_id" = "idx" ]; then
        continue
    fi

    if [ "$status" = "SKIP" ]; then
        skips=$((skips + 1))
        printf '%s\t%s\t%s\t%s\t%s\t%s\tSKIP\tSKIP\tSKIP\t%s\n' \
            "$codec" "$va_profile" "$rc_mode" "$encoder" "$ff_profile" "$pixfmt" "$reason" >>"$summary_tsv"
        if [ "$ui_mode" = "plain" ]; then
            printf '[SKIP] codec=%s profile=%s rc=%s reason=%s\n' \
                "$codec" "$va_profile" "$rc_mode" "$reason"
        fi
        continue
    fi

    run_index=$((run_index + 1))
    set_case_state "$row_id" "RUNNING" "-"
    current_msg="$(format_case_msg RUNNING "$run_index" "$run_total" "$codec" "$va_profile" "$rc_mode")"
    if [ "$ui_mode" = "plain" ]; then
        printf '[%d/%d] RUNNING codec=%s profile=%s rc=%s encoder=%s ff_profile=%s pixfmt=%s\n' \
            "$run_index" "$run_total" "$codec" "$va_profile" "$rc_mode" "$encoder" "$ff_profile" "$pixfmt"
    else
        render_dashboard "$current_msg"
    fi

    case_status="FAIL"
    encode_status="FAIL"
    decode_status="SKIP"
    reason_text="encode_failed"

    if run_encode_decode_case "$va_profile" "$rc_mode" "$encoder" "$ff_profile" "$pixfmt"; then
        run_rc=0
    else
        run_rc=$?
    fi
    case "$run_rc" in
        0)
            case_status="OK"
            encode_status="OK"
            decode_status="OK"
            reason_text="-"
            ;;
        1)
            reason_text="encode_failed"
            ;;
        2)
            reason_text="invalid_rc_mode"
            ;;
        3)
            encode_status="OK"
            decode_status="FAIL"
            reason_text="decode_failed"
            ;;
        *)
            reason_text="unexpected_error"
            ;;
    esac

    set_case_state "$row_id" "$case_status" "$reason_text"
    if [ "$ui_mode" = "plain" ]; then
        printf '[%d/%d] %s codec=%s profile=%s rc=%s reason=%s\n' \
            "$run_index" "$run_total" "$case_status" "$codec" "$va_profile" "$rc_mode" "$reason_text"
    else
        render_dashboard "$(format_case_msg "$case_status" "$run_index" "$run_total" "$codec" "$va_profile" "$rc_mode")"
    fi

    if [ "$case_status" = "FAIL" ]; then
        failures=$((failures + 1))
    fi

    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
        "$codec" "$va_profile" "$rc_mode" "$encoder" "$ff_profile" "$pixfmt" \
        "$case_status" "$encode_status" "$decode_status" "$reason_text" >>"$summary_tsv"
done <"$queue_tsv"

dashboard_teardown

if [ "$run_total" -eq 0 ]; then
    echo "no runnable runtime cases (all skipped, codec filter: $selected_codecs)" >&2
    exit 1
fi

if [ "$failures" -ne 0 ]; then
    echo "runtime matrix failed: failures=$failures, skips=$skips, runnable=$run_total" >&2
else
    echo "runtime matrix passed on device $VAAPI_DEVICE (OK=$run_total SKIP=$skips FAIL=0)"
fi

echo "runtime matrix summary:"
if command -v column >/dev/null 2>&1; then
    column -t -s "$(printf '\t')" "$summary_tsv"
else
    cat "$summary_tsv"
fi

if [ "$failures" -ne 0 ]; then
    exit 1
fi
