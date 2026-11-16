#!/usr/bin/env bash
# netem.sh — inject packet loss, delay and jitter on this laptop's OUTGOING traffic
# using Linux tc netem. Used for the Phase 1 gate (netem.sh on 5) and experiments E2–E4.
#
# Usage:
#   netem.sh on [loss%] [delay_ms] [jitter_ms]   e.g.  netem.sh on 5
#                                                       netem.sh on 0 30 10
#   netem.sh off
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
set -euo pipefail

usage() {
    sed -n '2,18p' "$0" | sed 's/^# \{0,1\}//'
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

has_netem() {
    tc qdisc show dev "$1" root 2>/dev/null | grep -q netem
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

cmd_off() {
    local iface=$1
    if [[ "${DRY_RUN:-0}" == "1" ]] || has_netem "$iface"; then
        run_tc qdisc del dev "$iface" root
        echo "netem OFF on $iface"
    else
        echo "netem already off on $iface"
    fi
}

cmd_status() {
    local iface=$1
    if has_netem "$iface"; then
        echo "netem ON on $iface:"
    else
        echo "netem off on $iface (no netem qdisc). Current root qdisc:"
    fi
    tc qdisc show dev "$iface" root
}

main() {
    [[ $# -ge 1 ]] || usage
    command -v tc >/dev/null || die "tc not found (install iproute2)"
    local action=$1
    shift
    local iface
    case "$action" in
        on)
            [[ $# -le 3 ]] || usage
            iface=$(detect_iface)
            cmd_on "$iface" "$@"
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
