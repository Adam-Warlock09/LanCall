#!/usr/bin/env bash
# exp_e8_repair.sh — experiment E8: repairing loss (phase 5). Random loss against four ways of
# coping with it: nothing (the strips alone, like E2), NACK retransmission, FEC parity, both.
# One laptop, over loopback, with netem dropping packets and adding a small delay so a NACK
# takes a realistic LAN round trip (~2 ms).
#
# Usage (from the repo root, after building):
#   tools/exp_e8_repair.sh                     3 losses x 4 modes, 10 s each (~3 min)
#   LOSSES="2 5 10 20" DURATION=20 tools/exp_e8_repair.sh
#
# results.csv:
#   loss_set_pct,mode,shown_fps,complete_frames_pct,loss_after_repair_pct,
#   nack_sent,resent,fec_recovered,upload_kbps
# complete_frames_pct is the share of the frames the receiver assembled that had every strip
# in (the point of all this). loss_after_repair_pct is what was still missing after NACK
# resends and FEC rebuilds.
# upload_kbps is everything the sender sent, parity and resends included: the price.
# The ladder is off (--fixed) so only the repair methods are compared.
#
# Needs: a camera, a display for the receiver window, and sudo for tc.
set -euo pipefail

BUILD=${BUILD:-build}
DURATION=${DURATION:-10}
LOSSES=${LOSSES:-"5 10 20"}
MODES=${MODES:-"none nack fec both"}
DELAY_MS=${DELAY_MS:-1}
OUT="experiments/e8_repair_$(date +%Y%m%d_%H%M%S)"

[[ -x "$BUILD/video/video_sender" ]] || { echo "build first: $BUILD/video/video_sender missing"; exit 1; }
mkdir -p "$OUT"
sudo -v

cleanup() { IFACE=lo tools/netem.sh off >/dev/null 2>&1 || true; }
trap cleanup EXIT
# Ctrl+C: the foreground program gets it too (timeout --foreground) and stops; here the
# programs still running in the background are stopped, then the EXIT trap turns netem off
on_interrupt() {
    echo
    echo "interrupted"
    # shellcheck disable=SC2046
    kill $(jobs -p) 2>/dev/null || true
    exit 130
}
trap on_interrupt INT TERM

echo "loss_set_pct,mode,shown_fps,complete_frames_pct,loss_after_repair_pct,nack_sent,resent,fec_recovered,upload_kbps" \
    >"$OUT/results.csv"

# events lines in log_summary: "  name   count N  total T"
event_count() { awk -v n="$1" '$1 == n { print $3; exit }' "$2"; }
event_total() { awk -v n="$1" '$1 == n { print $5; exit }' "$2"; }

for loss in $LOSSES; do
    for mode in $MODES; do
        case "$mode" in
            none) flags="--no-nack" ;;
            nack) flags="" ;;
            fec) flags="--fec --no-nack" ;;
            both) flags="--fec" ;;
            *) echo "unknown mode $mode (none, nack, fec, both)"; exit 2 ;;
        esac
        tag="loss${loss}_${mode}"
        echo "== $tag ($DURATION s)"
        IFACE=lo tools/netem.sh on "$loss" "$DELAY_MS" >/dev/null

        timeout -s INT $((DURATION + 2)) "$BUILD/video/video_receiver" \
            --log "$OUT/${tag}_rx.csv" >/dev/null 2>&1 &
        rx=$!
        sleep 1
        # $flags unquoted on purpose: zero, one or two options
        # shellcheck disable=SC2086
        timeout --foreground -s INT "$DURATION" "$BUILD/video/video_sender" 127.0.0.1 --fixed $flags \
            --log "$OUT/${tag}_tx.csv" >/dev/null 2>&1 || true
        wait "$rx" || true
        IFACE=lo tools/netem.sh off >/dev/null

        "$BUILD/tools/log_summary" "$OUT/${tag}_rx.csv" >"$OUT/${tag}_rx_summary.txt" || true
        "$BUILD/tools/log_summary" "$OUT/${tag}_tx.csv" >"$OUT/${tag}_tx_summary.txt" || true
        rxs="$OUT/${tag}_rx_summary.txt"
        txs="$OUT/${tag}_tx_summary.txt"

        # "video/0 loss 1.2% (34/2840) ... 15.0 fps": loss after NACK, lost/expected, fps
        read -r measured lost expected fps < <(awk '/video\/0 loss/ {
                for (i = 1; i <= NF; i++) {
                    if ($i == "loss") l = $(i + 1)
                    if ($i == "fps") f = $(i - 1)
                    if ($i ~ /^\([0-9]+\/[0-9]+\)$/) { c = $i; gsub(/[()]/, "", c); split(c, n, "/") }
                }
                gsub("%", "", l); print l, n[1], n[2], f; exit }' "$rxs") || true
        nacks=$(event_total nack_sent "$rxs")
        fixed=$(event_count fec_recovered "$rxs")
        # complete frames out of every frame the receiver handed on (frame_pushed, value = strips
        # missing); frame_shown counts only the newest per display pass, a different set of
        # frames, so dividing by it could even go negative. Older builds: frame_shown
        pushed=$(event_count frame_pushed "$rxs")
        [ -n "$pushed" ] || pushed=$(event_count frame_shown "$rxs")
        incomplete=$(event_count frame_incomplete "$rxs")
        complete=$(awk -v s="${pushed:-0}" -v i="${incomplete:-0}" \
            'BEGIN { if (s > 0) printf "%.1f", 100 * (1 - i / s); else print "NA" }')
        # loss left after repair: the R rows include NACK resends but not FEC rebuilds
        if [ -n "${expected:-}" ] && [ "${expected:-0}" -gt 0 ]; then
            measured=$(awk -v l="${lost:-0}" -v f="${fixed:-0}" -v e="$expected" \
                'BEGIN { r = l - f; if (r < 0) r = 0; printf "%.2f", 100 * r / e }')
        fi
        resent=$(event_total nack_resent "$txs")
        # sent lines: "  video/0   N packets  X pkt/s  Y kbps  (Z s)", parity is fec/0
        up=$(awk '/^sent:/ { s = 1; next } s && /^[a-z]/ { s = 0 }
                  s && ($1 ~ /^video\// || $1 ~ /^fec\//) {
                      for (i = 1; i <= NF; i++) if ($i == "kbps") t += $(i - 1) }
                  END { printf "%.0f", t }' "$txs")

        echo "$loss,$mode,${fps:-0},$complete,${measured:-NA},${nacks:-0},${resent:-0},${fixed:-0},${up:-0}" \
            >>"$OUT/results.csv"
        echo "   ${fps:-0} fps, ${complete}% frames complete, loss after repair ${measured:-NA}%, up ${up:-0} kbps"
    done
done

echo
echo "done: $OUT/results.csv"
column -s, -t "$OUT/results.csv"
