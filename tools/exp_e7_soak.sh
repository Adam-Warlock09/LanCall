#!/usr/bin/env bash
# exp_e7_soak.sh — experiment E7 (phase 10): the soak. One call left running for ~30 min to
# catch what a 2-minute test can't: memory that keeps growing, latency that creeps up, clock
# sync drifting, fps sagging, a crash after millions of packets.
#
# Usage (from the repo root, after building; headphones on):
#   tools/exp_e7_soak.sh run <name> [more lancall options]
#       starts lancall --name <name> in this terminal (type call / accept / stats as usual)
#       and samples its memory and CPU every SAMPLE_S s in the background. Hang up or quit
#       after ~30 min; the summary is printed and saved when lancall exits.
#       e.g. laptop 1: tools/exp_e7_soak.sh run L1          (then: call L2)
#            laptop 2: tools/exp_e7_soak.sh run L2          (then: accept)
#   tools/exp_e7_soak.sh summarize <experiments/e7_soak_...>   redo the summary of a run
#
# Output in experiments/e7_soak_<name>_<time>/:
#   lancall.csv     the lancall log
#   resources.csv   t_s,rss_kb,cpu_pct  (t_s since lancall started; cpu_pct over the last
#                   interval, 100 = one full core)
#   summary.txt     the first WINDOW_S of the call (after WARMUP_S) against the last WINDOW_S:
#                   audio/video e2e p50 and p95, A/V sync, clock-sync rtt, fps shown, kbit/s
#                   received, concealed / underrun / drift-trim counts; memory 5 min in
#                   against the end; mean CPU. Plus pass/fail lines for the soak gate.
#
# Environment:
#   SAMPLE_S=10 WARMUP_S=60 WINDOW_S=300 BUILD=build
#   MIN_CALL_S=1500   the gate's shortest acceptable call (e.g. 840 for a 15 min soak)
#
# Needs: Linux (/proc) for the memory and CPU samples.
set -euo pipefail

BUILD=${BUILD:-build}
SAMPLE_S=${SAMPLE_S:-10}
WARMUP_S=${WARMUP_S:-60}
WINDOW_S=${WINDOW_S:-300}
MIN_CALL_S=${MIN_CALL_S:-1500}

usage() {
    awk 'NR > 1 && /^#/ { sub(/^# ?/, ""); print; next } NR > 1 { exit }' "$0"
    exit 2
}

# Nearest-rank percentile (the ceil(p*n)-th smallest) of the numbers in a file, or NA.
percentile() {
    [[ -s "$1" ]] || { echo NA; return; }
    sort -g "$1" | awk -v p="$2" '{ v[NR] = $1 }
        END { r = int(p * NR); if (r < p * NR) r++; if (r < 1) r = 1; printf "%.1f", v[r] }'
}

# Samples memory and CPU of the process whose command line holds $1 until it exits.
sample() {
    local marker=$1 out=$2 pid="" tick t0 t prev_ticks="" prev_t ticks rss
    tick=$(getconf CLK_TCK)
    for _ in $(seq 100); do
        pid=$(pgrep -f -- "$marker" | head -1 || true)
        [[ -n "$pid" ]] && break
        sleep 0.1
    done
    [[ -n "$pid" ]] || { echo "[soak] lancall not found, no resource samples" >&2; return; }
    echo "t_s,rss_kb,cpu_pct" >"$out"
    t0=$(date +%s.%N)
    while [[ -r /proc/$pid/stat ]]; do
        t=$(date +%s.%N)
        ticks=$(awk '{ print $14 + $15 }' "/proc/$pid/stat" 2>/dev/null) || break
        rss=$(awk '/^VmRSS:/ { print $2 }' "/proc/$pid/status" 2>/dev/null) || break
        if [[ -n "$prev_ticks" ]]; then
            awk -v t="$t" -v t0="$t0" -v pt="$prev_t" -v d="$((ticks - prev_ticks))" \
                -v hz="$tick" -v rss="$rss" \
                'BEGIN { printf "%.0f,%d,%.1f\n", t - t0, rss, 100 * d / hz / (t - pt) }' >>"$out"
        fi
        prev_ticks=$ticks prev_t=$t
        sleep "$SAMPLE_S"
    done
}

