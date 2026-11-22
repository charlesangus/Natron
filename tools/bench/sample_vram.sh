#!/usr/bin/env bash
# Samples a GPU's VRAM (and on amdgpu GTT) use every INTERVAL seconds into OUT while PID lives:
#   tools/bench/sample_vram.sh DEVICE INTERVAL OUT PID
# DEVICE is a /sys/class/drm/cardN/device directory of an amdgpu card, or nvidia-smi:<index> for an
# NVIDIA GPU, read through nvidia-smi. OUT starts with
#   # device <DEVICE> vram_total_mb <n> gtt_total_mb <n>
# (no gtt_total_mb on NVIDIA) then one "<epoch seconds> <vram used bytes> <gtt used bytes>" line per
# sample, the GTT column "-" on NVIDIA, which has none. The counters are the device's, so other
# clients of the GPU (the desktop) count too.
#   tools/bench/sample_vram.sh --pick
# prints the device to sample: VRAM_CARD when set (cardN, or nvidia<index>), else the amdgpu card with
# the most VRAM, else, with no amdgpu card and nvidia-smi on PATH, the NVIDIA GPU with the most.
set -u
# EPOCHREALTIME takes the locale's decimal separator.
LC_ALL=C

pick_nvidia() {
    local index="${1:-}" best
    command -v nvidia-smi > /dev/null 2>&1 || return 1
    if [ -n "$index" ]; then
        nvidia-smi --id="$index" --query-gpu=index --format=csv,noheader,nounits > /dev/null 2>&1 || {
            echo "sample_vram.sh: nvidia-smi knows no GPU $index" >&2
            return 1
        }
        echo "nvidia-smi:$index"
        return 0
    fi
    best="$(nvidia-smi --query-gpu=index,memory.total --format=csv,noheader,nounits 2> /dev/null \
        | tr -d ' ' | sort -t, -k2,2 -nr | head -n 1 | cut -d, -f1)"
    [ -n "$best" ] || return 1
    echo "nvidia-smi:$best"
}

pick_device() {
    local dir best="" best_total=0 total
    if [ -n "${VRAM_CARD:-}" ]; then
        case "$VRAM_CARD" in
            nvidia[0-9]*)
                pick_nvidia "${VRAM_CARD#nvidia}"
                return $?
                ;;
        esac
        dir="/sys/class/drm/$VRAM_CARD/device"
        [ -r "$dir/mem_info_vram_used" ] && { echo "$dir"; return 0; }
        echo "sample_vram.sh: $dir has no mem_info_vram_used" >&2
        return 1
    fi
    for dir in /sys/class/drm/card[0-9]*/device; do
        if [ ! -r "$dir/mem_info_vram_total" ] || [ ! -r "$dir/mem_info_vram_used" ]; then
            continue
        fi
        read -r total < "$dir/mem_info_vram_total" || continue
        if [ "$total" -gt "$best_total" ]; then
            best="$dir"
            best_total="$total"
        fi
    done
    if [ -n "$best" ]; then
        readlink -f "$best"
        return 0
    fi
    pick_nvidia
}

if [ "${1:-}" = --pick ]; then
    pick_device
    exit $?
fi
[ $# -eq 4 ] || { echo "usage: $0 DEVICE INTERVAL OUT PID | $0 --pick" >&2; exit 2; }
device=$1
interval=$2
out=$3
pid=$4

read_or_zero() {
    local value=0
    [ -r "$1" ] && read -r value < "$1"
    printf '%s' "${value:-0}"
}

# nvidia-smi's own loop keeps the interval; starting it once per sample would cost tens of ms each.
sample_nvidia() {
    local index="${device#nvidia-smi:}" total ms smi line status
    total="$(nvidia-smi --id="$index" --query-gpu=memory.total --format=csv,noheader,nounits 2> /dev/null | tr -d ' ')"
    printf '# device %s vram_total_mb %d\n' "$device" "${total:-0}" > "$out"
    ms="$(awk -v s="$interval" 'BEGIN { ms = int(s * 1000); print (ms < 1 ? 1 : ms) }')"
    exec 3< <(exec nvidia-smi --id="$index" --query-gpu=memory.used --format=csv,noheader,nounits -lms "$ms" 2> /dev/null)
    smi=$!
    while kill -0 "$pid" 2> /dev/null; do
        if read -r -t 1 line <&3; then
            line="${line// /}"
            case "$line" in
                '' | *[!0-9]*) continue ;;
            esac
            printf '%s %d -\n' "$EPOCHREALTIME" $((line * 1048576)) >> "$out"
        else
            status=$?
            # Above 128 the read timed out, which only means no sample yet; anything else is the end of the stream.
            [ "$status" -gt 128 ] || break
        fi
    done
    kill "$smi" 2> /dev/null
    exec 3<&-
}

case "$device" in
    nvidia-smi:*)
        sample_nvidia
        exit 0
        ;;
esac

vram_total=$(read_or_zero "$device/mem_info_vram_total")
gtt_total=$(read_or_zero "$device/mem_info_gtt_total")
printf '# device %s vram_total_mb %d gtt_total_mb %d\n' "$device" $((vram_total / 1048576)) $((gtt_total / 1048576)) > "$out"
while kill -0 "$pid" 2> /dev/null; do
    vram=0
    gtt=0
    read -r vram < "$device/mem_info_vram_used"
    [ -r "$device/mem_info_gtt_used" ] && read -r gtt < "$device/mem_info_gtt_used"
    printf '%s %s %s\n' "$EPOCHREALTIME" "$vram" "$gtt" >> "$out"
    sleep "$interval"
done
