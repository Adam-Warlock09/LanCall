#pragma once
// Pure helpers of the lancall app for the control plane (Person 2, call/): who `call <arg>`
// means, the status line for each call state, and the `list` output. No I/O, so they are
// unit-tested on their own (tests/metrics/test_call_logic.cpp).

#include <cstdint>
#include <string>
#include <vector>

#include "common/protocol.hpp"
#include "control/discovery.hpp"
#include "control/signaling.hpp"

namespace call {

struct Target {
    std::string name;  // peer name if known (may be empty when called by IP)
    std::string ip;
    uint16_t port = proto::kSignalPort;
    std::string error;  // why the target could not be resolved, ready to print
    bool ok() const { return error.empty(); }
};

// `call <arg>`: arg is an IPv4 address (works across floors, where discovery does not reach)
// or a peer name from `list`. Names match exactly first, then ignoring case if that is
// unambiguous. Two peers with the same name must be called by IP.
Target resolve_target(const std::string& arg, const std::vector<control::Peer>& peers);

// What the window's status line says.
struct StatusInfo {
    std::string my_name;
    control::CallState state = control::CallState::Idle;
    std::string peer_name, peer_ip;
    std::string media_parts;  // e.g. "audio+video"; used while InCall
    double rtt_ms = 0;        // 0 = unknown
};
std::string status_line(const StatusInfo& s);

// The `list` output, one line per peer, given the current proto::now_us().
std::string format_peers(const std::vector<control::Peer>& peers, uint32_t now_us);

}  // namespace call
