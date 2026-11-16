#!/usr/bin/env bash
# netem.sh — inject packet loss, delay and jitter, or cap the bandwidth, on this laptop's
# OUTGOING traffic using Linux tc. Used for the Phase 1 gate (netem.sh on 5) and
# experiments E2–E4.
#
# Usage:
#   netem.sh on [loss%] [delay_ms] [jitter_ms]   e.g.  netem.sh on 5
#                                                       netem.sh on 0 30 10
#   netem.sh cap <mbit>                           e.g.  netem.sh cap 2   (bandwidth cap)
#   netem.sh off                                  removes either of the above
#   netem.sh status
#
# Environment:
#   IFACE=<name>   use this interface instead of auto-detecting it
#   DRY_RUN=1      print the tc commands instead of running them
#
# Notes:
#   - netem only shapes traffic LEAVING this laptop. For a symmetric test, run it on both sides.
#   - It affects ALL outgoing traffic on the interface (ssh too), so turn it off after each run.
#   - With jitter, netem can reorder packets; the receivers' stats count reordering, not loss.
#   - cap uses tbf with a short queue (50 ms), so going over the cap shows up as loss within a
#     fraction of a second, like a real congested link. netem's own "rate" queues 1000 packets,
#     which shows up as seconds of delay first and makes the video ladder react late.
#   - on and cap replace each other: only one shaper at a time.
#   - IFACE=lo shapes loopback, for single-laptop tests (all local traffic is affected).
set -euo pipefail

usage() {
    # the comment block at the top of this file, without the leading "# "
    awk 'NR > 1 && /^#/ { sub(/^# ?/, ""); print; next } NR > 1 { exit }' "$0"
    exit 2
}

die() {
    echo "netem.sh: $*" >&2
    exit 1
}

# Interface carrying the default route; if there is none (offline LAN without a default
# gateway), the first UP physical interface with a global IPv4 address. Virtual interfaces
# (docker0, virbr0, VXLAN, ...) have no /sys/class/net/<if>/device and are skipped.
detect_iface() {
    if [[ -n "${IFACE:-}" ]]; then
        echo "$IFACE"
        return
    fi
    local dev
    dev=$(ip -4 route show default 2>/dev/null |
        awk '{for (i = 1; i < NF; i++) if ($i == "dev") { print $(i + 1); exit }}')
    if [[ -z "$dev" ]]; then
        local cand
        for cand in $(ip -4 -o addr show scope global up 2>/dev/null | awk '{print $2}'); do
            if [[ -e "/sys/class/net/$cand/device" ]]; then
                dev=$cand
                break
            fi
        done
    fi
    [[ -n "$dev" ]] || die "no network interface found; set IFACE=<name>"
    echo "$dev"
}

is_number() {
    [[ "$1" =~ ^[0-9]+([.][0-9]+)?$ ]]
}

is_zero() {
    [[ "$1" =~ ^0+([.]0+)?$ ]]
}

# Run a tc command as root (via sudo when needed), or just print it under DRY_RUN=1.
run_tc() {
    if [[ "${DRY_RUN:-0}" == "1" ]]; then
        echo "+ tc $*"
        return
    fi
    if [[ $EUID -eq 0 ]]; then
        tc "$@"
    else
        sudo tc "$@"
    fi
}

# true if one of our shapers (netem or tbf) is the root qdisc
has_shaper() {
    tc qdisc show dev "$1" root 2>/dev/null | grep -Eq 'netem|tbf'
}

cmd_on() {
    local iface=$1 loss=${2:-0} delay=${3:-0} jitter=${4:-0}
    is_number "$loss" || die "loss must be a number (percent), got '$loss'"
    is_number "$delay" || die "delay must be a number (ms), got '$delay'"
    is_number "$jitter" || die "jitter must be a number (ms), got '$jitter'"
    awk -v l="$loss" 'BEGIN { exit !(l <= 100) }' || die "loss must be at most 100%"

    local args=()
    if ! is_zero "$delay" || ! is_zero "$jitter"; then
        args+=(delay "${delay}ms")
        if ! is_zero "$jitter"; then
            args+=("${jitter}ms")
            if is_zero "$delay"; then
                echo "netem.sh: warning: jitter with 0 ms delay; negative delays are clipped to 0," \
                    "so the effective jitter is smaller than requested" >&2
            fi
        fi
    fi
    if ! is_zero "$loss"; then
        args+=(loss "${loss}%")
    fi

    run_tc qdisc replace dev "$iface" root netem "${args[@]}"
    echo "netem ON  on $iface: loss ${loss}%, delay ${delay} ms, jitter ${jitter} ms (outgoing only)"
}

cmd_cap() {
    local iface=$1 mbit=$2
    is_number "$mbit" || die "cap must be a number (Mbit/s), got '$mbit'"
    ! is_zero "$mbit" || die "cap must be more than 0 (use off to remove it)"
    run_tc qdisc replace dev "$iface" root tbf rate "${mbit}mbit" burst 32kbit latency 50ms
    echo "cap ON    on $iface: ${mbit} Mbit/s, 50 ms queue (outgoing only)"
}

cmd_off() {
    local iface=$1
    if [[ "${DRY_RUN:-0}" == "1" ]] || has_shaper "$iface"; then
        run_tc qdisc del dev "$iface" root
        echo "shaping OFF on $iface"
    else
        echo "shaping already off on $iface"
    fi
}

cmd_status() {
    local iface=$1
    if has_shaper "$iface"; then
        echo "shaping ON on $iface:"
    else
        echo "shaping off on $iface. Current root qdisc:"
    fi
    tc qdisc show dev "$iface" root
}

main() {
    [[ $# -ge 1 ]] || usage
    [[ "${DRY_RUN:-0}" == "1" ]] || command -v tc >/dev/null || die "tc not found (install iproute2)"
    local action=$1
    shift
    local iface
    case "$action" in
        on)
            [[ $# -le 3 ]] || usage
            iface=$(detect_iface)
            cmd_on "$iface" "$@"
            ;;
        cap)
            [[ $# -eq 1 ]] || usage
            iface=$(detect_iface)
            cmd_cap "$iface" "$1"
            ;;
        off)
            [[ $# -eq 0 ]] || usage
            iface=$(detect_iface)
            cmd_off "$iface"
            ;;
        status)
            [[ $# -eq 0 ]] || usage
            iface=$(detect_iface)
            cmd_status "$iface"
            ;;
        -h | --help | help) usage ;;
        *) usage ;;
    esac
}

main "$@"