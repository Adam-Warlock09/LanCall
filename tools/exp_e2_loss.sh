#!/usr/bin/env bash
# exp_e2_loss.sh — experiment E2: how video holds up under random packet loss, whole frames
# against strips. One laptop, over loopback (lo), with netem dropping packets.
#
# Usage (from the repo root, after building):
#   tools/exp_e2_loss.sh                 6 loss levels x 2 strip settings, 30 s each (~6 min)
#   DURATION=10 tools/exp_e2_loss.sh     quicker, noisier
#
# For every run it saves the sender and receiver logs, and at the end writes results.csv:
#   loss_set_pct,strips,shown_fps,measured_loss_pct
# shown_fps is what the viewer actually got; measured_loss_pct checks that netem did its job.
# The ladder is off (--fixed) so only the strips are being compared.
#
# Needs: a camera, a display for the receiver window, and sudo for tc.
set -euo pipefail

BUILD=${BUILD:-build}
DURATION=${DURATION:-30}
LOSSES=${LOSSES:-"0 1 2 5 10 20"}
STRIPS=${STRIPS:-"1 10"}
OUT="experiments/e2_loss_$(date +%Y%m%d_%H%M%S)"

[[ -x "$BUILD/video/video_sender" ]] || { echo "build first: $BUILD/video/video_sender missing"; exit 1; }
mkdir -p "$OUT"
sudo -v  # ask for the password once, up front

cleanup() { IFACE=lo tools/netem.sh off >/dev/null 2>&1 || true; }
trap cleanup EXIT

echo "loss_set_pct,strips,shown_fps,measured_loss_pct" >"$OUT/results.csv"

for loss in $LOSSES; do
    for strips in $STRIPS; do
        tag="loss${loss}_strips${strips}"
        echo "== $tag ($DURATION s)"
        IFACE=lo tools/netem.sh on "$loss" >/dev/null

        # receiver first, so the first frames aren't lost to a closed port
        timeout -s INT $((DURATION + 2)) "$BUILD/video/video_receiver" \
            --log "$OUT/${tag}_rx.csv" >/dev/null 2>&1 &
        rx=$!
        sleep 1
        timeout -s INT "$DURATION" "$BUILD/video/video_sender" 127.0.0.1 --strips "$strips" \
            --fixed --log "$OUT/${tag}_tx.csv" >/dev/null 2>&1 || true
        wait "$rx" || true
        IFACE=lo tools/netem.sh off >/dev/null

        # pull "loss X%" and "N fps" out of the receiver's summary line
        "$BUILD/tools/log_summary" "$OUT/${tag}_rx.csv" >"$OUT/${tag}_summary.txt" || true
        read -r measured fps < <(awk '/video\/0 loss/ {
                for (i = 1; i <= NF; i++) { if ($i == "loss") l = $(i + 1); if ($i == "fps") f = $(i - 1) }
                gsub("%", "", l); print l, f; exit }' "$OUT/${tag}_summary.txt")
        echo "$loss,$strips,${fps:-0},${measured:-NA}" >>"$OUT/results.csv"
        echo "   shown ${fps:-0} fps, measured loss ${measured:-NA}%"
    done
done

echo
echo "done: $OUT/results.csv"
column -s, -t "$OUT/results.csv"