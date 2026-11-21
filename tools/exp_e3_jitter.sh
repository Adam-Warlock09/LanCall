#!/usr/bin/env bash
# exp_e3_jitter.sh — experiment E3: network jitter against the audio playout buffer. A bigger
# buffer rides out more jitter but adds delay; this measures both sides of that trade.
# One laptop: a lancall self-call over loopback (exact latency, one clock), with netem adding
# 30 ms delay plus J ms jitter.
#
# Usage (from the repo root, after building; use headphones):
#   tools/exp_e3_jitter.sh                 4 jitters x 5 buffers, 20 s each (~8 min)
#   JITTERS="0 20" BUFFERS="40 100" tools/exp_e3_jitter.sh
#
# results.csv:
#   jitter_ms,buffer_ms,audio_e2e_p50_ms,audio_e2e_p95_ms,late_drops,concealed
# late_drops = packets that arrived after their play time; concealed = 20 ms gaps filled
# with silence. The sweet spot is the smallest buffer where those stay near 0.
#
# Needs: a camera, mic and speaker (headphones!), and sudo for tc.
set -euo pipefail

BUILD=${BUILD:-build}
DURATION=${DURATION:-20}
JITTERS=${JITTERS:-"0 10 20 40"}
BUFFERS=${BUFFERS:-"20 40 60 100 200"}
OUT="experiments/e3_jitter_$(date +%Y%m%d_%H%M%S)"

[[ -x "$BUILD/call/lancall" ]] || { echo "build first: $BUILD/call/lancall missing"; exit 1; }
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

echo "jitter_ms,buffer_ms,audio_e2e_p50_ms,audio_e2e_p95_ms,late_drops,concealed" >"$OUT/results.csv"

for j in $JITTERS; do
    IFACE=lo tools/netem.sh on 0 30 "$j" >/dev/null
    for b in $BUFFERS; do
        tag="jitter${j}_buffer${b}"
        echo "== $tag ($DURATION s)"
        # stdin from /dev/null: lancall just runs the self-call until the timeout
        timeout --foreground -s INT "$DURATION" "$BUILD/call/lancall" --name e3 --peer 127.0.0.1 \
            --buffer-ms "$b" --log "$OUT/${tag}.csv" </dev/null >/dev/null 2>&1 || true

        "$BUILD/tools/log_summary" "$OUT/${tag}.csv" >"$OUT/${tag}_summary.txt" || true
        # timings line: name count min avg p50 p95 max; events line: name count N total T
        read -r p50 p95 < <(awk '$1 == "audio_e2e_ms" { print $5, $6; exit }' \
            "$OUT/${tag}_summary.txt") || true
        late=$(awk '$1 == "late_drop" { print $5; exit }' "$OUT/${tag}_summary.txt")
        conc=$(awk '$1 == "concealed" { print $5; exit }' "$OUT/${tag}_summary.txt")
        echo "$j,$b,${p50:-NA},${p95:-NA},${late:-0},${conc:-0}" >>"$OUT/results.csv"
        echo "   e2e p50 ${p50:-NA} ms, p95 ${p95:-NA} ms, late ${late:-0}, concealed ${conc:-0}"
    done
    IFACE=lo tools/netem.sh off >/dev/null
done

echo
echo "done: $OUT/results.csv"
column -s, -t "$OUT/results.csv"