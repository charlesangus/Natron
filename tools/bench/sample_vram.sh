#!/usr/bin/env bash
# Samples an amdgpu device's VRAM and GTT use every INTERVAL seconds into OUT while PID lives:
#   tools/bench/sample_vram.sh DEVICE INTERVAL OUT PID
# DEVICE is a /sys/class/drm/cardN/device directory. OUT starts with
#   # device <DEVICE> vram_total_mb <n> gtt_total_mb <n>
# then one "<epoch seconds> <vram used bytes> <gtt used bytes>" line per sample. The counters are the
# device's, so other clients of the GPU (the desktop) count too.
#   tools/bench/sample_vram.sh --pick
# prints the device to sample: VRAM_CARD (cardN) when set, else the card with the most VRAM.
set -u
# EPOCHREALTIME takes the locale's decimal separator.
LC_ALL=C

pick_device() {
    local dir best="" best_total=0 total
    if [ -n "${VRAM_CARD:-}" ]; then
        dir="/sys/class/drm/$VRAM_CARD/device"
        [ -r "$dir/mem_info_vram_used" ] && { echo "$dir"; return 0; }
        echo "sample_vram.sh: $dir has no mem_info_vram_used" >&2
        return 1
    fi
    for dir in /sys/class/drm/card[0-9]*/device; do
        [ -r "$dir/mem_info_vram_total" ] && [ -r "$dir/mem_info_vram_used" ] || continue
        read -r total < "$dir/mem_info_vram_total" || continue
        if [ "$total" -gt "$best_total" ]; then
            best="$dir"
            best_total="$total"
        fi
    done
    [ -n "$best" ] || return 1
    readlink -f "$best"
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
