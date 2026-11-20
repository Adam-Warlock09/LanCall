// What lancall_gui offers where (Person 2, call/, phase 9). See ui_logic.hpp.
#include "call/ui_logic.hpp"

namespace call {

bool is_broadcasting(const control::Peer& p) {
    return !p.remote && p.group != 0 && !p.broadcast_title.empty();
}

PeerActions peer_actions(const control::Peer& p, control::CallState state, bool is_host,
                         bool broadcasting, bool watching) {
    PeerActions a;
    const bool idle = state == control::CallState::Idle;
    const bool free = idle && !broadcasting;  // the camera and the media ports are ours
    a.call = free && !watching;
    a.add = state == control::CallState::InCall && is_host;
    a.watch = free && is_broadcasting(p);  // watching someone else: watch switches over
    return a;
}

std::string watch_target(const control::Peer& p, const std::vector<control::Peer>& peers) {
    int same_name = 0;
    for (const control::Peer& q : peers) same_name += q.name == p.name;
    return same_name > 1 ? p.ip : p.name;
}

std::string mode_label(control::MediaMode mode, const std::string& address) {
    switch (mode) {
        case control::MediaMode::Relay:
            return "relay " + address;
        case control::MediaMode::Multicast:
            return "multicast " + address;
        default:
            return "mesh";
    }
}

}  // namespace call
