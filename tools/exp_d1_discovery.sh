#!/usr/bin/env bash
# exp_d1_discovery.sh — test D1 (phase 10): how long until a laptop that just started lancall
# shows up in someone else's `list` (target: within 5 s; beacons go out every 2 s).
#
# One laptop, one clock: a "Probe" lancall (signaling port 5013) types `list` every POLL_S s
# and timestamps what it prints; then a "Newbie" lancall starts, and the discovery time is
# Newbie's start -> the first `list` line that shows Newbie. Repeated RUNS times, because where
# the first beacon falls against the probe's polling varies. Resolution: POLL_S.
#
# Usage (from the repo root, after building; any floor, wired port):
#   tools/exp_d1_discovery.sh
#   RUNS=5 tools/exp_d1_discovery.sh
#
# Output: experiments/d1_discovery_<time>/results.csv (run,discovery_s) plus each run's raw
# probe output, and the mean and max on screen.
#
# If Newbie never shows up, this laptop doesn't hear its own broadcast beacons: run Probe and
# Newbie on two laptops of one floor instead, started on a spoken countdown (+-0.5 s).
#
# Environment: RUNS=3 POLL_S=0.2 WAIT_S=10 (how long to wait for Newbie) BUILD=build
set -euo pipefail

BUILD=${BUILD:-build}
RUNS=${RUNS:-3}
POLL_S=${POLL_S:-0.2}
WAIT_S=${WAIT_S:-10}

[[ "${1:-}" == "-h" || "${1:-}" == "--help" ]] && {
    awk 'NR > 1 && /^#/ { sub(/^# ?/, ""); print; next } NR > 1 { exit }' "$0"
    exit 0
}
[[ -x "$BUILD/call/lancall" ]] || { echo "build first: $BUILD/call/lancall missing"; exit 1; }
OUT="experiments/d1_discovery_$(date +%Y%m%d_%H%M%S)"
mkdir -p "$OUT"
echo "run,discovery_s" >"$OUT/results.csv"

polls=$(awk -v w="$WAIT_S" -v p="$POLL_S" 'BEGIN { printf "%d", (w + 5) / p }')

for run in $(seq "$RUNS"); do
    echo "== run $run"
    # the probe: `list` every POLL_S s, each output line stamped with the time it was printed
    # (stdbuf: lancall doesn't flush after `list` when its output is a pipe)
    { sleep 1; for _ in $(seq "$polls"); do echo list; sleep "$POLL_S"; done; echo quit; } |
        stdbuf -oL "$BUILD/call/lancall" --name Probe --port 5013 --log "$OUT/probe_$run.csv" 2>&1 |
        while IFS= read -r line; do echo "$(date +%s.%N) $line"; done >"$OUT/probe_$run.txt" &
    probe=$!
    sleep 3  # let the probe start and send its own first beacon

    start=$(date +%s.%N)
    timeout -s INT "$WAIT_S" "$BUILD/call/lancall" --name Newbie --log "$OUT/newbie_$run.csv" \
        </dev/null >/dev/null 2>&1 || true
    wait "$probe" 2>/dev/null || true

    # list lines look like "<stamp>   Newbie   10.8.66.20   seen 0.4 s ago"
    found=$(awk '$2 == "Newbie" && $0 ~ /seen/ { print $1; exit }' "$OUT/probe_$run.txt")
    if [[ -z "$found" ]]; then
        echo "   Newbie never listed (see $OUT/probe_$run.txt)"
        echo "$run,NA" >>"$OUT/results.csv"
        continue
    fi
    secs=$(awk -v a="$start" -v b="$found" 'BEGIN { printf "%.2f", b - a }')
    echo "   listed after $secs s"
    echo "$run,$secs" >>"$OUT/results.csv"
done

echo
awk -F, 'NR > 1 && $2 != "NA" { s += $2; n++; if ($2 > m) m = $2 }
    END { if (n) printf "discovery: mean %.2f s, max %.2f s over %d runs (target < 5 s)\n", s / n, m, n
          else print "discovery: Newbie never listed: use two laptops (see the top of this script)" }' \
    "$OUT/results.csv"
echo "done: $OUT/results.csv"
