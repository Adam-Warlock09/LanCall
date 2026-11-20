#pragma once
// What lancall_gui offers where (Person 2, call/, phase 9): pure decisions behind its
// buttons, kept apart from the drawing (gui/) so they're unit-tested even on a laptop
// without GLFW (tests/metrics/test_front_end.cpp). App still checks everything again when
// a button is pressed and says why not, so these only decide what's worth showing.

#include <string>
#include <vector>

#include "control/discovery.hpp"
#include "control/signaling.hpp"

namespace call {

// A peer on our floor announcing a broadcast in its beacon (other floors can't be watched).
bool is_broadcasting(const control::Peer& p);

// The buttons a peer's row gets.
struct PeerActions {
    bool call = false;   // we're idle and free: ring them
    bool add = false;    // we host a call: ring them into it
    bool watch = false;  // they broadcast on our floor and we're free to watch
};
PeerActions peer_actions(const control::Peer& p, control::CallState state, bool is_host,
                         bool broadcasting, bool watching);

// What `watch` needs to find p's broadcast: its name, or its IP when another peer has the
// same name (the IP then picks whichever of them broadcasts).
std::string watch_target(const control::Peer& p, const std::vector<control::Peer>& peers);

// "mesh", "relay 10.8.66.20", "multicast 239.255.77.4": how a call's media flows.
std::string mode_label(control::MediaMode mode, const std::string& address);

}  // namespace call
