#!/usr/bin/env bash
# exp_e10_multicast.sh — experiment E10 (phase 7, Person 2): one presenter, 1-3 viewers on one
# floor, three ways of getting the presenter's audio and video to them:
#
#   unicast    the presenter sends a copy to every viewer (video_sender a,b,c / audio_sender --also)
#   relay      the presenter sends one copy to lancall_relay, which runs on one viewer's laptop
#              (role relay) and copies it on to the others: the fan-out moves to that laptop
#   multicast  lancall's own broadcast / watch: one copy to a group, the floor's switches copy it
#
# Same media every time: the fixed video level, FEC on, NACK off (what a broadcast uses), audio.
# What it measures, over the same WINDOW on every laptop:
#   - the wired card's counters (/proc/net/dev): kbit/s out and in, packets/s in. The real cost
#     on the wire, headers included, and the only thing that shows the multicast flood
#   - the programs' own logs: kbit/s sent (S rows) and received (R rows)
# A laptop on the floor that isn't watching (role idle) runs nothing and only reads its card:
# multicast reaches it anyway (no IGMP snooping), unicast and relay don't.
#
# One run = one mode and number of viewers, ~90 s. Every laptop runs the same command with its
# own role, all started within a few seconds of each other (receivers first is nicest):
#
#   presenter:  VIEWERS="10.8.66.20 10.8.66.31" tools/exp_e10_multicast.sh run unicast 2 presenter Atul
#   viewer:     PRESENTER=10.8.66.10 tools/exp_e10_multicast.sh run unicast 2 viewer Aric
#   idle:       PRESENTER=10.8.66.10 tools/exp_e10_multicast.sh run unicast 2 idle akshat
#
#   relay mode: everyone gets RELAY=<ip of the relay laptop>; that laptop is role relay (it also
#               watches, with VIEWER_ID=2); the other viewers VIEWER_ID=3, 4 (the presenter is 1)
#   multicast:  the presenter runs lancall and broadcasts; viewers get PRESENTER_NAME=<its
#               lancall name> and watch it; PRESENTER=<its ip> only helps find the network card
#
# The 9 runs: unicast/relay/multicast x 1/2/3 viewers. With 4 laptops, runs with 1 or 2 viewers
# leave a laptop for role idle. Each laptop appends one row per run to
# experiments/e10_rows_<name>.csv; afterwards copy everyone's rows to one laptop and:
#   tools/exp_e10_multicast.sh collect e10_rows_*.csv > results/e10_multicast.csv
#
# Columns: mode,viewers,role,laptop,window_s,nic_tx_kbps,nic_rx_kbps,nic_rx_pps,
#          app_up_kbps,app_down_kbps
# (the relay's forwarding isn't in any log: on the relay laptop it shows in nic_tx_kbps only)
#
# Environment:
#   PRESENTER       the presenter's IP (viewers in unicast; finding the card for everyone)
#   VIEWERS         the viewers' IPs, space separated (presenter, unicast)
#   RELAY           the relay laptop's IP (everyone, relay mode)
#   VIEWER_ID       2.. (viewers in relay mode: their member id for the relay)
#   PRESENTER_NAME  the presenter's lancall name (viewers, multicast)
#   DURATION=90 WARMUP=20 WINDOW=60   the counters are read at WARMUP and WARMUP+WINDOW s
#   IFACE           the wired card (default: whichever the route to PRESENTER/RELAY uses)
#   EXTRA=""        more lancall options (multicast mode), BUILD=build
#
# Needs: Linux (/proc/net/dev, ip route), everyone on one floor, a camera and mic on the
# presenter, headphones on the viewers, a display for the viewers' video windows.
set -euo pipefail

BUILD=${BUILD:-build}
DURATION=${DURATION:-90}
WARMUP=${WARMUP:-20}
WINDOW=${WINDOW:-60}
EXTRA=${EXTRA:-}
NETDEV=${NETDEV:-/proc/net/dev}  # only changed by the self-test

HEADER="mode,viewers,role,laptop,window_s,nic_tx_kbps,nic_rx_kbps,nic_rx_pps,app_up_kbps,app_down_kbps"

usage() {
    sed -n '2,48p' "$0" | sed 's/^# \{0,1\}//'
    exit 2
}

die() {
    echo "$*" >&2
    exit 2
}

# rx_bytes rx_packets tx_bytes tx_packets of one card
read_counters() {
    sed 's/:/ /' "$NETDEV" | awk -v i="$1" '$1 == i { print $2, $3, $10, $11; found = 1 }
        END { if (!found) exit 1 }'
}

