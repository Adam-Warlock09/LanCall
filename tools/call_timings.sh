#!/usr/bin/env bash
# call_timings.sh — timings from a lancall log that log_summary doesn't give (phase 10):
#
#   setup     per call: ring time (call_incoming -> call_started, includes the human, callee
#             side only) and call_started -> first audio packet / first video packet / first
#             frame shown. The "call setup < 2 s" target is call_started -> first frame.
#   joins     in a group call, per member who joined (member_joined, value = member id =
#             stream id): member_joined -> first audio / video packet from that member
#   events    a timeline of the call's events: calls, members joining and leaving, mesh <->
#             relay <-> multicast switches, broadcasts and watching, bridges
#   gaps      media gaps: every stretch longer than GAP_MS with no video (or no audio) packet
#             from one stream, while in a call or watching. After a 1 s outage (netem 100% loss
#             for 1 s), "gap - 1 s" is the recovery time. After a mesh/relay/multicast switch it
#             is the interruption the switch caused
#   freezes   stretches longer than GAP_MS with no frame shown (all streams pooled, so in a
#             group call a freeze of one tile only shows under gaps)
#
# Usage (from the repo root, after building):
#   tools/call_timings.sh logs/C1_L3.csv [more.csv ...]
#   GAP_MS=500 tools/call_timings.sh logs/C5_L2.csv
#
# Times are seconds since the log's first row. Log times are microseconds mod 2^32 (they wrap
# every 71.6 min); the script unwraps them, so long logs (the soak) are fine.
set -euo pipefail

GAP_MS=${GAP_MS:-250}

usage() {
    awk 'NR > 1 && /^#/ { sub(/^# ?/, ""); print; next } NR > 1 { exit }' "$0"
    exit 2
}
[[ $# -ge 1 ]] || usage

timings() {
    # rows: local_us,S|R,type,stream,seq,frame,frag,bytes,timestamp_us  or  local_us,E,name,value
    # packet types: 1 video, 2 audio (FEC parity, reports, keepalives and NACKs are ignored)
    awk -F, -v gap_ms="$GAP_MS" '
    function wall(us) {  # unwrap the 32-bit microsecond clock into seconds since the first row
        if (!have) { have = 1; prev = us; t = 0; return 0 }
        d = us - prev
        if (d < -2147483648) d += 4294967296
        else if (d > 2147483648) d -= 4294967296
        prev = us; t += d
        return t / 1e6
    }
    function reset_media() { delete last_rx; last_shown = -1 }
    function start_active(s) {
        active = 1; reset_media()
        if (calls_open) close_call()
        n++; started[n] = s; ring[n] = (incoming >= 0) ? s - incoming : -1; incoming = -1
        first_a[n] = first_v[n] = first_f[n] = -1; calls_open = 1
    }
    function close_call() { calls_open = 0 }
    function fmt(x) { return x < 0 ? "-" : sprintf("%.3f s", x) }

    BEGIN { gap = gap_ms / 1000.0; incoming = -1; n = 0; nj = 0; ng = 0; nf = 0; ne = 0; last_shown = -1 }
    $1 !~ /^[0-9]+$/ { next }
    {
        s = wall($1 + 0)
        if (s > span) span = s
    }
    $2 == "E" {
        name = $3
        if (name == "call_incoming") { incoming = s }
        if (name == "call_started" || name == "watch_started") start_active(s)
        if (name == "call_ended" || name == "watch_ended") { active = 0; reset_media(); close_call() }
        if (name == "member_joined") {
            nj++; jm[nj] = $4 + 0; jt[nj] = s; ja[nj] = jv[nj] = -1; open_join[$4 + 0] = nj
        }
        if (name ~ /^(call_|member_|media_|broadcast_|watch_|bridge_)/ || name == "members") {
            ne++; ev[ne] = sprintf("  %9.3f  %-18s %s", s, name, $4)
        }
        if (name == "frame_shown" && active) {
            if (calls_open && first_f[n] < 0) first_f[n] = s - started[n]
            if (last_shown >= 0 && s - last_shown > gap) {
                nf++; fr[nf] = sprintf("  %9.3f  no frame shown for %.3f s", last_shown, s - last_shown)
            }
            last_shown = s
        }
        next
    }
    $2 == "R" && ($3 == 1 || $3 == 2) && active {
        kind = ($3 == 1) ? "video" : "audio"
        key = kind "/" $4
        if (calls_open) {
            if ($3 == 1 && first_v[n] < 0) first_v[n] = s - started[n]
            if ($3 == 2 && first_a[n] < 0) first_a[n] = s - started[n]
        }
        if (($4 + 0) in open_join) {
            j = open_join[$4 + 0]
            if ($3 == 1 && jv[j] < 0) jv[j] = s - jt[j]
            if ($3 == 2 && ja[j] < 0) ja[j] = s - jt[j]
            if (ja[j] >= 0 && jv[j] >= 0) delete open_join[$4 + 0]
        }
        if ((key in last_rx) && s - last_rx[key] > gap) {
            ng++; gp[ng] = sprintf("  %9.3f  %-9s gap %.3f s", last_rx[key], key, s - last_rx[key])
        }
        last_rx[key] = s
    }
    END {
        printf "span %.1f s\n", span
        print "setup (per call or watch; ring = call_incoming -> call_started, callee side only):"
        if (n == 0) print "  no call_started / watch_started in this log"
        for (i = 1; i <= n; i++)
            printf "  #%d at %8.3f s  ring %s  first audio %s  first video %s  first frame %s\n",
                   i, started[i], fmt(ring[i]), fmt(first_a[i]), fmt(first_v[i]), fmt(first_f[i])
        print "joins (member_joined -> first packet from that member):"
        if (nj == 0) print "  none"
        for (i = 1; i <= nj; i++)
            printf "  member %d at %8.3f s  first audio %s  first video %s\n", jm[i], jt[i], fmt(ja[i]), fmt(jv[i])
        print "events (t_s, name, value):"
        if (ne == 0) print "  none"
        for (i = 1; i <= ne; i++) print ev[i]
        printf "media gaps > %d ms (received, per stream; t_s = last packet before the gap):\n", gap_ms
        if (ng == 0) print "  none"
        for (i = 1; i <= ng; i++) print gp[i]
        printf "display freezes > %d ms (no frame shown, all streams pooled):\n", gap_ms
        if (nf == 0) print "  none"
        for (i = 1; i <= nf; i++) print fr[i]
    }' "$1"
}

status=0
for f in "$@"; do
    if [[ ! -r "$f" ]]; then
        echo "cannot read $f" >&2
        status=1
        continue
    fi
    echo "== $f"
    timings "$f"
    echo
done
exit "$status"