summarize() {
    local dir=$1 log=$1/lancall.csv res=$1/resources.csv
    [[ -r "$log" ]] || { echo "no $log"; exit 1; }
    local tmp
    tmp=$(mktemp -d)

    # pass 1: the call's start and end (seconds since the first row, clock unwrapped)
    local start end
    read -r start end < <(awk -F, '
        $1 !~ /^[0-9]+$/ { next }
        { us = $1 + 0
          if (!have) { have = 1; prev = us; t = 0 }
          else { d = us - prev; if (d < -2147483648) d += 4294967296; else if (d > 2147483648) d -= 4294967296; prev = us; t += d }
          s = t / 1e6; last = s }
        $2 == "E" && $3 == "call_started" && start == "" { start = s }
        $2 == "E" && $3 == "call_ended" && start != "" && end == "" { end = s }
        END { if (start == "") start = -1; if (end == "") end = last; printf "%.3f %.3f\n", start, end }' "$log")
    if [[ "$start" == "-1.000" ]]; then
        echo "no call_started in $log: nothing to summarize"
        rm -rf "$tmp"
        exit 1
    fi

    local length a_from a_to b_from b_to
    length=$(awk -v a="$start" -v b="$end" 'BEGIN { printf "%.0f", b - a }')
    a_from=$(awk -v s="$start" -v w="$WARMUP_S" 'BEGIN { print s + w }')
    a_to=$(awk -v s="$a_from" -v w="$WINDOW_S" 'BEGIN { print s + w }')
    b_to=$(printf "%.0f" "$end")
    b_from=$(awk -v e="$end" -v w="$WINDOW_S" 'BEGIN { print e - w }')
    local note=""
    if awk -v a="$a_to" -v b="$b_from" 'BEGIN { exit !(a > b) }'; then
        note="  (call shorter than WARMUP_S + 2 x WINDOW_S: the two windows overlap)"
    fi

    # pass 2: per window, the timing events to files and the counters
    local counts
    counts=$(awk -F, -v a1="$a_from" -v a2="$a_to" -v b1="$b_from" -v b2="$b_to" -v dir="$tmp" '
        $1 !~ /^[0-9]+$/ { next }
        { us = $1 + 0
          if (!have) { have = 1; prev = us; t = 0 }
          else { d = us - prev; if (d < -2147483648) d += 4294967296; else if (d > 2147483648) d -= 4294967296; prev = us; t += d }
          s = t / 1e6
          w = ""; if (s >= a1 && s <= a2) w = "A"; if (s >= b1 && s <= b2) w = w "B" }
        w == "" { next }
        {
            for (i = 1; i <= 2; i++) {
                x = (i == 1) ? "A" : "B"
                if (!index(w, x)) continue
                if ($2 == "R") rx[x] += $8
                if ($2 == "E") {
                    if ($3 == "audio_e2e_ms" || $3 == "video_e2e_ms" || $3 == "av_sync_ms" || $3 == "rtt_ms")
                        print $4 > (dir "/" x "_" $3)
                    if ($3 == "frame_shown") frames[x]++
                    if ($3 == "concealed" || $3 == "underrun" || $3 == "drift_trim") ev[x "_" $3] += $4
                }
            }
        }
        END {
            for (i = 1; i <= 2; i++) {
                x = (i == 1) ? "A" : "B"
                secs = (x == "A") ? a2 - a1 : b2 - b1
                printf "%s %.1f %.1f %d %d %d\n", x, frames[x] / secs, rx[x] * 8 / secs / 1000,
                       ev[x "_concealed"], ev[x "_underrun"], ev[x "_drift_trim"]
            }
        }' "$log")

    local fpsA kbpsA concA undA trimA fpsB kbpsB concB undB trimB
    read -r _ fpsA kbpsA concA undA trimA < <(echo "$counts" | awk '$1 == "A"')
    read -r _ fpsB kbpsB concB undB trimB < <(echo "$counts" | awk '$1 == "B"')

    local row metric
    {
        echo "E7 soak: $dir"
        echo "call length ${length} s; first window ${a_from}-${a_to} s, last window ${b_from}-${b_to} s (since the log's first row)$note"
        echo
        printf "%-20s %12s %12s %10s\n" metric first last change
        for metric in audio_e2e_ms:0.5 audio_e2e_ms:0.95 video_e2e_ms:0.5 video_e2e_ms:0.95 \
            av_sync_ms:0.5 rtt_ms:0.5; do
            local name=${metric%%:*} p=${metric##*:} va vb
            va=$(percentile "$tmp/A_$name" "$p")
            vb=$(percentile "$tmp/B_$name" "$p")
            row="${name%_ms}_p$(awk -v p="$p" 'BEGIN { printf "%d", p * 100 }')_ms"
            printf "%-20s %12s %12s %10s\n" "$row" "$va" "$vb" \
                "$(awk -v a="$va" -v b="$vb" 'BEGIN { if (a == "NA" || b == "NA") print "NA"; else printf "%+.1f", b - a }')"
            echo "$row,$va,$vb" >>"$tmp/rows"
        done
        printf "%-20s %12s %12s %10s\n" fps_shown "$fpsA" "$fpsB" "$(awk -v a="$fpsA" -v b="$fpsB" 'BEGIN { printf "%+.1f", b - a }')"
        printf "%-20s %12s %12s %10s\n" rx_kbps "$kbpsA" "$kbpsB" "$(awk -v a="$kbpsA" -v b="$kbpsB" 'BEGIN { printf "%+.1f", b - a }')"
        printf "%-20s %12s %12s\n" concealed "$concA" "$concB"
        printf "%-20s %12s %12s\n" underruns "$undA" "$undB"
        printf "%-20s %12s %12s\n" drift_trims "$trimA" "$trimB"

        local rss_early="NA" rss_last="NA" rss_max="NA" cpu_mean="NA" growth="NA"
        if [[ -s "$res" ]] && (($(wc -l <"$res") > 1)); then
            read -r rss_early rss_last rss_max cpu_mean < <(awk -F, 'NR == 1 { next }
                { last = $2; if ($2 > max) max = $2
                  if (early == "" && $1 >= 300) early = $2
                  all += $3; na++
                  if ($1 >= 60) { cpu += $3; nc++ } }
                END { if (early == "") early = last
                      printf "%d %d %d %.1f\n", early, last, max, nc ? cpu / nc : (na ? all / na : 0) }' "$res")
            growth=$(awk -v a="$rss_early" -v b="$rss_last" 'BEGIN { printf "%+.1f", a ? 100 * (b - a) / a : 0 }')
        fi
        echo
        echo "memory: RSS ${rss_early} kB at 5 min, ${rss_last} kB at the end (${growth}%), max ${rss_max} kB"
        echo "cpu: mean ${cpu_mean}% of one core after the first minute (this is the encrypted call's CPU for E9)"

        echo
        echo "gate:"
        awk -v l="$length" -v m="$MIN_CALL_S" 'BEGIN { printf "  %s call lasted %d s (want >= %d)\n", (l >= m ? "PASS" : "FAIL"), l, m }'
        awk -v g="$growth" 'BEGIN { if (g == "NA") print "  ?    no memory samples"; else printf "  %s memory grew %s%% after 5 min (want < 10%%)\n", (g + 0 < 10 ? "PASS" : "FAIL"), g }'
        local a50 b50
        a50=$(percentile "$tmp/A_audio_e2e_ms" 0.5)
        b50=$(percentile "$tmp/B_audio_e2e_ms" 0.5)
        awk -v a="$a50" -v b="$b50" 'BEGIN { if (a == "NA" || b == "NA") print "  ?    no audio e2e samples"; else { d = b - a; if (d < 0) d = -d; printf "  %s audio e2e p50 moved %.1f ms (want < 5)\n", (d < 5 ? "PASS" : "FAIL"), d } }'
        awk -v a="$fpsA" -v b="$fpsB" 'BEGIN { d = a - b; printf "  %s fps shown dropped %.1f (want < 1)\n", (d < 1 ? "PASS" : "FAIL"), d }'
    } | tee "$dir/summary.txt"
    rm -rf "$tmp"
}

run() {
    [[ $# -ge 1 ]] || usage
    local name=$1
    shift
    [[ -x "$BUILD/call/lancall" ]] || { echo "build first: $BUILD/call/lancall missing"; exit 1; }
    local out
    out="experiments/e7_soak_${name}_$(date +%Y%m%d_%H%M%S)"
    mkdir -p "$out"

    sample "call/lancall --name $name --log $out/lancall.csv" "$out/resources.csv" &
    local sampler=$!
    # a handler (not an ignore) so Ctrl+C reaches lancall, which ends cleanly, and we carry on
    trap 'true' INT
    echo "[soak] logging to $out; call, wait ~30 min, then hangup and quit (or Ctrl+C)"
    "$BUILD/call/lancall" --name "$name" --log "$out/lancall.csv" "$@" || true
    trap - INT
    wait "$sampler" 2>/dev/null || true
    echo
    summarize "$out"
}

case "${1:-}" in
    run) shift; run "$@" ;;
    summarize) shift; [[ $# -eq 1 ]] || usage; summarize "$1" ;;
    *) usage ;;
esac
