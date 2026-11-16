#!/usr/bin/env bash
# exp_e4_cap.sh — experiment E4: what adaptive video does when the bandwidth drops, compared
# with fixed quality. One laptop, over loopback, with a tbf cap switched on and off mid-run:
#
#   0 s ............ 15 s ................... 40 s ........................... 75 s
#   no cap           CAP Mbit/s                no cap (watch it climb back)
#
# Usage (from the repo root, after building):
#   tools/exp_e4_cap.sh               adaptive run, then fixed run, 2 Mbit/s cap (~3 min)
#   CAP=1 tools/exp_e4_cap.sh
#
# Per mode (adaptive, fixed) it writes:
#   <mode>_persecond.csv   log_summary --per-second of the receiver: rate, loss, fps each second
#   <mode>_levels.csv      t_s,level from the sender's video_level events (adaptive only)
# The gate: after the cap goes on at 15 s, the adaptive sender should step down within 5 s.
#
# Needs: a camera, a display for the receiver window, and sudo for tc.
set -euo pipefail

BUILD=${BUILD:-build}
CAP=${CAP:-2}
OUT="experiments/e4_cap_$(date +%Y%m%d_%H%M%S)"

[[ -x "$BUILD/video/video_sender" ]] || { echo "build first: $BUILD/video/video_sender missing"; exit 1; }
mkdir -p "$OUT"
sudo -v

cleanup() { IFACE=lo tools/netem.sh off >/dev/null 2>&1 || true; }
trap cleanup EXIT

for mode in adaptive fixed; do
    echo "== $mode: free 15 s, cap ${CAP} Mbit/s 25 s, free 35 s"
    flag=""
    [[ "$mode" == "fixed" ]] && flag="--fixed"

    timeout -s INT 78 "$BUILD/video/video_receiver" --log "$OUT/${mode}_rx.csv" >/dev/null 2>&1 &
    rx=$!
    sleep 1
    timeout -s INT 75 "$BUILD/video/video_sender" 127.0.0.1 $flag --log "$OUT/${mode}_tx.csv" \
        >"$OUT/${mode}_tx_stats.txt" 2>&1 &
    tx=$!

    sleep 15
    IFACE=lo tools/netem.sh cap "$CAP" >/dev/null
    echo "   cap on"
    sleep 25
    IFACE=lo tools/netem.sh off >/dev/null
    echo "   cap off"
    wait "$tx" || true
    wait "$rx" || true

    "$BUILD/tools/log_summary" --per-second "$OUT/${mode}_rx.csv" >"$OUT/${mode}_persecond.csv" || true
    # metrics rows are local_us,E,name,value; time is relative to the log's first row
    awk -F, 'NR == 1 { t0 = $1 } $2 == "E" && $3 == "video_level" {
                 printf "%.1f,%s\n", ($1 - t0) / 1e6, $4 }' "$OUT/${mode}_tx.csv" \
        | { echo "t_s,level"; cat; } >"$OUT/${mode}_levels.csv"
done

echo
echo "done: $OUT"
echo "adaptive level changes (t_s,level):"
cat "$OUT/adaptive_levels.csv"
echo "(the cap went on at ~15 s and off at ~40 s after the sender started)"