#!/usr/bin/env bash
# exp_e5_group.sh — experiment E5: group calls, mesh against relay (Person 2, tools/).
# What one person uploads, and the latency everyone gets, for 2, 3 and 4 people.
#
# Mesh: every member sends every stream to every other member, so upload grows with N-1
# (audio alone is 652 B x 50/s = ~261 kbps per copy). Relay: everyone sends one copy to the
# relay, which forwards it, so upload stays flat at the price of one more hop.
#
# One run = one call of a given size and mode, 1-2 minutes, on real laptops. Every laptop in
# the call runs this script with the same <mode> and <people>; the host also names whom to
# add. Start the members first (they wait up to MEMBER_WAIT s for the call), then the host:
#
#   member laptops:  tools/exp_e5_group.sh run mesh 3 Aric
#                    tools/exp_e5_group.sh run mesh 3 Rishika
#   host laptop:     tools/exp_e5_group.sh run mesh 3 Atul Aric Rishika
#
# The host types into its own lancall for you: relay/mesh, then `add` for each name (a name
# from `list`, or an IP), waits DURATION s with everyone in, hangs up. Members answer every
# call automatically. Each laptop then summarizes its own log into one row, appended to
# experiments/e5_rows_<name>.csv. The six runs of the full experiment:
#
#   mesh 2, mesh 3, mesh 4, relay 2, relay 3, relay 4
#
# Collect everyone's rows into results/e5_group.csv (one header line):
#   head -1 experiments/e5_rows_Atul.csv > results/e5_group.csv
#   tail -q -n +2 e5_rows_*.csv >> results/e5_group.csv     (after copying the files over)
# or copy the logs to one laptop and run:  tools/exp_e5_group.sh summarize <logs...>
#
# Columns (all over the last WINDOW s of the call, when everyone was in):
#   mode,people,laptop,window_s           the run, and whose log this is
#   up_audio_kbps,up_video_kbps           what this laptop sent (every copy, headers included)
#   up_total_kbps,down_kbps               all sent / all received, reports included
#   streams_in                            distinct streams received (type/stream_id)
#   audio_e2e_p50_ms,audio_e2e_p95_ms     capture -> heard, all people heard pooled
#   video_e2e_p50_ms,video_e2e_p95_ms     capture -> shown
#   av_sync_p50_ms,rtt_p50_ms             lip-sync offset, clock-sync round trip
#   relay_on                              1 if the log shows media switched to a relay: a
#                                         "relay" row with 0 here ran as mesh (check console)
# Percentiles are nearest rank, like log_summary.
#
# Environment:
#   DURATION=90       seconds the full room is held (the 1-2 minute rule, NOTES.md)
#   WINDOW=60         seconds summarized, counted back from the last packet sent
#   JOIN_WAIT=6       seconds between two adds (the member answers within ~1 s)
#   RELAY=me          relay runs: me = inside the host's lancall, or the IP of a laptop
#                     running lancall_relay (or lancall: the relay starts where its IP is)
#   MEMBER_WAIT=120   how long a member waits for the call to start
#   EXTRA=""          more lancall options for this laptop, e.g. "--directory 10.8.64.5"
#   BUILD=build
#
# Needs: a camera and microphone (headphones!) on every laptop, and phase 4 group calls in
# signaling, video and the relay. With only two laptops, the 2-person runs work already.
set -euo pipefail

BUILD=${BUILD:-build}
DURATION=${DURATION:-90}
WINDOW=${WINDOW:-60}
JOIN_WAIT=${JOIN_WAIT:-6}
RELAY=${RELAY:-me}
MEMBER_WAIT=${MEMBER_WAIT:-120}
EXTRA=${EXTRA:-}

HEADER="mode,people,laptop,window_s,up_audio_kbps,up_video_kbps,up_total_kbps,down_kbps,streams_in,audio_e2e_p50_ms,audio_e2e_p95_ms,video_e2e_p50_ms,video_e2e_p95_ms,av_sync_p50_ms,rtt_p50_ms,relay_on"

usage() {
    sed -n '2,24p' "$0" | sed 's/^# \{0,1\}//'
    exit 2
}

# Nearest-rank percentile (the ceil(p*n)-th smallest) of the numbers in a file, or NA.
percentile() {
    local file=$1 p=$2
    [[ -s "$file" ]] || { echo NA; return; }
    sort -g "$file" | awk -v p="$p" '{ v[NR] = $1 }
        END { r = int(p * NR); if (r < p * NR) r++; if (r < 1) r = 1; printf "%.1f", v[r] }'
}