# kbit/s sent and received by the programs, from their logs: S and R bytes over WINDOW s
# ending TAIL s before each log's last row (the programs stop at DURATION, so that's the same
# stretch the card's counters cover). Several logs (audio + video) are added up.
app_rates() {
    local tail_s=$1
    shift
    local up=0 down=0 u d
    for f in "$@"; do
        [[ -s "$f" ]] || continue
        read -r u d < <(awk -F, -v W="$WINDOW" -v T="$tail_s" '
            $1 !~ /^[0-9]+$/ { next }
            !have { t0 = $1; have = 1 }
            { d = $1 - t0; if (d < 0) d += 4294967296
              n++; t[n] = d; dir[n] = $2; b[n] = $8; if (d > last) last = d }
            END {
                to = last - T * 1e6; from = to - W * 1e6
                for (i = 1; i <= n; i++) {
                    if (t[i] < from || t[i] > to) continue
                    if (dir[i] == "S") s += b[i]; else if (dir[i] == "R") r += b[i]
                }
                printf "%.1f %.1f\n", s * 8 / W / 1000, r * 8 / W / 1000
            }' "$f")
        up=$(awk -v a="$up" -v b="$u" 'BEGIN { printf "%.1f", a + b }')
        down=$(awk -v a="$down" -v b="$d" 'BEGIN { printf "%.1f", a + b }')
    done
    echo "$up $down"
}

collect() {
    [[ $# -ge 1 ]] || usage
    echo "$HEADER"
    for f in "$@"; do tail -n +2 "$f"; done
}

run() {
    [[ $# -eq 4 ]] || usage
    local mode=$1 viewers=$2 role=$3 name=$4
    [[ "$mode" =~ ^(unicast|relay|multicast)$ ]] || die "mode: unicast, relay or multicast"
    [[ "$viewers" =~ ^[1-3]$ ]] || die "viewers: 1, 2 or 3"
    [[ "$role" =~ ^(presenter|viewer|relay|idle)$ ]] || die "role: presenter, viewer, relay or idle"
    [[ "$name" =~ ^[A-Za-z0-9_-]+$ ]] || die "name: letters, digits, - and _"
    [[ "$role" == relay && "$mode" != relay ]] && die "role relay is only for relay mode"

    # what each role needs
    case "$mode/$role" in
        unicast/presenter) [[ -n "${VIEWERS:-}" ]] || die "set VIEWERS to the viewers' IPs" ;;
        unicast/viewer) [[ -n "${PRESENTER:-}" ]] || die "set PRESENTER to the presenter's IP" ;;
        relay/presenter) [[ -n "${RELAY:-}" ]] || die "set RELAY to the relay laptop's IP" ;;
        relay/viewer | relay/relay)
            [[ -n "${RELAY:-}" ]] || die "set RELAY to the relay laptop's IP"
            [[ "${VIEWER_ID:-}" =~ ^[2-6]$ ]] || die "set VIEWER_ID (2..6; the presenter is 1)"
            ;;
        multicast/viewer) [[ -n "${PRESENTER_NAME:-}" ]] || die "set PRESENTER_NAME" ;;
    esac
    if [[ "$role" != idle ]]; then
        for p in video/video_sender video/video_receiver audio/audio_sender audio/audio_receiver \
            call/lancall net/lancall_relay; do
            [[ -x "$BUILD/$p" ]] || die "build first: $BUILD/$p missing"
        done
    fi

    # the wired card: the one the route towards the others uses
    # (none of PRESENTER / RELAY / VIEWERS may be set, e.g. the multicast presenter; and the
    # route to our own IP, the relay laptop's RELAY, is "local ... dev lo": then the card of
    # the default route)
    local target="${PRESENTER:-${RELAY:-${VIEWERS:-}}}"
    target="${target%% *}"
    local iface="${IFACE:-}"
    if [[ -z "$iface" ]]; then
        iface=$(ip route get "${target:-1.1.1.1}" 2>/dev/null |
            awk '{ for (i = 1; i < NF; i++) if ($i == "dev") { print $(i + 1); exit } }')
        if [[ -z "$iface" || "$iface" == lo ]]; then
            iface=$(ip route show default 2>/dev/null |
                awk '{ for (i = 1; i < NF; i++) if ($i == "dev") { print $(i + 1); exit } }')
        fi
    fi
    read_counters "$iface" >/dev/null || die "no network card '$iface' (set IFACE)"
    ((DURATION >= WARMUP + WINDOW + 5)) || die "DURATION must be at least WARMUP + WINDOW + 5"

    local out="experiments/e10"
    mkdir -p "$out"
    local tag="e10_${mode}_v${viewers}_${role}_${name}"
    rm -f "$out/$tag"_*.csv "$out/$tag"_*.txt
    local logs=()
    local pids=()
    echo "== $mode, $viewers viewer(s), $role on $iface, ${DURATION} s"

    # stop everything we started, whatever happens
    trap 'kill "${pids[@]}" 2>/dev/null || true' EXIT

    case "$mode/$role" in
        unicast/presenter | relay/presenter)
            local vdest adest also=()
            if [[ "$mode" == unicast ]]; then
                read -r -a vs <<<"$VIEWERS"
                vdest=$(
                    IFS=,
                    echo "${vs[*]}"
                )
                adest=${vs[0]}
                for v in "${vs[@]:1}"; do also+=(--also "$v"); done
            else
                vdest="$RELAY:5014"
                adest="$RELAY:5014"
            fi
            timeout -s INT "$DURATION" "$BUILD/video/video_sender" "$vdest" --fixed --fec --no-nack \
                --id 1 --log "$out/${tag}_video.csv" >"$out/${tag}_video.txt" 2>&1 &
            pids+=($!)
            timeout -s INT "$DURATION" "$BUILD/audio/audio_sender" "$adest" "${also[@]}" --stream 1 \
                --log "$out/${tag}_audio.csv" >"$out/${tag}_audio.txt" 2>&1 &
            pids+=($!)
            logs+=("$out/${tag}_video.csv" "$out/${tag}_audio.csv")
            ;;
        unicast/viewer | relay/viewer | relay/relay)
            local from=${PRESENTER:-}
            local vextra=() aextra=()
            if [[ "$mode" == relay ]]; then
                from=$RELAY
                vextra=(--id "$VIEWER_ID")
                aextra=(--id "$VIEWER_ID" --keepalive "$RELAY")
            fi
            if [[ "$role" == relay ]]; then
                timeout -s INT "$DURATION" "$BUILD/net/lancall_relay" >"$out/${tag}_relay.txt" 2>&1 &
                pids+=($!)
                sleep 1
            fi
            timeout -s INT "$DURATION" "$BUILD/video/video_receiver" --from "$from" "${vextra[@]}" \
                --log "$out/${tag}_video.csv" >"$out/${tag}_video.txt" 2>&1 &
            pids+=($!)
            timeout -s INT "$DURATION" "$BUILD/audio/audio_receiver" --from "$from" "${aextra[@]}" \
                --log "$out/${tag}_audio.csv" >"$out/${tag}_audio.txt" 2>&1 &
            pids+=($!)
            logs+=("$out/${tag}_video.csv" "$out/${tag}_audio.csv")
            ;;
        multicast/presenter | multicast/viewer)
            local console="$out/${tag}_console.txt"
            feed() {
                if [[ "$role" == presenter ]]; then
                    sleep 3  # our own beacon has come back by then: lancall knows its address
                    echo "broadcast e10 $viewers viewers"
                    sleep "$DURATION"
                else
                    # watch as soon as the presenter's beacon announces the broadcast
                    local t=0
                    while ((t < DURATION)) && ! grep -q "^watching " "$console" 2>/dev/null; do
                        echo "watch $PRESENTER_NAME"
                        sleep 2
                        t=$((t + 2))
                    done
                    sleep "$DURATION"
                fi
            }
            # shellcheck disable=SC2086  # EXTRA is a list of options
            feed | timeout -s INT "$DURATION" "$BUILD/call/lancall" --name "$name" \
                --log "$out/${tag}.csv" $EXTRA >"$console" 2>&1 &
            pids+=($!)
            logs+=("$out/${tag}.csv")
            ;;
        */idle) ;;  # nothing to run: just the card
        *) die "role $role doesn't exist in $mode mode" ;;
    esac

    # the card's counters over the window
    sleep "$WARMUP"
    local c0 c1 t0 t1
    c0=$(read_counters "$iface")
    t0=$(date +%s.%N)
    sleep "$WINDOW"
    c1=$(read_counters "$iface")
    t1=$(date +%s.%N)
    echo "   window done, waiting for the programs to stop"
    for p in "${pids[@]}"; do wait "$p" 2>/dev/null || true; done
    trap - EXIT

    local nic
    nic=$(awk -v a="$c0" -v b="$c1" -v t0="$t0" -v t1="$t1" 'BEGIN {
            split(a, x, " "); split(b, y, " "); s = t1 - t0
            printf "%.1f,%.1f,%.1f,%.1f", s, (y[3] - x[3]) * 8 / s / 1000,
                   (y[1] - x[1]) * 8 / s / 1000, (y[2] - x[2]) / s }')
    local app="0.0 0.0"
    ((${#logs[@]} > 0)) && app=$(app_rates $((DURATION - WARMUP - WINDOW)) "${logs[@]}")
    local row="$mode,$viewers,$role,$name,$nic,${app// /,}"

    local rows="experiments/e10_rows_${name}.csv"
    [[ -s "$rows" ]] || echo "$HEADER" >"$rows"
    echo "$row" >>"$rows"
    echo
    echo "$HEADER"
    echo "$row"
    echo "files: $out/${tag}_*   rows so far: $rows"
}

case "${1:-}" in
    run) shift; run "$@" ;;
    collect) shift; collect "$@" ;;
    app-rates) shift; app_rates "$@" ;;      # for checking the script itself
    counters) shift; read_counters "$@" ;;   # likewise
    *) usage ;;
esac