# One CSV row for one lancall log named e5_<mode>_n<people>_<laptop>.csv.
summarize_one() {
    local f=$1 base mode people laptop
    base=$(basename "$f" .csv)
    if [[ "$base" =~ ^e5_(mesh|relay)_n([0-9]+)_(.+)$ ]]; then
        mode=${BASH_REMATCH[1]} people=${BASH_REMATCH[2]} laptop=${BASH_REMATCH[3]}
    else
        mode=NA people=NA laptop=$base
    fi

    # pass 1: the first row's time, the last packet we sent (relative, mod 2^32), and
    # whether media ever went through a relay
    local t0 last relay_on
    read -r t0 last relay_on < <(awk -F, '$1 !~ /^[0-9]+$/ { next }
        !have { t0 = $1; have = 1 }
        { d = $1 - t0; if (d < 0) d += 4294967296; if ($2 == "S") last = d }
        $2 == "E" && $3 == "media_relay" && $4 == 1 { relay = 1 }
        END { printf "%.0f %.0f %d\n", t0, last + 0, relay + 0 }' "$f")
    if [[ -z "$t0" || "$last" == 0 ]]; then
        echo "$mode,$people,$laptop,0,NA,NA,NA,NA,0,NA,NA,NA,NA,NA,NA,0"
        return
    fi
    local from=$((last - WINDOW * 1000000))
    ((from < 0)) && from=0
    local secs
    secs=$(awk -v a="$from" -v b="$last" 'BEGIN { printf "%.1f", (b - a) / 1e6 }')

    # pass 2: rates and the timing events inside the window
    local tmp
    tmp=$(mktemp -d)
    local rates
    rates=$(awk -F, -v t0="$t0" -v from="$from" -v to="$last" -v secs="$secs" -v dir="$tmp" '
        $1 !~ /^[0-9]+$/ { next }
        { d = $1 - t0; if (d < 0) d += 4294967296; if (d < from || d > to) next }
        $2 == "S" { up += $8; if ($3 == 2) ua += $8; else if ($3 == 1) uv += $8 }
        $2 == "R" { down += $8; seen[$3 "/" $4] = 1 }
        $2 == "E" && ($3 == "audio_e2e_ms" || $3 == "video_e2e_ms" || $3 == "av_sync_ms" ||
                      $3 == "rtt_ms") { print $4 > (dir "/" $3) }
        END {
            n = 0; for (k in seen) n++
            k = secs > 0 ? 8 / secs / 1000 : 0
            printf "%.1f,%.1f,%.1f,%.1f,%d", ua * k, uv * k, up * k, down * k, n
        }' "$f")

    echo "$mode,$people,$laptop,$secs,$rates,$(percentile "$tmp/audio_e2e_ms" 0.5),$(percentile "$tmp/audio_e2e_ms" 0.95),$(percentile "$tmp/video_e2e_ms" 0.5),$(percentile "$tmp/video_e2e_ms" 0.95),$(percentile "$tmp/av_sync_ms" 0.5),$(percentile "$tmp/rtt_ms" 0.5),$relay_on"
    rm -rf "$tmp"
}

summarize() {
    [[ $# -ge 1 ]] || usage
    echo "$HEADER"
    for f in "$@"; do summarize_one "$f"; done
}

run() {
    [[ $# -ge 3 ]] || usage
    local mode=$1 people=$2 name=$3
    shift 3
    local peers=("$@")
    [[ "$mode" == mesh || "$mode" == relay ]] || { echo "mode: mesh or relay"; exit 2; }
    [[ "$people" =~ ^[2-6]$ ]] || { echo "people: 2 to 6"; exit 2; }
    if ((${#peers[@]} > 0 && ${#peers[@]} != people - 1)); then
        echo "the host adds people-1 = $((people - 1)) others, got ${#peers[@]}"
        exit 2
    fi
    [[ -x "$BUILD/call/lancall" ]] || { echo "build first: $BUILD/call/lancall missing"; exit 1; }

    local out="experiments/e5_group"
    mkdir -p "$out"
    local log="$out/e5_${mode}_n${people}_${name}.csv"
    local console="$out/e5_${mode}_n${people}_${name}_console.txt"
    rm -f "$log"  # one log per run and laptop; a re-run replaces it

    local feed total
    if ((${#peers[@]} > 0)); then
        # host: choose the mode while idle (it applies to the call we start), add everyone,
        # hold the room, hang up
        total=$((5 + JOIN_WAIT * ${#peers[@]} + DURATION + 5))
        echo "== host, $mode, $people people: adding ${peers[*]}, then ${DURATION} s in the call"
        feed() {
            sleep 5  # discovery beacons come every 2 s: let names show up in list
            if [[ "$mode" == relay ]]; then echo "relay $RELAY"; else echo "mesh"; fi
            for p in "${peers[@]}"; do
                echo "add $p"
                sleep "$JOIN_WAIT"
            done
            echo "members"
            sleep "$DURATION"
            echo "stats"
            echo "hangup"
            sleep 3
            echo "quit"
        }
    else
        # member: answer whatever call comes in, until the host hangs up or time runs out
        total=$((MEMBER_WAIT + JOIN_WAIT * (people - 1) + DURATION + 15))
        echo "== member, $mode, $people people: answering calls for up to ${total} s"
        feed() {
            local t=0
            while ((t < total)); do
                echo "accept"
                sleep 1
                t=$((t + 1))
                # the host hung up (or we were dropped): done, a few seconds for the summary
                if grep -q "^call ended" "$console" 2>/dev/null; then
                    sleep 2
                    break
                fi
            done
            echo "quit"
        }
    fi

    # shellcheck disable=SC2086  # EXTRA is a list of options
    feed | timeout -s INT $((total + 20)) "$BUILD/call/lancall" --name "$name" --log "$log" \
        $EXTRA >"$console" 2>&1 || true
    grep -v "^nobody is calling$" "$console" | tail -n 25 || true

    local rows="experiments/e5_rows_${name}.csv"
    [[ -s "$rows" ]] || echo "$HEADER" >"$rows"
    local row
    row=$(summarize_one "$log")
    echo "$row" >>"$rows"
    echo
    echo "$HEADER"
    echo "$row"
    echo "log: $log   rows so far: $rows"
}

case "${1:-}" in
    run) shift; run "$@" ;;
    summarize) shift; summarize "$@" ;;
    *) usage ;;
esac
